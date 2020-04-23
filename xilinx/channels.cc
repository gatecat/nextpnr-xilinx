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

#include <boost/optional.hpp>
#include <queue>
#include "channel_router.h"
#include "int_graph.h"
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace ChannelRouter {

struct UltrascaleChannelGraph : ChannelGraph
{
    Context *ctx;
    UltrascaleChannelGraph(Context *ctx) : ctx(ctx)
    {
        setup();
        setup_arcs();
    };

    // Mapping from real to "INT grid" coordinates
    std::vector<int> row_r2i, col_r2i;
    std::set<int> int_rows, int_cols;
    int int_width, int_height;
    // Mapping from INT wire name to INT wire index
    std::unordered_map<IdString, int> name2idx;

    std::unordered_set<int> driving_nodes, driven_nodes;

    void setup()
    {
        row_r2i.resize(ctx->chip_info->height, -1);
        col_r2i.resize(ctx->chip_info->width, -1);
        for (int y = 0; y < ctx->chip_info->height; y++) {
            for (int x = 0; x < ctx->chip_info->width; x++) {
                IdString tt = IdString(
                        ctx->chip_info->tile_types[ctx->chip_info->tile_insts[y * ctx->chip_info->width + x].type]
                                .type);
                if (tt != ctx->id("INT"))
                    continue;
                int_rows.insert(y);
                int_cols.insert(x);
            }
        }
        int ir = 0, ic = 0;
        for (auto r : int_rows)
            row_r2i.at(r) = ir++;
        for (auto c : int_cols)
            col_r2i.at(c) = ic++;
        int_width = ic;
        int_height = ir;
        for (int i = 0; i < IntGraph::num_wires; i++) {
            name2idx[ctx->id(IntGraph::wire_names[i])] = i;
        }
        for (int i = 0; i < IntGraph::num_pips; i++) {
            auto &p = IntGraph::int_pips[i];
            driving_nodes.insert(p.from.index);
            driven_nodes.insert(p.to.index);
        }
    }

    int get_width() const override { return int_width; }
    int get_height() const override { return int_height; }
    std::vector<Channel> get_channels() const override
    {
        std::vector<Channel> channels;
        channels.resize(IntGraph::num_wires);
        for (int i = 0; i < IntGraph::num_wires; i++) {
            auto &c = channels.at(i);
            c.width = 1; // Current all-width-1 model
            c.cost = 10;
            c.dir = DIR_ANY; // Maybe should infer direction?
            c.type_name = IntGraph::wire_names[i];
        }
        for (int i = 0; i < IntGraph::num_pips; i++) {
            auto &p = IntGraph::int_pips[i];
            auto &sc = channels.at(p.from.index);
            sc.downhill.emplace_back(p.from.dx, p.from.dy, p.to.dx, p.to.dy, p.to.index);
        }
        return channels;
    }

    struct ArcKey
    {
        ArcKey(const NetInfo *net, size_t user) : net(net), user(user){};
        const NetInfo *net;
        size_t user;
        struct Hash
        {
            size_t operator()(const ArcKey &k) const noexcept
            {
                std::size_t seed = 0;
                boost::hash_combine(seed, std::hash<int>()(k.net->name.index));
                boost::hash_combine(seed, std::hash<size_t>()(k.user));
                return seed;
            }
        };
        bool operator==(const ArcKey &other) const { return other.net == net && other.user == user; }
    };

    boost::optional<ChannelNode> wire_to_int_node(WireId wire, bool onto_int)
    {
        IdString id_int = ctx->id("INT");
        auto twr = ctx->getTileWireRange(wire);
        for (auto denorm : twr) {
            IdString tt = ctx->locInfo(denorm).type;
            if (tt != id_int)
                continue;
            if (!(onto_int ? driving_nodes.count(denorm.index) : driven_nodes.count(denorm.index)))
                continue;
            int ix = col_r2i.at(denorm.tile % ctx->chip_info->width);
            NPNR_ASSERT(ix != -1);
            int iy = row_r2i.at(denorm.tile / ctx->chip_info->width);
            NPNR_ASSERT(iy != -1);
            return ChannelNode(ix, iy, denorm.index);
        }
        return {};
    }

    std::unordered_set<IdString> ignore_nets;
    std::unordered_set<ArcKey, ArcKey::Hash> ignore_arcs;
    std::unordered_map<ArcKey, ChannelNode, ArcKey::Hash> arc2node;
    std::unordered_map<IdString, ChannelNode> src2node;

    std::unordered_map<WireId, IdString> used_wires;

    // Route to/from pins on/off the general interconnect graph
    void setup_arcs()
    {
        std::unordered_map<WireId, PipId> backtrace;
        std::queue<WireId> visit;
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            auto &drv = ni->driver;
            if (drv.cell == nullptr || ctx->getBelGlobalBuf(drv.cell->bel)) {
                // Skip undriven or global nets
                ignore_nets.insert(ni->name);
                continue;
            }
            WireId src_wire = ctx->getNetinfoSourceWire(ni);
            if (src_wire == WireId()) {
                ignore_nets.insert(ni->name);
                continue;
            }
            bool at_least_one_int = false;
            for (size_t i = 0; i < ni->users.size(); i++) {
                auto &usr = ni->users.at(i);
                if (ctx->getBelGlobalBuf(usr.cell->bel)) {
                    // Skip global buffers, clock routing is a special case
                    ignore_arcs.emplace(ni, i);
                    continue;
                }
                WireId dst_wire = ctx->getNetinfoSinkWire(ni, usr);
                if (dst_wire == WireId() || dst_wire == src_wire || ni->wires.count(dst_wire)) {
                    // Skip sinks that don't exist, need no routing, or are already routed
                    ignore_arcs.emplace(ni, i);
                    continue;
                }
                backtrace.clear();
                {
                    std::queue<WireId> empty;
                    std::swap(visit, empty);
                }
                // Simple routing from sink pin until a point off the general interconnect graph
                bool reached_src = false, reached_int = false;
                visit.push(dst_wire);
                while (!visit.empty()) {
                    WireId cursor = visit.front();
                    visit.pop();
                    if (cursor == src_wire) {
                        // We've actually reached the source without ever touching general interconnect
                        // e.g. general
                        ignore_arcs.emplace(ni, i);
                        reached_src = true;
                        break;
                    }
                    auto int_node = wire_to_int_node(cursor, false);
                    if (int_node) {
                        // Reached the general interconnect graph
                        arc2node.emplace(ArcKey(ni, i), *int_node);
                        reached_int = true;
                        break;
                    }
                    for (PipId pip : ctx->getPipsUphill(cursor)) {
                        if (!ctx->checkPipAvail(pip))
                            continue;
                        WireId src = ctx->getPipSrcWire(pip);
                        if (backtrace.count(src))
                            continue;
                        if (!ctx->checkWireAvail(src))
                            continue;
                        auto fnd = used_wires.find(src);
                        if (fnd != used_wires.end() && fnd->second != ni->name)
                            continue;
                        backtrace[src] = pip;
                        visit.push(src);
                    }
                }
                if (!reached_src && !reached_int) {
                    log_info("Mysteriously tricky arc %s: %s->%s\n", ctx->nameOf(ni), ctx->nameOfWire(src_wire),
                             ctx->nameOfWire(dst_wire));
                    ignore_arcs.emplace(ni, i);
                    continue;
                }
                {
                    // Mark all the wires we used in the process, so we can't use them for other nets
                    WireId cursor = dst_wire;
                    used_wires[cursor] = ni->name;
                    while (backtrace.count(cursor)) {
                        cursor = ctx->getPipSrcWire(backtrace.at(cursor));
                        used_wires[cursor] = ni->name;
                    }
                }
                at_least_one_int |= reached_int;
            }
            // If at least one sink needs general interconnect, we need to route into the general interconnect graph
            // from the source wire
            if (at_least_one_int) {
                backtrace.clear();
                {
                    std::queue<WireId> empty;
                    std::swap(visit, empty);
                }
                bool reached_int = false;
                visit.push(src_wire);
                while (!visit.empty()) {
                    WireId cursor = visit.front();
                    visit.pop();
                    auto int_node = wire_to_int_node(cursor, true);
                    if (int_node) {
                        // Reached the general interconnect graph
                        src2node.emplace(ni->name, *int_node);
                        reached_int = true;
                        break;
                    }
                    for (PipId pip : ctx->getPipsDownhill(cursor)) {
                        if (!ctx->checkPipAvail(pip))
                            continue;
                        WireId dst = ctx->getPipDstWire(pip);
                        if (backtrace.count(dst))
                            continue;
                        if (!ctx->checkWireAvail(dst))
                            continue;
                        auto fnd = used_wires.find(dst);
                        if (fnd != used_wires.end() && fnd->second != ni->name)
                            continue;
                        backtrace[dst] = pip;
                        visit.push(dst);
                    }
                }
                if (!reached_int) {
                    log_info("Mysteriously tricky source %s: %s\n", ctx->nameOf(ni), ctx->nameOfWire(src_wire));
                    ignore_nets.insert(ni->name);
                    continue;
                }
                {
                    WireId cursor = src_wire;
                    used_wires[cursor] = ni->name;
                    while (backtrace.count(cursor)) {
                        cursor = ctx->getPipDstWire(backtrace.at(cursor));
                        used_wires[cursor] = ni->name;
                    }
                }
            } else {
                ignore_nets.insert(ni->name);
            }
        }
    }

    ChannelNode get_source_node(const NetInfo *net) const override
    {
        if (ignore_nets.count(net->name))
            return ChannelNode();
        return src2node.at(net->name);
    }

    ChannelNode get_sink_node(const NetInfo *net, size_t usr_i) const override
    {
        ArcKey k(net, usr_i);
        if (ignore_nets.count(net->name) || ignore_arcs.count(k))
            return ChannelNode();
        return arc2node.at(k);
    }

    bool is_global_net(const NetInfo *net) const override { return ignore_nets.count(net->name); }
};

} // namespace ChannelRouter

void usp_channel_router_test(Context *ctx)
{
    ChannelRouter::UltrascaleChannelGraph g(ctx);
    ChannelRouter::run_channelrouter(ctx, &g, ChannelRouter::ChannelRouterCfg());
    NPNR_ASSERT_FALSE("DONE");
}

NEXTPNR_NAMESPACE_END
