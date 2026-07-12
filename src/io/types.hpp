#pragma once
#include <cstdint>

struct Edge {
    uint32_t src;
    uint32_t dst;
};

struct GraphMeta {
    uint32_t n_nodes; // max_node_id + 1
    uint64_t n_edges;
};
