// Input sources. The simulation asks for exactly one InputFrame per tick.
//  * ScriptedRoute   - fixed 60 s route as a pure function of the tick number
//                      (identical input regardless of render FPS).
//  * LiveInput       - latest device state, written by the window/render thread,
//                      sampled by the sim thread at each tick. Edge events
//                      (toggle) are latched so they are never lost.
//  * Recorder/Replay - per-tick input log; replaying it reproduces a live
//                      session bit-exactly at any FPS limit.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sim.hpp"

namespace pcport {

class InputSource {
public:
    virtual ~InputSource() = default;
    virtual InputFrame sample(uint64_t tick) = 0;
};

class ScriptedRoute final : public InputSource {
public:
    InputFrame sample(uint64_t tick) override;
    static constexpr double kLengthSeconds = 60.0;
};

class LiveInput final : public InputSource {
public:
    void setAxes(float mx, float my, float throttle, float steer, bool sprint);
    void pressToggle();
    InputFrame sample(uint64_t tick) override;
private:
    std::mutex m_;
    InputFrame cur_;
    bool toggleLatched_ = false;
};

class RecordingSource final : public InputSource {
public:
    explicit RecordingSource(InputSource& inner) : inner_(inner) {}
    InputFrame sample(uint64_t tick) override;
    bool save(const std::string& path) const;
    const std::vector<InputFrame>& frames() const { return frames_; }
private:
    InputSource& inner_;
    std::vector<InputFrame> frames_;
};

class ReplaySource final : public InputSource {
public:
    explicit ReplaySource(std::vector<InputFrame> frames) : frames_(std::move(frames)) {}
    static bool load(const std::string& path, std::vector<InputFrame>& out);
    InputFrame sample(uint64_t tick) override;
    size_t length() const { return frames_.size(); }
private:
    std::vector<InputFrame> frames_;
};

}  // namespace pcport
