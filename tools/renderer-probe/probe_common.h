// Shared helpers of the pipeline measurement programs (pso_d3d12, pso_vulkan).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace probe {

struct Pair {
    int group;
    std::string vs, ps;
};

struct Options {
    std::string shaderDir, pairFile, out, save, load;
    int threads = 1;
    int group = -1;     // -1 = all
    int variants = 1;   // state variants per pair (blend/depth/cull)
    int limit = 0;
    bool warp = false;
    bool debug = false;
};

inline Options parseArgs(int argc, char** argv) {
    Options o;
    if (argc < 3) {
        std::printf("Usage: %s <shader-folder> <pairs.tsv> [--threads N] [--group 0|1] [--variants K] [--limit N]\n"
                    "        [--out file.tsv] [--save cache.bin] [--load cache.bin] [--warp] [--debug]\n", argv[0]);
        std::exit(2);
    }
    o.shaderDir = argv[1];
    o.pairFile = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--threads") o.threads = std::atoi(next());
        else if (a == "--group") o.group = std::atoi(next());
        else if (a == "--variants") o.variants = std::atoi(next());
        else if (a == "--limit") o.limit = std::atoi(next());
        else if (a == "--out") o.out = next();
        else if (a == "--save") o.save = next();
        else if (a == "--load") o.load = next();
        else if (a == "--warp") o.warp = true;
        else if (a == "--debug") o.debug = true;
        else { std::printf("unknown option %s\n", a.c_str()); std::exit(2); }
    }
    return o;
}

inline std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline std::vector<Pair> readPairs(const Options& o) {
    std::vector<Pair> pairs;
    std::ifstream f(o.pairFile);
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        Pair p{std::atoi(line.substr(0, a).c_str()), line.substr(a + 1, b - a - 1), line.substr(b + 1)};
        while (!p.ps.empty() && (p.ps.back() == '\r' || p.ps.back() == '\n')) p.ps.pop_back();
        if (o.group >= 0 && p.group != o.group) continue;
        pairs.push_back(std::move(p));
        if (o.limit && (int)pairs.size() >= o.limit) break;
    }
    return pairs;
}

using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Runs fn(i) for i in [0, n) on `threads` threads.
inline void parallelFor(int n, int threads, const std::function<void(int)>& fn) {
    std::atomic<int> next{0};
    auto work = [&] {
        for (;;) {
            int i = next.fetch_add(1);
            if (i >= n) return;
            fn(i);
        }
    };
    if (threads <= 1) { work(); return; }
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();
}

inline void printStats(const char* label, std::vector<double> ms, double wallMs, int threads, int failed) {
    if (ms.empty()) { std::printf("%s: no values\n", label); return; }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    auto q = [&](double p) { return ms[std::min(ms.size() - 1, (size_t)(p * ms.size()))]; };
    std::printf("%s: n=%zu failed=%d threads=%d wall=%.1f ms sum=%.1f ms mean=%.3f median=%.3f "
                "p90=%.3f p99=%.3f max=%.3f ms\n",
                label, ms.size(), failed, threads, wallMs, sum, sum / ms.size(), q(0.5), q(0.9), q(0.99), ms.back());
}

}  // namespace probe
