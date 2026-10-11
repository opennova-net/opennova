#include <runtime/inmatch/integrity_ammo_image.h>

#include <base/io/crc32_mpeg2.h>
#include <base/io/crt_ftol.h>
#include <base/io/le.h>
#include <base/io/strutil.h>
#include <formats/def/def.h>
#include <formats/def/def_scan.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/world/ammo_table.h>

#include <algorithm>
#include <cstring>

namespace opennova::inmatch {

namespace {

// Retail's tokens[k] (k >= 1) is the tokenizer's slot k - 1; a slot no line
// has set reads as the empty string here (retail would hand the parse a null
// pointer, which no shipped file reaches).
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
int32_t fixed16(const char *s) {
	return defscan::parse_fixed16_digits_n(s, std::strlen(s));
}

// The low 32 bits of ((int64)v * scale + 0x8000) >> 16: imul, add/adc 0x8000,
// shrd 16 [orig: AmmoDef_ParseSecondsToTicks @0x40A0F0 (scale 62),
// AmmoDef_ParseTurnRate @0x40A130 (scale 0x2EFAA, @0x40A14F..0x40A15A)].
int32_t scaled_q16(int32_t v, int64_t scale) {
	const uint64_t product = static_cast<uint64_t>(static_cast<int64_t>(v) * scale + 0x8000);
	return static_cast<int32_t>(static_cast<uint32_t>(product >> 16));
}

int32_t seconds_to_ticks(const char *s) {
	return scaled_q16(fixed16(s), 62);
}

// Degrees to BAM in 32-bit arithmetic that wraps (imul 0xB60B60).
int32_t bam(int32_t degrees) {
	return static_cast<int32_t>(static_cast<uint32_t>(degrees) * 11930464u);
}

// ((r << 8) + g) << 8) + b over three atol'd slots, wrapping [orig: the
// light_move arm @0x40AF1F..0x40AF72, light_impact @0x40AF7F..0x40AFD3].
int32_t packed_rgb(const io::ConfigTokens &tokens, int first) {
	const uint32_t r = static_cast<uint32_t>(atol_token(tokens, first));
	const uint32_t g = static_cast<uint32_t>(atol_token(tokens, first + 1));
	const uint32_t b = static_cast<uint32_t>(atol_token(tokens, first + 2));
	return static_cast<int32_t>((((r << 8) + g) << 8) + b);
}

// The tracer type name or its atol [orig: AmmoDef_ParseTypeName @0x409B90].
int32_t tracer_type(const char *name) {
	for (size_t i = 0; def::def_ammo_tracer_keyword(i) != nullptr; ++i)
		if (strutil::iequals(name, def::def_ammo_tracer_keyword(i)))
			return def::def_ammo_tracer_value(i);
	return io::retail_atol(name);
}

// The effects-table stage: 28 rows of 16 bytes at the tag's index, cleared
// by every reset [orig: g_AmmoFxStageTagId @0xA2E964 (stride 16);
// AmmoDef_ResetParseState @0x409B5D..0x409B85]. Only what the row reads back
// is kept: the tag and the sound set (found, and its +0x48 target id).
struct FxStageRow {
	int32_t tag = 0;
	bool sound = false;
	int32_t sound_target_id = 0;
};

class AmmoParse {
public:
	AmmoParse(AmmoRecordTable &table, const std::vector<IntegrityItemRow> &items,
			const audio::SoundSetIndex *sounds) :
			table_(table), items_(items), sounds_(sounds) {}

	// One line; true ends the file walk. [orig: AmmoDef_ParseProperty @0x40A2D0]
	bool line(const io::ConfigTokens &tokens) {
		if (key_is(tokens, "debug")) {
			// g_AmmoParsePass = 2 (logging only), then the property chain
			// [orig: @0x40A300].
		} else if (key_is(tokens, "ammo")) {
			// [orig: @0x40A34D..0x40A397]
			if (current_ >= 0)
				return true; // "definition missing end" [orig: @0x40A383]
			current_ = allocate_slot(token(tokens, 2));
			return false;
		} else if (key_is(tokens, "end")) {
			// [orig: @0x40A3BF..0x40A442]
			if (current_ < 0)
				return false;
			if (in_fx_table_) {
				in_fx_table_ = false;
				init_effects_table(current_);
			} else {
				inherit_defaults(current_);
				reset_parse_state();
			}
			return false;
		}
		if (current_ < 0)
			return false; // [orig: @0x40A310]
		if (in_fx_table_) {
			fx_row(tokens);
			return false;
		}
		property(tokens);
		return false;
	}

private:
	uint8_t *row(int32_t index) {
		return table_.bytes.data() + static_cast<size_t>(index) * kAmmoRecordSize;
	}
	void put32(int32_t index, size_t off, int32_t v) {
		io::write_u32_le(row(index) + off, static_cast<uint32_t>(v));
	}
	void put16(int32_t index, size_t off, int32_t v) {
		io::write_u16_le(row(index) + off, static_cast<uint16_t>(v));
	}
	void put8(int32_t index, size_t off, int32_t v) { row(index)[off] = static_cast<uint8_t>(v); }
	int32_t get32(int32_t index, size_t off) {
		return static_cast<int32_t>(io::read_u32_le(row(index) + off));
	}

	// [orig: AmmoDef_AllocateSlot @0x409A20]
	int32_t allocate_slot(const char *name) {
		if (table_.count >= table_.capacity)
			return -1; // "Used up ammo def slots" [orig: @0x409A33]
		const int32_t r = table_.count++;
		// The defaults over the zeroed table [orig: @0x409A67..0x409AF6].
		for (const size_t off : { 0u, 32u, 20u, 16u, 24u, 12u, 52u, 56u, 64u, 68u, 76u, 92u, 96u,
					 104u, 112u, 196u, 200u, 184u })
			put32(r, off, 0);
		for (const size_t off : { 4u, 8u, 80u, 84u, 88u })
			put32(r, off, -1);
		put32(r, 28, 0x10000);
		put32(r, 60, 0x7FFFFFFF);
		for (const size_t off : { 44u, 46u, 100u, 102u, 108u, 116u, 118u })
			put16(r, off, 0);
		put8(r, 224, 0);
		put8(r, 225, 0);
		put8(r, 227, 24);
		put8(r, 228, 24);
		put8(r, 229, 24);
		// The name: at most 31 characters, then a NUL [orig: @0x409AFC..0x409B24].
		uint8_t *dst = row(r) + 144;
		for (int remaining = 32; remaining > 1 && *name != '\0'; --remaining)
			*dst++ = static_cast<uint8_t>(*name++);
		*dst = 0;
		return r;
	}

	// [orig: AmmoDef_InheritDefaults @0x409EB0, never for the first row @0x409EBC]
	void inherit_defaults(int32_t r) {
		if (r == 0)
			return;
		for (const size_t off : { 4u, 8u, 16u, 20u, 28u })
			if (get32(r, off) == -1)
				put32(r, off, get32(0, off));
	}

	// [orig: AmmoDef_ResetParseState @0x409B30]
	void reset_parse_state() {
		current_ = -1;
		in_fx_table_ = false;
		for (FxStageRow &s : stage_)
			s = FxStageRow{};
	}

	// One effects-table row: four columns or more, the tag matched from row 1
	// of the tag table, a tag staged once (the first row wins), the sound set
	// looked up across the banks unless `none`. The effect handle and the
	// fourth column feed no CRC'd byte. [orig: @0x40A326..0x40A599]
	void fx_row(const io::ConfigTokens &tokens) {
		if (tokens.count < 4)
			return; // "wrong number of columns" [orig: @0x40A338]
		int tag = 1;
		while (!strutil::iequals(world::kImpactEffectTagNames[tag], token(tokens, 1)))
			if (++tag >= world::kImpactEffectTagCount)
				return; // "unknown effect tag" [orig: @0x40A49F]
		FxStageRow &s = stage_[static_cast<size_t>(tag)];
		if (s.tag != 0)
			return; // "redefining the effect" [orig: @0x40A502]
		s.tag = tag;
		s.sound = false;
		s.sound_target_id = 0;
		const char *sound = token(tokens, 3);
		if (!strutil::iequals(sound, "none") && sounds_ != nullptr) {
			// [orig: SoundBank_FindSetByNameAnyBank @0x5274F0, @0x40A57B]
			const audio::SetLocation set = sounds_->find(sound);
			s.sound = set.valid();
			s.sound_target_id = set.cull_range;
		}
	}

	// Installs the first closed table only: the count word +108 is one row per
	// staged tag plus the always-copied row 0, and +140 is the whiz radius from
	// a copied move (1) or zip (3) row with a sound set, its set's +0x48 word
	// << 16 capped at 50.0, in index order (zip after move). +104 is the table
	// pointer, written as a nonzero mark only (the CRC zeroes it).
	// [orig: AmmoDef_InitEffectsTable @0x409F20 (the +104 / +108 gate ahead of
	//  @0x409F62), the copy loop @0x409FD1..0x40A097, +140 @0x40A04B..0x40A07F, +108 @0x40A088]
	void init_effects_table(int32_t r) {
		if (get32(r, 104) != 0 || io::read_u16_le(row(r) + 108) != 0)
			return;
		put32(r, 104, 1);
		int32_t count = 0;
		for (int index = 0; index < world::kImpactEffectTagCount; ++index) {
			const FxStageRow &s = stage_[static_cast<size_t>(index)];
			if (s.tag == 0 && index > 0)
				continue;
			if (s.sound && (s.tag == 3 || s.tag == 1)) {
				const int32_t radius =
						static_cast<int32_t>(static_cast<uint32_t>(s.sound_target_id) << 16);
				put32(r, 140, radius > 3276800 ? 3276800 : radius);
			}
			put16(r, 108, ++count);
		}
	}

	// frndlyTrcrID / foeTrcrID: the item index of the type id, else (on a miss
	// or a match on row 0) the index of the item named as this ammo; neither
	// leaves the field. [orig: @0x40A5C4..0x40A690; ItemList_FindIndexByTypeId
	// @0x49E100 (0 on a miss); ItemList_FindIndexByPrimaryName @0x49E010
	// (-1 on a miss)]
	void tracer_item(int32_t r, size_t off, const char *type_id) {
		const int32_t id = io::retail_atol(type_id);
		int32_t index = 0;
		for (size_t i = 0; i < items_.size(); ++i) {
			if (items_[i].id == id) {
				index = static_cast<int32_t>(i);
				break;
			}
		}
		if (index == 0) {
			index = -1;
			const char *own = reinterpret_cast<const char *>(row(r) + 144);
			const std::string name(own, strnlen(own, kAmmoRecordSize - 144));
			for (size_t i = 0; i < items_.size(); ++i) {
				if (strutil::iequals(name, items_[i].name)) {
					index = static_cast<int32_t>(i);
					break;
				}
			}
			if (index == -1)
				return;
		}
		put32(r, off, index);
	}

	// notarmmedammo: an unbounded strcpy at +241, which a name of 35
	// characters or more runs into the next row's bytes (its allocation then
	// re-stamps the defaults it writes) and, from the last row, past the table
	// (dropped here) [orig: @0x40A76D..0x40A782].
	void copy_unbounded(int32_t r, size_t off, const char *s) {
		size_t at = static_cast<size_t>(r) * kAmmoRecordSize + off;
		do {
			if (at < table_.bytes.size())
				table_.bytes[at] = static_cast<uint8_t>(*s);
			++at;
		} while (*s++ != '\0');
	}

	// The property chain [orig: @0x40A5A3..0x40B092]; an unknown key warns
	// "unrecognized token" and stores nothing.
	void property(const io::ConfigTokens &tokens) {
		const int32_t r = current_;
		const char *a = token(tokens, 2);
		if (key_is(tokens, "effects_table")) {
			in_fx_table_ = true;
		} else if (key_is(tokens, "frndlyTrcrID")) {
			tracer_item(r, 16, a);
		} else if (key_is(tokens, "foeTrcrID")) {
			tracer_item(r, 20, a);
		} else if (key_is(tokens, "velocity")) {
			put32(r, 4, io::retail_atol(a));
		} else if (key_is(tokens, "dopplerdiv")) {
			put8(r, 240, io::retail_atol(a));
		} else if (key_is(tokens, "min_stable_velocity")) {
			put32(r, 176, io::retail_atol(a));
		} else if (key_is(tokens, "tracerRate")) {
			put8(r, 226, io::retail_atol(a));
		} else if (key_is(tokens, "notarmmedammo")) {
			copy_unbounded(r, 241, a);
		} else if (key_is(tokens, "tracer_type")) {
			// [orig: @0x40A7A0..0x40A7FA]
			put32(r, 232, tokens.count < 2 ? 0 : tracer_type(a));
			put32(r, 236, tokens.count < 3 ? get32(r, 232) : tracer_type(token(tokens, 3)));
		} else if (key_is(tokens, "MF_Light")) {
			put32(r, 36, 1);
			put32(r, 40, io::retail_atol(a));
		} else if (key_is(tokens, "bullet_radius")) {
			put32(r, 32, fixed16(a));
		} else if (key_is(tokens, "max_age")) {
			put32(r, 8, seconds_to_ticks(a));
		} else if (key_is(tokens, "arm_age")) {
			put32(r, 12, seconds_to_ticks(a));
		} else if (key_is(tokens, "ai_launch") || key_is(tokens, "ai_launcheffect") ||
				key_is(tokens, "kz_sound") || key_is(tokens, "secondary_effect")) {
			// A sound-set pointer or an effect handle at +64 / +68 / +76 / +72:
			// bytes the CRC zeroes [orig: @0x40A8EF, @0x40A91D, @0x40A94B, @0x40AA36].
		} else if (key_is(tokens, "secondary_anim")) {
			put8(r, 224, io::retail_atol(a));
		} else if (key_is(tokens, "kz_physics")) {
			put8(r, 225, io::retail_atol(a));
		} else if (key_is(tokens, "recoil")) {
			put8(r, 227, io::retail_atol(a));
			put8(r, 228, atol_token(tokens, 3));
			put8(r, 229, atol_token(tokens, 4));
		} else if (key_is(tokens, "heat_det_range")) {
			put16(r, 100, static_cast<int32_t>(static_cast<uint32_t>(fixed16(a)) >> 16));
		} else if (key_is(tokens, "radar_det_range")) {
			put16(r, 102, static_cast<int32_t>(static_cast<uint32_t>(fixed16(a)) >> 16));
		} else if (key_is(tokens, "drag")) {
			put32(r, 28, fixed16(a));
		} else if (key_is(tokens, "error")) {
			put32(r, 24, fixed16(a));
		} else if (key_is(tokens, "tumble_error")) {
			put32(r, 180, fixed16(a));
		} else if (key_is(tokens, "weight_in_grains")) {
			put32(r, 184, io::retail_atol(a));
		} else if (key_is(tokens, "min_damage")) {
			put32(r, 188, io::retail_atol(a));
		} else if (key_is(tokens, "max_damage")) {
			put32(r, 192, io::retail_atol(a));
		} else if (key_is(tokens, "penetration_impact")) {
			put32(r, 196, io::retail_atol(a));
		} else if (key_is(tokens, "penetration_kz")) {
			put32(r, 200, io::retail_atol(a));
		} else if (key_is(tokens, "armor_density")) {
			put32(r, 204, io::retail_atol(a));
			put32(r, 208, atol_token(tokens, 3));
			put32(r, 212, atol_token(tokens, 4));
		} else if (key_is(tokens, "kztype")) {
			// Stored only for table rows 1..7, else "bad kill zone type"
			// [orig: @0x40AC9C..0x40ACC1, the table @0x8133E0].
			for (int i = 1; i < 8; ++i) {
				if (strutil::iequals(a, def::def_ammo_kz_keyword(static_cast<size_t>(i)))) {
					put16(r, 44, i);
					break;
				}
			}
		} else if (key_is(tokens, "kz_minradius")) {
			put32(r, 52, fixed16(a));
		} else if (key_is(tokens, "kz_maxradius")) {
			put32(r, 56, fixed16(a));
		} else if (key_is(tokens, "kz_pieslice")) {
			put32(r, 60, bam(io::retail_atol(a) / 2)); // cdq/sub/sar [orig: @0x40AD54]
		} else if (key_is(tokens, "kz_damage")) {
			put16(r, 46, io::retail_atol(a));
		} else if (key_is(tokens, "spread_count")) {
			put32(r, 48, io::retail_atol(a));
		} else if (key_is(tokens, "turnrate_maxpit")) {
			put32(r, 80, scaled_q16(fixed16(a), 192426));
		} else if (key_is(tokens, "turnrate_maxyaw")) {
			put32(r, 84, scaled_q16(fixed16(a), 192426));
		} else if (key_is(tokens, "boresight_maxang")) {
			put32(r, 88, bam(io::retail_atol(a)));
		} else if (key_is(tokens, "climb_angle")) {
			put32(r, 92, bam(io::retail_atol(a)));
		} else if (key_is(tokens, "climb_time")) {
			put32(r, 96, seconds_to_ticks(a));
		} else if (key_is(tokens, "scorch_id")) {
			put16(r, 116, io::retail_atol(a));
		} else if (key_is(tokens, "scar_type")) {
			put16(r, 118, io::retail_atol(a));
		} else if (key_is(tokens, "light_move")) {
			put32(r, 120, fixed16(a));
			put32(r, 124, packed_rgb(tokens, 3));
		} else if (key_is(tokens, "light_impact")) {
			// [orig: @0x40AF9E..0x40B005]
			put32(r, 132, fixed16(a));
			put32(r, 128, packed_rgb(tokens, 3));
			const int32_t ticks = seconds_to_ticks(token(tokens, 6));
			put32(r, 136, ticks != 0 ? ticks : 10);
		} else if (key_is(tokens, "flag")) {
			// The first of the 30 table names that matches ORs its mask; the
			// two later `internal` rows never match [orig: @0x40B03B..0x40B053,
			// the table @0x813500].
			for (size_t i = 0; def::def_ammo_flag_keyword(i) != nullptr; ++i) {
				if (strutil::iequals(a, def::def_ammo_flag_keyword(i))) {
					put32(r, 0,
							static_cast<int32_t>(static_cast<uint32_t>(get32(r, 0)) |
									def::def_ammo_flag_bit(i)));
					break;
				}
			}
		}
	}

	AmmoRecordTable &table_;
	const std::vector<IntegrityItemRow> &items_;
	const audio::SoundSetIndex *sounds_;
	int32_t current_ = -1;
	bool in_fx_table_ = false;
	FxStageRow stage_[world::kImpactEffectTagCount];
};

} // namespace

std::vector<IntegrityItemRow> read_integrity_item_rows(const uint8_t *data, size_t size) {
	std::vector<IntegrityItemRow> rows;
	if (data == nullptr)
		return rows;
	bool open = false;
	io::for_each_config_line(
			reinterpret_cast<const char *>(data), size, [&](io::ConfigTokens &tokens) {
				if (key_is(tokens, "end")) {
					open = false; // [orig: @0x49EB2F..0x49EB5F]
					return;
				}
				if (key_is(tokens, "begin")) {
					// A row per `begin`, the name cut at 46 characters in the line
					// buffer itself [orig: @0x49EBA8..0x49EBFB].
					open = true;
					const char *name = token(tokens, 2);
					if (std::strlen(name) >= 46)
						tokens.terminate_at(name + 46);
					rows.push_back(IntegrityItemRow{ token(tokens, 2), 0 });
					return;
				}
				if (open && key_is(tokens, "id"))
					rows.back().id =
							static_cast<int32_t>(static_cast<uint32_t>(atol_token(tokens, 2)) -
									100000u); // [orig: @0x49EC54]
			});
	return rows;
}

AmmoRecordTable build_ammo_record_table(const uint8_t *data, size_t size,
		const std::vector<IntegrityItemRow> &items, const audio::SoundSetIndex *sounds,
		io::ConfigTokens &tokens) {
	AmmoRecordTable table;
	if (data == nullptr)
		return table;
	const char *text = reinterpret_cast<const char *>(data);
	// The count pass: every line whose key is `ammo` [orig: AmmoDef_LoadAll
	// @0x40B0DE -> AmmoDef_CountCallback @0x409E70]. The global itself is not
	// clamped (the buffer is, to 256 rows [orig: AmmoDef_AllocateBuffer
	// @0x409990]); the memset that follows covers capacity rows
	// [orig: @0x40B106].
	io::for_each_config_line(text, size, tokens, [&](io::ConfigTokens &line) {
		if (key_is(line, "ammo"))
			++table.capacity;
	});
	table.bytes.assign(static_cast<size_t>(table.capacity) * kAmmoRecordSize, 0);
	// The parse over the same file, its early stale slots the count pass's
	// [orig: @0x40B116].
	AmmoParse parse(table, items, sounds);
	io::for_each_config_line(
			text, size, tokens, [&](io::ConfigTokens &line) { return parse.line(line); });
	return table;
}

int32_t AmmoRecordTable::lookup_by_name(const char *name) const {
	for (int32_t i = 0; i < capacity; ++i) {
		const char *row_name = reinterpret_cast<const char *>(
				bytes.data() + static_cast<size_t>(i) * kAmmoRecordSize + 144);
		if (strutil::iequals(name, std::string(row_name, strnlen(row_name, kAmmoRecordSize - 144))))
			return i;
	}
	return 0;
}

bool AmmoRecordTable::row_crc(int32_t index, uint32_t &crc) const {
	if (index < 0 || index >= capacity)
		return false;
	uint8_t image[kAmmoRecordSize];
	std::memcpy(
			image, bytes.data() + static_cast<size_t>(index) * kAmmoRecordSize, kAmmoRecordSize);
	for (const size_t off : { 64u, 68u, 72u, 76u, 104u, 112u })
		io::write_u32_le(image + off, 0);
	crc = io::crc32_mpeg2(image, kAmmoRecordSize);
	return true;
}

} // namespace opennova::inmatch
