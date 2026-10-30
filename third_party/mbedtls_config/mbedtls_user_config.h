/* OpenNova's one change to mbedTLS's default configuration (CMakeLists.txt beside this file):
 * thread safety, so TLS connections on several threads share the PSA crypto core (TLS 1.3's key
 * store) safely. POSIX takes mbedTLS's own pthread mutexes; Windows takes the alternative mutexes
 * threading_alt.h declares, which engine/net/http/tls_stream.cpp hands mbedTLS
 * (mbedtls_threading_set_alt) before its first call. */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif
