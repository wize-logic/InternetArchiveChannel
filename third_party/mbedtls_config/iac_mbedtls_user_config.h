/* Internet Archive Channel: mbedTLS 3.6 user config overrides for Nintendo Wii.
 * Applied on top of the default mbedtls_config.h via MBEDTLS_USER_CONFIG_FILE. */
#ifndef IAC_MBEDTLS_USER_CONFIG_H
#define IAC_MBEDTLS_USER_CONFIG_H

/* No BSD sockets or POSIX timers on libogc; we provide our own BIO callbacks. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

/* No /dev/urandom: entropy comes from mbedtls_hardware_poll() in src/net/entropy.c. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* No POSIX monotonic clock detection on libogc: mbedtls_ms_time() lives in src/net/tls.c. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* The Wii RTC is local time and often wrong; tls.c installs a corrected UTC clock
 * via mbedtls_platform_set_time() once a server Date: header has been seen. */
#define MBEDTLS_PLATFORM_TIME_ALT

/* Several network threads use TLS at once. TLS 1.3 goes through PSA's global key store and
 * the shared DRBG/entropy, which are only thread-safe with MBEDTLS_THREADING_C. Without it,
 * parallel downloads failed ~1 MB in, on both the Wii and Dolphin. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

/* PSA persistent key storage is unused and would need a filesystem at init. */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

/* Record buffers: 16 KB in (server may send full records), 4 KB out. */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* Trim protocols we never need on the client. */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C

#endif
