#pragma once

// The content store's manifest (ADR 0069): one revision of a content project, the project's own
// files (never the machine's `.opennova/`) each by its path, its SHA-256 and its size, in one
// canonical text whose own SHA-256 names the revision in the store (`content/<game id>/manifests/
// <sha256>.json`; each file at `content/<game id>/blobs/<sha256>`). The service checks a push by
// it and the editor writes and reads it, so both take the one form here.
// A path is one every system the project is downloaded to can hold: '/'-separated and relative,
// UTF-8, no part a Windows file name refuses, and none two paths share once their ASCII letters are
// folded (Windows and macOS hold one of the two). OpenNova's own format, not a port.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::content {

inline constexpr int kManifestSchemaVersion = 1;
// A path's bound in bytes, and each of its parts' (NTFS's 255 UTF-16 units hold 255 bytes).
inline constexpr size_t kMaxPathBytes = 1024;
inline constexpr size_t kMaxPartBytes = 255;
// A file's bound (S3 takes one PUT of up to 5 GB; the store asks for one PUT a file).
inline constexpr uint64_t kMaxFileBytes = uint64_t(4) << 30;

// One file of a revision.
struct ContentFile {
	std::string path; // project-relative, '/'-separated: "art/on_pist/on_pist.blend"
	std::string sha256; // 64 lower-case hex digits
	uint64_t size = 0;
};

// Whether `text` is a SHA-256 as the store names a file: 64 lower-case hex digits.
bool is_sha256_hex(std::string_view text);

// Whether `text` is well-formed UTF-8: no stray continuation byte, no overlong form, no surrogate,
// nothing past U+10FFFF. A path's rule, and a revision's message's.
bool is_utf8(std::string_view text);

// Whether `path` is a path a project's file may have: non-empty, at most kMaxPathBytes, valid
// UTF-8, its parts '/'-separated (none empty, "." or "..", none past kMaxPartBytes), no backslash,
// control character or character a Windows name refuses (< > : " | ? *), no part ending in a dot or
// a space (Windows drops them) or named as a Windows device (CON, PRN, AUX, NUL, COM0-9, LPT0-9,
// with or without an extension), and nothing under `.opennova/` (the machine's own folder). False
// with `why`.
bool is_content_path(std::string_view path, std::string &why);

// `path` with its ASCII letters folded to lower case: two paths that fold the same are one file on
// Windows and macOS.
std::string folded_path(std::string_view path);

// Whether `files` is a revision's file list: every path a content path, every sha a SHA-256, every
// size within kMaxFileBytes, no two paths the same once folded, no sha at two sizes, and no file a
// folder of another path (once folded). False with `why`, which names the first file at fault.
bool check_files(const std::vector<ContentFile> &files, std::string &why);

// The canonical text of a revision's files (sorted by their paths' bytes): a JSON object
// {"files": [{"path", "sha256", "size"}], "schema_version": 1} as io::json_write writes it.
std::string encode_manifest(std::vector<ContentFile> files);
// `text` read back into `out`, sorted by path: false with `error` when it is not a manifest, its
// files fail check_files, or it is not in its canonical form (the bytes encode_manifest writes for
// its files), so the SHA-256 of a manifest that reads is its files' one name.
bool decode_manifest(std::string_view text, std::vector<ContentFile> &out, std::string &error);
// The SHA-256 of a manifest's text: the revision's name in the store.
std::string manifest_sha256(std::string_view text);

// The files' sizes summed.
uint64_t total_bytes(const std::vector<ContentFile> &files);

} // namespace opennova::content
