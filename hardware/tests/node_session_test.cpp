#include "NodeSession.h"

#include <assert.h>
#include <stdio.h>
#include <vector>

namespace {
std::vector<LinkPacket> fromHip, fromWrist;
void sendHip(const LinkPacket& p) { fromHip.push_back(p); }
void sendWrist(const LinkPacket& p) { fromWrist.push_back(p); }
void quietLog(const char*) {}

struct Pair {
  NodeSession hip{sendHip, quietLog};
  NodeSession wrist{sendWrist, quietLog};
  uint32_t now = 0;
  Pair() {
    fromHip.clear(); fromWrist.clear();
    hip.begin(HIP, 100);
    wrist.begin(WRIST, 200);
    assert(!hip.localActive() && !wrist.localActive());
    assert(!hip.consumeRestart() && !wrist.consumeRestart());
  }
  void exclusive() const { assert(!(hip.localActive() && wrist.localActive())); }
  void toHip(const LinkPacket& p) { hip.receive(p, now); exclusive(); }
  void toWrist(const LinkPacket& p) { wrist.receive(p, now); exclusive(); }
  void deliver(bool wristFirst = false) {
    for (int i = 0; i < 10 && (!fromHip.empty() || !fromWrist.empty()); ++i) {
      auto h = fromHip; fromHip.clear();
      auto w = fromWrist; fromWrist.clear();
      if (wristFirst) for (const auto& p : w) toHip(p);
      for (const auto& p : h) toWrist(p);
      if (!wristFirst) for (const auto& p : w) toHip(p);
    }
    assert(fromHip.empty() && fromWrist.empty());
  }
  void tick(uint32_t time) { now = time; hip.update(now); wrist.update(now); exclusive(); }
};

void selectionAndLocalOutput() {
  Pair p;
  p.tick(10000);
  assert(fromHip.empty() && fromWrist.empty()); // No discovery or heartbeat traffic.
  p.hip.activate(p.now);
  assert(p.hip.waiting() && !p.hip.localActive());
  p.deliver();
  assert(p.hip.localActive() && !p.wrist.localActive());
  assert(p.hip.consumeRestart() && !p.hip.consumeRestart());
  p.hip.activate(p.now); // Already active does not reset calibration/counts.
  assert(fromHip.empty() && !p.hip.consumeRestart());

  TelemetryData hipData = {}, wristData = {};
  hipData.repState = 2; hipData.reps = 12; hipData.mode = PAUSE_MODE; hipData.buzzer = 1;
  wristData.repState = 2; wristData.reps = 7; wristData.mode = CONTINUOUS_MODE;
  assert(p.hip.output(hipData).reps == 12 && p.hip.output(hipData).buzzer);
  assert(p.wrist.output(wristData).repState == NODE_STANDBY);
  p.wrist.activate(p.now); p.deliver();
  assert(!p.hip.localActive() && p.wrist.localActive());
  assert(p.wrist.consumeRestart());
  assert(p.hip.output(hipData).source == HIP && p.hip.output(hipData).buzzer);
  assert(p.wrist.output(wristData).source == WRIST && p.wrist.output(wristData).reps == 7);
  assert(p.wrist.output(wristData).mode == CONTINUOUS_MODE);

  // Losing ESP-NOW after selection cannot interrupt local counting or output.
  p.tick(1000000);
  assert(p.wrist.localActive() && fromHip.empty() && fromWrist.empty());
  p.wrist.standby();
  assert(!p.wrist.localActive() && !p.wrist.consumeRestart());
  assert(fromHip.empty() && fromWrist.empty());
}

void droppedRequestsAndAcknowledgements() {
  Pair p;
  p.hip.activate(0);
  fromHip.clear(); // Lose first request.
  p.tick(LinkTiming::RETRY_MS);
  assert(fromHip.size() == 1);
  p.toWrist(fromHip.front()); fromHip.clear();
  const LinkPacket oldAck = fromWrist.front(); fromWrist.clear(); // Lose first ACK.
  assert(!p.hip.localActive() && !p.wrist.localActive());
  p.tick(2 * LinkTiming::RETRY_MS); p.deliver();
  assert(p.hip.localActive() && p.hip.consumeRestart());
  p.toHip(oldAck);
  assert(p.hip.localActive() && !p.hip.consumeRestart());

  p.wrist.activate(p.now); p.deliver();
  assert(!p.hip.localActive() && p.wrist.localActive());
  p.toHip(oldAck); // A completed request cannot be resurrected.
  assert(!p.hip.localActive());
}

void boundedRetriesAndLateAck() {
  Pair p;
  p.wrist.activate(0);
  for (uint32_t t = LinkTiming::RETRY_MS; t <= LinkTiming::REQUEST_TIMEOUT_MS; t += LinkTiming::RETRY_MS) {
    p.tick(t);
  }
  assert(fromWrist.size() == LinkTiming::MAX_ATTEMPTS);
  assert(!p.wrist.localActive() && !p.wrist.waiting());
  TelemetryData data = {};
  assert(p.wrist.output(data).repState == ACTIVATION_FAILED);
  p.deliver(); // Delayed requests/ACKs after timeout never activate.
  assert(!p.wrist.localActive());

  Pair q;
  q.hip.activate(0);
  q.toWrist(fromHip.front()); fromHip.clear();
  const auto ack = fromWrist.front(); fromWrist.clear();
  q.now = LinkTiming::REQUEST_TIMEOUT_MS;
  q.toHip(ack); // No update() call: receive() itself enforces the deadline.
  assert(!q.hip.localActive() && !q.hip.waiting());
  q.hip.activate(q.now); q.deliver();
  assert(q.hip.localActive());
}

void cancelledAndRebootedRequests() {
  Pair p;
  p.hip.activate(0); p.toWrist(fromHip.front()); fromHip.clear();
  const auto ack = fromWrist.front(); fromWrist.clear();
  p.hip.standby(); p.toHip(ack);
  assert(!p.hip.localActive() && !p.hip.consumeRestart());
  p.now = 1;
  p.hip.activate(p.now); p.toHip(ack); // Previous request number.
  assert(p.hip.waiting());
  p.deliver(); assert(p.hip.localActive());

  p.hip.begin(HIP, 101); // Boot nonce changes even when request numbers repeat.
  p.now = 2;
  p.hip.activate(p.now); p.toHip(ack);
  assert(!p.hip.localActive());
  p.deliver(); assert(p.hip.localActive());

  p.wrist.begin(WRIST, 201);
  assert(!p.wrist.localActive());
  p.now = 3;
  p.wrist.activate(p.now); p.deliver();
  assert(!p.hip.localActive() && p.wrist.localActive());
}

void simultaneousRequestsAndDuplicates() {
  for (int wristFirst = 0; wristFirst < 2; ++wristFirst) {
    Pair p;
    p.hip.activate(0); p.wrist.activate(0);
    const auto oldWristRequest = fromWrist.front();
    p.deliver(wristFirst);
    assert(p.hip.localActive() && !p.wrist.localActive());
    assert(p.hip.consumeRestart() && !p.wrist.consumeRestart());
    // A delayed retry may stop an active peer, but can never reactivate the
    // cancelled requester. Safety is preferred to silently running both.
    p.toHip(oldWristRequest); p.deliver();
    assert(!p.hip.localActive() && !p.wrist.localActive());
  }
}

void invalidPacketsAndTimerWrap() {
  Pair p;
  p.hip.activate(0); p.toWrist(fromHip.front()); fromHip.clear();
  const auto ack = fromWrist.front(); fromWrist.clear();
  auto bad = ack; bad.magic = 0; p.toHip(bad);
  bad = ack; bad.version = 1; p.toHip(bad);
  bad = ack; bad.role = HIP; p.toHip(bad);
  bad = ack; bad.targetBoot++; p.toHip(bad);
  bad = ack; bad.request++; p.toHip(bad);
  bad = ack; bad.boot = 0; p.toHip(bad);
  bad = ack; bad.kind = static_cast<PacketKind>(99); p.toHip(bad);
  assert(p.hip.waiting() && !p.hip.localActive());
  p.toHip(ack); assert(p.hip.localActive());

  Pair q;
  q.now = UINT32_MAX - 100;
  q.wrist.activate(q.now); fromWrist.clear();
  q.tick(q.now + LinkTiming::RETRY_MS); q.deliver();
  assert(q.wrist.localActive());
}

void modeSyncAndRetries() {
  Pair p;
  p.wrist.activate(0); p.deliver(); p.wrist.consumeRestart();
  p.wrist.setMode(CONTINUOUS_MODE, 0);
  const auto request = fromWrist.front();
  p.toHip(request); fromWrist.clear();
  assert(p.hip.mode() == CONTINUOUS_MODE && p.wrist.mode() == CONTINUOUS_MODE);
  assert(!p.hip.localActive() && p.wrist.localActive());
  assert(p.hip.consumeRestart() && p.wrist.consumeRestart());
  fromHip.clear(); // Lose ACK.
  p.tick(LinkTiming::RETRY_MS); p.deliver();
  assert(!p.wrist.modePending() && !p.hip.consumeRestart());
  p.wrist.setMode(CONTINUOUS_MODE, p.now); p.deliver();
  assert(!p.hip.consumeRestart() && !p.wrist.consumeRestart());
  p.wrist.setMode(PAUSE_MODE, p.now); p.deliver();
  assert(p.hip.mode() == PAUSE_MODE && p.wrist.mode() == PAUSE_MODE);
  p.hip.consumeRestart(); p.wrist.consumeRestart();
  p.toHip(request); // Older command cannot undo newer mode.
  assert(p.hip.mode() == PAUSE_MODE && !p.hip.consumeRestart());
  p.tick(100000); assert(fromHip.empty() && fromWrist.empty());
}

void modeFailuresAndStaleAcks() {
  Pair p;
  p.hip.activate(0); p.deliver(); p.hip.consumeRestart();
  p.hip.setMode(CONTINUOUS_MODE, 0);
  for (uint32_t t = LinkTiming::RETRY_MS; t <= LinkTiming::REQUEST_TIMEOUT_MS; t += LinkTiming::RETRY_MS) p.tick(t);
  assert(fromHip.size() == LinkTiming::MAX_ATTEMPTS && !p.hip.modePending());
  assert(p.hip.mode() == CONTINUOUS_MODE && p.hip.localActive());
  p.deliver();
  assert(!p.hip.modePending());
  p.hip.setMode(PAUSE_MODE, p.now);
  p.toWrist(fromHip.front()); fromHip.clear();
  const auto ack = fromWrist.front(); fromWrist.clear();
  p.hip.setMode(CONTINUOUS_MODE, p.now);
  p.toHip(ack); assert(p.hip.modePending());
  p.deliver(); assert(!p.hip.modePending());
  p.hip.begin(HIP, 101);
  p.hip.activate(p.now); p.deliver();
  p.hip.setMode(PAUSE_MODE, p.now);
  p.toHip(ack); assert(p.hip.modePending());
  p.deliver(); assert(!p.hip.modePending() && p.wrist.mode() == PAUSE_MODE);

  Pair q;
  q.now = UINT32_MAX - 100;
  q.wrist.activate(q.now); q.deliver();
  q.wrist.setMode(CONTINUOUS_MODE, q.now); fromWrist.clear();
  q.tick(q.now + LinkTiming::RETRY_MS); q.deliver();
  assert(q.hip.mode() == CONTINUOUS_MODE && !q.wrist.modePending());
}

void onlyActiveNodeCanSelectMode() {
  Pair p;
  p.hip.setMode(CONTINUOUS_MODE, 0);
  p.wrist.setMode(CONTINUOUS_MODE, 0);
  assert(fromHip.empty() && fromWrist.empty());
  assert(p.hip.mode() == PAUSE_MODE && p.wrist.mode() == PAUSE_MODE);
  assert(!p.hip.consumeRestart() && !p.wrist.consumeRestart());
  p.hip.activate(0);
  const size_t queued = fromHip.size();
  p.hip.setMode(CONTINUOUS_MODE, 0); // Pending activation is not active.
  assert(p.hip.waiting() && p.hip.mode() == PAUSE_MODE && fromHip.size() == queued);
  p.deliver(); p.hip.consumeRestart();
  p.wrist.setMode(CONTINUOUS_MODE, 0);
  assert(fromWrist.empty() && !p.wrist.consumeRestart());
  p.hip.setMode(CONTINUOUS_MODE, 0);
  const auto oldMode = fromHip.front(); fromHip.clear();
  assert(p.hip.modePending());
  p.wrist.activate(0); p.deliver(); // Handover cancels old node's mode retries.
  assert(!p.hip.modePending() && p.wrist.localActive());
  p.hip.consumeRestart(); p.wrist.consumeRestart();
  p.toWrist(oldMode); // Old peer command cannot change the newly active node.
  assert(p.wrist.mode() == PAUSE_MODE && !p.wrist.consumeRestart());
  p.hip.setMode(PAUSE_MODE, 0);
  // Wrist activation has now automatically synchronized its selected PAUSE.
  assert(fromHip.empty() && p.hip.mode() == PAUSE_MODE);
  p.wrist.setMode(PAUSE_MODE, 0); p.deliver();
  assert(p.hip.mode() == PAUSE_MODE && !p.wrist.modePending());
  p.wrist.setMode(CONTINUOUS_MODE, 0); fromWrist.clear();
  p.wrist.standby();
  p.tick(LinkTiming::RETRY_MS);
  assert(!p.wrist.modePending() && fromWrist.empty());
  p.hip.activate(p.now); fromHip.clear();
  p.tick(p.now + LinkTiming::REQUEST_TIMEOUT_MS);
  p.hip.setMode(CONTINUOUS_MODE, p.now); // Failed activation is not active.
  assert(fromHip.empty() && p.hip.mode() == PAUSE_MODE);
}

void exerciseDefaultsAndOverrides() {
  Pair p;
  p.wrist.begin(WRIST, 200, CONTINUOUS_MODE); // Default wrist exercise: jacks.
  p.hip.activate(0); p.deliver();
  p.hip.consumeRestart(); p.wrist.consumeRestart();
  assert(p.wrist.mode() == PAUSE_MODE); // Peer sync is separate from preference.
  assert(p.wrist.selectedMode() == CONTINUOUS_MODE);
  p.wrist.activate(0); p.deliver();
  assert(p.wrist.mode() == CONTINUOUS_MODE && p.hip.mode() == CONTINUOUS_MODE);
  assert(p.wrist.consumeRestart() && !p.wrist.consumeRestart());
  p.hip.consumeRestart();
  assert(p.hip.selectedMode() == PAUSE_MODE && !p.wrist.modePending());

  // Active exercise change to sit-ups: default mode and one restart.
  p.wrist.selectExerciseMode(PAUSE_MODE, 0); p.deliver();
  assert(p.wrist.localActive() && p.wrist.mode() == PAUSE_MODE);
  assert(p.wrist.consumeRestart() && !p.wrist.consumeRestart());
  p.hip.consumeRestart();
  p.wrist.selectExerciseMode(PAUSE_MODE, 0); p.deliver();
  assert(p.wrist.consumeRestart() && !p.wrist.consumeRestart());
  assert(!p.hip.consumeRestart()); // Same mode does not restart the peer.

  // Manual override persists across handover and RESET (no preference change).
  p.wrist.setMode(CONTINUOUS_MODE, 0); p.deliver();
  assert(p.wrist.selectedMode() == CONTINUOUS_MODE);
  p.hip.activate(0); p.deliver();
  assert(p.wrist.mode() == PAUSE_MODE && p.wrist.selectedMode() == CONTINUOUS_MODE);
  p.wrist.setMode(PAUSE_MODE, 0); // Inactive manual command cannot erase override.
  assert(p.wrist.selectedMode() == CONTINUOUS_MODE && fromWrist.empty());
  p.wrist.activate(0); p.deliver();
  assert(p.wrist.mode() == CONTINUOUS_MODE);

  p.hip.activate(0); p.deliver();
  p.wrist.consumeRestart();
  p.wrist.selectExerciseMode(PAUSE_MODE, 0); // Changed exercise in standby.
  assert(!p.wrist.localActive() && !p.wrist.waiting());
  assert(p.wrist.selectedMode() == PAUSE_MODE && fromWrist.empty());
  assert(p.wrist.consumeRestart() && !p.wrist.consumeRestart());
  p.hip.setMode(CONTINUOUS_MODE, 0); p.deliver();
  assert(p.wrist.mode() == CONTINUOUS_MODE && p.wrist.selectedMode() == PAUSE_MODE);
  p.wrist.activate(0); p.deliver();
  assert(p.wrist.mode() == PAUSE_MODE && p.hip.mode() == PAUSE_MODE);
}

void queuedExerciseModeFailures() {
  Pair p;
  p.wrist.selectExerciseMode(CONTINUOUS_MODE, 0);
  assert(p.wrist.mode() == PAUSE_MODE && !p.wrist.modePending());
  assert(fromWrist.empty()); // Selecting in standby sends no mode packets.
  p.wrist.consumeRestart();
  p.wrist.activate(0);
  p.toHip(fromWrist.front()); fromWrist.clear();
  const auto staleAck = fromHip.front(); fromHip.clear();
  p.wrist.selectExerciseMode(PAUSE_MODE, 0); // Cancels pending activation.
  p.wrist.consumeRestart();
  p.toWrist(staleAck);
  assert(!p.wrist.localActive() && !p.wrist.waiting() && fromWrist.empty());
  assert(!p.wrist.consumeRestart());

  p.wrist.selectExerciseMode(CONTINUOUS_MODE, 0);
  p.wrist.activate(0); fromWrist.clear();
  p.tick(LinkTiming::REQUEST_TIMEOUT_MS);
  assert(!p.wrist.localActive() && !p.wrist.modePending() && fromWrist.empty());
  assert(p.wrist.selectedMode() == CONTINUOUS_MODE); // Retained for retry.
  p.wrist.activate(p.now);
  p.toHip(fromWrist.front()); fromWrist.clear();
  p.toWrist(fromHip.front()); fromHip.clear();
  assert(p.wrist.localActive() && p.wrist.mode() == CONTINUOUS_MODE);
  assert(p.wrist.modePending() && fromWrist.size() == 1);
  assert(fromWrist.front().kind == PacketKind::SET_MODE);
  assert(p.wrist.consumeRestart() && !p.wrist.consumeRestart());
  fromWrist.clear(); // Lost automatic sync; retry succeeds without a restart.
  p.tick(p.now + LinkTiming::RETRY_MS); p.deliver();
  assert(!p.wrist.modePending() && !p.wrist.consumeRestart());
  assert(p.hip.mode() == CONTINUOUS_MODE);
}

void mirroredLedPulses() {
  for (int activeHip = 0; activeHip < 2; ++activeHip) {
    Pair p;
    auto& active = activeHip ? p.hip : p.wrist;
    auto& inactive = activeHip ? p.wrist : p.hip;
    active.activate(0); p.deliver(); active.consumeRestart();
    inactive.showLedFeedback(LedFeedback::REP, 0);
    assert(fromHip.empty() && fromWrist.empty());
    active.showLedFeedback(LedFeedback::READY, 0); p.deliver();
    // Three distinct 100 ms flashes, starting with 100 ms OFF to separate
    // readiness from the active node's previously steady calibration LED.
    for (uint32_t start : {100U, 300U, 500U}) {
      assert(!active.feedbackLedOn(start - 1) && !inactive.feedbackLedOn(start - 1));
      assert(active.feedbackLedOn(start) && inactive.feedbackLedOn(start));
      assert(active.feedbackLedOn(start + 99) && inactive.feedbackLedOn(start + 99));
      assert(!active.feedbackLedOn(start + 100) && !inactive.feedbackLedOn(start + 100));
    }
    assert(active.feedbackLedOn(LedTiming::READY_MS - 1));
    assert(!active.feedbackLedOn(LedTiming::READY_MS));
    assert(!inactive.feedbackLedOn(LedTiming::READY_MS));
    assert(!inactive.localActive() && !inactive.consumeRestart());
    p.now = 1000;
    active.showLedFeedback(LedFeedback::REP, p.now); p.deliver();
    assert(active.feedbackLedOn(p.now) && inactive.feedbackLedOn(p.now));
    assert(!inactive.feedbackLedOn(p.now + LedTiming::REP_MS));
    TelemetryData data = {}; data.reps = 9; data.mode = PAUSE_MODE;
    assert(active.output(data).reps == 9);
    assert(inactive.output(data).repState == NODE_STANDBY);
    assert(!active.consumeRestart() && !inactive.consumeRestart());
    // Drop the next LED packet: local feedback and counting remain independent.
    p.now = 2000;
    active.showLedFeedback(LedFeedback::REP, p.now);
    fromHip.clear(); fromWrist.clear(); p.tick(10000);
    assert(active.localActive() && active.output(data).reps == 9);
    assert(fromHip.empty() && fromWrist.empty()); // No LED retries/heartbeats.
  }
}

void ledOrderingClearingAndHandover() {
  Pair p;
  p.hip.activate(0);
  const auto oldActivation = fromHip.front();
  p.deliver(); p.hip.consumeRestart();
  p.hip.showLedFeedback(LedFeedback::READY, 0);
  const auto ready = fromHip.front(); fromHip.clear();
  p.toWrist(ready);
  p.now = 100;
  p.hip.showLedFeedback(LedFeedback::REP, p.now);
  const auto rep = fromHip.front(); fromHip.clear();
  p.toWrist(rep);
  p.now = 250;
  p.toWrist(rep); p.toWrist(ready); // Do not extend/replay pulses.
  assert(!p.wrist.feedbackLedOn(300));
  p.hip.clearLedFeedback(p.now); p.deliver(); // RESET path.
  assert(!p.hip.feedbackLedOn(p.now) && !p.wrist.feedbackLedOn(p.now));
  p.toWrist(rep); assert(!p.wrist.feedbackLedOn(p.now));
  p.hip.showLedFeedback(LedFeedback::READY, p.now); p.deliver();
  p.hip.setMode(CONTINUOUS_MODE, p.now); p.deliver();
  assert(!p.hip.feedbackLedOn(p.now) && !p.wrist.feedbackLedOn(p.now));
  p.hip.clearLedFeedback(p.now); p.deliver(); // ExerciseNode's mode restart.
  p.hip.showLedFeedback(LedFeedback::REP, p.now); p.deliver();
  p.hip.standby(); p.deliver();
  assert(!p.wrist.feedbackLedOn(p.now));
  p.hip.activate(p.now); p.deliver(); // New activation session.
  p.toWrist(rep); assert(!p.wrist.feedbackLedOn(p.now));
  p.toWrist(oldActivation); p.deliver(); // Old retry cannot restore old LED binding.
  p.toWrist(rep); assert(!p.wrist.feedbackLedOn(p.now));
  p.hip.showLedFeedback(LedFeedback::REP, p.now); p.deliver();
  assert(p.wrist.feedbackLedOn(p.now));
  p.wrist.activate(p.now); p.deliver();
  assert(!p.hip.feedbackLedOn(p.now) && !p.wrist.feedbackLedOn(p.now));
  p.toWrist(rep); assert(!p.wrist.feedbackLedOn(p.now));
  p.wrist.showLedFeedback(LedFeedback::REP, p.now); p.deliver();
  assert(p.hip.feedbackLedOn(p.now));
}

void ledValidationRebootsAndWrap() {
  Pair p;
  p.hip.activate(0); p.deliver();
  p.hip.showLedFeedback(LedFeedback::READY, 0);
  const auto ready = fromHip.front(); fromHip.clear();
  auto bad = ready; bad.targetBoot++; p.toWrist(bad);
  bad = ready; bad.boot++; p.toWrist(bad);
  bad = ready; bad.request++; p.toWrist(bad);
  bad = ready; bad.mode = 99; p.toWrist(bad);
  bad = ready; bad.sequence = 0; p.toWrist(bad);
  assert(!p.wrist.feedbackLedOn(p.now));
  p.toWrist(ready); assert(p.wrist.feedbackLedOn(p.now + 100));
  p.wrist.begin(WRIST, 201);
  p.toWrist(ready); assert(!p.wrist.feedbackLedOn(p.now));
  p.hip.standby(); p.deliver();
  p.now = UINT32_MAX - 100;
  p.hip.activate(p.now); p.deliver();
  p.hip.showLedFeedback(LedFeedback::REP, p.now); p.deliver();
  assert(p.wrist.feedbackLedOn(p.now + 199));
  assert(!p.wrist.feedbackLedOn(p.now + 200));
  p.hip.showLedFeedback(LedFeedback::READY, p.now); p.deliver();
  for (uint32_t offset : {100U, 300U, 500U}) {
    assert(p.wrist.feedbackLedOn(p.now + offset));
    assert(!p.wrist.feedbackLedOn(p.now + offset + 100));
  }
  p.wrist.standby();
  assert(!p.wrist.feedbackLedOn(p.now));
}

// Exercise arbitrary delivery order, duplicates, drops, local cancellations and
// reboots. Check exclusion after every event, not only after queues drain.
void reorderedLossyTraffic() {
  for (uint32_t seed = 1; seed <= 20; ++seed) {
    Pair p;
    uint32_t random = seed, hipBoot = 100, wristBoot = 200;
    for (int step = 0; step < 10000; ++step) {
      random = random * 1664525U + 1013904223U;
      const unsigned action = (random >> 16) % 20;
      if (action == 0) p.hip.activate(p.now);
      else if (action == 1) p.wrist.activate(p.now);
      else if (action == 2) p.hip.standby();
      else if (action == 3) p.wrist.standby();
      else if (action == 4) p.hip.begin(HIP, ++hipBoot);
      else if (action == 5) p.wrist.begin(WRIST, ++wristBoot);
      else if (action < 16) {
        const bool fromH = (random & 1) != 0;
        auto& queue = fromH ? fromHip : fromWrist;
        if (!queue.empty()) {
          const size_t index = (random >> 8) % queue.size();
          const auto packet = queue[index];
          if (action != 6) queue.erase(queue.begin() + index); // Occasionally duplicate.
          if (action != 7) { // Occasionally drop.
            if (fromH) p.toWrist(packet); else p.toHip(packet);
          }
        }
      } else p.tick(p.now + 1 + ((random >> 8) % 600));
      p.exclusive();
    }
  }
}
}

int main() {
  selectionAndLocalOutput();
  droppedRequestsAndAcknowledgements();
  boundedRetriesAndLateAck();
  cancelledAndRebootedRequests();
  simultaneousRequestsAndDuplicates();
  invalidPacketsAndTimerWrap();
  modeSyncAndRetries();
  modeFailuresAndStaleAcks();
  onlyActiveNodeCanSelectMode();
  exerciseDefaultsAndOverrides();
  queuedExerciseModeFailures();
  mirroredLedPulses();
  ledOrderingClearingAndHandover();
  ledValidationRebootsAndWrap();
  reorderedLossyTraffic();
  puts("PASS: mirrored LEDs, LED ordering/clear/handover/reboot/loss/timers, mode sync, active-only mode selection, exclusive activation, reboots, local output, no heartbeats, timer wrap, 200000 lossy/reordered events");
}
