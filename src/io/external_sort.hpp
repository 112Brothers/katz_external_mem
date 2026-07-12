#pragma once
#include "edge_file.hpp"
#include "types.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

// External LSD radix sort for binary edge files.
//
// Algorithm: 4 passes over the data, 8 bits per pass (256 buckets).
// Each pass does two sequential scans (count then distribute).
//
// Memory per pass:
//   read buffer (inside read_edge_file):    EDGE_IO_BUF   = 64 MB
//   output buffers: 256 × buf_per_bucket   (default 256 KB) = 64 MB
//   ────────────────────────────────────────────────────────────────
//   peak                                                  ≈ 128 MB
//
// Disk: needs one temporary file the same size as the input.
// File routing: input → tmp → output → tmp → output
//   (output is used as scratch between passes 1 and 2; if the process
//    is interrupted, delete both output and tmp before re-running.)

enum class SortKey: uint8_t { SRC,
                              DST };

namespace detail {

    inline uint32_t edge_key(Edge e, SortKey k) noexcept {
        return k == SortKey::DST ? e.dst : e.src;
    }

    // One LSD pass: stably sort edges by byte `pass_idx` of the key field.
    inline void radix_pass(const std::string& in_path,
                           const std::string& out_path,
                           uint64_t n_edges,
                           int pass_idx,
                           SortKey key,
                           size_t buf_per_bucket)
    {
        const size_t EDGES_PER_BUF =
            std::max<size_t>(1, buf_per_bucket / sizeof(Edge));

        // ── Phase 1: count ───────────────────────────────────────────────────
        uint64_t hist[256] = {};
        read_edge_file(in_path, [&](const Edge* c, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                ++hist[static_cast<uint8_t>(edge_key(c[i], key) >> (8 * pass_idx))];
            }
        });

        // Prefix sum → starting position (in edges) for each bucket
        uint64_t cursor[256];
        cursor[0] = 0;
        for (int i = 1; i < 256; ++i) {
            cursor[i] = cursor[i - 1] + hist[i - 1];
        }

        // ── Pre-allocate output file to full size ────────────────────────────
        int out_fd = ::open(out_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            throw std::runtime_error("cannot create: " + out_path);
        }
        if (n_edges > 0) {
            auto sz = static_cast<off_t>(n_edges * sizeof(Edge));
            if (::ftruncate(out_fd, sz) != 0) {
                throw std::runtime_error("ftruncate failed: " + out_path);
            }
        }

        // ── Phase 2: distribute ──────────────────────────────────────────────
        // Flat array: 256 contiguous sub-buffers, each EDGES_PER_BUF edges wide.
        std::vector<Edge> flat(256 * EDGES_PER_BUF);
        std::vector<size_t> pos(256, 0);

        auto flush_bucket = [&](int b) {
            if (pos[b] == 0) {
                return;
            }
            off_t byte_off = static_cast<off_t>(cursor[b] * sizeof(Edge));
            ssize_t written = ::pwrite(out_fd,
                                       &flat[static_cast<size_t>(b) * EDGES_PER_BUF],
                                       pos[b] * sizeof(Edge),
                                       byte_off);
            if (written < 0) {
                throw std::runtime_error("pwrite failed on: " + out_path);
            }
            cursor[b] += pos[b];
            pos[b] = 0;
        };

        read_edge_file(in_path, [&](const Edge* c, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                int b = static_cast<uint8_t>(edge_key(c[i], key) >> (8 * pass_idx));
                flat[static_cast<size_t>(b) * EDGES_PER_BUF + pos[b]++] = c[i];
                if (pos[b] == EDGES_PER_BUF) {
                    flush_bucket(b);
                }
            }
        });
        for (int b = 0; b < 256; ++b) {
            flush_bucket(b);
        }

        ::close(out_fd);
    }

} // namespace detail

// ── Public API ───────────────────────────────────────────────────────────────

// Sort `input_file` by key field and write result to `output_file`.
// `tmp_path` must be on the same filesystem (used as scratch, deleted on exit).
inline void sort_edges(const std::string& input_file,
                       const std::string& output_file,
                       uint64_t n_edges,
                       SortKey key,
                       const std::string& tmp_path,
                       size_t buf_per_bucket = 256 * 1024)
{
    using P = std::pair<const std::string*, const std::string*>;
    const std::array<P, 4> passes = {{
        {&input_file, &tmp_path},
        {&tmp_path, &output_file},
        {&output_file, &tmp_path},
        {&tmp_path, &output_file},
    }};

    for (int pass = 0; pass < 4; ++pass) {
        detail::radix_pass(*passes[pass].first, *passes[pass].second,
                           n_edges, pass, key, buf_per_bucket);
    }

    std::remove(tmp_path.c_str());
}
