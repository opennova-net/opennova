// The synthetic ban lists behind fixtures/banlist/: the models the generator writes through
// our own writers and the roundtrip test reads back. Invented PCIDs, names and addresses.
#pragma once

#include <formats/banlist/address_ban_list.h>
#include <formats/banlist/pcid_ban_list.h>

namespace banlist_synth {

inline opennova::banlist::PcidBanList pcids() {
	opennova::banlist::PcidBanList list;
	list.entries.push_back({"A-SY-NTH001", "Spammer"});
	list.entries.push_back({"A-SY-NTH002", ""});
	list.entries.push_back({"A-SY-NTH003", "Two Words"});
	list.entries.push_back({"A-SY-NTH001", "Again"}); // the list keeps duplicates
	return list;
}

inline opennova::banlist::AddressBanList addresses() {
	opennova::banlist::AddressBanList list;
	list.entries.push_back({10u | (0u << 8) | (0u << 16) | (7u << 24), "?"});
	list.entries.push_back({192u | (168u << 8) | (1u << 16) | (200u << 24), "Griefer"});
	list.entries.push_back({255u | (255u << 8) | (255u << 16) | (255u << 24), "FifteenCharName"});
	return list;
}

} // namespace banlist_synth
