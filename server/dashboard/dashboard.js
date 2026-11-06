"use strict";

const byId = id => document.getElementById(id);
const charts = new Map();
let serverRun = null, state = {}, nodeInfo = {}, lastMessage = 0, lastState = 0;
let seconds = 20, eventVersion = -1, lostTotal = 0;
const colors = ["#66c9ff", "#ffd37a", "#c3a0ff"];
const pretty = value => String(value || "—").replaceAll("_", " ");
const num = value => Number.isFinite(value) ? value.toFixed(3) : "—";

function ensureNode(id) {
  id = String(id);
  if (charts.has(id)) return charts.get(id);
  const card = document.createElement("article");
  card.className = "node";
  // id only comes from validated numeric node IDs; dynamic text below uses textContent.
  card.innerHTML = `<div class="node-head"><h3></h3><span class="node-status"></span></div>
    <p class="metrics"></p><div class="chart-title">Acceleration <span class="acc-unit"></span></div>
    <canvas aria-label="Acceleration over capture time" role="img"></canvas><div class="readings accel"></div>
    <div class="chart-title">Angular velocity <span class="gyro-unit"></span></div>
    <canvas aria-label="Angular velocity over capture time" role="img"></canvas><div class="readings gyro"></div>`;
  card.querySelector("h3").textContent = `Node ${id}`;
  for (const name of ["accel", "gyro"]) {
    for (const [i, axis] of ["X", "Y", "Z"].entries()) {
      const span = document.createElement("span");
      span.className = ["x", "y", "z"][i];
      span.textContent = `${axis}  —`;
      card.querySelector(`.${name}`).append(span);
    }
  }
  byId("nodes").append(card);
  const device = document.createElement("article");
  device.className = "device-connection";
  device.innerHTML = `<div class="device-heading"><strong></strong><span class="device-link-status waiting" role="status">Waiting</span></div><p class="device-detail"></p>`;
  device.querySelector("strong").textContent = `Node ${id}`;
  byId("device-connections").append(device);
  const entry = {card, device, points: [], epoch: null, latest: null, received: 0};
  charts.set(id, entry);
  return entry;
}

function addSample(sample) {
  const node = ensureNode(sample.node);
  const epoch = `${sample.session}:${sample.generation}:${sample.physical}`;
  const tail = node.points[node.points.length - 1];
  if (epoch !== node.epoch || (tail && sample.sequence < tail.sequence)) node.points = [];
  node.epoch = epoch;
  if (node.latest && sample.id <= node.latest.id) return;
  node.points.push(sample);
  node.latest = sample;
  node.received = performance.now();
  const cutoff = sample.capture_us - 30e6;
  let trim = 0;
  while (trim < node.points.length && node.points[trim].capture_us < cutoff) trim++;
  if (trim) node.points.splice(0, trim);
  if (node.points.length > 6500) node.points.splice(0, node.points.length - 6500);
}

function renderState(batch) {
  if (!batch.state) return;
  state = batch.state;
  nodeInfo = batch.nodes || {};
  lastState = performance.now();
  for (const id of Object.keys(state.connections || {})) ensureNode(id);
  for (const id of Object.keys(nodeInfo)) ensureNode(id);
  byId("capture").textContent = pretty(state.capture);
  byId("capture-detail").textContent = state.capture === "streaming"
    ? "Samples stream directly from the receiver"
    : "Charts pause when devices stop transmitting IMU data";
  byId("confirmed").textContent = pretty(state.confirmed?.exercise);
  byId("confirmed-detail").textContent = state.confirmed
    ? `Last confirmed at ${new Date(state.confirmed.received_at * 1000).toLocaleTimeString()}`
    : "Waiting for sufficient evidence";
  byId("prediction").textContent = pretty(state.prediction?.exercise);
  const p = state.prediction;
  byId("confidence").textContent = p
    ? `${Number.isFinite(p.confidence) ? (p.confidence * 100).toFixed(1) + "% confidence" : "Confidence unavailable"} · window ${p.window_id}`
    : "Waiting for a complete inference window";
  const w = state.window;
  byId("window").textContent = w ? `#${w.id} · ${w.quality.accepted ? "Accepted" : "Rejected"}` : "—";
  byId("window-detail").textContent = w
    ? `${Object.entries(w.quality.sample_counts).map(([id, count]) => `Node ${id}: ${count}/${w.quality.expected_per_node}`).join(" · ")} · skew ${w.quality.max_index_skew_us ?? "—"} µs${w.quality.rejection_reason ? " · " + pretty(w.quality.rejection_reason) : ""}`
    : "Waiting for synchronized samples";
  const d = state.device_event;
  byId("device").textContent = d
    ? `Node ${d.node} · ${new Date(d.time * 1000).toLocaleTimeString()} · sequence ${d.sequence}\n${JSON.stringify(d.result)}`
    : "No device reports received.";
  byId("feedback").textContent = state.feedback === undefined
    ? "Feedback status unavailable" : JSON.stringify(state.feedback, null, 2);
  const events = batch.events || [];
  const version = events.length ? events[events.length - 1].id : 0;
  if (version !== eventVersion) {
    eventVersion = version;
    byId("events").replaceChildren(...events.slice().reverse().map(event => {
      const li = document.createElement("li");
      li.className = event.kind;
      const time = document.createElement("time");
      time.textContent = new Date(event.time * 1000).toLocaleTimeString();
      li.append(time, document.createTextNode(event.message));
      return li;
    }));
  }
}

function draw(canvas, points, offset, end) {
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth, height = canvas.clientHeight;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  const left = 48, right = width - 12, top = 12, bottom = height - 25;
  const start = end - seconds * 1e6;
  const visible = points.filter(p => p.capture_us >= start && p.capture_us <= end);
  let low = Infinity, high = -Infinity;
  for (const point of visible) for (let axis = offset; axis < offset + 3; axis++) {
    low = Math.min(low, point.axes[axis]); high = Math.max(high, point.axes[axis]);
  }
  if (!visible.length) { low = -1; high = 1; }
  const padding = Math.max((high - low) * 0.1, 0.1);
  low -= padding; high += padding;
  ctx.font = "10px system-ui";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = top + (bottom - top) * i / 4;
    ctx.strokeStyle = "#28384e"; ctx.setLineDash([]);
    ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
    ctx.fillStyle = "#a0b2cb"; ctx.textAlign = "right";
    ctx.fillText((high - (high - low) * i / 4).toFixed(1), left - 7, y + 3);
    ctx.textAlign = "center";
    ctx.fillText(`${(-seconds + seconds * i / 4).toFixed(0)}s`, left + (right - left) * i / 4, height - 7);
  }
  if (!visible.length) {
    ctx.fillStyle = "#a0b2cb"; ctx.textAlign = "center";
    ctx.fillText("Waiting for IMU samples", width / 2, height / 2);
    return;
  }
  for (let axis = 0; axis < 3; axis++) {
    ctx.strokeStyle = colors[axis]; ctx.lineWidth = 1.6;
    let previous = null;
    let dashed = null;
    ctx.beginPath();
    for (const point of visible) {
      const x = left + (point.capture_us - start) / (seconds * 1e6) * (right - left);
      const y = bottom - (point.axes[offset + axis] - low) / (high - low) * (bottom - top);
      if (previous && point.sequence === previous.point.sequence + 1 && point.capture_us > previous.point.capture_us) {
        const history = point.historical || previous.point.historical;
        if (dashed !== history) {
          ctx.stroke(); ctx.beginPath(); ctx.setLineDash(history ? [4, 3] : []);
          dashed = history;
        }
        ctx.moveTo(previous.x, previous.y); ctx.lineTo(x, y);
      }
      previous = {point, x, y};
    }
    ctx.stroke();
  }
  ctx.setLineDash([]);
}

function refresh() {
  const now = performance.now();
  const online = lastMessage > 0 && now - lastMessage < 3000;
  byId("connection").textContent = online ? "● Live connection" : "Reconnecting…";
  byId("connection").className = `badge ${online ? "live" : "offline"}`;
  let end = 0;
  for (const node of charts.values()) if (node.latest) end = Math.max(end, node.latest.capture_us);
  for (const [id, node] of charts) {
    const info = nodeInfo[id] || {}, conn = (state.connections || {})[id];
    const link = info.connection;
    const stateAge = (now - lastState) / 1000;
    let linkStatus = link?.status || "waiting";
    let linkReason = link?.reason || "Waiting for device status";
    const seenAge = link?.last_seen_age_s == null ? null : link.last_seen_age_s + stateAge;
    if (linkStatus === "connected" && seenAge >= link.timeout_s) {
      linkStatus = "disconnected";
      linkReason = "Heartbeat / IMU data timed out";
    }
    if (!online) {
      linkStatus = "unknown";
      linkReason = "Server unreachable; device status unavailable";
    }
    const badge = node.device.querySelector(".device-link-status");
    badge.textContent = {connected: "Connected", disconnected: "Disconnected",
      connecting: "Connecting", waiting: "Waiting", unknown: "Unknown"}[linkStatus];
    badge.className = `device-link-status ${linkStatus}`;
    const heartbeatAge = info.heartbeat_age_s == null ? null : info.heartbeat_age_s + stateAge;
    node.device.querySelector(".device-detail").textContent = [linkReason,
      heartbeatAge == null ? "Heartbeat: not received yet" : `Last heartbeat: ${Math.max(0, heartbeatAge).toFixed(1)}s ago`,
      seenAge == null ? "" : `Last heartbeat / IMU packet: ${Math.max(0, seenAge).toFixed(1)}s ago`
    ].filter(Boolean).join(" · ");
    const latest = node.latest;
    const liveAge = (now - node.received) / 1000;
    const snapshotAge = info.sample_age_s == null ? Infinity : info.sample_age_s + (now - lastState) / 1000;
    // On first connection, old buffered samples must not be called live.
    const age = latest && info.sample && latest.id === info.sample.id ? snapshotAge : liveAge;
    const isCurrentSession = !latest || link?.session === latest.session;
    const fresh = linkStatus === "connected" && latest && age < 1.5 && !latest.historical && isCurrentSession;
    node.card.querySelector(".node-status").textContent = !online ? "Server disconnected"
      : linkStatus === "disconnected" ? "Node disconnected"
      : linkStatus !== "connected" ? "Waiting for device" : fresh ? "Live IMU"
      : state.capture === "streaming" ? "Waiting for fresh IMU" : "Connected · IMU paused";
    const heartbeat = info.heartbeat;
    node.card.querySelector(".metrics").textContent = [
      `Session ${conn?.session ?? latest?.session ?? "—"}`,
      `${fresh && info.sample_rate_hz != null ? info.sample_rate_hz : "—"} Hz`,
      latest ? `Sample age ${Number.isFinite(age) ? Math.max(0, age).toFixed(1) : "—"}s · seq ${latest.sequence}` : "No samples yet",
      conn?.offset_us != null ? `Sync offset ${conn.offset_us} µs · RTT ${conn.round_trip_us} µs` : "Clock not synchronized",
      heartbeat ? `Heartbeat overflow ${heartbeat.overflow_count}` : "",
      latest ? `Flags 0x${latest.status.toString(16)}${latest.historical ? " · historical sample" : ""}` : ""
    ].filter(Boolean).join(" · ");
    node.card.querySelector(".acc-unit").textContent = latest ? latest.physical ? "m/s²" : "raw counts" : "";
    node.card.querySelector(".gyro-unit").textContent = latest ? latest.physical ? "rad/s" : "raw counts" : "";
    for (const [group, offset] of [["accel", 0], ["gyro", 3]]) {
      node.card.querySelectorAll(`.${group} span`).forEach((span, i) => {
        span.textContent = `${["X", "Y", "Z"][i]}  ${num(latest?.axes[offset + i])}`;
      });
    }
    node.card.querySelectorAll("canvas").forEach((canvas, i) => draw(canvas, node.points, i * 3, end));
  }
}

byId("range").addEventListener("change", event => { seconds = Number(event.target.value); });
ensureNode(1); ensureNode(2);
const stream = new EventSource("/events");
stream.onmessage = event => {
  const batch = JSON.parse(event.data);
  lastMessage = performance.now();
  if (batch.run !== serverRun || batch.reset) {
    for (const node of charts.values()) { node.points = []; node.latest = null; node.epoch = null; }
    state = {}; nodeInfo = {}; eventVersion = -1; lostTotal = 0;
  }
  serverRun = batch.run;
  byId("demo").hidden = !batch.demo;
  lostTotal += batch.lost_samples;
  byId("loss").hidden = lostTotal === 0;
  byId("loss").textContent = `${lostTotal} display samples expired while this browser was behind. Sensor acquisition and inference continue independently.`;
  for (const sample of batch.samples) addSample(sample);
  renderState(batch);
};
stream.onerror = () => { lastMessage = 0; };
setInterval(refresh, 100);
