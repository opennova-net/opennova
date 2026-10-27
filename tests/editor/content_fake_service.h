#pragma once
// An in-memory content store service behind the editor's transport seam
// (engine/editor/content_store/content_transport.h, ADR 0069), keeping the routes' contract
// (apps/novaworld_web/products/project_routes.h): the website's sign-in (the password "pw" signs in
// an admin), the project list, one project "MOD" with its head, its manifests and its files by
// SHA-256, each file's download, the files it lacks with their uploads, and the push on its parent.
// Every request is answered as it starts. The content client's and the session's tests drive real
// folders over it; the routes themselves answer the same client over HTTP in
// tests/novaworld/content_client_routes_test.cpp.
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <base/content/content_manifest.h>
#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/sha256.h>
#include <editor/content_store/content_transport.h>

#include "common/file_io.h"

namespace content_fake {

namespace editor = opennova::editor;
namespace io = opennova::io;
using opennova::content::ContentFile;

inline constexpr const char *kServer = "https://content.test";
inline constexpr const char *kAdminCookie = "opennova_session=admin";
inline constexpr const char *kRenewedCookie = "opennova_session=renewed";
inline constexpr const char *kPlayerCookie = "opennova_session=player";
inline constexpr const char *kExpiredCookie = "opennova_session=expired";

// `bytes` written at `path` as they are (test_io::write_text stages a .def's line ends as CRLF),
// its folders made.
inline bool put(const std::string &path, const std::string &bytes) {
	return test_io::write_file(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

inline editor::ContentResponse json(int status, const io::JsonValue &body) {
	editor::ContentResponse r;
	r.transport_ok = true;
	r.status = status;
	r.body = io::json_write(body);
	return r;
}

inline editor::ContentResponse refusal(int status, const char *code) {
	io::JsonValue body = io::JsonValue::make_object();
	body.set("error", io::json_string(code));
	return json(status, body);
}

class FakeService : public editor::ContentTransport {
public:
	int64_t head = 0;
	std::map<int64_t, std::string> manifests; // by revision
	std::map<std::string, std::string> blobs; // by SHA-256
	bool renew_next = false; // the next reply renews the admin's session
	size_t uploads = 0, downloads = 0;
	// The transfers' calls in the order they came: "where" (a downloads ask, its files' count after a colon),
	// "get" (a blob downloaded), "lacks" (a missing ask, likewise) and "put" (a blob uploaded).
	std::vector<std::string> transfers;
	// The last release asked for: its version and revision.
	std::string release_version;
	int64_t release_revision = 0;
	// A second project, EXP, an expansion of MOD (its base), while `expansion` is set: its head and its
	// manifests (its files `blobs`').
	bool expansion = false;
	int64_t expansion_head = 0;
	std::map<int64_t, std::string> expansion_manifests;

	uint64_t start(editor::ContentRequest request) override {
		const uint64_t id = next_++;
		ready_[id] = handle(request);
		return id;
	}
	bool poll(uint64_t id, editor::ContentResponse &out) override {
		const auto it = ready_.find(id);
		if (it == ready_.end())
			return false;
		out = std::move(it->second);
		ready_.erase(it);
		return true;
	}
	void cancel(uint64_t id) override { ready_.erase(id); }
	size_t parallel() const override { return 3; }

private:
	editor::ContentResponse handle(const editor::ContentRequest &request) {
		std::string cookie;
		for (const auto &[name, value] : request.headers)
			if (name == "Cookie")
				cookie = value;
		if (cookie == kExpiredCookie)
			return refusal(401, "unauthorized");
		const bool admin = cookie == kAdminCookie || cookie == kRenewedCookie;
		editor::ContentResponse reply = route(request, admin);
		if (renew_next && admin) {
			reply.headers.emplace_back(
					"Set-Cookie", std::string(kRenewedCookie) + "; Path=/; HttpOnly");
			renew_next = false;
		}
		return reply;
	}

	io::JsonValue project_json(bool admin, const char *key, int64_t at, const char *base) {
		io::JsonValue project = io::JsonValue::make_object();
		project.set("key", io::json_string(key));
		project.set("title", io::json_string(*base ? "An expansion" : "A mod"));
		project.set("visibility", io::json_string("public"));
		project.set("base", *base ? io::json_string(base) : io::JsonValue::make_null());
		project.set("head", io::json_number(double(at)));
		project.set("can_push", io::JsonValue::make_bool(admin));
		return project;
	}

	editor::ContentResponse route(const editor::ContentRequest &request, bool admin) {
		io::JsonValue body;
		std::string error;
		if (request.method == "POST" && request.url == std::string(kServer) + "/api/login") {
			if (!io::json_parse(request.body, body, error) ||
					body.get_string("password", "") != "pw")
				return refusal(401, "invalid_credentials");
			io::JsonValue out = io::JsonValue::make_object();
			out.set("id", io::json_number(1));
			out.set("username", io::json_string(body.get_string("username", "")));
			out.set("role", io::json_string("admin"));
			editor::ContentResponse r = json(200, out);
			r.headers.emplace_back("Set-Cookie", std::string(kAdminCookie) + "; Path=/; HttpOnly");
			return r;
		}
		if (request.method == "POST" && request.url == std::string(kServer) + "/api/logout")
			return json(200, io::JsonValue::make_object());
		if (request.method == "GET" &&
				request.url == std::string(kServer) + "/api/content/projects") {
			io::JsonValue list = io::JsonValue::make_array();
			list.push(project_json(admin, "MOD", head, ""));
			if (expansion)
				list.push(project_json(admin, "EXP", expansion_head, "MOD"));
			io::JsonValue out = io::JsonValue::make_object();
			out.set("projects", list);
			return json(200, out);
		}
		if (request.method == "POST" &&
				request.url == std::string(kServer) + "/api/admin/content/projects/MOD/releases") {
			if (!admin)
				return refusal(403, "forbidden");
			if (!io::json_parse(request.body, body, error))
				return refusal(400, "invalid_json");
			release_version = body.get_string("version", "");
			release_revision = int64_t(body.get_number("revision", 0));
			io::JsonValue release = io::JsonValue::make_object();
			release.set("id", io::json_number(7));
			release.set("name", io::json_string("MOD " + release_version));
			release.set("status", io::json_string("building"));
			io::JsonValue out = io::JsonValue::make_object();
			out.set("release", release);
			return json(201, out);
		}
		// The project the route names: MOD, or EXP while the expansion is there.
		const std::string projects = std::string(kServer) + "/api/content/projects/";
		if (request.url.rfind(projects, 0) != 0)
			return refusal(404, "not_found");
		const std::string named = request.url.substr(projects.size());
		const auto names = [&](const char *key) {
			return named.rfind(key, 0) == 0 && (named.size() == 3 || named[3] == '/' || named[3] == '?');
		};
		const bool exp = expansion && names("EXP");
		if (!exp && !names("MOD"))
			return refusal(404, "not_found");
		const std::string key = exp ? "EXP" : "MOD";
		int64_t &at = exp ? expansion_head : head;
		std::map<int64_t, std::string> &kept = exp ? expansion_manifests : manifests;
		const std::string rest = named.substr(3);
		if (request.method == "GET" && rest.empty()) {
			io::JsonValue out = io::JsonValue::make_object();
			out.set("project", project_json(admin, key.c_str(), at, exp ? "MOD" : ""));
			if (at > 0) {
				io::JsonValue revision = io::JsonValue::make_object();
				revision.set("number", io::json_number(double(at)));
				revision.set("manifest_sha256",
						io::json_string(opennova::content::manifest_sha256(kept[at])));
				out.set("head_revision", revision);
			}
			return json(200, out);
		}
		if (request.method == "GET" && rest.rfind("/revisions/", 0) == 0) {
			const int64_t number = std::strtoll(rest.c_str() + 11, nullptr, 10);
			editor::ContentResponse r;
			r.transport_ok = true;
			r.status = kept.count(number) ? 200 : 404;
			r.body = kept[number];
			return r;
		}
		// The history, newest first.
		if (request.method == "GET" && (rest == "/revisions" || rest.rfind("/revisions?", 0) == 0)) {
			io::JsonValue list = io::JsonValue::make_array();
			for (int64_t number = at; number >= 1; --number) {
				io::JsonValue revision = io::JsonValue::make_object();
				revision.set("number", io::json_number(double(number)));
				revision.set("author", io::json_string("taylor"));
				revision.set("message", io::json_string("r" + std::to_string(number)));
				revision.set("manifest_sha256",
						io::json_string(opennova::content::manifest_sha256(kept[number])));
				revision.set("file_count", io::json_number(1));
				revision.set("bytes", io::json_number(1));
				revision.set("created_at", io::json_string("2026-10-25 12:00:00"));
				list.push(revision);
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("revisions", list);
			return json(200, out);
		}
		if (request.method == "GET" && rest.rfind("/blobs/", 0) == 0) {
			const auto blob = blobs.find(rest.substr(7));
			if (blob == blobs.end())
				return refusal(404, "not_found");
			editor::ContentResponse r;
			r.transport_ok = true;
			r.status = 200;
			io::write_file_atomic(request.response_file, blob->second, error);
			r.file_bytes = blob->second.size();
			r.file_sha256 = io::sha256_hex(blob->second.data(), blob->second.size());
			++downloads;
			transfers.push_back("get");
			return r;
		}
		// Where each file is downloaded from: the service's own blob route, as a store with no
		// signed URLs of its own answers.
		if (request.method == "POST" && rest == "/downloads" &&
				io::json_parse(request.body, body, error)) {
			io::JsonValue list = io::JsonValue::make_array();
			transfers.push_back("where:" + std::to_string(body.get("hashes")->array.size()));
			for (const io::JsonValue &sha : body.get("hashes")->array) {
				io::JsonValue entry = io::JsonValue::make_object();
				entry.set("sha256", sha);
				entry.set("method", io::json_string("GET"));
				entry.set("url", io::json_string("/api/content/projects/" + key + "/blobs/" + sha.string));
				entry.set("headers", io::JsonValue::make_object());
				list.push(entry);
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("downloads", list);
			return json(200, out);
		}
		if (!admin)
			return refusal(403, "forbidden");
		if (request.method == "POST" && rest == "/missing" &&
				io::json_parse(request.body, body, error)) {
			io::JsonValue list = io::JsonValue::make_array();
			transfers.push_back("lacks:" + std::to_string(body.get("blobs")->array.size()));
			for (const io::JsonValue &item : body.get("blobs")->array) {
				const std::string sha = item.get_string("sha256", "");
				if (blobs.count(sha))
					continue;
				io::JsonValue upload = io::JsonValue::make_object();
				upload.set("method", io::json_string("PUT"));
				upload.set("url", io::json_string("/api/content/projects/" + key + "/blobs/" + sha));
				io::JsonValue entry = io::JsonValue::make_object();
				entry.set("sha256", io::json_string(sha));
				entry.set("size", *item.get("size"));
				entry.set("upload", upload);
				list.push(entry);
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("missing", list);
			return json(200, out);
		}
		if (request.method == "PUT" && rest.rfind("/blobs/", 0) == 0) {
			std::string bytes;
			if (!io::read_file_text(request.body_file, bytes, error))
				return refusal(400, "unreadable");
			if (io::sha256_hex(bytes.data(), bytes.size()) != rest.substr(7))
				return refusal(400, "sha256_mismatch");
			blobs[rest.substr(7)] = bytes;
			++uploads;
			transfers.push_back("put");
			return json(200, io::JsonValue::make_object());
		}
		if (request.method == "POST" && rest == "/revisions" &&
				io::json_parse(request.body, body, error)) {
			const int64_t parent = int64_t(body.get_number("parent", -1));
			const std::string text = body.get_string("manifest", "");
			if (parent != at) {
				io::JsonValue stale = io::JsonValue::make_object();
				stale.set("error", io::json_string("stale"));
				stale.set("head", io::json_number(double(at)));
				return json(409, stale);
			}
			std::vector<ContentFile> files;
			if (!opennova::content::decode_manifest(text, files, error))
				return refusal(400, "invalid_manifest");
			for (const ContentFile &file : files)
				if (!blobs.count(file.sha256))
					return refusal(400, "missing_blobs");
			kept[++at] = text;
			io::JsonValue out = io::JsonValue::make_object();
			out.set("head", io::json_number(double(at)));
			return json(201, out);
		}
		return refusal(404, "not_found");
	}

	uint64_t next_ = 1;
	std::map<uint64_t, editor::ContentResponse> ready_;
};

} // namespace content_fake
