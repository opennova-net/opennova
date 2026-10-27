#pragma once
// An in-memory NovaWorld service behind the editor's transport seam
// (engine/editor/novaworld/novaworld_transport.h, ADR 0069), keeping the routes' contract
// (apps/novaworld_web/products/project_routes.h): the website's sign-in (the password "pw" signs in
// an admin, "player-pw" the player), the project list (the public projects, the caller's own, every
// one for an admin), one project "MOD" of the service's own (a game) with its head, its manifests
// and its files by SHA-256, the projects an account makes (POST /api/products, its owner), each
// file's download, the files it lacks with their uploads, and the push on its parent. A project its
// caller may not read is 404; its owner and an admin push it. Every request is answered as it
// starts. The NovaWorld client's and the session's tests drive real folders over it; the routes
// themselves answer the same client over HTTP in tests/novaworld/novaworld_client_routes_test.cpp.
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <editor/novaworld/novaworld_transport.h>
#include <editor/project/project_document.h>

#include "common/file_io.h"

namespace novaworld_fake {

namespace editor = opennova::editor;
namespace io = opennova::io;
using opennova::project::ProjectFile;

inline constexpr const char *kServer = "https://novaworld.test";
inline constexpr const char *kAdminCookie = "opennova_session=admin";
inline constexpr const char *kRenewedCookie = "opennova_session=renewed";
inline constexpr const char *kPlayerCookie = "opennova_session=player";
inline constexpr const char *kExpiredCookie = "opennova_session=expired";
inline constexpr const char *kPlayer = "player"; // the account kPlayerCookie is the session of

// `bytes` written at `path` as they are (test_io::write_text stages a .def's line ends as CRLF),
// its folders made.
inline bool put(const std::string &path, const std::string &bytes) {
	return test_io::write_file(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

inline editor::NovaWorldResponse json(int status, const io::JsonValue &body) {
	editor::NovaWorldResponse r;
	r.transport_ok = true;
	r.status = status;
	r.body = io::json_write(body);
	return r;
}

inline editor::NovaWorldResponse refusal(int status, const char *code) {
	io::JsonValue body = io::JsonValue::make_object();
	body.set("error", io::json_string(code));
	return json(status, body);
}

class FakeService : public editor::NovaWorldTransport {
public:
	// MOD, the service's own game: its head and its manifests (by revision), whose account owns it
	// ("" the service) and whether it is public.
	int64_t head = 0;
	std::map<int64_t, std::string> manifests;
	std::string owner;
	bool is_public = true;
	std::map<std::string, std::string> blobs; // every project's, by SHA-256
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
	// The projects the accounts made (POST /api/products), by key.
	struct Made {
		std::string title, kind, game, base, expansion_code, owner;
		bool is_public = false;
		int64_t head = 0;
		std::map<int64_t, std::string> manifests;
	};
	std::map<std::string, Made> made;

	uint64_t start(editor::NovaWorldRequest request) override {
		const uint64_t id = next_++;
		ready_[id] = handle(request);
		return id;
	}
	bool poll(uint64_t id, editor::NovaWorldResponse &out) override {
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
	// Who a request is: an admin, the player, or no one signed in.
	struct Caller {
		bool admin = false;
		std::string name;
	};
	// A project the routes name, its state by reference.
	struct Ref {
		std::string key, title, kind, game, base, expansion_code, owner;
		bool is_public = true;
		int64_t *head = nullptr;
		std::map<int64_t, std::string> *manifests = nullptr;
	};

	bool find(const std::string &key, Ref &out) {
		if (key == "MOD") {
			out = { "MOD", "A mod", "game", "jo", "", "", owner, is_public, &head, &manifests };
			return true;
		}
		if (key == "EXP" && expansion) {
			out = { "EXP", "An expansion", "expansion", "jo", "MOD", "exp", "", true,
				&expansion_head, &expansion_manifests };
			return true;
		}
		const auto it = made.find(key);
		if (it == made.end())
			return false;
		Made &m = it->second;
		out = { key, m.title, m.kind, m.game, m.base, m.expansion_code, m.owner, m.is_public,
			&m.head, &m.manifests };
		return true;
	}
	static bool may_read(const Ref &p, const Caller &caller) {
		return p.is_public || caller.admin || (!p.owner.empty() && p.owner == caller.name);
	}
	static bool may_push(const Ref &p, const Caller &caller) {
		return caller.admin || (!p.owner.empty() && p.owner == caller.name);
	}

	editor::NovaWorldResponse handle(const editor::NovaWorldRequest &request) {
		std::string cookie;
		for (const auto &[name, value] : request.headers)
			if (name == "Cookie")
				cookie = value;
		if (cookie == kExpiredCookie)
			return refusal(401, "unauthorized");
		Caller caller;
		caller.admin = cookie == kAdminCookie || cookie == kRenewedCookie;
		caller.name = caller.admin ? "admin" : cookie == kPlayerCookie ? kPlayer : "";
		editor::NovaWorldResponse reply = route(request, caller);
		if (renew_next && caller.admin) {
			reply.headers.emplace_back(
					"Set-Cookie", std::string(kRenewedCookie) + "; Path=/; HttpOnly");
			renew_next = false;
		}
		return reply;
	}

	io::JsonValue project_json(const Ref &p, const Caller &caller) {
		io::JsonValue project = io::JsonValue::make_object();
		project.set("key", io::json_string(p.key));
		project.set("title", io::json_string(p.title));
		project.set("kind", io::json_string(p.kind));
		project.set("game", io::json_string(p.game));
		project.set("expansion_code", io::json_string(p.expansion_code));
		project.set(
				"owner", p.owner.empty() ? io::JsonValue::make_null() : io::json_string(p.owner));
		project.set("visibility", io::json_string(p.is_public ? "public" : "private"));
		project.set("retail", io::JsonValue::make_bool(false));
		project.set("base", p.base.empty() ? io::JsonValue::make_null() : io::json_string(p.base));
		project.set("head", io::json_number(double(*p.head)));
		project.set("can_push", io::JsonValue::make_bool(may_push(p, caller)));
		project.set("can_manage", io::JsonValue::make_bool(may_push(p, caller)));
		return project;
	}

	// POST /api/products: a new project of the caller's.
	editor::NovaWorldResponse create(
			const editor::NovaWorldRequest &request, const Caller &caller) {
		if (caller.name.empty())
			return refusal(401, "unauthorized");
		io::JsonValue body;
		std::string error;
		if (!io::json_parse(request.body, body, error))
			return refusal(400, "invalid_json");
		const std::string key = body.get_string("key", "");
		Ref existing;
		if (!editor::is_novaworld_key(key))
			return refusal(400, "invalid_field");
		if (find(key, existing))
			return refusal(409, "duplicate_key");
		Made m;
		m.title = body.get_string("display_name", "");
		m.kind = body.get_string("kind", "");
		m.owner = caller.name;
		m.is_public = body.get_string("visibility", "") == "public";
		if (m.kind == "game") {
			m.game = body.get_string("game", "");
			if (m.game != "jo" && m.game != "dfx2")
				return refusal(400, "invalid_field");
		} else if (m.kind == "expansion") {
			m.base = body.get_string("base", "");
			m.expansion_code = body.get_string("expansion_code", "");
			Ref base;
			if (!find(m.base, base) || base.kind != "game")
				return refusal(404, "no_such_base");
			m.game = base.game;
		} else {
			return refusal(400, "invalid_field");
		}
		made[key] = m;
		Ref ref;
		find(key, ref);
		io::JsonValue out = io::JsonValue::make_object();
		out.set("project", project_json(ref, caller));
		return json(201, out);
	}

	editor::NovaWorldResponse route(const editor::NovaWorldRequest &request, const Caller &caller) {
		io::JsonValue body;
		std::string error;
		if (request.method == "POST" && request.url == std::string(kServer) + "/api/login") {
			if (!io::json_parse(request.body, body, error))
				return refusal(400, "invalid_json");
			const std::string password = body.get_string("password", "");
			if (password != "pw" && password != "player-pw")
				return refusal(401, "invalid_credentials");
			const bool admin = password == "pw";
			io::JsonValue out = io::JsonValue::make_object();
			out.set("id", io::json_number(admin ? 1 : 2));
			out.set("username", io::json_string(body.get_string("username", "")));
			out.set("role", io::json_string(admin ? "admin" : "player"));
			editor::NovaWorldResponse r = json(200, out);
			r.headers.emplace_back("Set-Cookie",
					std::string(admin ? kAdminCookie : kPlayerCookie) + "; Path=/; HttpOnly");
			return r;
		}
		if (request.method == "POST" && request.url == std::string(kServer) + "/api/logout")
			return json(200, io::JsonValue::make_object());
		if (request.url == std::string(kServer) + "/api/products") {
			if (request.method == "POST")
				return create(request, caller);
			io::JsonValue list = io::JsonValue::make_array();
			std::vector<std::string> keys = { "MOD", "EXP" };
			for (const auto &[key, m] : made)
				keys.push_back(key);
			for (const std::string &key : keys) {
				Ref p;
				if (find(key, p) && may_read(p, caller))
					list.push(project_json(p, caller));
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("projects", list);
			return json(200, out);
		}
		if (request.method == "POST" &&
				request.url == std::string(kServer) + "/api/admin/products/MOD/releases") {
			if (!caller.admin)
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
		// The project the route names, which the caller may read.
		const std::string projects = std::string(kServer) + "/api/products/";
		if (request.url.rfind(projects, 0) != 0)
			return refusal(404, "not_found");
		const std::string named = request.url.substr(projects.size());
		const std::string key = named.substr(0, named.find_first_of("/?"));
		Ref p;
		if (!find(key, p) || !may_read(p, caller))
			return refusal(404, "not_found");
		int64_t &at = *p.head;
		std::map<int64_t, std::string> &kept = *p.manifests;
		const std::string rest = named.substr(key.size());
		if (request.method == "GET" && rest.empty()) {
			io::JsonValue out = io::JsonValue::make_object();
			out.set("project", project_json(p, caller));
			if (at > 0) {
				io::JsonValue revision = io::JsonValue::make_object();
				revision.set("number", io::json_number(double(at)));
				revision.set("manifest_sha256",
						io::json_string(opennova::project::manifest_sha256(kept[at])));
				out.set("head_revision", revision);
			}
			return json(200, out);
		}
		if (request.method == "GET" && rest.rfind("/revisions/", 0) == 0) {
			const int64_t number = std::strtoll(rest.c_str() + 11, nullptr, 10);
			editor::NovaWorldResponse r;
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
						io::json_string(opennova::project::manifest_sha256(kept[number])));
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
			editor::NovaWorldResponse r;
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
				entry.set("url", io::json_string("/api/products/" + key + "/blobs/" + sha.string));
				entry.set("headers", io::JsonValue::make_object());
				list.push(entry);
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("downloads", list);
			return json(200, out);
		}
		if (!may_push(p, caller))
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
				upload.set("url", io::json_string("/api/products/" + key + "/blobs/" + sha));
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
			std::vector<ProjectFile> files;
			if (!opennova::project::decode_manifest(text, files, error))
				return refusal(400, "invalid_manifest");
			for (const ProjectFile &file : files)
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
	std::map<uint64_t, editor::NovaWorldResponse> ready_;
};

} // namespace novaworld_fake
