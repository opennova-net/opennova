// UTF-8 path strings at the operating system's file calls (header-only, C++17).
//
// The engine keeps every path as a UTF-8 std::string (what the Godot side hands it,
// what it reports back). Two Windows facts make that string unusable as is at an OS
// call: a narrow path (fopen, std::filesystem::path(std::string)) is read in the ANSI
// code page, not UTF-8, so a root such as C:/Users/José/... names another file; and a
// path of MAX_PATH (260) characters or more fails every Win32 call unless it carries
// the \\?\ prefix (or the process is long-path aware AND the machine opted in, which is
// off by default). These helpers convert at the call: os_path() is the path to hand
// std::filesystem / std::ifstream, fopen_utf8() the stdio open, utf8_path() the way
// back for a path the system enumerated (std::filesystem::path::string() would throw
// on a name the code page cannot hold). No exceptions, no <windows.h>; on other
// systems they are plain pass-throughs (paths there are bytes and PATH_MAX is large).
//
// Platform file I/O, not a port: nothing here is witnessed engine behaviour.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace opennova::io {

#ifdef _WIN32

namespace os_path_detail {

// UTF-8 to UTF-16; an invalid or truncated sequence becomes U+FFFD.
inline std::wstring widen_utf8(std::string_view in) {
	std::wstring out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		const unsigned char c = static_cast<unsigned char>(in[i]);
		unsigned int cp = 0xFFFD;
		size_t len = 1;
		if (c < 0x80) {
			cp = c;
		} else if (c >= 0xC2 && c <= 0xF4) {
			len = c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4);
			unsigned int v = c & (len == 2 ? 0x1F : (len == 3 ? 0x0F : 0x07));
			bool ok = i + len <= in.size();
			for (size_t k = 1; ok && k < len; ++k) {
				const unsigned char cc = static_cast<unsigned char>(in[i + k]);
				ok = (cc & 0xC0) == 0x80;
				v = (v << 6) | (cc & 0x3F);
			}
			const unsigned int min = len == 2 ? 0x80 : (len == 3 ? 0x800 : 0x10000);
			if (ok && v >= min && v <= 0x10FFFF && (v < 0xD800 || v > 0xDFFF)) {
				cp = v;
			} else {
				len = 1;
			}
		}
		i += len;
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
			out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
		} else {
			out.push_back(static_cast<wchar_t>(cp));
		}
	}
	return out;
}

// UTF-16 to UTF-8; a lone surrogate becomes U+FFFD.
inline std::string narrow_utf8(std::wstring_view in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		unsigned int cp = static_cast<unsigned int>(in[i]);
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<unsigned int>(in[i + 1]) - 0xDC00);
			++i;
		} else if (cp >= 0xD800 && cp <= 0xDFFF) {
			cp = 0xFFFD;
		}
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}
	return out;
}

inline bool is_separator(wchar_t c) { return c == L'\\' || c == L'/'; }

// \\?\ or \\.\, either separator spelled (//?/ names what \\?\ does once the system
// normalizes it).
inline bool has_long_prefix(const std::wstring &p) {
	return p.size() >= 4 && is_separator(p[0]) && is_separator(p[1]) && (p[2] == L'?' || p[2] == L'.') &&
	       is_separator(p[3]);
}

// A prefixed path the way the system would have normalized it without the prefix,
// which turns that normalization off (a '/' is then a name character, "." and ".."
// are names): the prefix spelled with '\', the rest '\'-separated with repeated
// separators, "." and ".." resolved; \\?\UNC\server\share keeps its server and share.
inline std::filesystem::path normal_prefixed(const std::wstring &text) {
	const std::wstring prefix = {L'\\', L'\\', text[2], L'\\'};
	std::wstring rest = text.substr(4);
	const bool unc = rest.size() >= 4 && (rest[0] == L'U' || rest[0] == L'u') && (rest[1] == L'N' || rest[1] == L'n') &&
	                 (rest[2] == L'C' || rest[2] == L'c') && is_separator(rest[3]);
	if (unc) rest = L"\\\\" + rest.substr(4);
	std::wstring normal = std::filesystem::path(rest).lexically_normal().native();
	for (wchar_t &c : normal) {
		if (c == L'/') c = L'\\';
	}
	if (unc && normal.size() >= 2 && normal[0] == L'\\' && normal[1] == L'\\') {
		return std::filesystem::path(prefix + L"UNC\\" + normal.substr(2));
	}
	return std::filesystem::path(prefix + normal);
}

// The length past which a path needs the prefix: MAX_PATH less the 12 characters a
// directory must leave for an 8.3 name (CreateDirectory), which also covers the "\*"
// a directory iteration appends.
constexpr size_t kLongPathThreshold = 248;

} // namespace os_path_detail

// `native` with the \\?\ prefix when it is long: made absolute and lexically normal
// first (the prefix turns off the system's own normalization), "\\server\share" as
// "\\?\UNC\server\share". A path already prefixed comes back normalized the same way
// (normal_prefixed); a short one comes back as is.
inline std::filesystem::path os_path(const std::filesystem::path &native) {
	using namespace os_path_detail;
	const std::wstring &text = native.native();
	if (has_long_prefix(text)) return normal_prefixed(text);
	if (text.size() < kLongPathThreshold) return native;
	std::error_code ec;
	std::filesystem::path absolute = native.is_absolute() ? native : std::filesystem::absolute(native, ec);
	if (ec) return native;
	std::wstring full = absolute.lexically_normal().native();
	for (wchar_t &c : full) {
		if (c == L'/') c = L'\\';
	}
	if (has_long_prefix(full)) return std::filesystem::path(full);
	if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') {
		return std::filesystem::path(L"\\\\?\\UNC\\" + full.substr(2));
	}
	return std::filesystem::path(L"\\\\?\\" + full);
}

// A UTF-8 path string as the OS path to open it by.
inline std::filesystem::path os_path(std::string_view utf8) {
	return os_path(std::filesystem::path(os_path_detail::widen_utf8(utf8)));
}

// A path back as UTF-8 (native separators), without a \\?\ prefix.
inline std::string utf8_path(const std::filesystem::path &path) {
	std::wstring_view text = path.native();
	if (text.size() >= 8 && text.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
		return "\\\\" + os_path_detail::narrow_utf8(text.substr(8));
	}
	if (text.size() >= 4 && text.compare(0, 4, L"\\\\?\\") == 0) text.remove_prefix(4);
	return os_path_detail::narrow_utf8(text);
}

// fopen of a UTF-8 path. `mode` is ASCII ("rb", "wb", ...).
inline std::FILE *fopen_utf8(const char *utf8, const char *mode) {
	if (utf8 == nullptr || mode == nullptr) return nullptr;
	std::wstring wide_mode;
	for (const char *m = mode; *m != '\0'; ++m) wide_mode.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*m)));
	return _wfopen(os_path(std::string_view(utf8)).c_str(), wide_mode.c_str());
}

#else

inline std::filesystem::path os_path(const std::filesystem::path &native) { return native; }
inline std::filesystem::path os_path(std::string_view utf8) { return std::filesystem::path(std::string(utf8)); }
inline std::string utf8_path(const std::filesystem::path &path) { return path.native(); }
inline std::FILE *fopen_utf8(const char *utf8, const char *mode) {
	return (utf8 == nullptr || mode == nullptr) ? nullptr : std::fopen(utf8, mode);
}

#endif

// A std::string or a C string converts to both a string_view and a path: name the UTF-8 one.
inline std::filesystem::path os_path(const std::string &utf8) { return os_path(std::string_view(utf8)); }
inline std::filesystem::path os_path(const char *utf8) { return os_path(std::string_view(utf8)); }

// utf8_path with '/' separators (what the Godot side joins and compares paths with).
inline std::string utf8_generic_path(const std::filesystem::path &path) {
	std::string out = utf8_path(path);
#ifdef _WIN32
	for (char &c : out) {
		if (c == '\\') c = '/';
	}
#endif
	return out;
}

// The last component of a '/'- or '\'-separated UTF-8 path (the whole string without one).
inline std::string utf8_file_name(std::string_view path) {
	const size_t slash = path.find_last_of("/\\");
	return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// `dir` and `name` joined with '/' ("" dir gives `name`).
inline std::string utf8_join(std::string_view dir, std::string_view name) {
	if (dir.empty()) return std::string(name);
	std::string out(dir);
	if (out.back() != '/' && out.back() != '\\') out.push_back('/');
	out.append(name);
	return out;
}

} // namespace opennova::io
