// Game loops.
//
//  runThreaded()     - production loop: the simulation runs on its own thread
//                      at a fixed 120 Hz against the wall clock; the calling
//                      thread renders as fast as the FrameLimiter allows and
//                      interpolates between the last two published ticks.
//                      Render FPS cannot change sim speed or sim results.
//
//  runSingleThread() - classic fixed-timestep accumulator on one thread.
//                      Works with any Clock, so tests can drive it with a
//                      VirtualClock (exact, fast) to cover pathological frame
//                      times deterministically.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

#include "clock.hpp"
#include "frame_limiter.hpp"
#include "frame_stats.hpp"
#include "input.hpp"
#include "sim.hpp"

namespace pcport {

class Renderer {
public:
    virtual ~Renderer() = default;
    // Called once per frame before waiting; may change limiter mode, poll
    // devices, etc. Return false to request exit.
    virtual bool beginFrame(FrameLimiter&) { return true; }
    // Draw the interpolated state. `latest` is the newest simulated tick.
    virtual void draw(const SimState& interpolated, const SimState& latest) = 0;
};

class NullRenderer final : public Renderer {
public:
    // Optional synthetic per-frame cost (seconds) with +-jitter fraction.
    NullRenderer(Clock& c, double cost = 0, double jitter = 0, uint64_t seed = 1)
        : clock_(c), cost_(cost), jitter_(jitter), rng_(seed) {}
    void draw(const SimState&, const SimState&) override;
private:
    Clock& clock_;
    double cost_, jitter_;
    Rng rng_;
};

struct RunConfig {
    FpsMode fps = FpsMode::Fps60;
    uint64_t ticks = 0;          // stop after this many sim ticks (0 = until renderer quits)
    size_t warmupFrames = 30;    // frames excluded from the summary
    std::atomic<bool>* stop = nullptr;
};

struct RunResult {
    SimState final;
    FrameStats frames;
    FrameSummary summary;
    uint64_t lateTicks = 0;      // ticks executed >1 tick late (sim thread starved)
    uint64_t maxCatchUp = 0;     // max ticks executed per render frame (single-thread)
    double wallSeconds = 0;
    double simToWall = 0;        // sim seconds / wall seconds (1.0 = real time)
};

RunResult runThreaded(const RunConfig& cfg, InputSource& input, Renderer& r, RealClock& clock);
RunResult runSingleThread(const RunConfig& cfg, InputSource& input, Renderer& r, Clock& clock);

}  // namespace pcport
