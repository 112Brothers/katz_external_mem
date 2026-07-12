#pragma once
#include "types.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <functional>
#include <vector>

// Reads an edge list file in large chunks and calls cb(Edge) for each edge.
//
// Handles:
//   - optional header row (skipped if first column is not a digit)
//   - comment lines starting with '#' or '%' (SNAP / KONECT formats)
//   - extra columns beyond the first two (ignored)
//   - edges spanning chunk boundaries
//   - node_offset: subtracted from every node id (use 1 for 1-indexed files)
//
// Returns GraphMeta with n_nodes = max_id + 1 and total edge count.

template <typename Callback>
GraphMeta read_csv_edges(const std::string& path, Callback&& cb,
                         size_t chunk_size = 32 * 1024 * 1024,
                         uint32_t node_offset = 0)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        throw std::runtime_error("cannot open: " + path);
    }

#ifdef __linux__
    posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    // +256 so we can always safely append a partial tail line
    std::vector<char> buf(chunk_size + 256);

    uint32_t max_node = 0;
    uint64_t n_edges = 0;
    size_t leftover = 0; // bytes kept from previous chunk
    bool first_row = true;

    while (true) {
        size_t n = fread(buf.data() + leftover, 1, chunk_size, f);
        if (n == 0) {
            break;
        }

        char* p = buf.data();
        const char* end = buf.data() + leftover + n;

        // Find the last newline so we never process a partial edge.
        const char* safe_end = end - 1;
        while (safe_end > p && *safe_end != '\n') {
            --safe_end;
        }

        while (p < safe_end) {
            // skip whitespace / blank lines
            while (p < safe_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
                ++p;
            }
            if (p >= safe_end) {
                break;
            }

            // skip comment lines (#  SNAP,  %  KONECT)
            if (*p == '#' || *p == '%') {
                while (p < safe_end && *p != '\n') {
                    ++p;
                }
                continue;
            }

            // skip non-numeric header on the very first data row
            if (first_row) {
                first_row = false;
                if (*p < '0' || *p > '9') {
                    while (p < safe_end && *p != '\n') {
                        ++p;
                    }
                    continue;
                }
            }

            // parse src
            uint32_t src = 0;
            while (p < safe_end && *p >= '0' && *p <= '9') {
                src = src * 10 + static_cast<uint32_t>(*p++ - '0');
            }

            // skip separator (comma / space / tab)
            while (p < safe_end && (*p == ',' || *p == ' ' || *p == '\t')) {
                ++p;
            }

            // parse dst
            uint32_t dst = 0;
            while (p < safe_end && *p >= '0' && *p <= '9') {
                dst = dst * 10 + static_cast<uint32_t>(*p++ - '0');
            }

            // skip rest of the line (extra columns, \r\n)
            while (p < safe_end && *p != '\n') {
                ++p;
            }

            src -= node_offset;
            dst -= node_offset;

            cb(Edge{src, dst});

            if (src > max_node) {
                max_node = src;
            }
            if (dst > max_node) {
                max_node = dst;
            }
            ++n_edges;
        }

        // keep the partial line at the tail for the next iteration
        leftover = static_cast<size_t>(end - safe_end - 1);
        if (leftover > 0) {
            std::memmove(buf.data(), safe_end + 1, leftover);
        }
    }

    fclose(f);
    return GraphMeta{n_edges > 0 ? max_node + 1 : 0, n_edges};
}
