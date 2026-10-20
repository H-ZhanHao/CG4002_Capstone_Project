/**
 * Copy to demo_tls_config.h and replace the values for your laptop relay.
 * Keep the real file private. The certificate must contain the expected
 * identity in its Subject Alternative Name extension.
 */
#ifndef DEMO_TLS_CONFIG_H
#define DEMO_TLS_CONFIG_H

#define DEMO_TLS_SERVER_IDENTITY "demo-relay.local"
#define DEMO_TLS_CA_CERTIFICATE \
    "-----BEGIN CERTIFICATE-----\n" \
    "REPLACE_WITH_YOUR_CA_CERTIFICATE_BASE64\n" \
    "-----END CERTIFICATE-----\n"

#endif
