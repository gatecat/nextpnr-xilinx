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
#include "placer_ripple_int.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
// Run a function on all ports of a net - including the driver and all users
template <typename Tf> void foreach_port(NetInfo *net, Tf func)
{
    if (net->driver.cell != nullptr)
        func(net->driver, -1);
    for (size_t i = 0; i < net->users.size(); i++)
        func(net->users.at(i), i);
}
} // namespace

namespace Ripple {
void RippleFPGAPlacer::lower_bound_solver(double tol, double alpha, int iters)
{

    std::vector<int> cell2var(cells.size(), -1);
    int num_vars = 0;
    for (auto &cell : cells) {
        if (cell.locked)
            continue;
        cell2var[cell.index] = num_vars++;
    }

    EquationSystem eqx(num_vars, num_vars), eqy(num_vars, num_vars);

    // Return the x or y position of a cell, depending on ydir
    auto cell_pos = [&](CellInfo *cell, bool yaxis) {
        auto &i = cell_index.at(cell->udata);
        auto &c = cells[i.cell];
        auto &sc = c.base_cells[i.subcell];
        return yaxis ? (c.solver_y + sc.offset_y) : (c.solver_x + sc.offset_x);
    };
    auto cell_offset = [&](CellInfo *cell, bool yaxis) {
        auto &i = cell_index.at(cell->udata);
        auto &c = cells[i.cell];
        auto &sc = c.base_cells[i.subcell];
        return yaxis ? sc.offset_y : sc.offset_x;
    };

    auto build_equations = [&](bool yaxis) {
        auto &eq = yaxis ? eqy : eqx;
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            if (ni->driver.cell == nullptr)
                continue;
            if (ni->users.empty())
                continue;
            // FIXME: skip global nets
            // Find the bounds of the net in this axis, and the ports that correspond to these bounds
            PortRef *lbport = nullptr, *ubport = nullptr;
            int lbpos = std::numeric_limits<int>::max(), ubpos = std::numeric_limits<int>::min();
            foreach_port(ni, [&](PortRef &port, int user_idx) {
                int pos = cell_pos(port.cell, yaxis);
                if (pos < lbpos) {
                    lbpos = pos;
                    lbport = &port;
                }
                if (pos > ubpos) {
                    ubpos = pos;
                    ubport = &port;
                }
            });

            NPNR_ASSERT(lbport != nullptr);
            NPNR_ASSERT(ubport != nullptr);

            auto stamp_equation = [&](PortRef &var, PortRef &eqn, double weight) {
                if (cell2var.at(eqn.cell->udata) == -1)
                    return;
                int row = cell2var.at(eqn.cell->udata);
                int v_pos = cell_pos(var.cell, yaxis);
                if (cell2var.at(var.cell->udata) != -1) {
                    eq.add_coeff(row, cell2var.at(var.cell->udata), weight);
                } else {
                    eq.add_rhs(row, -v_pos * weight);
                }
                eq.add_rhs(row, -cell_offset(var.cell, yaxis) * weight);
            };

            // Add all relevant connections to the matrix
            foreach_port(ni, [&](PortRef &port, int user_idx) {
                int this_pos = cell_pos(port.cell, yaxis);
                auto process_arc = [&](PortRef *other) {
                    if (other == &port)
                        return;
                    int o_pos = cell_pos(other->cell, yaxis);
                    double weight = 1.0 / (ni->users.size() * std::max<double>(1, std::abs(o_pos - this_pos)));

                    // If cell 0 is not fixed, it will stamp +w on its equation and -w on the other end's equation,
                    // if the other end isn't fixed
                    stamp_equation(port, port, weight);
                    stamp_equation(port, *other, -weight);
                    stamp_equation(*other, *other, weight);
                    stamp_equation(*other, port, -weight);
                };
                process_arc(lbport);
                process_arc(ubport);
            });
        }
    };
    auto solve_equations = [&](bool yaxis) {
        auto &eq = yaxis ? eqy : eqx;
        std::vector<double> vals;
        for (int i = 0; i < GetSize(cell2var); ++i) {
            int var = cell2var.at(i);
            if (var == -1)
                continue;
            auto &c = cells.at(i);
            vals.push_back(yaxis ? c.solver_y : c.solver_x);
        }
        eq.solve(vals, tol);
        for (int i = 0; i < GetSize(cell2var); ++i) {
            int var = cell2var.at(i);
            if (var == -1)
                continue;
            auto &c = cells.at(i);
            if (yaxis) {
                c.solver_y = vals.at(var);
                c.placed_y = std::max(0, std::min(d.height - 1, int(c.solver_y)));
            } else {
                c.solver_x = vals.at(var);
                c.placed_x = std::max(0, std::min(d.width - 1, int(c.solver_x)));
            }
        }
    };
    // Initial setup
    for (auto &cell : cells) {
        cell.solver_x = cell.placed_x;
        cell.solver_y = cell.placed_y;
    }
    // Main loop
    for (int iter = 0; iter < iters; ++iter) {
        // FIXME: parallelise
        build_equations(false);
        build_equations(true);
        solve_equations(false);
        solve_equations(true);
    }
}

void RippleFPGAPlacer::setup_spreader_grid()
{
    for (auto loc : grid) {
        for (auto &type : loc.value.per_type) {
            type.avail_area = 0;
            type.cell_area = 0;
        }
    }
    for (auto st : site_types) {
        int idx = sitetype_to_idx.at(st);
        if (d.site_locations.count(st)) {
            // Overriden location grid
            for (auto l : d.site_locations.at(st))
                grid.at(l).per_type.at(idx).avail_area += 1.0;
        } else {
            // Assume this type has a 1:1 mapping with Bels
            for (BelId b : ctx->getBels()) {
                if (ctx->getBelType(b) != st)
                    continue;
                if (!ctx->checkBelAvail(b))
                    continue;
                Loc l = ctx->getBelLocation(b);
                grid.at(l).per_type.at(idx).avail_area += 1.0;
            }
        }
    }
    for (auto &cell : cells) {
        if (cell.locked)
            continue;
        cell.area = 0;
        for (auto &sc : cell.base_cells) {
            IdString type = sc.ci->type;
            if (d.celltype_to_sitetype.count(type))
                type = d.celltype_to_sitetype.at(type);
            int type_idx = sitetype_to_idx.at(type);
            float area = f->getCellArea(sc.ci);
            cell.area += area;
            grid.at(cell.placed_x + sc.offset_x, cell.placed_y + sc.offset_y).per_type.at(type_idx).cell_area += area;
        }
    }
}

} // namespace Ripple

NEXTPNR_NAMESPACE_END
