// Montage — small dense linear algebra shared by the solvers (camera solve, lens calibration): a symmetric matrix's
// eigen decomposition and a symmetric positive definite system solved by Cholesky. Matrices are row-major n x n.
// And a sparse symmetric positive definite matrix factored once and solved for many right-hand sides (the mesh warp).
#pragma once

#include <cstddef>
#include <vector>

namespace montage {

// Eigenvalues (ascending) and eigenvectors (column k of `vecs`, row-major n x n) of the symmetric n x n matrix `a`
// (cyclic Jacobi).
void symmetricEigen(std::vector<double> a, int n, std::vector<double>& values, std::vector<double>& vecs);
// Solves a x = b for symmetric positive definite a (Cholesky, in place: `b` gets x). False when a is not.
bool choleskySolve(std::vector<double>& a, int n, std::vector<double>& b);

// A sparse symmetric positive definite matrix's Cholesky factor, kept in its envelope (the band each row reaches
// from its first nonzero) after a reverse Cuthill–McKee ordering, which keeps that band narrow for a mesh.
class SparseCholesky {
public:
    struct Entry {
        int row = 0, col = 0;
        double value = 0;
    };
    // Factors the n x n matrix whose entries are given (summed where repeated; an off-diagonal one given once, with
    // either index first, stands for both). False when the matrix is not positive definite.
    bool factor(int n, const std::vector<Entry>& entries);
    // Solves A x = b in place (b gets x).
    void solve(std::vector<double>& b) const;
    int size() const { return n_; }
    size_t stored() const { return l_.size(); }  // entries kept in the factor's envelope

private:
    int n_ = 0;
    std::vector<int> perm_;     // perm_[new] = old
    std::vector<int> first_;    // the first column of each row's envelope (new order)
    std::vector<size_t> start_; // where each row's envelope begins in l_
    std::vector<double> l_;
};

}  // namespace montage
