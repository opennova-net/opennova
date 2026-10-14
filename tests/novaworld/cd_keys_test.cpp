// The service's CD keys below the HTTP layer (apps/novaworld_server/cd_keys.h,
// migration 0015), over a migrated in-memory database: the serial format, the
// keys an admin generates, lists, assigns and revokes, and the activations, an
// account's (the retail login's: under its live key, the first key minted when
// minting is on, none once its keys are revoked) and a key's (an admin's manual
// one), each NWACT read back to the key's wire key by the client's own leg
// (net/novacrypto/cdkey.h), the same NWACT for a machine that asks again. Runs on
// every build (no Crow).

#include "cd_keys.h"

#include <net/novacrypto/cdkey.h>
#include <net/novaworld/db/sqlite.h>

#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace cdkeys = opennova::novaworld_server::cdkeys;
namespace cdkey = opennova::cdkey;
using opennova::db::BindValue;
using opennova::db::Database;

namespace {

constexpr const char *kMachineA = "ABCDEFGHIJKLMNOPQRSTUVW";
constexpr const char *kMachineB = "TTTTTTTTTTTTTTTTTTTTTTT";

int64_t add_player(Database &db, const std::string &name, const char *kind = "player") {
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle, kind) VALUES (?, "
			"'x', ?, '1', ?, ?);",
			{ BindValue(name), BindValue("pc-" + name), BindValue(name),
					BindValue(std::string(kind)) });
	return db.last_insert_rowid();
}

bool is_upper_hex(char c) {
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

// The NWACT, read on `machine` the way the client reads it, is the key's wire key.
bool activates(const std::string &nwact, const char *machine, const std::string &wire_key) {
	std::string wire;
	return cdkey::read_activation(nwact, machine, wire) && wire == wire_key;
}

int serial_format() {
	std::set<std::string> seen;
	for (int i = 0; i < 64; ++i) {
		const std::string serial = cdkeys::make_serial("dfx2");
		TEST_EXPECT(serial.size() == 24 && serial.compare(0, 4, "DFX2") == 0);
		for (size_t at = 4; at < serial.size(); ++at) {
			if (at % 5 == 4)
				TEST_EXPECT(serial[at] == '-');
			else
				TEST_EXPECT(is_upper_hex(serial[at]));
		}
		seen.insert(serial);
	}
	TEST_EXPECT(seen.size() == 64);
	TEST_EXPECT(cdkeys::title_activates("dfx2"));
	TEST_EXPECT(!cdkeys::title_activates("jo") && !cdkeys::title_activates("") &&
			!cdkeys::title_activates("DFX2"));
	return 0;
}

int needs_activation() {
	const std::string wire = cdkey::make_wire_key("DFX2-0000-0000-0000-0001");
	TEST_EXPECT(cdkeys::needs_activation("dfx2", ""));
	TEST_EXPECT(cdkeys::needs_activation("dfx2", "JUNK"));
	TEST_EXPECT(!cdkeys::needs_activation("dfx2", wire));
	// Joint Operations activates nothing, whatever its NWCDKIID holds.
	TEST_EXPECT(!cdkeys::needs_activation("jo", ""));
	TEST_EXPECT(!cdkeys::needs_activation("jo", wire));
	return 0;
}

int keys_lifecycle(Database &db) {
	const int64_t owner = add_player(db, "owner");
	const int64_t other = add_player(db, "other");
	const int64_t service = add_player(db, "srv-1", "service");

	const auto made = cdkeys::generate_keys(db, "dfx2", 3, "admin root");
	TEST_EXPECT(made.size() == 3);
	for (const auto &k : made) {
		TEST_EXPECT(k.title == "dfx2" && k.wire_key == cdkey::make_wire_key(k.serial));
		TEST_EXPECT(std::string(k.state()) == "unassigned" && !k.account &&
				k.created_by == "admin root");
		TEST_EXPECT(!k.created_at.empty() && k.assigned_at.empty() && k.revoked_at.empty() &&
				k.activations == 0);
	}

	int64_t total = 0;
	cdkeys::KeyFilter filter;
	filter.limit = 2;
	auto listed = cdkeys::list_keys(db, filter, &total);
	TEST_EXPECT(total == 3 && listed.size() == 2 && listed[0].id == made[2].id &&
			listed[1].id == made[1].id);
	filter.offset = 2;
	listed = cdkeys::list_keys(db, filter, &total);
	TEST_EXPECT(total == 3 && listed.size() == 1 && listed[0].id == made[0].id);

	using A = cdkeys::AssignOutcome;
	TEST_EXPECT(cdkeys::assign_key(db, made[0].id, owner) == A::Assigned);
	TEST_EXPECT(cdkeys::assign_key(db, made[0].id, other) == A::AlreadyAssigned);
	TEST_EXPECT(cdkeys::assign_key(db, made[1].id, owner) == A::AccountHasKey);
	TEST_EXPECT(cdkeys::assign_key(db, made[1].id, service) == A::NoSuchAccount);
	TEST_EXPECT(cdkeys::assign_key(db, made[1].id, 999999) == A::NoSuchAccount);
	TEST_EXPECT(cdkeys::assign_key(db, 999999, other) == A::NoSuchKey);
	const auto on_owner = cdkeys::find_key(db, made[0].id);
	TEST_EXPECT(on_owner && on_owner->account && on_owner->account->id == owner &&
			on_owner->account->username == "owner");
	TEST_EXPECT(std::string(on_owner->state()) == "assigned" && !on_owner->assigned_at.empty());

	cdkeys::KeyFilter by_state;
	by_state.state = "unassigned";
	TEST_EXPECT(cdkeys::list_keys(db, by_state, &total).size() == 2 && total == 2);
	by_state.state = "assigned";
	TEST_EXPECT(cdkeys::list_keys(db, by_state, &total).size() == 1 && total == 1);
	cdkeys::KeyFilter by_search;
	by_search.search = "OWN"; // the account's username, case-insensitively
	listed = cdkeys::list_keys(db, by_search, &total);
	TEST_EXPECT(total == 1 && listed[0].id == made[0].id);
	by_search.search = made[1].serial.substr(5, 9);
	listed = cdkeys::list_keys(db, by_search, &total);
	TEST_EXPECT(total == 1 && listed[0].id == made[1].id);
	by_search.search = made[2].wire_key.substr(10, 12);
	TEST_EXPECT(cdkeys::list_keys(db, by_search, &total).size() >= 1);

	using R = cdkeys::RevokeOutcome;
	TEST_EXPECT(cdkeys::revoke_key(db, made[2].id) == R::Revoked);
	TEST_EXPECT(cdkeys::revoke_key(db, made[2].id) == R::AlreadyRevoked);
	TEST_EXPECT(cdkeys::revoke_key(db, 999999) == R::NoSuchKey);
	TEST_EXPECT(cdkeys::assign_key(db, made[2].id, other) == A::Revoked);
	by_state.state = "revoked";
	TEST_EXPECT(cdkeys::list_keys(db, by_state, &total).size() == 1 && total == 1);
	return 0;
}

int activations(Database &db) {
	using O = cdkeys::ActivateOutcome;
	const int64_t first = add_player(db, "first_activation");
	TEST_EXPECT(cdkeys::activate_account(db, first, "jo", kMachineA, true).outcome ==
			O::TitleHasNoKeys);
	TEST_EXPECT(cdkeys::activate_account(db, first, "dfx2", "", true).outcome == O::BadFingerprint);
	TEST_EXPECT(
			cdkeys::activate_account(db, first, "dfx2", "abcdefghijklmnopqrstuvw", true).outcome ==
			O::BadFingerprint);
	TEST_EXPECT(cdkeys::keys_of_account(db, first).empty());
	// Minting off: no key, nothing written.
	TEST_EXPECT(cdkeys::activate_account(db, first, "dfx2", kMachineA, false).outcome == O::NoKey);
	TEST_EXPECT(cdkeys::keys_of_account(db, first).empty());

	// Minting on: the account's first activation mints its key.
	auto result = cdkeys::activate_account(db, first, "dfx2", kMachineA, true);
	TEST_EXPECT(result.outcome == O::Activated && result.minted && result.key);
	TEST_EXPECT(result.key->created_by == "auto" && result.key->account &&
			result.key->account->id == first);
	TEST_EXPECT(!result.key->assigned_at.empty() && result.key->activations == 1);
	TEST_EXPECT(activates(result.nwact, kMachineA, result.key->wire_key));
	const std::string nwact_a = result.nwact;
	const int64_t key_id = result.key->id;
	// Again: the same NWACT, the same key, no new row.
	result = cdkeys::activate_account(db, first, "dfx2", kMachineA, true);
	TEST_EXPECT(result.outcome == O::Reactivated && !result.minted && result.nwact == nwact_a &&
			result.key->id == key_id);
	// Another machine: its own NWACT under the same key.
	result = cdkeys::activate_account(db, first, "dfx2", kMachineB, false);
	TEST_EXPECT(result.outcome == O::Activated && result.nwact != nwact_a &&
			activates(result.nwact, kMachineB, result.key->wire_key));
	const auto history = cdkeys::activations_of_account(db, first);
	TEST_EXPECT(history.size() == 2 && history[0].nwpssk == kMachineB &&
			history[1].nwpssk == kMachineA);
	TEST_EXPECT(history[1].nwact == nwact_a && history[1].key_id == key_id &&
			history[1].title == "dfx2");
	TEST_EXPECT(cdkeys::activations_of_key(db, key_id).size() == 2);
	TEST_EXPECT(cdkeys::keys_of_account(db, first).size() == 1);

	// A manual activation under the key: the machine's NWACT, recorded once.
	const std::string third = "QQQQQQQQQQQQQQQQQQQQQQQ";
	result = cdkeys::activate_key(db, key_id, third);
	TEST_EXPECT(result.outcome == O::Activated &&
			activates(result.nwact, third.c_str(), result.key->wire_key));
	TEST_EXPECT(cdkeys::activate_key(db, key_id, third).outcome == O::Reactivated);
	TEST_EXPECT(cdkeys::activate_key(db, key_id, "nope").outcome == O::BadFingerprint);
	TEST_EXPECT(cdkeys::activate_key(db, 999999, third).outcome == O::NoSuchKey);
	TEST_EXPECT(cdkeys::activations_of_key(db, key_id).size() == 3);

	// Revoked: neither the account's activation nor a manual one, and no key minted in its place.
	TEST_EXPECT(cdkeys::revoke_key(db, key_id) == cdkeys::RevokeOutcome::Revoked);
	result = cdkeys::activate_account(db, first, "dfx2", kMachineA, true);
	TEST_EXPECT(result.outcome == O::Revoked && result.nwact.empty() && !result.minted);
	TEST_EXPECT(cdkeys::activate_key(db, key_id, kMachineA).outcome == O::Revoked);
	TEST_EXPECT(cdkeys::keys_of_account(db, first).size() == 1);
	// An admin's new key for the account activates again.
	const auto fresh = cdkeys::generate_keys(db, "dfx2", 1, "token");
	TEST_EXPECT(cdkeys::assign_key(db, fresh[0].id, first) == cdkeys::AssignOutcome::Assigned);
	result = cdkeys::activate_account(db, first, "dfx2", kMachineA, false);
	TEST_EXPECT(result.outcome == O::Activated && result.key->id == fresh[0].id);
	TEST_EXPECT(activates(result.nwact, kMachineA, fresh[0].wire_key) && result.nwact != nwact_a);

	// A deleted account takes its keys and their activations.
	db.exec("DELETE FROM players WHERE id = ?;", { BindValue(first) });
	TEST_EXPECT(!cdkeys::find_key(db, key_id) && !cdkeys::find_key(db, fresh[0].id));
	TEST_EXPECT(cdkeys::activations_of_key(db, key_id).empty());
	return 0;
}

} // namespace

int main() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	Database db(":memory:");
	opennova::db::run_migrations(db, source_dir / "backend" / "migrations");
	int rc = 0;
	rc |= serial_format();
	rc |= needs_activation();
	rc |= keys_lifecycle(db);
	rc |= activations(db);
	if (rc == 0)
		std::printf("OK: cd keys\n");
	return rc;
}
