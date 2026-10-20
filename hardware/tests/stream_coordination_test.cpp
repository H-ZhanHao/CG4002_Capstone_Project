#include <assert.h>
#include <stdio.h>
#include "StreamCoordination.h"

static void fill(SensorHistory& history, uint32_t offset, uint32_t from, uint32_t to) {
  for (uint32_t elapsed = 0; elapsed <= to - from; elapsed += 20) {
    SensorSample sample = {};
    sample.timestampMs = offset + from + elapsed;
    sample.acceleration[0] = (float)(from + elapsed);
    sample.gyro[2] = (float)offset; // Distinguish the two independent payloads.
    assert(history.push(sample));
  }
}

static GenericPeakEvent peak(uint32_t time) {
  GenericPeakEvent event;
  event.peakMs = time;
  event.detectedMs = time + 40;
  return event;
}

static bool measure(StreamCoordinator& coordinator, NodeStreamControl& node,
                    uint32_t offset, uint32_t sent) {
  const auto reply = node.clockReply(7, sent + offset + 10, sent + offset + 14);
  return coordinator.observeClock(reply, sent, sent + 24);
}

int main() {
  SensorHistory hipHistory, wristHistory;
  WindowScheduler hipWindows, wristWindows;
  NodeStreamControl hip(hipWindows, hipHistory), wrist(wristWindows, wristHistory);
  StreamCoordinator coordinator;
  hip.begin(HIP, 11);
  wrist.begin(WRIST, 22);
  coordinator.begin(99);
  const uint32_t hipOffset = 1000, wristOffset = 40000;
  fill(hipHistory, hipOffset, 0, 6120);
  fill(wristHistory, wristOffset, 0, 6120);
  assert(hip.peak(peak(hipOffset + 6000)));
  assert(wrist.peak(peak(wristOffset + 5980))); // Earlier physical peak, received second.
  StreamTrigger hipTrigger, wristTrigger, retried;
  assert(hip.pollTrigger(hipOffset + 6040, hipTrigger));
  assert(!hip.pollTrigger(hipOffset + 6539, retried));
  assert(hip.pollTrigger(hipOffset + 6540, retried) && retried.peakMs == hipTrigger.peakMs);
  assert(!hip.peak(peak(hipOffset + 6100))); // First request is retained.
  assert(wrist.pollTrigger(wristOffset + 6040, wristTrigger));
  assert(hipWindows.status().queued == 0 && wristWindows.status().queued == 0);
  assert(coordinator.trigger(hipTrigger, 6050) == StreamTriggerResult::NOT_READY);
  assert(measure(coordinator, hip, hipOffset, 5900));
  assert(coordinator.trigger(hipTrigger, 6050) == StreamTriggerResult::NOT_READY);
  assert(measure(coordinator, wrist, wristOffset, 5900));
  assert(coordinator.clockUncertaintyMs(HIP) == 11);
  assert(coordinator.trigger(hipTrigger, 6050) == StreamTriggerResult::CREATED);
  assert(coordinator.trigger(wristTrigger, 6051) == StreamTriggerResult::EXISTING);
  StreamStart hipStart, wristStart, retry;
  assert(coordinator.pollStart(HIP, 6050, hipStart));
  assert(coordinator.pollStart(WRIST, 6050, wristStart));
  assert(hipStart.coordinator == 99 && hipStart.session == 1 && wristStart.session == 1);
  assert(hipStart.localT0 == hipOffset + 6000 && wristStart.localT0 == wristOffset + 6000);
  assert(hip.join(hipStart, hipOffset + 6120) == StreamJoinResult::STARTED);
  assert(wrist.join(wristStart, wristOffset + 6120) == StreamJoinResult::STARTED);
  assert(!hip.pollTrigger(hipOffset + 7000, retried));
  assert(!wrist.peak(peak(wristOffset + 6200)));

  // Lost ACK: start is retried; the receiver preserves sequence zero and its queue.
  assert(!coordinator.pollStart(HIP, 6549, retry));
  assert(coordinator.pollStart(HIP, 6550, retry));
  assert(hip.join(retry, hipOffset + 6550) == StreamJoinResult::ALREADY_RUNNING);
  assert(hipWindows.status().queued == 1 && hipWindows.status().nextSequence == 1);
  assert(coordinator.acknowledge(retry, StreamJoinResult::ALREADY_RUNNING));
  assert(!coordinator.bothStarted());
  assert(coordinator.acknowledge(wristStart, StreamJoinResult::STARTED));
  assert(coordinator.bothStarted() && !coordinator.pollStart(HIP, 10000, retry));
  auto conflicting = hipStart;
  ++conflicting.localT0;
  assert(hip.join(conflicting, hipOffset + 6550) == StreamJoinResult::CONFLICT);
  assert(!coordinator.acknowledge(conflicting, StreamJoinResult::STARTED));
  ++conflicting.session;
  assert(hip.join(conflicting, hipOffset + 6550) == StreamJoinResult::BUSY);
  assert(!hip.end(99, 2));
  ScheduledWindow hipFrame, wristFrame;
  assert(hipWindows.take(hipFrame) && wristWindows.take(wristFrame));
  assert(hipFrame.info.coordinator == wristFrame.info.coordinator);
  assert(hipFrame.info.session == wristFrame.info.session && hipFrame.info.sequence == 0);
  assert(hipFrame.info.window.count == 251 && wristFrame.info.window.count == 251);
  for (size_t i = 0; i < hipFrame.info.window.count; ++i) {
    assert(hipFrame.samples[i].timestampMs - hipOffset == wristFrame.samples[i].timestampMs - wristOffset);
    assert(hipFrame.samples[i].acceleration[0] == wristFrame.samples[i].acceleration[0]);
    assert(hipFrame.samples[i].gyro[2] != wristFrame.samples[i].gyro[2]);
  }
  fill(hipHistory, hipOffset, 6140, 7000);
  fill(wristHistory, wristOffset, 6140, 7000);
  assert(hipWindows.update(hipOffset + 7000, hipHistory));
  assert(wristWindows.update(wristOffset + 7000, wristHistory));
  assert(hipWindows.take(hipFrame) && wristWindows.take(wristFrame));
  assert(hipFrame.info.sequence == 1 && wristFrame.info.sequence == 1);
  assert(hipFrame.info.window.endMs - hipOffset == wristFrame.info.window.endMs - wristOffset);

  // Clock updates cannot move a session's frozen starts.
  auto oldProbe = hip.clockReply(8, hipOffset + 6912, hipOffset + 6916);
  oldProbe.armed = true; // Reply to a probe taken while armed, delivered during streaming.
  assert(coordinator.observeClock(oldProbe, 6900, 6924));
  assert(coordinator.getStart(HIP, retry) && retry.localT0 == hipStart.localT0);

  // Close/rearm rejects delayed starts and triggers, and duplicate END is harmless.
  assert(coordinator.close(99, 1) && coordinator.close(99, 1));
  assert(hip.end(99, 1) && wrist.end(99, 1));
  assert(hip.join(hipStart, hipOffset + 7000) == StreamJoinResult::STALE_TARGET);
  const uint32_t armAfterEnd = hip.identity().arm;
  assert(hip.end(99, 1) && hip.identity().arm == armAfterEnd);
  hip.arm();
  wrist.arm();
  assert(hip.peak(peak(hipOffset + 7000)));
  assert(hip.end(99, 1)); // A retransmission must not disarm or cancel the new request.
  assert(hip.pendingTrigger(retried) && hipWindows.status().state == WindowStreamState::ARMED);
  auto staleProbe = oldProbe;
  staleProbe.receivedMs = hipOffset + 7110;
  staleProbe.sentMs = hipOffset + 7114;
  assert(!coordinator.observeClock(staleProbe, 7100, 7124));
  assert(measure(coordinator, hip, hipOffset, 7100));
  assert(measure(coordinator, wrist, wristOffset, 7100));
  assert(coordinator.trigger(hipTrigger, 7200) == StreamTriggerResult::STALE_TARGET);

  // Wrist wins the next session; hip can join without a matching local peak.
  assert(wrist.peak(peak(wristOffset + 7100)));
  assert(wrist.pollTrigger(wristOffset + 7200, wristTrigger));
  assert(coordinator.trigger(wristTrigger, 7200) == StreamTriggerResult::CREATED);
  assert(coordinator.getStart(HIP, hipStart) && coordinator.getStart(WRIST, wristStart));
  assert(hipStart.session == 2 && hipStart.localT0 == hipOffset + 7100);

  // Stale/slow clock measurements prevent unsynchronized starts.
  StreamCoordinator unsynced;
  unsynced.begin(55);
  auto probe = hip.clockReply(9, 8010, 8014);
  assert(!unsynced.observeClock(probe, 7000, 7100)); // 96 ms network RTT.
  assert(!unsynced.observeClock(probe, 7000, 7002)); // Impossible processing time.
  assert(measure(unsynced, hip, hipOffset, 7100));
  assert(measure(unsynced, wrist, wristOffset, 7100));
  assert(!unsynced.ready(HIP, 37125));
  assert(unsynced.trigger(wristTrigger, 37125) == StreamTriggerResult::NOT_READY);
  assert(!unsynced.observeClock(probe, 6900, 6924)); // Out-of-order old probe.

  // A quiet second node, failed delivery and reboot do not invent an activation.
  hip.stop(); wrist.stop(); hip.arm(); wrist.arm();
  coordinator.begin(100);
  assert(measure(coordinator, hip, hipOffset, 7200));
  assert(measure(coordinator, wrist, wristOffset, 7200));
  assert(wrist.peak(peak(wristOffset + 7300)));
  assert(wrist.pollTrigger(wristOffset + 7400, wristTrigger));
  assert(!hip.pendingTrigger(retried));
  assert(coordinator.trigger(wristTrigger, 7400) == StreamTriggerResult::CREATED);
  assert(coordinator.getStart(HIP, hipStart));
  fill(hipHistory, hipOffset, 7020, 7500);
  assert(hip.join(hipStart, hipOffset + 7500) == StreamJoinResult::STARTED);
  assert(hipWindows.take(hipFrame) && hipFrame.info.window.status == SensorWindowStatus::READY);
  assert(!coordinator.bothStarted());
  hip.begin(HIP, 33);
  assert(hip.join(hipStart, hipOffset + 7500) == StreamJoinResult::STALE_TARGET);
  assert(!measure(coordinator, hip, hipOffset, 7500)); // Explicitly close interrupted session first.

  // UINT32 rollover works for clock probes, translated t0 and node commands.
  hipHistory.clear(); wristHistory.clear();
  hip.begin(HIP, 44); wrist.begin(WRIST, 55); coordinator.begin(101);
  const uint32_t wrap = UINT32_MAX - 5999;
  fill(hipHistory, wrap, 0, 6200);
  fill(wristHistory, 10000, 0, 6200);
  assert(measure(coordinator, hip, wrap, 5990)); // Probe crosses local rollover.
  assert(measure(coordinator, wrist, 10000, 5990));
  assert(hip.peak(peak(wrap + 6100)));
  assert(hip.pollTrigger(wrap + 6140, hipTrigger));
  assert(coordinator.trigger(hipTrigger, 6150) == StreamTriggerResult::CREATED);
  assert(coordinator.getStart(HIP, hipStart) && coordinator.getStart(WRIST, wristStart));
  assert(hipStart.localT0 == wrap + 6100 && wristStart.localT0 == 16100);
  assert(hip.join(hipStart, wrap + 6200) == StreamJoinResult::STARTED);
  assert(wrist.join(wristStart, 16200) == StreamJoinResult::STARTED);
  assert(hipWindows.take(hipFrame) && wristWindows.take(wristFrame));
  assert(hipFrame.info.window.copied() && wristFrame.info.window.copied());
  puts("Two-node coordination, clocks, simultaneous triggers, retries, stale commands and rollover tests passed");
}
