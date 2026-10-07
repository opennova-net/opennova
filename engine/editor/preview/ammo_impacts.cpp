#include <editor/preview/ammo_impacts.h>

#include <cstdio>

#include <editor/preview/mission_ground_facts.h>
#include <formats/def/def.h>
#include <formats/til/til_tsd.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/world/impact_scar.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

const char *tag_name(int tag) {
	return tag >= 0 && tag < world::kImpactEffectTagCount ? world::kImpactEffectTagNames[tag] : "";
}

std::string metres(float value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.4g m", double(value));
	return text;
}

// The ring scar a round of an ammo of `scar_type` leaves on an object's face of material `surface`: the kind gate
// (0 none, 2 the glass decal alone [orig: Impact_SpawnGlassEffectsOrScar @0x5cf1b0, `kind != 0` at entry, kind 2
// skipping the ring @0x5cf289]), the id by the face (15, glass, the hole [orig: @0x5CF295]), its textures (the id's
// strip range, one drawn at random [orig: Scar_TextureForId @0x5CC360]) and its radius [orig: Scar_RadiusForId
// @0x5CC3B0], world/impact_scar.h's.
void scar_of(int scar_type, int surface, AmmoImpactRow &row) {
	if (scar_type == world::kScarKindNone) {
		row.scar_words = "scar_type 0: no mark.";
		return;
	}
	if (!world::scar_kind_takes_ring_scar(scar_type)) {
		row.scar_words = "scar_type 2: no ring scar; only a building's glass point takes a decal (not drawn here).";
		return;
	}
	row.scar = world::scar_id_for_surface(surface);
	row.scar_radius = float(world::scar_radius_q16(row.scar)) / 65536.0f;
	if (row.scar == world::kScarIdGlassFallback) {
		row.scar_textures.push_back(world::scar_texture_strip_name(world::kScarGlassFallbackTextureStrip));
	} else {
		for (int strip = world::kScarNormalTextureFirst;
		     strip < world::kScarNormalTextureFirst + world::kScarNormalTextureCount; ++strip)
			row.scar_textures.push_back(world::scar_texture_strip_name(strip));
	}
	std::string textures;
	for (size_t i = 0; i < row.scar_textures.size(); ++i)
		textures += (i ? (i + 1 == row.scar_textures.size() ? " or " : ", ") : "") + row.scar_textures[i];
	row.scar_words = "On an object's face: " + textures + ", " + metres(row.scar_radius) + " radius (" +
	                 metres(row.scar_radius * 2.0f) + " across), spun at random about the face's normal.";
}

AmmoImpactRow row_of(const world::AmmoTable &table, const world::AmmoTableEntry &ammo, int surface, int tag) {
	AmmoImpactRow row;
	row.surface = surface;
	row.tag = tag;
	row.pick = world::impact_effect_row(table, ammo, tag);
	return row;
}

} // namespace

AmmoImpactBoard ammo_impact_board(const world::AmmoTable &table, const std::string &ammo) {
	AmmoImpactBoard board;
	board.ammo = ammo;
	board.index = table.index_of(ammo.c_str());
	const world::AmmoTableEntry *entry = table.by_index(board.index);
	if (!entry) return board;
	board.found = true;
	board.ammo = entry->name;
	if (const world::AmmoTableEntry *null_ammo = table.by_index(0)) board.null_ammo = null_ammo->name;
	board.null_rows = table.null_bank_rows > 0 ? table.null_bank_rows - 1 : 0;
	board.scar_type = entry->scar_type;
	board.scorch_id = entry->scorch_id;
	// The twenty classes the char map, a tile's table and a face's byte hold, each at its tag (the class + 4).
	for (int surface = 0; surface < TIL_TSD_SURFACE_NAME_COUNT; ++surface) {
		AmmoImpactRow row = row_of(table, *entry, surface, surface + 4);
		if (surface == 7) {
			// Underwater: the ocean past the mapped sectors, and the water handler's row where a round crosses the
			// plane first [orig: the water impact handler @ 0x4e9b80, tag 11 @0x4e9d2b].
			row.where = "On the terrain where the char map, a tile or the ocean past the map reads 7, wherever a round "
			            "crosses the water plane first (the water handler's row), and on an object's bullet face of "
			            "material 7.";
		} else {
			row.where = "On the terrain where the char map or a placed tile reads " + std::to_string(surface) +
			            ", and on an object's bullet face of material " + std::to_string(surface) + ".";
		}
		// The terrain and the water are never marked; an object's face is, by the ammo's kind.
		scar_of(board.scar_type, surface, row);
		board.surfaces.push_back(std::move(row));
	}
	// The rows no surface reaches, which the same presenter plays: the in-flight emitter's, the local player's hit,
	// a round passing the listener, a hit on body armour.
	{
		AmmoImpactRow move;
		move.tag = 1;
		move.pick = world::impact_effect_own_row(*entry, 1);
		move.where = "Riding the round in flight (its own row: the spawn attaches it directly).";
		board.others.push_back(std::move(move));
		AmmoImpactRow player = row_of(table, *entry, -1, 2);
		player.where = "A round striking the local player [orig: push 2 @0x4e9aa1].";
		board.others.push_back(std::move(player));
		AmmoImpactRow zip = row_of(table, *entry, -1, 3);
		zip.where = "A round passing the listener, its sound alone [orig: AmmoDef_ProcessImpactEffect(ammo, 3) @0x4e5c20].";
		board.others.push_back(std::move(zip));
		AmmoImpactRow armor = row_of(table, *entry, -1, 24);
		armor.where = "A hit on a person carrying body armour, beside the flesh row [orig: @0x4e99f8..0x4e9a38].";
		board.others.push_back(std::move(armor));
	}
	return board;
}

std::string ammo_impact_from_words(const AmmoImpactBoard &board, const AmmoImpactRow &row) {
	const std::string tag = tag_name(row.tag);
	if (row.pick.from == world::ImpactRowFrom::Own)
		return board.ammo + "'s own " + tag + " row" +
		       (row.pick.effect.empty() && row.pick.sound.empty() ? " (authored none: nothing plays)." : ".");
	// The fallback: no row of the tag, so the presenter reads ammo def 0's static bank at the tag's place.
	std::string out = board.ammo + " authors no " + tag + " row: the game plays " +
	                  (board.null_ammo.empty() ? std::string("ammo def 0") : board.null_ammo) + "'s bank at place " +
	                  std::to_string(row.pick.tag);
	if (row.pick.bank_tag == 0)
		return out + ", which holds no row (" + std::to_string(board.null_rows) + " authored): nothing plays.";
	out += ", its " + std::string(tag_name(row.pick.bank_tag)) + " row";
	if (row.pick.bank_tag != row.pick.tag) out += " (not its " + tag + " row: the bank is packed in tag order)";
	return out + ".";
}

io::JsonValue ammo_impact_board_to_json(const AmmoImpactBoard &board) {
	JsonValue out = JsonValue::make_object();
	out.set("ammo", json_string(board.ammo));
	out.set("found", JsonValue::make_bool(board.found));
	out.set("index", json_number(board.index));
	out.set("null_ammo", json_string(board.null_ammo));
	out.set("null_rows", json_number(board.null_rows));
	out.set("scar_type", json_number(board.scar_type));
	out.set("scorch_id", json_number(board.scorch_id));
	out.set("rule", json_string("A tag the ammo authors no row of plays ammo def 0's static bank at the tag's place "
	                            "[orig: AmmoDef_ProcessImpactEffect @0x40a1b8..0x40a1fd]; a tag past the table, obj's."));
	const auto rows = [&](const std::vector<AmmoImpactRow> &of) {
		JsonValue array = JsonValue::make_array();
		for (const AmmoImpactRow &row : of) {
			JsonValue each = JsonValue::make_object();
			if (row.surface >= 0) {
				each.set("surface", json_number(row.surface));
				each.set("name", json_string(mission_surface_name(row.surface)));
				each.set("words", json_string(mission_surface_words(row.surface)));
			}
			each.set("tag", json_number(row.tag));
			each.set("row", json_string(tag_name(row.tag)));
			each.set("from", json_string(row.pick.from == world::ImpactRowFrom::Own ? "own" : "bank"));
			if (row.pick.from == world::ImpactRowFrom::NullBank) {
				each.set("bank_tag", json_number(row.pick.bank_tag));
				each.set("bank_row", json_string(tag_name(row.pick.bank_tag)));
			}
			each.set("effect", json_string(row.pick.effect));
			each.set("sound", json_string(row.pick.sound));
			each.set("played", JsonValue::make_bool(!row.pick.effect.empty() || !row.pick.sound.empty()));
			JsonValue scar = JsonValue::make_object();
			scar.set("id", json_number(row.scar));
			JsonValue textures = JsonValue::make_array();
			for (const std::string &texture : row.scar_textures) textures.push(json_string(texture));
			scar.set("textures", std::move(textures));
			scar.set("radius", json_number(row.scar_radius));
			scar.set("words", json_string(row.scar_words));
			each.set("scar", std::move(scar));
			each.set("where", json_string(row.where));
			each.set("from_words", json_string(ammo_impact_from_words(board, row)));
			array.push(std::move(each));
		}
		return array;
	};
	out.set("surfaces", rows(board.surfaces));
	out.set("others", rows(board.others));
	return out;
}

bool AmmoTableSource::refresh(const std::shared_ptr<const FileSource> &files) {
	if (files == files_ && read_ && files && !reads_.moved(*files)) return false;
	files_ = files;
	reads_.clear();
	table_ = world::AmmoTable();
	why_.clear();
	read_ = true;
	if (!files) {
		why_ = "No project is open.";
		return true;
	}
	// ammo.def by the name the game loads it by [orig: AmmoDef_LoadAll @ 0x40B0B0].
	auto stamped = std::make_shared<StampedFiles>(files);
	std::vector<uint8_t> bytes;
	def::DefAmmoFile ammo{};
	if (!stamped->read("ammo.def", bytes) || bytes.empty()) {
		why_ = "The project has no ammo.def.";
	} else if (def::def_parse_ammo_memory(bytes.data(), bytes.size(), &ammo) != 0) {
		why_ = "ammo.def does not read as the game's ammo table.";
	} else {
		table_ = world::build_ammo_table(ammo);
		def::def_free_ammo(&ammo);
	}
	reads_ = stamped->stamps();
	return true;
}

} // namespace opennova::editor
