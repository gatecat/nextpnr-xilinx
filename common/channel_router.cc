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
struct ChannelRouterState
{
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
            ChannelNode bwd;
            WireScore score;
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
        ChannelNode source_node;
        std::vector<PerArcData> arcs;
        ArcBounds bb;
        // Coordinates of the center of the net, used for the weight-to-average
        int cx, cy, hpwl;
        int total_route_us = 0;
    };

    Context *ctx;
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

    PerNodeData &node_data(const ChannelNode &node)
    {
        NPNR_ASSERT(node.x >= 0 && node.x < width);
        NPNR_ASSERT(node.y >= 0 && node.y < height);
        return nodes.at(node.y * width + node.x).at(node.type);
    }

    float present_node_cost(const PerNodeData &w, int net_uid)
    {
        int over_capacity = int(w.bound_nets.size());
        over_capacity -= (channel_types.at(w.type).width - 1);
        if (w.bound_nets.count(net_uid))
            over_capacity -= 1;
        if (over_capacity <= 0)
            return 1.0f;
        else
            return 1 + over_capacity * curr_cong_weight;
    }
};
}; // namespace ChannelRouter

NEXTPNR_NAMESPACE_END
