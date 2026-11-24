// Render frame limiter with three switchable modes: 30, 60, unlimited.
// It only paces presentation; simulation speed is controlled separately.
#pragma once
#include "clock.hpp"

namespace pcport {

enum class FpsMode { Fps30 = 30, Fps60 = 60, Unlimited = 0 };

const char* fpsModeName(FpsMode m);
bool parseFpsMode(const char* s, FpsMode& out);  // "30", "60", "0"/"unlimited"

class FrameLimiter {
public:
    explicit FrameLimiter(Clock& clock, FpsMode mode = FpsMode::Fps60);
    void setMode(FpsMode m);
    FpsMode mode() const { return mode_; }
    // Blocks until the next frame slot. Deadlines are absolute (t0 + k*period)
    // so that rounding of individual waits does not accumulate as drift.
    void waitForNextFrame();
private:
    Clock& clock_;
    FpsMode mode_;
    double next_ = -1.0;
};

}  // namespace pcport
