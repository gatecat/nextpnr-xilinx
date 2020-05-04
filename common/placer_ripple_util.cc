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
 */

// Some data structures to support the RippleFPGA placer

#include "placer_ripple_util.h"

#include <Eigen/Core>
#include <Eigen/IterativeLinearSolvers>

NEXTPNR_NAMESPACE_BEGIN

namespace Ripple {
void EquationSystem::solve(std::vector<double> &x, float tolerance)
{
    using namespace Eigen;
    if (x.empty())
        return;
    NPNR_ASSERT(x.size() == A.size());

    VectorXd vx(x.size()), vb(rhs.size());
    SparseMatrix<double> mat(A.size(), A.size());

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

    ConjugateGradient<SparseMatrix<double>, Lower | Upper> solver;
    solver.setTolerance(tolerance);
    VectorXd xr = solver.compute(mat).solveWithGuess(vb, vx);
    for (int i = 0; i < int(x.size()); i++)
        x.at(i) = xr[i];
    // for (int i = 0; i < int(x.size()); i++)
    //    log_info("x[%d] = %f\n", i, x.at(i));
}
} // namespace Ripple

NEXTPNR_NAMESPACE_END