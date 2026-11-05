#include "NodeSession.h"
#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

static std::vector<LinkPacket> fromHip, fromWrist;
static unsigned int beeps = 0, clears = 0;
static std::string logs;
static void sendHip(const LinkPacket& p) { fromHip.push_back(p); }
static void sendWrist(const LinkPacket& p) { fromWrist.push_back(p); }
static void logMessage(const char* text) { logs += text; }
static void feedback(RepFeedback kind) {
  if (kind == RepFeedback::REP) ++beeps;
  else ++clears;
}
struct Pair {
  NodeSession hip{sendHip, logMessage, feedback}, wrist{sendWrist, logMessage};
  uint32_t now = 0;
  Pair() {
    fromHip.clear(); fromWrist.clear(); logs.clear(); beeps = clears = 0;
    hip.begin(HIP, 100); wrist.begin(WRIST, 200);
  }
  void deliver() {
    for (int i = 0; i < 50 && (!fromHip.empty() || !fromWrist.empty()); ++i) {
      auto h = fromHip; fromHip.clear();
      auto w = fromWrist; fromWrist.clear();
      for (const auto& p : h) wrist.receive(p, now);
      for (const auto& p : w) hip.receive(p, now);
    }
    assert(fromHip.empty() && fromWrist.empty());
  }
  void activateWrist() {
    wrist.activate(now); deliver();
    assert(wrist.localActive() && !hip.localActive());
  }
  void tick(uint32_t ms) { now += ms; hip.update(now); wrist.update(now); }
};

int main() {
  {
    Pair p;
    p.wrist.showRepFeedback(0); p.deliver();
    assert(!beeps); // Inactive sender cannot create feedback.
    p.activateWrist();
    p.wrist.showRepFeedback(p.now);
    const auto first = fromWrist.front(); fromWrist.clear(); // Drop first event.
    p.tick(RepFeedbackTiming::RETRY_MS);
    assert(fromWrist.size() == 1);
    p.hip.receive(fromWrist.front(), p.now); fromWrist.clear();
    assert(beeps == 1 && fromHip.size() == 1);
    const auto ack = fromHip.front(); fromHip.clear(); // Drop ACK.
    p.tick(RepFeedbackTiming::RETRY_MS); p.deliver();
    assert(beeps == 1); // Retry was acknowledged without another beep.
    p.tick(RepFeedbackTiming::RETRY_MS);
    assert(fromWrist.empty());
    p.hip.receive(first, p.now); p.deliver();
    assert(beeps == 1);

    // FIFO keeps distinct fast reps; an ACK for an earlier event cannot pop one.
    p.wrist.showRepFeedback(p.now);
    p.wrist.showRepFeedback(p.now);
    p.wrist.showRepFeedback(p.now);
    p.wrist.receive(ack, p.now);
    p.deliver();
    assert(beeps == 4);
    // Replay/malformed/unrelated packets cannot sound the buzzer.
    for (int field = 0; field < 7; ++field) {
      auto bad = first; bad.sequence = 100;
      if (field == 0) ++bad.boot;
      if (field == 1) ++bad.targetBoot;
      if (field == 2) ++bad.request;
      if (field == 3) bad.role = HIP;
      if (field == 4) bad.mode = 99;
      if (field == 5) ++bad.version;
      if (field == 6) bad.sequence = 0;
      p.hip.receive(bad, p.now);
    }
    assert(beeps == 4 && fromHip.empty());
    p.wrist.showRepFeedback(p.now);
    const auto cancelled = fromWrist.front(); fromWrist.clear();
    p.wrist.clearRepFeedback(p.now); p.deliver();
    const auto afterClear = clears;
    p.hip.receive(cancelled, p.now); p.deliver();
    assert(beeps == 4 && clears == afterClear);
    p.wrist.showRepFeedback(p.now); p.deliver();
    assert(beeps == 5);
    p.wrist.standby(p.now); p.deliver();
    assert(clears == afterClear + 1);
    p.wrist.showRepFeedback(p.now); p.deliver();
    assert(beeps == 5);

    // New activation rejects the old event even when its sequence is larger.
    p.activateWrist();
    auto old = cancelled; old.sequence = 1000;
    p.hip.receive(old, p.now);
    assert(beeps == 5 && fromHip.empty());
    p.wrist.showRepFeedback(p.now); p.deliver();
    assert(beeps == 6);
    p.hip.activate(p.now); p.deliver();
    p.hip.receive(first, p.now);
    assert(p.hip.localActive() && beeps == 6);
  }
  {
    Pair p; p.activateWrist();
    p.wrist.showRepFeedback(p.now);
    const auto beforeReboot = fromWrist.front(); fromWrist.clear();
    p.hip.begin(HIP, 101);
    p.hip.receive(beforeReboot, p.now);
    assert(beeps == 0 && fromHip.empty());
    p.wrist.standby(p.now); fromWrist.clear();
    p.activateWrist(); // Fresh peer boot is learned by the handshake.
    p.wrist.showRepFeedback(p.now); p.deliver();
    assert(beeps == 1);
    p.wrist.begin(WRIST, 201); p.activateWrist();
    auto old = beforeReboot; old.targetBoot = 101;
    p.hip.receive(old, p.now);
    assert(beeps == 1);
    p.wrist.showRepFeedback(p.now); p.deliver();
    assert(beeps == 2);
  }
  {
    Pair p; p.activateWrist();
    p.now = UINT32_MAX - 100;
    for (int i = 0; i < RepFeedbackTiming::QUEUE_CAPACITY + 1; ++i)
      p.wrist.showRepFeedback(p.now);
    assert(logs.find("queue full") != std::string::npos);
    unsigned int sends = fromWrist.size(); fromWrist.clear();
    for (int i = 0; i < RepFeedbackTiming::MAX_ATTEMPTS; ++i) {
      p.tick(RepFeedbackTiming::RETRY_MS);
      sends += fromWrist.size(); fromWrist.clear(); // Outage, bounded across rollover.
    }
    assert(sends == RepFeedbackTiming::MAX_ATTEMPTS);
    assert(logs.find("Unconfirmed/expired") != std::string::npos);
    assert(p.wrist.localActive() && !beeps);
    p.tick(10000); assert(fromWrist.empty());
    p.wrist.showRepFeedback(p.now); p.deliver();
    assert(beeps == 1); // New feedback works after recovery; no old backlog.
  }
  puts("Rep forwarding: loss/retry/dedup, FIFO, reset, handover, reboot, bounds and rollover passed");
}
