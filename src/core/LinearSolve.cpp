#include "LinearSolve.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace montage {

void symmetricEigen(std::vector<double> a, int n, std::vector<double>& values, std::vector<double>& vecs) {
    std::vector<double> v(size_t(n) * n, 0);
    for (int i = 0; i < n; ++i) v[size_t(i) * n + i] = 1;
    double total = 0;
    for (double x : a) total += x * x;
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += a[size_t(p) * n + q] * a[size_t(p) * n + q];
        if (off <= 1e-26 * total) break;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[size_t(p) * n + q];
                if (std::fabs(apq) < 1e-300) continue;
                const double theta = (a[size_t(q) * n + q] - a[size_t(p) * n + p]) / (2 * apq);
                const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < n; ++k) {
                    double& kp = a[size_t(k) * n + p];
                    double& kq = a[size_t(k) * n + q];
                    const double x = kp, y = kq;
                    kp = c * x - s * y;
                    kq = s * x + c * y;
                }
                for (int k = 0; k < n; ++k) {
                    double& pk = a[size_t(p) * n + k];
                    double& qk = a[size_t(q) * n + k];
                    const double x = pk, y = qk;
                    pk = c * x - s * y;
                    qk = s * x + c * y;
                }
                for (int k = 0; k < n; ++k) {
                    double& kp = v[size_t(k) * n + p];
                    double& kq = v[size_t(k) * n + q];
                    const double x = kp, y = kq;
                    kp = c * x - s * y;
                    kq = s * x + c * y;
                }
            }
    }
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int i, int j) { return a[size_t(i) * n + i] < a[size_t(j) * n + j]; });
    values.resize(n);
    vecs.assign(size_t(n) * n, 0);
    for (int k = 0; k < n; ++k) {
        values[k] = a[size_t(order[k]) * n + order[k]];
        for (int i = 0; i < n; ++i) vecs[size_t(i) * n + k] = v[size_t(i) * n + order[k]];
    }
}

bool choleskySolve(std::vector<double>& a, int n, std::vector<double>& b) {
    for (int j = 0; j < n; ++j) {
        double d = a[size_t(j) * n + j];
        const double* rj = &a[size_t(j) * n];
        for (int k = 0; k < j; ++k) d -= rj[k] * rj[k];
        if (!(d > 0)) return false;
        d = std::sqrt(d);
        a[size_t(j) * n + j] = d;
        for (int i = j + 1; i < n; ++i) {
            double* ri = &a[size_t(i) * n];
            double s = ri[j];
            for (int k = 0; k < j; ++k) s -= ri[k] * rj[k];
            ri[j] = s / d;
        }
    }
    for (int i = 0; i < n; ++i) {
        double s = b[i];
        for (int k = 0; k < i; ++k) s -= a[size_t(i) * n + k] * b[k];
        b[i] = s / a[size_t(i) * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = b[i];
        for (int k = i + 1; k < n; ++k) s -= a[size_t(k) * n + i] * b[k];
        b[i] = s / a[size_t(i) * n + i];
    }
    return true;
}

bool SparseCholesky::factor(int n, const std::vector<Entry>& entries) {
    n_ = n;
    perm_.assign(size_t(n), 0);
    first_.assign(size_t(n), 0);
    start_.assign(size_t(n) + 1, 0);
    l_.clear();
    if (n <= 0) return n == 0;
    // Neighbours, then a reverse Cuthill–McKee order: breadth first from a low-degree row of each part, neighbours by
    // degree, reversed.
    std::vector<std::vector<int>> adj(static_cast<size_t>(n));
    for (const Entry& e : entries)
        if (e.row != e.col) {
            adj[size_t(e.row)].push_back(e.col);
            adj[size_t(e.col)].push_back(e.row);
        }
    for (auto& a : adj) {
        std::sort(a.begin(), a.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
    }
    std::vector<int> order, byDegree(static_cast<size_t>(n));
    order.reserve(size_t(n));
    std::iota(byDegree.begin(), byDegree.end(), 0);
    std::stable_sort(byDegree.begin(), byDegree.end(), [&](int a, int b) { return adj[size_t(a)].size() < adj[size_t(b)].size(); });
    std::vector<char> seen(static_cast<size_t>(n), 0);
    for (int root : byDegree) {
        if (seen[size_t(root)]) continue;
        // (a far end of the part: the last row reached from the root, a few times over)
        int start = root;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<int> level{start};
            std::vector<char> in(size_t(n), 0);
            in[size_t(start)] = 1;
            for (size_t i = 0; i < level.size(); ++i)
                for (int nb : adj[size_t(level[i])])
                    if (!in[size_t(nb)]) in[size_t(nb)] = 1, level.push_back(nb);
            start = level.back();
        }
        const size_t from = order.size();
        order.push_back(start);
        seen[size_t(start)] = 1;
        for (size_t i = from; i < order.size(); ++i) {
            std::vector<int> next;
            for (int nb : adj[size_t(order[i])])
                if (!seen[size_t(nb)]) seen[size_t(nb)] = 1, next.push_back(nb);
            std::stable_sort(next.begin(), next.end(), [&](int a, int b) { return adj[size_t(a)].size() < adj[size_t(b)].size(); });
            order.insert(order.end(), next.begin(), next.end());
        }
    }
    std::reverse(order.begin(), order.end());
    std::vector<int> inv(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) perm_[size_t(i)] = order[size_t(i)], inv[size_t(order[size_t(i)])] = i;
    // Each row's envelope, then the entries placed in it.
    for (int i = 0; i < n; ++i) first_[size_t(i)] = i;
    for (const Entry& e : entries) {
        const int a = inv[size_t(e.row)], b = inv[size_t(e.col)];
        const int r = std::max(a, b), c = std::min(a, b);
        first_[size_t(r)] = std::min(first_[size_t(r)], c);
    }
    for (int i = 0; i < n; ++i) start_[size_t(i) + 1] = start_[size_t(i)] + size_t(i - first_[size_t(i)] + 1);
    l_.assign(start_[size_t(n)], 0.0);
    auto at = [&](int r, int c) -> double& { return l_[start_[size_t(r)] + size_t(c - first_[size_t(r)])]; };
    for (const Entry& e : entries) {
        const int a = inv[size_t(e.row)], b = inv[size_t(e.col)];
        at(std::max(a, b), std::min(a, b)) += e.value;
    }
    // Cholesky, row by row within the envelope.
    for (int i = 0; i < n; ++i) {
        const int fi = first_[size_t(i)];
        double* li = &l_[start_[size_t(i)]];  // li[k - fi] is column k of row i
        for (int j = fi; j <= i; ++j) {
            const int fj = first_[size_t(j)];
            const double* lj = &l_[start_[size_t(j)]];
            double s = li[j - fi];
            for (int k = std::max(fi, fj); k < j; ++k) s -= li[k - fi] * lj[k - fj];
            if (j < i) li[j - fi] = s / lj[j - fj];
            else {
                if (!(s > 0)) return false;
                li[i - fi] = std::sqrt(s);
            }
        }
    }
    return true;
}

void SparseCholesky::solve(std::vector<double>& b) const {
    if (int(b.size()) != n_ || n_ == 0) return;
    std::vector<double> y(static_cast<size_t>(n_));
    for (int i = 0; i < n_; ++i) y[size_t(i)] = b[size_t(perm_[size_t(i)])];
    for (int i = 0; i < n_; ++i) {
        const int fi = first_[size_t(i)];
        const double* li = &l_[start_[size_t(i)]];
        double s = y[size_t(i)];
        for (int k = fi; k < i; ++k) s -= li[k - fi] * y[size_t(k)];
        y[size_t(i)] = s / li[i - fi];
    }
    for (int i = n_ - 1; i >= 0; --i) {
        const int fi = first_[size_t(i)];
        const double* li = &l_[start_[size_t(i)]];
        y[size_t(i)] /= li[i - fi];
        const double xi = y[size_t(i)];
        for (int k = fi; k < i; ++k) y[size_t(k)] -= li[k - fi] * xi;
    }
    for (int i = 0; i < n_; ++i) b[size_t(perm_[size_t(i)])] = y[size_t(i)];
}

}  // namespace montage
