// pcport_bench - headless frame-pacing benchmark (no window, no GPU).
// Same loops as the test scene with a NullRenderer of configurable cost.
//   pcport_bench --fps 60 --seconds 60 --loop threaded --cost-ms 2 --runs 3 --csv out.csv
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../core/loop.hpp"

using namespace pcport;

int main(int argc, char** argv) {
    FpsMode fps = FpsMode::Fps60;
    double seconds = 60, costMs = 2.0, jitter = 0.5;
    int runs = 3;
    bool threaded = true;
    std::string csv;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(a, "--fps") && v) { if (!parseFpsMode(v, fps)) return 2; i++; }
        else if (!std::strcmp(a, "--seconds") && v) { seconds = std::atof(v); i++; }
        else if (!std::strcmp(a, "--cost-ms") && v) { costMs = std::atof(v); i++; }
        else if (!std::strcmp(a, "--jitter") && v) { jitter = std::atof(v); i++; }
        else if (!std::strcmp(a, "--runs") && v) { runs = std::atoi(v); i++; }
        else if (!std::strcmp(a, "--loop") && v) { threaded = std::strcmp(v, "single") != 0; i++; }
        else if (!std::strcmp(a, "--csv") && v) { csv = v; i++; }
        else {
            std::fprintf(stderr, "usage: pcport_bench [--fps 30|60|unlimited] [--seconds S] "
                                 "[--loop threaded|single] [--cost-ms MS] [--jitter F] [--runs N] [--csv PATH]\n");
            return 2;
        }
    }
    RealClock& clk = RealClock::instance();
    std::printf("bench: limit=%s seconds=%.1f loop=%s render_cost=%.2fms+-%.0f%% runs=%d\n",
                fpsModeName(fps), seconds, threaded ? "threaded" : "single", costMs, jitter * 100, runs);
    for (int r = 0; r < runs; r++) {
        ScriptedRoute route;
        NullRenderer nr(clk, costMs / 1000.0, jitter, 1000 + r);
        RunConfig cfg;
        cfg.fps = fps;
        cfg.ticks = (uint64_t)(seconds * kSimHz);
        RunResult res = threaded ? runThreaded(cfg, route, nr, clk) : runSingleThread(cfg, route, nr, clk);
        std::printf("run %d: %s sim/wall=%.5f late_ticks=%llu hash=%016llx\n", r + 1,
                    formatSummary(res.summary).c_str(), res.simToWall,
                    (unsigned long long)res.lateTicks, (unsigned long long)stateHash(res.final));
        if (!csv.empty()) {
            std::string p = csv;
            const size_t dot = p.rfind('.');
            p.insert(dot == std::string::npos ? p.size() : dot, "_run" + std::to_string(r + 1));
            res.frames.writeCsv(p);
        }
        std::fflush(stdout);
    }
    return 0;
}
