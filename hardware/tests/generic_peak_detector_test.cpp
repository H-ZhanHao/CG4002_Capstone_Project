#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <vector>
#include "GenericPeakDetector.h"
#include "ExercisePresets.h"

struct Rig {
  GenericPeakDetector detector;
  std::vector<GenericPeakEvent> events;
  uint32_t time = 0;
  int accelerationAxis = 2, gyroAxis = 0;
  void feed(float motion = 0, float rotation = 0, uint32_t elapsed = 20) {
    time += elapsed;
    SensorSample sample = {};
    sample.timestampMs = time;
    sample.acceleration[accelerationAxis] = GeneralConfig::GRAVITY_MPS2 + motion;
    sample.gyro[gyroAxis] = rotation;
    GenericPeakEvent event;
    event.peakMs = 123;
    if (detector.update(sample, event)) events.push_back(event);
    else assert(event.peakMs == 123); // No partial output on rejected samples.
  }
  void hold(int samples, float motion = 0, float rotation = 0) {
    for (int i = 0; i < samples; ++i) feed(motion, rotation);
  }
  void pulse(bool gyroOnly = false, float sign = 1) {
    const float values[] = {0, .4f, .8f, 1.2f, 1.6f, 2, 2.4f, 2.4f,
                           2, 1.5f, 1, .5f, 0, 0, 0, 0, 0, 0};
    for (float value : values) feed(gyroOnly ? 0 : sign * value,
                                   gyroOnly ? sign * value : 0);
  }
};

int main() {
  // Stationary sensor noise remains quiet regardless of gravity's axis.
  for (int axis = 0; axis < 3; ++axis) {
    Rig rig;
    rig.accelerationAxis = axis;
    for (int i = 0; i < 1000; ++i)
      rig.feed(.12f * sinf(i * .7f), .08f * cosf(i * .3f));
    // Isolated large spikes and 40 ms bursts are not sustained movement.
    for (int i = 0; i < 10; ++i) {
      rig.feed(12, 8);
      rig.hold(25);
      rig.hold(2, 4, 4);
      rig.hold(25);
    }
    assert(rig.events.empty());
  }

  // Acceleration-only and gyro-only movement, both signs, all sensor axes.
  for (int axis = 0; axis < 3; ++axis) {
    for (bool gyroOnly : {false, true}) {
      for (float sign : {-1.0f, 1.0f}) {
        Rig rig;
        rig.accelerationAxis = rig.gyroAxis = axis;
        rig.hold(10);
        const uint32_t started = rig.time;
        rig.pulse(gyroOnly, sign);
        assert(rig.events.size() == 1);
        const auto& event = rig.events[0];
        assert(event.peakMs == started + 160); // Median peak, not confirmation time.
        assert(event.detectedMs > event.peakMs && event.detectedMs <= rig.time);
        assert(fabsf((gyroOnly ? event.gyroRadS : event.accelerationMps2) - 2.4f) < .001f);
        assert((gyroOnly ? event.accelerationMps2 : event.gyroRadS) < .001f);
      }
    }
  }

  // Starting already in motion needs only the short filter/duration guard.
  Rig moving;
  moving.hold(8, 2);
  moving.hold(6);
  assert(moving.events.size() == 1 && moving.events[0].detectedMs < 3000);
  // Both channels contribute to one excursion, not two triggers.
  Rig both;
  both.hold(10);
  both.hold(8, 2, 3);
  both.hold(10);
  assert(both.events.size() == 1);

  // Generic gates must also admit motion near the weakest counting preset peaks.
  for (bool gyroOnly : {false, true}) {
    Rig weak;
    weak.hold(10);
    weak.hold(10, gyroOnly ? 0 : .70f, gyroOnly ? .70f : 0);
    weak.hold(10);
    assert(weak.events.size() == 1);
  }

  // At 100 Hz, reject sub-gate noise and short strong bursts,
  // but retain sustained weak exercise peaks on either channel independently.
  for (bool gyroOnly : {false, true}) {
    for (int scenario = 0; scenario < 3; ++scenario) {
      Rig rig;
      for (int i = 0; i < 20; ++i) rig.feed(0, 0, 10);
      const float amplitude = scenario == 0 ? .35f : (scenario == 1 ? 1.2f : .70f);
      const int samples = scenario == 1 ? 8 : 20;
      for (int i = 0; i < samples; ++i)
        rig.feed(gyroOnly ? 0 : amplitude, gyroOnly ? amplitude : 0, 10);
      for (int i = 0; i < 20; ++i) rig.feed(0, 0, 10);
      assert(rig.events.size() == (scenario == 2 ? 1u : 0u));
    }
  }

  // Replay a smooth one-second peak at each preset's minimum amplitude.
  // Keep coverage tied to the presets so a threshold increase cannot silently
  // exclude a supported exercise. These are synthetic signals, not worn traces.
  for (int exercise = 0; exercise < EXERCISE_COUNT; ++exercise) {
    const auto& preset = ExercisePresets::get(static_cast<ExerciseType>(exercise));
    assert(GenericPeakConfig::ACCELERATION_GATE_MPS2 < preset.peak);
    assert(GenericPeakConfig::GYRO_GATE_RAD_S < preset.gyroPeak);
    for (bool gyroOnly : {false, true}) {
      Rig rig;
      for (int i = 0; i < 20; ++i) rig.feed(0, 0, 10);
      const float peak = gyroOnly ? preset.gyroPeak : preset.peak;
      for (int i = 0; i <= 100; ++i) {
        const float amplitude = peak * (i <= 50 ? i : 100 - i) / 50.0f;
        rig.feed(gyroOnly ? 0 : amplitude, gyroOnly ? amplitude : 0, 10);
      }
      for (int i = 0; i < 20; ++i) rig.feed(0, 0, 10);
      assert(rig.events.size() == 1);
    }
  }

  // Real sample intervals can vary: duration guards use timestamps, not counts.
  Rig jitter;
  jitter.hold(10);
  const float wave[] = {0, .4f, .8f, 1.2f, 1.6f, 2, 2.4f, 2.4f,
                       2, 1.5f, 1, .5f, 0, 0, 0, 0, 0, 0};
  unsigned int index = 0;
  for (float value : wave) jitter.feed(value, 0, 20 + 7 * (index++ % 3));
  assert(jitter.events.size() == 1);
  assert(jitter.events[0].detectedMs - jitter.events[0].peakMs < 250);

  // No second trigger while motion stays above the lower rearm gate.
  Rig sustained;
  sustained.hold(10);
  sustained.hold(10, 3);
  sustained.hold(100, 1.2f); // Confirms the first decline but never rearms.
  sustained.hold(10, 3);
  sustained.hold(10, 1.2f);
  assert(sustained.events.size() == 1);
  sustained.hold(60);
  sustained.pulse();
  assert(sustained.events.size() == 2);

  // Brief quiet dips cannot rearm; cooldown rejects an entire rapid excursion.
  Rig cooldown;
  cooldown.hold(10);
  cooldown.pulse();
  cooldown.hold(10);
  cooldown.pulse(true);
  assert(cooldown.events.size() == 1);
  cooldown.hold(60);
  cooldown.pulse(true);
  assert(cooldown.events.size() == 2);
  assert(cooldown.events[1].detectedMs - cooldown.events[0].detectedMs >= 1000);
  Rig dip;
  dip.hold(10);
  dip.hold(10, 3);
  dip.hold(100, 1.2f);
  dip.hold(2);
  dip.hold(10, 3);
  dip.hold(10, 1.2f);
  assert(dip.events.size() == 1);

  // A plateau that never forms a timely peak must not trigger much later.
  Rig plateau;
  plateau.hold(10);
  plateau.hold(200, 2);
  plateau.hold(60);
  assert(plateau.events.empty());
  plateau.pulse();
  assert(plateau.events.size() == 1);

  // Never join a pre-outage rise to a post-outage fall.
  Rig gap;
  gap.hold(10);
  gap.hold(8, 2);
  gap.feed(0, 0, 220);
  gap.hold(20);
  assert(gap.events.empty());
  gap.pulse();
  assert(gap.events.size() == 1);
  // A sensor interruption cannot bypass the one-second cooldown either.
  gap.feed(0, 0, 120);
  gap.pulse();
  assert(gap.events.size() == 1);

  // Nonfinite input discards the candidate, including finite-value overflow.
  for (float bad : {NAN, INFINITY, 1.0e30f}) {
    Rig invalid;
    invalid.hold(10);
    invalid.hold(8, 2);
    invalid.feed(bad);
    invalid.hold(20);
    assert(invalid.events.empty());
    invalid.pulse();
    assert(invalid.events.size() == 1);
  }
  // Duplicate / out-of-order timestamps cannot manufacture elapsed duration.
  Rig timestamps;
  timestamps.hold(10);
  GenericPeakEvent ignored;
  SensorSample old = {};
  old.acceleration[2] = 12;
  old.timestampMs = timestamps.time;
  for (int i = 0; i < 20; ++i) assert(!timestamps.detector.update(old, ignored));
  old.timestampMs -= 100;
  assert(!timestamps.detector.update(old, ignored));
  timestamps.pulse();
  assert(timestamps.events.size() == 1);

  // Timer rollover preserves peak timing, confirmation and cooldown.
  Rig rollover;
  rollover.time = UINT32_MAX - 300;
  rollover.hold(10);
  rollover.pulse();
  assert(rollover.events.size() == 1);
  assert(rollover.events[0].peakMs == 59);
  assert(rollover.events[0].detectedMs - rollover.events[0].peakMs == 100);
  rollover.hold(10);
  rollover.pulse();
  assert(rollover.events.size() == 1);
  rollover.hold(60);
  rollover.pulse();
  assert(rollover.events.size() == 2);

  // Boot reset clears old candidates and cooldown without requiring stillness.
  moving.detector.reset();
  moving.hold(8, 2);
  moving.hold(6);
  assert(moving.events.size() == 2);
  puts("Generic peak noise, axes, timing, rearm, cooldown, gaps and rollover tests passed");
}
