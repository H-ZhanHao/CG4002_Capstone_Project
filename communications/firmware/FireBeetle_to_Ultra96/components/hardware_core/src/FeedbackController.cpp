#include "FeedbackController.h"
// Use the canonical config; the older communications copy has separate types.
#include "../../../../../../hardware/node/ExerciseConfig.h"
#include "NodeSession.h"

void FeedbackController::begin(uint8_t node) {
  hip_ = node == 1;
  led_ = Led::Off;
  buzzer_ = Buzzer::Off;
  ledOn_ = buzzerOn_ = false;
}

bool FeedbackController::trigger(Action action, uint32_t now) {
  update(now);
  switch (action) {
    case Action::Rep:
      led_ = Led::Rep;
      ledStart_ = now;
      break;
    case Action::Ready:
      // A ready request must not truncate a current rep pulse.
      if (led_ == Led::Rep) return false;
      led_ = Led::Ready;
      ledStart_ = now;
      break;
    case Action::Idle:
      if (!hip_ || buzzer_ == Buzzer::Fall) return false;
      buzzer_ = Buzzer::Idle;
      buzzerStart_ = now;
      break;
    case Action::FallAlarm:
      if (!hip_) return false;
      buzzer_ = Buzzer::Fall; // Discard interrupted idle; never resume it.
      buzzerStart_ = now;
      break;
  }
  update(now);
  return true;
}

void FeedbackController::update(uint32_t now) {
  const uint32_t ledElapsed = now - ledStart_;
  if ((led_ == Led::Rep && ledElapsed >= LedTiming::REP_MS) ||
      (led_ == Led::Ready && ledElapsed >= LedTiming::READY_MS)) led_ = Led::Off;
  ledOn_ = led_ == Led::Rep || (led_ == Led::Ready &&
      (ledElapsed / LedTiming::READY_STEP_MS) % 2 == 1);
  const uint32_t buzzerElapsed = now - buzzerStart_;
  if ((buzzer_ == Buzzer::Idle && buzzerElapsed >= 300) ||
      (buzzer_ == Buzzer::Fall && buzzerElapsed >= FallConfig::BUZZER_DURATION_MS))
    buzzer_ = Buzzer::Off;
  buzzerOn_ = hip_ && (buzzer_ == Buzzer::Fall ||
      (buzzer_ == Buzzer::Idle && (buzzerElapsed < 100 || buzzerElapsed >= 200)));
}

const char *FeedbackController::ledPattern() const {
  return led_ == Led::Rep ? "rep" : led_ == Led::Ready ? "ready" : "off";
}
const char *FeedbackController::buzzerPattern() const {
  return buzzer_ == Buzzer::Fall ? "fall-alarm-output-only" :
      buzzer_ == Buzzer::Idle ? "idle" : "off";
}
