#pragma once
#include "types.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <functional>
#include <vector>

static constexpr size_t EDGE_IO_BUF = 64 * 1024 * 1024; // 64 MB

// ---------------------------------------------------------------------------
// EdgeWriter — buffered binary edge file writer
// ---------------------------------------------------------------------------
class EdgeWriter {
public:
    explicit EdgeWriter(const std::string& path, size_t buf_edges = EDGE_IO_BUF / sizeof(Edge))
        : f_(fopen(path.c_str(), "wb"))
        , buf_(buf_edges)
        , pos_(0)
    {
        if (!f_) {
            throw std::runtime_error("cannot open for writing: " + path);
        }
    }

    void write(Edge e) {
        buf_[pos_++] = e;
        if (pos_ == buf_.size()) {
            flush();
        }
    }

    void flush() {
        if (pos_) {
            fwrite(buf_.data(), sizeof(Edge), pos_, f_);
            pos_ = 0;
        }
    }

    ~EdgeWriter() {
        flush();
        fclose(f_);
    }

    EdgeWriter(const EdgeWriter&) = delete;
    EdgeWriter& operator=(const EdgeWriter&) = delete;

private:
    FILE* f_;
    std::vector<Edge> buf_;
    size_t pos_;
};

// ---------------------------------------------------------------------------
// read_edge_file — stream edges from a binary file in large chunks
//
// cb(const Edge* chunk, size_t count) is called for each chunk.
// Returns total number of edges read.
// ---------------------------------------------------------------------------
template <typename Callback>
uint64_t read_edge_file(const std::string& path, Callback&& cb,
                        size_t buf_edges = EDGE_IO_BUF / sizeof(Edge))
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        throw std::runtime_error("cannot open: " + path);
    }

#ifdef __linux__
    posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    std::vector<Edge> buf(buf_edges);
    uint64_t total = 0;
    size_t n;

    while ((n = fread(buf.data(), sizeof(Edge), buf.size(), f)) > 0) {
        cb(buf.data(), n);
        total += n;
    }

    fclose(f);
    return total;
}

// ---------------------------------------------------------------------------
// GraphMeta persistence
// ---------------------------------------------------------------------------
inline void save_meta(const std::string& path, GraphMeta meta) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        throw std::runtime_error("cannot open: " + path);
    }
    fwrite(&meta, sizeof(meta), 1, f);
    fclose(f);
}

inline GraphMeta load_meta(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        throw std::runtime_error("cannot open: " + path);
    }
    GraphMeta meta{};
    fread(&meta, sizeof(meta), 1, f);
    fclose(f);
    return meta;
}
