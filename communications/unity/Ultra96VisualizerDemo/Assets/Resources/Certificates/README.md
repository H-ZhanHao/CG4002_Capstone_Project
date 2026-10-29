# Pinned Ultra96 certificate

Before running or building the visualizer, copy the public certificate from
the Ultra96 to this directory with the exact filename:

```text
server-cert.bytes
```

For the current temporary deployment, its Ultra96 source is:

```text
/tmp/comms/tls/server-cert.pem
```

The certificate must contain `IP:192.168.0.2` in its Subject Alternative Name;
verify that before copying it into this resource directory.
The contents remain the PEM certificate; the `.bytes` extension makes Unity
import it as a `TextAsset`. Unity embeds this public certificate and pins its SHA-256 fingerprint. The
private file `server-key.pem` must never be copied here or anywhere off the
Ultra96.

The repository ignores `server-cert.pem`, so every developer/build machine
must supply the expected public certificate deliberately.
