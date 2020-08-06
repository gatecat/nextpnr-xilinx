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
#include <boost/optional.hpp>
#include <boost/thread.hpp>
#include <chrono>
#include <deque>
#include <fstream>
#include <numeric>
#include <queue>
#include <tuple>
#include <unordered_map>
#include "log.h"
#include "nextpnr.h"
#include "place_common.h"
#include "placer1.h"
#include "placer_ripple_int.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Ripple {
void RippleFPGAPlacer::init_cells()
{
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->constr_parent != nullptr)
            continue;
        int root = cells.add();
        auto &c = cells[root];
        c.type = ci->type;
        c.macro_extent.x0 = 0;
        c.macro_extent.y0 = 0;
        c.macro_extent.x1 = 0;
        c.macro_extent.y1 = 0;
        c.is_packed = false;
        c.is_macro = false;
        c.index = root;
        process_chain(root, ci, 0, 0);
    }
}

void RippleFPGAPlacer::process_chain(int root, CellInfo *ci, int dx, int dy)
{
    auto &c = cells[root];
    c.macro_extent.x0 = std::min(c.macro_extent.x0, dx);
    c.macro_extent.y0 = std::min(c.macro_extent.y0, dy);
    c.macro_extent.x1 = std::min(c.macro_extent.x1, dx);
    c.macro_extent.y1 = std::min(c.macro_extent.y1, dy);
    int subcell_idx = c.base_cells.add();
    auto &sc = c.base_cells[subcell_idx];
    sc.ci = ci;
    sc.offset_x = dx;
    sc.offset_y = dy;
    ci->udata = GetSize(cells_by_udata);
    cells_by_udata.push_back(ci);
    cell_index.emplace_back(root, subcell_idx);

    for (CellInfo *ch : ci->constr_children) {
        process_chain(root, ch, (ch->constr_x == ch->UNCONSTR) ? 0 : ch->constr_x,
                      (ch->constr_y == ch->UNCONSTR) ? 0 : ch->constr_y);
        c.is_macro = true;
    }
}

Loc RippleFPGAPlacer::get_cell_location(const CellInfo *cell)
{
    Loc loc;
    auto &i = cell_index.at(cell->udata);
    auto &c = cells[i.cell];
    auto &sc = c.base_cells[i.subcell];
    loc.x = c.placed_x + sc.offset_x;
    loc.y = c.placed_y + sc.offset_y;
    loc.z = 0;
    return clamp_location(loc);
}

Loc RippleFPGAPlacer::clamp_location(Loc loc)
{
    loc.x = std::max(0, std::min(loc.x, d.width - 1));
    loc.y = std::max(0, std::min(loc.y, d.height - 1));
    return loc;
}

void RippleFPGAPlacer::run()
{
    ctx->lock();
    f->setPlacer(this);
    init_cells();
    place_constraints();
    place_initial();
    place_global(0);
    array2d<double> cong(d.width, d.height);
    est_congestion_map(cong);
    auto max_cong = *std::max_element(
            cong.begin(), cong.end(),
            [&](const decltype(*cong.begin()) &a, const decltype(*cong.begin()) &b) { return a.value < b.value; });
    log_info("Max congestion: %d, %d: %f\n", max_cong.x, max_cong.y, max_cong.value);

    f->doBlePacking();
    place_global(0);

    ctx->unlock();
}

void RippleFPGAPlacer::merge_positions(RippleCell &a, RippleCell &b)
{
    auto do_merge = [&](int &p1, int &p2) {
        p1 = (p1 * GetSize(a.base_cells) + p2 * GetSize(b.base_cells)) /
             (GetSize(a.base_cells) + GetSize(b.base_cells));
    };
    do_merge(a.placed_x, b.placed_x);
    do_merge(a.placed_y, b.placed_y);
    a.solver_x = a.placed_x;
    a.solver_y = a.placed_y;
}

void RippleFPGAPlacer::merge_cells(RippleCell &base, RippleCell &sub, int rel_x, int rel_y, int rel_z)
{
    merge_positions(base, sub);
    for (auto sc : sub.base_cells) {
        auto &cell_idx = cell_index.at(sc.ci->udata);
        sc.offset_x += rel_x;
        sc.offset_y += rel_y;
        sc.offset_z += rel_z;
        cell_idx.cell = base.index;
        cell_idx.subcell = base.base_cells.add(sc);
    }
    cells.reserve(sub.index);
}

void RippleFPGAPlacer::merge_cells(RippleCell &base, RippleCell &sub, int rel_z)
{
    merge_cells(base, sub, 0, 0, rel_z);
}

} // namespace Ripple

NEXTPNR_NAMESPACE_END
