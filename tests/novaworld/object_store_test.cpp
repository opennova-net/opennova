// The object store seam (apps/novaworld_common/object_store.h): the URL forms, the key rule, the
// local store's files and multipart uploads over a directory (each part handed over, the
// completion checking the parts reported), and the S3 store's every call as the argv it hands
// the AWS CLI through a recording runner (a part's bytes in a scratch file for `upload-part`,
// gone after), its replies read back.

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include "command_runner.h"
#include "local_object_store.h"
#include "object_store.h"
#include "s3_object_store.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::novaworld_server;

namespace {

class StubRunner : public proc::CommandRunner {
public:
	proc::CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		Call call;
		call.argv = argv;
		call.timeout_ms = timeout_ms;
		for (size_t i = 0; i + 1 < argv.size(); ++i) {
			if (argv[i] == "--multipart-upload" && argv[i + 1].rfind("file://", 0) == 0) {
				std::string error;
				io::read_file_text(argv[i + 1].substr(7), call.file_text, error);
			}
			if (argv[i] == "--body") {
				std::string error;
				io::read_file_text(argv[i + 1], call.file_text, error);
			}
		}
		calls.push_back(call);
		proc::CommandResult result;
		result.outcome = proc::CommandResult::Outcome::Exited;
		if (replies.empty()) {
			result.exit_code = 255;
			result.err = "no reply";
			return result;
		}
		result = replies.front();
		replies.pop_front();
		return result;
	}
	struct Call {
		std::vector<std::string> argv;
		int timeout_ms = 0;
		std::string file_text; // what the call's file argument held
	};
	std::vector<Call> calls;
	std::deque<proc::CommandResult> replies;

	static proc::CommandResult ok(std::string out = "") {
		proc::CommandResult r;
		r.outcome = proc::CommandResult::Outcome::Exited;
		r.out = std::move(out);
		return r;
	}
	static proc::CommandResult failed(uint32_t code, std::string err) {
		proc::CommandResult r;
		r.outcome = proc::CommandResult::Outcome::Exited;
		r.exit_code = code;
		r.err = std::move(err);
		return r;
	}
};

bool has_arg(const std::vector<std::string> &argv, const std::string &value) {
	for (const std::string &a : argv)
		if (a == value)
			return true;
	return false;
}

std::string arg_after(const std::vector<std::string> &argv, const std::string &flag) {
	for (size_t i = 0; i + 1 < argv.size(); ++i)
		if (argv[i] == flag)
			return argv[i + 1];
	return "";
}

int test_config() {
	ObjectStoreConfig config;
	std::string error;
	TEST_EXPECT(parse_object_store_url("s3://opennova-backups/data/prod", "us-east-1", config, error));
	TEST_EXPECT(config.kind == ObjectStoreConfig::Kind::S3 && config.bucket == "opennova-backups" &&
			config.root == "data/prod" && config.region == "us-east-1");
	TEST_EXPECT(parse_object_store_url("s3://b/data/staging/", "us-east-1", config, error) &&
			config.root == "data/staging");
	TEST_EXPECT(!parse_object_store_url("s3://b", "us-east-1", config, error));
	TEST_EXPECT(!parse_object_store_url("s3://b/", "us-east-1", config, error));
	TEST_EXPECT(!parse_object_store_url("s3://b/root", "", config, error));
	TEST_EXPECT(parse_object_store_url("file:///tmp/store/", "", config, error) &&
			config.kind == ObjectStoreConfig::Kind::Local && config.root == "/tmp/store");
	TEST_EXPECT(parse_object_store_url("file:///C:/store", "", config, error) && config.root == "C:/store");
	TEST_EXPECT(!parse_object_store_url("http://x", "", config, error));
	TEST_EXPECT(is_store_key("trees/7/resource.pff") && is_store_key("trees/7/manifest.json") &&
			is_store_key("a-b_c.d"));
	TEST_EXPECT(!is_store_key("") && !is_store_key("/a") && !is_store_key("a/") &&
			!is_store_key("a//b") && !is_store_key("a/../b") && !is_store_key("a b") &&
			!is_store_key("a\\b") && !is_store_key("."));
	return 0;
}

int test_local(const test_temp::TempDir &dir) {
	LocalObjectStore store(dir.file("store"));
	std::string error;
	TEST_EXPECT(std::string(store.kind()) == "local");
	TEST_EXPECT(store.url_of("trees/1/a.txt").rfind("file:///", 0) == 0);
	TEST_EXPECT(store.url_of("trees/1/a.txt").find("/store/trees/1/a.txt") != std::string::npos);

	const std::string local = dir.file("a.txt");
	TEST_EXPECT(io::write_file_atomic(local, std::string("hello"), error));
	TEST_EXPECT(store.put_file("trees/1/a.txt", local, error));
	TEST_EXPECT(store.put_file("trees/1/b.txt", local, error));
	TEST_EXPECT(store.put_file("trees/2/a.txt", local, error));
	TEST_EXPECT(store.put_file("trees/2/b.txt", local, error));
	TEST_EXPECT(!store.put_file("../x", local, error));
	bool exists = false;
	uint64_t size = 0;
	TEST_EXPECT(store.head("trees/1/a.txt", exists, size, error) && exists && size == 5);
	TEST_EXPECT(store.head("trees/1/c.txt", exists, size, error) && !exists);
	std::vector<StoredObject> objects;
	TEST_EXPECT(store.list("trees/2/", objects, error) && objects.size() == 2);
	TEST_EXPECT(store.list("trees/", objects, error) && objects.size() == 4);
	// A copy, and a prefix copied object by object.
	TEST_EXPECT(store.copy("trees/1/a.txt", "trees/1/c.txt", error));
	TEST_EXPECT(!store.copy("trees/1/a.txt", "../x", error));
	TEST_EXPECT(store.copy_prefix("trees/1/", "trees/5/", error));
	TEST_EXPECT(store.list("trees/5/", objects, error) && objects.size() == 3);
	TEST_EXPECT(store.head("trees/5/c.txt", exists, size, error) && exists && size == 5);
	TEST_EXPECT(store.remove_prefix("trees/5/", error) && store.remove("trees/1/c.txt", error));
	TEST_EXPECT(store.list("trees/", objects, error) && objects.size() == 4);
	const std::string back = dir.file("back.txt");
	TEST_EXPECT(store.get_file("trees/2/b.txt", back, error));
	std::string text;
	TEST_EXPECT(io::read_file_text(back, text, error) && text == "hello");
	TEST_EXPECT(store.remove("trees/2/a.txt", error));
	TEST_EXPECT(store.list("trees/2/", objects, error) && objects.size() == 1);
	TEST_EXPECT(store.remove_prefix("trees/1/", error));
	TEST_EXPECT(store.list("trees/", objects, error) && objects.size() == 1 &&
			objects[0].key == "trees/2/b.txt");

	// A multipart upload: begin, each part handed over, complete.
	std::string upload;
	TEST_EXPECT(store.multipart_begin("trees/3/big.bin", upload, error));
	TEST_EXPECT(LocalObjectStore::is_upload_id(upload));
	std::string etag1, etag2;
	const std::string p1 = "part one,", p2 = " part two";
	TEST_EXPECT(store.upload_part("trees/3/big.bin", upload, 1, p1, etag1, error));
	TEST_EXPECT(store.upload_part("trees/3/big.bin", upload, 2, p2, etag2, error));
	TEST_EXPECT(etag1.size() == 64 && etag1 != etag2);
	// A part of an upload that is not open, of another key, or out of range.
	TEST_EXPECT(!store.upload_part("trees/3/big.bin", "nope", 1, p1, etag1, error));
	TEST_EXPECT(!store.upload_part("trees/3/other.bin", upload, 1, p1, etag1, error));
	TEST_EXPECT(!store.upload_part("trees/3/big.bin", upload, 10001, p1, etag1, error));
	TEST_EXPECT(!store.multipart_complete("trees/3/big.bin", upload, { { 1, etag1 }, { 2, "bad" } }, error));
	TEST_EXPECT(!store.multipart_complete("trees/3/other.bin", upload, { { 1, etag1 }, { 2, etag2 } }, error));
	TEST_EXPECT(store.multipart_complete("trees/3/big.bin", upload, { { 1, etag1 }, { 2, etag2 } }, error));
	TEST_EXPECT(store.head("trees/3/big.bin", exists, size, error) && exists && size == p1.size() + p2.size());
	TEST_EXPECT(store.get_file("trees/3/big.bin", back, error) &&
			io::read_file_text(back, text, error) && text == "part one, part two");
	// Uploads in flight are not listed as objects; an abort under a prefix drops them.
	std::string abandoned;
	TEST_EXPECT(store.multipart_begin("trees/4/x.bin", abandoned, error));
	TEST_EXPECT(store.list("trees/", objects, error) && objects.size() == 2);
	TEST_EXPECT(store.abort_uploads_under("trees/4/", error));
	TEST_EXPECT(!store.upload_part("trees/4/x.bin", abandoned, 1, "x", etag1, error));
	return 0;
}

int test_s3(const test_temp::TempDir &dir) {
	ObjectStoreConfig config;
	std::string error;
	TEST_EXPECT(parse_object_store_url("s3://opennova-backups/data/prod", "us-east-1", config, error));
	StubRunner runner;
	S3ObjectStore store(config, runner, "/usr/local/bin/aws", dir.file("scratch"));
	std::error_code ec;
	std::filesystem::create_directories(io::os_path(dir.file("scratch")), ec);
	TEST_EXPECT(std::string(store.kind()) == "s3");
	TEST_EXPECT(store.url_of("trees/7/") == "s3://opennova-backups/data/prod/trees/7/");

	runner.replies.push_back(StubRunner::ok());
	TEST_EXPECT(store.put_file("trees/7/a.pff", "/tmp/a.pff", error));
	TEST_EXPECT(runner.calls.size() == 1);
	const std::vector<std::string> &put = runner.calls[0].argv;
	TEST_EXPECT(put.size() >= 5 && put[1] == "s3" && put[2] == "cp" && put[3] == "/tmp/a.pff" &&
			put[4] == "s3://opennova-backups/data/prod/trees/7/a.pff" && has_arg(put, "--only-show-errors") &&
			arg_after(put, "--region") == "us-east-1");
	TEST_EXPECT(runner.calls[0].timeout_ms == 20 * 60000);

	runner.replies.push_back(StubRunner::ok(
			R"({"Contents":[{"Key":"data/prod/trees/7/a.pff","Size":12},{"Key":"data/prod/trees/7/manifest.json","Size":3}]})"));
	std::vector<StoredObject> objects;
	TEST_EXPECT(store.list("trees/7/", objects, error) && objects.size() == 2 &&
			objects[0].key == "trees/7/a.pff" && objects[0].size == 12);
	TEST_EXPECT(arg_after(runner.calls[1].argv, "--prefix") == "data/prod/trees/7/" &&
			arg_after(runner.calls[1].argv, "--bucket") == "opennova-backups");
	runner.replies.push_back(StubRunner::ok(""));
	TEST_EXPECT(store.list("trees/9/", objects, error) && objects.empty());

	bool exists = false;
	uint64_t size = 0;
	runner.replies.push_back(StubRunner::ok(R"({"ContentLength": 4096})"));
	TEST_EXPECT(store.head("trees/7/a.pff", exists, size, error) && exists && size == 4096);
	runner.replies.push_back(StubRunner::failed(254,
			"An error occurred (404) when calling the HeadObject operation: Not Found"));
	TEST_EXPECT(store.head("trees/7/none", exists, size, error) && !exists);
	runner.replies.push_back(StubRunner::failed(255, "An error occurred (AccessDenied)"));
	TEST_EXPECT(!store.head("trees/7/none", exists, size, error) && error.find("AccessDenied") != std::string::npos);

	runner.replies.push_back(StubRunner::ok());
	TEST_EXPECT(store.remove_prefix("trees/7/", error));
	TEST_EXPECT(runner.calls.back().argv[2] == "rm" && has_arg(runner.calls.back().argv, "--recursive") &&
			runner.calls.back().argv[3] == "s3://opennova-backups/data/prod/trees/7/");

	// A copy is server-side: copy-object, and a prefix an s3 sync between two prefixes.
	runner.replies.push_back(StubRunner::ok(R"({"CopyObjectResult":{"ETag":"\"c\""}})"));
	TEST_EXPECT(store.copy("trees/7/a.pff", "trees/8/a.pff", error));
	const std::vector<std::string> &copy = runner.calls.back().argv;
	TEST_EXPECT(copy[2] == "copy-object" && arg_after(copy, "--copy-source") == "opennova-backups/data/prod/trees/7/a.pff" &&
			arg_after(copy, "--key") == "data/prod/trees/8/a.pff");
	TEST_EXPECT(!store.copy("trees/7/a.pff", "../x", error));
	runner.replies.push_back(StubRunner::ok());
	TEST_EXPECT(store.copy_prefix("trees/7", "trees/8", error));
	const std::vector<std::string> &sync = runner.calls.back().argv;
	TEST_EXPECT(sync[2] == "sync" && sync[3] == "s3://opennova-backups/data/prod/trees/7/" &&
			sync[4] == "s3://opennova-backups/data/prod/trees/8/");
	TEST_EXPECT(runner.calls.back().timeout_ms == 20 * 60000);

	// A multipart upload: begin (no checksum asked for), a part's bytes through a scratch
	// file `upload-part` reads (gone after, the ETag back without its quotes), complete with
	// the parts' JSON in a scratch file, abort, and the uploads under a prefix aborted.
	runner.replies.push_back(StubRunner::ok(R"({"Bucket":"opennova-backups","Key":"data/prod/trees/8/r.pff","UploadId":"U1"})"));
	std::string upload;
	TEST_EXPECT(store.multipart_begin("trees/8/r.pff", upload, error) && upload == "U1");
	const std::vector<std::string> &begin = runner.calls.back().argv;
	TEST_EXPECT(begin[2] == "create-multipart-upload" && arg_after(begin, "--key") == "data/prod/trees/8/r.pff" &&
			!has_arg(begin, "--checksum-algorithm"));
	runner.replies.push_back(StubRunner::ok(R"({"ETag":"\"e3\""})"));
	std::string etag;
	TEST_EXPECT(store.upload_part("trees/8/r.pff", "U1", 3, "the third part's bytes", etag, error));
	TEST_EXPECT(etag == "e3");
	const StubRunner::Call &part = runner.calls.back();
	TEST_EXPECT(part.argv[2] == "upload-part" && arg_after(part.argv, "--key") == "data/prod/trees/8/r.pff" &&
			arg_after(part.argv, "--upload-id") == "U1" && arg_after(part.argv, "--part-number") == "3");
	TEST_EXPECT(part.file_text == "the third part's bytes" && part.timeout_ms == 20 * 60000);
	TEST_EXPECT(arg_after(part.argv, "--body").rfind(dir.file("scratch"), 0) == 0 &&
			!std::filesystem::exists(io::os_path(arg_after(part.argv, "--body")), ec));
	// A part the CLI refused: false with its line, the scratch file gone.
	runner.replies.push_back(StubRunner::failed(254, "An error occurred (NoSuchUpload)"));
	TEST_EXPECT(!store.upload_part("trees/8/r.pff", "U1", 4, "x", etag, error) &&
			error.find("aws s3api upload-part failed") == 0 && error.find("NoSuchUpload") != std::string::npos);
	TEST_EXPECT(!std::filesystem::exists(io::os_path(arg_after(runner.calls.back().argv, "--body")), ec));

	runner.replies.push_back(StubRunner::ok(R"({"ETag":"\"final\""})"));
	TEST_EXPECT(store.multipart_complete("trees/8/r.pff", "U1", { { 1, "e1" }, { 2, "e2" } }, error));
	const StubRunner::Call &complete = runner.calls.back();
	TEST_EXPECT(complete.argv[2] == "complete-multipart-upload" && arg_after(complete.argv, "--upload-id") == "U1");
	TEST_EXPECT(complete.file_text.find("\"PartNumber\": 1") != std::string::npos ||
			complete.file_text.find("\"PartNumber\":1") != std::string::npos);
	TEST_EXPECT(complete.file_text.find("\"ETag\": \"e2\"") != std::string::npos ||
			complete.file_text.find("\"ETag\":\"e2\"") != std::string::npos);
	TEST_EXPECT(complete.file_text.find("Checksum") == std::string::npos);

	runner.replies.push_back(StubRunner::ok());
	TEST_EXPECT(store.multipart_abort("trees/8/r.pff", "U1", error));
	TEST_EXPECT(runner.calls.back().argv[2] == "abort-multipart-upload");

	runner.replies.push_back(StubRunner::ok(
			R"({"Uploads":[{"Key":"data/prod/trees/9/a.pff","UploadId":"U9"},{"Key":"data/other/x","UploadId":"UX"}]})"));
	runner.replies.push_back(StubRunner::ok());
	TEST_EXPECT(store.abort_uploads_under("trees/9/", error));
	TEST_EXPECT(runner.calls.back().argv[2] == "abort-multipart-upload" &&
			arg_after(runner.calls.back().argv, "--upload-id") == "U9" &&
			arg_after(runner.calls.back().argv, "--key") == "data/prod/trees/9/a.pff");

	// A failed call names the operation and the CLI's own line.
	runner.replies.push_back(StubRunner::failed(1, "fatal error: An error occurred (AccessDenied)"));
	TEST_EXPECT(!store.remove("trees/8/r.pff", error) && error.find("aws s3 rm failed (1)") == 0 &&
			error.find("AccessDenied") != std::string::npos);
	// Nothing of the parts or the completions stays in the scratch directory.
	size_t left = 0;
	for (const auto &entry : std::filesystem::directory_iterator(io::os_path(dir.file("scratch")), ec))
		left += entry.is_regular_file(ec);
	TEST_EXPECT(left == 0);
	return 0;
}

} // namespace

int main() {
	test_temp::TempDir dir("object_store");
	if (int rc = test_config())
		return rc;
	if (int rc = test_local(dir))
		return rc;
	if (int rc = test_s3(dir))
		return rc;
	std::printf("object_store: ok\n");
	return 0;
}
