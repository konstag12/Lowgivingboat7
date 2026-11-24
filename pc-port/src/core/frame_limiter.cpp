#include "frame_limiter.hpp"

#include <cstring>

namespace pcport {

const char* fpsModeName(FpsMode m) {
    switch (m) {
        case FpsMode::Fps30: return "30";
        case FpsMode::Fps60: return "60";
        default: return "unlimited";
    }
}

bool parseFpsMode(const char* s, FpsMode& out) {
    if (!std::strcmp(s, "30")) { out = FpsMode::Fps30; return true; }
    if (!std::strcmp(s, "60")) { out = FpsMode::Fps60; return true; }
    if (!std::strcmp(s, "0") || !std::strcmp(s, "unlimited") || !std::strcmp(s, "off")) {
        out = FpsMode::Unlimited;
        return true;
    }
    return false;
}

FrameLimiter::FrameLimiter(Clock& clock, FpsMode mode) : clock_(clock), mode_(mode) {}

void FrameLimiter::setMode(FpsMode m) {
    mode_ = m;
    next_ = -1.0;  // re-anchor on the next frame
}

void FrameLimiter::waitForNextFrame() {
    if (mode_ == FpsMode::Unlimited) return;
    const double period = 1.0 / (double)(int)mode_;
    const double now = clock_.now();
    if (next_ < 0.0 || now - next_ > period) {
        // first frame, or we fell behind by more than a frame: re-anchor and
        // present immediately. (Bug fixed: the first version waited a full
        // extra period here, turning a 45 ms GPU-bound frame into 78 ms.)
        next_ = now + period;
        return;
    }
    clock_.waitUntil(next_);
    next_ += period;
}

}  // namespace pcport
