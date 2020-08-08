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
 *  [[cite]] RippleFPGA
 *  RippleFPGA: A Routability-Driven Placement forLarge-Scale Heterogeneous FPGAs
 *  Chak-Wa Pui, Gengjie Chen, Wing-Kai Chow, Ka-Chun Lam, Jian Kuang,Peishan Tu, Hang Zhang, Evangeline F. Y. Young,
 * Bei Yu https://chengengjie.github.io/papers/C2-ICCAD16-RippleFPGA.pdf
 *
 *
 *  Original Implementation: https://github.com/cuhk-eda/ripple-fpga
 *
 *  Original Copyright:
 *
 * Copyright (c) 2019 by The Chinese University of Hong Kong
 * All rights reserved
 * CU-SD LICENSE (adapted from the original BSD license)
 * Redistribution of the any code, with or without modification, are permitted provided that the conditions below are
 * met. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution. Neither the name nor
 * trademark of the copyright holder or the author may be used to endorse or promote products derived from this software
 * without specific prior written permission. Users are entirely responsible, to the exclusion of the author, for
 * compliance with (a) regulations set by owners or administrators of employed equipment, (b) licensing terms of any
 * other software, and (c) local, national, and international regulations regarding use, including those regarding
 * import, export, and use of encryption software. THIS FREE SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR ANY CONTRIBUTOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, EFFECTS OF
 * UNAUTHORIZED OR MALICIOUS NETWORK ACCESS; PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *
 */

#include "placer_ripple_int.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Ripple {

bool RippleFPGAPlacer::place_cell(int cell, Loc root, DetailMove *move_to_update)
{
    auto &c = cells.at(cell);
    NPNR_ASSERT(!c.placed);
    for (auto &sc : c.base_cells) {
        Loc l = sc.actual_loc(root);
        BelId bel = ctx->getBelByLocation(l);
        if (bel == BelId())
            goto fail;
        if (ctx->getBelType(bel) != sc.ci->type)
            goto fail;
        if (!ctx->checkBelAvail(bel))
            goto fail;
        ctx->bindBel(bel, sc.ci, STRENGTH_WEAK);
        if (move_to_update != nullptr)
            update_move_costs(*move_to_update, sc.ci, ctx->getBelLocation(bel), bel);
    }
    if (false) {
    fail:
        ripup_cell(cell);
        return false;
    }
    c.root_loc = root;
    c.placed_x = root.x;
    c.placed_y = root.y;
    c.placed = true;
    return true;
}

void RippleFPGAPlacer::ripup_cell(int cell)
{
    auto &c = cells.at(cell);
    for (auto &sc : c.base_cells) {
        if (sc.ci->bel != BelId())
            ctx->unbindBel(sc.ci->bel);
    }
    c.placed = false;
}

bool RippleFPGAPlacer::check_placement(int cell)
{
    auto &c = cells.at(cell);
    NPNR_ASSERT(c.placed);
    for (auto &sc : c.base_cells)
        if (!ctx->isBelLocationValid(sc.ci->bel))
            return false;
    return true;
}

bool RippleFPGAPlacer::find_conflicting_cells(int cell, Loc root, std::map<int, Loc> &conflicts)
{
    auto insert_conflicts = [&](CellInfo *conflict) {
        int idx = cell_index.at(conflict->udata).cell;
        Loc conflict_root = cells.at(idx).root_loc;
        Loc offset;
        offset.x = conflict_root.x - root.x;
        offset.y = conflict_root.y - root.y;
        offset.z = conflict_root.z - root.z;
        if (!conflicts.count(idx))
            conflicts[idx] = offset;
        else
            NPNR_ASSERT(conflicts.at(idx) == offset);
    };

    auto &c = cells.at(cell);
    for (auto &sc : c.base_cells) {
        Loc l = sc.actual_loc(root);
        BelId bel = ctx->getBelByLocation(l);
        if (bel == BelId())
            return false;
        if (ctx->getBelType(bel) != sc.ci->type)
            return false;
        if (ctx->checkBelAvail(bel))
            continue;
        CellInfo *bound = ctx->getBoundBelCell(bel);
        if (bound != nullptr) {
            if (bound->belStrength > STRENGTH_STRONG)
                return false;
            insert_conflicts(bound);
            continue;
        }
        CellInfo *conflicting = ctx->getConflictingBelCell(bel);
        if (conflicting != nullptr) {
            if (conflicting->belStrength > STRENGTH_STRONG)
                return false;
            insert_conflicts(conflicting);
            continue;
        }
        return false;
    }
    return true;
}

bool RippleFPGAPlacer::detail_find_candidate_locs(const std::vector<int> &move_cells, DetailMove &optimal)
{
    // We might be moving more than one ripple-cell at once, when we do whole-tile swaps
    // In this case we need to keep track of offsets from cell 0
    std::vector<Loc> cell_offset(GetSize(move_cells));
    auto &front_cell = cells.at(move_cells.front());
    for (int i = 1; i < GetSize(move_cells); i++) {
        int c_idx = move_cells.at(i);
        auto &cell = cells.at(c_idx);
        // Moving of multiple cells at once is only supported in refinement, not legalisation
        NPNR_ASSERT(cell.placed);
        NPNR_ASSERT(front_cell.placed);
        cell_offset.at(i).x = cell.root_loc.x - front_cell.root_loc.x;
        cell_offset.at(i).y = cell.root_loc.y - front_cell.root_loc.y;
        cell_offset.at(i).z = cell.root_loc.z - front_cell.root_loc.z;
    }
    // Determine the x, y and z bounds of the overall move_cells array, so we can quickly rule
    // out moves that there isn't room for in the first place.
    Bounds macro_bounds;
    int macro_zheight = 0;
    // Ensuring that 0,0 is in the macro bounds removes the need to handle various tedious
    // special cases later on
    macro_bounds.expand(0, 0);
    for (int i = 0; i < GetSize(move_cells); i++) {
        auto &cell = cells.at(move_cells.at(i));
        auto &offset = cell_offset.at(i);
        Bounds bb = cell.macro_extent;
        bb.x0 += offset.x;
        bb.y0 += offset.y;
        bb.x1 += offset.x;
        bb.y1 += offset.y;
        macro_bounds.expand(bb.x0, bb.y0);
        macro_bounds.expand(bb.x1, bb.y1);
        for (auto sc : cell.base_cells)
            macro_zheight = std::max(macro_zheight, sc.abs_z ? sc.offset_z : (sc.offset_z + offset.z));
    }

    // Center X and Y coordinates
    int cx, cy;

    if (front_cell.placed) {
        cx = front_cell.root_loc.x;
        cy = front_cell.root_loc.y;
    } else {
        cx = front_cell.placed_x;
        cy = front_cell.placed_y;
    };

    int max_radius = 5, locs_to_check = 10;
    int radius = 0, found_cells = 0;
    Bounds search_bounds;
    search_bounds.expand(cx, cy);
    Bounds last_bounds;

    auto proc_xy = [&](int x, int y) {

    };

    int type_idx = sitetype_to_idx.at(front_cell.type);
    auto &type_rc = site_rows_cols.at(type_idx);

    // Keep searching until radius limit is reached; enough candidate locations have been checked or no more progress is
    // being made
    while (radius < max_radius && found_cells < locs_to_check && search_bounds != last_bounds) {
        last_bounds = search_bounds;
        for (int x = search_bounds.x0; x <= search_bounds.x1; x++) {
            // Top edge
            proc_xy(x, search_bounds.y0);
            // Bottom edge (assuming it is different from top edge)
            if (search_bounds.y0 != search_bounds.y1)
                proc_xy(x, search_bounds.y1);
        }
        // This search is exclusive because the corners were covered already above
        for (int y = (search_bounds.y0 + 1); y <= (search_bounds.y1 - 1); y++) {
            // Left edge
            proc_xy(search_bounds.x0, y);
            // Right edge (assuming it is different from left edge)
            if (search_bounds.x0 != search_bounds.x1)
                proc_xy(search_bounds.x1, y);
        }
        // Extend bounds by 1 until we find a column with cells, or reach the limit
        for (; search_bounds.x0 >= -macro_bounds.x0; search_bounds.x0--)
            if (search_bounds.x0 != last_bounds.x0 && type_rc.cols.at(search_bounds.x0))
                break;
        for (; search_bounds.y0 >= -macro_bounds.y0; search_bounds.y0--)
            if (search_bounds.y0 != last_bounds.y0 && type_rc.rows.at(search_bounds.y0))
                break;
        for (; search_bounds.x1 < d.width - macro_bounds.x1; search_bounds.x1++)
            if (search_bounds.x1 != last_bounds.x1 && type_rc.cols.at(search_bounds.x1))
                break;
        for (; search_bounds.y1 < d.height - macro_bounds.y1; search_bounds.y1++)
            if (search_bounds.y1 != last_bounds.y1 && type_rc.rows.at(search_bounds.y1))
                break;
    }

    return false;
}

} // namespace Ripple
NEXTPNR_NAMESPACE_END