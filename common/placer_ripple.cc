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
#define WITH_RIPPLEFPGA
#ifdef WITH_RIPPLEFPGA

#include <Eigen/Core>
#include <Eigen/IterativeLinearSolvers>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/find_flow_cost.hpp>
#include <boost/graph/successive_shortest_path_nonnegative_weights.hpp>
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
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
// A simple internal representation for a sparse system of equations Ax = rhs
// This is designed to decouple the functions that build the matrix to the engine that
// solves it, and the representation that requires
template <typename T> struct EquationSystem
{

    EquationSystem(size_t rows, size_t cols)
    {
        A.resize(cols);
        rhs.resize(rows);
    }

    // Simple sparse format, easy to convert to CCS for solver
    std::vector<std::vector<std::pair<int, T>>> A; // col -> (row, x[row, col]) sorted by row
    std::vector<T> rhs;                            // RHS vector
    void reset()
    {
        for (auto &col : A)
            col.clear();
        std::fill(rhs.begin(), rhs.end(), T());
    }

    void add_coeff(int row, int col, T val)
    {
        auto &Ac = A.at(col);
        // Binary search
        int b = 0, e = int(Ac.size()) - 1;
        while (b <= e) {
            int i = (b + e) / 2;
            if (Ac.at(i).first == row) {
                Ac.at(i).second += val;
                return;
            }
            if (Ac.at(i).first > row)
                e = i - 1;
            else
                b = i + 1;
        }
        Ac.insert(Ac.begin() + b, std::make_pair(row, val));
    }

    void add_rhs(int row, T val) { rhs[row] += val; }

    void solve(std::vector<T> &x, float tolerance)
    {
        using namespace Eigen;
        if (x.empty())
            return;
        NPNR_ASSERT(x.size() == A.size());

        VectorXd vx(x.size()), vb(rhs.size());
        SparseMatrix<T> mat(A.size(), A.size());

        std::vector<int> colnnz;
        for (auto &Ac : A)
            colnnz.push_back(int(Ac.size()));
        mat.reserve(colnnz);
        for (int col = 0; col < int(A.size()); col++) {
            auto &Ac = A.at(col);
            for (auto &el : Ac)
                mat.insert(el.first, col) = el.second;
        }

        for (int i = 0; i < int(x.size()); i++)
            vx[i] = x.at(i);
        for (int i = 0; i < int(rhs.size()); i++)
            vb[i] = rhs.at(i);

        ConjugateGradient<SparseMatrix<T>, Lower | Upper> solver;
        solver.setTolerance(tolerance);
        VectorXd xr = solver.compute(mat).solveWithGuess(vb, vx);
        for (int i = 0; i < int(x.size()); i++)
            x.at(i) = xr[i];
        // for (int i = 0; i < int(x.size()); i++)
        //    log_info("x[%d] = %f\n", i, x.at(i));
    }
};

struct Bounds
{
    int x0, x1, y0, y1;
};

// We create our own simple netlist structures, to account for packing and other transformations as well as efficient
// handling of macros like carry chains as a single block
struct RippleCellPort
{
    std::vector<PortRef> orig_ports;
    PortType dir;
    NetInfo *net;
};
struct RippleCell
{
    int index;
    std::vector<CellInfo *> base_cells;
    IdString type;
    bool is_macro, is_packed;
    Bounds macro_extent;
    std::vector<RippleCellPort> ext_ports;

    int area;
    int area_scale;
    int chiplet;
    double solver_x, solver_y;
    int placed_x, placed_y;
    BelId root_bel;
};

struct Chiplet
{
    std::string name;
    int x0, y0, x1, y1;
};

struct PackedCellStructure
{
    IdString type_name;
    IdString root_bel_type;
    std::unordered_map<IdString, int> constituent_cells;
};

struct SiteLocation
{
    int x, y;
    int root_bel_z;
};

struct DeviceInfo
{
    int width, height;
    std::vector<Chiplet> chiplets;
    bool pack_bles, pack_clbs;
    PackedCellStructure ble_structure;
    PackedCellStructure clb_structure;

    std::vector<SiteLocation> bles;
    std::vector<SiteLocation> clbs;
};

class ArchFunctions
{
    virtual DeviceInfo getDeviceInfo() = 0;
    virtual double getCellArea(const CellInfo *cell) = 0;
    virtual double getBelArea(BelId bel) = 0;
    virtual bool checkBleCompatability(const std::vector<CellInfo *> &cells) = 0;
    virtual bool checkClbCompatability(const std::vector<CellInfo *> &cells) = 0;
    virtual ~ArchFunctions(){};
};

} // namespace

NEXTPNR_NAMESPACE_END

#endif