// Time sources. Everything that waits or reads time goes through Clock so the
// same loop code can run on the real wall clock or on a virtual clock in tests.
#pragma once
#include <cstdint>

namespace pcport {

class Clock {
public:
    virtual ~Clock() = default;
    virtual double now() = 0;                    // seconds, monotonic
    virtual void waitUntil(double deadline) = 0; // returns at or after deadline
    virtual void busyFor(double seconds) = 0;    // simulate CPU/GPU work
};

// Monotonic wall clock with a precise wait:
//  * Windows: high-resolution waitable timer (Win10 1803+), falls back to
//    timeBeginPeriod(1)+Sleep, then spins for the last ~0.5 ms.
//  * POSIX: nanosleep for the bulk, then spin.
class RealClock final : public Clock {
public:
    RealClock();
    ~RealClock() override;
    double now() override;
    void waitUntil(double deadline) override;
    void busyFor(double seconds) override;
    static RealClock& instance();
private:
    void* timer_ = nullptr;
    bool  raisedPeriod_ = false;
};

// Deterministic virtual time for tests: waiting just moves time forward.
class VirtualClock final : public Clock {
public:
    double now() override { return t_; }
    void waitUntil(double deadline) override { if (deadline > t_) t_ = deadline; }
    void busyFor(double seconds) override { if (seconds > 0) t_ += seconds; }
private:
    double t_ = 0.0;
};

// Small deterministic PRNG (xorshift64*) for jitter in tests/bench.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ull; }
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
};

}  // namespace pcport
