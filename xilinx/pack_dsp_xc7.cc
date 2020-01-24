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

#include <boost/algorithm/string.hpp>
#include "pack.h"

NEXTPNR_NAMESPACE_BEGIN

void XC7Packer::pack_dsps()
{
    std::unordered_map<IdString, XFormRule> dsp_rules;
    dsp_rules[ctx->id("DSP48E1")].new_type = ctx->id("DSP48E1_DSP48E1");
    generic_xform(dsp_rules, true);

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("DSP48E1_DSP48E1")) {
            // DRC
            NetInfo *clk = get_net_or_empty(ci, id_CLK);
            if (clk != nullptr && clk->name != ctx->id("$PACKER_GND_NET"))
                log_error("Clocked DSP48E1s are currently unsupported (while processing cell %s, clocked by %s).\n",
                          ctx->nameOf(ci), ctx->nameOf(clk));
            for (auto &port : ci->ports) {
                std::string n = port.first.str(ctx);
                if (boost::starts_with(n, "ACIN") || boost::starts_with(n, "BCIN") || boost::starts_with(n, "PCIN")) {
                    if (port.second.net == nullptr)
                        continue;
                    if (port.second.net->name == ctx->id("$PACKER_GND_NET"))
                        disconnect_port(ctx, ci, port.first);
                    else
                        log_error("Cascaded DSP48E1s are currently unsupported (while processing cell %s).\n",
                                  ctx->nameOf(ci));
                }
            }
        }
    }
}

NEXTPNR_NAMESPACE_END
