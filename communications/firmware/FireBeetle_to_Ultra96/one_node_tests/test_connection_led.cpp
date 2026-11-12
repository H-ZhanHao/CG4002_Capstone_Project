#include "FeedbackController.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using Action = FeedbackController::Action;

static void check(FeedbackController& c, uint32_t now, bool on) {
  c.update(now);
  if (c.ledOn() != on)
    fprintf(stderr, "LED mismatch at %u ms: expected=%d actual=%d pattern=%s\n",
        now, on, c.ledOn(), c.ledPattern());
  assert(c.ledOn() == on);
}

static void connectionSequence(uint8_t node, uint32_t start) {
  FeedbackController c;
  c.begin(node);
  c.enableConnectionFeedback(start);
  check(c, start, true);
  check(c, start + 499, true);
  check(c, start + 500, false);
  check(c, start + 999, false);
  check(c, start + 1000, true);
  check(c, start + 4000, true); // Retries do not expire the connecting pattern.
  assert(strcmp(c.ledPattern(), "connecting") == 0);

  const uint32_t accepted = start + 4100;
  c.resetSession();
  c.setServerConnected(true, accepted);
  assert(strcmp(c.ledPattern(), "connected-confirmation") == 0);
  // OFF separator, then exactly three distinguishable ON pulses.
  for (uint32_t phase = 0; phase < 7; ++phase) {
    check(c, accepted + phase * 100, phase % 2 == 1);
    check(c, accepted + phase * 100 + 99, phase % 2 == 1);
  }
  check(c, accepted + 700, false);
  assert(strcmp(c.ledPattern(), "idle") == 0);
  check(c, accepted + 100000, false); // Connected idle never blinks again.
  assert(!c.buzzerOn());

  // Streaming arriving during a new connection burst does not truncate it.
  const uint32_t again = accepted + 101000;
  c.resetSession();
  c.setServerConnected(true, again);
  assert(c.trigger(Action::Active, again + 50));
  check(c, again + 50, false);
  check(c, again + 100, true);
  check(c, again + 200, false);
  c.setServerConnected(true, again + 250); // Repeated state is not a new session.
  check(c, again + 300, true);
  check(c, again + 400, false);
  check(c, again + 500, true);
  check(c, again + 600, false);
  check(c, again + 700, true);
  check(c, again + 10000, true);
  assert(strcmp(c.ledPattern(), "active") == 0);
  assert(c.trigger(Action::Idle, again + 10001));
  assert(!c.ledOn());

  // A connection lost mid-confirmation immediately returns to slow blinking.
  const uint32_t lost = again + 11000;
  c.setServerConnected(false, lost);
  check(c, lost, true);
  check(c, lost + 500, false);
  c.resetSession();
  c.setServerConnected(true, lost + 600);
  check(c, lost + 700, true);
  c.setServerConnected(false, lost + 750);
  check(c, lost + 750, true);
  check(c, lost + 1250, false);

  // Connected idle/STOP arriving during the burst takes effect after it ends.
  c.resetSession();
  c.setServerConnected(true, lost + 2000);
  c.trigger(Action::Active, lost + 2010);
  c.trigger(Action::Idle, lost + 2020);
  check(c, lost + 2100, true);
  check(c, lost + 2500, true);
  check(c, lost + 2700, false);
}

static void legacyAndBuzzer() {
  FeedbackController c;
  c.begin(1); // Existing profiles do not enable connection feedback.
  c.setServerConnected(true, 0);
  check(c, 100, false);
  c.trigger(Action::Active, 200);
  check(c, 100000, true);
  c.resetSession();
  check(c, 100001, false);

  c.enableConnectionFeedback(101000);
  c.trigger(Action::FallAlarm, 101000);
  assert(c.buzzerOn());
  c.resetSession();
  c.setServerConnected(true, 101010);
  c.trigger(Action::Active, 101100);
  c.trigger(Action::Rep, 101200); // A fall still suppresses queued rep sounds.
  check(c, 101210, false);
  assert(c.buzzerOn());
  c.resetSession();
  c.setServerConnected(false, 101500);
  check(c, 103999, false);
  assert(c.buzzerOn());
  c.update(104000);
  assert(!c.buzzerOn());
  c.update(110000);
  assert(!c.buzzerOn());

  c.begin(2);
  c.enableConnectionFeedback(0);
  assert(!c.trigger(Action::Rep, 0));
  assert(!c.trigger(Action::FallAlarm, 0));
  assert(!c.buzzerOn());
}

int main() {
  for (uint8_t node = 1; node <= 2; ++node) {
    connectionSequence(node, 0);
    connectionSequence(node, UINT32_MAX - 4200); // Slow and fast rollover.
  }
  legacyAndBuzzer();
  puts("Connection LEDs: both roles, three pulses, rollover, reconnect, streaming handoff and buzzer priority PASS");
}
