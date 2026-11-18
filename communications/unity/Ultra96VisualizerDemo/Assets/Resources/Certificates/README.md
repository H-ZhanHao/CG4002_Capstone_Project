# Pinned gateway public certificate

Reviewed on 16 November 2026. Before running/building this Unity client, place
the gateway's actual **public** PEM certificate here as `server-cert.bytes`.
The extension makes Unity import PEM text as a TextAsset; the client loads
`Certificates/server-cert` and pins its exact SHA-256 fingerprint and dates.

Use the certificate supplied to the running Ultra96 gateway's `--tls-cert`
argument. There is no fixed present-day `/tmp` source path. Check its IP SAN
is `192.168.0.2` for the configured phone URL and its dates are valid. The
wearable-facing laptop relay has a separate certificate; do not use it here.
Never copy `server-key.pem` or another private key into Unity.

Both `server-cert.pem` and `server-cert.bytes` and their metadata are ignored by
Git. Each developer/build machine must provision the intended public certificate.
Replacing the gateway certificate requires updating this resource and rebuilding/
reinstalling the client. No trust-all validation fallback is supported.

See [Unity provisioning](../../../README.md#supply-the-pinned-public-certificate)
and [Network setup](../../../../../docs/NETWORK_SETUP.md#3-certificates-two-separate-endpoints)
for the copy command, endpoint identities and certificate preparation.
