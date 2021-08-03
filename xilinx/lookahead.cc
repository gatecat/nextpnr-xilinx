/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  gatecat <gatecat@ds0.me>
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

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct LookaheadFinder {
    Context *ctx;
    dict<int, int> int_rows, int_cols;
    std::vector<std::vector<int>> int_tile_ids;

    LookaheadFinder(Context *ctx) : ctx(ctx) {};
    void run() {
        init_int();
    }

    void init_int() {
        for (int y = 0; y < ctx->chip_info->height; y++) {
            bool has_int = false;
            for (int x = 0; x < ctx->chip_info->width; x++) {
                int t = y * ctx->chip_info->width + x;
                IdString tt(ctx->chip_info->tile_types[ctx->chip_info->tile_insts[t].type].type);
                if (tt == ctx->id("INT")) {
                    has_int = true;
                    break;
                }
            }
            if (has_int) {
                int idx = int_rows.size();
                int_rows[y] = idx;
            }
        }
        for (int x = 0; x < ctx->chip_info->width; x++) {
            bool has_int = false;
            for (int y = 0; y < ctx->chip_info->height; y++) {
                int t = y * ctx->chip_info->width + x;
                IdString tt(ctx->chip_info->tile_types[ctx->chip_info->tile_insts[t].type].type);
                if (tt == ctx->id("INT")) {
                    has_int = true;
                    break;
                }
            }
            if (has_int) {
                int idx = int_cols.size();
                int_cols[x] = idx;
            }
        }
        int_tile_ids.resize(int_rows.size());
        for (auto y : int_rows) {
            int_tile_ids.at(y.second).resize(int_cols.size(), -1);
            for (auto x : int_cols) {
                int t = y.first * ctx->chip_info->width + x.first;
                IdString tt(ctx->chip_info->tile_types[ctx->chip_info->tile_insts[t].type].type);
                if (tt == ctx->id("INT"))
                    int_tile_ids.at(y.second).at(x.second) = t;
            }
        }
    }

    int compute_cost(WireId wire) {
        int32_t intent = ctx->wireIntent(wire);
        switch (intent) {
            case ID_NODE_HLONG:
            case ID_NODE_VLONG:
                return 12;
            case ID_NODE_HQUAD:
            case ID_NODE_VQUAD:
                return 4;
            case ID_NODE_DOUBLE:
            case ID_NODE_PINBOUNCE:
                return 2;
            default:
                return 1;
        };
    }
};

void Arch::findLookahead() {
    LookaheadFinder l(getCtx());
    l.run();
}

NEXTPNR_NAMESPACE_END
