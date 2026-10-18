#include <runtime/inmatch/integrity_weapon_image.h>

#include <base/io/crc32_mpeg2.h>
#include <base/io/crt_ftol.h>
#include <base/io/le.h>
#include <base/io/strutil.h>
#include <formats/def/def_scan.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/anim/adm_ring_table.h>
#include <runtime/inmatch/integrity_ammo_image.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/world/weapon_fsm.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

namespace opennova::inmatch {

namespace {

constexpr size_t kTableBytes = static_cast<size_t>(kWeaponRecordCapacity) * kWeaponRecordSize;
constexpr int kActionCount = world::weapon_action::kCount;

// Retail's tokens[k] (k >= 1) is the tokenizer's slot k - 1 (a slot no line
// has set reads as the empty string here; retail would read a null pointer).
const char *token(const io::ConfigTokens &tokens, int k) {
	return tokens.token(k - 1);
}

bool key_is(const io::ConfigTokens &tokens, const char *key) {
	return strutil::iequals(token(tokens, 1), key);
}

int32_t atol_token(const io::ConfigTokens &tokens, int k) {
	return io::retail_atol(token(tokens, k));
}

// [orig: Math_ParseFixedPoint16 @0x6131F0] (defscan's structural translation)
int32_t fixed16_token(const io::ConfigTokens &tokens, int k) {
	const char *s = token(tokens, k);
	return defscan::parse_fixed16_digits_n(s, std::strlen(s));
}

int32_t abs32(int32_t v) {
	return v < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(v)) : v;
}

int32_t mul32(int32_t a, int32_t b) {
	return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

// The builtin ammo classes, ids 1..10, ahead of the weapon.def ones from 11
// [orig: the 65-byte name table aClassMana @0x830F10, ids at +64; reset by
//  AnimDef_InitAll @0x543659 (count 10) / @0x543660 (next id 11)].
const char *const kBuiltinAmmoClasses[] = { "CLASS_MANA", "CLASS_HP", "CLASS_POWER1",
	"CLASS_POWER2", "CLASS_POWER3", "CLASS_POWER4", "CLASS_POWER5", "CLASS_POWER6", "CLASS_POWER7",
	"CLASS_POWER8" };
constexpr uint32_t kAmmoClassLimit = 0x80;

// `charfilter` / `classrounds` / `teamfilter` [orig: g_WeaponCharFilterTable, g_WeaponClassRoundsTable,
// g_WeaponTeamFilterTable].
struct NamedValue {
	const char *name;
	int32_t value;
};
constexpr NamedValue kCharFilter[] = { { "medic", 0x01 }, { "sniper", 0x02 }, { "gunner", 0x04 },
	{ "rifleman", 0x08 }, { "engineer", 0x10 } };
constexpr NamedValue kClassRounds[] = { { "medic", 1 }, { "sniper", 2 }, { "gunner", 3 },
	{ "rifleman", 5 }, { "engineer", 6 } };
constexpr NamedValue kTeamFilter[] = { { "red", 0x01 }, { "blue", 0x02 } };

const NamedValue *find_named(const NamedValue *table, size_t count, const char *name) {
	for (size_t i = 0; i < count; ++i)
		if (strutil::iequals(name, table[i].name))
			return &table[i];
	return nullptr;
}

// One ActionDef pool entry, the fields the bind and the post-parse sum read:
// its name (+122), its anim (+58) and the two delays (+36, +40), each -1 for
// `auto` until the bind resolves it [orig: the 204-byte pool @0x85C620;
// ActionDef_InitDefaults @0x4022B0 zeroes the whole entry].
struct ActionDef {
	std::string name;
	std::string anim;
	int32_t delay_start = 0;
	int32_t delay_end = 0;
};

// The bind's duration read: the ring head of the slot the key names, advanced,
// then trunc(frames * 62.5 / fps + 0.5) + 1 in single precision (the x87
// precision the device leaves); slot 0 and a clip that did not load read 0.
// [orig: Anim_GetDurationTicks @0x53EE10 — the head advance @0x53EE20..0x53EE26,
//  the slot-0 and no-clip arms @0x53EE29..0x53EE2F, fild +0xC (frames) / fmul
//  62.5f / fild +8 (fps) / fdivp / fadd 0.5f / _ftol2_sse / +1 @0x53EE37..0x53EE67]
int32_t duration_ticks(anim::AdmRingTable &rings, const std::string &adm, const std::string &key) {
	const anim::AdmServed served = rings.serve(adm, key);
	if (anim::adm_slot_index(key) == 0 || !served.valid() || served.clip == nullptr)
		return 0;
	float ticks = static_cast<float>(served.clip->frames) * 62.5f;
	ticks = ticks / static_cast<float>(served.clip->fps);
	ticks = ticks + 0.5f;
	return static_cast<int32_t>(static_cast<uint32_t>(io::retail_ftol_sse2(ticks)) + 1u);
}

class WeaponParse {
public:
	WeaponParse(WeaponRecordTable &table, const AmmoRecordTable &ammo,
			const mission::BootFileSource &files, const assets::AssetStore *assets,
			io::ConfigTokens &tokens) :
			table_(table), ammo_(ammo), files_(files), assets_(assets), tokens_(tokens) {
		for (auto &actions : slot_actions_)
			actions.fill(-1);
		anim_object_.fill(false);
	}

	// [orig: AnimDef_InitAll @0x5435C0]
	void init_all() {
		table_.bytes.assign(kTableBytes, 0);
		for (int32_t slot = 0; slot < kWeaponRecordCapacity; ++slot)
			init_entry_defaults(slot);
		// The first free slot is named "null" and bound [orig: @0x543615, @0x54364F].
		std::memcpy(row(0) + 20, "null", 5);
		reset_parse_state();
		anim_init_actions(0);
		class_names_.assign(std::begin(kBuiltinAmmoClasses), std::end(kBuiltinAmmoClasses));
		class_ids_.clear();
		for (uint8_t id = 1; id <= 10; ++id)
			class_ids_.push_back(id);
		next_class_id_ = 11;
	}

	// One weapon.def line; true ends the file walk.
	// [orig: WeaponDefs_ParseLineCallback @0x543680]
	bool line(const io::ConfigTokens &tokens) {
		if (key_is(tokens, "weapon"))
			return weapon_line(tokens);
		if (key_is(tokens, "end")) {
			// [orig: @0x543746..0x5437DC: the action close @0x543796,
			// Anim_InitActions @0x5437D0, WeaponDefs_ResetParseState @0x5437DC]
			if (in_action_block_) {
				in_action_block_ = false;
				action_line(tokens);
				return false;
			}
			if (current_ >= 0) {
				anim_init_actions(current_);
				reset_parse_state();
			}
			return false;
		}
		if (key_is(tokens, "ammoclass_max_carry")) {
			// The carry caps sit outside the record; only the 118th new class
			// (id 128) lands in it, on the "null" row's category dword, the
			// table's first. [orig: @0x5437EC..0x543873, g_AmmoClassMaxCarry[id]
			// = g_AdmDefs - 0x200]
			uint8_t id = class_id(token(tokens, 2));
			if (id == 0) {
				if (class_names_.size() >= kAmmoClassLimit)
					return false; // "unrecognized ammo class"
				id = add_class(token(tokens, 2));
			}
			if (id == kAmmoClassLimit)
				put32(0, 0, abs32(atol_token(tokens, 3)));
		}
		if (current_ < 0)
			return false; // [orig: @0x54387A..0x543881]
		if (in_action_block_) {
			action_line(tokens); // [orig: @0x543887..0x543899]
			return false;
		}
		if (key_is(tokens, "action"))
			return action_open(tokens);
		property(tokens);
		return false;
	}

	// The fire cycle in ticks: FIRE's delayEnd + RECOIL's delayStart and
	// delayEnd, for every named row [orig: AdmDefs_PostParseRecompute
	// @0x53FEA0, the stores @0x53FEC5..0x53FEDC].
	void post_parse_recompute() {
		for (int32_t slot = 0; slot < kWeaponRecordCapacity; ++slot) {
			if (row(slot)[20] == 0)
				continue;
			const int32_t fire =
					slot_actions_[static_cast<size_t>(slot)][world::weapon_action::kFire];
			const int32_t recoil =
					slot_actions_[static_cast<size_t>(slot)][world::weapon_action::kRecoil];
			if (fire < 0 || recoil < 0) {
				// An unbound action pointer: retail reads through null.
				table_.computable = false;
				continue;
			}
			const ActionDef &f = pool_[static_cast<size_t>(fire)];
			const ActionDef &r = pool_[static_cast<size_t>(recoil)];
			put32(slot, 1104,
					static_cast<int32_t>(static_cast<uint32_t>(f.delay_end) +
							static_cast<uint32_t>(r.delay_start) +
							static_cast<uint32_t>(r.delay_end)));
		}
	}

private:
	uint8_t *row(int32_t slot) {
		return table_.bytes.data() + static_cast<size_t>(slot) * kWeaponRecordSize;
	}
	void put32(int32_t slot, size_t off, int32_t v) {
		io::write_u32_le(row(slot) + off, static_cast<uint32_t>(v));
	}
	void put8(int32_t slot, size_t off, int32_t v) { row(slot)[off] = static_cast<uint8_t>(v); }
	int32_t get32(int32_t slot, size_t off) {
		return static_cast<int32_t>(io::read_u32_le(row(slot) + off));
	}

	// The row's name: the C string at +20, which a 32-character name leaves
	// unterminated so it runs on into `sameas` (+52).
	std::string weapon_name(int32_t slot) {
		const size_t start = static_cast<size_t>(slot) * kWeaponRecordSize + 20;
		size_t end = start;
		while (end < table_.bytes.size() && table_.bytes[end] != 0)
			++end;
		return std::string(
				reinterpret_cast<const char *>(table_.bytes.data()) + start, end - start);
	}

	// strcpy at +off, unbounded: a long value runs on into the following
	// fields and records; past the table it is dropped.
	void copy_unbounded(int32_t slot, size_t off, const char *s) {
		size_t at = static_cast<size_t>(slot) * kWeaponRecordSize + off;
		do {
			if (at < table_.bytes.size())
				table_.bytes[at] = static_cast<uint8_t>(*s);
			++at;
		} while (*s++ != '\0');
	}

	// strncpy(+off, s, n): the characters up to n, NUL-padded to n.
	void copy_padded(int32_t slot, size_t off, const char *s, size_t n) {
		uint8_t *dst = row(slot) + off;
		size_t i = 0;
		for (; i < n && s[i] != '\0'; ++i)
			dst[i] = static_cast<uint8_t>(s[i]);
		for (; i < n; ++i)
			dst[i] = 0;
	}

	// [orig: AdmDef_InitEntryDefaults @0x53FEF0]
	void init_entry_defaults(int32_t slot) {
		std::fill(row(slot), row(slot) + kWeaponRecordSize, uint8_t{ 0 });
		put32(slot, 88, 1); // clipsize
		put32(slot, 92, -1); // startrounds
		const float fov = 80.0f; // renderfov
		uint32_t fov_bits;
		std::memcpy(&fov_bits, &fov, sizeof(fov_bits));
		put32(slot, 328, static_cast<int32_t>(fov_bits));
		put32(slot, 4, -1); // round_type
		put32(slot, 224, 1); // the ammo-class count per round
		put32(slot, 344, 0x10000); // stability x3
		put32(slot, 348, 0x10000);
		put32(slot, 352, 0x10000);
		put32(slot, 152, 2); // scope_min_mag
		slot_actions_[static_cast<size_t>(slot)].fill(-1);
		anim_object_[static_cast<size_t>(slot)] = false;
		anim_name_[static_cast<size_t>(slot)].clear();
	}

	// [orig: WeaponDefs_ResetParseState @0x53FF90]
	void reset_parse_state() {
		in_action_block_ = false;
		current_ = -1;
		animadm_.clear();
	}

	// `weapon <name>`: the row of that name (never "null"), else the lowest
	// free row, reset to the defaults and named (strncpy 32). A `weapon` line
	// inside an open block or a full table ends the file walk.
	// [orig: @0x543694..0x543741; AdmDef_FindIndexByName @0x53FD80;
	//  AdmDef_FindFreeSlot @0x53FC50]
	bool weapon_line(const io::ConfigTokens &tokens) {
		if (current_ >= 0)
			return true; // "weapon didn't have an end" [orig: @0x5436CB]
		const char *name = token(tokens, 2);
		int32_t slot = -1;
		if (!strutil::iequals(name, "null")) {
			for (int32_t i = 0; i < kWeaponRecordCapacity && slot < 0; ++i)
				if (row(i)[20] != 0 && strutil::iequals(name, weapon_name(i)))
					slot = i;
		}
		if (slot < 0) {
			for (int32_t i = 0; i < kWeaponRecordCapacity && slot < 0; ++i)
				if (row(i)[20] == 0)
					slot = i;
		}
		if (slot < 0)
			return true; // "need more space in table" [orig: @0x543719]
		init_entry_defaults(slot);
		copy_padded(slot, 20, name, 32);
		current_ = slot;
		return false;
	}

	int32_t find_action(const std::string &name) const {
		for (size_t i = 0; i < pool_.size(); ++i)
			if (!pool_[i].name.empty() && strutil::iequals(name, pool_[i].name))
				return static_cast<int32_t>(i);
		return -1;
	}

	// `action <name>`: one of the twelve, which opens the block and binds the
	// row's slot to the action the script parse opens [orig: @0x5438C9..0x543958;
	// g_WeaponActionTable @0x830B90].
	bool action_open(const io::ConfigTokens &tokens) {
		int i = 0;
		while (!strutil::iequals(token(tokens, 2), world::kWeaponActionSuffixes[i]))
			if (++i >= kActionCount)
				return false; // "invalid action"
		in_action_block_ = true;
		action_line(tokens);
		slot_actions_[static_cast<size_t>(current_)][static_cast<size_t>(i)] = script_action_;
		return false;
	}

	// The action script lines the bind reads: `action` opens (or reopens) the
	// pool entry "<weapon>_<name>" from its defaults, `end` closes it, `anim`
	// names its clip, `delaystart` / `delay` / `delayend` its delays (`auto`
	// is -1). The other keys bind sounds, effects and handlers the CRC never
	// reads. An `action` line while one is open ("forgot an end") opens
	// nothing. [orig: ActionDef_ParseScriptLine @0x4023C0 — the lookup
	// @0x4024A1, ActionDef_InitDefaults @0x4024DA]
	void action_line(const io::ConfigTokens &tokens) {
		if (!key_is(tokens, "action")) {
			if (key_is(tokens, "end")) {
				script_action_ = -1;
				return;
			}
			if (script_action_ < 0)
				return;
			ActionDef &a = pool_[static_cast<size_t>(script_action_)];
			const char *v = token(tokens, 2);
			if (key_is(tokens, "delaystart")) {
				a.delay_start = strutil::iequals("auto", v) ? -1 : io::retail_atol(v);
			} else if (key_is(tokens, "delay") || key_is(tokens, "delayend")) {
				a.delay_end = strutil::iequals("auto", v) ? -1 : io::retail_atol(v);
			} else if (key_is(tokens, "anim")) {
				a.anim = v;
			}
			return;
		}
		if (script_action_ >= 0)
			return; // "forgot an end"
		const std::string name = weapon_name(current_) + "_" + token(tokens, 2);
		int32_t entry = find_action(name);
		if (entry < 0) {
			pool_.emplace_back();
			entry = static_cast<int32_t>(pool_.size()) - 1;
		}
		pool_[static_cast<size_t>(entry)] = ActionDef{ name, {}, 0, 0 };
		script_action_ = entry;
	}

	// The weapon's `end` bind: its .adm loaded (first load parses the file
	// through the tokenizer), the per-round count defaulted, each of the
	// twelve actions bound (an unbound one found by name or made from the
	// defaults) with its `auto` delays read off the rings, then the idle slot
	// played. [orig: Anim_InitActions @0x541FA0 — the load @0x541FEF
	// (AnimMap_LoadAdmFile @0x40CC40, File_ParseASCIIFile @0x40CEB4), +224
	// @0x54201B..0x542024, the generated action @0x5420C6..0x54210D, the anim
	// arms @0x542143..0x542210, the idle play @0x54225A]
	void anim_init_actions(int32_t slot) {
		const size_t s = static_cast<size_t>(slot);
		if (!animadm_.empty() && assets_ != nullptr) {
			const std::string resolved = anim::adm_load_name(
					animadm_, [this](const std::string &file) { return files_.has_file(file); });
			if (!rings_.loaded(resolved) && files_.has_file(resolved)) {
				std::vector<uint8_t> text;
				if (files_.read_file(resolved, text))
					io::for_each_config_line(reinterpret_cast<const char *>(text.data()),
							text.size(), tokens_, [](io::ConfigTokens &) {});
			}
			anim_object_[s] = rings_.load(assets_, resolved);
			anim_name_[s] = resolved;
		}
		if (get32(slot, 224) == 0 && row(slot)[216] != 0)
			put32(slot, 224, 1);
		for (int i = 0; i < kActionCount; ++i) {
			int32_t &entry = slot_actions_[s][static_cast<size_t>(i)];
			if (entry < 0) {
				const std::string name = weapon_name(slot) + "_" + world::kWeaponActionSuffixes[i];
				entry = find_action(name);
				if (entry < 0) {
					pool_.push_back(ActionDef{ name, {}, 0, 0 });
					entry = static_cast<int32_t>(pool_.size()) - 1;
				}
			}
			ActionDef &a = pool_[static_cast<size_t>(entry)];
			if (!a.anim.empty() && anim_object_[s] && rings_.resolves(anim_name_[s], a.anim)) {
				if (a.delay_start == -1)
					a.delay_start = duration_ticks(rings_, anim_name_[s], a.anim);
				if (a.delay_end == -1) {
					const int32_t duration = duration_ticks(rings_, anim_name_[s], a.anim);
					a.delay_end = duration > a.delay_start ? duration - a.delay_start : duration;
				}
			} else {
				if (a.delay_start == -1)
					a.delay_start = 0;
				if (a.delay_end == -1)
					a.delay_end = 0;
			}
		}
		if (anim_object_[s])
			rings_.serve(anim_name_[s], "anim_wpn_idle");
	}

	uint8_t class_id(const char *name) const {
		for (size_t i = 0; i < class_names_.size(); ++i)
			if (strutil::iequals(name, class_names_[i]))
				return class_ids_[i];
		return 0;
	}

	uint8_t add_class(const char *name) {
		class_names_.emplace_back(name);
		class_ids_.push_back(next_class_id_);
		return next_class_id_++;
	}

	// The property chain [orig: @0x543979..0x545098]: an unknown key stores
	// nothing. Keys whose stores land only in bytes the whole-table CRC
	// overwrites (gfx1/gfx3, the texture keys, pos/tpos, the text ids,
	// heat_sound) are recognized and leave the record as it is.
	void property(const io::ConfigTokens &tokens) {
		const int32_t c = current_;
		const char *a = token(tokens, 2);
		if (key_is(tokens, "category")) {
			const uint32_t v = static_cast<uint32_t>(io::retail_atol(a));
			put32(c, 0, v >= 12 ? 0 : static_cast<int32_t>(v)); // [orig: @0x543979..0x5439C6]
		} else if (key_is(tokens, "rank")) {
			const uint32_t v = static_cast<uint32_t>(io::retail_atol(a));
			put32(c, 16, v > 0x40 ? 0 : static_cast<int32_t>(v)); // [orig: @0x5439CD..0x543A1B]
		} else if (key_is(tokens, "statid")) {
			put32(c, 84, io::retail_atol(a));
		} else if (key_is(tokens, "clipsize")) {
			put32(c, 88, io::retail_atol(a));
		} else if (key_is(tokens, "startrounds")) {
			put32(c, 92, io::retail_atol(a));
		} else if (key_is(tokens, "classrounds")) {
			// [orig: @0x543AB0..0x543B16; an unmatched name reads the table
			// past its five rows]
			if (const NamedValue *cls = find_named(kClassRounds, std::size(kClassRounds), a))
				put32(c, 96 + 4 * static_cast<size_t>(cls->value), atol_token(tokens, 3));
		} else if (key_is(tokens, "error")) {
			for (int k = 0; k < 6; ++k)
				put32(c, 176 + 4 * static_cast<size_t>(k), fixed16_token(tokens, 2 + k));
		} else if (key_is(tokens, "error_hipTheta")) {
			put32(c, 204, fixed16_token(tokens, 2));
		} else if (key_is(tokens, "error_upTheta")) {
			put32(c, 208, fixed16_token(tokens, 2));
		} else if (key_is(tokens, "vmacrotoken")) {
			copy_padded(c, 728, a, 16);
		} else if (key_is(tokens, "round_type")) {
			// An ammo row index; 0 (a miss, or the first row) leaves it
			// [orig: @0x543C57..0x543CA9 -> AmmoDef_LookupByName @0x409870].
			const int32_t index = ammo_.lookup_by_name(a);
			if (index != 0)
				put32(c, 4, index);
		} else if (key_is(tokens, "special_hold")) {
			put32(c, 164, io::retail_atol(a));
		} else if (key_is(tokens, "attack_anim")) {
			put32(c, 168, io::retail_atol(a));
		} else if (key_is(tokens, "run_anim")) {
			put32(c, 172, io::retail_atol(a));
		} else if (key_is(tokens, "animadm") || key_is(tokens, "animcal")) {
			animadm_ = a; // the global g_WeaponParseAnimAdmFile, not the record [orig: @0x543D47 / @0x543D77]
		} else if (key_is(tokens, "flags")) {
			// The first matching name ORs both masks [orig: @0x543DA7..0x543E00,
			// the 44-row table @0x830BF0].
			for (size_t i = 0; defscan::weapon_flag_at(i) != nullptr; ++i) {
				const defscan::FlagEntry *flag = defscan::weapon_flag_at(i);
				if (strutil::iequals(a, flag->name)) {
					put32(c, 8, get32(c, 8) | flag->bit);
					put32(c, 12, get32(c, 12) | flag->bit2);
					break;
				}
			}
		} else if (key_is(tokens, "heat_effect")) {
			copy_unbounded(c, 856, a);
			put32(c, 884, fixed16_token(tokens, 3));
		} else if (key_is(tokens, "heat_values")) {
			put32(c, 876, fixed16_token(tokens, 2) / 100); // signed idiv [orig: @0x543EEB]
			put32(c, 880, fixed16_token(tokens, 3) / 6200); // [orig: @0x543F1B]
		} else if (key_is(tokens, "charfilter")) {
			if (const NamedValue *v = find_named(kCharFilter, std::size(kCharFilter), a))
				put32(c, 124, get32(c, 124) | v->value);
		} else if (key_is(tokens, "teamfilter")) {
			if (const NamedValue *v = find_named(kTeamFilter, std::size(kTeamFilter), a))
				put32(c, 128, get32(c, 128) | v->value);
		} else if (key_is(tokens, "ammobucket")) {
			put32(c, 220, abs32(io::retail_atol(a)));
		} else if (key_is(tokens, "sameas")) {
			copy_padded(c, 52, a, 32);
		} else if (key_is(tokens, "maxclips")) {
			put32(c, 332, io::retail_atol(a));
		} else if (key_is(tokens, "clipweight")) {
			put32(c, 336, fixed16_token(tokens, 2));
		} else if (key_is(tokens, "weaponweight")) {
			put32(c, 340, fixed16_token(tokens, 2));
		} else if (key_is(tokens, "stability")) {
			for (int k = 0; k < 3; ++k)
				put32(c, 344 + 4 * static_cast<size_t>(k), fixed16_token(tokens, 2 + k));
		} else if (key_is(tokens, "emplacedstance")) {
			put32(c, 228, io::retail_atol(a));
		} else if (key_is(tokens, "ammoclass")) {
			// The class's id, a new class taking the next one, then the
			// per-round count [orig: @0x5441A6..0x544242].
			uint8_t id = class_id(a);
			if (id == 0 && class_names_.size() < kAmmoClassLimit)
				id = add_class(a);
			put8(c, 216, id);
			put32(c, 224, abs32(atol_token(tokens, 3)));
		} else if (key_is(tokens, "hudclipgfx")) {
			// Stored only when the image is mounted [orig: @0x54427F..0x5442D3].
			if (files_.has_file(token(tokens, 4))) {
				put32(c, 620, io::retail_atol(a));
				put32(c, 624, atol_token(tokens, 3));
			}
		} else if (key_is(tokens, "hudrndgfx")) {
			// [orig: @0x5442FC..0x5443A8]
			if (files_.has_file(token(tokens, 7))) {
				for (int k = 0; k < 4; ++k)
					put32(c, 644 + 4 * static_cast<size_t>(k), atol_token(tokens, 2 + k));
				put8(c, 727, atol_token(tokens, 6));
			}
		} else if (key_is(tokens, "targetpitchmax")) {
			put32(c, 316, mul32(io::retail_atol(a), 11930464));
		} else if (key_is(tokens, "targetpitchmin")) {
			put32(c, 320, mul32(io::retail_atol(a), -11930464));
		} else if (key_is(tokens, "targetyawrange")) {
			put32(c, 324, mul32(io::retail_atol(a), 11930464)); // [orig: @0x54445C..0x54446E]
		} else if (key_is(tokens, "launchuserpoint")) {
			copy_unbounded(c, 744, a);
		} else if (key_is(tokens, "soundfireloop")) {
			copy_unbounded(c, 760, a);
		} else if (key_is(tokens, "soundtrailoff")) {
			copy_unbounded(c, 784, a);
		} else if (key_is(tokens, "soundhead")) {
			copy_unbounded(c, 808, a);
		} else if (key_is(tokens, "soundlockedtone")) {
			copy_unbounded(c, 832, a);
		} else if (key_is(tokens, "switchcategory")) {
			put32(c, 360, 1);
			put32(c, 356, io::retail_atol(a)); // [orig: @0x5445AE / @0x5445CD]
		} else if (key_is(tokens, "renderfov")) {
			// fild of the atol, stored as a float [orig: @0x54482A..0x544857].
			const float fov = static_cast<float>(io::retail_atol(a));
			uint32_t bits;
			std::memcpy(&bits, &fov, sizeof(bits));
			put32(c, 328, static_cast<int32_t>(bits));
		} else if (key_is(tokens, "splash")) {
			put32(c, 1108, io::retail_atol(a));
		} else if (key_is(tokens, "designation_time")) {
			put32(c, 1112, mul32(io::retail_atol(a), 62)); // [orig: @0x544895..0x5448C5]
		} else if (key_is(tokens, "sights")) {
			// The row count rises for an image that is mounted, and the row
			// written is the count before it: a fifth row's texture pointer
			// lands on the count itself [orig: @0x544AC8..0x544B32].
			if (files_.has_file(a)) {
				const int32_t index = get32(c, 600);
				put32(c, 600, static_cast<int32_t>(static_cast<uint32_t>(index) + 1u));
				if (index < 0 || index >= 4)
					table_.computable = false;
			}
		} else if (key_is(tokens, "loadout_menu_icon")) {
			// strncpy of min(strlen, 32) characters: no terminator, a shorter
			// later value keeping the earlier one's tail [orig: @0x544BD6..0x544C61].
			if (files_.has_file(a)) {
				const size_t n = std::min<size_t>(std::strlen(a), 32);
				std::memcpy(row(c) + 888, a, n);
			}
		} else if (key_is(tokens, "weapon_class")) {
			// [orig: @0x544CA8..0x544D60]
			if (strutil::iequals(a, "primary"))
				put32(c, 932, 1);
			else if (strutil::iequals(a, "secondary"))
				put32(c, 932, 2);
			else if (strutil::iequals(a, "grenade"))
				put32(c, 932, 3);
			else if (strutil::iequals(a, "accessory"))
				put32(c, 932, 0);
		} else if (key_is(tokens, "farpinfo")) {
			put32(c, 232, io::retail_atol(a));
			put32(c, 236, atol_token(tokens, 3));
		} else if (key_is(tokens, "loadout_selectable")) {
			put32(c, 936, io::retail_atol(a));
		} else if (key_is(tokens, "loadout_subclasses")) {
			put32(c, 940, io::retail_atol(a));
		} else if (key_is(tokens, "scope_paralax_distance")) {
			// atof * 65535.0 under the device's single-precision x87 control
			// word, then _ftol2_sse [orig: @0x544E4E..0x544E80].
			const float product = static_cast<float>(io::retail_atof(a) * 65535.0);
			put32(c, 140, io::retail_ftol_sse2(product));
		} else if (key_is(tokens, "scope_max_zero")) {
			// [orig: @0x544E8B..0x544EFD]
			put32(c, 132, io::retail_atol(a));
			put32(c, 156, atol_token(tokens, 3));
			put32(c, 160, atol_token(tokens, 4));
			if (tokens.count > 4)
				put32(c, 136, atol_token(tokens, 5));
		} else if (key_is(tokens, "scope_max_mag")) {
			put32(c, 144, io::retail_atol(a));
			put32(c, 148, atol_token(tokens, 3));
		} else if (key_is(tokens, "scope_min_mag")) {
			put32(c, 152, io::retail_atol(a));
		}
	}

	WeaponRecordTable &table_;
	const AmmoRecordTable &ammo_;
	const mission::BootFileSource &files_;
	const assets::AssetStore *assets_;
	io::ConfigTokens &tokens_;
	int32_t current_ = -1; // g_WeaponParseCurWeaponDef
	bool in_action_block_ = false; // g_WeaponParseInActionBlock
	int32_t script_action_ = -1; // ActionDef_ParseScriptLine's g_CurrentActionDef
	std::string animadm_; // g_WeaponParseAnimAdmFile
	std::vector<ActionDef> pool_;
	std::array<std::array<int32_t, kActionCount>, kWeaponRecordCapacity> slot_actions_{};
	std::array<bool, kWeaponRecordCapacity> anim_object_{}; // +372 non-null
	std::array<std::string, kWeaponRecordCapacity> anim_name_{};
	anim::AdmRingTable rings_;
	std::vector<std::string> class_names_;
	std::vector<uint8_t> class_ids_;
	uint8_t next_class_id_ = 11;
};

} // namespace

WeaponRecordTable build_weapon_record_table(const uint8_t *data, size_t size,
		const AmmoRecordTable &ammo, const mission::BootFileSource &files,
		const assets::AssetStore *assets, io::ConfigTokens &tokens) {
	WeaponRecordTable table;
	WeaponParse parse(table, ammo, files, assets, tokens);
	parse.init_all();
	if (data != nullptr) {
		// [orig: WeaponDefs_LoadFile @0x5450A0 — File_ParseASCIIFile @0x5450D4,
		//  AdmDefs_PostParseRecompute @0x5450E0]
		io::for_each_config_line(reinterpret_cast<const char *>(data), size, tokens,
				[&](io::ConfigTokens &line) { return parse.line(line); });
	}
	parse.post_parse_recompute();
	return table;
}

int32_t WeaponRecordTable::live_count() const {
	int32_t count = 0;
	while (count < kWeaponRecordCapacity &&
			bytes.size() >= (static_cast<size_t>(count) + 1) * kWeaponRecordSize &&
			bytes[static_cast<size_t>(count) * kWeaponRecordSize + 20] != 0)
		++count;
	return count;
}

uint32_t WeaponRecordTable::sanitized_crc() const {
	const int32_t count = live_count();
	if (count == 0)
		return 0; // [orig: @0x53FBCA]
	uint32_t crc = io::kCrc32Mpeg2Init;
	uint8_t image[kWeaponRecordSize];
	for (int32_t slot = 0; slot < count; ++slot) {
		std::memcpy(image, bytes.data() + static_cast<size_t>(slot) * kWeaponRecordSize,
				kWeaponRecordSize);
		const auto fill32 = [&image](size_t from, size_t to, uint32_t v) {
			for (size_t off = from; off < to; off += 4)
				io::write_u32_le(image + off, v);
		};
		// [orig: @0x53F942..0x53FB9F]
		io::write_u32_le(image + 240, 22);
		fill32(244, 316, 0x0B0B0B0Bu);
		float fov;
		std::memcpy(&fov, image + 328, sizeof(fov));
		const int32_t truncated = io::retail_ftol_sse2(fov);
		io::write_u32_le(image + 328,
				static_cast<uint32_t>(
						static_cast<int32_t>(static_cast<uint32_t>(truncated) + 1u) >> 1));
		fill32(364, 376, 0);
		fill32(660, 676, 0);
		fill32(676, 724, 44);
		io::write_u32_le(image + 872, 0);
		io::write_u32_le(image + 924, 0);
		io::write_u32_le(image + 928, 0);
		fill32(376, 456, 0x42424242u);
		fill32(604, 620, 0x42424242u);
		fill32(628, 644, 0x42424242u);
		io::write_u32_le(image + 212, 77);
		io::write_u32_le(image + 920, 88);
		std::memset(image + 456, 99, 0x90);
		image[724] = image[725] = image[726] = 111;
		std::memset(image + 944, 222, 0xA0);
		crc = io::crc32_mpeg2_update(crc, image, kWeaponRecordSize);
	}
	return crc;
}

} // namespace opennova::inmatch
