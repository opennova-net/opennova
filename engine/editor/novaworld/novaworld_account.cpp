#include <editor/novaworld/novaworld_account.h>

#include <filesystem>
#include <system_error>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

constexpr int kAccountSchemaVersion = 1;

} // namespace

std::string novaworld_server(const std::string &server) {
	size_t start = 0, end = server.size();
	while (start < end && (server[start] == ' ' || server[start] == '\t'))
		++start;
	while (end > start &&
			(server[end - 1] == ' ' || server[end - 1] == '\t' || server[end - 1] == '/'))
		--end;
	std::string out = server.substr(start, end - start);
	if (!out.empty() && out.find("://") == std::string::npos)
		out = "https://" + out;
	return out;
}

NovaWorldAccount FileNovaWorldAccountStore::load() {
	NovaWorldAccount account;
	account.server = default_server_;
	std::string text, error;
	std::error_code ec;
	io::JsonValue json;
	if (!fs::is_regular_file(system_path(path_), ec) || !io::read_file_text(path_, text, error) ||
			!io::json_parse(text, json, error) || !json.is_object() ||
			json.get_number("schema_version", 0) != kAccountSchemaVersion)
		return account;
	const std::string kept = novaworld_server(json.get_string("server", ""));
	if (!kept.empty())
		account.server = kept;
	account.username = json.get_string("username", "");
	account.role = json.get_string("role", "");
	account.cookie = json.get_string("cookie", "");
	return account;
}

bool FileNovaWorldAccountStore::save(const NovaWorldAccount &account, std::string &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::json_number(kAccountSchemaVersion));
	json.set("server", io::json_string(account.server));
	json.set("username", io::json_string(account.username));
	json.set("role", io::json_string(account.role));
	json.set("cookie", io::json_string(account.cookie));
	std::error_code ec;
	fs::create_directories(system_path(path_).parent_path(), ec);
	return io::write_file_atomic(path_, io::json_write(json), error);
}

} // namespace opennova::editor
