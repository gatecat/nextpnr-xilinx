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
    for (int bc : move.bounds_changed_nets_x) {
        dt_nets.at(bc).change_type_x = NO_CHANGE;
        dt_nets.at(bc).new_bounds = dt_nets.at(bc).curr_bounds;
    }
    for (int bc : move.bounds_changed_nets_y) {
        dt_nets.at(bc).change_type_y = NO_CHANGE;
        dt_nets.at(bc).new_bounds = dt_nets.at(bc).curr_bounds;
    }
    move.bounds_changed_nets_x.clear();
    move.bounds_changed_nets_y.clear();
    move.wirelen_delta = 0;
    move.routeability_delta = 0;
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
        if (!ret)
            return false;
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

void RippleFPGAPlacer::update_move_costs(DetailMove &move, CellInfo *cell, BelId old_bel)
{
    Loc curr_loc = ctx->getBelLocation(cell->bel);
    Loc old_loc = ctx->getBelLocation(old_bel);
    // Check net bounds
    for (const auto &port : cell->ports) {
        NetInfo *pn = port.second.net;
        if (pn == nullptr)
            continue;
        if (cost_ignore_net(pn))
            continue;
        auto &net_data = dt_nets.at(pn->udata);
        NetBoundingBox &curr_bounds = net_data.new_bounds;
        // Incremental bounding box updates
        // Note that everything other than full updates are applied immediately rather than being queued,
        // so further updates to the same net in the same move are dealt with correctly.
        // If a full update is already queued, this can be considered a no-op
        if (net_data.change_type_x != BoundChangeType::FULL_RECOMPUTE) {
            // Bounds x0
            if (curr_loc.x < curr_bounds.x0) {
                // Further out than current bounds x0
                curr_bounds.x0 = curr_loc.x;
                curr_bounds.nx0 = 1;
                if (net_data.change_type_x == BoundChangeType::NO_CHANGE) {
                    // Checking already_bounds_changed_x ensures that each net is only added once
                    // to bounds_changed_nets, lest we add its HPWL change multiple times skewing the
                    // overall cost change
                    net_data.change_type_x = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_x.push_back(pn->udata);
                }
            } else if (curr_loc.x == curr_bounds.x0 && old_loc.x > curr_bounds.x0) {
                curr_bounds.nx0++;
                if (net_data.change_type_x == BoundChangeType::NO_CHANGE) {
                    net_data.change_type_x = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_x.push_back(pn->udata);
                }
            } else if (old_loc.x == curr_bounds.x0 && curr_loc.x > curr_bounds.x0) {
                if (net_data.change_type_x == BoundChangeType::NO_CHANGE)
                    move.bounds_changed_nets_x.push_back(pn->udata);
                if (curr_bounds.nx0 == 1) {
                    net_data.change_type_x = BoundChangeType::FULL_RECOMPUTE;
                } else {
                    curr_bounds.nx0--;
                    if (net_data.change_type_x == BoundChangeType::NO_CHANGE)
                        net_data.change_type_x = BoundChangeType::CELL_MOVED_INWARDS;
                }
            }

            // Bounds x1
            if (curr_loc.x > curr_bounds.x1) {
                // Further out than current bounds x1
                curr_bounds.x1 = curr_loc.x;
                curr_bounds.nx1 = 1;
                if (net_data.change_type_x == BoundChangeType::NO_CHANGE) {
                    // Checking already_bounds_changed_x ensures that each net is only added once
                    // to bounds_changed_nets, lest we add its HPWL change multiple times skewing the
                    // overall cost change
                    net_data.change_type_x = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_x.push_back(pn->udata);
                }
            } else if (curr_loc.x == curr_bounds.x1 && old_loc.x < curr_bounds.x1) {
                curr_bounds.nx1++;
                if (net_data.change_type_x == BoundChangeType::NO_CHANGE) {
                    net_data.change_type_x = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_x.push_back(pn->udata);
                }
            } else if (old_loc.x == curr_bounds.x1 && curr_loc.x < curr_bounds.x1) {
                if (net_data.change_type_x == BoundChangeType::NO_CHANGE)
                    move.bounds_changed_nets_x.push_back(pn->udata);
                if (curr_bounds.nx1 == 1) {
                    net_data.change_type_x = BoundChangeType::FULL_RECOMPUTE;
                } else {
                    curr_bounds.nx1--;
                    if (net_data.change_type_x == BoundChangeType::NO_CHANGE)
                        net_data.change_type_x = BoundChangeType::CELL_MOVED_INWARDS;
                }
            }
        }
        if (net_data.change_type_y != BoundChangeType::FULL_RECOMPUTE) {
            // Bounds y0
            if (curr_loc.y < curr_bounds.y0) {
                // Further out than current bounds y0
                curr_bounds.y0 = curr_loc.y;
                curr_bounds.ny0 = 1;
                if (net_data.change_type_y == BoundChangeType::NO_CHANGE) {
                    net_data.change_type_y = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_y.push_back(pn->udata);
                }
            } else if (curr_loc.y == curr_bounds.y0 && old_loc.y > curr_bounds.y0) {
                curr_bounds.ny0++;
                if (net_data.change_type_y == BoundChangeType::NO_CHANGE) {
                    net_data.change_type_y = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_y.push_back(pn->udata);
                }
            } else if (old_loc.y == curr_bounds.y0 && curr_loc.y > curr_bounds.y0) {
                if (net_data.change_type_y == BoundChangeType::NO_CHANGE)
                    move.bounds_changed_nets_y.push_back(pn->udata);
                if (curr_bounds.ny0 == 1) {
                    net_data.change_type_y = BoundChangeType::FULL_RECOMPUTE;
                } else {
                    curr_bounds.ny0--;
                    if (net_data.change_type_y == BoundChangeType::NO_CHANGE)
                        net_data.change_type_y = BoundChangeType::CELL_MOVED_INWARDS;
                }
            }

            // Bounds y1
            if (curr_loc.y > curr_bounds.y1) {
                // Further out than current bounds y1
                curr_bounds.y1 = curr_loc.y;
                curr_bounds.ny1 = 1;
                if (net_data.change_type_y == BoundChangeType::NO_CHANGE) {
                    net_data.change_type_y = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_y.push_back(pn->udata);
                }
            } else if (curr_loc.y == curr_bounds.y1 && old_loc.y < curr_bounds.y1) {
                curr_bounds.ny1++;
                if (net_data.change_type_y == BoundChangeType::NO_CHANGE) {
                    net_data.change_type_y = BoundChangeType::CELL_MOVED_OUTWARDS;
                    move.bounds_changed_nets_y.push_back(pn->udata);
                }
            } else if (old_loc.y == curr_bounds.y1 && curr_loc.y < curr_bounds.y1) {
                if (net_data.change_type_y == BoundChangeType::NO_CHANGE)
                    move.bounds_changed_nets_y.push_back(pn->udata);
                if (curr_bounds.ny1 == 1) {
                    net_data.change_type_y = BoundChangeType::FULL_RECOMPUTE;
                } else {
                    curr_bounds.ny1--;
                    if (net_data.change_type_y == BoundChangeType::NO_CHANGE)
                        net_data.change_type_y = BoundChangeType::CELL_MOVED_INWARDS;
                }
            }
        }
    }
}

void RippleFPGAPlacer::setup_detail()
{
    dt_nets.reserve(GetSize(ctx->nets));
    nets_by_udata.reserve(GetSize(ctx->nets));
    for (auto net : sorted(ctx->nets)) {
        NetInfo *ni = net.second;
        ni->udata = GetSize(nets_by_udata);
        nets_by_udata.push_back(ni);
        dt_nets.emplace_back();
        dt_nets.back().curr_bounds = get_net_bounds(ni);
        dt_nets.back().arcs.resize(GetSize(ni->users));
    }
}

RippleFPGAPlacer::NetBoundingBox RippleFPGAPlacer::get_net_bounds(NetInfo *net)
{
    NetBoundingBox bb;
    Loc dloc = ctx->getBelLocation(net->driver.cell->bel);
    bb.x0 = bb.x1 = dloc.x;
    bb.y0 = bb.y1 = dloc.y;
    bb.nx0 = bb.nx1 = bb.ny0 = bb.ny1 = 1;
    for (auto &usr : net->users) {
        if (usr.cell->bel == BelId())
            continue;
        Loc uloc = ctx->getBelLocation(usr.cell->bel);
        /*
         * If the location of the sink is on the current edge,
         * increment the count of ports on that edge.
         * If the location of the sink is beyond the current bounds,
         * expand the bounding box and reset the number of edge ports to 1.
         */
        if (bb.x0 == uloc.x)
            ++bb.nx0;
        else if (uloc.x < bb.x0) {
            bb.x0 = uloc.x;
            bb.nx0 = 1;
        }
        if (bb.x1 == uloc.x)
            ++bb.nx1;
        else if (uloc.x > bb.x1) {
            bb.x1 = uloc.x;
            bb.nx1 = 1;
        }
        if (bb.y0 == uloc.y)
            ++bb.ny0;
        else if (uloc.y < bb.y0) {
            bb.y0 = uloc.y;
            bb.ny0 = 1;
        }
        if (bb.y1 == uloc.y)
            ++bb.ny1;
        else if (uloc.y > bb.y1) {
            bb.y1 = uloc.y;
            bb.ny1 = 1;
        }
    }
    return bb;
}

void RippleFPGAPlacer::compute_move_costs(DetailMove &move)
{
    move.wirelen_delta = 0;
    for (auto bc : move.bounds_changed_nets_x)
        if (dt_nets.at(bc).change_type_x == BoundChangeType::FULL_RECOMPUTE)
            dt_nets.at(bc).new_bounds = get_net_bounds(nets_by_udata.at(bc));
    for (auto bc : move.bounds_changed_nets_y)
        if (dt_nets.at(bc).change_type_x != BoundChangeType::FULL_RECOMPUTE &&
            dt_nets.at(bc).change_type_y == BoundChangeType::FULL_RECOMPUTE)
            dt_nets.at(bc).new_bounds = get_net_bounds(nets_by_udata.at(bc));
    for (auto bc : move.bounds_changed_nets_x)
        move.wirelen_delta += dt_nets.at(bc).new_bounds.hpwl() - dt_nets.at(bc).curr_bounds.hpwl();
}

void RippleFPGAPlacer::finalise_move(DetailMove &move)
{
    for (auto bc : move.bounds_changed_nets_x)
        dt_nets.at(bc).curr_bounds = dt_nets.at(bc).new_bounds;
    for (auto bc : move.bounds_changed_nets_y)
        dt_nets.at(bc).curr_bounds = dt_nets.at(bc).new_bounds;
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
