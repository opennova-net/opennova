#include <novaworld/registration_url.h>

#include <novacrypto/url_cipher.h>

namespace opennova {

namespace {

constexpr char FIELD_TERMINATOR = '&';

// Locate `token` in `url` and return the starting offset of the value
// (i.e. just past the token). Returns std::string_view::npos if absent.
size_t find_token(std::string_view url, std::string_view token) {
	const size_t p = url.find(token);
	if (p == std::string_view::npos) {
		return std::string_view::npos;
	}
	return p + token.size();
}

// Extract a '&'-terminated substring starting at `start` in `url`.
std::string extract_plain_value(std::string_view url, size_t start) {
	if (start >= url.size()) {
		return {};
	}
	const size_t end = url.find(FIELD_TERMINATOR, start);
	const size_t n = (end == std::string_view::npos ? url.size() : end) - start;
	return std::string(url.substr(start, n));
}

} // namespace

bool registration_url_parse(std::string_view url,
                            RegistrationUrl &out,
                            std::string_view nk_separator) {
	out = RegistrationUrl{};

	// HOSTKEY= preempts everything else in the original code path.
	const size_t hk = find_token(url, "HOSTKEY=");
	if (hk != std::string_view::npos) {
		out.has_host_key = true;
		// The original trims at an unidentified "chars" set and then at ']'.
		// We implement the latter (witnessed) and leave the "chars" trim as
		// a no-op since the set's contents aren't verified in the log yet.
		std::string raw = extract_plain_value(url, hk);
		const size_t close_bracket = raw.find(']');
		if (close_bracket != std::string::npos) {
			raw.resize(close_bracket);
		}
		out.host_key = std::move(raw);
		return true;
	}

	// Client flow: NK=, CK=, NI=, NP=, BK=.
	// NK cipher decode + split at the separator.
	const size_t nk = find_token(url, "NK=");
	if (nk == std::string_view::npos) {
		return false; // the original also fails here
	}
	{
		const std::string raw = extract_plain_value(url, nk);
		std::string decoded = url_cipher_decode(raw, URL_CIPHER_KEY_NK);
		const size_t sep_pos = nk_separator.empty() ? std::string::npos : decoded.find(nk_separator);
		if (sep_pos == std::string::npos) {
			out.name_key = std::move(decoded);
		} else {
			out.name_key = decoded.substr(0, sep_pos);
			out.name_key_suffix = decoded.substr(sep_pos + nk_separator.size());
		}
	}

	// CK cipher decode (no split).
	if (const size_t ck = find_token(url, "CK="); ck != std::string_view::npos) {
		const std::string raw = extract_plain_value(url, ck);
		out.cd_key = url_cipher_decode(raw, URL_CIPHER_KEY_CK);
	}

	// Plaintext fields.
	if (const size_t ni = find_token(url, "NI="); ni != std::string_view::npos) {
		out.name_info = extract_plain_value(url, ni);
	}
	if (const size_t np = find_token(url, "NP="); np != std::string_view::npos) {
		out.player_name = extract_plain_value(url, np);
	}
	if (const size_t bk = find_token(url, "BK="); bk != std::string_view::npos) {
		out.bank_key = extract_plain_value(url, bk);
	}

	return true;
}

} // namespace opennova
