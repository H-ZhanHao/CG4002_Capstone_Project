# Dynamic capture integration for server_rep_fall.py

Updated on 2026-11-06 using the capture changes in
`communications/ultra96/server_one_node.py`.

## Files to deploy

Update these files beside the existing Ultra96 server dependencies:

- `server_rep_fall.py`
- `window_assembler_updated.py`

Also copy `communications/ultra96/dynamic_capture.py` into that same directory.
That copy is intentionally left to the user; it was not placed in `server/`
during this merge. The server needs that file before it can start.

Continue launching the existing entry point with the usual model/dashboard
options:

```bash
python server_rep_fall.py
```

No `--one-node` flag or alternate capture mode is needed. The optional
`--node-wait-ms` setting defaults to 250 ms of capture-time progress, matching
the communications implementation.

## Changes

- Each synchronized node can arm/start independently.
- A second node can join live on the existing capture/window timeline.
- Individual command failures, disconnects, or stalled streams are isolated.
- Sample submission passes through the coordinator, so obsolete sessions and
  unacknowledged streams cannot contribute windows.
- The dashboard retains its existing fields, with additive per-node `phase`
  and run-wide `streaming_nodes` metadata.
- The existing `window_assembler_updated.py` now releases windows from available
  usable nodes, with bounded waiting for a starting/lagging producer. It exposes
  `node_joined` and `node_lost` for the shared coordinator.
- The active assembler's real-sample policy is preserved: no zero padding and
  no trimming real samples to force an exact count. Existing per-node quality
  validation and exact retransmission deduplication remain in use.

The AI pipeline import remains `from inference_pipeline import InferencePipeline`.
Startup tracing/resource cleanup, dashboard telemetry, model file checks,
model-window validation, rep-control setup, fall event delivery and idle
callbacks remain in place. No firmware profiles were changed in this merge.

## AI handoff still required

The supplied `late_fusion_vart.py` currently iterates over nodes 1 and 2 in
`infer_window` and raises `ValueError("missing IMU Node ...")` if either is
absent. The communications integration can deliver single-node windows, but
that supplied model runtime cannot yet classify them.

The AI teammate must adapt their pipeline/model to consume `{1}`, `{2}`, or
`{1, 2}` keys in `window.samples_by_node`. `quality.available_nodes` and
`quality.node_rejection_reasons` describe availability and rejected coverage.
Do not replace the AI pipeline with `dynamic_inference.py`; it is not imported
by this active server. The AI files were preserved unchanged during the merge.

## Validation

### Feedback ACK receiver crash fix

The shared `dynamic_capture.py` now dispatches feedback command ACKs directly
to `FeedbackDelivery.acknowledge`. A leftover `super().control_acknowledged`
call referred to the removed legacy coordinator method, causing an
`AttributeError` after an activity LED (or rep/idle feedback) acknowledgement.
That terminated the receiving worker and caused a reconnect during capture.

Deploy the corrected `communications/ultra96/dynamic_capture.py` beside the
running `server_rep_fall.py` and restart that service. This particular fix does
not require firmware changes. Regression coverage reproduces the original
failure through `process_control_ack`, then checks activity, duplicate,
partner-node, rep, and malformed feedback ACKs. Fourteen integration tests
now pass.

```bash
python -m unittest discover -s server -p test_one_node_integration.py -q
```

The test adds the communications directory to its own import path to resolve
the separately supplied `dynamic_capture.py`; production startup still needs
the user's copy alongside the server.

Fourteen tests pass, covering either node alone, late joining, fusion windows,
disconnect/reconnect fallback, stale-session rejection, an unsynchronized or
silent partner, STOP/rearming, real-only sample count policy, CLI validation,
and startup wiring with the teammate's pipeline/dashboard/rep services.
Startup services/model runners are doubled in the startup test; it does not
program the DPU or validate real inference. AI, rep/fall delivery and dashboard
modules were also checked unchanged byte-for-byte against the pre-merge files.
