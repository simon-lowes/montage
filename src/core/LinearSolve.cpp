#include "LinearSolve.h"

#include <algorithm>
#include <cmath>

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

}  // namespace montage
