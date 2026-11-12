"""Small verified-TLS phone simulator for Stage 3; does not count real reps."""
import argparse
import asyncio
import json
import ssl
import uuid

import websockets
from visualizer_protocol import REP_EXERCISES, parse_rep_message


async def receive(websocket, kind):
    while True:
        message = json.loads(await asyncio.wait_for(websocket.recv(), 4))
        print(json.dumps(message), flush=True)
        if message.get("type") == kind:
            return message


async def simulate(args):
    context = ssl.create_default_context(cafile=args.ca_cert)
    async with websockets.connect(args.url, ssl=context, max_size=16384, max_queue=32) as websocket:
        welcome = await receive(websocket, "rep_welcome")
        identity = args.session_id or uuid.uuid4().hex
        sequence = 0

        async def send(kind, count):
            nonlocal sequence
            sequence += 1
            message = {"v": 1, "type": kind, "run_id": welcome["run_id"], "seq": sequence,
                       "rep_session_id": identity, "rep_index": count, "exercise": args.exercise}
            payload = json.dumps(message)
            parse_rep_message(payload)
            await websocket.send(payload)
            reply = await receive(websocket, "rep_result")
            if reply["status"] == "rejected":
                raise RuntimeError(reply["reason"])
            if any(node["status"] == "pending" for node in reply["nodes"]):
                await receive(websocket, "rep_delivery")

        await send("rep_sync" if args.sync else "rep_session_start", args.baseline)
        print("rep_session_id=" + identity, flush=True)
        for count in args.counts:
            await send("rep_event", count)
            await asyncio.sleep(args.interval)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="existing wss:// endpoint")
    parser.add_argument("--ca-cert", required=True, help="trusted gateway certificate PEM")
    parser.add_argument("--exercise", choices=sorted(REP_EXERCISES), default="squat")
    parser.add_argument("--session-id", help="new ID, or existing ID with --sync")
    parser.add_argument("--sync", action="store_true", help="reconnect baseline; never flashes")
    parser.add_argument("--baseline", type=int, default=0)
    parser.add_argument("--counts", default="1,2,3", help="cumulative counts, e.g. 1,1,4")
    parser.add_argument("--interval", type=float, default=0.5)
    args = parser.parse_args()
    if not args.url.startswith("wss://") or (args.sync and not args.session_id):
        parser.error("use WSS and supply --session-id for --sync")
    try:
        args.counts = [int(value) for value in args.counts.split(",") if value]
    except ValueError:
        parser.error("--counts must be comma-separated integers")
    if len(args.counts) > 64 or not 0.25 <= args.interval <= 60:
        parser.error("at most 64 counts; interval must be 0.25..60 seconds")
    asyncio.run(simulate(args))


if __name__ == "__main__":
    main()
