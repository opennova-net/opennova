#pragma once
// An in-memory NovaWorld service behind the editor's transport seam
// (engine/editor/novaworld/novaworld_transport.h, ADR 0069), keeping the routes' contract
// (apps/novaworld_web/products/project_routes.h): the website's sign-in (the password "pw" signs in
// an admin, "player-pw" the player), the project list (the public projects, the caller's own, every
// one for an admin), one project "MOD" of the service's own (a game, a retail game's while `retail`
// is set) with its head, its manifests and its files by SHA-256, the projects an account makes (POST
// /api/products, its owner; 409 duplicate_expansion on the expansion_code field for a mod's short
// name another project holds, its case aside) and the games it may make a mod of (GET
// /api/me/projects), each file's download, the files it lacks with their uploads (413 quota_exceeded
// past an owner's storage), the push on its parent, and a release asked of a project (POST
// /api/products/<key>/releases, refused as `refuse_release` names). A project its caller may not
// read is 404, one it reads but may not push 403; its owner and an admin push and release it, each
// project's published release its `published` version. Every request is answered as it starts. The NovaWorld client's and the session's tests drive
// real folders over it; the routes themselves answer the same client over HTTP in
// tests/novaworld/novaworld_client_routes_test.cpp.
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/base64.h>
#include <base/io/json.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
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
	// ("" the service), whether it is public, and the title it (and EXP, its mod) speaks.
	int64_t head = 0;
	std::map<int64_t, std::string> manifests;
	std::string owner;
	bool is_public = true;
	bool retail = false;
	std::string mod_game = "jo";
	std::map<std::string, std::string> blobs; // every project's, by SHA-256
	bool renew_next = false; // the next reply renews the admin's session
	bool sessions_ended = false; // every signed-in request answered 401 (a session the service ended)
	size_t uploads = 0, downloads = 0;
	// The transfers' calls in the order they came: "where" (a downloads ask, its files' count after a colon),
	// "get" (a blob downloaded), "lacks" (a missing ask, likewise) and "put" (a blob uploaded).
	std::vector<std::string> transfers;
	// The last release asked for: its project's key, its version and its revision; the code the next one is
	// refused with ("release_in_progress", "too_many_releases", "quota_exceeded"; "" none); and each project's
	// published release's version, by key.
	std::string release_key;
	std::string release_version;
	int64_t release_revision = 0;
	std::string refuse_release;
	std::map<std::string, std::string> published;
	// Every release asked for, newest last: its id, its project's key, its version, its status (building;
	// a test makes it a draft) and an admin's note on one sent back. A draft's publish publishes an
	// admin's, and submits an owner's (awaiting_approval).
	struct Release {
		int64_t id = 0;
		std::string key, version, status, note;
	};
	std::vector<Release> releases;
	// A second project, EXP, an expansion of MOD (its base), while `expansion` is set: its head and its
	// manifests (its files `blobs`').
	bool expansion = false;
	int64_t expansion_head = 0;
	std::map<int64_t, std::string> expansion_manifests;
	// The bytes the accounts' own projects may store, 0 no limit, and those they store.
	int64_t quota = 0;
	int64_t stored = 0;
	// The projects the accounts made (POST /api/products), by key.
	struct Made {
		std::string title, kind, game, base, expansion_code, owner;
		std::string lobby, new_lobby_name;
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
		bool retail = false;
		int64_t *head = nullptr;
		std::map<int64_t, std::string> *manifests = nullptr;
	};

	bool find(const std::string &key, Ref &out) {
		if (key == "MOD") {
			out = { "MOD", "A mod", "game", mod_game, "", "", owner, is_public, retail, &head, &manifests };
			return true;
		}
		if (key == "EXP" && expansion) {
			out = { "EXP", "An expansion", "expansion", mod_game, "MOD", "exp", "", true, false,
				&expansion_head, &expansion_manifests };
			return true;
		}
		const auto it = made.find(key);
		if (it == made.end())
			return false;
		Made &m = it->second;
		out = { key, m.title, m.kind, m.game, m.base, m.expansion_code, m.owner, m.is_public, false,
			&m.head, &m.manifests };
		return true;
	}
	// Every project's key, the service's own first.
	std::vector<std::string> keys() const {
		std::vector<std::string> out = { "MOD", "EXP" };
		for (const auto &[key, m] : made)
			out.push_back(key);
		return out;
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
		if (cookie == kExpiredCookie || (sessions_ended && !cookie.empty()))
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

	// A protocol as the service names it: {code, name, version} (the title's: JO's for jo and
	// acidgambit, DFX2's for dfx2).
	static io::JsonValue protocol_json(const std::string &title) {
		io::JsonValue out = io::JsonValue::make_object();
		const bool dfx2 = title == "dfx2";
		out.set("code", io::json_string(dfx2 ? "dfx2" : "jo"));
		out.set("name", io::json_string(dfx2 ? "DFX2" : "JO"));
		out.set("version", io::json_string("16"));
		return out;
	}

	static io::JsonValue release_json(const Release &r) {
		io::JsonValue out = io::JsonValue::make_object();
		out.set("id", io::json_number(double(r.id)));
		out.set("name", io::json_string(r.key + " " + r.version));
		out.set("version", io::json_string(r.version));
		out.set("one_version", io::json_string("0.0.22"));
		out.set("status", io::json_string(r.status));
		out.set("review_note", io::json_string(r.note));
		out.set("error", io::json_string(""));
		return out;
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
		project.set("retail", io::JsonValue::make_bool(p.retail));
		project.set("base", p.base.empty() ? io::JsonValue::make_null() : io::json_string(p.base));
		project.set("head", io::json_number(double(*p.head)));
		project.set("can_push", io::JsonValue::make_bool(may_push(p, caller)));
		project.set("can_manage", io::JsonValue::make_bool(may_push(p, caller)));
		project.set("can_release", io::JsonValue::make_bool(may_push(p, caller)));
		io::JsonValue lobby = io::JsonValue::make_object();
		const std::string lobby_key = p.base.empty() ? p.key : p.base;
		const auto selected = made.find(lobby_key);
		const std::string slug = selected != made.end() && !selected->second.lobby.empty()
				? selected->second.lobby
				: "mod_lobby";
		lobby.set("slug", io::json_string(slug));
		lobby.set("display_name",
				io::json_string(selected != made.end() && !selected->second.new_lobby_name.empty()
								? selected->second.new_lobby_name
								: "Mod lobby"));
		lobby.set("protocol", protocol_json(p.game));
		const std::string artifact = "service-generated:" + slug;
		lobby.set("gate_tag_file",
				io::json_string(
						io::base64_encode(std::vector<uint8_t>(artifact.begin(), artifact.end()))));
		project.set("lobby", std::move(lobby));
		project.set("lobby_locked",
				io::JsonValue::make_bool(p.kind == "expansion" || published.count(p.key)));
		const auto release = published.find(p.key);
		if (release == published.end()) {
			project.set("release", io::JsonValue::make_null());
		} else {
			io::JsonValue row = io::JsonValue::make_object();
			row.set("id", io::json_number(7));
			row.set("version", io::json_string(release->second));
			row.set("one_version", io::json_string("0.0.22"));
			project.set("release", row);
		}
		// An expansion's version.txt as its release build writes it, once a release names its version.
		if (p.kind == "expansion" && release != published.end()) {
			const std::string line = p.title + ", V" + release->second + "; English\r\n";
			project.set("version_file",
					io::json_string(io::base64_encode(std::vector<uint8_t>(line.begin(), line.end()))));
		} else {
			project.set("version_file", io::JsonValue::make_null());
		}
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
			m.lobby = body.get_string("lobby", "");
			m.new_lobby_name = body.get_string("new_lobby_name", "");
			if (m.game != "jo" && m.game != "dfx2" && m.game != "acidgambit")
				return refusal(400, "invalid_field");
		} else if (m.kind == "expansion") {
			m.base = body.get_string("base", "");
			m.expansion_code = body.get_string("expansion_code", "");
			Ref base;
			if (!find(m.base, base) || base.kind != "game")
				return refusal(404, "no_such_base");
			m.game = base.game;
			// A mod's short name is one project's across every game, its case aside.
			for (const std::string &other : keys()) {
				Ref held;
				if (find(other, held) && !held.expansion_code.empty() &&
						opennova::strutil::iequals(held.expansion_code, m.expansion_code)) {
					io::JsonValue taken = io::JsonValue::make_object();
					taken.set("error", io::json_string("duplicate_expansion"));
					taken.set("field", io::json_string("expansion_code"));
					taken.set("message", io::json_string("another mod has this short name"));
					return json(409, taken);
				}
			}
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
		// The caller's own projects, and the games it may make a mod of: the service's own and its
		// own.
		if (request.method == "GET" && request.url == std::string(kServer) + "/api/me/projects") {
			if (caller.name.empty())
				return refusal(401, "unauthorized");
			io::JsonValue own = io::JsonValue::make_array();
			io::JsonValue games = io::JsonValue::make_array();
			std::vector<std::string> listed = { "MOD" };
			for (const auto &[key, m] : made)
				listed.push_back(key);
			for (const std::string &key : listed) {
				Ref p;
				if (!find(key, p))
					continue;
				if (!p.owner.empty() && p.owner == caller.name)
					own.push(project_json(p, caller));
				if (p.kind == "game" && (p.owner.empty() || p.owner == caller.name)) {
					io::JsonValue game = io::JsonValue::make_object();
					game.set("key", io::json_string(p.key));
					game.set("title", io::json_string(p.title));
					game.set("game", io::json_string(p.game));
					games.push(game);
				}
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("projects", own);
			out.set("games", games);
			out.set("titles", io::json_string_array({ "jo", "dfx2", "acidgambit" }));
			io::JsonValue lobbies = io::JsonValue::make_array();
			for (const char *title : { "jo", "dfx2" }) {
				io::JsonValue lobby = io::JsonValue::make_object();
				lobby.set("slug", io::json_string(std::string(title) + "_lobby"));
				lobby.set("display_name", io::json_string(std::string(title) + " lobby"));
				lobby.set("protocol", protocol_json(title));
				lobbies.push(std::move(lobby));
			}
			out.set("lobbies", std::move(lobbies));
			return json(200, out);
		}
		if (request.url == std::string(kServer) + "/api/products") {
			if (request.method == "POST")
				return create(request, caller);
			io::JsonValue list = io::JsonValue::make_array();
			for (const std::string &key : keys()) {
				Ref p;
				if (find(key, p) && may_read(p, caller))
					list.push(project_json(p, caller));
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("projects", list);
			return json(200, out);
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
		if (request.method == "POST" && rest == "/releases") {
			if (!io::json_parse(request.body, body, error))
				return refusal(400, "invalid_json");
			if (refuse_release == "quota_exceeded") {
				io::JsonValue over = io::JsonValue::make_object();
				over.set("error", io::json_string("quota_exceeded"));
				over.set("used", io::json_number(double(stored)));
				over.set("limit", io::json_number(double(quota)));
				return json(413, over);
			}
			if (!refuse_release.empty())
				return refusal(refuse_release == "release_in_progress" ? 409 : 429,
						refuse_release.c_str());
			release_key = key;
			release_version = body.get_string("version", "");
			release_revision = int64_t(body.get_number("revision", 0));
			releases.push_back({ int64_t(releases.size()) + 7, key, release_version, "building", "" });
			io::JsonValue out = io::JsonValue::make_object();
			out.set("release", release_json(releases.back()));
			return json(201, out);
		}
		if (request.method == "GET" && rest == "/releases") {
			io::JsonValue list = io::JsonValue::make_array();
			for (auto it = releases.rbegin(); it != releases.rend(); ++it)
				if (it->key == key)
					list.push(release_json(*it));
			io::JsonValue out = io::JsonValue::make_object();
			out.set("releases", list);
			return json(200, out);
		}
		if (request.method == "POST" && rest.rfind("/releases/", 0) == 0 &&
				rest.size() > 18 && rest.compare(rest.size() - 8, 8, "/publish") == 0) {
			const int64_t id = std::strtoll(rest.c_str() + 10, nullptr, 10);
			for (Release &release : releases)
				if (release.id == id && release.key == key) {
					if (release.status != "draft")
						return refusal(409, "not_draft");
					release.status = caller.admin ? "published" : "awaiting_approval";
					if (caller.admin)
						published[key] = release.version;
					io::JsonValue out = io::JsonValue::make_object();
					out.set("release", release_json(release));
					return json(202, out);
				}
			return refusal(404, "not_found");
		}
		if (request.method == "POST" && rest == "/missing" &&
				io::json_parse(request.body, body, error)) {
			io::JsonValue list = io::JsonValue::make_array();
			transfers.push_back("lacks:" + std::to_string(body.get("blobs")->array.size()));
			int64_t lacking = 0;
			for (const io::JsonValue &item : body.get("blobs")->array)
				if (!blobs.count(item.get_string("sha256", "")))
					lacking += int64_t(item.get_number("size", 0));
			if (quota > 0 && !p.owner.empty() && stored + lacking > quota) {
				io::JsonValue over = io::JsonValue::make_object();
				over.set("error", io::json_string("quota_exceeded"));
				over.set("message", io::json_string("the account's projects would store too much"));
				over.set("used", io::json_number(double(stored)));
				over.set("limit", io::json_number(double(quota)));
				return json(413, over);
			}
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
			if (!p.owner.empty())
				stored += int64_t(bytes.size());
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
