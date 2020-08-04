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

#include "log.h"
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
                int row = cell2var.at(cell_index.at(eqn.cell->udata).cell);
                if (row == -1)
                    return;
                int col = cell2var.at(cell_index.at(var.cell->udata).cell);
                int v_pos = cell_pos(var.cell, yaxis);
                if (col != -1) {
                    eq.add_coeff(row, col, weight);
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
        eqx.reset();
        eqy.reset();
        build_equations(false);
        build_equations(true);
        solve_equations(false);
        solve_equations(true);
    }
#if 0
    for (auto &cell : cells) {
        log_info("%s: (%d, %d)\n", ctx->nameOf(cell.base_cells[0].ci), cell.placed_x, cell.placed_y);
    }
#endif
}

void RippleFPGAPlacer::setup_spreader_grid()
{

    for (auto &cell : cells) {
        IdString site_type;
        if (d.celltype_to_sitetype.count(cell.type))
            site_type = d.celltype_to_sitetype.at(cell.type);
        else
            site_type = cell.type;
        if (!sitetype_to_idx.count(site_type)) {
            sitetype_to_idx[site_type] = GetSize(site_types);
            site_types.push_back(site_type);
            spread_site_data.emplace_back();
        }
    }
    for (auto loc : grid) {
        loc.value.per_type.resize(GetSize(site_types));

        for (auto &type : loc.value.per_type) {
            type.avail_area = 0;
            type.cell_area = 0;
            type.overfull = false;
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

void RippleFPGAPlacer::setup_spreader_bins(int bin_w, int bin_h)
{
    this->bin_w = bin_w;
    this->bin_h = bin_h;
    int nx = (d.width + (bin_w - 1)) / bin_w;
    int ny = (d.height + (bin_h - 1)) / bin_h;
    for (size_t i = 0; i < spread_site_data.size(); i++) {
        auto &s = spread_site_data.at(i);
        s.avail_area = 0;
        s.cell_area = 0;
        s.target_area = 0;
        s.bins.reset(nx, ny);
        for (auto loc : grid) {
            auto &l = loc.value.per_type.at(i);
            s.avail_area += l.avail_area;
            s.cell_area += l.cell_area;
            s.target_area += l.target_area();
            auto &bin = s.bins.at(loc.x / bin_w, loc.y / bin_h);
            bin.avail_area += l.avail_area;
            bin.target_density += l.target_density / double(bin_w * bin_h);
            bin.cell_area += l.cell_area;
        }

        double scale = 1.0;
        if (s.target_area <= s.cell_area) {
            // Target area is too low, need to increase target density
            scale = s.cell_area / s.target_area;
        }

        // Update per-bin target area
        for (auto bin_kv : s.bins) {
            auto &bin = bin_kv.value;
            if ((bin.target_area * scale) > bin.avail_area)
                bin.target_area = bin.avail_area;
            else
                bin.target_area = bin.avail_area * bin.target_density * scale;
        }
    }
    for (auto cell_entry : cells.enumerate()) {
        auto &c = cell_entry.value;
        if (c.locked)
            continue;
        IdString type = c.type;
        if (d.celltype_to_sitetype.count(type))
            type = d.celltype_to_sitetype.at(type);
        int type_idx = sitetype_to_idx.at(type);
        auto &s = spread_site_data.at(type_idx);
        for (auto sc_entry : c.base_cells.enumerate()) {
            auto &sc = sc_entry.value;
            int bx = (c.placed_x + sc.offset_x) / bin_w;
            int by = (c.placed_y + sc.offset_y) / bin_h;
            auto &bin = s.bins.at(bx, by);
            bin.placed_cells.emplace_back(cell_entry.index, sc_entry.index);
        }
    }
}

void RippleFPGAPlacer::reset_spread_cell_areas(int site_type, int x0, int y0, int x1, int y1)
{
    auto &s = spread_site_data.at(site_type);
    for (int x = x0; x <= x1; x++) {
        for (int y = y0; y <= y1; y++) {
            s.bins.at(x, y).cell_area = 0.0;
            s.bins.at(x, y).placed_cells.clear();
        }
    }
}
void RippleFPGAPlacer::update_spread_cell_area(int site_type, int x0, int y0, int x1, int y1)
{
    std::set<int> update_cells;
    auto &s = spread_site_data.at(site_type);

    for (int x = x0; x <= x1; x++) {
        for (int y = y0; y <= y1; y++) {
            for (auto c : s.bins.at(x, y).placed_cells)
                update_cells.insert(c.cell);
        }
    }
    reset_spread_cell_areas(site_type, x0, y0, x1, y1);

    for (auto cell : update_cells) {
        auto &c = cells.at(cell);
        IdString type = c.type;
        if (d.celltype_to_sitetype.count(type))
            type = d.celltype_to_sitetype.at(type);
        int type_idx = sitetype_to_idx.at(type);
        for (auto sc_entry : c.base_cells.enumerate()) {
            auto &sc = sc_entry.value;
            int bx = (c.placed_x + sc.offset_x) / bin_w;
            int by = (c.placed_y + sc.offset_y) / bin_h;
            if (bx < x0 || bx > x1 || by < y0 || by > y1)
                continue;
            auto &bin = spread_site_data.at(type_idx).bins.at(bx, by);
            bin.cell_area += f->getCellArea(sc.ci);
            bin.placed_cells.emplace_back(cell, sc_entry.index);
        }
    }
}

void RippleFPGAPlacer::find_overfilled_regions()
{
    for (auto &s : spread_site_data) {
        s.overfull.clear();
        for (auto bin_kv : s.bins) {
            auto &bin = bin_kv.value;
            if (bin.cell_area <= bin.target_area) {
                bin.is_overfull = false;
                continue;
            }
            bin.is_overfull = true;
            s.overfull.emplace_back(bin_kv.x, bin_kv.y);
            auto &of = s.overfull.back();
            of.target_overutil_area = bin.cell_area - bin.target_area;
            of.strict_overutil_area = bin.cell_area - bin.avail_area;
            of.expanded_cell_area = bin.cell_area;
            of.expanded_avail_area = bin.avail_area;
            of.expanded_target_area = bin.target_area;
            if (bin.target_area < 1.0) {
                of.target_overutil_ratio = of.target_overutil_area;
                of.strict_overutil_ratio = of.strict_overutil_area;
            } else {
                of.target_overutil_ratio = of.target_overutil_area / bin.target_area;
                of.strict_overutil_ratio = of.strict_overutil_area / bin.avail_area;
            }
        }
    }
}

void RippleFPGAPlacer::expand_overfilled_region(int st, OverfilledRegion &of)
{
    auto &bin = spread_site_data.at(st).bins.at(of.cx, of.cy);

    int nx = spread_site_data.at(st).bins.width();
    int ny = spread_site_data.at(st).bins.height();

    of.x0 = of.x1 = of.cx;
    of.y0 = of.y1 = of.cy;
    of.expanded_target_area = bin.target_area;
    of.expanded_avail_area = bin.avail_area;
    of.expanded_cell_area = bin.cell_area;

    enum
    {
        LEFT = 0,
        DOWN,
        RIGHT,
        UP
    } direction = LEFT;
    double box_w = 1.0, box_h = 1.0;
    double dx_ratio = 0.0, dy_ratio = 0.0, dx_cost = 0.0, dy_cost = 0.0;
    bool dis_box_ratio = (expand_box_ratio == 0.0);
    while (of.expanded_cell_area > of.expanded_target_area) {
        if (!dis_box_ratio) {
            dx_ratio = (box_w + 1.0) / box_h;
            dy_ratio = box_w / (box_h + 1.0);
            dx_cost = (dx_ratio < expand_box_ratio) ? (expand_box_ratio / dx_ratio) : (dx_ratio / expand_box_ratio);
            dy_cost = (dy_ratio < expand_box_ratio) ? (expand_box_ratio / dy_ratio) : (dy_ratio / expand_box_ratio);
        }
        switch (direction) {
        case LEFT:
            if (of.x0 > 0 && (dis_box_ratio || (dx_cost <= dy_cost))) {
                of.x0--;
                box_w += 1.0;
                for (int y = of.y0; y <= of.y1; y++) {
                    auto &bin2 = spread_site_data.at(st).bins.at(of.x0, y);
                    of.expanded_avail_area += bin2.avail_area;
                    of.expanded_target_area += bin2.target_area;
                    of.expanded_cell_area += bin2.cell_area;
                }
            }
            direction = DOWN;
            break;
        case DOWN:
            if (of.y0 > 0 && (dis_box_ratio || (dy_cost <= dx_cost))) {
                of.y0--;
                box_h += 1.0;
                for (int x = of.x0; x <= of.x1; x++) {
                    auto &bin2 = spread_site_data.at(st).bins.at(x, of.y0);
                    of.expanded_avail_area += bin2.avail_area;
                    of.expanded_target_area += bin2.target_area;
                    of.expanded_cell_area += bin2.cell_area;
                }
            }
            direction = RIGHT;
            break;
        case RIGHT:
            if (of.x1 < (nx - 1) && (dis_box_ratio || (dx_cost <= dy_cost))) {
                of.x1++;
                box_w += 1.0;
                for (int y = of.y0; y <= of.y1; y++) {
                    auto &bin2 = spread_site_data.at(st).bins.at(of.x1, y);
                    of.expanded_avail_area += bin2.avail_area;
                    of.expanded_target_area += bin2.target_area;
                    of.expanded_cell_area += bin2.cell_area;
                }
            }
            direction = UP;
            break;
        case UP:
            if (of.y1 < (ny - 1) && (dis_box_ratio || (dy_cost <= dx_cost))) {
                of.y1++;
                box_h += 1.0;
                for (int x = of.x0; x <= of.x1; x++) {
                    auto &bin2 = spread_site_data.at(st).bins.at(x, of.y1);
                    of.expanded_avail_area += bin2.avail_area;
                    of.expanded_target_area += bin2.target_area;
                    of.expanded_cell_area += bin2.cell_area;
                }
            }
            direction = LEFT;
            break;
        }
        // log_info("spread progress %f / %f\n", of.expanded_cell_area, of.expanded_target_area);
        bool fully_expanded_x = (of.x0 == 0 && of.x1 == (nx - 1));
        bool fully_expanded_y = (of.y0 == 0 && of.y1 == (ny - 1));
        if (dis_box_ratio) {
            if (fully_expanded_x && fully_expanded_y)
                break;
        } else {
            if (fully_expanded_x || fully_expanded_y)
                break;
        }
        if ((of.expanded_avail_area / (spread_site_data.at(st).avail_area)) > expand_box_limit)
            break;
    }
}

bool RippleFPGAPlacer::spread_cells(int site_type, SpreaderBox &box, std::vector<SpreaderBox> &spread_boxes)
{
    auto &s = spread_site_data.at(site_type);
    bool ydir = (box.dir == SpreaderBox::VERT);

    // Direction-independent start/endpoints
    int start = ydir ? box.y0 : box.x0;
    int end = ydir ? box.y1 : box.x1;

    int L = (end - start) + 1;

    if (L < min_expand_size)
        return false;

    // Sort input cells
    std::sort(box.spread_cells.begin(), box.spread_cells.end(), [&](int cell0, int cell1) {
        auto &c0 = cells.at(cell0);
        auto &c1 = cells.at(cell1);
        return ydir ? (c0.placed_y < c1.placed_y) : (c0.placed_x < c1.placed_x);
    });

    std::vector<double> target_areas(L, 0);
    double total_area = 0;
    // Bounds of new boxes
    int lo_start = start, lo_end = start;
    int hi_start = end, hi_end = end;

    // Determine areas for each 1-bin wide/tall 'slither'
    for (int x = box.x0; x <= box.x1; x++) {
        for (int y = box.y0; y <= box.y1; y++) {
            double area = s.bins.at(x, y).target_area;
            target_areas.at(ydir ? (y - box.y0) : (x - box.x0)) += area;
            total_area += area;
        }
    }
    // Move the box bounds to eliminate 'padding' with no useful sites at either end
    for (int i = start; (i <= end) && (lo_end < (hi_start - 1)); i++) {
        if (target_areas.at(i - start) > 0)
            break;
        ++lo_start;
        ++lo_end;
    }
    for (int i = end; (i >= start) && (hi_start > (lo_end + 1)); i--) {
        if (target_areas.at(i - start) > 0)
            break;
        --hi_start;
        --hi_end;
    }
    if (lo_end == hi_start) {
        // Not enough bins with non-zero useful area to do spreading
        return false;
    }
    // Search for cut point with even lo/hi area
    double lo_area = target_areas.at(lo_end - start);
    double hi_area = target_areas.at(hi_start - start);
    while ((hi_start - lo_end) > 1) {
        if (lo_area <= hi_area) {
            ++lo_end;
            lo_area += target_areas.at(lo_end - start);
        } else {
            --hi_start;
            hi_area += target_areas.at(hi_start - start);
        }
    }
    log_info("lo %d %d hi %d %d\n", lo_start, lo_end, hi_start, hi_end);
    log_info("lo_area=%f hi_area=%f\n", lo_area, hi_area);

    std::vector<int> lo_cells, hi_cells;
    cut_cells_by_area(box.spread_cells, lo_cells, hi_cells, lo_area / total_area);
    log_info("spread_cells=%d lo_cells=%d hi_cells=%d\n", GetSize(box.spread_cells), GetSize(lo_cells),
             GetSize(hi_cells));

    auto spread_cut_cells = [&](const std::vector<int> &box_cells, int box_start, int box_end, bool lo) {
        int cell_start = 0, cell_end = 0;
        for (int i = (lo ? box_end : box_start); lo ? (i >= box_start) : (i <= box_end); lo ? i-- : i++) {
            if (cell_start == GetSize(box_cells))
                continue;
            double accum_area = 0.0;
            int bin_cells = 0;
            cell_end = cell_start;
            auto &start_cell = cells.at(box_cells.at(cell_start));
            double orig_lo = ydir ? start_cell.solver_y : start_cell.solver_x;
            double orig_hi = orig_lo;
            // Determine number of cells in bin
            while (cell_end < GetSize(box_cells) &&
                   ((i == (lo ? box_start : box_end)) || accum_area < target_areas.at(i - start))) {
                auto &end_cell = cells.at(box_cells.at(cell_end));
                accum_area += end_cell.area;
                if (lo)
                    orig_lo = ydir ? end_cell.solver_y : end_cell.solver_x;
                else
                    orig_hi = ydir ? end_cell.solver_y : end_cell.solver_x;
                bin_cells++;
                cell_end++;
            }
            // Spread cells evenly based on original solver positions
            if (bin_cells > 0) {
                double spread_lo = ydir ? ((i * bin_h) + (bin_h / double(bin_cells * 2.0)))
                                        : ((i * bin_w) + (bin_w / double(bin_cells * 2.0)));
                double spread_hi = spread_lo + (ydir ? (bin_h - (bin_h / double(bin_cells)))
                                                     : (bin_w - (bin_w / double(bin_cells))));
                double spread_scale = (spread_hi - spread_lo) / std::max<double>(orig_hi - orig_lo, 1.0);
                for (int j = cell_start; j < cell_end; j++) {
                    auto &c = cells.at(box_cells.at(j));
                    if (ydir) {
                        c.solver_y = spread_lo + (c.solver_y - orig_lo) * spread_scale;
                        c.placed_y = std::min(std::max(int(c.solver_y), 0), d.height);
                    } else {
                        c.solver_x = spread_lo + (c.solver_x - orig_lo) * spread_scale;
                        c.placed_x = std::min(std::max(int(c.solver_x), 0), d.width);
                    }
                }
            }
            cell_start = cell_end;
        }
    };
    spread_cut_cells(lo_cells, lo_start, lo_end, true);
    spread_cut_cells(hi_cells, hi_start, hi_end, false);

    spread_boxes.emplace_back(ydir ? box.x0 : lo_start, ydir ? lo_start : box.y0, ydir ? box.x1 : lo_end,
                              ydir ? lo_end : box.y1, ydir ? SpreaderBox::HORIZ : SpreaderBox::VERT, box.level - 1);
    std::copy(lo_cells.begin(), lo_cells.end(), std::back_inserter(spread_boxes.back().spread_cells));
    spread_boxes.emplace_back(ydir ? box.x0 : hi_start, ydir ? hi_start : box.y0, ydir ? box.x1 : hi_end,
                              ydir ? hi_end : box.y1, ydir ? SpreaderBox::HORIZ : SpreaderBox::VERT, box.level - 1);
    std::copy(hi_cells.begin(), hi_cells.end(), std::back_inserter(spread_boxes.back().spread_cells));

    return true;
}

void RippleFPGAPlacer::spread_cells_in_region(int site_type, OverfilledRegion &of)
{
    auto &s = spread_site_data.at(site_type);

    expand_overfilled_region(site_type, of);
    const int max_level = 1000;
    SpreaderBox sbox(of.x0, of.y0, of.x1, of.y1, SpreaderBox::HORIZ, max_level);
    std::set<int> placed_cells;
    for (int x = of.x0; x <= of.x1; x++) {
        for (int y = of.y0; y <= of.y1; y++) {
            auto &l = s.bins.at(x, y);
            // Mark overfilling as resolved
            l.is_overfull = false;
            // Add cells from this location
            for (auto i : l.placed_cells) {
                auto &c = cells.at(i.cell);
                int cx = c.placed_x / bin_w;
                int cy = c.placed_y / bin_h;
                if (cx >= of.x0 && cx <= of.x1 && cy >= of.y0 && cy <= of.y1)
                    placed_cells.insert(i.cell);
            }
        }
    }

    for (auto i : placed_cells)
        sbox.spread_cells.push_back(i);

    std::queue<SpreaderBox> boxes;
    std::vector<SpreaderBox> split;
    boxes.push(std::move(sbox));
    while (!boxes.empty()) {
        auto curr = std::move(boxes.front());
#if 1
        log_info("x0 %d y0 %d x1 %d y1 %d\n", curr.x0, curr.y0, curr.x1, curr.y1);
#endif
        boxes.pop();
        split.clear();
        if (((curr.x1 - curr.x0) < (min_expand_size - 1)) || ((curr.y1 - curr.y0) < (min_expand_size - 1)))
            continue;
        bool success = false;
        auto orig_dir = curr.dir;
        auto other_dir = (orig_dir == SpreaderBox::HORIZ) ? SpreaderBox::VERT : SpreaderBox::HORIZ;
        // Try spreading in preferred direction
        success |= spread_cells(site_type, curr, split);
        if (!success) {
            // If it fails try spreading in the other direction
            curr.dir = other_dir;
            success |= spread_cells(site_type, curr, split);
        }
        if (success) {
            for (auto &r : split) {
                r.dir = other_dir;
                // Add result to queue
                log_info("   add to queue: %d %d %d %d, %d, %d\n", r.x0, r.y0, r.x1, r.y1, r.level,
                         GetSize(r.spread_cells));
                if (!r.spread_cells.empty() && r.level > 0)
                    boxes.push(r);
            }
        } else {
#if 1
            log_info("***** failed\n");
#endif
        }
    }
    // Update placements
    update_spread_cell_area(site_type, of.x0, of.y0, of.x1, of.y1);
}

void RippleFPGAPlacer::cut_cells_by_area(const std::vector<int> &cells_in, std::vector<int> &lo, std::vector<int> &hi,
                                         double ratio)
{
    double total_area = 0;
    for (int c : cells_in)
        total_area += cells.at(c).area;
    double half_area = total_area * ratio;
    double accum_area = 0;

    lo.reserve(GetSize(cells_in) / 2);
    hi.reserve(GetSize(cells_in) / 2);

    for (int c : cells_in) {
        if (accum_area > half_area)
            hi.push_back(c);
        else
            lo.push_back(c);
        accum_area += cells.at(c).area;
    }
    std::reverse(lo.begin(), lo.end());
}

void RippleFPGAPlacer::upper_bound_spread()
{
    setup_spreader_grid();
    setup_spreader_bins(2, 2);
    find_overfilled_regions();
    for (int i = 0; i < GetSize(spread_site_data); i++) {
        auto &st = spread_site_data.at(i);
        for (auto &of : st.overfull) {
            log_info("spreading cells of type '%s' in overfull region (%d, %d, %d, %d)\n",
                     ctx->nameOf(site_types.at(i)), of.x0, of.y0, of.x1, of.y1);
            spread_cells_in_region(i, of);
        }
    }
}

int RippleFPGAPlacer::total_hpwl()
{
    int hpwl = 0;
    for (auto net : sorted(ctx->nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell == nullptr)
            continue;
        Bounds bb;
        Loc l = get_cell_location(ni->driver.cell);
        bb.expand(l);
        for (auto &usr : ni->users) {
            bb.expand(get_cell_location(usr.cell));
        }
        hpwl += bb.hpwl();
    }
    return hpwl;
}

void RippleFPGAPlacer::place_global(int stage)
{
    for (int iter = 0; iter < 10; iter++) {
        lower_bound_solver(1e-5, 0.2, 10);
        log_info("iter %d lower-bound HPWL: %d\n", iter, total_hpwl());
        upper_bound_spread();
        log_info("iter %d upper-bound HPWL: %d\n", iter, total_hpwl());
    }
}

} // namespace Ripple

NEXTPNR_NAMESPACE_END
