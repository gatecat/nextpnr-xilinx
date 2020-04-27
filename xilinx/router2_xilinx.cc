/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020  David Shah <dave@ds0.me>
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
 */

#include "router2_xilinx.h"
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Router2 {
/*
 * This is a simple pass to split locally high-fanout nets
 * that force the use of dedicated "leaf" clock resources
 * (using the CE->leaf route-through) to reduce routing
 * congestion
 */
struct BufceLeafInserter
{
    Context *ctx;
    // int tile --> rclk tile and side
    std::unordered_map<int, std::pair<int, bool>> int_to_rclk;
    std::vector<std::pair<std::string, std::string>> tiletypes;
    void setup_int2rclk()
    {
        tiletypes = ctx->getTilesAndTypes();
        for (int y = 0; y < ctx->chip_info->height; y++) {
            for (int x = 0; x < ctx->chip_info->width; x++) {
                int t = y * ctx->chip_info->width + x;
                std::string tiletype = tiletypes.at(t).second;
                if (tiletype != "RCLK_INT_L" && tiletype != "RCLK_INT_R")
                    continue;
                for (int y2 = y - 30; y2 <= y + 30; y2++)
                    int_to_rclk[y2 * ctx->chip_info->width + x] = std::make_pair(t, y2 >= y);
            }
        }
    }
    int fanout_thresh = 20;

    struct IntBoolHash
    {
        std::size_t operator()(const std::pair<int, bool> &p) const noexcept
        {
            std::size_t seed = 0;
            boost::hash_combine(seed, std::hash<bool>()(p.first));
            boost::hash_combine(seed, std::hash<bool>()(p.second));
            return seed;
        }
    };
    std::unordered_map<std::pair<int, bool>, std::unordered_map<IdString, std::vector<size_t>>, IntBoolHash>
            rclk_to_sink;

    std::unordered_map<IdString, std::unordered_set<size_t>> promoted_sinks;
    std::unordered_map<IdString, std::vector<NetSegment>> rclk_segments;

    int total_promoted = 0;

    std::pair<int, bool> sink_to_rclk(NetInfo *net, const PortRef &sink)
    {
        WireId dst_wire = ctx->getNetinfoSinkWire(net, sink);
        if (dst_wire == WireId())
            return {-1, false};
        int sink_tile = -1;

        if (tiletypes.at(sink.cell->bel.tile).second.find("XIPHY") != std::string::npos) {
            return {-1, false};
        }

        if (ctx->sink_locs.count(dst_wire)) {
            auto loc = ctx->sink_locs.at(dst_wire);
            sink_tile = loc.y * ctx->chip_info->width + loc.x;
        } else {
            auto bel_data = ctx->locInfo(sink.cell->bel).bel_data[sink.cell->bel.index];
            if (bel_data.site == -1)
                return {-1, false};
            auto site_data = ctx->chip_info->tile_insts[sink.cell->bel.tile].site_insts[bel_data.site];
            if (site_data.inter_x == -1)
                return {-1, false};
            sink_tile = site_data.inter_y * ctx->chip_info->width + site_data.inter_x;
        }
        if (sink_tile == -1 || !int_to_rclk.count(sink_tile))
            return {-1, false};
        return int_to_rclk.at(sink_tile);
    }

    void determine_rclk_fanout()
    {
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            if (int(ni->users.size()) < fanout_thresh)
                continue;
            if (!ni->wires.empty())
                continue;
            if (ni->name == ctx->id("$PACKER_GND_NET") || ni->name == ctx->id("$PACKER_VCC_NET"))
                continue;
            for (size_t i = 0; i < ni->users.size(); i++) {
                const auto &user = ni->users.at(i);
                auto rclk = sink_to_rclk(ni, user);
                if (rclk.first == -1)
                    continue;
                rclk_to_sink[rclk][ni->name].push_back(i);
            }
        }
    }

    WireId get_rclk_wire(int tile, bool top_half, int i)
    {
        NPNR_ASSERT(i < 16);
        static const std::vector<int> top_rclk = {0, 1, 4, 5, 6, 7, 12, 13, 14, 15, 20, 21, 22, 23, 28, 29};
        static const std::vector<int> bot_rclk = {2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27, 30, 31};

        int leaf_clk = top_half ? top_rclk.at(i) : bot_rclk.at(i);
        IdString wire_name = ctx->id(
                stringf("%s/CLK_LEAF_SITES_%d_CLK_LEAF", ctx->chip_info->tile_insts[tile].name.get(), leaf_clk));
        return ctx->getWireByName(wire_name);
    }

    void promote_leaf_clocks()
    {
        for (auto &r : rclk_to_sink) {
            std::set<int> available_rclk;
            for (int i = 0; i < 16; i++)
                if (ctx->checkWireAvail(get_rclk_wire(r.first.first, r.first.second, i)))
                    available_rclk.insert(i);
            int to_promote_rclk = std::min<int>(8, available_rclk.size());
            int promoted = 0;
            while (promoted < to_promote_rclk) {
                auto max = std::max_element(
                        r.second.begin(), r.second.end(),
                        [](const decltype(r.second)::value_type &a, const decltype(r.second)::value_type &b) {
                            return a.second.size() < b.second.size();
                        });
                if (max == r.second.end() || int(max->second.size()) < fanout_thresh)
                    break;
                NPNR_ASSERT(!available_rclk.empty());
                int rclk_idx = *(available_rclk.begin());
                WireId rclk_wire = get_rclk_wire(r.first.first, r.first.second, rclk_idx);
                NetInfo *ni = ctx->nets.at(max->first).get();

                WireId src_wire = ctx->getNetinfoSourceWire(ni);
                NPNR_ASSERT(src_wire != WireId());
                rclk_segments[ni->name].emplace_back(ni, max->second.front(), src_wire, rclk_wire);

                for (auto usr_idx : max->second) {
                    WireId dst_wire = ctx->getNetinfoSinkWire(ni, ni->users.at(usr_idx));
                    NPNR_ASSERT(dst_wire != WireId());
                    rclk_segments[ni->name].emplace_back(ni, usr_idx, rclk_wire, dst_wire);
                    promoted_sinks[ni->name].insert(usr_idx);
                }

                // Ensure bounding box and estimates are accurate
                if (!ctx->sink_locs.count(rclk_wire)) {
                    for (auto uh : ctx->getPipsUphill(rclk_wire)) {
                        WireId src = ctx->getPipSrcWire(uh);
                        if (ctx->getWireName(src).str(ctx).find("CE_INT") != std::string::npos) {
                            ctx->sink_locs[rclk_wire] = ctx->getPipLocation(uh);
                            break;
                        }
                    }
                }

                r.second.erase(max->first);
                available_rclk.erase(rclk_idx);
                ++promoted;
                ++total_promoted;
            }
        }
    }

    BufceLeafInserter(Context *ctx) : ctx(ctx) {}

    void operator()()
    {
        setup_int2rclk();
        determine_rclk_fanout();
        promote_leaf_clocks();
        log_info("Used %d leaf clock wires for high-fanout nets\n", total_promoted);
    }
};

struct IntSplitter
{
    Context *ctx;
    BufceLeafInserter *leaf_splitter;

    std::unordered_map<IdString, std::unordered_set<size_t>> split_sinks;
    std::unordered_map<IdString, std::vector<NetSegment>> new_segments;
    bool reached_int(WireId wire, bool onto_int)
    {
        // Assume any wire with two or more pips on/off general routing is the boundary to interconnect
        // (I think this holds always...)
        int int_count = 0;
        IdString id_int = ctx->id("INT");
        auto process = [&](PipId pip) {
            IdString tt(ctx->locInfo(pip).type);
            int intent = ctx->wireIntent(onto_int ? ctx->getPipDstWire(pip) : ctx->getPipSrcWire(pip));
            if (tt == id_int && (intent == ID_NODE_HLONG || intent == ID_NODE_VLONG || intent == ID_NODE_DEDICATED ||
                                 intent == ID_NODE_DOUBLE || intent == ID_NODE_SINGLE || intent == ID_NODE_VQUAD ||
                                 intent == ID_NODE_HQUAD || intent == ID_NODE_PINBOUNCE || intent == ID_NODE_LOCAL))
                ++int_count;
        };
        if (onto_int) {
            for (auto pip : ctx->getPipsDownhill(wire)) {
                process(pip);
                if (int_count >= 2)
                    break;
            }
        } else {
            for (auto pip : ctx->getPipsUphill(wire)) {
                process(pip);
                if (int_count >= 2)
                    break;
            }
        }
        return int_count >= 2;
    }
    bool include_sink(const NetInfo *net, const PortRef &user)
    {
        WireId user_wire = ctx->getNetinfoSinkWire(net, user);
        if (net->wires.count(user_wire) || user_wire == WireId())
            return false;
        CellInfo *ci = user.cell;
        if (ci->type == id_SLICE_LUTX)
            return (user.port == id_DI1) || (user.port == id_DI2) || (user.port == id_CLK) || (user.port == id_WE);
        else if (ci->type == id_CARRY8)
            return false;
        return true;
    }
    bool include_source(const NetInfo *net)
    {
        if (net->driver.cell == nullptr)
            return false;
        WireId src_wire = ctx->getNetinfoSourceWire(net);
        if (net->wires.count(src_wire) || src_wire == WireId())
            return false;
        const CellInfo *ci = net->driver.cell;
        if (ci->type == id_SLICE_LUTX)
            return (net->driver.port == id_O5);
        return true;
    }

    std::unordered_map<WireId, std::pair<IdString, PipId>> wire2net;

    std::unordered_map<WireId, PipId> backtrace;
    std::queue<WireId> visit;
    void split_sink(NetInfo *net, WireId src_wire, size_t user_idx)
    {
        auto &usr = net->users.at(user_idx);
        if (!include_sink(net, usr))
            return;
        backtrace.clear();
        {
            std::queue<WireId> empty_queue;
            std::swap(visit, empty_queue);
        }
        int iter = 0;
        const int iter_limit = 10000;
        WireId dst_wire = ctx->getNetinfoSinkWire(net, usr);

        WireId int_wire = WireId();
        visit.push(dst_wire);

        while (!visit.empty() && iter < iter_limit) {
            WireId cursor = visit.front();
            visit.pop();
            ++iter;
            if (cursor == src_wire) {
                // Reached source without ever going through general routing. No need to create segments.
                break;
            }
            if (reached_int(cursor, false)) {
                // Reached general interconnect
                int_wire = cursor;
                break;
            }
            for (PipId uh : ctx->getPipsUphill(cursor)) {
                if (!ctx->checkPipAvail(uh))
                    continue; // pip unavailable according to arch
                WireId prev = ctx->getPipSrcWire(uh);
                if (!ctx->checkWireAvail(prev))
                    continue; // wire unavailable according to arch
                if (backtrace.count(prev))
                    continue; // wire already visited
                auto found_w2n = wire2net.find(prev);
                if (found_w2n != wire2net.end() &&
                    (found_w2n->second.first != net->name || found_w2n->second.second != uh))
                    continue; // wire already used to connect another net to interconnect
                backtrace[prev] = uh;
                visit.push(prev);
            }
        }

        if (int_wire != WireId()) {
            // We reached interconnect!
            split_sinks[net->name].insert(user_idx);
            new_segments[net->name].emplace_back(net, user_idx, src_wire, int_wire);
            new_segments[net->name].emplace_back(net, user_idx, int_wire, dst_wire);

            WireId cursor2 = int_wire;
            while (backtrace.count(cursor2)) {
                PipId uh = backtrace.at(cursor2);
                wire2net[cursor2] = std::make_pair(net->name, uh);
                cursor2 = ctx->getPipDstWire(uh);
            }
            NPNR_ASSERT(cursor2 == dst_wire);
            wire2net[cursor2] = std::make_pair(net->name, PipId());
        }
    }

    void process_net(NetInfo *net)
    {
        if (net->name == ctx->id("$PACKER_GND_NET") || net->name == ctx->id("$PACKER_VCC_NET"))
            return;
        if (net->driver.cell == nullptr)
            return;
        // FIXME: combination of leaf promotion and INT splitting
        if (leaf_splitter->promoted_sinks.count(net->name))
            return;
        WireId src_wire = ctx->getNetinfoSourceWire(net);
        for (size_t i = 0; i < net->users.size(); i++)
            split_sink(net, src_wire, i);
    }

    IntSplitter(Context *ctx, BufceLeafInserter *leaf_splitter) : ctx(ctx), leaf_splitter(leaf_splitter) {}

    void operator()()
    {
        for (auto net : sorted(ctx->nets))
            process_net(net.second);
    }
};

ArcRouteResult Router2Xilinx::route_segment(Router2Thread &th, NetInfo *net, size_t seg_idx, bool is_mt, bool is_bb)
{
    if ((net->name == ctx->id("$PACKER_GND_NET") || net->name == ctx->id("$PACKER_VCC_NET"))) {
        if (is_mt)
            return ARC_RETRY_WITHOUT_BB;
        route_xilinx_const(th, net, seg_idx, is_mt, is_bb);
        return ARC_SUCCESS;
    }
    return ARC_USE_DEFAULT;
}

std::vector<NetSegment> Router2Xilinx::segment_net(NetInfo *net)
{
    if (leaf_inserter == nullptr) {
        leaf_inserter = new BufceLeafInserter(ctx);
        (*leaf_inserter)();
    }
    if (int_splitter == nullptr) {
        int_splitter = new IntSplitter(ctx, leaf_inserter);
        (*int_splitter)();
    }
    if (leaf_inserter->promoted_sinks.count(net->name)) {
        const auto &rclk_sinks = leaf_inserter->promoted_sinks.at(net->name);
        const auto &rclk_segs = leaf_inserter->rclk_segments.at(net->name);
        // Add segments from leaf clock promotion
        std::vector<NetSegment> segments(rclk_segs.begin(), rclk_segs.end());
        // Create segments for arcs not promoted to use leaf clock resources
        for (size_t i = 0; i < net->users.size(); i++)
            if (!rclk_sinks.count(i))
                segments.emplace_back(ctx, net, i);
        return segments;
    } else if (int_splitter->split_sinks.count(net->name)) {
        const auto &int_sinks = int_splitter->split_sinks.at(net->name);
        const auto &int_segs = int_splitter->new_segments.at(net->name);
        // Add segments from leaf clock promotion
        std::vector<NetSegment> segments(int_segs.begin(), int_segs.end());
        // Create segments for arcs not promoted to use leaf clock resources
        for (size_t i = 0; i < net->users.size(); i++)
            if (!int_sinks.count(i))
                segments.emplace_back(ctx, net, i);
        return segments;
    } else {
        return Router2ArchFunctions::segment_net(net);
    }
}

void Router2Xilinx::route_xilinx_const(Router2Thread &t, NetInfo *net, size_t seg_idx, bool is_mt, bool is_bb)
{
    auto &nd = r->nets[net->udata];
    auto &sd = nd.segments[seg_idx];

    if (!t.queue.empty()) {
        std::priority_queue<Router2State::QueuedWire, std::vector<Router2State::QueuedWire>,
                            Router2State::QueuedWire::Greater>
                new_queue;
        t.queue.swap(new_queue);
    }
    if (!t.backwards_queue.empty()) {
        std::queue<int> new_queue;
        t.backwards_queue.swap(new_queue);
    }
    r->reset_wires(t);

    int backwards_iter = 0;
    int backwards_limit = ctx->xc7 ? 100000 : 5000000;

    bool const_val = false;
    if (net->name == ctx->id("$PACKER_VCC_NET"))
        const_val = true;
    else
        NPNR_ASSERT(net->name == ctx->id("$PACKER_GND_NET"));

    int src_wire_idx = r->wire_to_idx.at(sd.s.src_wire);

    for (int allowed_cong = 0; allowed_cong < 10; allowed_cong++) {
        backwards_iter = 0;
        if (!t.backwards_queue.empty()) {
            std::queue<int> new_queue;
            t.backwards_queue.swap(new_queue);
        }
        t.backwards_queue.push(r->wire_to_idx.at(sd.s.dst_wire));
        r->reset_wires(t);
        while (!t.backwards_queue.empty() && backwards_iter < backwards_limit) {
            int cursor = t.backwards_queue.front();
            t.backwards_queue.pop();
            auto &cwd = r->flat_wires[cursor];
            PipId cpip;
            if (cwd.bound_nets.count(net->udata)) {
                // If we can tack onto existing routing; try that
                // Only do this if the existing routing is uncontented; however
                int cursor2 = cursor;
                bool bwd_merge_fail = false;
                while (r->flat_wires.at(cursor2).bound_nets.count(net->udata)) {
                    if (int(r->flat_wires.at(cursor2).bound_nets.size()) > (allowed_cong + 1)) {
                        bwd_merge_fail = true;
                        break;
                    }
                    PipId p = r->flat_wires.at(cursor2).bound_nets.at(net->udata).pip;
                    if (p == PipId())
                        break;
                    cursor2 = r->wire_to_idx.at(ctx->getPipSrcWire(p));
                }
                if (!bwd_merge_fail && cursor2 == src_wire_idx) {
                    // Found a path to merge to existing routing; backwards
                    cursor2 = cursor;
                    while (r->flat_wires.at(cursor2).bound_nets.count(net->udata)) {
                        PipId p = r->flat_wires.at(cursor2).bound_nets.at(net->udata).pip;
                        if (p == PipId())
                            break;
                        cursor2 = r->wire_to_idx.at(ctx->getPipSrcWire(p));
                        r->set_visited(t, cursor2, p, Router2State::WireScore());
                    }
                    break;
                }
                cpip = cwd.bound_nets.at(net->udata).pip;
            }
#if 0
                log("   explore %s\n", ctx->nameOfWire(cwd.w));
#endif
            if (ctx->wireIntent(cwd.w) == (const_val ? ID_PSEUDO_VCC : ID_PSEUDO_GND)) {
#if 0
                    log("    Hit global network at %s\n", ctx->nameOfWire(cwd.w));
#endif
                // We've hit the constant pseudo-network, continue from here
                int cursor2 = cursor;
                while (cursor2 != src_wire_idx) {
                    auto &c2wd = r->flat_wires.at(cursor2);
                    bool found = false;
                    for (auto p : ctx->getPipsUphill(c2wd.w)) {
                        if (!ctx->checkPipAvail(p) && ctx->getBoundPipNet(p) != net)
                            continue;
                        WireId src = ctx->getPipSrcWire(p);
                        if (ctx->wireIntent(src) != (const_val ? ID_PSEUDO_VCC : ID_PSEUDO_GND))
                            continue;
                        if (r->is_wire_undriveable(src))
                            continue;
                        cursor2 = r->wire_to_idx.at(src);
                        r->set_visited(t, cursor2, p, Router2State::WireScore());
                        found = true;
                        break;
                    }
                    if (!found)
                        log_error("Invalid global constant node '%s'\n", ctx->nameOfWire(c2wd.w));
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
            for (auto uh : ctx->getPipsUphill(r->flat_wires[cursor].w)) {
                did_something = true;
                if (!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net)
                    continue;
                if (cpip != PipId() && cpip != uh)
                    continue; // don't allow multiple pips driving a wire with a net
                int next = r->wire_to_idx.at(ctx->getPipSrcWire(uh));
                if (r->was_visited(next))
                    continue; // skip wires that have already been visited
                auto &wd = r->flat_wires[next];
                if (wd.unavailable)
                    continue;
                if (wd.reserved_net != -1 && wd.reserved_net != net->udata)
                    continue;
                if (int(wd.bound_nets.size()) > (allowed_cong + 1) ||
                    (allowed_cong == 0 && wd.bound_nets.size() == 1 && !wd.bound_nets.count(net->udata)))
                    continue; // never allow congestion in backwards routing
                t.backwards_queue.push(next);
                r->set_visited(t, next, uh, Router2State::WireScore());
            }
            if (did_something)
                ++backwards_iter;
        }
        int dst_wire_idx = r->wire_to_idx.at(sd.s.dst_wire);
        if (r->was_visited(src_wire_idx)) {
            if (ctx->debug)
                log("   Routed (backwards): ");
            int cursor_fwd = src_wire_idx;
            r->bind_pip_internal(net, src_wire_idx, PipId());
            while (r->was_visited(cursor_fwd)) {
                auto &v = r->flat_wires.at(cursor_fwd).visit;
                cursor_fwd = r->wire_to_idx.at(ctx->getPipDstWire(v.pip));
                r->bind_pip_internal(net, cursor_fwd, v.pip);
                if (ctx->debug) {
                    auto &wd = r->flat_wires.at(cursor_fwd);
                    if (ctx->debug)
                        log("      wire: %s (curr %d hist %f)\n", ctx->nameOfWire(wd.w), int(wd.bound_nets.size()) - 1,
                            wd.hist_cong_cost);
                }
            }
            NPNR_ASSERT(cursor_fwd == dst_wire_idx);
            sd.routed = true;
            t.processed_sinks.insert(sd.s.dst_wire);
            r->reset_wires(t);
            return;
        }
    }
    log_error("Unrouteable %s sink %s.%s (%s)\n", ctx->nameOf(net), ctx->nameOf(net->users.at(sd.s.orig_user).cell),
              ctx->nameOf(net->users.at(sd.s.orig_user).port), ctx->nameOfWire(sd.s.dst_wire));
}
} // namespace Router2
NEXTPNR_NAMESPACE_END
