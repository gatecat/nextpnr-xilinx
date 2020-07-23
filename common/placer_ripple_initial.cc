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
*/

#include "placer_ripple_int.h"
#include "log.h"
NEXTPNR_NAMESPACE_BEGIN

namespace Ripple {
// Place cells with the BEL attribute set to constrain them
void RippleFPGAPlacer::place_constraints()
{
    size_t placed_cells = 0;
    // Initial constraints placer
    for (int i = 0; i < GetSize(cells); i++) {
        auto &cell = cells.at(i);
        CellInfo *primary = cell.base_cells.at(0).ci;
        auto loc = primary->attrs.find(ctx->id("BEL"));
        if (loc != primary->attrs.end()) {
            std::string loc_name = loc->second.as_string();
            BelId bel = ctx->getBelByName(ctx->id(loc_name));
            if (bel == BelId()) {
                log_error("No Bel named '%s' located for "
                          "this chip (processing BEL attribute on '%s')\n",
                          loc_name.c_str(), ctx->nameOf(primary));
            }

            IdString bel_type = ctx->getBelType(bel);
            if (bel_type != primary->type) {
                log_error("Bel '%s' of type '%s' does not match cell "
                          "'%s' of type '%s'\n",
                          loc_name.c_str(), ctx->nameOf(bel_type), ctx->nameOf(primary), ctx->nameOf(primary->type));
            }

            auto bound_cell = ctx->getBoundBelCell(bel);
            if (bound_cell) {
                log_error("Cell '%s' cannot be bound to bel '%s' since it is already bound to cell \'%s\'\n",
                          ctx->nameOf(primary), loc_name.c_str(), ctx->nameOf(bound_cell));
            }

            Loc bel_loc = ctx->getBelLocation(bel);
            if (!place_cell(i, bel_loc))
                log_error("Error placing cell '%s' on bel '%s'\n", ctx->nameOf(primary), ctx->nameOfBel(bel));

            if (!check_placement(i))
                log_error("Validity check failed placing cell '%s' on bel '%s'\n", ctx->nameOf(primary), ctx->nameOfBel(bel));

            cell.locked = true;
            placed_cells++;
        }
    }
    log_info("Placed %d cells based on constraints.\n", int(placed_cells));
    ctx->yield();
}
}

NEXTPNR_NAMESPACE_END