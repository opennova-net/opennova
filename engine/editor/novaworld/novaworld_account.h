#pragma once

// The account NovaWorld's client acts as (ADR 0069): the service's address, and the
// website session a sign-in got there (its cookie, as the service names it: `opennova_session`, or
// `__Host-opennova_session` on a secure deployment, apps/novaworld_common/accounts/web_session.h),
// with the account's name and role. Signed out, it is the address alone, which reads the public
// projects. Kept by the Shell in the editor's own user folder (`user://content_account.json`, a
// debug build's `content_account_staging.json`), never in a project, through the store seam here:
// a file, or memory for the tests and the command line.

#include <string>
#include <utility>

namespace opennova::editor {

struct NovaWorldAccount {
	std::string server; // "https://opennova.net", no trailing slash
	std::string username;
	std::string role; // "admin" or "player"
	std::string cookie; // "<name>=<value>", "" when signed out

	bool signed_in() const { return !cookie.empty(); }
	bool admin() const { return signed_in() && role == "admin"; }
};

// `server` as the client keeps it: trimmed of spaces and a trailing '/', with https:// before a
// bare host ("opennova.net" is "https://opennova.net").
std::string novaworld_server(const std::string &server);

class NovaWorldAccountStore {
public:
	virtual ~NovaWorldAccountStore() = default;
	// The kept account; one with no server when none is kept.
	virtual NovaWorldAccount load() = 0;
	virtual bool save(const NovaWorldAccount &account, std::string &error) = 0;
};

class MemoryNovaWorldAccountStore : public NovaWorldAccountStore {
public:
	NovaWorldAccount load() override { return account_; }
	bool save(const NovaWorldAccount &account, std::string &) override {
		account_ = account;
		return true;
	}

private:
	NovaWorldAccount account_;
};

// The account in a JSON file of its own (the Shell's `user://content_account.json`), written
// atomically; a file that is missing or unreadable is no account. With no account kept, it is the
// address `default_server` alone: the service the build reaches (the Shell's: a debug build
// staging's, a release build the live one's).
class FileNovaWorldAccountStore : public NovaWorldAccountStore {
public:
	FileNovaWorldAccountStore(std::string path, std::string default_server) :
			path_(std::move(path)), default_server_(novaworld_server(default_server)) {}
	NovaWorldAccount load() override;
	bool save(const NovaWorldAccount &account, std::string &error) override;

private:
	std::string path_;
	std::string default_server_;
};

} // namespace opennova::editor
