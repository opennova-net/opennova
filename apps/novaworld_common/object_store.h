#pragma once

// The object store the hosted game data and the update packages live in (ADR 0064): the
// deployment's private S3 bucket, under one root per deployment
// (`s3://<bucket>/data/<deployment>`, s3_object_store.h), or a directory for a developer's
// machine and the tests (`file:///<dir>`, local_object_store.h). Keys are flat generic paths
// under the root (`trees/<tree id>/resource.pff`, `updates/<sha256>.sfs`); a prefix is a key
// with its trailing slash. What the service does with it: web begins each file's multipart
// upload, hands the store every part the browser PUTs through it and completes the upload;
// the worker downloads, applies and uploads whole files and copies a tree server-side; hostd
// syncs a tree from the URL `url_of` gives it (its own `aws s3 sync`, or a directory copy).
// Every call answers false with `error` on failure and throws nothing; none holds a lock
// across a command. Service policy, not a port.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

struct ObjectStoreConfig {
	enum class Kind { S3, Local };
	Kind kind = Kind::Local;
	std::string bucket; // S3
	std::string root; // S3: the key prefix with no slashes at either end; local: the directory
	std::string region; // S3
};

// `s3://<bucket>/<root>` (the root one or more path parts) or `file:///<dir>` into `out`; false
// with `error` otherwise. `region` is S3's (required there).
bool parse_object_store_url(std::string_view url, std::string_view region, ObjectStoreConfig &out,
		std::string &error);

// Whether `key` is one the store takes: parts of [A-Za-z0-9_.-], '/' between them, no part
// "." or "..", at most 512 bytes.
bool is_store_key(std::string_view key);

// The URL hostd fetches `key` (or a prefix) of a store configured as `config` from, with no
// store built: `s3://<bucket>/<root>/<key>` or `file:///<dir>/<key>` (a local root with no
// leading slash, a Windows drive's, gets one). ObjectStore::url_of answers the same.
std::string object_store_url(const ObjectStoreConfig &config, std::string_view key);

struct StoredObject {
	std::string key; // relative to the root
	uint64_t size = 0;
};

// One part of a multipart upload as the uploader completes it.
struct UploadPart {
	int number = 0; // 1-based
	std::string etag; // what upload_part answered for it
};

class ObjectStore {
public:
	virtual ~ObjectStore() = default;

	virtual const char *kind() const = 0; // "s3" or "local"
	// The URL hostd fetches a key or a prefix from: `s3://<bucket>/<root>/<key>` or
	// `file:///<dir>/<key>`.
	virtual std::string url_of(std::string_view key) const = 0;

	virtual bool put_file(std::string_view key, const std::string &path, std::string &error) = 0;
	virtual bool get_file(std::string_view key, const std::string &path, std::string &error) = 0;
	virtual bool copy(std::string_view from_key, std::string_view to_key, std::string &error) = 0;
	// Every object under `from_prefix` copied under `to_prefix` (server-side on S3).
	virtual bool copy_prefix(
			std::string_view from_prefix, std::string_view to_prefix, std::string &error) = 0;
	virtual bool list(
			std::string_view prefix, std::vector<StoredObject> &out, std::string &error) = 0;
	// Whether `key` is there, and its size when it is.
	virtual bool head(std::string_view key, bool &exists, uint64_t &size, std::string &error) = 0;
	virtual bool remove(std::string_view key, std::string &error) = 0;
	virtual bool remove_prefix(std::string_view prefix, std::string &error) = 0;

	// A multipart upload of `key`: its id, each part's bytes handed over (`etag` what the
	// completion reports for it), its completion from the parts the uploader reports, or its
	// abort. `abort_uploads_under` aborts every upload in flight under `prefix` (an upload the
	// page abandoned).
	virtual bool multipart_begin(std::string_view key, std::string &upload_id, std::string &error) = 0;
	virtual bool upload_part(std::string_view key, std::string_view upload_id, int part_number,
			std::string_view data, std::string &etag, std::string &error) = 0;
	virtual bool multipart_complete(std::string_view key, std::string_view upload_id,
			const std::vector<UploadPart> &parts, std::string &error) = 0;
	virtual bool multipart_abort(
			std::string_view key, std::string_view upload_id, std::string &error) = 0;
	virtual bool abort_uploads_under(std::string_view prefix, std::string &error) = 0;
};

} // namespace opennova::novaworld_server
