// The ring heads of the loaded first-person weapon tables (adm_ring_table.h).

#include <runtime/anim/adm_ring_table.h>

#include <formats/adm/adm.h>
#include <base/io/strutil.h>

namespace opennova::anim {

namespace {

std::string table_key(const std::string &adm_name) {
	std::string name = strutil::to_lower(adm_name);
	if (!strutil::ends_with_icase(name, ".adm")) name += ".adm";
	return name;
}

const char *const kResetKey = "anim_reset";

} // namespace

bool AdmRingTable::load(const assets::AssetStore *assets, const std::string &adm_name) {
	if (adm_name.empty()) return false;
	const std::string name = table_key(adm_name);
	if (tables_.count(name) != 0) return true;
	AdmClipIndex index;
	index.load(assets, adm_name);
	// A table whose reset clip does not load does not load [orig:
	// AnimMap_LoadAdmFile @0x40cc40, the slot-0 head read @0x40ce11..0x40ce16].
	const std::vector<AdmClipFacts> *reset = index.clips_for(kResetKey);
	if (reset == nullptr || reset->empty()) return false;
	tables_[name].rings = index.all_clips();
	return true;
}

bool AdmRingTable::loaded(const std::string &adm_name) const { return table(adm_name) != nullptr; }

AdmRingTable AdmRingTable::copy_of(const std::string &adm_name) const {
	AdmRingTable copy;
	if (const Table *t = table(adm_name)) copy.tables_.emplace(table_key(adm_name), *t);
	return copy;
}

void AdmRingTable::adopt(const std::string &adm_name,
		const std::unordered_map<std::string, std::vector<AdmClipFacts>> &clips) {
	if (adm_name.empty()) return;
	const std::string name = table_key(adm_name);
	if (tables_.count(name) != 0) return;
	Table table;
	for (const auto &kv : clips) {
		if (kv.second.empty() || adm_slot_index(kv.first) < 0) continue;
		table.rings[adm::adm_slot_key(kv.first)] = kv.second;
	}
	if (!table.rings.empty()) tables_.emplace(name, std::move(table));
}

bool AdmRingTable::resolves(const std::string &adm_name, const std::string &key) const {
	return table(adm_name) != nullptr && adm_slot_index(key) >= 0;
}

AdmServed AdmRingTable::peek(const std::string &adm_name, const std::string &key) const {
	AdmServed out;
	const int slot = adm_slot_index(key);
	const Table *table = this->table(adm_name);
	if (table == nullptr || slot < 0) return out;
	const std::string slot_key = adm::adm_slot_key(key);
	auto ring = table->rings.find(slot_key);
	if (slot == 0 || ring == table->rings.end() || ring->second.empty()) {
		// Slot 0 serves the last reset token that loaded and never moves; a slot
		// the table does not author holds the FIRST reset token that loaded, a
		// self-ring too. [orig: AnimMap_RegisterBoneNode @0x40C38B..0x40C38F; the
		// backfill @0x40C39A..0x40C3E2]
		auto reset = table->rings.find(kResetKey);
		if (reset == table->rings.end() || reset->second.empty()) return out;
		out.key = kResetKey;
		out.variant = slot == 0 ? static_cast<int32_t>(reset->second.size()) - 1 : 0;
		out.clip = &reset->second[static_cast<size_t>(out.variant)];
		return out;
	}
	// Serve n takes the variant n steps back from the last: the head starts on
	// the row's last token and each serve moves it to the one before.
	// [orig: table = node @0x40C385; the serve @0x40BDB4]
	const int64_t count = static_cast<int64_t>(ring->second.size());
	auto served = table->served.find(slot_key);
	const int64_t n = served != table->served.end() ? served->second : 0;
	out.key = slot_key;
	out.variant = static_cast<int32_t>(count - 1 - n % count);
	out.clip = &ring->second[static_cast<size_t>(out.variant)];
	return out;
}

AdmServed AdmRingTable::serve(const std::string &adm_name, const std::string &key) {
	const AdmServed out = peek(adm_name, key);
	// Only a ring the table authors advances [orig: the advance @0x40BDC1].
	if (out.valid() && out.key != kResetKey) {
		Table &table = tables_[table_key(adm_name)];
		const int64_t count = static_cast<int64_t>(table.rings[out.key].size());
		int64_t &served = table.served[out.key];
		served = (served + 1) % count;
	}
	return out;
}

std::vector<std::string> AdmRingTable::keys(const std::string &adm_name) const {
	std::vector<std::string> out;
	if (const Table *t = table(adm_name))
		for (const auto &kv : t->rings) out.push_back(kv.first);
	return out;
}

const AdmClipFacts *AdmRingTable::clip(const std::string &adm_name, const std::string &key,
		int32_t variant) const {
	const Table *t = table(adm_name);
	if (t == nullptr || variant < 0) return nullptr;
	auto ring = t->rings.find(adm::adm_slot_key(key));
	if (ring == t->rings.end() || static_cast<size_t>(variant) >= ring->second.size()) return nullptr;
	return &ring->second[static_cast<size_t>(variant)];
}

void AdmRingTable::clear() { tables_.clear(); }

const AdmRingTable::Table *AdmRingTable::table(const std::string &adm_name) const {
	auto found = tables_.find(table_key(adm_name));
	return found != tables_.end() ? &found->second : nullptr;
}

} // namespace opennova::anim
