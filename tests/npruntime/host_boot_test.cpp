// The one host boot (ADR 0051 d4, inmatch/host_boot.h), ungated: the two
// phases over an in-memory mount (tests/common/boot_file_source.h source_over)
// carrying a synthetic .env, .til, .TSD, score.ini, gametext.bin and
// charattr.def, booting the two-entity mission as a dedicated host. It pins
// what the boot reads and where each table lands, and the order the fixes ride:
//   - the placed tiles and the .TSD table survive the boot's terrain wiring
//     (before, the boot's wire_terrain replaced the tiles the game stamped);
//   - the .env water plane stands before the bring-up and the PreMission pass
//     (before, both embedders stamped it after the boot);
//   - every host carries the gametext "Server" strings (D-NET-344) and the
//     charattr.def class attributes (D-NET-345);
//   - phase B completes the mission start and only then the session's load,
//     and the 0x0A view distance is the world's fog distance (D-NET-139);
//   - a set mpreset word refuses the session create and phase A reports the
//     process exit to the embedder (D-NET-385);
//   - the host's session config carries the anti-cheat sources the boot
//     computed from the mount (D-NET-181).
#include <runtime/inmatch/host_boot.h>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>

#include <base/gameprofile/game_type.h>
#include <formats/cpt/cpt.h>
#include <formats/mission/bms.h>
#include <formats/rtxt/rtxt.h>
#include <formats/til/til.h>
#include <formats/til/til_io.h>
#include <formats/trn/trn.h>
#include <runtime/inmatch/integrity_ammo_image.h>
#include <runtime/inmatch/integrity_sources.h>
#include <runtime/inmatch/local_role.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/world/world.h>

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

std::string bytes_string(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

// The mission's .til: one placed tile of palette index 7 at the origin cell.
std::string til_bytes() {
	TilFile til;
	TilOverlayEntry tile;
	tile.x_fixed = 4 << 16;
	tile.z_fixed = -(4 << 16);
	tile.tile_index = 7;
	til.entries.push_back(tile);
	std::vector<uint8_t> out;
	std::string error;
	if (!save_til(til, out, error)) std::printf("save_til: %s\n", error.c_str());
	return bytes_string(out);
}

// gametext.bin with the "Server" strings the host's handlers print through.
std::string gametext_bytes() {
	rtxt::File file;
	file.sections.push_back({"Menu", 0});
	file.sections.push_back({"Server", 0});
	file.entries.push_back({"UNTITLED", "Untitled", {}, 0});
	file.entries.push_back({"STRSRV_MEDREQ", "%s needs a medic", {}, 1});
	file.entries.push_back({"C2Blue", "%s joined blue", {}, 1});
	file.entries.push_back({"C2Red", "%s joined red", {}, 1});
	file.normalize_grouping();
	std::vector<uint8_t> out;
	std::string error;
	if (!rtxt::write(file, out, error)) std::printf("rtxt::write: %s\n", error.c_str());
	return bytes_string(out);
}

// A mission text whose [Info] carries one Title entry.
std::string mission_text_bytes(const char *title) {
	rtxt::File file;
	file.sections.push_back({"Info", 0});
	file.entries.push_back({"Title", title, {}, 0});
	file.normalize_grouping();
	std::vector<uint8_t> out;
	std::string error;
	if (!rtxt::write(file, out, error)) std::printf("rtxt::write: %s\n", error.c_str());
	return bytes_string(out);
}

std::map<std::string, std::string> mount() {
	std::map<std::string, std::string> files;
	files["synth.til"] = til_bytes();
	files["synth.env"] = "enviro_name \"Synth\"\r\nwater_height 40\r\nfog_level 900\r\n";
	files["synthstrip.tsd"] = "INDEX_7 TSD_SNOW\r\n";
	files["score.ini"] = "VERSION 40\r\nGAMETYPE \"DM\"\r\nFIELD \"NUMENEMYKILLS\" 1\r\n"
	                     "VAR \"FIRE\" 7\r\nVAR \"ENEMYKILL\" 9\r\n";
	files["gametext.bin"] = gametext_bytes();
	files["charattr.def"] = "[CHARACTER1]\r\nATTRIBUTES\t= AutoScope\r\n"
	                        "[CHARACTER2]\r\nATTRIBUTES\t= Medic\r\n";
	return files;
}

bms::File deathmatch_mission() {
	bms::File m = test_mission::two_entity_mission();
	m.header.attrib_flags = bms::AttribFlags::Deathmatch;
	std::snprintf(m.header.environment, sizeof(m.header.environment), "synth");
	return m;
}

// The game's terrain entry: the embedder builds the store from its parsed
// documents before the boot (TerrainData in the game).
void build_terrain(mission::MissionKernel &kernel, int trn_water_raw) {
	CptFile cpt;
	cpt.depth_buffer.assign(64, 0);
	TrnConfig trn;
	for (int r = 0; r < 16; ++r)
		for (int c = 0; c < 16; ++c) trn.sector_grid[r][c] = 1;
	trn.tilestrip = "synthstrip.tga";
	trn.water_height = trn_water_raw;
	std::vector<uint8_t> charmap(16, 2);
	terrain::terrain_field_store_build(kernel.terrain_store, cpt, trn, charmap.data(), 4, 4);
}

struct Host {
	std::map<std::string, std::string> files = mount();
	std::unique_ptr<mission::MissionKernel> kernel;
	inmatch::HostRole role{inmatch::RoleKind::DedicatedHost};
	inmatch::Session session{role};
	inmatch::HostBoot boot;
	int32_t water_at_bringup = -1;
	bool mission_start_pending_at_bringup = false;

	inmatch::HostBootRequest request(int trn_water_raw) {
		inmatch::HostBootRequest r;
		r.host_cfg.title = &kJo;
		r.mission = deathmatch_mission();
		r.mission_basename = "synth";
		r.files = test_boot::source_over(&files);
		r.session = &session;
		r.role = &role;
		r.host = &role;
		r.host_cfg.config.game_type = game_type::kDeathmatch;
		r.host_cfg.config.max_players = 9;
		r.host_cfg.serve_and_play = false;
		r.session_score_ini = true;
		r.boot_options.playable = false;
		r.boot_options.mp_session = true;
		r.boot_options.terrain = false;
		r.boot_options.game_type = game_type::kDeathmatch;
		r.boot_options.player_limit = 9;
		r.fresh_kernel = [this]() -> mission::MissionKernel & {
			kernel = std::make_unique<mission::MissionKernel>();
			return *kernel;
		};
		r.before_terrain = [trn_water_raw](mission::MissionKernel &k) {
			build_terrain(k, trn_water_raw);
		};
		// The bring-up runs inside the boot, ahead of the PreMission pass.
		r.after_bringup = [this](bool) {
			water_at_bringup = kernel->world.env.water_z;
			mission_start_pending_at_bringup = kernel->mission_start_pending;
		};
		return r;
	}
};

} // namespace

int main() {
	// --- phase A: what the boot reads and where it lands ---------------------
	{
		Host host;
		std::string error;
		CHECK(inmatch::boot_host_mission(host.request(0), host.boot, error));
		CHECK(error.empty());
		if (!host.kernel) return (std::printf("FAIL no kernel: %s\n", error.c_str()), 1);
		mission::MissionKernel &kernel = *host.kernel;
		const inmatch::NapiNPServerCtx &ctx = host.role.state.host_owner.ctx;
		CHECK(host.boot.kernel == &kernel);
		// The session holds its load across the device stages.
		CHECK(host.session.state() == inmatch::State::Loading);
		CHECK(kernel.mission_start_pending);

		// score.ini reached world.match through the staged config.
		CHECK(kernel.world.match.rules().score_values.has_value());
		CHECK(kernel.world.match.rules().score_values &&
				(*kernel.world.match.rules().score_values)[0] == 7);
		CHECK(kernel.world.match.rules().score_values &&
				(*kernel.world.match.rules().score_values)[3] == 9);
		CHECK(kernel.world.match.rules().score_fields.size() == 1);
		CHECK(ctx.config.max_players == 9u);

		// The .til reached the S2C 0x45 source and the surface walk; the placed
		// tiles and the .TSD table survived the boot's terrain wiring.
		CHECK(bytes_string(ctx.terrain_til_data) == host.files["synth.til"]);
		CHECK(kernel.terrain_store.placed_tiles().size() == 1);
		CHECK(kernel.world.tables.surface_map.tiles != nullptr);
		CHECK(kernel.world.tables.surface_map.tile_count == 1);
		CHECK(kernel.world.tables.surface_map.tile_surface != nullptr &&
				kernel.world.tables.surface_map.tile_surface[7] == 3); // TSD_SNOW
		CHECK(kernel.world.tables.surface_map.data != nullptr); // the charmap rides too

		// The .env water (40 half-units = 20 world units) stood before the
		// bring-up, which the boot runs ahead of the PreMission pass.
		CHECK(host.boot.water_z_q16 == (20 << 16));
		CHECK(host.water_at_bringup == (20 << 16));
		CHECK(host.mission_start_pending_at_bringup == false); // the boot had not ended yet
		CHECK(kernel.world.env.water_z == (20 << 16));
		CHECK(kernel.terrain_store.height_field().has_water);

		// D-NET-344: the dedicated host carries the "Server" strings.
		CHECK(ctx.server_text.medic_request_format == "%s needs a medic");
		CHECK(ctx.server_text.change_to_blue_format == "%s joined blue");
		CHECK(ctx.server_text.change_to_red_format == "%s joined red");
		// D-NET-345: the authority's class attributes came from charattr.def.
		CHECK(host.boot.charattr.loaded);
		CHECK(kernel.world.tables.class_has_attribute(2, world::MissionTables::kCharAttrMedic));
		CHECK(!kernel.world.tables.class_has_attribute(1, world::MissionTables::kCharAttrMedic));
		CHECK(kernel.world.tables.class_attribute_flags[0] == 0x1u); // CHARACTER1: AutoScope

		// Serve Only: no player of the host's own, no local client.
		CHECK(!host.role.state.host_owner.serve_and_play);
		CHECK(host.role.client_runtime() == nullptr);
		CHECK(!kernel.local.has_local_player());
		CHECK(kernel.world.rules.mp_session);

		// --- phase B: the mission start, then the load's end -----------------
		CHECK(inmatch::start_host_mission(host.boot, inmatch::HostStartDevice{}, error));
		CHECK(error.empty());
		CHECK(!kernel.mission_start_pending);
		CHECK(kernel.have_baseline);
		CHECK(host.session.state() == inmatch::State::Running);
		CHECK(kernel.world.env.water_z == (20 << 16));
		// D-NET-139: the 0x0A priority score's view distance is this world's
		// fog distance (the .env fog_level 900, settled by the 255 ticks).
		CHECK(replication::view_distance_units(kernel.world) == 900);
		// The placed tiles are still on the view the footsteps read.
		CHECK(kernel.world.tables.surface_map.tiles != nullptr);

		// The presentation half the headless host discards.
		kernel.world.out.weather_sounds.push_back(world::WeatherSoundEvent{0x10000, 0});
		kernel.world.out.tip_events.push_back(1);
		kernel.world.out.terrain_scorches.emit_page_invalidation(0, 0, 0x10000);
		kernel.world.out.discard_presentation();
		CHECK(kernel.world.out.weather_sounds.empty());
		CHECK(kernel.world.out.tip_events.empty());
		CHECK(kernel.world.out.terrain_scorches.pending_page_invalidations().empty());
	}

	// --- the host's character table (D-NET-137): with none installed by the
	// embedder, the boot takes the mounted Avatars.def, the file g_AvatarDefs
	// loads, so a joiner's add validates against it; an embedder's table stands.
	// [orig: CAvatarDefs_Init @0x57B180 - File_ParseASCIIFile("Avatars.def") @0x57B1D5]
	{
		const std::string avatars =
				"define head H\r\n{\r\n\tgraphic h.3di\r\n\tcamo 0 0 0\r\n\tvoice 1\r\n\tsex m\r\n}\r\n"
				"define body B\r\n{\r\n\tgraphic b.3di\r\n\tcamo 0 0 0\r\n}\r\n"
				"define arms A\r\n{\r\n\tgraphic a.3di\r\n\tcamo 0 0 0\r\n}\r\n"
				"nationality 0 N\r\n{\r\n\talignment good\r\n\tdivision 0 D\r\n\t{\r\n"
				"\t\tcombo 1 H B A\r\n\t}\r\n}\r\n";
		Host host;
		host.files["Avatars.def"] = avatars;
		std::string error;
		CHECK(inmatch::boot_host_mission(host.request(0), host.boot, error));
		const inmatch::CharacterRegistry &mounted = host.role.state.host_owner.ctx.characters;
		CHECK(mounted.entries().size() == 1);
		CHECK(mounted.find_by_packed_id(mounted.first_character_id(0)) != nullptr);

		Host installed;
		installed.files["Avatars.def"] = avatars;
		inmatch::HostBootRequest r = installed.request(0);
		r.host_cfg.characters.add_entry(1, 0, 0, 7, 0, 1, 1, 4, false); // 0x8207
		r.host_cfg.characters.add_entry(1, 0, 1, 7, 0, 2, 1, 9, false); // 0x8407
		CHECK(inmatch::boot_host_mission(std::move(r), installed.boot, error));
		CHECK(installed.role.state.host_owner.ctx.characters.entries().size() == 2);
	}

	// --- the mission text and the mount's override table (D-NET-657): every
	// lookup of the mission's table searches the expansion's override table
	// first, so its briefing page reaches the S2C 0x7E source and its
	// location text the names, and the mission's own answers the rest.
	// [orig: TextResource_FindEntryBySectionAndKey @0x75D250, @0x75D27B;
	//  MissionText_GetString @0x51EB50; NetPacket_WriteBriefingText @0x506620]
	{
		// Section 0 [info], section 1 [Locations].
		const auto table = [](const std::vector<rtxt::Entry> &rows) {
			rtxt::File file;
			file.sections.push_back({"info", 0});
			file.sections.push_back({"Locations", 0});
			for (const rtxt::Entry &row : rows) {
				file.entries.push_back(row);
				++file.sections[row.section_index].string_count;
			}
			file.normalize_grouping();
			std::vector<uint8_t> out;
			std::string error;
			if (!rtxt::write(file, out, error)) std::printf("rtxt::write: %s\n", error.c_str());
			return out;
		};
		Host host;
		const std::vector<uint8_t> mission_bin = table({{"briefing3", "Mission three", {}, 0},
				{"briefing2", "Mission two", {}, 0}, {"LOCATION001", "Harbor", {}, 1},
				{"LOCATION002", "Ridge", {}, 1}});
		host.files["synth.bin"] = bytes_string(mission_bin);
		inmatch::HostBootRequest r = host.request(0);
		r.text_override_table = table({{"BRIEFING3", "Override three", {}, 0},
				{"location002", "Override ridge", {}, 1}});
		std::string error;
		CHECK(inmatch::boot_host_mission(std::move(r), host.boot, error));
		const inmatch::NapiNPServerCtx &ctx = host.role.state.host_owner.ctx;
		CHECK(host.boot.mission_text.loaded && host.boot.mission_text.override_table != nullptr);
		CHECK(ctx.mission_text_loaded);
		CHECK(ctx.mission_briefing3 == "Override three");
		CHECK(ctx.mission_briefing2 == "Mission two");
		CHECK(host.boot.mission_text.location_name(1) == "Harbor");
		CHECK(host.boot.mission_text.location_name(2) == "Override ridge");

		// No override table: the mission's own text.
		Host plain;
		plain.files["synth.bin"] = bytes_string(mission_bin);
		CHECK(inmatch::boot_host_mission(plain.request(0), plain.boot, error));
		CHECK(plain.boot.mission_text.override_table == nullptr);
		CHECK(plain.role.state.host_owner.ctx.mission_briefing3 == "Mission three");
		CHECK(plain.boot.mission_text.location_name(2) == "Ridge");
	}

	// --- the water rungs (env #44): the .env's line writes after the .trn's,
	// so the .env's height (40 half units) beats the terrain's (30); with no
	// .env line the terrain's stands -----------------------------------------
	{
		Host host;
		std::string error;
		CHECK(inmatch::boot_host_mission(host.request(/*trn_water_raw=*/30), host.boot, error));
		CHECK(host.boot.water_z_q16 == (20 << 16));
		CHECK(host.water_at_bringup == (20 << 16));
	}
	{
		Host host;
		host.files["synth.env"] = "enviro_name \"Synth\"\r\nfog_level 900\r\n";
		std::string error;
		CHECK(inmatch::boot_host_mission(host.request(/*trn_water_raw=*/30), host.boot, error));
		CHECK(host.boot.water_z_q16 == (15 << 16));
		CHECK(host.water_at_bringup == (15 << 16));
	}

	// --- the terrain's pass (env #43): the mission's .trn reads ahead of its
	// .env, so its murk and height stand where the .env writes neither --------
	{
		Host host;
		host.files["synth.env"] = "enviro_name \"Synth\"\r\nfog_level 900\r\n";
		host.files["synth.trn"] = "terrain_name \"synth\"\r\nwater_height 24\r\nwater_murk .3\r\n";
		inmatch::HostBootRequest request = host.request(/*trn_water_raw=*/0);
		std::snprintf(request.mission.header.terrain, sizeof(request.mission.header.terrain), "synth");
		std::string error;
		CHECK(inmatch::boot_host_mission(std::move(request), host.boot, error));
		CHECK(host.boot.env_config.water_height_set && host.boot.env_config.water_height == 24.0f);
		CHECK(host.boot.env_config.water_murk > 0.299f && host.boot.env_config.water_murk < 0.301f);
		CHECK(host.boot.water_z_q16 == (12 << 16));
	}

	// --- the authority's tile fallback: no <mission>.til, so the terrain's own
	// polytrn_tileinfo, its extension forced from the first '.', and the
	// mission's name cut at its FIRST '.' for its own -----------------------
	// [orig: PolyTrn_LoadTerrainConfig @0x60E6C9..0x60E6E5; Terrain_Init
	//  @0x60FCFD; Path_ReplaceOrAppendExtension @0x53C7C4]
	{
		Host host;
		host.files["synth.trn"] = "terrain_name \"synth\"\r\npolytrn_tileinfo synthown.v1.dat\r\n";
		host.files["synthown.til"] = host.files["synth.til"];
		host.files.erase("synth.til");
		inmatch::HostBootRequest request = host.request(/*trn_water_raw=*/0);
		std::snprintf(request.mission.header.terrain, sizeof(request.mission.header.terrain), "synth");
		std::string error;
		CHECK(inmatch::boot_host_mission(std::move(request), host.boot, error));
		CHECK(bytes_string(host.boot.terrain_til) == host.files["synthown.til"]);
		CHECK(host.kernel && host.kernel->terrain_store.placed_tiles().size() == 1);

		// A first-dot base finds its own .til ahead of the terrain's.
		Host dotted;
		dotted.files["synth.trn"] = host.files["synth.trn"];
		dotted.files["synthown.til"] = std::string(16, '\0'); // refused: no magic
		inmatch::HostBootRequest r = dotted.request(0);
		r.mission_basename = "synth.v2";
		std::snprintf(r.mission.header.terrain, sizeof(r.mission.header.terrain), "synth");
		CHECK(inmatch::boot_host_mission(std::move(r), dotted.boot, error));
		CHECK(bytes_string(dotted.boot.terrain_til) == dotted.files["synth.til"]);
	}

	// --- D-NET-374: the session's mp_No* words restrict the class table ------
	// Each mission start on the authority zeroes and disables the property each
	// set word names; the table and its latches are the boot's, kept for the
	// next mission, whose session without the word restores nothing.
	// [orig: Server_ResetRoundCounters @0x4FCF10 (CharAttr_ApplyMpRestrictions
	//  @0x4247D0's body), CharAttr_PackDisabledProperties @0x412550]
	{
		Host host;
		std::string error;
		inmatch::HostBootRequest r = host.request(0);
		r.host_cfg.config.no_char_abilities = 1;
		r.host_cfg.config.no_scope_drift = 1;
		CHECK(inmatch::boot_host_mission(std::move(r), host.boot, error));
		CHECK(host.kernel != nullptr);
		if (host.kernel) {
			const world::World &w = host.kernel->world;
			CHECK(host.boot.charattr.loaded && host.boot.charattr_read);
			CHECK(!w.tables.class_has_attribute(2, world::MissionTables::kCharAttrMedic));
			CHECK(w.tables.class_attribute_flags[0] == 0u);
			CHECK(w.tables.charattr_disabled_word == 0x0012u); // bit 1 ATTRIBUTES, bit 4 SCOPE_MUTE
			CHECK(host.boot.charattr.table.rows[1].attributes == 0u);
		}
		// A boot over the kept table (an embedder reuses its HostBoot from mission to
		// mission), its session without the words: the table is not read again and the
		// latches stay.
		Host next;
		next.boot.charattr = host.boot.charattr;
		next.boot.charattr_read = true;
		next.files.erase("charattr.def");
		CHECK(inmatch::boot_host_mission(next.request(0), next.boot, error));
		if (next.kernel) {
			CHECK(next.kernel->world.tables.charattr_disabled_word == 0x0012u);
			CHECK(!next.kernel->world.tables.class_has_attribute(2, world::MissionTables::kCharAttrMedic));
			CHECK(next.boot.charattr.loaded);
		}
	}

	// --- D-NET-385: the session create's mpreset exit, reported to the
	// embedder. A set word refuses the create inside the kernel boot's bring-up:
	// phase A returns false with the exit outcome, no session stands, and the
	// session stays Loading for the embedder, which ends its process with code
	// 0 and runs no phase B. A clear word boots as ever. The dedicated host
	// first, then a listen host (serve-and-play), whose clear boot registers
	// its own type-2 loopback connection and builds its HostClient view: with
	// the word set the role's bring-up returns at the refused create, before
	// either.
	// [orig: CNapiGameSession_CreateSession @0x4C97E7..0x4C97F0 -> crt_exit(0)]
	{
		Host host;
		std::string error;
		inmatch::HostBootRequest r = host.request(0);
		r.host_cfg.config.multiplayer_reset = 1;
		CHECK(!inmatch::boot_host_mission(std::move(r), host.boot, error));
		CHECK(host.boot.session_create == inmatch::CreateSessionResult::ProcessExit);
		CHECK(host.role.session_create() == inmatch::CreateSessionResult::ProcessExit);
		CHECK(error.find("mpreset") != std::string::npos);
		const inmatch::NapiNPServerCtx &ctx = host.role.state.host_owner.ctx;
		CHECK(ctx.is_in_session == 0);
		CHECK(ctx.np_protocol.host_running == 0);
		CHECK(host.session.state() == inmatch::State::Loading);

		Host clear;
		CHECK(inmatch::boot_host_mission(clear.request(0), clear.boot, error));
		CHECK(clear.boot.session_create == inmatch::CreateSessionResult::Created);
		CHECK(clear.role.state.host_owner.ctx.is_in_session == 1);
		CHECK(clear.role.state.host_owner.ctx.np_protocol.host_running == 1);
	}
	{
		auto listen_request = [](Host &h, int32_t mpreset) {
			h.role.set_kind(inmatch::RoleKind::ListenHost);
			inmatch::HostBootRequest r = h.request(0);
			r.host_cfg.serve_and_play = true;
			r.host_cfg.config.multiplayer_reset = mpreset;
			return r;
		};
		auto loopbacks = [](const inmatch::NapiNPServerCtx &ctx) {
			int n = 0;
			for (const inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list)
				if (c.type == inmatch::NapiNPConnection::kTypeClientSide) ++n;
			return n;
		};
		std::string error;
		Host clear;
		CHECK(inmatch::boot_host_mission(listen_request(clear, 0), clear.boot, error));
		CHECK(clear.boot.session_create == inmatch::CreateSessionResult::Created);
		CHECK(clear.role.client_runtime() != nullptr);
		CHECK(loopbacks(clear.role.state.host_owner.ctx) == 1);
		CHECK(clear.role.state.host_owner.ctx.is_in_session == 1);

		Host reset;
		CHECK(!inmatch::boot_host_mission(listen_request(reset, 1), reset.boot, error));
		CHECK(reset.boot.session_create == inmatch::CreateSessionResult::ProcessExit);
		CHECK(reset.role.session_create() == inmatch::CreateSessionResult::ProcessExit);
		CHECK(reset.role.client_runtime() == nullptr);
		CHECK(loopbacks(reset.role.state.host_owner.ctx) == 0);
		CHECK(reset.role.state.host_owner.ctx.is_in_session == 0);
		CHECK(reset.role.state.host_owner.ctx.total_logins == 0);
		CHECK(reset.session.state() == inmatch::State::Loading);
	}

	// --- a role with no HostRole (the game's joiner) skips the authority's legs
	{
		std::map<std::string, std::string> files = mount();
		std::unique_ptr<mission::MissionKernel> kernel;
		inmatch::LocalRole role;
		inmatch::Session session(role);
		inmatch::HostBoot boot;
		inmatch::HostBootRequest r;
		r.host_cfg.title = &kJo;
		r.mission = deathmatch_mission();
		r.mission_basename = "synth";
		r.files = test_boot::source_over(&files);
		r.session = &session;
		r.role = &role;
		r.terrain_til = std::vector<uint8_t>{}; // the host's stream: no tiles
		r.session_score_ini = true;             // ignored without a host
		r.boot_options.playable = false;
		r.boot_options.terrain = false;
		r.fresh_kernel = [&]() -> mission::MissionKernel & {
			kernel = std::make_unique<mission::MissionKernel>();
			return *kernel;
		};
		std::string error;
		CHECK(inmatch::boot_host_mission(std::move(r), boot, error));
		CHECK(kernel != nullptr);
		if (kernel) {
			CHECK(!boot.charattr.loaded);
			CHECK(kernel->world.tables.class_attribute_flags[1] == 0u);
			CHECK(boot.server_text.medic_request_format.empty());
			CHECK(kernel->terrain_store.placed_tiles().empty());
			CHECK(!kernel->world.match.rules().score_values.has_value());
		}
	}

	// --- the anti-cheat challenge sources ------------------------------------
	// Every boot computes them from the mounted corpus (inmatch/integrity_sources.h)
	// and the host's session config carries them into the C2S 0x20/0x21
	// validation. This mount has no asset store, so the weapon table (whose
	// action bind reads .adm clips) stays unknown; without ammo.def nothing is
	// computed and the host validates nothing.
	{
		Host host;
		host.files["items.def"] = "begin ITEM_ZERO\r\nid 100000\r\nend\r\n";
		host.files["ammo.def"] = "ammo AT_NULL\r\nend\r\nammo AMMO_ONE\r\nvelocity 3\r\nend\r\n";
		std::string error;
		CHECK(inmatch::boot_host_mission(host.request(0), host.boot, error));
		const inmatch::IntegritySources &sources =
				host.role.state.host_owner.ctx.config.integrity_sources;
		const std::vector<uint8_t> ammo_text(
				host.files["ammo.def"].begin(), host.files["ammo.def"].end());
		io::ConfigTokens tokens;
		const inmatch::AmmoRecordTable ammo = inmatch::build_ammo_record_table(
				ammo_text.data(), ammo_text.size(), {{"ITEM_ZERO", 0}}, nullptr, tokens);
		uint32_t row1 = 0;
		CHECK(ammo.row_crc(1, row1));
		CHECK(sources.ammo_known && sources.ammo_row_crcs.size() == 2 &&
				sources.ammo_row_crcs[1] == row1);
		CHECK(!sources.weapon_table_known);
	}
	{
		Host host;
		std::string error;
		CHECK(inmatch::boot_host_mission(host.request(0), host.boot, error));
		const inmatch::IntegritySources &sources =
				host.role.state.host_owner.ctx.config.integrity_sources;
		CHECK(!sources.ammo_known && !sources.weapon_table_known);
	}

	// --- the listen host's loading-screen band ahead of its load ---------------
	// MISSIONNAME as the session serializer advertises it: the mission text's
	// [info]/title, else (empty) the header's name; the stock Co-op selector
	// the map file; with game_type_auto the header's own mode
	// [orig: Game_SerializeMissionInfoToDataStream @0x523620 -- the coop arm
	//  @0x523758, [info]/title @0x5237df, dword_A761D4 @0x52380d].
	{
		std::map<std::string, std::string> files;
		// The band reads the header alone: a .bms that is its 616-byte header.
		bms::Header header{};
		header.magic[0] = 'B';
		header.magic[1] = 'M';
		header.magic[2] = 'S';
		header.magic[3] = static_cast<char>(bms::kMinVersion);
		std::snprintf(header.mission_name, sizeof(header.mission_name), "Header Name");
		header.attrib_flags = bms::AttribFlags::Deathmatch;
		files["band.bms"] = std::string(reinterpret_cast<const char *>(&header), sizeof(header));
		const mission::BootFileSource src = test_boot::source_over(&files);

		inmatch::HostLoadingBand band = inmatch::host_loading_band(src, "band.bms", 0x10020u, true, {});
		CHECK(band.mission_name == "Header Name"); // no mission text: the header's name
		CHECK(band.game_type == game_type::for_mission_attribs(bms::AttribFlags::Deathmatch));

		files["band.bin"] = mission_text_bytes("Text Title");
		band = inmatch::host_loading_band(src, "band.bms", 0x10000u, false, {});
		CHECK(band.mission_name == "Text Title" && band.game_type == 0x10000u);
		band = inmatch::host_loading_band(src, "band.bms", 0x30020u, false, {});
		CHECK(band.mission_name == "Text Title"); // objective Co-op keeps the title
		band = inmatch::host_loading_band(src, "band.bms", 0x10020u, false, {});
		CHECK(band.mission_name == "band.bms"); // the stock Co-op selector: the map file

		files["band.bin"] = mission_text_bytes("");
		band = inmatch::host_loading_band(src, "band.bms", 0x10000u, false, {});
		CHECK(band.mission_name == "Header Name"); // an empty title: the header's name
	}

	if (failures != 0) {
		std::printf("host_boot: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("host_boot: ok\n");
	return 0;
}
