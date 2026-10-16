#pragma once

// The object store as a directory (object_store.h): every key a file under the root, a prefix
// a directory, `url_of` a file:// URL hostd and the local machine driver copy from. Its
// multipart uploads land in `<root>/.uploads/<token>/`, one file per part as web hands each
// over; complete joins the parts into the key. The developer's machine and the tests run on
// it. Service policy, not a port.

#include "object_store.h"

#include <cstdint>
#include <string>

namespace opennova::novaworld_server {

class LocalObjectStore : public ObjectStore {
public:
	explicit LocalObjectStore(std::string root) : root_(std::move(root)) {}

	const char *kind() const override { return "local"; }
	std::string url_of(std::string_view key) const override;
	const std::string &root() const { return root_; }
	// The file a key maps to.
	std::string path_of(std::string_view key) const;

	bool put_file(std::string_view key, const std::string &path, std::string &error) override;
	bool get_file(std::string_view key, const std::string &path, std::string &error) override;
	bool list(std::string_view prefix, std::vector<StoredObject> &out, std::string &error) override;
	bool head(std::string_view key, bool &exists, uint64_t &size, std::string &error) override;
	bool remove(std::string_view key, std::string &error) override;
	bool remove_prefix(std::string_view prefix, std::string &error) override;

	bool multipart_begin(std::string_view key, std::string &upload_id, std::string &error) override;
	// `etag` gets the part's SHA-256 in hex, which the completion checks the reported part
	// against. False with `error` for an upload that is not open or not of `key`, or a part
	// out of range.
	bool upload_part(std::string_view key, std::string_view upload_id, int part_number,
			std::string_view data, std::string &etag, std::string &error) override;
	bool multipart_complete(std::string_view key, std::string_view upload_id,
			const std::vector<UploadPart> &parts, std::string &error) override;
	bool multipart_abort(
			std::string_view key, std::string_view upload_id, std::string &error) override;
	bool abort_uploads_under(std::string_view prefix, std::string &error) override;

	// Whether `upload_id` is a token of an upload in flight.
	static bool is_upload_id(std::string_view upload_id);

private:
	std::string upload_dir(std::string_view upload_id) const;

	std::string root_;
};

} // namespace opennova::novaworld_server
