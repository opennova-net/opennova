#pragma once

// The object store on S3 (object_store.h): the AWS CLI v2 through apps/common's command
// runner, the program's own role answering for it (ec2_machine_driver.h's pattern: an argv
// array, no shell, bounded by a timeout). A part the browser uploads is written to a scratch
// file and handed to `s3api upload-part`, so no credential ever leaves the process. Service
// policy, not a port.

#include "command_runner.h"
#include "object_store.h"

#include <string>

namespace opennova::novaworld_server {

struct S3StoreTimeouts {
	int command_ms = 60000; // a listing, a head, a multipart call
	int transfer_ms = 20 * 60000; // a whole file up or down, a part up
};

class S3ObjectStore : public ObjectStore {
public:
	// `scratch_dir`: an owner-only directory for the files a CLI call reads (a part's bytes,
	// the completed parts' JSON).
	S3ObjectStore(ObjectStoreConfig config, proc::CommandRunner &runner, std::string aws_cli,
			std::string scratch_dir, S3StoreTimeouts timeouts = {});

	const char *kind() const override { return "s3"; }
	std::string url_of(std::string_view key) const override;

	bool put_file(std::string_view key, const std::string &path, std::string &error) override;
	bool get_file(std::string_view key, const std::string &path, std::string &error) override;
	bool copy(std::string_view from_key, std::string_view to_key, std::string &error) override;
	bool copy_prefix(
			std::string_view from_prefix, std::string_view to_prefix, std::string &error) override;
	bool list(std::string_view prefix, std::vector<StoredObject> &out, std::string &error) override;
	bool head(std::string_view key, bool &exists, uint64_t &size, std::string &error) override;
	bool remove(std::string_view key, std::string &error) override;
	bool remove_prefix(std::string_view prefix, std::string &error) override;

	bool multipart_begin(std::string_view key, std::string &upload_id, std::string &error) override;
	bool upload_part(std::string_view key, std::string_view upload_id, int part_number,
			std::string_view data, std::string &etag, std::string &error) override;
	bool multipart_complete(std::string_view key, std::string_view upload_id,
			const std::vector<UploadPart> &parts, std::string &error) override;
	bool multipart_abort(
			std::string_view key, std::string_view upload_id, std::string &error) override;
	bool abort_uploads_under(std::string_view prefix, std::string &error) override;

private:
	std::string full_key(std::string_view key) const; // "<root>/<key>"
	std::string s3_url(std::string_view key) const; // "s3://<bucket>/<root>/<key>"
	// `aws s3api <operation> <params...> --bucket <bucket> --region ... --output json`.
	std::vector<std::string> api(const std::string &operation,
			std::vector<std::string> params) const;
	// `aws s3 <operation> <params...> --region ... --only-show-errors`.
	std::vector<std::string> s3(const std::string &operation,
			std::vector<std::string> params) const;
	bool run(const std::vector<std::string> &argv, int timeout_ms, std::string &out,
			std::string &error) const;

	ObjectStoreConfig config_;
	proc::CommandRunner &runner_;
	std::string aws_cli_;
	std::string scratch_dir_;
	S3StoreTimeouts timeouts_;
};

} // namespace opennova::novaworld_server
