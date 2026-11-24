// Deterministic fixed-timestep simulation for the test scene.
// The simulation only ever advances in steps of kSimDt, so the game result
// depends on the number of ticks and the per-tick input, never on render FPS.
#pragma once
#include <cstdint>
#include <string>

namespace pcport {

constexpr int    kSimHz = 120;
constexpr double kSimDt = 1.0 / kSimHz;
constexpr int    kAudioRate = 48000;                   // virtual audio clock
constexpr int    kAudioSamplesPerTick = kAudioRate / kSimHz;  // 400

enum class Control : uint8_t { Character = 0, Vehicle = 1 };

// Input as consumed by ONE simulation tick.
struct InputFrame {
    float moveX = 0, moveY = 0;  // character stick, [-1,1]
    float throttle = 0;          // vehicle, [-1,1] (negative = reverse/brake)
    float steer = 0;             // vehicle, [-1,1]
    uint8_t sprint = 0;
    uint8_t toggleControl = 0;   // edge: switch character <-> vehicle
    uint8_t pad[2] = {0, 0};
};
static_assert(sizeof(InputFrame) == 20, "InputFrame layout is part of the replay format");

struct SimState {
    uint64_t tick = 0;
    Control control = Control::Character;
    // character
    double px = 0, py = 0, vx = 0, vy = 0;
    double charDistance = 0;
    double animPhase = 0;          // walk-cycle phase [0,1)
    uint64_t footsteps = 0;        // audio events emitted on phase wrap
    uint64_t lastFootstepSample = 0;  // audio-sample timestamp of the last event
    // vehicle
    double cx = 20, cy = 0, heading = 0, carSpeed = 0;
    double carDistance = 0;
    double wheelAngle = 0;         // wheel spin animation (radians)
    // mission
    double missionTimer = 300.0;   // counts down in sim time
    uint32_t checkpoints = 0;      // one per 25 m of total travel

    uint64_t audioSamples() const { return tick * (uint64_t)kAudioSamplesPerTick; }
    double simSeconds() const { return (double)tick * kSimDt; }
};

void step(SimState& s, const InputFrame& in);

// Bit-exact fingerprint of the state (FNV-1a over all fields).
uint64_t stateHash(const SimState& s);
std::string describe(const SimState& s);

// Linear interpolation for rendering between two consecutive ticks.
SimState lerp(const SimState& a, const SimState& b, double t);

// Control experiment only: the same physics integrated with a variable dt
// (the classic "speed depends on FPS" bug). Used by the tests to prove the
// comparison is sensitive enough to catch frame-rate dependence.
void stepVariableDt(SimState& s, const InputFrame& in, double dt);

}  // namespace pcport
