/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <dave@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  Core routing algorithm based on CRoute:
 *
 *     CRoute: A Fast High-quality Timing-driven Connection-based FPGA Router
 *     Dries Vercruyce, Elias Vansteenkiste and Dirk Stroobandt
 *     DOI 10.1109/FCCM.2019.00017 [PDF on SciHub]
 *
 *  Modified for the nextpnr Arch API and data structures; optimised for
 *  real-world FPGA architectures in particular ECP5 and Xilinx UltraScale+
 *
 */

#include "router2.h"
#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <chrono>
#include <deque>
#include <fstream>
#include <queue>
#include <shared_mutex>
#include <thread>
#include "log.h"
#include "nextpnr.h"
#include "router1.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
struct Router2
{

    struct PerArcData
    {
        WireId sink_wire;
        ArcBounds bb;
        bool routed = false;
        float arc_crit = 0;
    };

    // As we allow overlap at first; the nextpnr bind functions can't be used
    // as the primary relation between arcs and wires/pips
    struct PerNetData
    {
        WireId src_wire;
        std::vector<PerArcData> arcs;
        ArcBounds bb;
        // Coordinates of the center of the net, used for the weight-to-average
        int cx, cy, hpwl;
        int total_route_us = 0;
        float max_crit = 0;
        int fail_count = 0;

        dict<uint32_t, std::pair<PipId, int>> wires;
    };

    struct WireScore
    {
        float cost;
        float togo_cost;
        delay_t delay;
        float total() const { return cost + togo_cost; }
    };

    struct PerWireData
    {
        // nextpnr
        WireId w;
        // net --> number of arcs; driving pip
        boost::container::flat_map<int, std::pair<int, PipId>> bound_nets;
        // Historical congestion cost
        float hist_cong_cost = 1.0;
        // Wire is unavailable as locked to another arc
        bool unavailable = false;
        // This wire has to be used for this net
        int reserved_net = -1;
        // The notional location of the wire, to guarantee thread safety
        int16_t x = 0, y = 0;
        // Visit data
        struct
        {
            bool dirty = false, visited_fwd = false, visited_bwd = false;
            PipId pip_fwd, pip_bwd;
            WireScore score_fwd, score_bwd;
        } visit;
    };

    float present_wire_cost(const PerWireData &w, int net_uid)
    {
        int other_sources = int(w.bound_nets.size());
        if (w.bound_nets.count(net_uid))
            other_sources -= 1;
        if (other_sources == 0)
            return 1.0f;
        else
            return 1 + other_sources * curr_cong_weight;
    }

    Context *ctx;
    Router2Cfg cfg;

    Router2(Context *ctx, const Router2Cfg &cfg) : ctx(ctx), cfg(cfg) {}

    // Use 'udata' for fast net lookups and indexing
    std::vector<NetInfo *> nets_by_udata;
    std::vector<PerNetData> nets;

    bool timing_driven;

    // Criticality data from timing analysis
    NetCriticalityMap net_crit;

    std::vector<int> tile_wire_offset;

    uint32_t flat_wire_index(WireId w) { return tile_wire_offset.at(w.tile) + w.index; }

    void setup_nets()
    {
        // Populate per-net and per-arc structures at start of routing
        nets.resize(ctx->nets.size());
        nets_by_udata.resize(ctx->nets.size());
        size_t i = 0;
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            ni->udata = i;
            nets_by_udata.at(i) = ni;
            nets.at(i).arcs.resize(ni->users.size());

            // Start net bounding box at overall min/max
            nets.at(i).bb.x0 = std::numeric_limits<int>::max();
            nets.at(i).bb.x1 = std::numeric_limits<int>::min();
            nets.at(i).bb.y0 = std::numeric_limits<int>::max();
            nets.at(i).bb.y1 = std::numeric_limits<int>::min();
            nets.at(i).cx = 0;
            nets.at(i).cy = 0;

            if (ni->driver.cell != nullptr) {
                Loc drv_loc = ctx->getBelLocation(ni->driver.cell->bel);
                nets.at(i).cx += drv_loc.x;
                nets.at(i).cy += drv_loc.y;
            }

            for (size_t j = 0; j < ni->users.size(); j++) {
                auto &usr = ni->users.at(j);
                WireId src_wire = ctx->getNetinfoSourceWire(ni), dst_wire = ctx->getNetinfoSinkWire(ni, usr);
                nets.at(i).src_wire = src_wire;
                if (ni->driver.cell == nullptr)
                    src_wire = dst_wire;
                if (ni->driver.cell == nullptr && dst_wire == WireId())
                    continue;
                if (src_wire == WireId())
                    log_error("No wire found for port %s on source cell %s.\n", ctx->nameOf(ni->driver.port),
                              ctx->nameOf(ni->driver.cell));
                if (dst_wire == WireId())
                    log_error("No wire found for port %s on destination cell %s.\n", ctx->nameOf(usr.port),
                              ctx->nameOf(usr.cell));
                nets.at(i).arcs.at(j).sink_wire = dst_wire;
                // Set bounding box for this arc
                nets.at(i).arcs.at(j).bb = ctx->getRouteBoundingBox(src_wire, dst_wire);
                // Expand net bounding box to include this arc
                nets.at(i).bb.x0 = std::min(nets.at(i).bb.x0, nets.at(i).arcs.at(j).bb.x0);
                nets.at(i).bb.x1 = std::max(nets.at(i).bb.x1, nets.at(i).arcs.at(j).bb.x1);
                nets.at(i).bb.y0 = std::min(nets.at(i).bb.y0, nets.at(i).arcs.at(j).bb.y0);
                nets.at(i).bb.y1 = std::max(nets.at(i).bb.y1, nets.at(i).arcs.at(j).bb.y1);
                // Add location to centroid sum
                Loc usr_loc = ctx->getBelLocation(usr.cell->bel);
                nets.at(i).cx += usr_loc.x;
                nets.at(i).cy += usr_loc.y;
            }
            nets.at(i).hpwl = std::max(
                    std::abs(nets.at(i).bb.y1 - nets.at(i).bb.y0) + std::abs(nets.at(i).bb.x1 - nets.at(i).bb.x0), 1);
            nets.at(i).cx /= int(ni->users.size() + 1);
            nets.at(i).cy /= int(ni->users.size() + 1);
            if (ctx->debug)
                log_info("%s: bb=(%d, %d)->(%d, %d) c=(%d, %d) hpwl=%d\n", ctx->nameOf(ni), nets.at(i).bb.x0,
                         nets.at(i).bb.y0, nets.at(i).bb.x1, nets.at(i).bb.y1, nets.at(i).cx, nets.at(i).cy,
                         nets.at(i).hpwl);
            nets.at(i).bb.x0 = std::max(nets.at(i).bb.x0 - cfg.bb_margin_x, 0);
            nets.at(i).bb.y0 = std::max(nets.at(i).bb.y0 - cfg.bb_margin_y, 0);
            nets.at(i).bb.x1 = std::min(nets.at(i).bb.x1 + cfg.bb_margin_x, ctx->getGridDimX());
            nets.at(i).bb.y1 = std::min(nets.at(i).bb.y1 + cfg.bb_margin_y, ctx->getGridDimY());
            i++;
        }
    }

    template <uint8_t bits> struct PackedArray
    {
        using Tchunk = uint64_t;
        using Tvalue = uint8_t;
        static constexpr Tchunk vals_per_chunk = (64 / bits);
        static constexpr Tchunk mask = ((1 << bits) - 1);

        PackedArray() = default;
        PackedArray(size_t size) { resize(size); };
        Tvalue get(size_t index) const
        {
            Tchunk ch = chunks.at(index / vals_per_chunk);
            return (ch >> ((index % vals_per_chunk) * bits)) & mask;
        }
        void set(size_t index, Tvalue value)
        {
            Tchunk &ch = chunks.at(index / vals_per_chunk);
            size_t bit_offset = (index % vals_per_chunk) * bits;
            ch &= ~(mask << (bit_offset));
            ch |= ((value & mask) << bit_offset);
        }
        void resize(size_t new_size) { chunks.resize((new_size + vals_per_chunk - 1) / vals_per_chunk); }
        std::vector<Tchunk> chunks;
    };

    struct WireStatus
    {
        WireStatus() = default;
        WireStatus(int curr_cong, int hist_cong, IdString reserved = {})
                : curr_cong(curr_cong), hist_cong(hist_cong), reserved(reserved){};
        int curr_cong;
        int hist_cong;
        IdString reserved;
    };

    struct WireStatusStore
    {
        PackedArray<3> flat_data;
        // TODO: faster hash maps?
        dict<uint32_t, WireStatus> ext_data;
        mutable std::shared_timed_mutex ext_data_mutex;
        WireStatus get(uint32_t wire_idx) const
        {
            uint8_t f = flat_data.get(wire_idx);
            if (f == 0x7) {
                // wire is in extended array
                std::shared_lock<std::shared_timed_mutex> guard(ext_data_mutex);
                auto data = ext_data.at(wire_idx);
                return data;
            } else {
                return WireStatus((f & 0x3), (f >> 2));
            }
        }
        void set(uint32_t wire_idx, const WireStatus &st)
        {
            // Conditions to use extended data
            if (st.curr_cong > 2 || st.hist_cong > 1 || st.reserved != IdString()) {
                std::unique_lock<std::shared_timed_mutex> guard(ext_data_mutex);
                ext_data[wire_idx] = st;
                flat_data.set(wire_idx, 0x7);
            } else {
                flat_data.set(wire_idx, (st.hist_cong << 2) | st.curr_cong);
            }
        }
    };

    WireStatusStore wires;

    void setup_flat_wire_index()
    {
        int offset = 0;
        for (int i = 0; i < ctx->chip_info->num_tiles; i++) {
            // Round up to nearest multiple of wire store size so each tile is in its own uint64_t chunk - avoiding
            // cross-thread RMW issues
            uint32_t chunk_size = wires.flat_data.vals_per_chunk;
            offset = ((offset + (chunk_size - 1)) / chunk_size) * chunk_size;
            tile_wire_offset.push_back(offset);
            offset += ctx->chip_info->tile_types[ctx->chip_info->tile_insts[i].type].num_wires;
        }
        wires.flat_data.resize(offset);
    }

    void setup_wires()
    {
        setup_flat_wire_index();

        for (WireId wire : ctx->getWires()) {
            uint32_t idx = flat_wire_index(wire);
            WireStatus st;
            st.curr_cong = 0;
            st.hist_cong = 0;
            st.reserved = IdString();
            NetInfo *bound = ctx->getBoundWireNet(wire);
            if (bound != nullptr) {
                st.curr_cong = 1;
                nets.at(bound->udata).wires.emplace(idx, std::make_pair(bound->wires.at(wire).pip, 1));
                if (bound->wires.at(wire).strength > STRENGTH_STRONG)
                    st.reserved = ctx->id("$router2$unavailable$");
            }
            wires.set(idx, st);
        }
    }

    struct QueuedWire
    {

        explicit QueuedWire(WireId wire = WireId(), PipId pip = PipId(), Loc loc = Loc(), WireScore score = WireScore{},
                            int randtag = 0)
                : wire(wire), pip(pip), loc(loc), score(score), randtag(randtag){};

        WireId wire;
        PipId pip;
        Loc loc;
        WireScore score;
        int randtag = 0;

        struct Greater
        {
            bool operator()(const QueuedWire &lhs, const QueuedWire &rhs) const noexcept
            {
                float lhs_score = lhs.score.cost + lhs.score.togo_cost,
                      rhs_score = rhs.score.cost + rhs.score.togo_cost;
                return lhs_score == rhs_score ? lhs.randtag > rhs.randtag : lhs_score > rhs_score;
            }
        };
    };

    bool hit_test_pip(ArcBounds &bb, Loc l) { return l.x >= bb.x0 && l.x <= bb.x1 && l.y >= bb.y0 && l.y <= bb.y1; }

    double curr_cong_weight, hist_cong_weight, estimate_weight;

    struct ThreadContext
    {
        // Nets to route
        std::vector<NetInfo *> route_nets;
        // Nets that failed routing
        std::vector<NetInfo *> failed_nets;

        std::vector<int> route_arcs;

        std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> queue;
        // Special case where one net has multiple logical arcs to the same physical sink
        pool<WireId> processed_sinks;

        // Backwards routing
        std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> backwards_queue;

        dict<uint32_t, PipId> wire2pip_fwd, wire2pip_bwd;

        // Thread bounding box
        ArcBounds bb;

        DeterministicRNG rng;

        // Used to add existing routing to the heap
        dict<WireId, float> wire_costs;
        dict<std::pair<int, int>, pool<WireId>> wire_by_loc;
    };

    std::pair<int, int> get_wire_loc(WireId w)
    {
        int x = (w.tile % ctx->chip_info->width);
        int y = (w.tile / ctx->chip_info->width);
        return std::make_pair(x, y);
    }

    bool thread_test_wire(ThreadContext &t, WireId w)
    {
        int x = 0, y = 0;
        std::tie(x, y) = get_wire_loc(w);
        return x >= t.bb.x0 && x <= t.bb.x1 && y >= t.bb.y0 && y <= t.bb.y1;
    }

    enum ArcRouteResult
    {
        ARC_SUCCESS,
        ARC_RETRY_WITHOUT_BB,
        ARC_FATAL,
    };

// Define to make sure we don't print in a multithreaded context
#define ARC_LOG_ERR(...)                                                                                               \
    do {                                                                                                               \
        if (is_mt)                                                                                                     \
            return ARC_FATAL;                                                                                          \
        else                                                                                                           \
            log_error(__VA_ARGS__);                                                                                    \
    } while (0)
#define ROUTE_LOG_DBG(...)                                                                                             \
    do {                                                                                                               \
        if (!is_mt && ctx->debug)                                                                                      \
            log(__VA_ARGS__);                                                                                          \
    } while (0)

    void bind_pip_internal(PerNetData &net, size_t user, uint32_t wire, PipId pip)
    {
        auto found = net.wires.find(wire);
        if (found == net.wires.end()) {
            // Not yet used for any arcs of this net, add to list
            net.wires.emplace(wire, std::make_pair(pip, 1));
            // Increase bound count of wire by 1
            WireStatus st = wires.get(wire);
            ++st.curr_cong;
            wires.set(wire, st);
        } else {
            // Already used for at least one other arc of this net
            // Don't allow two uphill PIPs for the same net and wire
            NPNR_ASSERT(found->second.first == pip);
            // Increase the count of bound arcs
            ++found->second.second;
        }
    }

    void unbind_pip_internal(PerNetData &net, size_t user, uint32_t wire)
    {
        auto &b = net.wires.at(wire);
        --b.second;
        if (b.second == 0) {
            // No remaining arcs of this net bound to this wire
            WireStatus st = wires.get(wire);
            --st.curr_cong;
            wires.set(wire, st);
            net.wires.erase(wire);
        }
    }

    void ripup_arc(NetInfo *net, size_t user)
    {
        auto &nd = nets.at(net->udata);
        auto &ad = nd.arcs.at(user);
        if (!ad.routed)
            return;
        WireId src = nd.src_wire;
        WireId cursor = ad.sink_wire;
        while (cursor != src) {
            uint32_t cursor_idx = flat_wire_index(cursor);
            PipId pip = nd.wires.at(cursor_idx).first;
            unbind_pip_internal(nd, user, cursor_idx);
            cursor = ctx->getPipSrcWire(pip);
        }
        ad.routed = false;
    }

    float score_wire_for_arc(NetInfo *net, size_t user, WireId wire, uint32_t wire_idx, PipId pip)
    {
        auto wd = wires.get(wire_idx);
        auto &nd = nets.at(net->udata);
        float base_cost = ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(wire).maxDelay() +
                                          ctx->getDelayEpsilon());

        int source_uses = 0;
        int other_sources = wd.curr_cong;
        auto fnd_wire = nd.wires.find(wire_idx);
        if (fnd_wire != nd.wires.end()) {
            source_uses = fnd_wire->second.second;
            other_sources -= 1;
        }
        float present_cost = 1.0f;
        if (other_sources > 0) {
            present_cost = 1.0f + other_sources * curr_cong_weight;
        }

        float hist_cost = 1.0f + wd.hist_cong * hist_cong_weight;
        float bias_cost = 0;
        /*
                if (pip != PipId()) {
                    Loc pl = ctx->getPipLocation(pip);
                    bias_cost = cfg.bias_cost_factor * (base_cost / int(net->users.size())) *
                                ((std::abs(pl.x - nd.cx) + std::abs(pl.y - nd.cy)) / float(nd.hpwl));
                }
        */
        return base_cost * hist_cost * present_cost / (1 + source_uses) + bias_cost;
    }

    float get_togo_cost(NetInfo *net, size_t user, WireId wire, int wire_idx, WireId src_sink, bool bwd)
    {
        int source_uses = 0;
        auto &nd = nets.at(net->udata);
        auto fnd_wire = nd.wires.find(wire_idx);

        if (fnd_wire != nd.wires.end())
            source_uses = fnd_wire->second.second;

        // FIXME: timing/wirelength balance?
        return (ctx->getDelayNS(ctx->estimateDelay(bwd ? src_sink : wire, bwd ? wire : src_sink)) / (1 + source_uses)) +
               cfg.ipin_cost_adder;
    }

    bool check_arc_routing(NetInfo *net, size_t usr)
    {
        auto &nd = nets.at(net->udata);
        auto &ad = nd.arcs.at(usr);
        WireId src_wire = nd.src_wire;
        WireId cursor = ad.sink_wire;
        uint32_t cursor_idx = flat_wire_index(cursor);
        while (nd.wires.count(cursor_idx)) {
            auto wd = wires.get(cursor_idx);
            if (wd.curr_cong != 1)
                return false;
            auto &uh = nd.wires.at(cursor_idx).first;
            if (uh == PipId())
                break;
            cursor = ctx->getPipSrcWire(uh);
            cursor_idx = flat_wire_index(cursor);
        }
        return (cursor == src_wire);
    }

    // Returns true if a wire contains no source ports or driving pips
    bool is_wire_undriveable(WireId wire, const NetInfo *net, int iter_count = 0)
    {
        // This is specifically designed to handle a particularly icky case that the current router struggles with in
        // the nexus device,
        // C -> C lut input only
        // C; D; or F from another lut -> D lut input
        // D or M -> M ff input
        // without careful reservation of C for C lut input and D for D lut input, there is fighting for D between FF
        // and LUT
        if (iter_count > 0)
            return false; // heuristic to assume we've hit general routing
        auto wd = wires.get(flat_wire_index(wire));
        if (wd.reserved != IdString() && wd.reserved != net->name)
            return true; // reserved for another net
        for (auto bp : ctx->getWireBelPins(wire))
            if ((net->driver.cell == nullptr || bp.bel == net->driver.cell->bel) &&
                ctx->getBelPinType(bp.bel, bp.pin) != PORT_IN)
                return false;
        for (auto p : ctx->getPipsUphill(wire))
            if (ctx->checkPipAvail(p)) {
                if (!is_wire_undriveable(ctx->getPipSrcWire(p), net, iter_count + 1))
                    return false;
            }
        return true;
    }

    // Find all the wires that must be used to route a given arc
    bool reserve_wires_for_arc(NetInfo *net, size_t i)
    {
        bool did_something = false;
        WireId src = ctx->getNetinfoSourceWire(net);
        WireId sink = ctx->getNetinfoSinkWire(net, net->users.at(i));
        if (sink == WireId())
            return false;
        pool<WireId> rsv;
        WireId cursor = sink;
        bool done = false;
        if (ctx->debug)
            log("resevering wires for arc %d of net %s\n", int(i), ctx->nameOf(net));
        while (!done) {
            uint32_t cursor_idx = flat_wire_index(cursor);
            auto wd = wires.get(cursor_idx);
            if (ctx->debug)
                log("      %s\n", ctx->nameOfWire(cursor));
            if (wd.reserved != net->name) {
                did_something = true;
                wd.reserved = net->name;
                wires.set(cursor_idx, wd);
            }
            if (cursor == src)
                break;
            WireId next_cursor;
            for (auto uh : ctx->getPipsUphill(cursor)) {
                WireId w = ctx->getPipSrcWire(uh);
                if (is_wire_undriveable(w, net))
                    continue;
                if (next_cursor != WireId()) {
                    done = true;
                    break;
                }
                next_cursor = w;
            }
            if (next_cursor == WireId())
                break;
            cursor = next_cursor;
        }
        return did_something;
    }

    void find_all_reserved_wires()
    {
        // Run iteratively, as reserving wires for one net might limit choices for another
        bool did_something = false;
        do {
            did_something = false;
            for (auto net : nets_by_udata) {
                WireId src = ctx->getNetinfoSourceWire(net);
                if (src == WireId())
                    continue;
                for (size_t i = 0; i < net->users.size(); i++)
                    did_something |= reserve_wires_for_arc(net, i);
            }
        } while (did_something);
    }

    void reset_wires(ThreadContext &t)
    {
        t.wire2pip_fwd.clear();
        t.wire2pip_bwd.clear();
    }

    void set_visited_fwd(ThreadContext &t, int wire, PipId pip) { t.wire2pip_fwd[wire] = pip; }

    void set_visited_bwd(ThreadContext &t, int wire, PipId pip) { t.wire2pip_bwd[wire] = pip; }

    bool was_visited_fwd(ThreadContext &t, int wire) { return t.wire2pip_fwd.count(wire); }
    bool was_visited_bwd(ThreadContext &t, int wire) { return t.wire2pip_bwd.count(wire); }

#ifdef ARCH_XILINX
    // Special-case constant ground/vcc routing for Xilinx devices
    void route_xilinx_const(ThreadContext &t, NetInfo *net, size_t i, int src_wire_idx, WireId dst_wire, bool is_mt,
                            bool is_bb = true)
    {
        auto &nd = nets[net->udata];
        auto &ad = nd.arcs[i];

        int backwards_iter = 0;
        int backwards_limit = 5000000;

        bool const_val = false;
        if (net->name == ctx->id("$PACKER_VCC_NET"))
            const_val = true;
        else
            NPNR_ASSERT(net->name == ctx->id("$PACKER_GND_NET"));

        for (int allowed_cong = 0; allowed_cong < 10; allowed_cong++) {
            backwards_iter = 0;
            std::queue<WireId> wire_queue;
            wire_queue.push(dst_wire);
            reset_wires(t);
            while (!wire_queue.empty() && backwards_iter < backwards_limit) {
                WireId cursor_wire = wire_queue.front();
                uint32_t cursor = flat_wire_index(cursor_wire);
                wire_queue.pop();
                PipId cpip;
                if (nd.wires.count(cursor)) {
                    // If we can tack onto existing routing; try that
                    // Only do this if the existing routing is uncontented; however
                    WireId cursor2_wire = cursor_wire;
                    int cursor2 = cursor;
                    bool bwd_merge_fail = false;
                    while (nd.wires.count(cursor2)) {
                        PipId p = nd.wires.at(cursor2).first;
                        if (p == PipId())
                            break;
                        cursor2_wire = ctx->getPipSrcWire(p);
                        cursor2 = flat_wire_index(cursor2_wire);
                    }
                    if (!bwd_merge_fail && cursor2 == src_wire_idx) {
                        // Found a path to merge to existing routing; backwards
                        cursor2 = cursor;
                        cursor2_wire = cursor_wire;
                        while (nd.wires.count(cursor2)) {
                            PipId p = nd.wires.at(cursor2).first;
                            if (p == PipId())
                                break;
                            cursor2_wire = ctx->getPipSrcWire(p);
                            cursor2 = flat_wire_index(cursor2_wire);
                            set_visited_bwd(t, cursor2, p);
                        }
                        break;
                    }
                    cpip = nd.wires.at(cursor).first;
                }
#if 0
                log("   explore %s\n", ctx->nameOfWire(cwd.w));
#endif
                if (ctx->wireIntent(cursor_wire) == (const_val ? ID_PSEUDO_VCC : ID_PSEUDO_GND)) {
#if 0
                    log("    Hit global network at %s\n", ctx->nameOfWire(cwd.w));
#endif
                    // We've hit the constant pseudo-network, continue from here
                    WireId cursor2_wire = cursor_wire;
                    int cursor2 = cursor;
                    while (cursor2 != src_wire_idx) {
                        bool found = false;
                        for (auto p : ctx->getPipsUphill(cursor2_wire)) {
                            if (!ctx->checkPipAvail(p) && ctx->getBoundPipNet(p) != net)
                                continue;
                            WireId src = ctx->getPipSrcWire(p);
                            if (ctx->wireIntent(src) != (const_val ? ID_PSEUDO_VCC : ID_PSEUDO_GND))
                                continue;
                            if (is_wire_undriveable(src, net))
                                continue;
                            cursor2_wire = src;
                            cursor2 = flat_wire_index(cursor2_wire);
                            set_visited_bwd(t, cursor2, p);
                            found = true;
                            break;
                        }
                        if (!found)
                            log_error("Invalid global constant node '%s'\n", ctx->nameOfWire(cursor2_wire));
                    }

                    break;
                }
#if 0
                std::string name = ctx->nameOfWire(cwd.w);
                if (name.substr(int(name.size()) - 3) == "A_O") {
                    for (auto uh : ctx->getPipsUphill(flat_wires[cursor].w)) {
                        log("   %s <-- %s %d\n", ctx->nameOfWire(flat_wires[cursor].w), ctx->nameOfWire(ctx->getPipSrcWire(uh)), int(!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net));
                    }
                }
#endif
                bool did_something = false;
                for (auto uh : ctx->getPipsUphill(cursor_wire)) {
                    did_something = true;
                    if (!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net)
                        continue;
                    if (cpip != PipId() && cpip != uh)
                        continue; // don't allow multiple pips driving a wire with a net
                    WireId next_wire = ctx->getPipSrcWire(uh);
                    int next = flat_wire_index(next_wire);
                    if (was_visited_bwd(t, next))
                        continue; // skip wires that have already been visited
                    auto wd = wires.get(next);
                    if (wd.reserved != IdString() && wd.reserved != net->name)
                        continue;
                    if (wd.curr_cong > (allowed_cong + 1) ||
                        (allowed_cong == 0 && wd.curr_cong == 1 && !nd.wires.count(next)))
                        continue; // never allow congestion in backwards routing
                    wire_queue.push(next_wire);
                    set_visited_bwd(t, next, uh);
                }
                if (did_something)
                    ++backwards_iter;
            }
            int dst_wire_idx = flat_wire_index(dst_wire);
            if (was_visited_bwd(t, src_wire_idx)) {
                ROUTE_LOG_DBG("   Routed (backwards): ");
                int cursor_fwd = src_wire_idx;
                bind_pip_internal(nd, i, src_wire_idx, PipId());
                while (was_visited_bwd(t, cursor_fwd)) {
                    PipId p = t.wire2pip_bwd.at(cursor_fwd);
                    cursor_fwd = flat_wire_index(ctx->getPipDstWire(p));
                    bind_pip_internal(nd, i, cursor_fwd, p);
                    if (ctx->debug) {
                        auto wd = wires.get(cursor_fwd);
                        ROUTE_LOG_DBG("      wire: %s (curr %d hist %d)\n", ctx->nameOfWire(ctx->getPipSrcWire(p)),
                                      wd.curr_cong - 1, wd.hist_cong);
                    }
                }
                NPNR_ASSERT(cursor_fwd == dst_wire_idx);
                ad.routed = true;
                t.processed_sinks.insert(dst_wire);
                reset_wires(t);
                return;
            }
        }
        log_error("Unrouteable %s sink %s.%s (%s)\n", ctx->nameOf(net), ctx->nameOf(net->users.at(i).cell),
                  ctx->nameOf(net->users.at(i).port), ctx->nameOfWire(dst_wire));
    }
#endif

    void update_sink_costs(ThreadContext &t, NetInfo *net, size_t i, bool is_mt)
    {
        std::vector<PipId> path;
        auto &nd = nets.at(net->udata);
        auto &ad = nd.arcs.at(i);
        WireId cursor = ad.sink_wire;
        if (!nd.wires.count(flat_wire_index(cursor)))
            return;
        while (cursor != nd.src_wire) {
            auto &wd = nd.wires.at(flat_wire_index(cursor));
            auto p = wd.first;
            path.push_back(p);
            cursor = ctx->getPipSrcWire(p);
        }
        std::reverse(path.begin(), path.end());
        float base_cost = 0;
        for (auto pip : path) {
            WireId wire = ctx->getPipDstWire(pip);
            uint32_t wire_idx = flat_wire_index(wire);
            base_cost += score_wire_for_arc(net, i, wire, wire_idx, pip);
            ROUTE_LOG_DBG("   bt %s acc %f fwd %f bwd %f\n", ctx->nameOfWire(wire), base_cost,
                          get_togo_cost(net, i, wire, wire_idx, ad.sink_wire, false),
                          get_togo_cost(net, i, wire, wire_idx, nd.src_wire, true));
            if (!t.wire_costs.count(wire)) {
                t.wire_costs[wire] = base_cost;
                for (auto dh : ctx->getPipsDownhill(wire)) {
                    Loc dh_loc = ctx->getPipLocation(dh);
                    t.wire_by_loc[std::make_pair(dh_loc.x, dh_loc.y)].insert(wire);
                }
            }
        }
    }

    ArcRouteResult route_arc(ThreadContext &t, NetInfo *net, size_t i, bool is_mt, bool is_bb = true)
    {

        auto &nd = nets[net->udata];
        auto &ad = nd.arcs[i];
        auto &usr = net->users.at(i);
        ROUTE_LOG_DBG("Routing arc %d of net '%s' (%d, %d) -> (%d, %d)\n", int(i), ctx->nameOf(net), ad.bb.x0, ad.bb.y0,
                      ad.bb.x1, ad.bb.y1);
        WireId src_wire = ctx->getNetinfoSourceWire(net), dst_wire = ctx->getNetinfoSinkWire(net, usr);
        if (src_wire == WireId())
            ARC_LOG_ERR("No wire found for port %s on source cell %s.\n", ctx->nameOf(net->driver.port),
                        ctx->nameOf(net->driver.cell));
        if (dst_wire == WireId())
            ARC_LOG_ERR("No wire found for port %s on destination cell %s.\n", ctx->nameOf(usr.port),
                        ctx->nameOf(usr.cell));
        int src_wire_idx = flat_wire_index(src_wire);
        int dst_wire_idx = flat_wire_index(dst_wire);
        // Check if arc was already done _in this iteration_
        if (t.processed_sinks.count(dst_wire))
            return ARC_SUCCESS;

            // Special case
#ifdef ARCH_XILINX
        if (net->name == ctx->id("$PACKER_GND_NET") || net->name == ctx->id("$PACKER_VCC_NET")) {
            route_xilinx_const(t, net, i, src_wire_idx, dst_wire, is_mt, is_bb);
            return ARC_SUCCESS;
        }
#endif

        // Once we meet in the middle, set this to the meeting point
        WireId midpoint_wire = WireId();
        int explored = 0;

        // We have two modes:
        //     - starting within a small range of existing routing
        //     - expanding from all routing
        int mode = 0;
        if (net->users.size() < 4 || nd.wires.empty())
            mode = 1;

        for (; mode < 2; mode++) {
            if (!t.queue.empty()) {
                std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> new_queue;
                t.queue.swap(new_queue);
            }
            if (!t.backwards_queue.empty()) {
                std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> new_queue;
                t.backwards_queue.swap(new_queue);
            }

            reset_wires(t);

            auto seed_queue_fwd = [&](WireId wire, float wire_cost = 0) {
                WireScore base_score;
                base_score.cost = wire_cost;
                base_score.delay = 0;
                base_score.togo_cost = get_togo_cost(net, i, wire, flat_wire_index(wire), dst_wire, false);
                t.queue.push(QueuedWire(wire, PipId(), Loc(), base_score));
                set_visited_fwd(t, flat_wire_index(wire), PipId());
            };

            ArcBounds route_box;

            // Also seed with nearby routing
            auto dst_loc = get_wire_loc(dst_wire);

            if (mode == 0) {
                route_box = ctx->getRouteBoundingBox(dst_wire, dst_wire);
                for (int dy = -cfg.bb_margin_y; dy <= cfg.bb_margin_y; dy++)
                    for (int dx = -cfg.bb_margin_x; dx <= cfg.bb_margin_x; dx++) {
                        auto fnd = t.wire_by_loc.find(std::make_pair(dst_loc.first + dx, dst_loc.second + dy));
                        if (fnd == t.wire_by_loc.end())
                            continue;
                        for (WireId wire : fnd->second) {
                            float cost = t.wire_costs.at(wire);
                            ROUTE_LOG_DBG("   seeding with %s %f\n", ctx->nameOfWire(wire), cost);
                            seed_queue_fwd(wire, 0);
                            route_box.x0 = std::min(route_box.x0, fnd->first.first - cfg.bb_margin_x);
                            route_box.y0 = std::min(route_box.y0, fnd->first.second - cfg.bb_margin_y);
                            route_box.x1 = std::max(route_box.x1, fnd->first.first + cfg.bb_margin_x);
                            route_box.y1 = std::max(route_box.y1, fnd->first.second + cfg.bb_margin_y);
                        }
                    }
            }
            route_box = nd.bb;

            if (mode == 0 && t.queue.size() < 4)
                continue;

            if (mode == 1) {
                // Seed forwards with the source wire, if less than 8 existing wires added
                seed_queue_fwd(src_wire);
            } else {
                set_visited_fwd(t, src_wire_idx, PipId());
            }

            auto seed_queue_bwd = [&](WireId wire) {
                WireScore base_score;
                base_score.cost = 0;
                base_score.delay = 0;
                base_score.togo_cost = get_togo_cost(net, i, wire, flat_wire_index(wire), src_wire, true);
                t.backwards_queue.push(QueuedWire(wire, PipId(), Loc(), base_score));
                set_visited_bwd(t, flat_wire_index(wire), PipId());
            };

            // Seed backwards with the dest wire
            seed_queue_bwd(dst_wire);

            int iter = 0;
            int toexplore = is_bb ? (25000 * std::max(1, (ad.bb.x1 - ad.bb.x0) + (ad.bb.y1 - ad.bb.y0))) : 1000000000;
            while (!t.queue.empty() && !t.backwards_queue.empty() && iter < toexplore) {
                ++iter;
                if (!t.queue.empty()) {
                    // Explore forwards
                    auto curr = t.queue.top();
                    WireId curr_wire = curr.wire;
                    uint32_t curr_idx = flat_wire_index(curr_wire);
                    t.queue.pop();
                    // Meet in the middle
                    if (was_visited_bwd(t, curr_idx)) {
                        midpoint_wire = curr_wire;
                        break;
                    }
                    // Explore all pips downhill of cursor
                    for (auto dh : ctx->getPipsDownhill(curr_wire)) {
                        // Skip pips outside of box in bounding-box mode
                        if (is_bb && !hit_test_pip(route_box, ctx->getPipLocation(dh)))
                            continue;
                        WireId next = ctx->getPipDstWire(dh);
                        uint32_t next_idx = flat_wire_index(next);
                        // Skip already visited wires
                        if (was_visited_fwd(t, next_idx))
                            continue;
                        // Skip pips that are permanently unavailable
                        if (!ctx->checkPipAvail(dh) && ctx->getBoundPipNet(dh) != net)
                            continue;
                        ++explored;
                        // Check data for next wire
                        auto nwd = wires.get(next_idx);
                        // Reserved, but for another net...
                        if (nwd.reserved != IdString() && nwd.reserved != net->name)
                            continue;
                        // Bound to this net, but with a different driving PIP
                        auto fnd = nd.wires.find(next_idx);
                        if (fnd != nd.wires.end() && fnd->second.first != dh)
                            continue;
                        if (!thread_test_wire(t, next))
                            continue; // thread safety issue
                        // All checks passed, mark as visited and add to the queue
                        WireScore next_score;
                        next_score.cost = curr.score.cost + score_wire_for_arc(net, i, next, next_idx, dh);
                        next_score.delay =
                                curr.score.delay + ctx->getPipDelay(dh).maxDelay() + ctx->getWireDelay(next).maxDelay();
                        next_score.togo_cost =
                                cfg.estimate_weight * get_togo_cost(net, i, next, next_idx, dst_wire, false);
                        t.queue.push(QueuedWire(next, dh, ctx->getPipLocation(dh), next_score, t.rng.rng()));
                        set_visited_fwd(t, next_idx, dh);
                    }
                }

                if (!t.backwards_queue.empty()) {
                    // Explore forwards
                    auto curr = t.backwards_queue.top();
                    WireId curr_wire = curr.wire;
                    uint32_t curr_idx = flat_wire_index(curr_wire);
                    t.backwards_queue.pop();
                    // Meet in the middle
                    if (was_visited_fwd(t, curr_idx)) {
                        midpoint_wire = curr_wire;
                        break;
                    }
                    // Check if there's existing routing
                    PipId curr_pip = PipId();
                    if (nd.wires.count(curr_idx))
                        curr_pip = nd.wires.at(curr_idx).first;
                    // Explore all pips uphill of cursor
                    for (auto uh : ctx->getPipsUphill(curr_wire)) {
                        // Existing routing, only follow the existing path
                        if (curr_pip != PipId() && curr_pip != uh)
                            continue;
                        WireId next = ctx->getPipSrcWire(uh);
                        uint32_t next_idx = flat_wire_index(next);
                        // Skip already visited wires
                        if (was_visited_bwd(t, next_idx))
                            continue;
                        // Skip pips outside of box in bounding-box mode
                        if (is_bb && !hit_test_pip(route_box, ctx->getPipLocation(uh)))
                            continue;
                        // Skip pips that are permanently unavailable
                        if (!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net)
                            continue;
                        ++explored;
                        // Check data for next wire
                        auto nwd = wires.get(next_idx);
                        // Reserved, but for another net...
                        if (nwd.reserved != IdString() && nwd.reserved != net->name)
                            continue;
                        if (!thread_test_wire(t, next))
                            continue; // thread safety issue
                        // All checks passed, mark as visited and add to the queue
                        WireScore next_score;
                        next_score.cost = curr.score.cost + score_wire_for_arc(net, i, next, next_idx, uh);
                        next_score.delay =
                                curr.score.delay + ctx->getPipDelay(uh).maxDelay() + ctx->getWireDelay(next).maxDelay();
                        next_score.togo_cost =
                                cfg.estimate_weight * get_togo_cost(net, i, next, next_idx, src_wire, true);
                        t.backwards_queue.push(QueuedWire(next, uh, ctx->getPipLocation(uh), next_score, t.rng.rng()));
                        set_visited_bwd(t, next_idx, uh);
                    }
                }
            }

            ROUTE_LOG_DBG("mode %d explored %d\n", mode, explored);

            if (midpoint_wire != WireId())
                break;
        }

        if (midpoint_wire != WireId()) {
            ROUTE_LOG_DBG("   Routed (explored %d wires): ", explored);
            int cursor_bwd = flat_wire_index(midpoint_wire);
            while (was_visited_fwd(t, cursor_bwd)) {
                PipId pip = t.wire2pip_fwd.at(cursor_bwd);
                if (pip == PipId() && cursor_bwd != src_wire_idx)
                    break;
                bind_pip_internal(nd, i, cursor_bwd, pip);
                auto wd = wires.get(cursor_bwd);
                ROUTE_LOG_DBG("      fwd wire: %s (curr %d hist %d share %d)\n",
                              (pip == PipId()) ? ctx->nameOfWire(src_wire) : ctx->nameOfWire(ctx->getPipSrcWire(pip)),
                              wd.curr_cong - 1, wd.hist_cong, nd.wires.at(cursor_bwd).second);
                if (pip == PipId()) {
                    break;
                }
                ROUTE_LOG_DBG("         fwd pip: %s (%d, %d)\n", ctx->nameOfPip(pip), ctx->getPipLocation(pip).x,
                              ctx->getPipLocation(pip).y);
                cursor_bwd = flat_wire_index(ctx->getPipSrcWire(pip));
            }

            while (true) {
                // Tack onto existing routing
                if (!nd.wires.count(cursor_bwd))
                    break;
                auto &bound = nd.wires.at(cursor_bwd);
                PipId pip = bound.first;
                auto wd = wires.get(cursor_bwd);
                ROUTE_LOG_DBG("      ext wire: %s (curr %d hist %d share %d)\n",
                              (pip == PipId()) ? ctx->nameOfWire(src_wire) : ctx->nameOfWire(ctx->getPipSrcWire(pip)),
                              wd.curr_cong - 1, wd.hist_cong, bound.second);
                bind_pip_internal(nd, i, cursor_bwd, pip);
                if (pip == PipId())
                    break;
                cursor_bwd = flat_wire_index(ctx->getPipSrcWire(pip));
            }

            NPNR_ASSERT(cursor_bwd == src_wire_idx);

            int cursor_fwd = flat_wire_index(midpoint_wire);
            while (was_visited_bwd(t, cursor_fwd)) {
                PipId pip = t.wire2pip_bwd.at(cursor_fwd);
                if (ctx->debug) {
                    auto wd = wires.get(cursor_fwd);
                    ROUTE_LOG_DBG("      bwd wire: %s (curr %d hist %d share %d)\n",
                                  (pip == PipId()) ? ctx->nameOfWire(dst_wire)
                                                   : ctx->nameOfWire(ctx->getPipDstWire(pip)),
                                  wd.curr_cong - 1, wd.hist_cong, nd.wires.at(cursor_fwd).second);
                }
                if (pip == PipId()) {
                    break;
                }
                ROUTE_LOG_DBG("         bwd pip: %s (%d, %d)\n", ctx->nameOfPip(pip), ctx->getPipLocation(pip).x,
                              ctx->getPipLocation(pip).y);
                cursor_fwd = flat_wire_index(ctx->getPipDstWire(pip));
                bind_pip_internal(nd, i, cursor_fwd, pip);
            }
            NPNR_ASSERT(cursor_fwd == dst_wire_idx);

            update_sink_costs(t, net, i, is_mt);
            t.processed_sinks.insert(dst_wire);
            ad.routed = true;
            reset_wires(t);
            return ARC_SUCCESS;
        } else {
            reset_wires(t);
            return ARC_RETRY_WITHOUT_BB;
        }
    }
#undef ARC_ERR

    bool route_net(ThreadContext &t, NetInfo *net, bool is_mt)
    {

#ifdef ARCH_ECP5
        if (net->is_global)
            return true;
#endif

        ROUTE_LOG_DBG("Routing net '%s'...\n", ctx->nameOf(net));

        auto rstart = std::chrono::high_resolution_clock::now();

        // Nothing to do if net is undriven
        if (net->driver.cell == nullptr)
            return true;

        bool have_failures = false;
        t.processed_sinks.clear();
        t.route_arcs.clear();
        t.wire_costs.clear();
        t.wire_by_loc.clear();
        for (size_t i = 0; i < net->users.size(); i++) {
            // Ripup failed arcs to start with
            // Check if arc is already legally routed
            if (check_arc_routing(net, i)) {
                update_sink_costs(t, net, i, true);
                continue;
            }
            auto &usr = net->users.at(i);
            WireId dst_wire = ctx->getNetinfoSinkWire(net, usr);
            // Case of arcs that were pre-routed strongly (e.g. clocks)
            if (net->wires.count(dst_wire) && net->wires.at(dst_wire).strength > STRENGTH_STRONG)
                return ARC_SUCCESS;
            // Ripup arc to start with
            ripup_arc(net, i);
            t.route_arcs.push_back(i);
        }
        for (auto i : t.route_arcs) {
            auto res1 = route_arc(t, net, i, is_mt, true);
            if (res1 == ARC_FATAL)
                return false; // Arc failed irrecoverably
            else if (res1 == ARC_RETRY_WITHOUT_BB) {
                if (is_mt) {
                    // Can't break out of bounding box in multi-threaded mode, so mark this arc as a failure
                    have_failures = true;
                } else {
                    // Attempt a re-route without the bounding box constraint
                    ROUTE_LOG_DBG("Rerouting arc %d of net '%s' without bounding box, possible tricky routing...\n",
                                  int(i), ctx->nameOf(net));
                    auto res2 = route_arc(t, net, i, is_mt, false);
                    // If this also fails, no choice but to give up
                    if (res2 != ARC_SUCCESS)
                        log_error("Failed to route arc %d of net '%s', from %s to %s.\n", int(i), ctx->nameOf(net),
                                  ctx->nameOfWire(ctx->getNetinfoSourceWire(net)),
                                  ctx->nameOfWire(ctx->getNetinfoSinkWire(net, net->users.at(i))));
                }
            }
        }
        if (cfg.perf_profile) {
            auto rend = std::chrono::high_resolution_clock::now();
            nets.at(net->udata).total_route_us +=
                    (std::chrono::duration_cast<std::chrono::microseconds>(rend - rstart).count());
        }
        return !have_failures;
    }
#undef ROUTE_LOG_DBG

    int total_wire_use = 0;
    int overused_wires = 0;
    int total_overuse = 0;
    std::vector<int> route_queue;
    std::set<int> failed_nets;

    void update_congestion()
    {
        total_overuse = 0;
        overused_wires = 0;
        total_wire_use = 0;
        failed_nets.clear();
        pool<uint32_t> already_updated;
        for (size_t i = 0; i < nets.size(); i++) {
            auto &nd = nets.at(i);
            for (const auto &w : nd.wires) {
                ++total_wire_use;
                auto wd = wires.get(w.first);
                if (wd.curr_cong > 1) {
                    if (already_updated.count(w.first)) {
                        ++total_overuse;
                    } else {
                        if (curr_cong_weight > 0)
                            wd.hist_cong += (wd.curr_cong - 1);
                        wires.set(w.first, wd);
                        already_updated.insert(w.first);
                        ++overused_wires;
                    }
                    failed_nets.insert(i);
                }
            }
        }
        for (int n : failed_nets) {
            auto &net_data = nets.at(n);
            ++net_data.fail_count;
            if ((net_data.fail_count % 3) == 0) {
                // Every 3 times a net fails to route, expand the bounding box to increase the search space
                net_data.bb.x0 = std::max(net_data.bb.x0 - 1, 0);
                net_data.bb.y0 = std::max(net_data.bb.y0 - 1, 0);
                net_data.bb.x1 = std::min(net_data.bb.x1 + 1, ctx->getGridDimX());
                net_data.bb.y1 = std::min(net_data.bb.y1 + 1, ctx->getGridDimY());
            }
        }
    }

    bool bind_and_check(NetInfo *net, int usr_idx)
    {
#ifdef ARCH_ECP5
        if (net->is_global)
            return true;
#endif
        bool success = true;
        auto &nd = nets.at(net->udata);
        auto &ad = nd.arcs.at(usr_idx);
        auto &usr = net->users.at(usr_idx);
        WireId src = ctx->getNetinfoSourceWire(net);
        // Skip routes with no source
        if (src == WireId())
            return true;
        WireId dst = ctx->getNetinfoSinkWire(net, usr);
        // Skip routes where the destination is already bound
        if (dst == WireId() || ctx->getBoundWireNet(dst) == net)
            return true;

        if (dst == src) {
            NetInfo *bound = ctx->getBoundWireNet(src);
            if (bound == nullptr)
                ctx->bindWire(src, net, STRENGTH_WEAK);
            else
                NPNR_ASSERT(bound == net);
            return true;
        }

        // Skip routes where there is no routing (special cases)
        if (!ad.routed) {
            if ((src == dst) && ctx->getBoundWireNet(dst) != net)
                ctx->bindWire(src, net, STRENGTH_WEAK);
            return true;
        }

        WireId cursor = dst;

        std::vector<PipId> to_bind;

        while (cursor != src) {
            if (!ctx->checkWireAvail(cursor)) {
                if (ctx->getBoundWireNet(cursor) == net)
                    break; // hit the part of the net that is already bound
                else {
                    success = false;
                    break;
                }
            }
            auto &wd = nd.wires.at(flat_wire_index(cursor));
            auto p = wd.first;
            if (!ctx->checkPipAvail(p)) {
                success = false;
                break;
            } else {
                to_bind.push_back(p);
            }
            cursor = ctx->getPipSrcWire(p);
        }

        if (success) {
            if (ctx->getBoundWireNet(src) == nullptr)
                ctx->bindWire(src, net, STRENGTH_WEAK);
            for (auto tb : to_bind)
                ctx->bindPip(tb, net, STRENGTH_WEAK);
        } else {
            ripup_arc(net, usr_idx);
            failed_nets.insert(net->udata);
        }
        return success;
    }

    int arch_fail = 0;
    bool bind_and_check_all()
    {
        bool success = true;
        std::vector<WireId> net_wires;
        for (auto net : nets_by_udata) {
#ifdef ARCH_ECP5
            if (net->is_global)
                continue;
#endif
            // Ripup wires and pips used by the net in nextpnr's structures
            net_wires.clear();
            for (auto &w : net->wires) {
                if (w.second.strength <= STRENGTH_STRONG)
                    net_wires.push_back(w.first);
            }
            for (auto w : net_wires)
                ctx->unbindWire(w);
            // Bind the arcs using the routes we have discovered
            for (size_t i = 0; i < net->users.size(); i++) {
                if (!bind_and_check(net, i)) {
                    ++arch_fail;
                    success = false;
                }
            }
        }
        return success;
    }

    int mid_x = 0, mid_y = 0;

    void partition_nets()
    {
        // Create a histogram of positions in X and Y positions
        std::map<int, int> cxs, cys;
        for (auto &n : nets) {
            if (n.cx != -1)
                ++cxs[n.cx];
            if (n.cy != -1)
                ++cys[n.cy];
        }
        // 4-way split for now
        int accum_x = 0, accum_y = 0;
        int halfway = int(nets.size()) / 2;
        for (auto &p : cxs) {
            if (accum_x < halfway && (accum_x + p.second) >= halfway)
                mid_x = p.first;
            accum_x += p.second;
        }
        for (auto &p : cys) {
            if (accum_y < halfway && (accum_y + p.second) >= halfway)
                mid_y = p.first;
            accum_y += p.second;
        }
        if (ctx->verbose) {
            log_info("    x splitpoint: %d\n", mid_x);
            log_info("    y splitpoint: %d\n", mid_y);
        }
        std::vector<int> bins(5, 0);
        for (auto &n : nets) {
            if (n.bb.x0 < mid_x && n.bb.x1 < mid_x && n.bb.y0 < mid_y && n.bb.y1 < mid_y)
                ++bins[0]; // TL
            else if (n.bb.x0 >= mid_x && n.bb.x1 >= mid_x && n.bb.y0 < mid_y && n.bb.y1 < mid_y)
                ++bins[1]; // TR
            else if (n.bb.x0 < mid_x && n.bb.x1 < mid_x && n.bb.y0 >= mid_y && n.bb.y1 >= mid_y)
                ++bins[2]; // BL
            else if (n.bb.x0 >= mid_x && n.bb.x1 >= mid_x && n.bb.y0 >= mid_y && n.bb.y1 >= mid_y)
                ++bins[3]; // BR
            else
                ++bins[4]; // cross-boundary
        }
        if (ctx->verbose)
            for (int i = 0; i < 5; i++)
                log_info("        bin %d N=%d\n", i, bins[i]);
    }

    void router_thread(ThreadContext &t)
    {
        for (auto n : t.route_nets) {
            bool result = route_net(t, n, true);
            if (!result)
                t.failed_nets.push_back(n);
        }
    }

    void do_route()
    {
        // Don't multithread if fewer than 200 nets (heuristic)
        if (route_queue.size() < 200) {
            ThreadContext st;
            st.rng.rngseed(ctx->rng64());
            st.bb = ArcBounds(0, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
            for (size_t j = 0; j < route_queue.size(); j++) {
                route_net(st, nets_by_udata[route_queue[j]], false);
            }
            return;
        }
        const int Nq = 4, Nv = 2, Nh = 2;
        const int N = Nq + Nv + Nh;
        std::vector<ThreadContext> tcs(N + 1);
        for (auto &th : tcs) {
            th.rng.rngseed(ctx->rng64());
        }
        int le_x = mid_x;
        int rs_x = mid_x;
        int le_y = mid_y;
        int rs_y = mid_y;
        // Set up thread bounding boxes
        tcs.at(0).bb = ArcBounds(0, 0, mid_x, mid_y);
        tcs.at(1).bb = ArcBounds(mid_x + 1, 0, std::numeric_limits<int>::max(), le_y);
        tcs.at(2).bb = ArcBounds(0, mid_y + 1, mid_x, std::numeric_limits<int>::max());
        tcs.at(3).bb =
                ArcBounds(mid_x + 1, mid_y + 1, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());

        tcs.at(4).bb = ArcBounds(0, 0, std::numeric_limits<int>::max(), mid_y);
        tcs.at(5).bb = ArcBounds(0, mid_y + 1, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());

        tcs.at(6).bb = ArcBounds(0, 0, mid_x, std::numeric_limits<int>::max());
        tcs.at(7).bb = ArcBounds(mid_x + 1, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());

        tcs.at(8).bb = ArcBounds(0, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());

        for (auto n : route_queue) {
            auto &nd = nets.at(n);
            auto ni = nets_by_udata.at(n);
            int bin = N;
            // Quadrants
            if (nd.bb.x0 < le_x && nd.bb.x1 < le_x && nd.bb.y0 < le_y && nd.bb.y1 < le_y)
                bin = 0;
            else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x && nd.bb.y0 < le_y && nd.bb.y1 < le_y)
                bin = 1;
            else if (nd.bb.x0 < le_x && nd.bb.x1 < le_x && nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
                bin = 2;
            else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x && nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
                bin = 3;
            // Vertical split
            else if (nd.bb.y0 < le_y && nd.bb.y1 < le_y)
                bin = Nq + 0;
            else if (nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
                bin = Nq + 1;
            // Horizontal split
            else if (nd.bb.x0 < le_x && nd.bb.x1 < le_x)
                bin = Nq + Nv + 0;
            else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x)
                bin = Nq + Nv + 1;
            tcs.at(bin).route_nets.push_back(ni);
        }
        if (ctx->verbose)
            log_info("%d/%d nets not multi-threadable\n", int(tcs.at(N).route_nets.size()), int(route_queue.size()));
        // Multithreaded part of routing - quadrants
        std::vector<std::thread> threads;
        for (int i = 0; i < Nq; i++) {
            threads.emplace_back([this, &tcs, i]() { router_thread(tcs.at(i)); });
        }
        for (auto &t : threads)
            t.join();
        threads.clear();
        // Vertical splits
        for (int i = Nq; i < Nq + Nv; i++) {
            threads.emplace_back([this, &tcs, i]() { router_thread(tcs.at(i)); });
        }
        for (auto &t : threads)
            t.join();
        threads.clear();
        // Horizontal splits
        for (int i = Nq + Nv; i < Nq + Nv + Nh; i++) {
            threads.emplace_back([this, &tcs, i]() { router_thread(tcs.at(i)); });
        }
        for (auto &t : threads)
            t.join();
        threads.clear();
        // Singlethreaded part of routing - nets that cross partitions
        // or don't fit within bounding box
        for (auto st_net : tcs.at(N).route_nets)
            route_net(tcs.at(N), st_net, false);
        // Failed nets
        for (int i = 0; i < N; i++)
            for (auto fail : tcs.at(i).failed_nets)
                route_net(tcs.at(N), fail, false);
    }

    //#define ROUTER2_STATISTICS

    void operator()()
    {
        log_info("Running router2...\n");
        log_info("Setting up routing resources...\n");
        auto rstart = std::chrono::high_resolution_clock::now();
        setup_nets();
        setup_wires();
        find_all_reserved_wires();
        partition_nets();
        curr_cong_weight = cfg.init_curr_cong_weight;
        hist_cong_weight = cfg.hist_cong_weight;
        ThreadContext st;
        int iter = 1;

        for (size_t i = 0; i < nets_by_udata.size(); i++)
            route_queue.push_back(i);

        timing_driven = ctx->setting<bool>("timing_driven");
        log_info("Running main router loop...\n");
        do {
            ctx->sorted_shuffle(route_queue);

            if (timing_driven && (int(route_queue.size()) > (int(nets_by_udata.size()) / 50))) {
                // Heuristic: reduce runtime by skipping STA in the case of a "long tail" of a few
                // congested nodes
                get_criticalities(ctx, &net_crit);
                for (auto n : route_queue) {
                    IdString name = nets_by_udata.at(n)->name;
                    auto fnd = net_crit.find(name);
                    auto &net = nets.at(n);
                    net.max_crit = 0;
                    if (fnd == net_crit.end())
                        continue;
                    for (int i = 0; i < int(fnd->second.criticality.size()); i++) {
                        float c = fnd->second.criticality.at(i);
                        net.arcs.at(i).arc_crit = c;
                        net.max_crit = std::max(net.max_crit, c);
                    }
                }
                std::stable_sort(route_queue.begin(), route_queue.end(),
                                 [&](int na, int nb) { return nets.at(na).max_crit > nets.at(nb).max_crit; });
            }

#if 0
            for (size_t j = 0; j < route_queue.size(); j++) {
                route_net(st, nets_by_udata[route_queue[j]], false);
                if ((j % 1000) == 0 || j == (route_queue.size() - 1))
                    log("    routed %d/%d\n", int(j), int(route_queue.size()));
            }
#endif
            do_route();
            route_queue.clear();
            update_congestion();
#if 0
            if (iter == 1 && ctx->debug) {
                std::ofstream cong_map("cong_map_0.csv");
                write_heatmap(cong_map, true);
            }
#endif

            if (overused_wires == 0) {
                // Try and actually bind nextpnr Arch API wires
                bind_and_check_all();
            }
            for (auto cn : failed_nets)
                route_queue.push_back(cn);
            log_info("    iter=%d wires=%d overused=%d overuse=%d archfail=%s\n", iter, total_wire_use, overused_wires,
                     total_overuse, overused_wires > 0 ? "NA" : std::to_string(arch_fail).c_str());
            ++iter;
            if (curr_cong_weight == 0)
                curr_cong_weight = cfg.init_curr_cong_weight;
            else if (curr_cong_weight < 1e50)
                curr_cong_weight += cfg.curr_cong_mult;
        } while (!failed_nets.empty());
        if (cfg.perf_profile) {
            std::vector<std::pair<int, IdString>> nets_by_runtime;
            for (auto &n : nets_by_udata) {
                nets_by_runtime.emplace_back(nets.at(n->udata).total_route_us, n->name);
            }
            std::sort(nets_by_runtime.begin(), nets_by_runtime.end(), std::greater<std::pair<int, IdString>>());
            log_info("1000 slowest nets by runtime:\n");
            for (int i = 0; i < std::min(int(nets_by_runtime.size()), 1000); i++) {
                log("        %80s %6d %.1fms\n", nets_by_runtime.at(i).second.c_str(ctx),
                    int(ctx->nets.at(nets_by_runtime.at(i).second)->users.size()),
                    nets_by_runtime.at(i).first / 1000.0);
            }
        }
        auto rend = std::chrono::high_resolution_clock::now();
        log_info("Router2 time %.02fs\n", std::chrono::duration<float>(rend - rstart).count());

        log_info("Running router1 to check that route is legal...\n");

        router1(ctx, Router1Cfg(ctx));
    }
};
} // namespace

void router2(Context *ctx, const Router2Cfg &cfg)
{
    Router2 rt(ctx, cfg);
    rt.ctx = ctx;
    rt();
}

Router2Cfg::Router2Cfg(Context *ctx)
{
    backwards_max_iter = ctx->setting<int>("router2/bwdMaxIter", 20);
    global_backwards_max_iter = ctx->setting<int>("router2/glbBwdMaxIter", 200);
    bb_margin_x = ctx->setting<int>("router2/bbMargin/x", 3);
    bb_margin_y = ctx->setting<int>("router2/bbMargin/y", 3);
    ipin_cost_adder = ctx->setting<float>("router2/ipinCostAdder", 0.0f);
    bias_cost_factor = ctx->setting<float>("router2/biasCostFactor", 0.25f);
    init_curr_cong_weight = ctx->setting<float>("router2/initCurrCongWeight", 0.5f);
    hist_cong_weight = ctx->setting<float>("router2/histCongWeight", 1.0f);
    curr_cong_mult = ctx->setting<float>("router2/currCongWeightMult", 2.0f);
    estimate_weight = ctx->setting<float>("router2/estimateWeight", 0.8f);
    perf_profile = ctx->setting<float>("router2/perfProfile", false);
}

NEXTPNR_NAMESPACE_END
