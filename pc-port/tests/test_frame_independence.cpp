// Automated test: identical input over 60 s of simulation must give an
// identical game result at 30 FPS, 60 FPS and unlimited.
//
//   pcport_tests            full run (includes 3 x 60 s real-time runs)
//   pcport_tests --quick    real-time runs shortened to 5 s
//   pcport_tests --report results/test_report.txt
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/loop.hpp"
#include "../src/core/sha256.hpp"

using namespace pcport;

namespace {

int g_fail = 0;
FILE* g_report = nullptr;

#if defined(__GNUC__)
void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
void out(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout);
    if (g_report) { std::fputs(buf, g_report); std::fflush(g_report); }
}

void check(bool ok, const char* what) {
    out("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

const uint64_t kTicks60s = (uint64_t)(60.0 * kSimHz);  // 7200

// ---------------------------------------------------------------- T0
void testSha256() {
    out("T0 SHA-256 known-answer vectors (inventory tool)\n");
    check(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256(\"\")");
    check(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256(\"abc\")");
    check(sha256Hex(std::string(1000000, 'a')) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256(1e6 x 'a')");
}

// ---------------------------------------------------------------- T1
struct Case { const char* name; FpsMode fps; double cost; double jitter; bool hitches; };

class HitchRenderer final : public Renderer {
public:
    HitchRenderer(Clock& c, double cost, double jitter, bool hitches, uint64_t seed)
        : c_(c), cost_(cost), jitter_(jitter), hitches_(hitches), rng_(seed) {}
    void draw(const SimState&, const SimState&) override {
        double t = cost_ * (1.0 + (rng_.uniform() * 2 - 1) * jitter_);
        if (hitches_ && rng_.uniform() < 0.01) t += 0.120;  // 1% frames: 120 ms stall
        c_.busyFor(t);
    }
private:
    Clock& c_;
    double cost_, jitter_;
    bool hitches_;
    Rng rng_;
};

SimState testVirtual(SimState& reference) {
    out("\nT1 Virtual clock, single-thread fixed-step loop, 60 s of sim, scripted route\n");
    const Case cases[] = {
        {"30 FPS, 4ms+-50% render", FpsMode::Fps30, 0.004, 0.5, false},
        {"60 FPS, 4ms+-50% render", FpsMode::Fps60, 0.004, 0.5, false},
        {"unlimited, 1.3ms+-80% render", FpsMode::Unlimited, 0.0013, 0.8, false},
        {"60 FPS + 1% 120ms hitches", FpsMode::Fps60, 0.004, 0.5, true},
        {"30 FPS, render 45ms (GPU-bound)", FpsMode::Fps30, 0.045, 0.1, false},
    };
    std::vector<RunResult> rs;
    for (const Case& c : cases) {
        VirtualClock clk;
        ScriptedRoute route;
        HitchRenderer r(clk, c.cost, c.jitter, c.hitches, 42);
        RunConfig cfg;
        cfg.fps = c.fps;
        cfg.ticks = kTicks60s;
        RunResult res = runSingleThread(cfg, route, r, clk);
        out("  %-32s %s | %s\n", c.name, describe(res.final).c_str(),
            formatSummary(res.summary).c_str());
        rs.push_back(res);
    }
    reference = rs[0].final;
    bool same = true;
    for (auto& r : rs) same = same && stateHash(r.final) == stateHash(reference);
    check(same, "all 5 cases: bit-identical final state (position, distance, timer, anim, audio)");
    check(rs[0].final.tick == kTicks60s, "exactly 7200 ticks = 60.000 s of sim");
    check(std::fabs(rs[0].summary.avgFps - 30) < 0.5 && std::fabs(rs[1].summary.avgFps - 60) < 0.5,
          "limiter holds 30 / 60 FPS on the virtual clock");
    check(rs[2].summary.avgFps > 300, "unlimited mode is not capped");
    // regression: limiter must not add a full extra period once the frame is
    // over budget (45 ms render at a 30 FPS cap must give ~22 FPS, not ~13)
    check(rs[4].summary.avgFps > 21.0 && rs[4].summary.p50Ms < 50.0,
          "over-budget frames are presented immediately (GPU-bound 45 ms -> ~22 FPS)");
    check(rs[0].final.charDistance > 50 && rs[0].final.carDistance > 50,
          "route actually moves character and vehicle (>50 m each)");
    check(std::fabs(rs[0].final.missionTimer - 240.0) < 1e-6,
          "mission timer 300 s -> 240 s after 60 s of sim");
    check(rs[0].final.lastFootstepSample <= rs[0].final.audioSamples() &&
              rs[0].final.audioSamples() == kTicks60s * kAudioSamplesPerTick,
          "audio clock = 2,880,000 samples @48 kHz, events stamped in sim time");
    return reference;
}

// ---------------------------------------------------------------- T2
void testNaiveControl() {
    out("\nT2 Control: same physics with variable dt (frame-rate dependent loop)\n");
    double dist[2];
    const double periods[2] = {1.0 / 30.0, 1.0 / 60.0};
    for (int k = 0; k < 2; k++) {
        ScriptedRoute route;
        SimState s;
        double t = 0;
        while (t < 60.0 - 1e-9) {
            const uint64_t tick = (uint64_t)(t * kSimHz + 1e-9);
            InputFrame in = route.sample(tick);
            // toggles are edges on a tick; a variable-dt loop sees them if the frame spans them
            in.toggleControl = 0;
            for (uint64_t q = tick; q < (uint64_t)((t + periods[k]) * kSimHz + 1e-9); q++)
                if (route.sample(q).toggleControl) in.toggleControl = 1;
            stepVariableDt(s, in, periods[k]);
            t += periods[k];
        }
        dist[k] = s.charDistance + s.carDistance;
        out("  naive loop at %d FPS: total distance %.6f m\n", k ? 60 : 30, dist[k]);
    }
    out("  difference: %.6f m (%.3f%%)\n", dist[0] - dist[1],
        100.0 * (dist[0] - dist[1]) / dist[1]);
    check(std::fabs(dist[0] - dist[1]) > 1e-3,
          "the naive loop DOES drift, so the comparison in T1/T3 is sensitive");
}

// ---------------------------------------------------------------- T3
void testRealThreaded(double seconds, const SimState& ref60) {
    out("\nT3 Real wall clock, threaded loop (sim thread 120 Hz | render thread limited), %.0f s\n",
        seconds);
    const uint64_t ticks = (uint64_t)(seconds * kSimHz);
    // reference for shortened runs
    SimState ref = ref60;
    if (ticks != kTicks60s) {
        ScriptedRoute route;
        ref = SimState{};
        for (uint64_t i = 0; i < ticks; i++) step(ref, route.sample(i));
    }
    const FpsMode modes[3] = {FpsMode::Fps30, FpsMode::Fps60, FpsMode::Unlimited};
    RunResult rs[3];
    for (int i = 0; i < 3; i++) {
        RealClock& clk = RealClock::instance();
        ScriptedRoute route;
        NullRenderer r(clk, 0.002, 0.5, 7);  // 2 ms +-50% synthetic render cost
        RunConfig cfg;
        cfg.fps = modes[i];
        cfg.ticks = ticks;
        rs[i] = runThreaded(cfg, route, r, clk);
        out("  limit %-9s wall=%.3fs sim/wall=%.5f late_ticks=%llu\n      %s\n      %s\n",
            fpsModeName(modes[i]), rs[i].wallSeconds, rs[i].simToWall,
            (unsigned long long)rs[i].lateTicks, describe(rs[i].final).c_str(),
            formatSummary(rs[i].summary).c_str());
    }
    check(stateHash(rs[0].final) == stateHash(rs[1].final) &&
              stateHash(rs[1].final) == stateHash(rs[2].final),
          "30 / 60 / unlimited: bit-identical final state");
    check(stateHash(rs[0].final) == stateHash(ref),
          "threaded result == single-thread/virtual reference");
    char msg[256];
    std::snprintf(msg, sizeof msg, "distance 30 FPS = %.6f m, 60 FPS = %.6f m (char) / %.6f = %.6f m (car)",
                  rs[0].final.charDistance, rs[1].final.charDistance, rs[0].final.carDistance,
                  rs[1].final.carDistance);
    check(rs[0].final.charDistance == rs[1].final.charDistance &&
              rs[0].final.carDistance == rs[1].final.carDistance,
          msg);
    check(std::fabs(rs[0].summary.avgFps - 30) < 1.5, "measured render rate ~30 FPS");
    check(std::fabs(rs[1].summary.avgFps - 60) < 3.0, "measured render rate ~60 FPS");
    check(rs[2].summary.avgFps > 120, "unlimited mode renders faster than 60 FPS");
    bool realtime = true;
    for (auto& r : rs) realtime = realtime && std::fabs(r.simToWall - 1.0) < 0.01;
    check(realtime, "simulation runs at real-time speed (sim/wall within 1%) in every mode");
}

// ---------------------------------------------------------------- T4
class FakeHuman final : public Renderer {
public:
    FakeHuman(LiveInput& in, RealClock& c) : in_(in), c_(c), t0_(c.now()) {}
    bool beginFrame(FrameLimiter&) override {
        const double t = c_.now() - t0_;  // input depends on WALL time, like a person
        in_.setAxes((float)std::sin(t * 1.7), (float)std::cos(t * 0.9), (float)std::sin(t * 0.5),
                    (float)std::sin(t * 2.3), std::fmod(t, 3.0) < 1.0);
        if ((int)(t / 2.5) != lastToggle_) { lastToggle_ = (int)(t / 2.5); in_.pressToggle(); }
        return true;
    }
    void draw(const SimState&, const SimState&) override { c_.busyFor(0.001); }
private:
    LiveInput& in_;
    RealClock& c_;
    double t0_;
    int lastToggle_ = 0;
};

void testRecordReplay(const std::string& dir) {
    out("\nT4 Live input recorded at 60 FPS, replayed at 30 FPS and unlimited (10 s)\n");
    RealClock& clk = RealClock::instance();
    LiveInput live;
    RecordingSource rec(live);
    FakeHuman human(live, clk);
    RunConfig cfg;
    cfg.fps = FpsMode::Fps60;
    cfg.ticks = 10 * kSimHz;
    RunResult live60 = runThreaded(cfg, rec, human, clk);
    const std::string path = dir + "/t4_replay.pcrep";
    check(rec.save(path), "replay saved");
    std::vector<InputFrame> frames;
    check(ReplaySource::load(path, frames) && frames.size() == cfg.ticks, "replay loaded (1200 ticks)");
    out("  live@60:      %s\n", describe(live60.final).c_str());

    ReplaySource rp30(frames);
    NullRenderer nr(clk, 0.001);
    cfg.fps = FpsMode::Fps30;
    RunResult rep30 = runThreaded(cfg, rp30, nr, clk);
    out("  replay@30:    %s\n", describe(rep30.final).c_str());

    VirtualClock v;
    ReplaySource rpU(frames);
    NullRenderer vr(v, 0.0007, 0.9, 3);
    cfg.fps = FpsMode::Unlimited;
    RunResult repU = runSingleThread(cfg, rpU, vr, v);
    out("  replay@unl.:  %s\n", describe(repU.final).c_str());
    check(stateHash(live60.final) == stateHash(rep30.final) &&
              stateHash(live60.final) == stateHash(repU.final),
          "replay reproduces the live session bit-exactly at a different FPS limit");
}

}  // namespace

int main(int argc, char** argv) {
    double realSeconds = 60.0;
    std::string report, dir = ".";
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--quick")) realSeconds = 5.0;
        else if (!std::strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else if (!std::strcmp(argv[i], "--tmp") && i + 1 < argc) dir = argv[++i];
    }
    if (!report.empty()) g_report = std::fopen(report.c_str(), "w");
#ifdef _WIN32
    out("pcport_tests (Windows build)\n");
#else
    out("pcport_tests (POSIX build)\n");
#endif
    out("sim rate %d Hz, dt=%.6f s, audio %d Hz\n\n", kSimHz, kSimDt, kAudioRate);
    testSha256();
    SimState ref;
    testVirtual(ref);
    testNaiveControl();
    testRealThreaded(realSeconds, ref);
    testRecordReplay(dir);
    out("\nRESULT: %s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail,
        g_fail == 1 ? "" : "s");
    if (g_report) std::fclose(g_report);
    return g_fail ? 1 : 0;
}
