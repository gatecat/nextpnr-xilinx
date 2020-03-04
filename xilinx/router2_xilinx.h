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

#ifndef ROUTER2_XILINX_H
#define ROUTER2_XILINX_H

#include "nextpnr.h"
#include "router2_int.h"

NEXTPNR_NAMESPACE_BEGIN
namespace Router2 {
struct Router2Xilinx : public Router2ArchFunctions
{
    Context *ctx;
    Router2Xilinx(Context *ctx) : ctx(ctx){};

    ArcRouteResult route_segment(Router2Thread *th, NetInfo *net, size_t seg_idx, bool is_mt, bool no_bb) override;
    std::vector<NetSegment> segment_net(NetInfo *net) override;

    void route_xilinx_const(Router2Thread &t, NetInfo *net, size_t seg_idx, bool is_mt, bool is_bb = true);
};
} // namespace Router2
NEXTPNR_NAMESPACE_END

#endif