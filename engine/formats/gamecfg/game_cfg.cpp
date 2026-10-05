// game.cfg: the defaults, the per-line reader, the graphics clamp and the load.
// The record is docs/gamecfg/game-cfg-re.md.
#include "game_cfg.h"

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/os_path.h>

#include <cstdio>
#include <cstring>

namespace opennova::gamecfg {

namespace {

char ascii_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// _stricmp == 0 and Napi_StrCaseEqual in the C locale: an ASCII case fold
// [orig: _stricmp @0x76FDF6; Napi_StrCaseEqual @0x616E70].
bool equal_nocase(const char *a, const char *b) {
	for (;; ++a, ++b) {
		if (ascii_lower(*a) != ascii_lower(*b)) return false;
		if (*a == 0) return true;
	}
}

// _strnicmp(a, b, n) == 0 [orig: _strnicmp @0x7706B6].
bool equal_nocase_n(const char *a, const char *b, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
		if (a[i] == 0) return true;
	}
	return true;
}

// The first `limit` bytes of `src` up to its NUL: Napi_CopyString into a
// buffer of limit + 1 [orig: Napi_CopyString @0x617E10], strncpy(dst, src,
// limit) into a field of limit + 1 whose last byte nothing else writes, and
// (D-GAMECFG-1) an unbounded copy into a field of limit + 1.
std::string bounded(const char *src, size_t limit) {
	size_t n = 0;
	while (n < limit && src[n] != 0) ++n;
	return std::string(src, n);
}

// The weapon.def row a name selects, else -1 [orig: WeaponDef_FindIndexByName
// @0x54DD60: the first row whose name matches case-insensitively].
int find_weapon(const WeaponRoster &weapons, const char *name) {
	for (size_t i = 0; i < weapons.size(); ++i)
		if (equal_nocase(weapons[i].name.c_str(), name)) return static_cast<int>(i);
	return -1;
}

} // namespace

int32_t *weapon_slot(GameCfg &cfg, int row) {
	if (row < 0) return nullptr;
	if (row < kWeaponAvailabilityCount) return &cfg.weapon_availability[row];
	if (row < kWeaponAvailabilityCount + kClassAvailabilityCount)
		return &cfg.class_availability[row - kWeaponAvailabilityCount];
	return nullptr;
}

const int32_t *weapon_slot(const GameCfg &cfg, int row) {
	return weapon_slot(const_cast<GameCfg &>(cfg), row);
}

// The MULTIPLAYER cfg table, row for row [orig: g_ConfigVarTable @0x833050..
// 0x8333C8; each default a string at the row's +20].
const ConfigVarRow kConfigVarRows[] = {
	{ "MULTIPLAYER", ConfigVarType::None, 0, nullptr, nullptr, nullptr }, // [orig: @0x833050]
	{ "mpipaddressstring", ConfigVarType::String, 32, "0.0.0.0", nullptr, &GameCfg::mp_ip_address_string }, // [orig: @0x833068]
	{ "mpgateserverlocalportmin", ConfigVarType::UInt32, 0, "49152", &GameCfg::mp_gate_port_min, nullptr }, // [orig: @0x833080]
	{ "mpgateserverlocalportmax", ConfigVarType::UInt32, 0, "65536", &GameCfg::mp_gate_port_max, nullptr }, // [orig: @0x833098]
	{ "mpgateserverlocalportdelta", ConfigVarType::UInt32, 0, "1", &GameCfg::mp_gate_port_delta, nullptr }, // [orig: @0x8330B0]
	{ "mpgateserverlocalportrandom", ConfigVarType::Bool, 0, "0", &GameCfg::mp_gate_port_random, nullptr }, // [orig: @0x8330C8]
	{ "mpnovaworldportmin", ConfigVarType::UInt32, 0, "32768", &GameCfg::mp_novaworld_port_min, nullptr }, // [orig: @0x8330E0]
	{ "mpnovaworldportmax", ConfigVarType::UInt32, 0, "65535", &GameCfg::mp_novaworld_port_max, nullptr }, // [orig: @0x8330F8]
	{ "mpnovaworldportdelta", ConfigVarType::UInt32, 0, "1", &GameCfg::mp_novaworld_port_delta, nullptr }, // [orig: @0x833110]
	{ "mpnovaworldportrandom", ConfigVarType::Bool, 0, "0", &GameCfg::mp_novaworld_port_random, nullptr }, // [orig: @0x833128]
	{ "mpnovaworldlanenumsessionportmin", ConfigVarType::UInt32, 0, "32768", &GameCfg::mp_lan_enum_port_min, nullptr }, // [orig: @0x833140]
	{ "mpnovaworldlanenumsessionportmax", ConfigVarType::UInt32, 0, "65535", &GameCfg::mp_lan_enum_port_max, nullptr }, // [orig: @0x833158]
	{ "mpnovaworldlanenumsessionportdelta", ConfigVarType::UInt32, 0, "1", &GameCfg::mp_lan_enum_port_delta, nullptr }, // [orig: @0x833170]
	{ "mplanserverportmin", ConfigVarType::UInt32, 0, "32768", &GameCfg::mp_lan_server_port_min, nullptr }, // [orig: @0x833188]
	{ "mplanserverportmax", ConfigVarType::UInt32, 0, "32787", &GameCfg::mp_lan_server_port_max, nullptr }, // [orig: @0x8331A0]
	{ "mplanserverportdelta", ConfigVarType::UInt32, 0, "1", &GameCfg::mp_lan_server_port_delta, nullptr }, // [orig: @0x8331B8]
	{ "mplanclientportmin", ConfigVarType::UInt32, 0, "32768", &GameCfg::mp_lan_client_port_min, nullptr }, // [orig: @0x8331D0]
	{ "mplanclientportmax", ConfigVarType::UInt32, 0, "65535", &GameCfg::mp_lan_client_port_max, nullptr }, // [orig: @0x8331E8]
	{ "mplanclientportdelta", ConfigVarType::UInt32, 0, "1", &GameCfg::mp_lan_client_port_delta, nullptr }, // [orig: @0x833200]
	{ "mplanclientportrandom", ConfigVarType::Bool, 0, "0", &GameCfg::mp_lan_client_port_random, nullptr }, // [orig: @0x833218]
	{ "mplanservertojoinportmin", ConfigVarType::UInt32, 0, "32768", &GameCfg::mp_server_to_join_port_min, nullptr }, // [orig: @0x833230]
	{ "mplanservertojoinportmax", ConfigVarType::UInt32, 0, "32787", &GameCfg::mp_server_to_join_port_max, nullptr }, // [orig: @0x833248]
	{ "mplanservertojoinportdelta", ConfigVarType::UInt32, 0, "1", &GameCfg::mp_server_to_join_port_delta, nullptr }, // [orig: @0x833260]
	{ "mpmaxplayers", ConfigVarType::UInt32, 0, "64", &GameCfg::mp_max_players, nullptr }, // [orig: @0x833278]
	{ "mpuselineupqueue", ConfigVarType::Bool, 0, "1", &GameCfg::mp_use_lineup_queue, nullptr }, // [orig: @0x833290]
	{ "mplineupqueuesize", ConfigVarType::UInt32, 0, "100", &GameCfg::mp_lineup_queue_size, nullptr }, // [orig: @0x8332A8]
	{ "mphostgamepassword", ConfigVarType::String, 17, "", nullptr, &GameCfg::mp_host_game_password }, // [orig: @0x8332C0]
	{ "mphostsidepassworda", ConfigVarType::String, 17, "", nullptr, &GameCfg::mp_host_side_password_a }, // [orig: @0x8332D8]
	{ "mphostsidepasswordb", ConfigVarType::String, 17, "", nullptr, &GameCfg::mp_host_side_password_b }, // [orig: @0x8332F0]
	{ "mpnumspectatorsmax", ConfigVarType::Int32, 0, "0", &GameCfg::mp_num_spectators_max, nullptr }, // [orig: @0x833308]
	{ "mphostspectatorpassword", ConfigVarType::String, 64, "", nullptr, &GameCfg::mp_host_spectator_password }, // [orig: @0x833320]
	{ "mpaccesscodelist", ConfigVarType::String, 128, "", nullptr, &GameCfg::mp_access_code_list }, // [orig: @0x833338]
	{ "mphostpuntplayerswithsamepcids", ConfigVarType::Bool, 0, "1", &GameCfg::mp_host_punt_same_pcids, nullptr }, // [orig: @0x833350]
	{ "mptodcontinuity", ConfigVarType::Bool, 0, "1", &GameCfg::mp_tod_continuity, nullptr }, // [orig: @0x833368]
	{ "mpmaxpacketsize", ConfigVarType::Int32, 0, "1300", &GameCfg::mp_max_packet_size, nullptr }, // [orig: @0x833380]
	{ "mpextractextendedmetricinformation", ConfigVarType::Bool, 0, "1", &GameCfg::mp_extract_extended_metric_information, nullptr }, // [orig: @0x833398]
	{ "mpnovaworldhostlanonly", ConfigVarType::Bool, 0, "0", &GameCfg::mp_novaworld_host_lan_only, nullptr }, // [orig: @0x8333B0]
	{ "mpreset", ConfigVarType::Bool, 0, "0", &GameCfg::mp_reset, nullptr }, // [orig: @0x8333C8]
};
const size_t kConfigVarRowCount = sizeof(kConfigVarRows) / sizeof(kConfigVarRows[0]);

namespace {

// NapiConfigVar_AssignValue for the table's types: the header row has no
// storage and assigns nothing [orig: NapiConfigVar_AssignValue @0x6351A0 —
// String @0x6351E4..0x6351F3, Int32/UInt32 @0x63520F, Bool @0x6352AD..0x6352CD,
// no storage @0x6351B4].
void assign_config_var(GameCfg &cfg, const ConfigVarRow &row, const char *value) {
	switch (row.type) {
		case ConfigVarType::String:
			if (row.size >= 1) cfg.*row.string_field = bounded(value, static_cast<size_t>(row.size - 1));
			return;
		case ConfigVarType::Int32:
		case ConfigVarType::UInt32:
			cfg.*row.int_field = io::retail_atol(value);
			return;
		case ConfigVarType::Bool:
			cfg.*row.int_field = io::retail_atol(value) != 0 ? 1 : 0;
			return;
		case ConfigVarType::None:
			return;
	}
}

// How a switch key stores its value [orig: Config_ParseSettingsLine @0x54F740;
// atol is the CRT's strtol(s, NULL, 10) @0x76AB0A, atof _atof @0x76B6A1].
enum class Store {
	Int,         // atol
	Float,       // atof, stored as a float
	UInt16,      // atol, a 16-bit store
	Text,        // `limit` bytes of the value
	TextIfAny,   // the same, an empty value ignored (game_name)
	ClassSlot,   // class_availability[slot] = atol
	ModeBit,     // display_device_flags |= bit when atol == 1
	StartDelay,  // atol clamped 0..300
	NwIspType,   // atol; below 0 -> 18, above 18 -> 0
	LanMode,     // atol; not 1..4 -> 2
};

struct SwitchKey {
	char arm; // the lowercased first letter whose arm holds the compare
	const char *name;
	Store store;
	int32_t GameCfg::*int_field;
	float GameCfg::*float_field;
	std::string GameCfg::*text_field;
	uint32_t extra; // the Text limit, the class slot or the mode bit
};

constexpr SwitchKey I(char arm, const char *name, int32_t GameCfg::*field) {
	return { arm, name, Store::Int, field, nullptr, nullptr, 0 };
}
constexpr SwitchKey F(char arm, const char *name, float GameCfg::*field) {
	return { arm, name, Store::Float, nullptr, field, nullptr, 0 };
}
constexpr SwitchKey T(char arm, const char *name, std::string GameCfg::*field, uint32_t limit) {
	return { arm, name, Store::Text, nullptr, nullptr, field, limit };
}
constexpr SwitchKey C(const char *name, uint32_t slot) {
	return { 'a', name, Store::ClassSlot, nullptr, nullptr, nullptr, slot };
}
constexpr SwitchKey M(const char *name, uint32_t bit) {
	return { 'h', name, Store::ModeBit, nullptr, nullptr, nullptr, bit };
}
constexpr SwitchKey S(char arm, const char *name, Store store, int32_t GameCfg::*field) {
	return { arm, name, store, field, nullptr, nullptr, 0 };
}

// Every compare of the reader's switch, arm by arm in its compare order. A
// key matches only in the arm of its own first letter, so the two 's' keys the
// 't' arm holds can never match (they are kept, dead, as retail has them).
// The limits: strncpy 16 into the 17-byte passwords and video_res
// (@0x55039A, @0x55115E, @0x5512EF), Napi_CopyString 128 for servermsg
// (@0x551000), and an unbounded copy elsewhere, kept to its field less one
// (D-GAMECFG-1).
// clang-format off
const SwitchKey kSwitchKeys[] = {
	// 'a' [orig: @0x54F794..0x54FA10; avail_wpn after the chain @0x54FA1E]
	I('a', "audio_channels", &GameCfg::audio_channels),
	I('a', "audio_rate", &GameCfg::audio_rate),
	I('a', "antialias_mode", &GameCfg::antialias_mode),
	I('a', "allowcustomskins", &GameCfg::allowcustomskins),
	I('a', "AltLock", &GameCfg::alt_lock),
	I('a', "AltLockType", &GameCfg::alt_lock_type),
	I('a', "autobalanceonmissionrecycleenabled", &GameCfg::autobalance_on_recycle_enabled),
	I('a', "autobalanceonmissionrecyclenumplayerdiffmin", &GameCfg::autobalance_on_recycle_diff_min),
	I('a', "autobalanceonmissionrecyclenumplayerdiffmax", &GameCfg::autobalance_on_recycle_diff_max),
	I('a', "armory_reuse_time", &GameCfg::armory_reuse_time),
	C("avail_class_rifleman", kClassRifleman),
	C("avail_class_sniper", kClassSniper),
	C("avail_class_medic", kClassMedic),
	C("avail_class_gunner", kClassGunner),
	C("avail_class_engineer", kClassEngineer),
	// 'b' [orig: @0x54FA66..0x54FAB3]
	I('b', "balance_join", &GameCfg::balance_join),
	F('b', "balance_join_percent", &GameCfg::balance_join_percent),
	// 'c' [orig: @0x54FAC1..0x54FBC4]
	I('c', "crosshairs", &GameCfg::crosshairs),
	I('c', "crosshairs_color", &GameCfg::crosshairs_color),
	I('c', "contest", &GameCfg::contest),
	T('c', "country", &GameCfg::country, 7),
	I('c', "choosespawnonbegin", &GameCfg::choosespawnonbegin),
	I('c', "cl_punkbuster", &GameCfg::cl_punkbuster),
	// 'd' [orig: @0x54FBCD..0x54FD4B]
	I('d', "DialogueLevel", &GameCfg::dialogue_level),
	I('d', "dedicated", &GameCfg::dedicated),
	I('d', "destroybuild", &GameCfg::destroybuild),
	I('d', "dominpingcheck", &GameCfg::dominpingcheck),
	I('d', "dirtyupstream", &GameCfg::dirtyupstream),
	I('d', "dirtyupstreampost", &GameCfg::dirtyupstreampost),
	I('d', "domaxpingcheck", &GameCfg::domaxpingcheck),
	I('d', "deathmes", &GameCfg::deathmes),
	I('d', "display_16x9", &GameCfg::display_16x9),
	// 'e' [orig: @0x54FD54..0x54FDFB]
	I('e', "enable_ai", &GameCfg::enable_ai),
	I('e', "enable_keyboardtips", &GameCfg::enable_keyboardtips),
	I('e', "enable_gameplaytips", &GameCfg::enable_gameplaytips),
	I('e', "enable_slotmachine", &GameCfg::enable_slotmachine),
	// 'f' [orig: @0x54FE04..0x54FEAB]
	I('f', "fatbullets", &GameCfg::fatbullets),
	I('f', "force_vsync", &GameCfg::force_vsync),
	I('f', "fbeffects_level", &GameCfg::fbeffects_level),
	I('f', "farp_reuse_time", &GameCfg::farp_reuse_time),
	// 'g' [orig: @0x54FEB4..0x54FF23: game_name's strlen gate @0x54FEFE]
	F('g', "gamma", &GameCfg::gamma),
	{ 'g', "game_name", Store::TextIfAny, nullptr, nullptr, &GameCfg::game_name, 31 },
	// 'h' [orig: @0x54FF2A..0x550339; hw3d_deviceno's store into dword_B4C22C at
	//  @0x54FF4C, the name/guid copies @0x54FF6D, @0x54FF9D]
	I('h', "hw3d_deviceno", &GameCfg::hw3d_deviceno),
	T('h', "hw3d_name", &GameCfg::hw3d_name, 31),
	T('h', "hw3d_guid", &GameCfg::hw3d_guid, 35),
	M("hw3d_res640x480", 0x1),
	M("hw3d_res800x600", 0x2),
	M("hw3d_res1024x768", 0x4),
	M("hw3d_res1280x720", 0x8),
	M("hw3d_res1280x768", 0x10),
	M("hw3d_res1280x960", 0x20),
	M("hw3d_res1280x1024", 0x40),
	M("hw3d_res1680x1050", 0x100),
	M("hw3d_res1600x1200", 0x200),
	M("hw3d_res1920x1080", 0x400),
	M("hw3d_res1920x1200", 0x800),
	M("hw3d_res1440x900", 0x80),
	M("hw3d_res1920x1440", 0x1000),
	M("hw3d_res2048x1536", 0x2000),
	I('h', "hitfeedback", &GameCfg::hitfeedback),
	I('h', "hud_color_index", &GameCfg::hud_color_index),
	I('h', "hud_detail", &GameCfg::hud_detail),
	// 'i' [orig: @0x550342..0x55036F]
	T('i', "internet_address", &GameCfg::internet_address, 34),
	// 'j' [orig: @0x550376..0x55039A]
	T('j', "join_password", &GameCfg::join_password, 16),
	// 'k' [orig: @0x5503A7..0x5503F8]
	I('k', "koth_limit", &GameCfg::koth_limit),
	I('k', "koth_delta", &GameCfg::koth_delta),
	// 'l' [orig: @0x550401..0x550480; the lanmode fold @0x550425..0x55044C]
	S('l', "lanmode", Store::LanMode, &GameCfg::lanmode),
	I('l', "lock_framerate", &GameCfg::lock_framerate),
	// 'm' [orig: @0x550489..0x550B42; mp_NoScopeDrift and mp_NoWeaponRecoil are
	//  compared twice, @0x55076C/@0x550843 and @0x550797/@0x55086E]
	I('m', "music_volume", &GameCfg::music_volume),
	I('m', "mp_eula_accepted", &GameCfg::mp_eula_accepted),
	I('m', "mpattrib", &GameCfg::mpattrib),
	I('m', "mp_difficulty", &GameCfg::mp_difficulty),
	I('m', "mp_gametype", &GameCfg::mp_gametype),
	I('m', "max_team_lives", &GameCfg::max_team_lives),
	I('m', "max_kills", &GameCfg::max_kills),
	I('m', "max_score", &GameCfg::max_score),
	I('m', "minping", &GameCfg::minping),
	I('m', "maxping", &GameCfg::maxping),
	I('m', "mp_numteams", &GameCfg::mp_numteams),
	I('m', "mp_flagreturntime", &GameCfg::flag_return_time),
	I('m', "mp_flagresettime", &GameCfg::flag_reset_time),
	I('m', "map_infrared", &GameCfg::map_infrared),
	I('m', "map_iff", &GameCfg::map_iff),
	I('m', "map_AWAC", &GameCfg::map_awac),
	T('m', "msg", &GameCfg::msg, 23),
	I('m', "mp_NoScopeDrift", &GameCfg::mp_no_scope_drift),
	I('m', "mp_NoWeaponRecoil", &GameCfg::mp_no_weapon_recoil),
	I('m', "mp_verbose", &GameCfg::mp_verbose),
	I('m', "mp_NoCharAbilities", &GameCfg::mp_no_char_abilities),
	I('m', "mp_NoCrossHairSpread", &GameCfg::mp_no_crosshair_spread),
	I('m', "mp_gpsicons", &GameCfg::mp_gpsicons),
	I('m', "mp_wind", &GameCfg::mp_wind),
	I('m', "mp_DroppedWeaponDisappear", &GameCfg::mp_dropped_weapon_disappear),
	I('m', "mp_NoDropWeapons", &GameCfg::mp_no_drop_weapons),
	I('m', "mp_NoRespawnWithPrimary", &GameCfg::mp_no_respawn_with_primary),
	I('m', "mpvoting", &GameCfg::mpvoting),
	I('m', "mpvoting_min_players", &GameCfg::mpvoting_min_players),
	F('m', "mpvoting_percent", &GameCfg::mpvoting_percent),
	I('m', "mpvoting_period", &GameCfg::mpvoting_period),
	I('m', "mpchangeteam", &GameCfg::mpchangeteam),
	I('m', "mpchangeteam_interval", &GameCfg::mpchangeteam_interval),
	I('m', "mpchangeteam_penalty", &GameCfg::mpchangeteam_penalty),
	I('m', "mp_lfp_takeoverspeed", &GameCfg::mp_lfp_takeoverspeed),
	I('m', "mp_allowsniperscopezoom", &GameCfg::mp_allowsniperscopezoom),
	I('m', "mp_permanent_death", &GameCfg::mp_permanent_death),
	I('m', "mp_3rdperson_driver", &GameCfg::mp_3rdperson_driver),
	// 'n' [orig: @0x550B4B..0x550CC4; the nwisptype fold @0x550C1A..0x550C3A]
	I('n', "no_anim", &GameCfg::no_anim),
	I('n', "NoBlood", &GameCfg::no_blood),
	I('n', "NoCasings", &GameCfg::no_casings),
	I('n', "NoSmoke", &GameCfg::no_smoke),
	S('n', "nwisptype", Store::NwIspType, &GameCfg::nwisptype),
	I('n', "numallowablefriendlykills", &GameCfg::numallowablefriendlykills),
	I('n', "nodefaultspawnpoints", &GameCfg::nodefaultspawnpoints),
	I('n', "networkconnecttype", &GameCfg::networkconnecttype),
	// 'o' [orig: @0x550CCD..0x550D49]
	I('o', "object_polydetail", &GameCfg::object_polydetail),
	I('o', "object_texdetail", &GameCfg::object_texdetail),
	I('o', "oneshotonekill", &GameCfg::oneshotonekill),
	// 'p' [orig: @0x550D52..0x550E24]
	I('p', "particle_density", &GameCfg::particle_density),
	I('p', "player_index", &GameCfg::player_index),
	I('p', "preempt_pff", &GameCfg::preempt_pff),
	I('p', "play_exit_credits", &GameCfg::play_exit_credits),
	I('p', "ping", &GameCfg::ping),
	// 'r' [orig: @0x550E2D..0x550F00; remote_admin_port's `mov word` @0x550EA5]
	I('r', "rotor_volume", &GameCfg::rotor_volume),
	I('r', "replay", &GameCfg::replay),
	{ 'r', "remote_admin_port", Store::UInt16, nullptr, nullptr, nullptr, 0 },
	I('r', "reduce_mouselag", &GameCfg::reduce_mouselag),
	I('r', "rememberlogin", &GameCfg::rememberlogin),
	// 's' [orig: @0x550F09..0x5510DF; the startdelay clamp @0x550FAD..0x550FCF]
	I('s', "SFXLevel", &GameCfg::sfx_level),
	I('s', "shadow_quality", &GameCfg::shadow_quality),
	I('s', "showgun", &GameCfg::showgun),
	S('s', "startdelay", Store::StartDelay, &GameCfg::startdelay),
	T('s', "servermsg", &GameCfg::servermsg, 127),
	I('s', "sendplayerlist", &GameCfg::sendplayerlist),
	I('s', "spawnregulator_psp", &GameCfg::spawnregulator_psp),
	I('s', "spawnregulator_lfp", &GameCfg::spawnregulator_lfp),
	I('s', "shader_usage_level", &GameCfg::shader_usage_level),
	I('s', "sv_punkbuster", &GameCfg::sv_punkbuster),
	// 't' [orig: @0x5510E8..0x551268; side_password @0x551141 and side_req
	//  @0x55116E sit here, unreachable]
	I('t', "terrain_polydetail", &GameCfg::terrain_polydetail),
	I('t', "terrain_texdetail", &GameCfg::terrain_texdetail),
	T('t', "side_password", &GameCfg::side_password, 16),
	I('t', "side_req", &GameCfg::side_req),
	I('t', "time_limit", &GameCfg::time_limit),
	I('t', "timeout", &GameCfg::timeout),
	I('t', "teamchange_time", &GameCfg::teamchange_time),
	I('t', "texfilter_level", &GameCfg::texfilter_level),
	I('t', "texcompression_level", &GameCfg::texcompression_level),
	// 'u' [orig: @0x551271..0x551297]
	I('u', "unlimited_vehicles", &GameCfg::unlimited_vehicles),
	// 'v' [orig: @0x5512A0..0x551303; video_res also parses its width and
	//  height into +0x060/+0x064 (String_ParseResolutionDimensions @0x51E6D0),
	//  derived state the file never carries]
	I('v', "version", &GameCfg::version),
	T('v', "video_res", &GameCfg::video_res, 16),
	// 'w' [orig: @0x551310..0x55138C]
	I('w', "windowed", &GameCfg::windowed),
	I('w', "windows_volume", &GameCfg::windows_volume),
	I('w', "water_quality", &GameCfg::water_quality),
	// 'x' [orig: @0x551395..0x551409]
	I('x', "xhair_color", &GameCfg::xhair_color),
	I('x', "xhair_appearance", &GameCfg::xhair_appearance),
	I('x', "xhair_spread", &GameCfg::xhair_spread),
};
// clang-format on

void store_switch_key(GameCfg &cfg, const SwitchKey &key, const char *value) {
	switch (key.store) {
		case Store::Int:
			cfg.*key.int_field = io::retail_atol(value);
			return;
		case Store::Float:
			cfg.*key.float_field = static_cast<float>(io::retail_atof(value));
			return;
		case Store::UInt16:
			cfg.remote_admin_port = static_cast<uint16_t>(io::retail_atol(value));
			return;
		case Store::Text:
			cfg.*key.text_field = bounded(value, key.extra);
			return;
		case Store::TextIfAny:
			if (value[0] != 0) cfg.*key.text_field = bounded(value, key.extra);
			return;
		case Store::ClassSlot:
			cfg.class_availability[key.extra] = io::retail_atol(value);
			return;
		case Store::ModeBit:
			if (io::retail_atol(value) == 1) cfg.display_device_flags |= key.extra;
			return;
		case Store::StartDelay: {
			const int32_t v = io::retail_atol(value);
			cfg.startdelay = v < 0 ? 0 : (v > 300 ? 300 : v);
			return;
		}
		case Store::NwIspType: {
			const int32_t v = io::retail_atol(value);
			cfg.nwisptype = v < 0 ? 18 : (v > 18 ? 0 : v);
			return;
		}
		case Store::LanMode: {
			const int32_t v = io::retail_atol(value);
			cfg.lanmode = (v == 1 || v == 2 || v == 3 || v == 4) ? v : 2;
			return;
		}
	}
}

} // namespace

GameCfg defaults(const DefaultTexts &texts, int32_t player_index, int32_t hw3d_deviceno) {
	// The block is zeroed, keeping its first word [orig: Config_SetDefaults
	// @0x54D033..0x54D046, restored @0x54D0C4], then the cfg table's
	// defaults [orig: NapiConfigVar_ApplyDefaults @0x6356D0 from @0x54D05B].
	GameCfg cfg;
	for (size_t i = 0; i < kConfigVarRowCount; ++i) {
		const ConfigVarRow &row = kConfigVarRows[i];
		if (row.default_value != nullptr) assign_config_var(cfg, row, row.default_value);
	}
	// The packet-size and player-count folds, unsigned and signed
	// [orig: @0x54D060..0x54D090, @0x54D09A..0x54D0B0].
	const uint32_t packet = static_cast<uint32_t>(cfg.mp_max_packet_size);
	if (packet == 0)
		cfg.mp_max_packet_size = 1300;
	else if (packet < 100u)
		cfg.mp_max_packet_size = 100;
	else if (packet > 0x4000u)
		cfg.mp_max_packet_size = 0x4000;
	if (cfg.mp_max_players < 1)
		cfg.mp_max_players = 1;
	else if (cfg.mp_max_players > 64)
		cfg.mp_max_players = 64;
	cfg.player_index = player_index;
	cfg.hw3d_deviceno = hw3d_deviceno; // dword_B4C22C is not in the block

	// [orig: Config_SetDefaults @0x54D0C4..0x54D48B, in its store order]
	cfg.version = kConfigVersion;
	cfg.preempt_pff = 0;
	cfg.windowed = 0;
	cfg.no_anim = 0;
	cfg.gamma = 1.0f;
	cfg.force_vsync = 0;
	cfg.lock_framerate = 1;
	cfg.reduce_mouselag = 1;
	cfg.enable_keyboardtips = 1;
	cfg.enable_gameplaytips = 1;
	cfg.windows_volume = 255;
	cfg.audio_channels = 2;
	cfg.audio_rate = 44100;
	cfg.enable_slotmachine = 0;
	cfg.music_volume = 192;
	cfg.sfx_level = 192;
	cfg.dialogue_level = 192;
	cfg.rotor_volume = 192;
	cfg.crosshairs = 1;
	cfg.crosshairs_color = 1;
	cfg.hitfeedback = 1;
	cfg.showgun = 3;
	cfg.mp_eula_accepted = -1;
	cfg.game_name = bounded(texts.untitled.c_str(), 31); // unbounded strcpy @0x54D1B3 (D-GAMECFG-1)
	cfg.networkconnecttype = 1;
	cfg.mp_difficulty = 0;
	cfg.max_team_lives = 100;
	cfg.max_kills = 50;
	cfg.max_score = 5;
	cfg.side_req = -1;
	cfg.startdelay = 0;
	cfg.destroybuild = 0;
	cfg.autobalance_on_recycle_enabled = 0;
	cfg.autobalance_on_recycle_diff_min = 1;
	cfg.autobalance_on_recycle_diff_max = 1;
	cfg.oneshotonekill = 0;
	cfg.fatbullets = 0;
	cfg.nwisptype = 5;
	cfg.lanmode = 1;
	cfg.servermsg = bounded(texts.user_message.c_str(), 127); // unbounded copy @0x54D248 (D-GAMECFG-1)
	cfg.koth_delta = 5;
	cfg.timeout = 5;
	cfg.mpvoting_percent = 0.66f;
	cfg.internet_address = "0.0.0.0";
	cfg.koth_limit = 10;
	cfg.mp_numteams = 2;
	cfg.spawnregulator_lfp = 10;
	cfg.hud_color_index = 2;
	cfg.minping = 0;
	cfg.dominpingcheck = 0;
	cfg.maxping = 1500;
	cfg.domaxpingcheck = 0;
	cfg.dirtyupstream = 1;
	cfg.dirtyupstreampost = 1;
	cfg.deathmes = 1;
	cfg.numallowablefriendlykills = 3;
	cfg.replay = 1;
	cfg.time_limit = 30;
	cfg.flag_return_time = 210;
	cfg.flag_reset_time = 420;
	cfg.country.clear();
	cfg.msg.clear();
	cfg.ping = 1;
	cfg.sendplayerlist = 1;
	cfg.teamchange_time = 15;
	cfg.mp_lfp_takeoverspeed = 1;
	cfg.spawnregulator_psp = 0;
	cfg.choosespawnonbegin = 0;
	cfg.nodefaultspawnpoints = 0;
	cfg.unlimited_vehicles = 1;
	cfg.mp_permanent_death = 0;
	cfg.mp_3rdperson_driver = 1;
	cfg.mp_allowsniperscopezoom = 0;
	// The cfg table's mptodcontinuity default "1" is overwritten here: the
	// effective default is 0 [orig: @0x54D36A, timeOfDayContinuity +0x334].
	cfg.mp_tod_continuity = 0;
	cfg.rememberlogin = 1;
	cfg.mpvoting = 0;
	cfg.mpvoting_min_players = 6;
	cfg.mpvoting_period = 180;
	cfg.mpchangeteam = 0;
	cfg.mpchangeteam_interval = 300;
	cfg.mpchangeteam_penalty = 60;
	cfg.map_infrared = 1;
	cfg.map_iff = 1;
	cfg.map_awac = 1;
	cfg.hud_detail = 0;
	cfg.contest = 0;
	cfg.mp_verbose = 1;
	cfg.alt_lock = 1;
	cfg.alt_lock_type = 1;
	cfg.play_exit_credits = -1;
	cfg.mpattrib = 14850;
	for (int32_t &word : cfg.weapon_availability) word = 3; // memset32(3, 0xFF) @0x54D410
	for (int32_t &word : cfg.class_availability) word = 1;
	cfg.enable_ai = 1;
	cfg.remote_admin_port = 0;
	cfg.xhair_appearance = 0;
	cfg.xhair_color = 0xFFFFFF;
	cfg.xhair_spread = 1;
	cfg.balance_join_percent = 0.5f;
	cfg.balance_join = 1;
	cfg.armory_reuse_time = 30;
	cfg.farp_reuse_time = 90;
	return cfg;
}

void apply_line(GameCfg &cfg, const io::ConfigTokens &line, const WeaponRoster &weapons) {
	// The key is token 0 and the value token 2 [orig: tokens[1] / tokens[3]
	// of the count-first array @0x54F74D..0x54F750].
	const char *key = line.token(0);
	const char *value = line.token(2);

	// The cfg table first; a matched row ends the line whatever it assigned
	// [orig: Config_ParseSettingsLine @0x54F75C..0x54F76B;
	// NapiConfigVar_FindByName @0x635150 walks the rows in order].
	for (size_t i = 0; i < kConfigVarRowCount; ++i) {
		if (equal_nocase(kConfigVarRows[i].name, key)) {
			assign_config_var(cfg, kConfigVarRows[i], value);
			return;
		}
	}

	// The switch on the lowercased first letter, 'a'..'x' [orig: @0x54F771..
	// 0x54F78D, the jump table @0x551418; any other letter is the default].
	const char arm = ascii_lower(key[0]);
	if (arm < 'a' || arm > 'x') return;
	for (const SwitchKey &k : kSwitchKeys) {
		if (k.arm != arm || !equal_nocase(key, k.name)) continue;
		store_switch_key(cfg, k, value);
		return;
	}
	// The 'a' arm's tail: `avail_wpn<x>` names the weapon.def row matching the
	// key from its sixth character [orig: @0x54FA1E..0x54FA5A].
	if (arm == 'a' && equal_nocase_n(key, "avail_wpn", 9)) {
		const int index = find_weapon(weapons, key + 6);
		if (int32_t *slot = weapon_slot(cfg, index)) *slot = io::retail_atol(value);
	}
}

void parse(const char *text, size_t size, GameCfg &cfg, const WeaponRoster &weapons) {
	io::for_each_config_file_line(text, size, [&](io::ConfigTokens &line) { apply_line(cfg, line, weapons); });
}

void clamp_graphics_options(GameCfg &cfg) {
	// [orig: Settings_ClampGraphicsOptions @0x54D4A0, field by field]
	auto clamp = [](int32_t &v, int32_t lo, int32_t hi) {
		if (v < lo)
			v = lo;
		else if (v > hi)
			v = hi;
	};
	clamp(cfg.terrain_polydetail, 0, 3);
	clamp(cfg.terrain_texdetail, 0, 3);
	clamp(cfg.object_polydetail, 0, 3);
	clamp(cfg.object_texdetail, 0, 3);
	clamp(cfg.water_quality, 1, 3);
	clamp(cfg.shadow_quality, 0, 3);
	clamp(cfg.particle_density, 0, 2);
	clamp(cfg.antialias_mode, 0, 16); // a signed compare with 0 @0x54D56E..0x54D584
	clamp(cfg.texfilter_level, 0, 3);
	clamp(cfg.fbeffects_level, 0, 3);
	// The x87 compares keep a NaN [orig: @0x54D5C4..0x54D5FE: below 0.5f ->
	// 0.5f, above 2.0f -> 2.0f, the unordered case jumps past both stores].
	if (cfg.gamma < 0.5f)
		cfg.gamma = 0.5f;
	else if (cfg.gamma > 2.0f)
		cfg.gamma = 2.0f;
	// atol("64") as the ceiling [orig: @0x54D600..0x54D622].
	clamp(cfg.mp_max_players, 1, 64);
}

LoadResult load(const char *text, size_t size, const LoadOptions &options) {
	// [orig: Game_LoadConfig @0x551480]
	LoadResult result;
	result.cfg = defaults(options.texts, options.player_index, options.hw3d_deviceno);
	result.cfg.version = -1; // @0x55148F
	if (text != nullptr) {
		result.file_read = true;
		parse(text, size, result.cfg, options.weapons);
	}
	if (result.cfg.mp_reset != 0) { // crt_exit(0) @0x5514AC
		result.reset_exit = true;
		return result;
	}
	if (options.lan_switch) result.cfg.mp_novaworld_host_lan_only = 1; // @0x5514BA
	if (result.cfg.version != kConfigVersion) { // @0x5514C4
		result.version_reset = true;
		result.cfg = defaults(options.texts, result.cfg.player_index, result.cfg.hw3d_deviceno);
	}
	clamp_graphics_options(result.cfg); // @0x5514D2
	return result;
}

LoadResult load_file(const std::string &path, const LoadOptions &options) {
	// fopen "r" [orig: File_ParseASCIIFileWithCallback @0x53D9A3..0x53D9C9: a
	// file that does not open parses nothing]
	std::FILE *f = io::fopen_utf8(path.c_str(), "rb");
	if (f == nullptr) return load(nullptr, 0, options);
	std::string text;
	char chunk[4096];
	size_t got = 0;
	while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
	std::fclose(f);
	return load(text.data(), text.size(), options);
}

} // namespace opennova::gamecfg
