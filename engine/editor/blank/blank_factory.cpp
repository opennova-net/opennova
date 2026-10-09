#include <editor/blank/blank_factory.h>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>

#include "blank_makers.h"

#include <editor/assets/asset_kinds.h>
#include <editor/project/project_files.h>
#include <formats/configfile/config_file.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

namespace {

// What a new mission asks: the name its header carries (its file's stem when left out), and the
// terrain and the environment it loads, each a file of the project (a mission with neither loads
// nothing).
const BlankParam k_mission_params[] = {
	{ "title", "Title", ReferenceKind::None, false },
	{ "terrain", "Terrain", ReferenceKind::Terrain, true },
	{ "environment", "Environment", ReferenceKind::Environment, true },
};
const BlankParam k_mission_text_params[] = {
	{ "title", "Title", ReferenceKind::None, false },
};
// A new shader: the tag it registers under, which a model's material names (blank_shader_tags).
const BlankParam k_shader_params[] = {
	{ "tag", "Shader tag (VS_PHONGT, VS_DOT3DIFF2, VS_SKBUMPPHONGT or VS_SKBUMPDIFFT)", ReferenceKind::None, true },
};

// A terrain made from images (S20): its images (files on disk, or of the project) and the importer's
// options, the new_terrain request's values.
const BlankParam k_terrain_params[] = {
	{ "heightmap", "Heightmap (1024 x 1024 PNG or .raw)", ReferenceKind::None, true },
	{ "colormap", "Colour map (1024 x 1024 image)", ReferenceKind::None, true },
	{ "detail", "Detail (optional, power-of-two image)", ReferenceKind::None, false },
	{ "tiles", "Tile set (optional, sides x64)", ReferenceKind::None, false },
	{ "surface", "Surface map (optional, square 256..1024)", ReferenceKind::None, false },
	{ "foliagemap", "Foliage map (optional, square, indexed or grey: its codes)", ReferenceKind::None, false },
	{ "foliage", "Foliage (graphic <model> match <codes>, | between)", ReferenceKind::None, false },
	{ "top", "Height of white (world units, 127.5)", ReferenceKind::None, false },
	{ "water", "Water level (world units, 0 none)", ReferenceKind::None, false },
	{ "layout", "Layout (island or tiled)", ReferenceKind::None, false },
};

const BlankFactory k_factories[] = {
	// Boot: the string tables and definition files Game_InitSubsystems demands.
	{ "gameerr", AssetKind::Strings, make_blank_empty_strings, "an empty error-message table", false },
	{ "gametext", AssetKind::Strings, make_blank_gametext,
	  "the in-game string table with the sections the game reads, empty", false },
	{ "vmacros", AssetKind::Strings, make_blank_empty_strings, "an empty voice-macro table", false },
	{ "keyhelp", AssetKind::Strings, make_blank_empty_strings, "an empty key-help table", false },
	{ "weapon_def", AssetKind::WeaponDefs, make_blank_weapon_def, "a weapon table with no weapons", true },
	{ "sndprof_def", AssetKind::SoundProfileDefs, make_blank_sound_profiles,
	  "the sound profiles: one \"default\" profile, every slot silent", true },
	{ "items_def", AssetKind::ItemDefs, make_blank_items_def, "an item table holding only the Null marker", true },
	{ "charattr_def", AssetKind::CharAttrDefs, make_blank_charattr_def,
	  "a character-attribute file with no classes", true },
	// Menu: the tables, the stylesheet, the startup screen and the seven fonts.
	{ "game_bin", AssetKind::Strings, make_blank_empty_strings, "an empty menu string table", false },
	{ "menu_style", AssetKind::MenuStyle, make_blank_menu_style,
	  "the menu stylesheet naming the fonts and colors the screens use", true },
	{ "brand_style", AssetKind::MenuStyle, make_blank_brand_style,
	  "a brand stylesheet with no variables yet, read after the menu stylesheet", false },
	{ "nw_cdata", AssetKind::StringTableCoo, make_blank_coo, "an empty NovaWorld data table", true },
	{ "main_menu", AssetKind::Menu, make_blank_main_menu,
	  "the startup screen: the project's title, an Exit button and the game's mouse pointer", false },
	// The pointer every blank menu names (kBlankPointerRole), made with the menu.
	{ kBlankPointerRole, AssetKind::Texture, make_blank_pointer,
	  "the game's mouse pointer, a white arrow outlined in black, which the menus name", false },
	{ "menutxt", AssetKind::Strings, make_blank_menutxt,
	  "a menu label table holding the common navigation labels", false },
	{ "font_arial12b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial14n", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial14b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial16n", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial16b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_impac22b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_impac38b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	// Mission: what a mission's start and its screens read by name (docs/required-resources.md). Each
	// screen names the game's pointer, the one every shipped screen names.
	{ "ammo_def", AssetKind::AmmoDefs, make_blank_ammo_def, "an ammo table holding only the null round", true },
	{ "powerup_def", AssetKind::PowerupDefs, make_blank_powerup_def, "a powerup table with no powerups yet", true },
	{ "cmap_menu", AssetKind::Menu, make_blank_cmap_menu,
	  "the command map screen (CMAP) with its Close button, on Esc and on V, the key that opens it", false },
	{ "game_menu", AssetKind::Menu, make_blank_game_menu,
	  "the in-mission menu (INGAME): Resume, on Esc, and Leave Mission with its question", false },
	{ "weapon_menu", AssetKind::Menu, make_blank_weapon_menu,
	  "the armory screen (WEAPON) with its Cancel button, on Esc; no weapon slots yet", false },
	{ "vehicle_menu", AssetKind::Menu, make_blank_vehicle_menu,
	  "the vehicle loadout screen (VEHICLE) with its Cancel button, on Esc; no weapon list yet", false },
	{ "stat_menu", AssetKind::Menu, make_blank_stat_menu,
	  "the end-of-round screen (STAT): Leave Mission, on Esc, with its question; no results table yet", false },
	{ "death_menu", AssetKind::Menu, make_blank_death_menu,
	  "the deploy screen (DEATH): the spawn list the game fills, and Leave Mission with its question", false },
	{ "mp_menu", AssetKind::Menu, make_blank_mp_menu,
	  "the multiplayer screen the game comes back to (NW_MULTI_PLAYER) with its Back button, on Esc", false },
	{ "font_arials18", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial22", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_couri20b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "game_wac", AssetKind::Script, make_blank_script,
	  "an empty script, compiled ahead of server.wac and every mission's own", false },
	{ "server_wac", AssetKind::Script, make_blank_script,
	  "an empty script, compiled after game.wac and ahead of every mission's own", false },
	{ "loadscrn_pcx", AssetKind::Texture, make_blank_loading_screen,
	  "the checkerboard the game draws for a missing texture, 800 by 600: the loading screen's image", false },
	{ "monogram_tga", AssetKind::Texture, make_blank_monogram,
	  "the checkerboard the game draws for a missing texture, 512 by 256: the boards' watermark", false },
	{ "boxtile_tga", AssetKind::Texture, make_blank_boxtile,
	  "the checkerboard the game draws for a missing texture, 256 by 256: the boards' fill", false },
	{ "border_tga", AssetKind::Texture, make_blank_texture,
	  "the checkerboard the game draws for a missing texture, 128 by 128: the boards' border pieces", false },
	// The mission's sound banks, each slot of the bank loop that loads what it finds (the sound lane):
	// an empty bank, which the editor's bank document fills.
	{ "game_lwf", AssetKind::SoundBank, make_blank_sound_bank, "the global sound bank, no set yet", false },
	{ "gamelocl_lwf", AssetKind::SoundBank, make_blank_sound_bank, "the localized sound bank, no set yet", false },
	{ "game2_lwf", AssetKind::SoundBank, make_blank_sound_bank, "a further global sound bank, no set yet", false },
	{ "game3_lwf", AssetKind::SoundBank, make_blank_sound_bank, "a further global sound bank, no set yet", false },
	{ "expansion_lwf", AssetKind::SoundBank, make_blank_sound_bank, "the expansion's sound bank, no set yet", false },
	{ "expansion_locl_lwf", AssetKind::SoundBank, make_blank_sound_bank,
	  "the expansion's localized sound bank, no set yet", false },
	// DI-33: the rest of the files the game reads by name that the engine has a writer for, each the smallest file
	// its loader takes (the makers cite it). The shell's sound bank; the music pairs, each bank made with its script
	// (the game opens the bank first, and a bank of no stream must never be played from); the clips the shell and
	// the player preview plays; the boot's loading screen, the end screens' backdrops, the player preview's
	// environment cube; the celestial model the environment draws; the mission text a mission without its own
	// reads; the HUD layout and the avatars table, each as the game has it with none. Not failsafe.bad, which
	// retail ships none of: with it, a clip that does not load plays it in place of the slot's reset clip.
	{ "menu_lwf", AssetKind::SoundBank, make_blank_sound_bank,
	  "the shell's sound bank (the player profile's voice previews), no set yet", false },
	{ "menumus_sbf", AssetKind::MusicBank, make_blank_music_bank,
	  "the shell's music bank, no stream yet, made with its script (MENUMUS.BIN), which plays none", false, nullptr, 0,
	  "menumus_bin" },
	{ "menumus_bin", AssetKind::MusicScript, make_blank_menu_music_script,
	  "the shell's music script: one section that plays no stream", false },
	{ "gamemus_sbf", AssetKind::MusicBank, make_blank_music_bank,
	  "a mission's music bank, no stream yet, made with its script (GAMEMUS.BIN), which plays none", false, nullptr, 0,
	  "gamemus_bin" },
	{ "gamemus_bin", AssetKind::MusicScript, make_blank_game_music_script,
	  "a mission's music script: one section that plays no stream, and the handler the round's end runs", false },
	{ "pi_idle_bad", AssetKind::Animation, make_blank_animation,
	  "the player preview's idle clip: one bone at rest", false },
	{ "dt1rst_bad", AssetKind::Animation, make_blank_animation,
	  "the player preview's rest pose: one bone at rest", false },
	{ "loading_pcx", AssetKind::Texture, make_blank_loading_screen,
	  "the checkerboard the game draws for a missing texture, 800 by 600: the boot's loading screen", false },
	{ "jo_epil_tga", AssetKind::Texture, make_blank_texture,
	  "the checkerboard the game draws for a missing texture: the single-player win screen's backdrop", false },
	{ "jo_epil2_tga", AssetKind::Texture, make_blank_texture,
	  "the checkerboard the game draws for a missing texture: the single-player lose screen's backdrop", false },
	{ "upl_3di", AssetKind::Model, make_blank_model,
	  "a model of one triangle, the celestial model a mission's environment may draw", false },
	{ "medmssn_bin", AssetKind::Strings, make_blank_empty_strings,
	  "an empty mission text table, which a mission without its own reads", false },
	{ "hwmcube_dds", AssetKind::Texture, make_blank_cube,
	  "the player preview's environment cube: six faces of the checkerboard the game draws for a missing texture", false },
	{ "hudpos_def", AssetKind::HudPosDefs, make_blank_hud_layout,
	  "the HUD layout with nothing moved: every element where the game puts it with no layout", false },
	{ "avatars_def", AssetKind::AvatarDefs, make_blank_avatars,
	  "an avatars table with no part, nationality or combo yet", false },
	// An expansion's own (ADR 0046 S16): its text table, naming it in the Mods list, and its version text.
	{ "expansion_table", AssetKind::Strings, make_blank_expansion_table,
	  "the expansion's text table: its name in the Mods list (the project's title) and an empty description", false },
	{ "expansion_version", AssetKind::Text, make_blank_expansion_version,
	  "the expansion's version text (the project's title), whose checksum a joiner must match", false },
	// Free-form: a new file of a kind whose required files are all specific (Create
	// menu, Create table, a font of another name, a missing texture's placeholder).
	{ "", AssetKind::Strings, make_blank_empty_strings, "an empty string table", true },
	{ "", AssetKind::Menu, make_blank_menu,
	  "a menu with one screen named after the file, empty but for the game's mouse pointer", true },
	{ "", AssetKind::Font, make_blank_font, "the built-in bitmap font", true },
	{ "", AssetKind::Texture, make_blank_texture,
	  "the checkerboard the game draws for a missing texture, 128 by 128 gray squares", true },
	// S14: a mission on the terrain and under the environment chosen, with no entity yet; the text
	// table made beside it (its title, an empty briefing); a script.
	{ "", AssetKind::Mission, make_blank_mission, "an empty mission on the terrain and under the environment chosen",
	  true, k_mission_params, sizeof(k_mission_params) / sizeof(k_mission_params[0]) },
	{ kBlankMissionTextRole, AssetKind::Strings, make_blank_mission_text,
	  "a mission's text table: its title and an empty briefing", false, k_mission_text_params,
	  sizeof(k_mission_text_params) / sizeof(k_mission_text_params[0]) },
	{ "", AssetKind::Script, make_blank_script, "an empty script", true },
	// A mission's tile placement (<mission>.til), which every shipped mission carries beside it.
	{ "", AssetKind::TileInfo, make_blank_tile_info, "a tile placement with no tile placed", true },
	// The renderer's own effect, which it opens by name as it starts: without it, and with no other
	// effect, no model draws (blank_shader.cpp); a new effect for one of the shader tags the base game's
	// models name.
	{ "ffp_shader", AssetKind::Shader, make_blank_ffp_shader,
	  "OpenNova's fixed-function effect, the renderer's twelve FF_ shader tags", false },
	{ "", AssetKind::Shader, make_blank_shader, "OpenNova's effect for the shader tag chosen", true, k_shader_params,
	  sizeof(k_shader_params) / sizeof(k_shader_params[0]) },
	// S20: an environment a mission can be made under (a terrain made from images has none).
	{ "", AssetKind::Environment, make_blank_environment,
	  "a daytime environment: noon light, sky and fog colours through the day, the stock cloud maps", true },
	{ "", AssetKind::SoundBank, make_blank_sound_bank, "a sound bank with no set yet", true },
	// DI-33: a new file of every other kind a reference names and the engine has a writer for.
	{ "", AssetKind::Model, make_blank_model,
	  "a model of one untextured triangle, a metre wide and a metre tall, standing on its origin, seen from both sides",
	  true },
	{ "", AssetKind::Animation, make_blank_animation, "a clip of one bone at rest, one looping frame", true },
	{ "", AssetKind::AnimationMap, make_blank_animation_map,
	  "an animation map of its anim_reset row alone, made with the clip it names (one bone at rest)", true },
	{ "", AssetKind::Wave, make_blank_wave, "a wave of one sample of silence, 16-bit mono at 22050 a second", true },
	{ "", AssetKind::DialogBank, make_blank_dialog_bank,
	  "a mission's dialog bank with no dialog yet, made with the sound bank of its name", true },
	{ "", AssetKind::Particles, make_blank_particles, "a particle file with no effect yet", true },
	{ "", AssetKind::Credits, make_blank_credits, "a credits roll of one line, the project's title", true },
	{ "", AssetKind::AiProfile, make_blank_ai_profile, "an AI profile of no type yet, its grammar in a comment", true },
};

const size_t k_factory_count = sizeof(k_factories) / sizeof(k_factories[0]);

} // namespace

const std::string &BlankRequest::value(std::string_view token) const {
	static const std::string none;
	for (const auto &entry : values)
		if (entry.first == token) return entry.second;
	return none;
}

bool blank_values_fit(const BlankFactory &factory, const BlankRequest &request, std::string &why) {
	std::string takes;
	for (size_t i = 0; i < factory.param_count; ++i) takes += std::string(i ? ", " : "") + factory.params[i].token;
	for (const auto &entry : request.values) {
		bool known = false;
		for (size_t i = 0; i < factory.param_count; ++i) known = known || entry.first == factory.params[i].token;
		if (known) continue;
		why = request.logical_name + " takes no value \"" + entry.first + "\"" +
		      (takes.empty() ? std::string(" (it takes none).") : " (it takes " + takes + ").");
		return false;
	}
	for (size_t i = 0; i < factory.param_count; ++i) {
		const BlankParam &param = factory.params[i];
		if (!param.required || !request.value(param.token).empty()) continue;
		why = request.logical_name + " needs its " + param.token + ".";
		return false;
	}
	return true;
}

size_t blank_factory_count() {
	return k_factory_count;
}

const BlankParam *new_file_params(AssetKind kind, size_t &count, bool *offered) {
	count = 0;
	if (offered) *offered = false;
	if (kind == AssetKind::Terrain) {
		count = sizeof(k_terrain_params) / sizeof(k_terrain_params[0]);
		if (offered) *offered = true;
		return k_terrain_params;
	}
	// The kind's free-form factory of no role: what Files' New lists under its kind's label.
	for (const BlankFactory &factory : k_factories)
		if (factory.kind == kind && factory.free_form && factory.role[0] == '\0') {
			if (offered) *offered = true;
			count = factory.param_count;
			return factory.params;
		}
	return nullptr;
}

const BlankFactory *blank_factory_at(size_t index) {
	return index < k_factory_count ? &k_factories[index] : nullptr;
}

const BlankFactory *find_blank_factory_for_role(std::string_view role) {
	if (role.empty()) return nullptr;
	for (const BlankFactory &factory : k_factories) {
		if (role == factory.role) return &factory;
	}
	return nullptr;
}

const BlankFactory *find_blank_factory_for_kind(AssetKind kind) {
	for (const BlankFactory &factory : k_factories) {
		if (factory.kind == kind && factory.free_form) return &factory;
	}
	return nullptr;
}

const BlankFactory *find_blank_factory(std::string_view role, const std::string &logical_name, AssetKind kind) {
	if (const BlankFactory *factory = find_blank_factory_for_role(role)) return factory;
	if (kind == AssetKind::Texture && strutil::iequals(logical_name, blank_pointer_name()))
		return find_blank_factory_for_role(kBlankPointerRole);
	return find_blank_factory_for_kind(kind);
}

const BlankFactory *blank_companion(const BlankFactory &factory, const std::string &made, const ProjectDocument &doc,
                                    std::string &name) {
	if (factory.companion && *factory.companion) {
		const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_by_role(factory.companion);
		const BlankFactory *companion = find_blank_factory_for_role(factory.companion);
		if (!row || !companion) return nullptr;
		name = row->name;
		return companion;
	}
	if (factory.kind == AssetKind::AnimationMap) {
		name = blank_reset_clip_name(made);
		return find_blank_factory_for_kind(AssetKind::Animation);
	}
	// A dialog bank's sounds are the sound bank of its name (mission::dialog_sounds_name, the name cut at its first
	// dot), which the game opens as it loads the dialog bank [orig: DialogManager_LoadFromFile @ 0x44e650, <base>.lwf
	// @ 0x44e7d4..0x44e807]: made with it, empty.
	if (factory.kind == AssetKind::DialogBank) {
		name = mission::dialog_sounds_name(made);
		return find_blank_factory_for_kind(AssetKind::SoundBank);
	}
	if (factory.kind != AssetKind::Menu || !doc.expansion.standalone()) return nullptr;
	name = blank_pointer_name();
	return find_blank_factory_for_role(kBlankPointerRole);
}

bool make_blank(const BlankRequest &request, AssetKind kind, std::vector<uint8_t> &out,
                Diagnostic &error) {
	const BlankFactory *factory = find_blank_factory(request.role, request.logical_name, kind);
	if (factory == nullptr) {
		error = make_finding(CoreFinding::BlankUnavailable, DiagnosticSeverity::Error,
		                     "The editor cannot create " + request.logical_name +
		                             " yet: no writer exists for this kind of file.",
		                     request.logical_name);
		return false;
	}
	return make_from(*factory, request, out, error);
}

bool make_from(const BlankFactory &factory, const BlankRequest &request, std::vector<uint8_t> &out,
               Diagnostic &error) {
	if (!factory.make(request, out, error)) return false;
	if (file_kind_facts(factory.kind).line_reader != LineReader::ConfigFile) return true;
	const configfile::DataStringsPool pool = configfile::data_strings_pool(out.data(), out.size());
	if (pool.overrun() == 0) return true;
	error = make_finding(CoreFinding::DocumentConfigOverrun, DiagnosticSeverity::Error,
	                     request.logical_name + " is not made: it would hold " + std::to_string(pool.values) +
	                             " values against a " + std::to_string(pool.pool_bytes) +
	                             "-byte buffer of its text values, which the game's ConfigFile reader clears one "
	                             "byte per value, " +
	                             std::to_string(pool.overrun()) +
	                             " bytes past it into the game's memory (ConfigFile_ParseText @ 0x7609e8).",
	                     request.logical_name);
	out.clear();
	return false;
}

void blank_text_to_bytes(const std::string &text, std::vector<uint8_t> &out) {
	const std::string crlf = strutil::normalized_crlf_line_ends(text);
	out.assign(crlf.begin(), crlf.end());
}

} // namespace opennova::editor
