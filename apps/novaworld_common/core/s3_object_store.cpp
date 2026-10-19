#include "s3_object_store.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <filesystem>
#include <system_error>

namespace opennova::novaworld_server {

namespace {

std::string with_slash(std::string_view prefix) {
	std::string out(prefix);
	if (!out.empty() && out.back() != '/')
		out += '/';
	return out;
}

bool is_not_found(const proc::CommandResult &result) {
	return result.outcome == proc::CommandResult::Outcome::Exited &&
			(result.err.find("(404)") != std::string::npos ||
					result.err.find("Not Found") != std::string::npos ||
					result.err.find("NoSuchKey") != std::string::npos);
}

// An ETag as the CLI prints it, its quotes taken off.
std::string unquoted(std::string etag) {
	if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"')
		return etag.substr(1, etag.size() - 2);
	return etag;
}

} // namespace

S3ObjectStore::S3ObjectStore(ObjectStoreConfig config, proc::CommandRunner &runner,
		std::string aws_cli, std::string scratch_dir, S3StoreTimeouts timeouts) :
		config_(std::move(config)),
		runner_(runner),
		aws_cli_(std::move(aws_cli)),
		scratch_dir_(std::move(scratch_dir)),
		timeouts_(timeouts) {}

std::string S3ObjectStore::full_key(std::string_view key) const {
	return config_.root + "/" + std::string(key);
}

std::string S3ObjectStore::s3_url(std::string_view key) const {
	return "s3://" + config_.bucket + "/" + full_key(key);
}

std::string S3ObjectStore::url_of(std::string_view key) const {
	return object_store_url(config_, key);
}

std::vector<std::string> S3ObjectStore::api(
		const std::string &operation, std::vector<std::string> params) const {
	std::vector<std::string> argv = { aws_cli_, "s3api", operation };
	argv.insert(argv.end(), std::make_move_iterator(params.begin()),
			std::make_move_iterator(params.end()));
	const std::vector<std::string> globals = { "--bucket", config_.bucket, "--region",
		config_.region, "--output", "json", "--no-cli-pager" };
	argv.insert(argv.end(), globals.begin(), globals.end());
	return argv;
}

std::vector<std::string> S3ObjectStore::s3(
		const std::string &operation, std::vector<std::string> params) const {
	std::vector<std::string> argv = { aws_cli_, "s3", operation };
	argv.insert(argv.end(), std::make_move_iterator(params.begin()),
			std::make_move_iterator(params.end()));
	const std::vector<std::string> globals = { "--region", config_.region, "--only-show-errors",
		"--no-cli-pager" };
	argv.insert(argv.end(), globals.begin(), globals.end());
	return argv;
}

bool S3ObjectStore::run(const std::vector<std::string> &argv, int timeout_ms, std::string &out,
		std::string &error) const {
	const proc::CommandResult result = runner_.run(argv, timeout_ms);
	if (result.ok()) {
		out = result.out;
		return true;
	}
	std::string what = "aws " + argv[1] + " " + argv[2];
	switch (result.outcome) {
		case proc::CommandResult::Outcome::Exited:
			error = what + " failed (" + std::to_string(result.exit_code) + "): " +
					strutil::trim(result.err);
			break;
		case proc::CommandResult::Outcome::TimedOut:
			error = what + " timed out";
			break;
		case proc::CommandResult::Outcome::NotStarted:
			error = what + " did not start: " + result.error;
			break;
	}
	return false;
}

bool S3ObjectStore::put_file(std::string_view key, const std::string &path, std::string &error) {
	if (!is_store_key(key)) {
		error = "not a store key: " + std::string(key);
		return false;
	}
	std::string out;
	return run(s3("cp", { path, s3_url(key) }), timeouts_.transfer_ms, out, error);
}

bool S3ObjectStore::get_file(std::string_view key, const std::string &path, std::string &error) {
	std::string out;
	return run(s3("cp", { s3_url(key), path }), timeouts_.transfer_ms, out, error);
}

bool S3ObjectStore::copy(std::string_view from_key, std::string_view to_key, std::string &error) {
	if (!is_store_key(to_key)) {
		error = "not a store key: " + std::string(to_key);
		return false;
	}
	std::string out;
	return run(api("copy-object",
						{ "--copy-source", config_.bucket + "/" + full_key(from_key), "--key",
								full_key(to_key) }),
			timeouts_.transfer_ms, out, error);
}

bool S3ObjectStore::copy_prefix(
		std::string_view from_prefix, std::string_view to_prefix, std::string &error) {
	std::string out;
	return run(s3("sync", { s3_url(with_slash(from_prefix)), s3_url(with_slash(to_prefix)) }),
			timeouts_.transfer_ms, out, error);
}

bool S3ObjectStore::list(
		std::string_view prefix, std::vector<StoredObject> &out, std::string &error) {
	out.clear();
	std::string text;
	if (!run(api("list-objects-v2", { "--prefix", full_key(prefix) }), timeouts_.command_ms, text,
				error))
		return false;
	if (strutil::trim(text).empty())
		return true; // the CLI prints nothing for an empty listing
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object()) {
		error = "list-objects-v2 answered something that is not JSON";
		return false;
	}
	const io::JsonValue *contents = json.get("Contents");
	if (contents == nullptr || !contents->is_array())
		return true;
	const std::string root = config_.root + "/";
	for (const io::JsonValue &item : contents->array) {
		if (!item.is_object())
			continue;
		const std::string key = item.get_string("Key", "");
		if (key.rfind(root, 0) != 0)
			continue;
		StoredObject object;
		object.key = key.substr(root.size());
		object.size = static_cast<uint64_t>(item.get_number("Size", 0));
		out.push_back(std::move(object));
	}
	return true;
}

bool S3ObjectStore::head(std::string_view key, bool &exists, uint64_t &size, std::string &error) {
	const proc::CommandResult result = runner_.run(
			api("head-object", { "--key", full_key(key) }), timeouts_.command_ms);
	if (!result.ok()) {
		if (is_not_found(result)) {
			exists = false;
			size = 0;
			return true;
		}
		error = "aws s3api head-object failed: " +
				(result.error.empty() ? strutil::trim(result.err) : result.error);
		return false;
	}
	io::JsonValue json;
	if (!io::json_parse(result.out, json, error) || !json.is_object()) {
		error = "head-object answered something that is not JSON";
		return false;
	}
	exists = true;
	size = static_cast<uint64_t>(json.get_number("ContentLength", 0));
	return true;
}

bool S3ObjectStore::remove(std::string_view key, std::string &error) {
	std::string out;
	return run(s3("rm", { s3_url(key) }), timeouts_.command_ms, out, error);
}

bool S3ObjectStore::remove_prefix(std::string_view prefix, std::string &error) {
	std::string out;
	return run(s3("rm", { s3_url(with_slash(prefix)), "--recursive" }), timeouts_.transfer_ms, out,
			error);
}

bool S3ObjectStore::multipart_begin(
		std::string_view key, std::string &upload_id, std::string &error) {
	if (!is_store_key(key)) {
		error = "not a store key: " + std::string(key);
		return false;
	}
	std::string text;
	if (!run(api("create-multipart-upload", { "--key", full_key(key) }), timeouts_.command_ms,
				text, error))
		return false;
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object() ||
			json.get_string("UploadId", "").empty()) {
		error = "create-multipart-upload answered no UploadId";
		return false;
	}
	upload_id = json.get_string("UploadId", "");
	return true;
}

bool S3ObjectStore::upload_part(std::string_view key, std::string_view upload_id, int part_number,
		std::string_view data, std::string &etag, std::string &error) {
	// The part's bytes in a file of their own for the CLI to read, gone once it answered.
	const std::string path = io::utf8_join(scratch_dir_, "part-" + make_uuid_v4());
	if (!io::write_file_atomic(path, data.data(), data.size(), error))
		return false;
	std::string text;
	const bool ok = run(api("upload-part",
								{ "--key", full_key(key), "--upload-id", std::string(upload_id),
										"--part-number", std::to_string(part_number), "--body",
										path }),
			timeouts_.transfer_ms, text, error);
	std::error_code ec;
	std::filesystem::remove(io::os_path(path), ec);
	if (!ok)
		return false;
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object() ||
			json.get_string("ETag", "").empty()) {
		error = "upload-part answered no ETag";
		return false;
	}
	etag = unquoted(json.get_string("ETag", ""));
	return true;
}

bool S3ObjectStore::multipart_complete(std::string_view key, std::string_view upload_id,
		const std::vector<UploadPart> &parts, std::string &error) {
	io::JsonValue list = io::JsonValue::make_array();
	for (const UploadPart &part : parts) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("PartNumber", io::json_number(part.number));
		item.set("ETag", io::json_string(part.etag));
		list.push(std::move(item));
	}
	io::JsonValue body = io::JsonValue::make_object();
	body.set("Parts", std::move(list));
	const std::string path = io::utf8_join(scratch_dir_, "complete-" + make_uuid_v4() + ".json");
	if (!io::write_file_atomic(path, io::json_write(body), error))
		return false;
	std::string out;
	const bool ok = run(api("complete-multipart-upload",
								{ "--key", full_key(key), "--upload-id", std::string(upload_id),
										"--multipart-upload", "file://" + path }),
			timeouts_.command_ms, out, error);
	std::error_code ec;
	std::filesystem::remove(io::os_path(path), ec);
	return ok;
}

bool S3ObjectStore::multipart_abort(
		std::string_view key, std::string_view upload_id, std::string &error) {
	std::string out;
	return run(api("abort-multipart-upload",
						{ "--key", full_key(key), "--upload-id", std::string(upload_id) }),
			timeouts_.command_ms, out, error);
}

bool S3ObjectStore::abort_uploads_under(std::string_view prefix, std::string &error) {
	std::string text;
	if (!run(api("list-multipart-uploads", { "--prefix", full_key(prefix) }), timeouts_.command_ms,
				text, error))
		return false;
	if (strutil::trim(text).empty())
		return true;
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object()) {
		error = "list-multipart-uploads answered something that is not JSON";
		return false;
	}
	const io::JsonValue *uploads = json.get("Uploads");
	if (uploads == nullptr || !uploads->is_array())
		return true;
	const std::string root = config_.root + "/";
	for (const io::JsonValue &item : uploads->array) {
		if (!item.is_object())
			continue;
		const std::string key = item.get_string("Key", "");
		const std::string id = item.get_string("UploadId", "");
		if (key.rfind(root, 0) != 0 || id.empty())
			continue;
		if (!multipart_abort(key.substr(root.size()), id, error))
			return false;
	}
	return true;
}

} // namespace opennova::novaworld_server
