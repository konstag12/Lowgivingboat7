// Frame-time statistics. Definitions (also written into every report):
//   avg FPS         = frames / sum(frame times)
//   1% low FPS      = 1000 / mean of the slowest 1% frame times (ms)
//   p99 frame time  = 99th percentile frame time, nearest-rank method
#pragma once
#include <cstdio>
#include <string>
#include <vector>

namespace pcport {

struct FrameSummary {
    size_t frames = 0;
    double totalSeconds = 0;
    double avgFps = 0;
    double low1Fps = 0;
    double p99Ms = 0;
    double p50Ms = 0;
    double maxMs = 0;
};

class FrameStats {
public:
    void reserve(size_t n) { ms_.reserve(n); }
    void add(double frameSeconds) { ms_.push_back(frameSeconds * 1000.0); }
    void clear() { ms_.clear(); }
    size_t size() const { return ms_.size(); }
    FrameSummary summarize(size_t skipWarmup = 0) const;
    bool writeCsv(const std::string& path) const;
    const std::vector<double>& samplesMs() const { return ms_; }
private:
    std::vector<double> ms_;
};

std::string formatSummary(const FrameSummary& s);

}  // namespace pcport
