// Montage — small dense linear algebra shared by the solvers (camera solve, lens calibration): a symmetric matrix's
// eigen decomposition and a symmetric positive definite system solved by Cholesky. Matrices are row-major n x n.
#pragma once

#include <vector>

namespace montage {

// Eigenvalues (ascending) and eigenvectors (column k of `vecs`, row-major n x n) of the symmetric n x n matrix `a`
// (cyclic Jacobi).
void symmetricEigen(std::vector<double> a, int n, std::vector<double>& values, std::vector<double>& vecs);
// Solves a x = b for symmetric positive definite a (Cholesky, in place: `b` gets x). False when a is not.
bool choleskySolve(std::vector<double>& a, int n, std::vector<double>& b);

}  // namespace montage
