#pragma once
#include "io/edge_file.hpp"
#include "io/types.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#ifdef _OPENMP
    #include <omp.h>
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

struct KatzResult {
    std::vector<float> scores;     // Katz centrality, one value per node
    std::vector<double> residuals; // ||r||/||b|| after each iteration
    int iterations;
    double elapsed_ms;
};

namespace detail {

    inline double dot(const std::vector<float>& a, const std::vector<float>& b) {
        double s = 0.0;
#pragma omp parallel for reduction(+ : s) schedule(static)
        for (size_t i = 0; i < a.size(); ++i) {
            s += static_cast<double>(a[i]) * b[i];
        }
        return s;
    }

    inline void axpby(float alpha, const std::vector<float>& x,
                      float beta, std::vector<float>& y) {
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < y.size(); ++i) {
            y[i] = alpha * x[i] + beta * y[i];
        }
    }

    inline void xpaz(const std::vector<float>& x, float alpha,
                     const std::vector<float>& z, std::vector<float>& y) {
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < y.size(); ++i) {
            y[i] = x[i] + alpha * z[i];
        }
    }

    inline double norm2(const std::vector<float>& v) {
        return std::sqrt(dot(v, v));
    }

} // namespace detail

// Katz solver with two SpMV backends:
//   CSC    — col_ptr+row_idx in RAM, O(n+m) memory, parallel by column
//   Stream — mmap rev_edges.bin, O(n) memory, dst-sorted chunks for lock-free OMP
// Backend is selected automatically based on mem_budget_mb.

class KatzSolver {
public:
    KatzSolver(std::string rev_path,
               uint32_t n_nodes,
               uint64_t n_edges,
               size_t mem_budget_mb = 128)
        : rev_path_(std::move(rev_path))
        , n_(n_nodes)
        , m_(n_edges)
    {
        size_t csc_mb = ((n_ + 1) * 8 + m_ * 4) / (1024 * 1024);

        if (csc_mb <= mem_budget_mb) {
            use_csc_ = true;
            build_csc();
            printf("KatzSolver: using CSC (%zu MB, budget %zu MB)\n",
                   csc_mb, mem_budget_mb);
        } else {
            use_csc_ = false;
            init_streaming();
            printf("KatzSolver: using streaming (CSC would need %zu MB, budget %zu MB)\n",
                   csc_mb, mem_budget_mb);
        }
    }

    ~KatzSolver() {
        if (mmap_data_) {
            munmap(mmap_data_, mmap_len_);
        }
        if (mmap_fd_ >= 0) {
            close(mmap_fd_);
        }
    }

    KatzSolver(const KatzSolver&) = delete;
    KatzSolver& operator=(const KatzSolver&) = delete;

    KatzResult solve_power(float alpha, float tol = 1e-6f, int max_iter = 1000) const;
    KatzResult solve_bicgstab(float alpha, float tol = 1e-6f, int max_iter = 300) const;

private:
    std::vector<uint64_t> col_ptr_;
    std::vector<uint32_t> row_idx_;

    void build_csc() {
        auto t0 = std::chrono::steady_clock::now();

        col_ptr_.assign(n_ + 1, 0);
        row_idx_.resize(m_);

        read_edge_file(rev_path_, [&](const Edge* e, size_t k) {
            for (size_t i = 0; i < k; ++i) {
                col_ptr_[e[i].dst + 1]++;
            }
        });
        for (uint32_t j = 0; j < n_; ++j) {
            col_ptr_[j + 1] += col_ptr_[j];
        }

        std::vector<uint64_t> pos = col_ptr_;
        read_edge_file(rev_path_, [&](const Edge* e, size_t k) {
            for (size_t i = 0; i < k; ++i) {
                row_idx_[pos[e[i].dst]++] = e[i].src;
            }
        });

        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        printf("  CSC built in %.0f ms\n", ms);
    }

    void apply_matrix_csc(float alpha, const std::vector<float>& x,
                          std::vector<float>& out) const {
        const uint64_t* cp = col_ptr_.data();
        const uint32_t* ri = row_idx_.data();
        const float* xp = x.data();
        float* op = out.data();

#pragma omp parallel for schedule(static)
        for (uint32_t j = 0; j < n_; ++j) {
            float sum = 0.0f;
            for (uint64_t k = cp[j]; k < cp[j + 1]; ++k) {
                sum += xp[ri[k]];
            }
            op[j] = xp[j] - alpha * sum;
        }
    }

    // Streaming: rev_edges.bin mmap'd, split at dst-group boundaries → no write conflicts
    std::vector<uint64_t> chunk_start_;
    int n_threads_ = 1;
    int mmap_fd_ = -1;
    void* mmap_data_ = nullptr;
    size_t mmap_len_ = 0;

    void init_streaming() {
#ifdef _OPENMP
        n_threads_ = omp_get_max_threads();
#else
        n_threads_ = 1;
#endif
        if (n_threads_ <= 1 || m_ < 1000000) {
            n_threads_ = 1;
            chunk_start_ = {0, m_};
            return;
        }

        const size_t edge_size = sizeof(Edge);
        chunk_start_.assign(n_threads_ + 1, 0);
        chunk_start_[0] = 0;
        chunk_start_[n_threads_] = m_;

        FILE* f = fopen(rev_path_.c_str(), "rb");
        if (!f) {
            throw std::runtime_error("cannot open: " + rev_path_);
        }

        for (int t = 1; t < n_threads_; ++t) {
            uint64_t pos = m_ * t / n_threads_;
            if (pos >= m_) {
                chunk_start_[t] = m_;
                continue;
            }

            Edge e;
            fseek(f, (long)(pos * edge_size), SEEK_SET);
            if (fread(&e, edge_size, 1, f) != 1) {
                chunk_start_[t] = pos;
                continue;
            }
            uint32_t boundary_dst = e.dst;

            while (pos < m_) {
                fseek(f, (long)(pos * edge_size), SEEK_SET);
                if (fread(&e, edge_size, 1, f) != 1) {
                    break;
                }
                if (e.dst != boundary_dst) {
                    break;
                }
                ++pos;
            }
            chunk_start_[t] = pos;
        }
        fclose(f);

        // mmap the edge file
        mmap_fd_ = open(rev_path_.c_str(), O_RDONLY);
        if (mmap_fd_ >= 0) {
            mmap_len_ = m_ * sizeof(Edge);
            mmap_data_ = mmap(nullptr, mmap_len_, PROT_READ, MAP_PRIVATE,
                              mmap_fd_, 0);
            if (mmap_data_ == MAP_FAILED) {
                mmap_data_ = nullptr;
                close(mmap_fd_);
                mmap_fd_ = -1;
            } else {
                madvise(mmap_data_, mmap_len_, MADV_SEQUENTIAL);
            }
        }
    }

    void spmv_stream(const std::vector<float>& x, std::vector<float>& y) const {
        std::fill(y.begin(), y.end(), 0.0f);

        if (n_threads_ <= 1) {
            read_edge_file(rev_path_, [&](const Edge* c, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    y[c[i].dst] += x[c[i].src];
                }
            });
            return;
        }

        if (mmap_data_) {
            const Edge* edges = static_cast<const Edge*>(mmap_data_);
#pragma omp parallel for schedule(static, 1)
            for (int t = 0; t < n_threads_; ++t) {
                uint64_t start = chunk_start_[t];
                uint64_t end = chunk_start_[t + 1];
                const Edge* p = edges + start;
                for (uint64_t i = start; i < end; ++i, ++p) {
                    y[p->dst] += x[p->src];
                }
            }
            return;
        }

        const size_t edge_size = sizeof(Edge);
        const size_t buf_edges = EDGE_IO_BUF / sizeof(Edge);

#pragma omp parallel for schedule(static, 1)
        for (int t = 0; t < n_threads_; ++t) {
            uint64_t start = chunk_start_[t];
            uint64_t end = chunk_start_[t + 1];
            if (start >= end) {
                continue;
            }

            FILE* tf = fopen(rev_path_.c_str(), "rb");
            if (!tf) {
                continue;
            }
            fseek(tf, (long)(start * edge_size), SEEK_SET);
            std::vector<Edge> buf(buf_edges);

            while (start < end) {
                size_t to_read = std::min(buf_edges, (size_t)(end - start));
                size_t got = fread(buf.data(), edge_size, to_read, tf);
                if (got == 0) {
                    break;
                }
                for (size_t i = 0; i < got; ++i) {
                    y[buf[i].dst] += x[buf[i].src];
                }
                start += got;
            }
            fclose(tf);
        }
    }

    void apply_matrix_stream(float alpha, const std::vector<float>& x,
                             std::vector<float>& out) const {
        spmv_stream(x, out);
#pragma omp parallel for schedule(static)
        for (uint32_t i = 0; i < n_; ++i) {
            out[i] = x[i] - alpha * out[i];
        }
    }

    bool use_csc_ = false;

    std::string rev_path_;
    uint32_t n_;
    uint64_t m_;
};

inline KatzResult KatzSolver::solve_power(float alpha, float tol, int max_iter) const {
    using namespace std::chrono;
    auto t0 = steady_clock::now();

    const double b_norm = std::sqrt(static_cast<double>(n_));

    std::vector<float> x(n_, 0.0f);
    KatzResult res;

    if (use_csc_) {
        const uint64_t* cp = col_ptr_.data();
        const uint32_t* ri = row_idx_.data();

        for (int iter = 0; iter < max_iter; ++iter) {
            double diff_sq = 0.0;
#pragma omp parallel for reduction(+ : diff_sq) schedule(static)
            for (uint32_t j = 0; j < n_; ++j) {
                float sum = 0.0f;
                for (uint64_t k = cp[j]; k < cp[j + 1]; ++k) {
                    sum += x[ri[k]];
                }
                float x_new = alpha * sum + 1.0f;
                float d = x_new - x[j];
                diff_sq += static_cast<double>(d) * d;
                x[j] = x_new;
            }

            double r = std::sqrt(diff_sq) / b_norm;
            res.residuals.push_back(r);
            if (r < tol) {
                res.iterations = iter + 1;
                break;
            }
            if (iter == max_iter - 1) {
                res.iterations = max_iter;
            }
        }
    } else {
        // Streaming: separate SpMV + update passes
        std::vector<float> tmp(n_);
        for (int iter = 0; iter < max_iter; ++iter) {
            spmv_stream(x, tmp);

            double diff_sq = 0.0;
#pragma omp parallel for reduction(+ : diff_sq) schedule(static)
            for (uint32_t i = 0; i < n_; ++i) {
                float x_new = alpha * tmp[i] + 1.0f;
                float d = x_new - x[i];
                diff_sq += static_cast<double>(d) * d;
                x[i] = x_new;
            }

            double r = std::sqrt(diff_sq) / b_norm;
            res.residuals.push_back(r);
            if (r < tol) {
                res.iterations = iter + 1;
                break;
            }
            if (iter == max_iter - 1) {
                res.iterations = max_iter;
            }
        }
    }

    res.scores = std::move(x);
    res.elapsed_ms = duration<double, std::milli>(steady_clock::now() - t0).count();
    return res;
}

inline KatzResult KatzSolver::solve_bicgstab(float alpha, float tol, int max_iter) const {
    using namespace std::chrono;
    auto t0 = steady_clock::now();

    const double b_norm = std::sqrt(static_cast<double>(n_));

    std::vector<float> x(n_, 0.0f);
    std::vector<float> r(n_, 1.0f);
    std::vector<float> r_hat = r;
    std::vector<float> p(n_, 0.0f);
    std::vector<float> v(n_, 0.0f);
    std::vector<float> s(n_);
    std::vector<float> t(n_);

    double rho_prev = 1.0, alpha_k = 1.0, omega = 1.0;

    KatzResult res;

    for (int iter = 0; iter < max_iter; ++iter) {
        double rho = detail::dot(r_hat, r);

        if (std::abs(rho) < 1e-30) {
            r_hat = r;
            rho = detail::dot(r_hat, r);
        }

        double beta = (rho / rho_prev) * (alpha_k / omega);
        detail::axpby(-static_cast<float>(omega), v, 1.0f, p);
        detail::axpby(1.0f, r, static_cast<float>(beta), p);

        if (use_csc_) {
            apply_matrix_csc(alpha, p, v);
        } else {
            apply_matrix_stream(alpha, p, v);
        }

        double denom = detail::dot(r_hat, v);
        if (std::abs(denom) < 1e-300) {
            res.iterations = iter;
            break;
        }
        alpha_k = rho / denom;

        detail::xpaz(r, -static_cast<float>(alpha_k), v, s);

        double s_norm = detail::norm2(s);
        if (s_norm / b_norm < tol) {
            detail::axpby(static_cast<float>(alpha_k), p, 1.0f, x);
            res.residuals.push_back(s_norm / b_norm);
            res.iterations = iter + 1;
            goto done;
        }

        if (use_csc_) {
            apply_matrix_csc(alpha, s, t);
        } else {
            apply_matrix_stream(alpha, s, t);
        }

        omega = detail::dot(t, s) / detail::dot(t, t);
        if (std::abs(omega) < 1e-30) {
            res.iterations = iter;
            break;
        }

        detail::axpby(static_cast<float>(alpha_k), p, 1.0f, x);
        detail::axpby(static_cast<float>(omega), s, 1.0f, x);

        detail::xpaz(s, -static_cast<float>(omega), t, r);

        double r_norm = detail::norm2(r);
        res.residuals.push_back(r_norm / b_norm);

        if (r_norm / b_norm < tol) {
            res.iterations = iter + 1;
            goto done;
        }

        rho_prev = rho;
        if (iter == max_iter - 1) {
            res.iterations = max_iter;
        }
    }

done:
    res.scores = std::move(x);
    res.elapsed_ms = duration<double, std::milli>(steady_clock::now() - t0).count();
    return res;
}
