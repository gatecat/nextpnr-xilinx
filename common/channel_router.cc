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

#include "channel_router.h"
#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <chrono>
#include <deque>
#include <fstream>
#include <queue>
#include <thread>
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace ChannelRouter {

inline bool operator==(const ChannelNode &a, const ChannelNode &b)
{
    return a.x == b.x && a.y == b.y && a.type == b.type;
}

inline bool operator!=(const ChannelNode &a, const ChannelNode &b)
{
    return a.x != b.x || a.y != b.y || a.type != b.type;
}

struct ChannelNodeHash
{
    size_t operator()(const ChannelNode &n) const noexcept
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, std::hash<int>()(n.x));
        boost::hash_combine(seed, std::hash<int>()(n.y));
        boost::hash_combine(seed, std::hash<int>()(n.type));
        return seed;
    }
};

struct ChannelRouterState
{

    struct NodeScore
    {
        float cost;
        float togo_cost;
        float total() const { return cost + togo_cost; }
    };

    struct PerNodeData
    {
        std::vector<ChannelNode> downhill;
        std::vector<ChannelNode> uphill;

        // net --> number of arcs; driving node
        boost::container::flat_map<int, std::pair<int, ChannelNode>> bound_nets;
        // Historical congestion cost
        float hist_cong_cost = 1.0;
        // Wire is unavailable as locked to another arc
        bool unavailable = false;
        // This wire has to be used for this net
        int reserved_net = -1;
        // Visit data
        struct
        {
            bool dirty = false, visited = false;
            ChannelNode prev;
            NodeScore score;
        } visit;
    };

    struct PerArcData
    {
        ChannelNode sink_node;
        ArcBounds bb;
        bool routed = false;
    };

    // As we allow overlap at first; the nextpnr bind functions can't be used
    // as the primary relation between arcs and wires/pips
    struct PerNetData
    {
        ChannelNode src_node;
        std::vector<PerArcData> arcs;
        ArcBounds bb;
        // Coordinates of the center of the net, used for the weight-to-average
        int cx, cy, hpwl;
        int total_route_us = 0;
    };

    Context *ctx;
    ChannelRouterCfg cfg;
    ChannelGraph *g;
    int width, height;

    // xy -> index
    std::vector<std::vector<PerNodeData>> nodes;

    void setup_nodes()
    {
        width = g->get_width();
        height = g->get_height();
        nodes.resize(width * height);
        channel_types = g->get_channels();
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                auto &l = nodes.at(y * width + x);
                l.resize(channel_types.size());
            }
        }
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                for (int t = 0; t < int(channel_types.size()); t++) {
                    auto &c = channel_types.at(t);
                    for (const auto &dh : c.downhill) {
                        int start_x = x, start_y = y;
                        NPNR_ASSERT(dh.src_along <= c.length);
                        switch (c.dir) {
                        case DIR_EAST:
                            start_x -= dh.src_along;
                            break;
                        case DIR_WEST:
                            start_x += dh.src_along;
                            break;
                        case DIR_NORTH:
                            start_y -= dh.src_along;
                            break;
                        case DIR_SOUTH:
                            start_y += dh.src_along;
                            break;
                        }
                        int end_x = x, end_y = y;
                        auto &d = channel_types.at(dh.dst_type);
                        NPNR_ASSERT(dh.dst_along <= d.length);
                        switch (d.dir) {
                        case DIR_EAST:
                            end_x -= dh.dst_along;
                            break;
                        case DIR_WEST:
                            end_x += dh.dst_along;
                            break;
                        case DIR_NORTH:
                            end_y -= dh.dst_along;
                            break;
                        case DIR_SOUTH:
                            end_y += dh.dst_along;
                            break;
                        }
                        auto &src = nodes.at(start_y * width + start_x).at(t);
                        auto &dst = nodes.at(end_y * width + end_x).at(dh.dst_type);
                        src.downhill.emplace_back(end_x, end_y, dh.dst_type);
                        dst.uphill.emplace_back(start_x, start_y, t);
                    }
                }
            }
        }
    }

    std::vector<Channel> channel_types;

    // Use 'udata' for fast net lookups and indexing
    std::vector<NetInfo *> nets_by_udata;
    std::vector<PerNetData> nets;

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

            if (ni->driver.cell == nullptr) {
                nets.at(i).hpwl = 0;
                continue;
            }

            ChannelNode src_node = g->get_source_node(ni);
            nets.at(i).src_node = src_node;
            nets.at(i).cx += src_node.x;
            nets.at(i).cy += src_node.y;
            nets.at(i).bb.x0 = src_node.x;
            nets.at(i).bb.x1 = src_node.x;
            nets.at(i).bb.y0 = src_node.y;
            nets.at(i).bb.y1 = src_node.y;

            for (size_t j = 0; j < ni->users.size(); j++) {
                auto &usr = ni->users.at(j);

                ChannelNode sink_node = g->get_sink_node(ni, usr);
                nets.at(i).arcs.at(j).sink_node = sink_node;
                // Set bounding box for this arc
                nets.at(i).arcs.at(j).bb.x0 = std::min(src_node.x, sink_node.x);
                nets.at(i).arcs.at(j).bb.x1 = std::max(src_node.x, sink_node.x);
                nets.at(i).arcs.at(j).bb.y0 = std::min(src_node.y, sink_node.y);
                nets.at(i).arcs.at(j).bb.y1 = std::max(src_node.y, sink_node.y);
                // Expand net bounding box to include this arc
                nets.at(i).bb.x0 = std::min(nets.at(i).bb.x0, sink_node.x);
                nets.at(i).bb.x1 = std::max(nets.at(i).bb.x1, sink_node.x);
                nets.at(i).bb.y0 = std::min(nets.at(i).bb.y0, sink_node.y);
                nets.at(i).bb.y1 = std::max(nets.at(i).bb.y1, sink_node.y);
                // Add location to centroid sum
                nets.at(i).cx += sink_node.x;
                nets.at(i).cy += sink_node.y;
            }
            nets.at(i).hpwl = std::max(
                    std::abs(nets.at(i).bb.y1 - nets.at(i).bb.y0) + std::abs(nets.at(i).bb.x1 - nets.at(i).bb.x0), 1);
            nets.at(i).cx /= int(ni->users.size() + 1);
            nets.at(i).cy /= int(ni->users.size() + 1);
            if (ctx->debug)
                log_info("%s: bb=(%d, %d)->(%d, %d) c=(%d, %d) hpwl=%d\n", ctx->nameOf(ni), nets.at(i).bb.x0,
                         nets.at(i).bb.y0, nets.at(i).bb.x1, nets.at(i).bb.y1, nets.at(i).cx, nets.at(i).cy,
                         nets.at(i).hpwl);
            i++;
        }
    }

    struct QueuedNode
    {

        explicit QueuedNode(ChannelNode node, ChannelNode prev = ChannelNode(), NodeScore score = NodeScore{},
                            int randtag = 0)
                : node(node), prev(prev), score(score), randtag(randtag){};

        ChannelNode node;
        ChannelNode prev;
        NodeScore score;
        int randtag = 0;

        struct Greater
        {
            bool operator()(const QueuedNode &lhs, const QueuedNode &rhs) const noexcept
            {
                float lhs_score = lhs.score.cost + lhs.score.togo_cost,
                      rhs_score = rhs.score.cost + rhs.score.togo_cost;
                return lhs_score == rhs_score ? lhs.randtag > rhs.randtag : lhs_score > rhs_score;
            }
        };
    };

    double curr_cong_weight, hist_cong_weight, estimate_weight;

    PerNodeData &node_data(const ChannelNode &node)
    {
        NPNR_ASSERT(node.x >= 0 && node.x < width);
        NPNR_ASSERT(node.y >= 0 && node.y < height);
        return nodes.at(node.y * width + node.x).at(node.type);
    }

    float present_node_cost(const PerNodeData &w, int channel_type, int net_uid)
    {
        int over_capacity = int(w.bound_nets.size());
        over_capacity -= (channel_types.at(channel_type).width - 1);
        if (w.bound_nets.count(net_uid))
            over_capacity -= 1;
        if (over_capacity <= 0)
            return 1.0f;
        else
            return 1 + over_capacity * curr_cong_weight;
    }

    bool hit_test_node(ArcBounds &bb, ChannelNode n)
    {
        return n.x >= (bb.x0 - cfg.bb_margin_x) && n.x <= (bb.x1 + cfg.bb_margin_x) &&
               n.y >= (bb.y0 - cfg.bb_margin_y) && n.y <= (bb.y1 + cfg.bb_margin_y);
    }

    void bind_node_internal(NetInfo *net, ChannelNode node, ChannelNode uphill)
    {
        auto &b = node_data(node).bound_nets[net->udata];
        ++b.first;
        if (b.first == 1) {
            b.second = uphill;
        } else {
            NPNR_ASSERT(b.second == uphill);
        }
    }

    void unbind_node_internal(NetInfo *net, ChannelNode node)
    {
        auto &b = node_data(node).bound_nets.at(net->udata);
        --b.first;
        if (b.first == 0) {
            node_data(node).bound_nets.erase(net->udata);
        }
    }

    void ripup_arc(NetInfo *net, size_t user)
    {
        auto &ad = nets.at(net->udata).arcs.at(user);
        if (!ad.routed)
            return;
        ChannelNode src = nets.at(net->udata).src_node;
        ChannelNode cursor = ad.sink_node;
        while (cursor != src) {
            auto &wd = node_data(cursor);
            ChannelNode uphill = wd.bound_nets.at(net->udata).second;
            unbind_node_internal(net, cursor);
            cursor = uphill;
        }
        ad.routed = false;
    }

    float score_node_for_arc(NetInfo *net, size_t user, ChannelNode node)
    {
        auto &wd = node_data(node);
        auto &nd = nets.at(net->udata);
        float base_cost = channel_types.at(node.type).cost;
        float present_cost = present_node_cost(wd, node.type, net->udata);
        float hist_cost = wd.hist_cong_cost;
        float bias_cost = 0;
        int source_uses = 0;
        if (wd.bound_nets.count(net->udata))
            source_uses = wd.bound_nets.at(net->udata).first;
        bias_cost = cfg.bias_cost_factor * (base_cost / int(net->users.size())) *
                    ((std::abs(node.x - nd.cx) + std::abs(node.y - nd.cy)) / float(nd.hpwl));

        return base_cost * hist_cost * present_cost / (1 + source_uses) + bias_cost;
    }

    float get_togo_cost(NetInfo *net, size_t user, ChannelNode curr, ChannelNode sink)
    {
        auto &wd = node_data(curr);
        int source_uses = 0;
        if (wd.bound_nets.count(net->udata))
            source_uses = wd.bound_nets.at(net->udata).first;
        int base_cost =
                cfg.togo_cost_dx * abs(curr.x - sink.x) + cfg.togo_cost_dy * abs(curr.y - sink.y) + cfg.togo_cost_adder;
        return base_cost / (1 + source_uses);
    }

    bool check_arc_routing(NetInfo *net, size_t usr)
    {
        auto &ad = nets.at(net->udata).arcs.at(usr);
        ChannelNode src_node = nets.at(net->udata).src_node;
        ChannelNode cursor = ad.sink_node;
        while (node_data(cursor).bound_nets.count(net->udata)) {
            auto &wd = node_data(cursor);
            int bound = int(wd.bound_nets.size());
            if (bound == 0 || bound > channel_types.at(cursor.type).width)
                return false;
            auto &uh = wd.bound_nets.at(net->udata).second;
            if (uh == ChannelNode())
                break;
            cursor = uh;
        }
        return (cursor == src_node);
    }

    struct ThreadContext
    {
        // Nets to route
        std::vector<NetInfo *> route_nets;
        // Nets that failed routing
        std::vector<NetInfo *> failed_nets;

        std::vector<int> route_arcs;

        std::priority_queue<QueuedNode, std::vector<QueuedNode>, QueuedNode::Greater> queue;
        // Special case where one net has multiple logical arcs to the same physical sink
        std::unordered_set<ChannelNode, ChannelNodeHash> processed_sinks;

        std::vector<ChannelNode> dirty_nodes;
    };

    void reset_nodes(ThreadContext &t)
    {
        for (auto n : t.dirty_nodes) {
            auto &node_d = node_data(n);
            node_d.visit.visited = false;
            node_d.visit.dirty = false;
            node_d.visit.prev = ChannelNode();
            node_d.visit.score = NodeScore();
        }
        t.dirty_nodes.clear();
    }

    void set_visited(ThreadContext &t, ChannelNode curr, ChannelNode prev, NodeScore score)
    {
        auto &v = node_data(curr).visit;
        if (!v.dirty)
            t.dirty_nodes.push_back(curr);
        v.dirty = true;
        v.visited = true;
        v.prev = prev;
        v.score = score;
    }
    bool was_visited(ChannelNode node) { return node_data(node).visit.visited; }

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

    ArcRouteResult route_arc(ThreadContext &t, NetInfo *net, size_t i, bool is_mt, bool is_bb = true)
    {
        auto &nd = nets[net->udata];
        auto &ad = nd.arcs[i];
        ROUTE_LOG_DBG("Routing arc %d of net '%s' (%d, %d) -> (%d, %d)\n", int(i), ctx->nameOf(net), ad.bb.x0, ad.bb.y0,
                      ad.bb.x1, ad.bb.y1);
        auto src_node = nd.src_node;
        auto dst_node = ad.sink_node;
        // Check if arc was already done _in this iteration_
        if (t.processed_sinks.count(dst_node))
            return ARC_SUCCESS;

        if (!t.queue.empty()) {
            std::priority_queue<QueuedNode, std::vector<QueuedNode>, QueuedNode::Greater> new_queue;
            t.queue.swap(new_queue);
        }
        // Forwards A* routing
        reset_nodes(t);
        NodeScore base_score;
        base_score.cost = channel_types.at(src_node.type).cost;
        base_score.togo_cost = get_togo_cost(net, i, src_node, dst_node);

        // Add source wire to queue
        t.queue.push(QueuedNode(src_node, ChannelNode(), base_score));
        set_visited(t, src_node, ChannelNode(), base_score);

        int toexplore = 10000 * std::max(1, (ad.bb.x1 - ad.bb.x0) + (ad.bb.y1 - ad.bb.y0));
        int iter = 0;
        int explored = 1;
        while (!t.queue.empty() && (!is_bb || iter < toexplore)) {
            auto curr = t.queue.top();
            auto &d = node_data(curr.node);
            t.queue.pop();
            ++iter;
            // Explore all nodes downhill of cursor
            for (auto dh : d.downhill) {
                // Skip nodes outside of box in bounding-box mode
                if (is_bb && !hit_test_node(ad.bb, dh))
                    continue;
                // Evaluate score of next wire
                if (was_visited(dh))
                    continue;
                auto &nwd = node_data(dh);
                if (nwd.bound_nets.count(net->udata) && nwd.bound_nets.at(net->udata).second != curr.node)
                    continue;
                NodeScore next_score;
                next_score.cost = curr.score.cost + score_node_for_arc(net, i, dh);
                next_score.togo_cost = cfg.estimate_weight * get_togo_cost(net, i, dh, dst_node);
                const auto &v = nwd.visit;
                if (!v.visited || (v.score.total() > next_score.total())) {
                    ++explored;
                    // Add wire to queue if it meets criteria
                    t.queue.push(QueuedNode(dh, curr.node, next_score, ctx->rng()));
                    set_visited(t, dh, curr.node, next_score);
                    if (dh == dst_node) {
                        toexplore = std::min(toexplore, iter + 5);
                    }
                }
            }
        }
        if (was_visited(dst_node)) {
            ROUTE_LOG_DBG("   Routed (explored %d nodes): ", explored);
            auto cursor_bwd = dst_node;
            while (was_visited(cursor_bwd)) {
                auto &v = node_data(cursor_bwd).visit;
                bind_node_internal(net, cursor_bwd, v.prev);
                if (ctx->debug) {
                    auto &wd = node_data(cursor_bwd);
                    auto &ct = channel_types.at(cursor_bwd.type);
                    ROUTE_LOG_DBG("      node: X%d/Y%d/%s (curr %d/%d hist %f share %d)\n", cursor_bwd.x, cursor_bwd.y,
                                  ct.type_name.c_str(), int(wd.bound_nets.size()), ct.width, wd.hist_cong_cost,
                                  wd.bound_nets.count(net->udata) ? wd.bound_nets.at(net->udata).first : 0);
                }
                if (v.prev == ChannelNode()) {
                    NPNR_ASSERT(cursor_bwd == src_node);
                    break;
                }
                cursor_bwd = v.prev;
            }
            t.processed_sinks.insert(dst_node);
            ad.routed = true;
            reset_nodes(t);
            return ARC_SUCCESS;
        } else {
            reset_nodes(t);
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
        for (size_t i = 0; i < net->users.size(); i++) {
            // Ripup failed arcs to start with
            // Check if arc is already legally routed
            if (check_arc_routing(net, i))
                continue;
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
                    if (res2 != ARC_SUCCESS) {
                        auto src = g->get_source_node(net);
                        auto sink = g->get_sink_node(net, net->users.at(i));
                        log_error("Failed to route arc %d of net '%s', X%d/Y%d/%s to X%d/Y%d/%s.\n", int(i),
                                  ctx->nameOf(net), src.x, src.y, channel_types.at(src.type).type_name.c_str(), sink.x,
                                  sink.y, channel_types.at(sink.type).type_name.c_str());
                    }
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
        for (auto &node_loc : nodes) {
            for (size_t i = 0; i < node_loc.size(); i++) {
                auto &node = node_loc.at(i);
                total_wire_use += int(node.bound_nets.size());
                int overuse = int(node.bound_nets.size()) - channel_types.at(i).width;
                if (overuse > 0) {
                    node.hist_cong_cost += overuse * hist_cong_weight;
                    total_overuse += overuse;
                    overused_wires += 1;
                    for (auto &bound : node.bound_nets)
                        failed_nets.insert(bound.first);
                }
            }
        }
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

    void do_route()
    {
        // Don't multithread if fewer than 200 nets (heuristic)
        if (route_queue.size() < 200) {
            ThreadContext st;
            for (size_t j = 0; j < route_queue.size(); j++) {
                route_net(st, nets_by_udata[route_queue[j]], false);
            }
            return;
        }
        const int Nq = 4, Nv = 2, Nh = 2;
        const int N = Nq + Nv + Nh;
        std::vector<ThreadContext> tcs(N + 1);
        for (auto n : route_queue) {
            auto &nd = nets.at(n);
            auto ni = nets_by_udata.at(n);
            int bin = N;
            int le_x = mid_x - cfg.bb_margin_x;
            int rs_x = mid_x + cfg.bb_margin_x;
            int le_y = mid_y - cfg.bb_margin_y;
            int rs_y = mid_y + cfg.bb_margin_y;
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

    void router_thread(ThreadContext &t)
    {
        for (auto n : t.route_nets) {
            bool result = route_net(t, n, true);
            if (!result)
                t.failed_nets.push_back(n);
        }
    }

    void operator()()
    {
        log_info("Running channel-based global routing...\n");
        log_info("Setting up abstract channel graph...\n");
        auto rstart = std::chrono::high_resolution_clock::now();
        setup_nets();
        setup_nodes();
        partition_nets();
        curr_cong_weight = cfg.init_curr_cong_weight;
        hist_cong_weight = cfg.hist_cong_weight;
        ThreadContext st;
        int iter = 1;

        for (size_t i = 0; i < nets_by_udata.size(); i++)
            route_queue.push_back(i);

        log_info("Running channel router loop...\n");
        do {
            ctx->sorted_shuffle(route_queue);
            do_route();
            route_queue.clear();
            update_congestion();
            for (auto cn : failed_nets)
                route_queue.push_back(cn);
            log_info("    iter=%d nodes=%d overused=%d overuse=%d\n", iter, total_wire_use, overused_wires,
                     total_overuse);
            ++iter;
            curr_cong_weight *= cfg.curr_cong_mult;
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
        log_info("Global route time %.02fs\n", std::chrono::duration<float>(rend - rstart).count());
    }
};
}; // namespace ChannelRouter

NEXTPNR_NAMESPACE_END
