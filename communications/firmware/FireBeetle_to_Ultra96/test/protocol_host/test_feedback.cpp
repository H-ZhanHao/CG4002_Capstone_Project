#include "FeedbackController.h"
#include "FeedbackDiagnostic.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

using Action = FeedbackController::Action;
using Command = FeedbackDiagnostic::Command;

static Command line(FeedbackDiagnostic& parser, const char* text) {
  Command result = Command::None;
  while (*text) result = parser.feed(*text++);
  return result;
}

static void outputs(uint32_t start) {
  FeedbackController c;
  c.begin(1);
  assert(!c.ledOn() && !c.buzzerOn());
  c.trigger(Action::Active, start);
  c.update(start + 10000);
  assert(c.ledOn() && !c.buzzerOn()); // Persistent, silent activity.

  c.trigger(Action::Rep, start + 10000);
  c.trigger(Action::Rep, start + 10010);
  assert(c.ledOn() && c.buzzerOn());
  c.update(start + 10099); assert(c.buzzerOn());
  c.update(start + 10100); assert(!c.buzzerOn());
  c.update(start + 10199); assert(!c.buzzerOn());
  c.update(start + 10200); assert(c.buzzerOn());
  c.trigger(Action::Idle, start + 10210);
  assert(!c.ledOn() && c.buzzerOn()); // LED state never truncates a rep.
  c.update(start + 10300); assert(!c.buzzerOn());
  c.update(start + 10400); assert(!c.buzzerOn()); // No idle double beep.

  c.trigger(Action::Active, start + 11000);
  c.trigger(Action::Rep, start + 11000);
  c.trigger(Action::Rep, start + 11010);
  c.trigger(Action::FallAlarm, start + 11050);
  c.trigger(Action::Rep, start + 11100); // Accepted, suppressed by fall priority.
  c.trigger(Action::Idle, start + 11200);
  assert(!c.ledOn() && c.buzzerOn());
  c.resetSession(); // A transport reconnect must not silence a local fall.
  c.update(start + 14049); assert(c.buzzerOn());
  c.update(start + 14050); assert(!c.buzzerOn());
  c.update(start + 15000); assert(!c.buzzerOn()); // No replay after alarm.

  c.trigger(Action::Active, start + 16000);
  c.trigger(Action::Rep, start + 16000);
  c.trigger(Action::Rep, start + 16001);
  c.resetSession();
  assert(!c.ledOn() && !c.buzzerOn());
  c.update(start + 17000); assert(!c.buzzerOn());

  c.begin(2);
  assert(!c.hasBuzzer() && !c.trigger(Action::Rep, start));
  assert(!c.trigger(Action::FallAlarm, start));
  c.trigger(Action::Active, start);
  c.update(start + 100000);
  assert(c.ledOn() && !c.buzzerOn());
  c.trigger(Action::Idle, start + 100000);
  assert(!c.ledOn() && !c.buzzerOn());
}

int main() {
  outputs(100);
  outputs(UINT32_MAX - 10050); // Pulse/gap timestamps cross millis rollover.
  FeedbackDiagnostic parser;
  assert(line(parser, "feedback rep\r") == Command::Rep);
  assert(parser.feed('\n') == Command::None);
  assert(line(parser, "feedback active\n") == Command::Active);
  assert(line(parser, "feedback idle\n") == Command::Idle);
  assert(line(parser, "feedback ready\n") == Command::Invalid);
  assert(line(parser, "feedback fall-alarm\n") == Command::FallAlarm);
  assert(line(parser, "fall inject\n") == Command::FallEvent);
  assert(line(parser, "feedback status\n") == Command::Status);
  for (int i = 0; i < 10000; ++i) parser.feed('x');
  assert(line(parser, "feedback rep\n") == Command::Invalid);
  assert(line(parser, "feedback rep\n") == Command::Rep);
  puts("Integrated activity LEDs, queued rep beeps, silent idle, fall priority, session reset, rollover and diagnostics PASS");
}
