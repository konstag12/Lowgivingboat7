#include "sim.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace pcport {

namespace {
constexpr double kPi = 3.14159265358979323846;

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

void integrate(SimState& s, const InputFrame& in, double dt) {
    if (in.toggleControl) {
        s.control = (s.control == Control::Character) ? Control::Vehicle : Control::Character;
    }

    // ---- character: acceleration + exponential drag (non-linear in dt on purpose)
    double ix = clampd(in.moveX, -1, 1), iy = clampd(in.moveY, -1, 1);
    if (s.control != Control::Character) { ix = 0; iy = 0; }
    const double len = std::sqrt(ix * ix + iy * iy);
    if (len > 1.0) { ix /= len; iy /= len; }
    const double accel = in.sprint ? 34.0 : 20.0;
    const double maxSpeed = in.sprint ? 7.5 : 4.2;
    s.vx += ix * accel * dt;
    s.vy += iy * accel * dt;
    const double drag = std::exp(-5.0 * dt);
    s.vx *= drag;
    s.vy *= drag;
    double sp = std::sqrt(s.vx * s.vx + s.vy * s.vy);
    if (sp > maxSpeed) { s.vx *= maxSpeed / sp; s.vy *= maxSpeed / sp; sp = maxSpeed; }
    s.px += s.vx * dt;
    s.py += s.vy * dt;
    s.charDistance += sp * dt;

    // walk cycle: phase speed proportional to ground speed; footstep on wrap
    s.animPhase += sp * 0.65 * dt;
    while (s.animPhase >= 1.0) {
        s.animPhase -= 1.0;
        s.footsteps++;
        s.lastFootstepSample = (s.tick + 1) * (uint64_t)kAudioSamplesPerTick;
    }

    // ---- vehicle: throttle, rolling drag, speed-dependent steering
    double th = clampd(in.throttle, -1, 1), st = clampd(in.steer, -1, 1);
    if (s.control != Control::Vehicle) { th = 0; st = 0; }
    s.carSpeed += (th * 14.0 - 0.35 * s.carSpeed - 0.012 * s.carSpeed * std::fabs(s.carSpeed)) * dt;
    s.heading += st * clampd(s.carSpeed, -12, 12) * 0.09 * dt;
    if (s.heading > kPi) s.heading -= 2 * kPi;
    if (s.heading < -kPi) s.heading += 2 * kPi;
    s.cx += std::cos(s.heading) * s.carSpeed * dt;
    s.cy += std::sin(s.heading) * s.carSpeed * dt;
    s.carDistance += std::fabs(s.carSpeed) * dt;
    s.wheelAngle = std::fmod(s.wheelAngle + s.carSpeed / 0.35 * dt, 2 * kPi);

    // ---- mission
    s.missionTimer -= dt;
    if (s.missionTimer < 0) s.missionTimer = 0;
    s.checkpoints = (uint32_t)((s.charDistance + s.carDistance) / 25.0);
}
}  // namespace

void step(SimState& s, const InputFrame& in) {
    integrate(s, in, kSimDt);
    s.tick++;
}

void stepVariableDt(SimState& s, const InputFrame& in, double dt) {
    integrate(s, in, dt);
}

uint64_t stateHash(const SimState& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const double d[] = {s.px, s.py, s.vx, s.vy, s.charDistance, s.animPhase,
                        s.cx, s.cy, s.heading, s.carSpeed, s.carDistance, s.wheelAngle,
                        s.missionTimer};
    mix(&s.tick, sizeof s.tick);
    uint8_t c = (uint8_t)s.control;
    mix(&c, 1);
    mix(d, sizeof d);
    mix(&s.footsteps, sizeof s.footsteps);
    mix(&s.lastFootstepSample, sizeof s.lastFootstepSample);
    mix(&s.checkpoints, sizeof s.checkpoints);
    return h;
}

std::string describe(const SimState& s) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "tick=%llu sim=%.3fs char_dist=%.6f m car_dist=%.6f m pos=(%.4f,%.4f) "
                  "car=(%.4f,%.4f) timer=%.4f s footsteps=%llu last_step_sample=%llu "
                  "checkpoints=%u hash=%016llx",
                  (unsigned long long)s.tick, s.simSeconds(), s.charDistance, s.carDistance, s.px,
                  s.py, s.cx, s.cy, s.missionTimer, (unsigned long long)s.footsteps,
                  (unsigned long long)s.lastFootstepSample, s.checkpoints,
                  (unsigned long long)stateHash(s));
    return buf;
}

SimState lerp(const SimState& a, const SimState& b, double t) {
    SimState r = b;
    auto L = [t](double x, double y) { return x + (y - x) * t; };
    r.px = L(a.px, b.px);
    r.py = L(a.py, b.py);
    r.cx = L(a.cx, b.cx);
    r.cy = L(a.cy, b.cy);
    double dh = b.heading - a.heading;
    if (dh > kPi) dh -= 2 * kPi;
    if (dh < -kPi) dh += 2 * kPi;
    r.heading = a.heading + dh * t;
    return r;
}

}  // namespace pcport
