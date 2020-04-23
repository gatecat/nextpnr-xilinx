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
#include "int_graph.h"
#include "nextpnr.h"

#include <queue>

NEXTPNR_NAMESPACE_BEGIN
namespace ChannelRouter {

struct UltrascaleChannelGraph : ChannelGraph
{
    Context *ctx;
    UltrascaleChannelGraph(Context *ctx) : ctx(ctx){};

    // Mapping from real to "INT grid" coordinates
    std::vector<int> row_r2i, col_r2i;
    std::set<int> int_rows, int_cols;
    int int_width, int_height;
    // Mapping from INT wire name to INT wire index
    std::unordered_map<IdString, int> name2idx;

    void setup()
    {
        row_r2i.resize(ctx->chip_info->height, -1);
        col_r2i.resize(ctx->chip_info->width, -1);
        for (int y = 0; y < ctx->chip_info->height; y++) {
            for (int x = 0; x < ctx->chip_info->width; x++) {
                IdString tt = IdString(
                        ctx->chip_info->tile_types[ctx->chip_info->tile_insts[y * ctx->chip_info->height + x].type]
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
    }
};

} // namespace ChannelRouter

NEXTPNR_NAMESPACE_END
