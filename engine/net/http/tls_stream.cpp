#include "tls_stream.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <utility>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h> // MBEDTLS_ERR_NET_SEND_FAILED, MBEDTLS_ERR_NET_RECV_FAILED
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

// The one TU that sees mbedTLS (scripts/lint/include_graph_check.py): TLS over the stream seam,
// the client's trust the OS's store. Infrastructure with no retail counterpart (retail speaks no
// TLS), so it carries no witness cite (scripts/lint/maturity_baseline.json
// citation_allowlist_engine).

namespace opennova::net {

namespace {

#if defined(MBEDTLS_THREADING_ALT)
// The alternative mutexes mbedTLS takes on Windows (third_party/mbedtls_config/threading_alt.h):
// a slim reader/writer lock held as its pointer-sized word.
PSRWLOCK srw_of(mbedtls_threading_mutex_t *mutex) {
	return reinterpret_cast<PSRWLOCK>(&mutex->lock);
}
void srw_init(mbedtls_threading_mutex_t *mutex) {
	if (mutex == nullptr) return;
	InitializeSRWLock(srw_of(mutex));
	mutex->is_valid = 1;
}
void srw_free(mbedtls_threading_mutex_t *mutex) {
	if (mutex != nullptr) mutex->is_valid = 0;
}
int srw_lock(mbedtls_threading_mutex_t *mutex) {
	if (mutex == nullptr || !mutex->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
	AcquireSRWLockExclusive(srw_of(mutex));
	return 0;
}
int srw_unlock(mbedtls_threading_mutex_t *mutex) {
	if (mutex == nullptr || !mutex->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
	ReleaseSRWLockExclusive(srw_of(mutex));
	return 0;
}
#endif

// mbedTLS's process-wide start, once and before any other call of it: the mutexes (Windows), then
// the PSA crypto core TLS 1.3 runs on.
bool tls_start(std::string &error) {
	static std::once_flag once;
	static psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	std::call_once(once, [] {
#if defined(MBEDTLS_THREADING_ALT)
		mbedtls_threading_set_alt(srw_init, srw_free, srw_lock, srw_unlock);
#endif
		status = psa_crypto_init();
	});
	if (status != PSA_SUCCESS) {
		error = "TLS could not start (PSA status " + std::to_string(status) + ")";
		return false;
	}
	return true;
}

std::string describe(int ret) {
	char text[200] = {};
	mbedtls_strerror(ret, text, sizeof(text));
	return text;
}

// The certificates in a chain mbedTLS holds (an empty chain reads as none).
int chain_count(const mbedtls_x509_crt &chain) {
	int count = 0;
	for (const mbedtls_x509_crt *cert = &chain; cert != nullptr && cert->version != 0;
			cert = cert->next)
		++count;
	return count;
}

// The OS's trust store into `chain`: Windows' ROOT certificate store (the current user's view,
// which holds the machine's), else the system bundle, the first of the distributions' paths that
// gives any, else the certificates directory. The count in the chain after.
int load_os_trust(mbedtls_x509_crt &chain) {
#if defined(_WIN32)
	HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
	if (store == nullptr) return 0;
	PCCERT_CONTEXT cert = nullptr;
	while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
		// A certificate this TLS cannot read (an algorithm it lacks) is left out, never fatal.
		if ((cert->dwCertEncodingType & X509_ASN_ENCODING) != 0)
			mbedtls_x509_crt_parse_der(&chain, cert->pbCertEncoded, cert->cbCertEncoded);
	}
	CertCloseStore(store, 0);
#else
	static const char *const kBundles[] = {
		"/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
		"/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
		"/etc/ssl/ca-bundle.pem",                            // openSUSE
		"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS, RHEL 7+
		"/etc/ssl/cert.pem",                                 // Alpine, macOS
	};
	for (const char *path : kBundles) {
		// A positive return counts the certificates it could not read; the rest are in.
		mbedtls_x509_crt_parse_file(&chain, path);
		if (chain_count(chain) > 0) break;
	}
	if (chain_count(chain) == 0) mbedtls_x509_crt_parse_path(&chain, "/etc/ssl/certs");
#endif
	return chain_count(chain);
}

// One TLS session over a connected stream, itself a stream. mbedTLS's contexts point at one
// another and at this object (the session's I/O), so it never moves: it lives on the heap.
class TlsStream final : public IStreamSocket {
public:
	explicit TlsStream(std::unique_ptr<IStreamSocket> inner) : inner_(std::move(inner)) {
		mbedtls_ssl_init(&ssl_);
		mbedtls_ssl_config_init(&config_);
		mbedtls_entropy_init(&entropy_);
		mbedtls_ctr_drbg_init(&drbg_);
		mbedtls_x509_crt_init(&trusted_);
		mbedtls_x509_crt_init(&own_chain_);
		mbedtls_pk_init(&own_key_);
	}

	~TlsStream() override {
		if (open_) {
			// The close's alert, sent at once; a peer that is gone takes none.
			wait_ms_ = 0;
			mbedtls_ssl_close_notify(&ssl_);
		}
		mbedtls_ssl_free(&ssl_);
		mbedtls_ssl_config_free(&config_);
		mbedtls_ctr_drbg_free(&drbg_);
		mbedtls_entropy_free(&entropy_);
		mbedtls_x509_crt_free(&trusted_);
		mbedtls_x509_crt_free(&own_chain_);
		mbedtls_pk_free(&own_key_);
	}

	TlsStream(const TlsStream &) = delete;
	TlsStream &operator=(const TlsStream &) = delete;

	// A client for `host`, trusting `trust`, the server's certificate required.
	bool client(const std::string &host, const TlsTrust &trust, std::string &error) {
		if (!start(MBEDTLS_SSL_IS_CLIENT, error)) return false;
		if (trust.ca_pem.empty()) {
			if (load_os_trust(trusted_) == 0) {
				error = "the OS's trust store gave no certificate";
				return false;
			}
		} else {
			// PEM text is read with its NUL counted.
			const int ret = mbedtls_x509_crt_parse(&trusted_,
					reinterpret_cast<const unsigned char *>(trust.ca_pem.c_str()),
					trust.ca_pem.size() + 1);
			if (ret < 0 || chain_count(trusted_) == 0) {
				error = "the trusted certificates do not read: " + describe(ret);
				return false;
			}
		}
		mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_REQUIRED);
		mbedtls_ssl_conf_ca_chain(&config_, &trusted_, nullptr);
		if (!setup(error)) return false;
		const int ret = mbedtls_ssl_set_hostname(&ssl_, host.c_str());
		if (ret != 0) {
			error = "TLS host name " + host + ": " + describe(ret);
			return false;
		}
		return true;
	}

	// A server presenting `cert_pem` with `key_pem`.
	bool server(const std::string &cert_pem, const std::string &key_pem, std::string &error) {
		if (!start(MBEDTLS_SSL_IS_SERVER, error)) return false;
		int ret = mbedtls_x509_crt_parse(&own_chain_,
				reinterpret_cast<const unsigned char *>(cert_pem.c_str()), cert_pem.size() + 1);
		if (ret != 0) {
			error = "the server's certificates do not read: " + describe(ret);
			return false;
		}
		ret = mbedtls_pk_parse_key(&own_key_, reinterpret_cast<const unsigned char *>(key_pem.c_str()),
				key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &drbg_);
		if (ret != 0) {
			error = "the server's key does not read: " + describe(ret);
			return false;
		}
		ret = mbedtls_ssl_conf_own_cert(&config_, &own_chain_, &own_key_);
		if (ret != 0) {
			error = "the server's certificate and key: " + describe(ret);
			return false;
		}
		return setup(error);
	}

	// The handshake, in waits of at most kTlsWaitSliceMs between looks at `cancelled`, the whole at
	// most `timeout_ms` (0: no bound).
	bool handshake(int timeout_ms, const std::atomic<bool> *cancelled, std::string &error) {
		const auto begun = std::chrono::steady_clock::now();
		for (;;) {
			if (cancelled != nullptr && cancelled->load()) {
				error = "cancelled";
				return false;
			}
			int slice = kTlsWaitSliceMs;
			if (timeout_ms > 0) {
				const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - begun)
										   .count();
				if (spent >= timeout_ms) {
					error = "TLS handshake timed out";
					return false;
				}
				slice = static_cast<int>(std::min<long long>(slice, timeout_ms - spent));
			}
			wait_ms_ = slice;
			const int ret = mbedtls_ssl_handshake(&ssl_);
			if (ret == 0) {
				open_ = true;
				return true;
			}
			if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
			error = "TLS handshake failed: " + describe(ret);
			if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) error += verify_reasons();
			return false;
		}
	}

	bool send_all(const uint8_t *data, std::size_t len) override {
		std::size_t sent = 0;
		while (sent < len) {
			const std::size_t part = std::min<std::size_t>(len - sent, INT_MAX);
			const int ret = mbedtls_ssl_write(&ssl_, data + sent, part);
			if (ret <= 0) return false; // the inner send blocks: no want-write comes back
			sent += static_cast<std::size_t>(ret);
		}
		return true;
	}

	int recv(uint8_t *buf, std::size_t cap, int wait_ms) override {
		wait_ms_ = wait_ms < 0 ? 0 : wait_ms;
		const std::size_t want = std::min<std::size_t>(cap, INT_MAX);
		for (;;) {
			const int ret = mbedtls_ssl_read(&ssl_, buf, want);
			if (ret > 0) return ret;
			// The peer's close: its alert, or its connection's end (mbedTLS reads it as 0).
			if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
			// No record yet: the wait ran out (a partial record waits in the session).
			if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
				return kStreamRecvTimedOut;
			// A TLS 1.3 server's session ticket carries no data: read on.
			if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
			return -1;
		}
	}

private:
	bool start(int endpoint, std::string &error) {
		static constexpr char kPersonal[] = "opennova tls";
		int ret = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
				reinterpret_cast<const unsigned char *>(kPersonal), sizeof(kPersonal) - 1);
		if (ret != 0) {
			error = "TLS random source: " + describe(ret);
			return false;
		}
		ret = mbedtls_ssl_config_defaults(&config_, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM,
				MBEDTLS_SSL_PRESET_DEFAULT);
		if (ret != 0) {
			error = "TLS configuration: " + describe(ret);
			return false;
		}
		mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &drbg_);
		return true;
	}

	bool setup(std::string &error) {
		const int ret = mbedtls_ssl_setup(&ssl_, &config_);
		if (ret != 0) {
			error = "TLS session: " + describe(ret);
			return false;
		}
		mbedtls_ssl_set_bio(&ssl_, this, &TlsStream::bio_send, &TlsStream::bio_recv, nullptr);
		return true;
	}

	// Why the server's chain failed verification, as mbedTLS words each flag.
	std::string verify_reasons() const {
		const uint32_t flags = mbedtls_ssl_get_verify_result(&ssl_);
		if (flags == 0 || flags == UINT32_MAX) return "";
		char info[512] = {};
		mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
		std::string reasons(info);
		while (!reasons.empty() && reasons.back() == '\n') reasons.pop_back();
		std::replace(reasons.begin(), reasons.end(), '\n', ';');
		return " (" + reasons + ")";
	}

	static int bio_send(void *context, const unsigned char *buf, size_t len) {
		auto *self = static_cast<TlsStream *>(context);
		const std::size_t part = std::min<std::size_t>(len, INT_MAX);
		return self->inner_->send_all(buf, part) ? static_cast<int>(part)
		                                         : MBEDTLS_ERR_NET_SEND_FAILED;
	}

	// One receive of the inner stream, waiting at most the operation's wait: a wait that runs out
	// is mbedTLS's want-read, which leaves the session to resume at the next call.
	static int bio_recv(void *context, unsigned char *buf, size_t len) {
		auto *self = static_cast<TlsStream *>(context);
		const int n = self->inner_->recv(buf, std::min<std::size_t>(len, INT_MAX), self->wait_ms_);
		if (n >= 0) return n; // 0: the connection's end
		if (n == kStreamRecvTimedOut) return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}

	std::unique_ptr<IStreamSocket> inner_;
	mbedtls_ssl_context ssl_;
	mbedtls_ssl_config config_;
	mbedtls_entropy_context entropy_;
	mbedtls_ctr_drbg_context drbg_;
	mbedtls_x509_crt trusted_;
	mbedtls_x509_crt own_chain_;
	mbedtls_pk_context own_key_;
	int wait_ms_ = 0; // the current operation's wait for the inner stream's bytes
	bool open_ = false;
};

} // namespace

std::unique_ptr<IStreamSocket> tls_client(std::unique_ptr<IStreamSocket> inner,
		const std::string &host, const TlsTrust &trust, int timeout_ms, std::string &error,
		const std::atomic<bool> *cancelled) {
	if (inner == nullptr) {
		error = "no stream";
		return nullptr;
	}
	if (!tls_start(error)) return nullptr;
	auto stream = std::make_unique<TlsStream>(std::move(inner));
	if (!stream->client(host, trust, error) || !stream->handshake(timeout_ms, cancelled, error))
		return nullptr;
	return stream;
}

std::unique_ptr<IStreamSocket> tls_server(std::unique_ptr<IStreamSocket> inner,
		const std::string &cert_pem, const std::string &key_pem, int timeout_ms,
		std::string &error) {
	if (inner == nullptr) {
		error = "no stream";
		return nullptr;
	}
	if (!tls_start(error)) return nullptr;
	auto stream = std::make_unique<TlsStream>(std::move(inner));
	if (!stream->server(cert_pem, key_pem, error) || !stream->handshake(timeout_ms, nullptr, error))
		return nullptr;
	return stream;
}

int tls_os_trust_count() {
	std::string error;
	if (!tls_start(error)) return 0;
	mbedtls_x509_crt chain;
	mbedtls_x509_crt_init(&chain);
	const int count = load_os_trust(chain);
	mbedtls_x509_crt_free(&chain);
	return count;
}

} // namespace opennova::net
