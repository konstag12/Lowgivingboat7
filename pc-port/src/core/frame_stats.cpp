#include "frame_stats.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pcport {

FrameSummary FrameStats::summarize(size_t skip) const {
    FrameSummary r;
    if (ms_.size() <= skip) return r;
    std::vector<double> v(ms_.begin() + (long)skip, ms_.end());
    r.frames = v.size();
    const double sum = std::accumulate(v.begin(), v.end(), 0.0);
    r.totalSeconds = sum / 1000.0;
    r.avgFps = sum > 0 ? (double)v.size() * 1000.0 / sum : 0;
    std::vector<double> sorted = v;
    std::sort(sorted.begin(), sorted.end());
    auto rank = [&](double p) {
        size_t k = (size_t)std::ceil(p * (double)sorted.size());
        if (k < 1) k = 1;
        return sorted[std::min(k, sorted.size()) - 1];
    };
    r.p50Ms = rank(0.50);
    r.p99Ms = rank(0.99);
    r.maxMs = sorted.back();
    const size_t worstN = std::max<size_t>(1, sorted.size() / 100);
    double worst = 0;
    for (size_t i = sorted.size() - worstN; i < sorted.size(); i++) worst += sorted[i];
    worst /= (double)worstN;
    r.low1Fps = worst > 0 ? 1000.0 / worst : 0;
    return r;
}

bool FrameStats::writeCsv(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "frame,frame_time_ms\n");
    for (size_t i = 0; i < ms_.size(); i++) std::fprintf(f, "%zu,%.4f\n", i, ms_[i]);
    std::fclose(f);
    return true;
}

std::string formatSummary(const FrameSummary& s) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "frames=%zu time=%.2fs avg_fps=%.2f low1_fps=%.2f p50_ms=%.3f p99_ms=%.3f "
                  "max_ms=%.3f",
                  s.frames, s.totalSeconds, s.avgFps, s.low1Fps, s.p50Ms, s.p99Ms, s.maxMs);
    return buf;
}

}  // namespace pcport
