#include "loop.hpp"

#include <algorithm>
#include <mutex>
#include <thread>

namespace pcport {

void NullRenderer::draw(const SimState&, const SimState&) {
    if (cost_ <= 0) return;
    const double j = (rng_.uniform() * 2.0 - 1.0) * jitter_;
    clock_.busyFor(cost_ * (1.0 + j));
}

namespace {

struct Published {
    SimState prev, curr;
};

class SimThread {
public:
    SimThread(InputSource& in, RealClock& clock, uint64_t ticks, std::atomic<bool>* extStop)
        : in_(in), clock_(clock), ticks_(ticks), extStop_(extStop) {}

    void start(double t0) {
        t0_ = t0;
        th_ = std::thread([this] { run(); });
    }
    void requestStop() { stop_ = true; }
    void join() { if (th_.joinable()) th_.join(); }
    bool done() const { return done_; }
    uint64_t lateTicks() const { return late_; }
    double t0() const { return t0_; }

    Published read() {
        std::lock_guard<std::mutex> l(m_);
        return pub_;
    }

private:
    void run() {
        SimState s;
        {
            std::lock_guard<std::mutex> l(m_);
            pub_.prev = pub_.curr = s;
        }
        while (!stop_ && !(extStop_ && *extStop_) && (ticks_ == 0 || s.tick < ticks_)) {
            // state with tick n is due at wall time t0 + n*dt
            const double due = t0_ + (double)(s.tick + 1) * kSimDt;
            clock_.waitUntil(due);
            if (clock_.now() - due > kSimDt) late_++;
            const SimState prev = s;
            step(s, in_.sample(s.tick));
            std::lock_guard<std::mutex> l(m_);
            pub_.prev = prev;
            pub_.curr = s;
        }
        done_ = true;
    }

    InputSource& in_;
    RealClock& clock_;
    uint64_t ticks_;
    std::atomic<bool>* extStop_;
    std::atomic<bool> stop_{false}, done_{false};
    std::atomic<uint64_t> late_{0};
    double t0_ = 0;
    std::mutex m_;
    Published pub_;
    std::thread th_;
};

}  // namespace

RunResult runThreaded(const RunConfig& cfg, InputSource& input, Renderer& r, RealClock& clock) {
    RunResult res;
    FrameLimiter limiter(clock, cfg.fps);
    SimThread sim(input, clock, cfg.ticks, cfg.stop);
    const double t0 = clock.now();
    sim.start(t0);

    double last = t0;
    size_t frame = 0;
    bool quit = false;
    while (!sim.done()) {
        if (!quit && !r.beginFrame(limiter)) {
            quit = true;
            sim.requestStop();
        }
        limiter.waitForNextFrame();
        const double now = clock.now();
        if (frame > 0) res.frames.add(now - last);
        last = now;
        frame++;

        const Published p = sim.read();
        // render one tick behind real time so we can interpolate
        const double showSimTime = now - t0 - kSimDt;
        double alpha = (showSimTime - p.prev.simSeconds()) / kSimDt;
        alpha = std::clamp(alpha, 0.0, 1.0);
        r.draw(lerp(p.prev, p.curr, alpha), p.curr);
    }
    sim.join();
    res.wallSeconds = clock.now() - t0;
    res.final = sim.read().curr;
    res.lateTicks = sim.lateTicks();
    res.simToWall = res.wallSeconds > 0 ? res.final.simSeconds() / res.wallSeconds : 0;
    res.summary = res.frames.summarize(cfg.warmupFrames);
    return res;
}

RunResult runSingleThread(const RunConfig& cfg, InputSource& input, Renderer& r, Clock& clock) {
    RunResult res;
    FrameLimiter limiter(clock, cfg.fps);
    SimState s, prev;
    const double t0 = clock.now();
    double last = t0, acc = 0;
    size_t frame = 0;
    for (;;) {
        if (cfg.stop && *cfg.stop) break;
        if (cfg.ticks && s.tick >= cfg.ticks) break;
        if (!r.beginFrame(limiter)) break;
        limiter.waitForNextFrame();
        const double now = clock.now();
        const double ft = now - last;
        last = now;
        if (frame > 0) res.frames.add(ft);
        frame++;

        acc += ft;
        uint64_t steps = 0;
        while (acc >= kSimDt && (cfg.ticks == 0 || s.tick < cfg.ticks)) {
            prev = s;
            step(s, input.sample(s.tick));
            acc -= kSimDt;
            steps++;
        }
        res.maxCatchUp = std::max(res.maxCatchUp, steps);
        r.draw(lerp(prev, s, std::clamp(acc / kSimDt, 0.0, 1.0)), s);
    }
    res.wallSeconds = clock.now() - t0;
    res.final = s;
    res.simToWall = res.wallSeconds > 0 ? s.simSeconds() / res.wallSeconds : 0;
    res.summary = res.frames.summarize(cfg.warmupFrames);
    return res;
}

}  // namespace pcport
