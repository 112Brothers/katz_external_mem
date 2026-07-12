#include "core/katz.hpp"
#include "io/csv_reader.hpp"
#include "io/edge_file.hpp"
#include "io/external_sort.hpp"
#include "utils/timer.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

static int cmd_preprocess(const std::string& csv_path, const std::string& outdir,
                          uint32_t node_offset = 0) {
    std::filesystem::create_directories(outdir);
    Timer t("read csv");
    EdgeWriter writer(outdir + "/raw_edges.bin");
    uint64_t n = 0;
    GraphMeta meta = read_csv_edges(csv_path, [&](Edge e) {
        writer.write(e);
        ++n;
        if (n % 50'000'000 == 0) {
            printf("  read %llu M edges...\n", (unsigned long long)(n / 1'000'000));
        }
    }, 32 * 1024 * 1024, node_offset);
    t.report();
    printf("n_nodes = %u\nn_edges = %llu\n", meta.n_nodes, (unsigned long long)meta.n_edges);
    save_meta(outdir + "/meta.bin", meta);
    return 0;
}

static int cmd_sort(const std::string& datadir) {
    GraphMeta meta = load_meta(datadir + "/meta.bin");
    printf("n_nodes=%u  n_edges=%llu\n",
           meta.n_nodes, (unsigned long long)meta.n_edges);
    const std::string raw = datadir + "/raw_edges.bin";
    const std::string tmp = datadir + "/sort_tmp.bin";
    Timer t("sort by dst");
    sort_edges(raw, datadir + "/rev_edges.bin", meta.n_edges, SortKey::DST, tmp);
    t.report();
    return 0;
}

static int cmd_katz(const std::string& datadir, float alpha,
                    const std::string& method, float tol, int max_iter,
                    size_t mem_budget_mb) {
    GraphMeta meta = load_meta(datadir + "/meta.bin");
    printf("n_nodes=%u  n_edges=%llu  alpha=%.6f  method=%s  mem_budget=%zuMB\n",
           meta.n_nodes, (unsigned long long)meta.n_edges, alpha,
           method.c_str(), mem_budget_mb);
    KatzSolver solver(datadir + "/rev_edges.bin",
                      meta.n_nodes, meta.n_edges, mem_budget_mb);
    KatzResult result;
    if (method == "power") {
        result = solver.solve_power(alpha, tol, max_iter);
    } else if (method == "bicgstab") {
        result = solver.solve_bicgstab(alpha, tol, max_iter);
    } else {
        fprintf(stderr, "unknown method: %s (use 'power' or 'bicgstab')\n", method.c_str());
        return 1;
    }
    printf("iterations=%d  final_residual=%.2e  time=%.1f ms\n",
           result.iterations,
           result.residuals.empty() ? 0.0 : result.residuals.back(),
           result.elapsed_ms);
    std::string out_path = datadir + "/katz_scores.csv";
    FILE* f = fopen(out_path.c_str(), "w");
    fprintf(f, "vertex,rank\n");
    for (uint32_t v = 0; v < (uint32_t)result.scores.size(); ++v) {
        fprintf(f, "%u,%.6f\n", v, result.scores[v]);
    }
    fclose(f);
    printf("scores written to %s\n", out_path.c_str());
    printf("convergence:");
    for (double r : result.residuals) {
        printf(" %.2e", r);
    }
    printf("\n");
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr,
                "Usage:\n"
                "  %s preprocess <edges.csv> <out_dir>\n"
                "  %s sort <out_dir>\n",
                argv[0], argv[0]);
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "preprocess" && argc >= 4) {
        uint32_t offset = argc > 4 ? static_cast<uint32_t>(std::stoul(argv[4])) : 0;
        return cmd_preprocess(argv[2], argv[3], offset);
    }
    if (cmd == "sort" && argc == 3) {
        return cmd_sort(argv[2]);
    }
    if (cmd == "katz" && argc >= 5) {
        float alpha = std::stof(argv[3]);
        std::string meth = argv[4];
        float tol = argc > 5 ? std::stof(argv[5]) : 1e-6f;
        int max_iter = argc > 6 ? std::stoi(argv[6]) : 500;
        size_t mem_mb = argc > 7 ? std::stoull(argv[7]) : 128;
        return cmd_katz(argv[2], alpha, meth, tol, max_iter, mem_mb);
    }
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
