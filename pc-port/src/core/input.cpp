#include "input.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace pcport {

// Route (sim seconds):
//  0-12  walk forward (+y), 12-20 sprint diagonally, 20-26 walk in a circle,
//  26    get into vehicle, 26-40 accelerate + gentle left, 40-46 hard right,
//  46-52 brake/reverse, 52 get out, 52-60 walk back (-x).
InputFrame ScriptedRoute::sample(uint64_t tick) {
    const double t = (double)tick * kSimDt;
    InputFrame f;
    const uint64_t toggleA = (uint64_t)(26.0 * kSimHz), toggleB = (uint64_t)(52.0 * kSimHz);
    if (tick == toggleA || tick == toggleB) f.toggleControl = 1;
    if (t < 12) {
        f.moveY = 1.0f;
    } else if (t < 20) {
        f.moveX = 0.7f; f.moveY = 0.7f; f.sprint = 1;
    } else if (t < 26) {
        const double a = (t - 20) * 1.3;
        f.moveX = (float)std::cos(a); f.moveY = (float)std::sin(a);
    } else if (t < 40) {
        f.throttle = 1.0f; f.steer = 0.15f;
    } else if (t < 46) {
        f.throttle = 0.6f; f.steer = -0.9f;
    } else if (t < 52) {
        f.throttle = -0.8f;
    } else {
        f.moveX = -1.0f;
    }
    return f;
}

void LiveInput::setAxes(float mx, float my, float throttle, float steer, bool sprint) {
    std::lock_guard<std::mutex> l(m_);
    cur_.moveX = mx; cur_.moveY = my; cur_.throttle = throttle; cur_.steer = steer;
    cur_.sprint = sprint ? 1 : 0;
}

void LiveInput::pressToggle() {
    std::lock_guard<std::mutex> l(m_);
    toggleLatched_ = true;
}

InputFrame LiveInput::sample(uint64_t) {
    std::lock_guard<std::mutex> l(m_);
    InputFrame f = cur_;
    f.toggleControl = toggleLatched_ ? 1 : 0;
    toggleLatched_ = false;
    return f;
}

InputFrame RecordingSource::sample(uint64_t tick) {
    InputFrame f = inner_.sample(tick);
    frames_.push_back(f);
    return f;
}

static const char kMagic[8] = {'P', 'C', 'P', 'R', 'E', 'P', '0', '1'};

bool RecordingSource::save(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint64_t n = frames_.size();
    std::fwrite(kMagic, 1, 8, f);
    std::fwrite(&n, sizeof n, 1, f);
    if (n) std::fwrite(frames_.data(), sizeof(InputFrame), n, f);
    std::fclose(f);
    return true;
}

bool ReplaySource::load(const std::string& path, std::vector<InputFrame>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char m[8];
    uint64_t n = 0;
    bool ok = std::fread(m, 1, 8, f) == 8 && !std::memcmp(m, kMagic, 8) &&
              std::fread(&n, sizeof n, 1, f) == 1 && n < (1ull << 28);
    if (ok) {
        out.resize(n);
        ok = n == 0 || std::fread(out.data(), sizeof(InputFrame), n, f) == n;
    }
    std::fclose(f);
    return ok;
}

InputFrame ReplaySource::sample(uint64_t tick) {
    if (tick < frames_.size()) return frames_[tick];
    return InputFrame{};
}

}  // namespace pcport
