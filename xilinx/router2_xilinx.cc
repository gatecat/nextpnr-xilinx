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

NEXTPNR_NAMESPACE_BEGIN
namespace Router2 {
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

std::vector<NetSegment> Router2Xilinx::segment_net(NetInfo *net) { return Router2ArchFunctions::segment_net(net); }

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
                    PipId p = r->flat_wires.at(cursor2).bound_nets.at(net->udata).second;
                    if (p == PipId())
                        break;
                    cursor2 = r->wire_to_idx.at(ctx->getPipSrcWire(p));
                }
                if (!bwd_merge_fail && cursor2 == src_wire_idx) {
                    // Found a path to merge to existing routing; backwards
                    cursor2 = cursor;
                    while (r->flat_wires.at(cursor2).bound_nets.count(net->udata)) {
                        PipId p = r->flat_wires.at(cursor2).bound_nets.at(net->udata).second;
                        if (p == PipId())
                            break;
                        cursor2 = r->wire_to_idx.at(ctx->getPipSrcWire(p));
                        r->set_visited(t, cursor2, p, Router2State::WireScore());
                    }
                    break;
                }
                cpip = cwd.bound_nets.at(net->udata).second;
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
