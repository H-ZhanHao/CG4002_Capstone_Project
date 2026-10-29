# Plain-TCP Private-Relay Profile Archive

The former PlatformIO environments `hub3_private_relay` and
`hub6_private_relay` were created before the Phase 4 TLS transport was
implemented. They connected Hub 3 and Hub 6 to the laptop relay over the
private DIR-842 network using the normal framed application protocol over
plain TCP.

Their effective build flags were:

```text
APP_NODE_ROLE=1
APP_NODE_ID=3 or 6
APP_HUB_LOCAL_ONLY=1
```

The profiles were useful while establishing the private-router route, laptop
relay, SSH tunnel, concurrent Ultra96 sessions, and local mock-data stream.
They did not enable `APP_TRANSPORT_TLS`, so the FireBeetle-to-laptop hop was
not encrypted or authenticated.

After the TLS implementation was physically verified, the plain-TCP profiles
became obsolete for the final demo. They were removed to avoid accidentally
flashing an insecure historical build. Their successors are:

```text
hub3_private_tls
hub6_private_tls
```

These are the former `hub3_demo_tls` and `hub6_demo_tls` environments under
clearer names. Only the environment names changed: their build flags and
firmware behavior remain identical.
