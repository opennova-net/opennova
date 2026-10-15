#pragma once

// CD keys: the keys the service issues for a title whose
// client activates against NovaWorld, the accounts they are on, and the
// machines each one activated. Only Delta Force Xtreme 2 activates: its boot
// and its menus gate on a registry CDKIID that NovaLogic's NovaWorld handed out
// as the NWACT browser variable, a value bound to the machine's NWPSSK
// fingerprint, and its LAN host checks the CDKIID that value decodes to
// (docs/titles/dfx2-vs-jo-re.md section 9, DFX2-13; the codec is
// engine/net/novacrypto/cdkey.h). Joint Operations validates no key, so it has
// none here, and its NovaWorld login keeps taking an empty NWCDKIID.
//
// A key is its serial, the plaintext it carries: "<TITLE>-XXXX-XXXX-XXXX-XXXX",
// the title's code in upper case and 64 bits from the OS CSPRNG as sixteen
// upper-case hex digits ("DFX2-9F3A-6C21-B04E-7D58"). The format is the
// service's own (NovaLogic's issuer is unwitnessed); every serial is unique,
// so no two accounts' wire keys meet at a LAN host's codes 12 and 13. Its wire
// key, the CDKIID a client keeps, nests the serial under the title's wire chain
// (cdkey::make_wire_key), so an activated copy can host and join on LAN. No
// "/MP" tail is minted: in the nested form the value the boot read tests for
// one is the A-P wire key, which never ends "/MP", and the LAN host never reads
// the serial. No CDKIIDEXP1 is issued: nothing in dfx2.exe writes one, so a
// stock DFX2 always sends it empty.
//
// An account holds at most one live (unrevoked) key per title. Its activation
// is one row per machine: the NWACT is cdkey::make_activation(wire key,
// NWPSSK), the same every time that machine asks. A revoked key activates no
// machine, and an account whose keys were all revoked gets no new one on its
// own. Crow-free; each function throws db::Error on a database failure.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::cdkeys {

// The titles keys are issued for: those whose client activates (games.title).
bool title_activates(std::string_view title);

// A fresh serial for `title` in the format above (the OS CSPRNG).
std::string make_serial(std::string_view title);

struct KeyAccount {
	int64_t id = 0;
	std::string username;
};

struct CdKey {
	int64_t id = 0;
	std::string title;
	std::string serial;
	std::string wire_key;
	std::optional<KeyAccount> account; // empty: not assigned
	std::string created_by; // "admin <username>", "token" or "auto"
	// UTC 'YYYY-MM-DD HH:MM:SS'; assigned_at and revoked_at empty until set.
	std::string created_at;
	std::string assigned_at;
	std::string revoked_at;
	int64_t activations = 0;

	bool revoked() const { return !revoked_at.empty(); }
	// "revoked", "assigned" or "unassigned".
	const char *state() const;
};

struct Activation {
	int64_t id = 0;
	int64_t key_id = 0;
	std::string title;
	std::string serial;
	std::string nwpssk;
	std::string nwact;
	std::string created_at;
};

// `count` new unassigned keys for `title` (title_activates), in one
// transaction, each recorded as made by `created_by`.
std::vector<CdKey> generate_keys(
		db::Database &db, std::string_view title, size_t count, const std::string &created_by);

struct KeyFilter {
	std::string title; // empty: every title
	std::string state; // "", "unassigned", "assigned" or "revoked"
	std::string search; // a part of the serial, the wire key or the account's username
	size_t limit = 100;
	size_t offset = 0;
};
// The keys the filter selects, newest first, and how many it selects in all.
std::vector<CdKey> list_keys(db::Database &db, const KeyFilter &filter, int64_t *total = nullptr);
std::optional<CdKey> find_key(db::Database &db, int64_t id);
// An account's keys, newest first, revoked ones included.
std::vector<CdKey> keys_of_account(db::Database &db, int64_t user_id);

enum class AssignOutcome : uint8_t {
	Assigned,
	NoSuchKey,
	NoSuchAccount, // no player account by that id
	Revoked,
	AlreadyAssigned, // the key is on an account (this one or another)
	AccountHasKey, // the account already holds a live key for the title
};
AssignOutcome assign_key(db::Database &db, int64_t key_id, int64_t user_id);

enum class RevokeOutcome : uint8_t { Revoked, NoSuchKey, AlreadyRevoked };
RevokeOutcome revoke_key(db::Database &db, int64_t key_id);

// A key's activations, oldest first.
std::vector<Activation> activations_of_key(db::Database &db, int64_t key_id);
// Every activation of an account's keys, newest first.
std::vector<Activation> activations_of_account(db::Database &db, int64_t user_id);

enum class ActivateOutcome : uint8_t {
	Activated, // a new machine
	Reactivated, // a machine the key already activated: the same NWACT
	NoSuchKey,
	Revoked,
	BadFingerprint, // not an NWPSSK (cdkey::is_fingerprint)
	NoKey, // the account holds no key for the title (and none was minted)
	TitleHasNoKeys, // the title does not activate
};
struct ActivateResult {
	ActivateOutcome outcome = ActivateOutcome::NoSuchKey;
	std::optional<CdKey> key;
	std::string nwact;
	bool minted = false; // the key was minted for this activation
};

// The machine `nwpssk` names activated under key `key_id` (an admin's manual
// activation): its NWACT, recorded once per machine.
ActivateResult activate_key(db::Database &db, int64_t key_id, const std::string &nwpssk);

// An account's activation of the machine `nwpssk` names for `title`, what a
// retail login hands out: under the account's live key for the title, or,
// when the account has never held one and `auto_mint` is set, a key minted
// onto it now ("auto"). An account whose keys were all revoked gets Revoked.
ActivateResult activate_account(db::Database &db, int64_t user_id, std::string_view title,
		const std::string &nwpssk, bool auto_mint);

// Whether a client of `title` that posts `nwcdkiid` (the NWCDKIID cookie, the
// wire key its boot read decoded) still needs activating: the title activates
// and the value is no wire key a LAN host would decode (empty, for a copy
// never activated).
bool needs_activation(std::string_view title, std::string_view nwcdkiid);

} // namespace opennova::novaworld_server::cdkeys
