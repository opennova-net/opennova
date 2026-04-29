#include <novacrypto/url_cipher.h>
#include <novaworld/registration_url.h>

#include <cstdio>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool check_host_key_flow() {
	using opennova::RegistrationUrl;
	using opennova::registration_url_parse;
	RegistrationUrl u;
	if (!expect(registration_url_parse("nw://example.com/path?HOSTKEY=abc123", u),
			"host-key flow parses")) return false;
	if (!expect(u.has_host_key, "has_host_key true")) return false;
	if (!expect(u.host_key == "abc123", "host_key extracted")) return false;
	if (!expect(u.name_key.empty() && u.cd_key.empty(),
			"other fields left empty in host flow")) return false;
	return true;
}

bool check_host_key_bracket_trim() {
	using opennova::RegistrationUrl;
	using opennova::registration_url_parse;
	RegistrationUrl u;
	registration_url_parse("nw://x?HOSTKEY=abc123]garbage", u);
	if (!expect(u.has_host_key && u.host_key == "abc123", "host_key trimmed at ']'")) return false;
	return true;
}

bool check_client_flow_roundtrip() {
	using opennova::registration_url_parse;
	using opennova::RegistrationUrl;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_CK;
	using opennova::URL_CIPHER_KEY_NK;
	// Build a synthetic URL with encoded NK/CK, plaintext NI/NP/BK.
	const std::string plain_nk = "192.168.1.42:4444";
	const std::string plain_ck = "CD-KEY-HERE";
	const std::string enc_nk = url_cipher_encode(plain_nk, URL_CIPHER_KEY_NK);
	const std::string enc_ck = url_cipher_encode(plain_ck, URL_CIPHER_KEY_CK);
	const std::string url = "nw://host/path?NK=" + enc_nk + "&CK=" + enc_ck +
			"&NI=AlphaServer&NP=Taylor&BK=BankXYZ";

	RegistrationUrl u;
	if (!expect(registration_url_parse(url, u), "client flow parses")) return false;
	if (!expect(!u.has_host_key, "has_host_key false in client flow")) return false;
	if (!expect(u.name_key == "192.168.1.42", "NK head == IP")) return false;
	if (!expect(u.name_key_suffix == "4444", "NK tail == port")) return false;
	if (!expect(u.cd_key == plain_ck, "CK roundtrips via url_cipher")) return false;
	if (!expect(u.name_info == "AlphaServer", "NI plaintext")) return false;
	if (!expect(u.player_name == "Taylor", "NP plaintext")) return false;
	if (!expect(u.bank_key == "BankXYZ", "BK plaintext")) return false;
	return true;
}

// Custom NK-separator override (e.g. for alt deployments).
bool check_custom_nk_separator() {
	using opennova::registration_url_parse;
	using opennova::RegistrationUrl;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_NK;
	const std::string plain_nk = "left#right";
	const std::string enc_nk = url_cipher_encode(plain_nk, URL_CIPHER_KEY_NK);
	const std::string url = "nw://h/?NK=" + enc_nk + "&";
	RegistrationUrl u;
	registration_url_parse(url, u, "#");
	if (!expect(u.name_key == "left", "custom separator splits head")) return false;
	if (!expect(u.name_key_suffix == "right", "custom separator splits tail")) return false;
	return true;
}

// NK without any separator should leave suffix empty.
bool check_no_nk_separator() {
	using opennova::registration_url_parse;
	using opennova::RegistrationUrl;
	using opennova::url_cipher_encode;
	using opennova::URL_CIPHER_KEY_NK;
	const std::string plain_nk = "nosuffix";
	const std::string enc_nk = url_cipher_encode(plain_nk, URL_CIPHER_KEY_NK);
	RegistrationUrl u;
	registration_url_parse("nw://h/?NK=" + enc_nk + "&", u);
	if (!expect(u.name_key == "nosuffix", "NK without separator kept whole")) return false;
	if (!expect(u.name_key_suffix.empty(), "suffix empty when no separator present")) return false;
	return true;
}

// Malformed URL without NK= should fail.
bool check_missing_nk_fails() {
	using opennova::registration_url_parse;
	using opennova::RegistrationUrl;
	RegistrationUrl u;
	if (!expect(!registration_url_parse("nw://x?CK=abc&", u),
			"URL missing NK= must fail")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_host_key_flow()) return 1;
	if (!check_host_key_bracket_trim()) return 1;
	if (!check_client_flow_roundtrip()) return 1;
	if (!check_custom_nk_separator()) return 1;
	if (!check_no_nk_separator()) return 1;
	if (!check_missing_nk_fails()) return 1;
	std::printf("OK: registration URL parse (HOSTKEY, NK split, CK cipher, NI/NP/BK plaintext)\n");
	return 0;
}
