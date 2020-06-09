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

#include "nextpnr.h"
#include "place_common.h"
#include "placer1.h"
#include "placer_ripple_int.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Ripple {

void RippleFPGAPlacer::reset_move(DetailMove &move)
{
    move.moved.clear();
    for (int bc : move.bounds_changed_nets_x)
        dt_nets.at(bc).change_type_x = NO_CHANGE;
    for (int bc : move.bounds_changed_nets_y)
        dt_nets.at(bc).change_type_y = NO_CHANGE;
    move.bounds_changed_nets_x.clear();
    move.bounds_changed_nets_y.clear();
}

Loc RippleFPGAPlacer::move_get_cell_loc(DetailMove &move, int i)
{
    if (i == 0)
        return move.new_root_loc;
    Loc base_loc = cells.at(move.move_cells.at(0)).root_loc;
    Loc new_loc = cells.at(move.move_cells.at(i)).root_loc;
    new_loc.x += (move.new_root_loc.x - base_loc.x);
    new_loc.y += (move.new_root_loc.y - base_loc.y);
    new_loc.z += (move.new_root_loc.z - base_loc.z);
    return new_loc;
}

bool RippleFPGAPlacer::find_move_conflicts(DetailMove &move)
{
    move.conflicts.clear();
    for (int i = 0; i < GetSize(move.move_cells); i++) {
        std::map<int, Loc> cell_conflicts;
        int cell_idx = move.move_cells.at(i);
        auto &cell_data = cells.at(cell_idx);
        Loc cell_loc = move_get_cell_loc(move, i);
        bool ret = find_conflicting_cells(cell_idx, cell_loc, cell_conflicts);
        // Translate conflicting cell coordinates from cell-relative to absolute
        for (auto &conflict : cell_conflicts) {
            conflict.second.x += cell_data.root_loc.x;
            conflict.second.y += cell_data.root_loc.y;
            conflict.second.z += cell_data.root_loc.z;
            // Abort if the conflicting cell would need to be placed at more than one distinct location
            if (move.conflicts.count(conflict.first)) {
                if (move.conflicts.at(conflict.first) != conflict.second)
                    return false;
            } else {
                move.conflicts[conflict.first] = conflict.second;
            }
        }
    }
    return true;
}

void RippleFPGAPlacer::compute_move_costs(DetailMove &move)
{
    // Go through all the moving cells and update the costs
    for (int i = 0; i < GetSize(move.move_cells); i++)
        update_move_costs(move, move.move_cells.at(i), move_get_cell_loc(move, i));
    for (auto conflict : move.conflicts)
        update_move_costs(move, conflict.first, conflict.second);
}

bool RippleFPGAPlacer::perform_move(DetailMove &move)
{
    // Ripup all cells involved in the move, and save the original locations
    for (auto conflict : move.conflicts) {
        move.moved.emplace_back();
        move.moved.back().cell = conflict.first;
        move.moved.back().old_root = cells.at(conflict.first).root_loc;
        ripup_cell(conflict.first);
    }
    std::vector<Loc> new_locs;
    for (int i = 0; i < GetSize(move.move_cells); i++) {
        int cell_idx = move.move_cells.at(i);
        Loc new_loc = move_get_cell_loc(move, i);
        new_locs.push_back(new_loc);
        move.moved.emplace_back();
        move.moved.back().cell = cell_idx;
        move.moved.back().old_root = cells.at(cell_idx).root_loc;
        ripup_cell(cell_idx);
    }
    // Now place cells at their new locations
    for (int i = 0; i < GetSize(move.move_cells); i++) {
        bool ret = place_cell(move.move_cells.at(i), new_locs.at(i));
        if (!ret)
            goto fail;
    }
    for (auto conflict : move.conflicts) {
        bool ret = place_cell(conflict.first, conflict.second);
        if (!ret)
            goto fail;
    }
    // Check new locations for legality
    // TODO: speedup validity checks when swapping whole tiles?
    for (auto m : move.moved)
        if (!check_placement(m.cell))
            goto fail;

    return true;
fail:
    revert_move(move);
    return false;
}

void RippleFPGAPlacer::revert_move(DetailMove &move)
{
    // First, ripup all moved cells in case the move is fully or partially placed
    for (auto m : move.moved)
        ripup_cell(m.cell);
    // Now place cells back in their original location
    for (auto m : move.moved)
        place_cell(m.cell, m.old_root);
}

} // namespace Ripple

NEXTPNR_NAMESPACE_END
