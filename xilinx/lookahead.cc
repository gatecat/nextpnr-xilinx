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

#include "log.h"
#include "nextpnr.h"

#include <fstream>
#include <queue>

NEXTPNR_NAMESPACE_BEGIN

struct LookaheadFinder
{
    Context *ctx;
    dict<int, int> int_rows, int_cols;
    std::vector<std::vector<int>> int_tile_ids;

    LookaheadFinder(Context *ctx) : ctx(ctx){};
    void run()
    {
        init_int();
        run_dijkstra();
    }

    void init_int()
    {
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

    int compute_cost(WireId wire)
    {
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
        case ID_NODE_GLOBAL_LEAF:
        case ID_NODE_GLOBAL_HDISTR:
        case ID_NODE_GLOBAL_VDISTR:
            return 300;
        default:
            return 1;
        };
    }

    struct QueuedWire
    {
        explicit QueuedWire(WireId wire, int score) : wire(wire), score(score){};

        WireId wire;
        int score;

        struct Greater
        {
            bool operator()(const QueuedWire &lhs, const QueuedWire &rhs) const noexcept
            {
                return lhs.score > rhs.score;
            }
        };
    };

    void run_dijkstra()
    {
        // Start from the middle of the device
        std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> queue;
        dict<WireId, int> visit_cost;
        // Pick middle tile
        const auto &mid_row = int_tile_ids.at(int_tile_ids.size() / 2);
        int mid_tile = mid_row.at(mid_row.size() / 2);
        // Add seed wires
        NPNR_ASSERT(mid_tile != -1);
        WireId denorm;
        denorm.tile = mid_tile;
        for (int i = 0; i < ctx->locInfo(denorm).num_wires; i++) {
            denorm.index = i;
            WireId w = canonicalWireId(ctx->chip_info, denorm.tile, denorm.index);
            // TODO: try different intents
            if (ctx->wireIntent(w) != ID_NODE_CLE_OUTPUT)
                continue;
            queue.push(QueuedWire(w, 0));
            visit_cost[w] = 0;
        }
        NPNR_ASSERT(!queue.empty());
        // Dijkstra heap expansion
        while (!queue.empty()) {
            auto curr = queue.top();
            queue.pop();
            if (curr.score > visit_cost.at(curr.wire))
                continue; // already visited with cheaper cost
            for (PipId dh : ctx->getPipsDownhill(curr.wire)) {
                WireId next = ctx->getPipDstWire(dh);
                int next_score = curr.score + compute_cost(next);
                if (visit_cost.count(next) && visit_cost.at(next) <= next_score)
                    continue;
                queue.push(QueuedWire(next, next_score));
                visit_cost[next] = next_score;
            }
        }
        // CSV output
        std::ofstream out("lookahead_map.csv");
        if (!out)
            log_error("Failed to open CSV file for writing");
        for (const auto &row : int_tile_ids) {
            for (auto tile : row) {
                if (tile == -1) {
                    out << "X,";
                    continue;
                }
                WireId denorm;
                denorm.tile = tile;
                int lowest_score = 9999999;
                for (int i = 0; i < ctx->locInfo(denorm).num_wires; i++) {
                    denorm.index = i;
                    WireId w = canonicalWireId(ctx->chip_info, denorm.tile, denorm.index);
                    if (ctx->wireIntent(w) != ID_NODE_PINFEED || !visit_cost.count(w))
                        continue;
                    lowest_score = std::min(lowest_score, visit_cost.at(w));
                }
                out << lowest_score << ",";
            }
            out << std::endl;
        }
    }
};

void Arch::findLookahead()
{
    LookaheadFinder l(getCtx());
    l.run();
}

NEXTPNR_NAMESPACE_END
