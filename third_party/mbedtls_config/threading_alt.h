/* mbedTLS's alternative mutex (MBEDTLS_THREADING_ALT, Windows; mbedtls_user_config.h): a Windows
 * slim reader/writer lock held as its one pointer-sized word (an SRWLOCK, SRWLOCK_INIT being zero),
 * so mbedTLS's C needs no <windows.h>, and whether its init ran. The functions over it are
 * engine/net/http/tls_stream.cpp's. */
#ifndef OPENNOVA_MBEDTLS_THREADING_ALT_H
#define OPENNOVA_MBEDTLS_THREADING_ALT_H

typedef struct mbedtls_threading_mutex_t {
    void *lock;
    char is_valid;
} mbedtls_threading_mutex_t;

#endif
