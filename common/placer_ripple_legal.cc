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

bool RippleFPGAPlacer::place_cell(int cell, Loc root)
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
    }
    if (false) {
    fail:
        ripup_cell(cell);
        return false;
    }
    c.root_loc = root;
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

} // namespace Ripple
NEXTPNR_NAMESPACE_END