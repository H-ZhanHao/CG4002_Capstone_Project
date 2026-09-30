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
  reorderedLossyTraffic();
  puts("PASS: exclusive activation, retries, stale ACKs, reboots, races, local output, no heartbeats, timer wrap, 200000 lossy/reordered events");
}
