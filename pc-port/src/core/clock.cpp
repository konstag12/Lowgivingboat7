#include "clock.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <time.h>
#endif

namespace pcport {

namespace {
#ifdef _WIN32
double qpcSeconds() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / (double)f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * inv;
}
#endif
constexpr double kSpinWindow = 0.0005;  // last 0.5 ms is spun for precision
}  // namespace

RealClock::RealClock() {
#ifdef _WIN32
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) {
        timeBeginPeriod(1);
        raisedPeriod_ = true;
    }
#endif
}

RealClock::~RealClock() {
#ifdef _WIN32
    if (timer_) CloseHandle((HANDLE)timer_);
    if (raisedPeriod_) timeEndPeriod(1);
#endif
}

RealClock& RealClock::instance() {
    static RealClock c;
    return c;
}

double RealClock::now() {
#ifdef _WIN32
    return qpcSeconds();
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

void RealClock::waitUntil(double deadline) {
    double remaining = deadline - now();
    if (remaining > kSpinWindow) {
        const double sleepFor = remaining - kSpinWindow;
#ifdef _WIN32
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(sleepFor * 1e7);  // relative, 100 ns units
            if (SetWaitableTimerEx((HANDLE)timer_, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject((HANDLE)timer_, INFINITE);
        } else {
            Sleep((DWORD)(sleepFor * 1000.0));
        }
#else
        timespec ts;
        ts.tv_sec = (time_t)sleepFor;
        ts.tv_nsec = (long)((sleepFor - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, nullptr);
#endif
    }
    while (now() < deadline) std::this_thread::yield();
}

void RealClock::busyFor(double seconds) {
    const double end = now() + seconds;
    volatile double sink = 0;
    while (now() < end) sink = sink + 1.0;
}

}  // namespace pcport
