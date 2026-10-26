#include <editor/content_store/content_api.h>

#include <base/io/json.h>
#include <base/io/strutil.h>

namespace opennova::editor {

namespace {

// The service's session cookie, by its two names (apps/novaworld_common/accounts/web_session.h).
constexpr const char *kSessionCookie = "opennova_session";
constexpr const char *kSecureSessionCookie = "__Host-opennova_session";
constexpr const char *kCsrfHeader = "X-OpenNova-Request";

ContentRequest request(const ContentAccount &account, const char *method, const std::string &path) {
	ContentRequest r;
	r.method = method;
	r.url = account.server + path;
	if (account.signed_in())
		r.headers.emplace_back("Cookie", account.cookie);
	if (std::string(method) != "GET")
		r.headers.emplace_back(kCsrfHeader, "1");
	return r;
}

ContentRequest json_request(const ContentAccount &account, const char *method,
		const std::string &path, const io::JsonValue &body) {
	ContentRequest r = request(account, method, path);
	r.headers.emplace_back("Content-Type", "application/json");
	r.body = io::json_write(body);
	return r;
}

std::string project_path(const std::string &key) {
	return "/api/content/projects/" + key;
}

// The reply's body as a JSON object, or the error it is: a reply that is not `want`, or not JSON.
bool json_reply(
		const ContentResponse &reply, int want, io::JsonValue &out, ContentApiError &error) {
	error = ContentApiError();
	if (!reply.transport_ok) {
		error.code = "transport";
		error.message = reply.error.empty() ? "no reply from the service" : reply.error;
		return false;
	}
	error.status = reply.status;
	std::string parse_error;
	const bool parsed = io::json_parse(reply.body, out, parse_error) && out.is_object();
	if (reply.status != want) {
		error.code = parsed ? out.get_string("error", "") : std::string();
		error.message = parsed ? out.get_string("message", "") : std::string();
		if (error.code.empty())
			error.code = "http_" + std::to_string(reply.status);
		if (error.message.empty())
			error.message = "the service answered HTTP " + std::to_string(reply.status);
		return false;
	}
	if (!parsed) {
		error.code = "bad_reply";
		error.message = "the service's reply is not JSON";
		return false;
	}
	return true;
}

int64_t whole(const io::JsonValue &json, const char *key) {
	const io::JsonValue *value = json.get(key);
	int64_t out = 0;
	return value &&
					io::json_exact_whole_in(
							*value, -io::kJsonSafeWholeMax, io::kJsonSafeWholeMax, out)
			? out
			: 0;
}

RemoteRevision revision_of(const io::JsonValue &json) {
	RemoteRevision r;
	r.number = whole(json, "number");
	r.author = json.get_string("author", "");
	r.message = json.get_string("message", "");
	r.manifest_sha256 = json.get_string("manifest_sha256", "");
	r.file_count = whole(json, "file_count");
	r.bytes = whole(json, "bytes");
	r.created_at = json.get_string("created_at", "");
	return r;
}

RemoteProject project_of(const io::JsonValue &json) {
	RemoteProject p;
	p.key = json.get_string("key", "");
	p.title = json.get_string("title", "");
	p.is_public = json.get_string("visibility", "") == "public";
	p.retail = json.get_bool("retail", false);
	p.base = json.get_string("base", "");
	p.head = whole(json, "head");
	p.can_push = json.get_bool("can_push", false);
	return p;
}

// A transfer the service handed out ({method, url, headers}) as the request that makes it: a route
// of the service's own (a URL from '/') sent as the account, with its session and the CSRF header;
// a URL of the store's own sent as it was handed out, with its headers alone (no cookie leaves the
// service).
ContentRequest transfer_request(
		const ContentAccount &account, const io::JsonValue &transfer, const char *method) {
	ContentRequest r;
	r.method = transfer.get_string("method", method);
	const std::string url = transfer.get_string("url", "");
	const bool own = !url.empty() && url[0] == '/';
	r.url = own ? account.server + url : url;
	if (own) {
		if (account.signed_in())
			r.headers.emplace_back("Cookie", account.cookie);
		if (r.method != "GET")
			r.headers.emplace_back(kCsrfHeader, "1");
	}
	if (const io::JsonValue *headers = transfer.get("headers"); headers && headers->is_object())
		for (const io::JsonMember &header : headers->object)
			if (header.value.is_string())
				r.headers.emplace_back(header.key, header.value.string);
	return r;
}

// A Set-Cookie value's name and value ("name=value"), "" for one of another cookie.
std::string session_pair(const std::string &set_cookie, bool &deleted) {
	const std::string pair = set_cookie.substr(0, set_cookie.find(';'));
	const size_t eq = pair.find('=');
	if (eq == std::string::npos)
		return {};
	const std::string name = pair.substr(0, eq);
	if (name != kSessionCookie && name != kSecureSessionCookie)
		return {};
	deleted = eq + 1 == pair.size();
	return deleted ? std::string() : pair;
}

} // namespace

std::string ContentResponse::header(const std::string &name) const {
	for (const auto &[key, value] : headers)
		if (strutil::iequals(key, name))
			return value;
	return {};
}

std::string ContentAddress::origin() const {
	return (tls ? "https://" : "http://") + host + ":" + std::to_string(port);
}

bool content_address(const std::string &url, ContentAddress &out) {
	out = ContentAddress();
	std::string rest;
	if (url.rfind("https://", 0) == 0) {
		out.tls = true;
		out.port = 443;
		rest = url.substr(8);
	} else if (url.rfind("http://", 0) == 0) {
		out.port = 80;
		rest = url.substr(7);
	} else {
		return false;
	}
	const size_t end = rest.find_first_of("/?#");
	const std::string authority = rest.substr(0, end);
	out.target = end == std::string::npos ? "/" : rest.substr(end);
	if (out.target[0] != '/')
		out.target = "/" + out.target;
	const size_t hash = out.target.find('#');
	if (hash != std::string::npos)
		out.target.resize(hash);
	if (authority.find_first_of("@[]") != std::string::npos)
		return false;
	const size_t colon = authority.find(':');
	out.host = authority.substr(0, colon);
	if (colon != std::string::npos) {
		const std::optional<int> port = strutil::parse_int(authority.substr(colon + 1));
		if (!port || *port < 1 || *port > 65535)
			return false;
		out.port = *port;
	}
	return !out.host.empty();
}

bool content_goes_again(
		const std::string &method, int attempt, bool replied, int status, bool lost) {
	const bool idempotent = method == "GET" || method == "PUT" || method == "DELETE";
	return idempotent && attempt < kContentRetries && (lost || (replied && status >= 500));
}

std::string ContentApiError::line() const {
	std::string out = message.empty() ? code : message;
	out += " (" + code;
	if (status != 0)
		out += ", HTTP " + std::to_string(status);
	return out + ")";
}

namespace content_api {

ContentRequest login(
		const std::string &server, const std::string &username, const std::string &password) {
	ContentAccount anonymous;
	anonymous.server = content_server(server);
	io::JsonValue body = io::JsonValue::make_object();
	body.set("username", io::json_string(username));
	body.set("password", io::json_string(password));
	return json_request(anonymous, "POST", "/api/login", body);
}

bool read_login(const ContentResponse &reply, ContentAccount &account, ContentApiError &error) {
	io::JsonValue json;
	if (!json_reply(reply, 200, json, error))
		return false;
	bool deleted = false;
	const std::string cookie = renewed_cookie(reply, deleted);
	if (cookie.empty()) {
		error.code = "no_session";
		error.message = "the service signed in no session";
		return false;
	}
	account.cookie = cookie;
	account.username = json.get_string("username", account.username);
	account.role = json.get_string("role", "player");
	return true;
}

ContentRequest logout(const ContentAccount &account) {
	return request(account, "POST", "/api/logout");
}

std::string renewed_cookie(const ContentResponse &reply, bool &deleted) {
	deleted = false;
	for (const auto &[key, value] : reply.headers) {
		if (!strutil::iequals(key, "Set-Cookie"))
			continue;
		bool gone = false;
		const std::string pair = session_pair(value, gone);
		if (gone)
			deleted = true;
		if (!pair.empty())
			return pair;
	}
	return {};
}

ContentRequest projects(const ContentAccount &account) {
	return request(account, "GET", "/api/content/projects");
}

bool read_projects(
		const ContentResponse &reply, std::vector<RemoteProject> &out, ContentApiError &error) {
	out.clear();
	io::JsonValue json;
	if (!json_reply(reply, 200, json, error))
		return false;
	const io::JsonValue *list = json.get("projects");
	if (!list || !list->is_array()) {
		error.code = "bad_reply";
		error.message = "the service's project list has no projects";
		return false;
	}
	for (const io::JsonValue &item : list->array)
		if (item.is_object())
			out.push_back(project_of(item));
	return true;
}

ContentRequest project(const ContentAccount &account, const std::string &key) {
	return request(account, "GET", project_path(key));
}

bool read_project(const ContentResponse &reply, RemoteProject &out,
		std::optional<RemoteRevision> &head, ContentApiError &error) {
	head.reset();
	io::JsonValue json;
	if (!json_reply(reply, 200, json, error))
		return false;
	const io::JsonValue *project = json.get("project");
	if (!project || !project->is_object()) {
		error.code = "bad_reply";
		error.message = "the service's reply names no project";
		return false;
	}
	out = project_of(*project);
	const io::JsonValue *revision = json.get("head_revision");
	if (revision && revision->is_object())
		head = revision_of(*revision);
	return true;
}

ContentRequest revisions(
		const ContentAccount &account, const std::string &key, int64_t before, int limit) {
	return request(account, "GET",
			project_path(key) + "/revisions?before=" + std::to_string(before) +
					"&limit=" + std::to_string(limit));
}

bool read_revisions(
		const ContentResponse &reply, std::vector<RemoteRevision> &out, ContentApiError &error) {
	out.clear();
	io::JsonValue json;
	if (!json_reply(reply, 200, json, error))
		return false;
	const io::JsonValue *list = json.get("revisions");
	if (!list || !list->is_array()) {
		error.code = "bad_reply";
		error.message = "the service's history has no revisions";
		return false;
	}
	for (const io::JsonValue &item : list->array)
		if (item.is_object())
			out.push_back(revision_of(item));
	return true;
}

ContentRequest manifest(const ContentAccount &account, const std::string &key, int64_t number) {
	return request(account, "GET",
			project_path(key) + "/revisions/" + std::to_string(number) + "/manifest");
}

bool read_manifest(const ContentResponse &reply, const std::string &manifest_sha256,
		std::vector<content::ContentFile> &out, ContentApiError &error) {
	out.clear();
	error = ContentApiError();
	if (!reply.transport_ok || reply.status != 200) {
		io::JsonValue ignored;
		json_reply(reply, 200, ignored, error);
		return false;
	}
	error.status = reply.status;
	if (!manifest_sha256.empty() && content::manifest_sha256(reply.body) != manifest_sha256) {
		error.code = "bad_manifest";
		error.message = "the manifest the service sent is not the revision's";
		return false;
	}
	std::string why;
	if (!content::decode_manifest(reply.body, out, why)) {
		error.code = "bad_manifest";
		error.message = "the manifest the service sent does not read: " + why;
		return false;
	}
	return true;
}

ContentRequest blob(const ContentAccount &account, const std::string &key,
		const std::string &sha256, const std::string &file) {
	ContentRequest r = request(account, "GET", project_path(key) + "/blobs/" + sha256);
	r.response_file = file;
	return r;
}

bool read_blob(const ContentResponse &reply, const content::ContentFile &expected,
		ContentApiError &error) {
	error = ContentApiError();
	if (!reply.transport_ok || reply.status != 200) {
		io::JsonValue ignored;
		json_reply(reply, 200, ignored, error);
		return false;
	}
	error.status = reply.status;
	if (reply.file_bytes != expected.size || reply.file_sha256 != expected.sha256) {
		error.code = "bad_download";
		error.message = "the download of " + expected.path + " is not its file";
		return false;
	}
	return true;
}

ContentRequest missing(const ContentAccount &account, const std::string &key,
		const std::vector<content::ContentFile> &files) {
	io::JsonValue blobs = io::JsonValue::make_array();
	for (const content::ContentFile &file : files) {
		io::JsonValue blob = io::JsonValue::make_object();
		blob.set("sha256", io::json_string(file.sha256));
		blob.set("size", io::json_number(static_cast<double>(file.size)));
		blobs.push(std::move(blob));
	}
	io::JsonValue body = io::JsonValue::make_object();
	body.set("blobs", std::move(blobs));
	return json_request(account, "POST", project_path(key) + "/missing", body);
}

bool read_missing(const ContentResponse &reply, const ContentAccount &account,
		std::vector<UploadTarget> &out, ContentApiError &error) {
	out.clear();
	io::JsonValue json;
	if (!json_reply(reply, 200, json, error))
		return false;
	const io::JsonValue *list = json.get("missing");
	if (!list || !list->is_array()) {
		error.code = "bad_reply";
		error.message = "the service's reply lists no missing files";
		return false;
	}
	for (const io::JsonValue &item : list->array) {
		const io::JsonValue *upload = item.get("upload");
		if (!item.is_object() || !upload || !upload->is_object())
			continue;
		UploadTarget target;
		target.file.sha256 = item.get_string("sha256", "");
		target.file.size = static_cast<uint64_t>(whole(item, "size"));
		target.request = transfer_request(account, *upload, "PUT");
		if (content::is_sha256_hex(target.file.sha256) && !target.request.url.empty())
			out.push_back(std::move(target));
	}
	return true;
}

ContentRequest downloads(const ContentAccount &account, const std::string &key,
		const std::vector<std::string> &sha256s) {
	io::JsonValue body = io::JsonValue::make_object();
	body.set("hashes", io::json_string_array(sha256s));
	return json_request(account, "POST", project_path(key) + "/downloads", body);
}

bool read_downloads(const ContentResponse &reply, const ContentAccount &account,
		std::map<std::string, ContentRequest> &out, ContentApiError &error) {
	io::JsonValue json;
	if (!json_reply(reply, 200, json, error))
		return false;
	const io::JsonValue *list = json.get("downloads");
	if (!list || !list->is_array()) {
		error.code = "bad_reply";
		error.message = "the service's reply lists no downloads";
		return false;
	}
	for (const io::JsonValue &item : list->array) {
		if (!item.is_object())
			continue;
		const std::string sha = item.get_string("sha256", "");
		ContentRequest request = transfer_request(account, item, "GET");
		if (content::is_sha256_hex(sha) && !request.url.empty())
			out[sha] = std::move(request);
	}
	return true;
}

ContentRequest upload(const ContentAccount &, const UploadTarget &target, const std::string &file) {
	ContentRequest r = target.request;
	r.body_file = file;
	return r;
}

bool read_upload(const ContentResponse &reply, ContentApiError &error) {
	error = ContentApiError();
	if (reply.transport_ok && reply.status >= 200 && reply.status < 300)
		return true;
	io::JsonValue ignored;
	json_reply(reply, 200, ignored, error);
	return false;
}

ContentRequest push(const ContentAccount &account, const std::string &key, int64_t parent,
		const std::string &message, const std::string &manifest_text) {
	io::JsonValue body = io::JsonValue::make_object();
	body.set("parent", io::json_number(static_cast<double>(parent)));
	body.set("message", io::json_string(message));
	body.set("manifest", io::json_string(manifest_text));
	return json_request(account, "POST", project_path(key) + "/revisions", body);
}

PushAnswer read_push(const ContentResponse &reply, int64_t &head, std::vector<std::string> &hashes,
		ContentApiError &error) {
	hashes.clear();
	io::JsonValue json;
	if (json_reply(reply, 201, json, error)) {
		head = whole(json, "head");
		return PushAnswer::Pushed;
	}
	if (!reply.transport_ok)
		return PushAnswer::Refused;
	std::string ignored;
	io::JsonValue body;
	if (!io::json_parse(reply.body, body, ignored) || !body.is_object())
		return PushAnswer::Refused;
	if (reply.status == 409 && error.code == "stale") {
		head = whole(body, "head");
		return PushAnswer::Stale;
	}
	if (reply.status == 400 && error.code == "missing_blobs") {
		if (const io::JsonValue *list = body.get("hashes"); list && list->is_array())
			for (const io::JsonValue &sha : list->array)
				if (sha.is_string())
					hashes.push_back(sha.string);
		return PushAnswer::MissingBlobs;
	}
	return PushAnswer::Refused;
}

ContentRequest release(const ContentAccount &account, const std::string &key,
		const std::string &version, int64_t revision) {
	io::JsonValue body = io::JsonValue::make_object();
	body.set("version", io::json_string(version));
	body.set("revision", io::json_number(static_cast<double>(revision)));
	return json_request(account, "POST", "/api/admin/content/projects/" + key + "/releases", body);
}

bool read_release(const ContentResponse &reply, int64_t &id, std::string &name, ContentApiError &error) {
	io::JsonValue json;
	if (!json_reply(reply, 201, json, error))
		return false;
	const io::JsonValue *release = json.get("release");
	if (!release || !release->is_object()) {
		error.code = "bad_reply";
		error.message = "the service's reply names no release";
		return false;
	}
	id = whole(*release, "id");
	name = release->get_string("name", "");
	return true;
}

} // namespace content_api

} // namespace opennova::editor
