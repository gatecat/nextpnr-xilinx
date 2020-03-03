/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019-2020  David Shah <dave@ds0.me>
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
#ifndef ROUTER2_INT_H
#define ROUTER2_INT_H
#include <boost/container/flat_map.hpp>
#include <queue>
#include "nextpnr.h"
#include "router2.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Router2 {
enum ArcRouteResult
{
    ARC_SUCCESS,
    ARC_RETRY_WITHOUT_BB,
    ARC_USE_DEFAULT,
    ARC_FATAL,
};

struct NetSegment
{
    NetSegment(const Context *ctx, NetInfo *net, size_t user) : orig_net(net), orig_user(user)
    {
        src_wire = ctx->getNetinfoSourceWire(net);
        dst_wire = ctx->getNetinfoSinkWire(net, net->users.at(user));
    }
    NetInfo *orig_net;
    size_t orig_user;
    WireId src_wire, dst_wire;
};

struct Router2State;
struct Router2Thread;

// Architectures may add their own implementations of these
struct Router2ArchFunctions
{
    Router2State *r;
    // Arch-specific routing of a segment (perhaps for constant or IO nets)
    // Return ARC_USE_DEFAULT to route normally
    ArcRouteResult route_segment(Router2Thread *th, const NetInfo *net, size_t seg_idx, bool is_mt, bool no_bb);
    // Arch-specific segmenting of a net (perhaps to use global or cross-SLR resources)
    std::vector<NetSegment> segment_net(NetInfo *net);
    // Return true if a net should be totally skipped by the router
    bool skip_net(NetInfo *net);
};

struct Router2State
{

    Context *ctx;
    Router2Cfg cfg;

    struct PerSegmentData
    {
        NetSegment segment;
        ArcBounds bb;
        bool routed = false;
    };

    // As we allow overlap at first; the nextpnr bind functions can't be used
    // as the primary relation between arcs and wires/pips
    struct PerNetData
    {
        WireId src_wire;
        std::vector<PerSegmentData> segments;
        ArcBounds bb;
        // Coordinates of the center of the net, used for the weight-to-average
        int cx, cy, hpwl;
        int total_route_us = 0;
        float max_crit = 0;
    };

    struct WireScore
    {
        float cost;
        float togo_cost;
        delay_t delay;
        float total() const { return cost + togo_cost; }
    };
    struct QueuedWire
    {

        explicit QueuedWire(int wire = -1, PipId pip = PipId(), Loc loc = Loc(), WireScore score = WireScore{},
                            int randtag = 0)
                : wire(wire), pip(pip), loc(loc), score(score), randtag(randtag){};

        int wire;
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
        // Visit data
        struct
        {
            bool dirty = false, visited = false;
            PipId pip;
            WireScore score;
        } visit;
    };

    // Use 'udata' for fast net lookups and indexing
    std::vector<NetInfo *> nets_by_udata;
    std::vector<PerNetData> nets;
    std::unordered_map<WireId, int> wire_to_idx;
    std::vector<PerWireData> flat_wires;

    double curr_cong_weight, hist_cong_weight, estimate_weight;

    // Criticality data from timing analysis
    NetCriticalityMap net_crit;

    int total_wire_use = 0;
    int overused_wires = 0;
    int total_overuse = 0;
    int arch_fail = 0;
    std::vector<int> route_queue;
    std::set<int> failed_nets;

    int mid_x = 0, mid_y = 0;

    void setup_nets();
    void setup_wires();

    float present_wire_cost(const PerWireData &w, int net_uid);
    bool hit_test_pip(ArcBounds &bb, Loc l);
    float score_wire_for_arc(NetInfo *net, size_t user, WireId wire, PipId pip);
    float get_togo_cost(NetInfo *net, size_t user, int wire, WireId sink);
    bool is_wire_undriveable(WireId wire);

    inline PerWireData &wire_data(WireId w) { return flat_wires[wire_to_idx.at(w)]; }

    void bind_pip_internal(NetInfo *net, size_t user, int wire, PipId pip);
    void unbind_pip_internal(NetInfo *net, size_t user, WireId wire);
    void ripup_arc(NetInfo *net, size_t user);
    bool check_arc_routing(NetInfo *net, size_t usr);
    bool bind_and_check(NetInfo *net, int usr_idx);
    bool bind_and_check_all();

    void reserve_wires_for_arc(NetInfo *net, size_t i);
    void find_all_reserved_wires();

    void reset_wires(Router2Thread &t);
    void set_visited(Router2Thread &t, int wire, PipId pip, WireScore score);
    inline bool was_visited(int wire) { return flat_wires.at(wire).visit.visited; }

    ArcRouteResult route_arc(Router2Thread &t, NetInfo *net, size_t i, bool is_mt, bool is_bb = true);
    bool route_net(Router2Thread &t, NetInfo *net, bool is_mt);

    void partition_nets();

    void update_congestion();
    void write_heatmap(std::ostream &out, bool congestion = false);

    void router_thread(Router2Thread &t);

    void do_route();

    Router2State(Context *ctx, const Router2Cfg &cfg) : ctx(ctx), cfg(cfg) {}

    void operator()();
};

struct Router2Thread
{
    // Nets to route
    std::vector<NetInfo *> route_nets;
    // Nets that failed routing
    std::vector<NetInfo *> failed_nets;

    std::vector<int> route_arcs;

    std::priority_queue<Router2State::QueuedWire, std::vector<Router2State::QueuedWire>,
                        Router2State::QueuedWire::Greater>
            queue;
    // Special case where one net has multiple logical arcs to the same physical sink
    std::unordered_set<WireId> processed_sinks;

    // Backwards routing
    std::queue<int> backwards_queue;

    std::vector<int> dirty_wires;
};

} // namespace Router2

NEXTPNR_NAMESPACE_END
#endif