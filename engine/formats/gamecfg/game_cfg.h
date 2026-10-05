// game.cfg — the player's and the host's persisted settings.
//
// A plain-text file in the game's working directory, read at boot, again once
// weapon.def has loaded, and at every main-menu entry
// [orig: Game_LoadConfig @0x551480; called from Game_Run @0x4A7FBB,
//  Game_InitSubsystems @0x4A70AB, Menu_InitShellResources @0x552534], and
// written back whole at every exit and by every screen that changes a setting
// [orig: Game_SaveConfig @0x54C490; Game_Run @0x4A7FFF among its 15 call sites].
// The record is docs/gamecfg/game-cfg-re.md (D-GAMECFG-n).
//
// THE FILE. One `key = value` line per setting: the line walk and tokenizer
// every retail text file shares (io::for_each_config_file_line), the key token
// 0 and the value token 2, so the `=` is just a token and `key value` reads an
// empty value [orig: Config_ParseSettingsLine @0x54F740 reads tokens[1] and
// tokens[3] of File_ParseASCIIFileWithCallback @0x53D980's count-first token
// array]. Two key sets:
//
//  - the MULTIPLAYER cfg table (NapiConfigVar rows, ConfigVarRow below),
//    consulted first: a case-insensitive name match assigns by the row's type
//    and ends the line [orig: NapiConfigVar_AssignByName @0x635600 from
//    @0x54F75C; g_ConfigVarTable @0x833050];
//  - then a switch on the key's lowercased first letter, each arm a chain of
//    case-insensitive compares [orig: the jump table @0x551418, 'a'..'x'].
//    A compare that sits in an arm its key cannot reach is dead, as retail's
//    `side_password` and `side_req` are (both under 't' @0x551141,
//    @0x55116E): written, never read.
//
// Unknown keys do nothing. Some keys are read and never written
// (`enable_slotmachine`, `mp_flagreturntime`, `mp_flagresettime`, the
// `hw3d_res*` set) and some written and never read (`flagreturntime`,
// `side_password`, `side_req`): the writer emits `flagreturntime` for the
// field the reader fills from `mp_flagreturntime`.
//
// THE LOAD (load below = Game_LoadConfig): the defaults, the version set to
// -1, the file's lines, then a version other than 29 resets everything to the
// defaults again (keeping player_index) and the graphics clamp runs.
//
// THE WRITE (write below = Game_SaveConfig): every key in one fixed order with
// fixed padding, CR LF line ends (a text-mode "w" stream), built from the model
// alone (ADR 0003), never from the bytes that were read.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::io {
struct ConfigTokens;
}

namespace opennova::gamecfg {

// The version the build writes and requires [orig: Config_SetDefaults
// @0x54D0D1 sets 29; Game_LoadConfig @0x5514C4 `cmp version, 1Dh`].
inline constexpr int32_t kConfigVersion = 29;

// The array lengths of the two availability blocks [orig: GameConfigState
// +0x5F0 int32[255] weaponAvailability, +0x9EC int32[10] classAvailability].
inline constexpr int kWeaponAvailabilityCount = 255;
inline constexpr int kClassAvailabilityCount = 10;

// The class slots the five avail_class_* keys address (the record's class
// index; slots 0..4 have no key) [orig: Config_ParseSettingsLine @0x54F964,
// @0x54F98F, @0x54F9BA, @0x54F9E5, @0x54FA10].
inline constexpr int kClassMedic = 5;
inline constexpr int kClassSniper = 6;
inline constexpr int kClassGunner = 7;
inline constexpr int kClassRifleman = 8;
inline constexpr int kClassEngineer = 9;

// One weapon.def row as the avail_wpn keys see it: its name (`WPN_colt45`)
// and its loadout_selectable word [orig: g_WeaponDefTable @0x2540CE0, 192-byte
// rows: the name at +0, loadout_selectable at +32 (WeaponDef_ParseProperty
// @0x54D9F0)]. The reader matches `avail_wpn_<x>` from its sixth character
// (`wpn_<x>`) against the whole name, case-insensitively
// [orig: WeaponDef_FindIndexByName @0x54DD60 from @0x54FA40]; the writer
// prints the name from its fifth character for every selectable row
// [orig: Game_SaveConfig @0x54CEDE..0x54CF0F]. The roster is weapon.def in
// file order: retail's first read at boot runs before weapon.def loads, so an
// empty roster drops every avail_wpn line, as that read does.
struct WeaponRosterEntry {
	std::string name;
	int32_t loadout_selectable = 0;
};
using WeaponRoster = std::vector<WeaponRosterEntry>;

// Every key, as the fields of retail's g_GameConfigState @0x25506B8 hold them.
// Strings keep at most the bytes retail's copy keeps (the sizes cited per
// field); a copy retail does without a bound keeps its field's size less one
// here (D-GAMECFG-1). Defaults are Config_SetDefaults' (defaults() below).
struct GameCfg {
	// GENERAL
	int32_t version = 0; // `version` (+0x00C)

	// DISPLAY
	int32_t windowed = 0;          // +0x010
	int32_t hw3d_deviceno = 0;     // dword_B4C22C, outside the block: defaults keep it
	std::string hw3d_name;         // +0x018 char[32], unbounded copy
	std::string hw3d_guid;         // +0x038 char[36], unbounded copy
	// +0x05C, the hw3d_res<W>x<H> keys: each raises its bit when its value is
	// exactly 1 (640x480 0x1 .. 2048x1536 0x2000, the 'h' arm @0x54FFB5..
	// 0x5502BC; game_cfg.cpp's table); never written
	uint32_t display_device_flags = 0;
	std::string video_res;         // +0x068, strncpy 16
	float gamma = 0.0f;            // +0x09C (the writer prints its applied copy flt_B4C298)
	int32_t terrain_polydetail = 0;   // +0x10C graphicsQuality +0x00
	int32_t terrain_texdetail = 0;    // +0x04
	int32_t object_polydetail = 0;    // +0x08
	int32_t object_texdetail = 0;     // +0x0C
	int32_t water_quality = 0;        // +0x10
	int32_t shadow_quality = 0;       // +0x14
	int32_t particle_density = 0;     // +0x18
	int32_t antialias_mode = 0;       // +0x1C
	int32_t display_16x9 = 0;         // +0x20
	int32_t texfilter_level = 0;      // +0x24
	int32_t fbeffects_level = 0;      // +0x28
	int32_t shader_usage_level = 0;   // +0x2C
	int32_t texcompression_level = 0; // +0x30
	int32_t lock_framerate = 0;       // +0x08C
	int32_t force_vsync = 0;          // +0x090
	int32_t reduce_mouselag = 0;      // +0x188
	int32_t enable_keyboardtips = 0;  // +0x18C
	int32_t enable_gameplaytips = 0;  // +0x190

	// AUDIO
	int32_t windows_volume = 0;     // +0x1D8
	int32_t music_volume = 0;       // +0x1DC
	int32_t sfx_level = 0;          // `SFXLevel` +0x1E0
	int32_t dialogue_level = 0;     // `DialogueLevel` +0x1E4
	int32_t rotor_volume = 0;       // +0x1E8
	int32_t audio_channels = 0;     // +0x1EC
	int32_t audio_rate = 0;         // +0x1F0
	int32_t enable_slotmachine = 0; // +0x1F4; never written
	int32_t no_blood = 0;           // `NoBlood` +0x174
	int32_t no_casings = 0;         // `NoCasings` +0x17C
	int32_t no_smoke = 0;           // `NoSmoke` +0x178

	// CONTROLS
	int32_t crosshairs = 0;       // +0x194
	int32_t crosshairs_color = 0; // +0x520
	int32_t hitfeedback = 0;      // +0x198
	int32_t showgun = 0;          // +0x19C
	int32_t hud_color_index = 0;  // +0x514
	int32_t hud_detail = 0;       // +0x518

	// MULTIPLAYER: the cfg table's rows (kConfigVarRows)
	std::string mp_ip_address_string;   // `mpipaddressstring` +0x1F8 [32]
	int32_t mp_gate_port_min = 0;       // +0x218
	int32_t mp_gate_port_max = 0;       // +0x21C
	int32_t mp_gate_port_delta = 0;     // +0x220
	int32_t mp_gate_port_random = 0;    // +0x224 (bool)
	int32_t mp_novaworld_port_min = 0;  // +0x228
	int32_t mp_novaworld_port_max = 0;  // +0x22C
	int32_t mp_novaworld_port_delta = 0;  // +0x230
	int32_t mp_novaworld_port_random = 0; // +0x234 (bool)
	int32_t mp_lan_enum_port_min = 0;   // +0x238
	int32_t mp_lan_enum_port_max = 0;   // +0x23C
	int32_t mp_lan_enum_port_delta = 0; // +0x240
	int32_t mp_lan_server_port_min = 0; // +0x244
	int32_t mp_lan_server_port_max = 0; // +0x248
	int32_t mp_lan_server_port_delta = 0; // +0x24C
	int32_t mp_lan_client_port_min = 0; // +0x250
	int32_t mp_lan_client_port_max = 0; // +0x254
	int32_t mp_lan_client_port_delta = 0;  // +0x258
	int32_t mp_lan_client_port_random = 0; // +0x25C (bool)
	int32_t mp_server_to_join_port_min = 0;   // +0x260
	int32_t mp_server_to_join_port_max = 0;   // +0x264
	int32_t mp_server_to_join_port_delta = 0; // +0x268
	int32_t mp_max_players = 0;         // `mpmaxplayers` +0x3F4
	int32_t mp_use_lineup_queue = 0;    // +0x3F8 (bool)
	int32_t mp_lineup_queue_size = 0;   // +0x3FC
	std::string mp_host_game_password;  // +0x350 [17]
	std::string mp_host_side_password_a; // +0x383 [17]
	std::string mp_host_side_password_b; // +0x394 [17]
	int32_t mp_num_spectators_max = 0;  // +0x26C
	std::string mp_host_spectator_password; // +0x270 [64]
	std::string mp_access_code_list;    // +0x2B0 [128]
	int32_t mp_host_punt_same_pcids = 0; // +0x330 (bool)
	int32_t mp_tod_continuity = 0;      // +0x334 (bool)
	int32_t mp_max_packet_size = 0;     // +0x338
	int32_t mp_extract_extended_metric_information = 0; // +0x33C (bool)
	int32_t mp_novaworld_host_lan_only = 0; // +0x340 (bool)
	int32_t mp_reset = 0;               // +0x344 (bool): a load that reads 1 exits the process

	// MULTIPLAYER: the switch keys
	int32_t mp_eula_accepted = 0;  // +0x348
	int32_t mpattrib = 0;          // +0x34C
	std::string join_password;     // +0x361 [17], strncpy 16
	std::string side_password;     // +0x372 [17]; written, never read
	std::string game_name;         // +0x3A5 [32], unbounded copy, an empty value ignored
	std::string internet_address;  // +0x3CD [35], unbounded copy
	int32_t networkconnecttype = 0; // +0x480
	int32_t mp_difficulty = 0;     // +0x484
	int32_t mp_gametype = 0;       // +0x3F0
	int32_t dedicated = 0;         // +0x400
	int32_t max_team_lives = 0;    // +0x404
	int32_t max_kills = 0;         // +0x408
	int32_t max_score = 0;         // +0x40C
	int32_t side_req = 0;          // +0x424; written, never read
	int32_t startdelay = 0;        // +0x410, clamped 0..300 at read
	int32_t destroybuild = 0;      // +0x414
	int32_t autobalance_on_recycle_enabled = 0;     // +0x528
	int32_t autobalance_on_recycle_diff_min = 0;    // +0x52C
	int32_t autobalance_on_recycle_diff_max = 0;    // +0x530
	int32_t oneshotonekill = 0;    // +0x534
	int32_t fatbullets = 0;        // +0x538
	int32_t nwisptype = 0;         // +0x540, below 0 -> 18, above 18 -> 0 at read
	int32_t lanmode = 0;           // +0x544, not 1..4 -> 2 at read
	int32_t allowcustomskins = 0;  // +0x524
	std::string servermsg;         // +0x560 [128], Napi_CopyString
	int32_t minping = 0;           // +0x548
	int32_t dominpingcheck = 0;    // +0x54C
	int32_t maxping = 0;           // +0x550
	int32_t domaxpingcheck = 0;    // +0x554
	int32_t dirtyupstream = 0;     // +0x558
	int32_t dirtyupstreampost = 0; // +0x55C
	int32_t deathmes = 0;          // +0x418
	int32_t numallowablefriendlykills = 0; // +0x5E0
	int32_t replay = 0;            // +0x46C
	int32_t time_limit = 0;        // +0x470
	int32_t koth_limit = 0;        // +0x474
	int32_t koth_delta = 0;        // +0x478
	int32_t timeout = 0;           // +0x47C
	int32_t mp_numteams = 0;       // +0x488
	int32_t contest = 0;           // +0x4B0
	int32_t flag_return_time = 0;  // +0x41C: read `mp_flagreturntime`, written `flagreturntime`
	int32_t flag_reset_time = 0;   // +0x420: read `mp_flagresettime`; never written
	int32_t ping = 0;              // +0x4B8
	int32_t sendplayerlist = 0;    // +0x4BC
	int32_t rememberlogin = 0;     // +0x4D4
	int32_t teamchange_time = 0;   // +0x4C0
	int32_t mp_lfp_takeoverspeed = 0; // +0x4CC
	int32_t spawnregulator_psp = 0;   // +0x4C4
	int32_t spawnregulator_lfp = 0;   // +0x4C8
	int32_t unlimited_vehicles = 0;   // +0x4D0
	int32_t choosespawnonbegin = 0;   // +0x4D8
	int32_t nodefaultspawnpoints = 0; // +0x4DC
	int32_t mp_allowsniperscopezoom = 0; // +0x5EC
	int32_t mp_permanent_death = 0;   // +0x5E4
	int32_t mp_3rdperson_driver = 0;  // +0x5E8
	int32_t mpvoting = 0;             // +0x4E0
	int32_t mpvoting_min_players = 0; // +0x4E4
	float mpvoting_percent = 0.0f;    // +0x4E8
	int32_t mpvoting_period = 0;      // +0x4EC
	int32_t mpchangeteam = 0;         // +0x4F0
	int32_t mpchangeteam_interval = 0; // +0x4F4
	int32_t mpchangeteam_penalty = 0;  // +0x4F8
	int32_t mp_verbose = 0;           // +0x51C
	int32_t mp_no_char_abilities = 0;    // `mp_NoCharAbilities` +0x48C
	int32_t mp_no_crosshair_spread = 0;  // `mp_NoCrossHairSpread` +0x498
	int32_t mp_no_scope_drift = 0;       // `mp_NoScopeDrift` +0x494
	int32_t mp_no_weapon_recoil = 0;     // `mp_NoWeaponRecoil` +0x490
	int32_t mp_gpsicons = 0;             // +0x49C
	int32_t mp_wind = 0;                 // +0x4A0
	int32_t mp_dropped_weapon_disappear = 0; // `mp_DroppedWeaponDisappear` +0x4A4
	int32_t mp_no_drop_weapons = 0;      // `mp_NoDropWeapons` +0x4A8
	int32_t mp_no_respawn_with_primary = 0; // `mp_NoRespawnWithPrimary` +0x4AC
	int32_t enable_ai = 0;            // +0xA14
	uint16_t remote_admin_port = 0;   // +0xA18 (a 16-bit store, written %u)
	int32_t cl_punkbuster = 0;        // +0xA1C
	int32_t sv_punkbuster = 0;        // +0xA20
	int32_t xhair_appearance = 0;     // +0xA24
	int32_t xhair_color = 0;          // +0xA28
	int32_t xhair_spread = 0;         // +0xA2C
	int32_t balance_join = 0;         // +0xA30
	float balance_join_percent = 0.0f; // +0xA34
	int32_t armory_reuse_time = 0;    // +0xA38
	int32_t farp_reuse_time = 0;      // +0xA3C

	// MULTIPLAYER CLASS / WEAPON AVAILABILITY
	int32_t class_availability[kClassAvailabilityCount] = {};   // +0x9EC
	int32_t weapon_availability[kWeaponAvailabilityCount] = {}; // +0x5F0, by weapon.def row

	// MAP
	int32_t map_infrared = 0; // +0x1BC
	int32_t map_iff = 0;      // +0x1C0
	int32_t map_awac = 0;     // `map_AWAC` +0x1C4

	// SYSTEM
	int32_t player_index = 0;      // +0x000; the defaults keep it
	int32_t preempt_pff = 0;       // +0x1A0
	std::string country;           // +0x3C5 [8], unbounded copy
	std::string msg;               // +0x4FC [24], unbounded copy
	int32_t play_exit_credits = 0; // +0x008
	int32_t no_anim = 0;           // +0x098
	int32_t alt_lock = 0;          // `AltLock` +0x180
	int32_t alt_lock_type = 0;     // `AltLockType` +0x184
};

// The availability word weapon.def row `row` addresses. Retail indexes the
// 255-word array with the row number unchecked, so rows 255..264 land on the
// class words that follow it (+0x5F0 + 4 * 255 = +0x9EC); null for a row past
// those, which retail would write further into the block (D-GAMECFG-1)
// [orig: Config_ParseSettingsLine @0x54FA5A; Game_SaveConfig @0x54CEF8].
int32_t *weapon_slot(GameCfg &cfg, int row);
const int32_t *weapon_slot(const GameCfg &cfg, int row);

// The localized strings Config_SetDefaults copies, looked up in gametext.bin
// with these fallbacks when the table is not loaded or lacks the key, as at the
// boot read in Game_Run [orig: GameText_GetStringWithFallback @0x51EB90 from
// Config_SetDefaults @0x54D1A1 (Menu/UNTITLED, "!Untitled") and @0x54D240
// (Menu/USERMESSAGE, "!Put your message here")].
struct DefaultTexts {
	std::string untitled = "!Untitled";
	std::string user_message = "!Put your message here";
};

// One row of the MULTIPLAYER cfg table [orig: g_ConfigVarTable @0x833050,
// 24-byte NapiConfigVarDescriptor rows {name, storage, type, size,
// default source (1 = a string), default}, NULL-name terminated @0x8333E0].
// The first row is the section header: no storage, so the writer prints it as
// a comment and an assignment to it does nothing.
enum class ConfigVarType : int32_t {
	None = 0,     // the header row
	String = 1,   // Napi_CopyString into `size` bytes
	Int32 = 2,    // atol (the signed spelling)
	UInt32 = 3,   // atol (the unsigned spelling; both print %ld)
	Bool = 10,    // atol != 0
};
struct ConfigVarRow {
	const char *name;
	ConfigVarType type;
	int32_t size;
	const char *default_value;
	int32_t GameCfg::*int_field;
	std::string GameCfg::*string_field;
};
extern const ConfigVarRow kConfigVarRows[];
extern const size_t kConfigVarRowCount;

// Config_SetDefaults: every field zeroed, then the cfg table's defaults, then
// the witnessed constants and the two localized strings. `player_index` and
// `hw3d_deviceno` are the values the defaults keep (retail saves and restores
// the first; the second lives outside the reset block).
// [orig: Config_SetDefaults @0x54D030]
GameCfg defaults(const DefaultTexts &texts = {}, int32_t player_index = 0, int32_t hw3d_deviceno = 0);

// Config_ParseSettingsLine: applies one tokenized line to `cfg`; a line whose
// key nothing accepts changes nothing. [orig: Config_ParseSettingsLine @0x54F740]
void apply_line(GameCfg &cfg, const io::ConfigTokens &line, const WeaponRoster &weapons);

// The read of one file's text: every line the walk hands its callback,
// through apply_line, onto `cfg` as it stands (no defaults, no version check).
// [orig: File_ParseASCIIFileWithCallback @0x53D980 with Config_ParseSettingsLine]
void parse(const char *text, size_t size, GameCfg &cfg, const WeaponRoster &weapons);

// Settings_ClampGraphicsOptions: the graphics levels, the gamma and
// mpmaxplayers into their ranges. [orig: Settings_ClampGraphicsOptions @0x54D4A0]
void clamp_graphics_options(GameCfg &cfg);

struct LoadOptions {
	DefaultTexts texts;
	WeaponRoster weapons;
	// The values the defaults keep: the live player_index and hw3d_deviceno
	// before this read (0 and 0 in a fresh process; the `/2` switch sets the
	// device number to 1 before the first read).
	int32_t player_index = 0;
	int32_t hw3d_deviceno = 0;
	// The `/LAN` command-line switch: forces mpnovaworldhostlanonly to 1 after
	// the read [orig: Game_ParseCommandLineAndInit @0x4A74EB sets
	// dword_B4C694; Game_LoadConfig @0x5514B1..0x5514BA].
	bool lan_switch = false;
};

struct LoadResult {
	GameCfg cfg;
	bool file_read = false;        // the file opened
	bool version_reset = false;    // the version was not 29: the defaults replaced the read
	// The file set mpreset: retail exits the process with code 0 before the
	// version check [orig: Game_LoadConfig @0x5514A1..0x5514AC]; `cfg` is the
	// state at that exit and the caller decides.
	bool reset_exit = false;
};

// Game_LoadConfig over a file's text; `text` null is a file that does not
// open (the defaults, silently). [orig: Game_LoadConfig @0x551480]
LoadResult load(const char *text, size_t size, const LoadOptions &options);

// load() over the file at `path` (a missing file loads the defaults).
LoadResult load_file(const std::string &path, const LoadOptions &options);

// Game_SaveConfig: the whole file, CR LF line ends.
// [orig: Game_SaveConfig @0x54C490]
std::string write(const GameCfg &cfg, const WeaponRoster &weapons);

// write() to `path`, replacing it. False (with `error`) when it cannot open.
bool save_file(const std::string &path, const GameCfg &cfg, const WeaponRoster &weapons, std::string &error);

// The CRT's fixed-point printf of a double (`%.<precision>f`), as the game's
// statically linked CRT formats it: 17 significant digits, then rounded half
// up at the requested place, the infinities and NaNs as their `1#INF` digit
// strings. [orig: __cftof_l @0x778B43 -> __fltout2 @0x788E2D (_I10_OUTPUT
// @0x78B319, 17 digits) -> _fptostr @0x788CB5 (`>= '5'` rounds up)]
std::string format_fixed(double value, int precision);

// activesrvr.txt: the directory-lock marker the host-file path writes in the
// working directory before it hosts and Game_Run deletes at every orderly exit.
// Nothing reads it. [orig: Game_HostMultiplayerSession @0x4A65C1..0x4A65F6:
// DeleteFileA, fopen "wb", this one line; Game_Run @0x4A8009..0x4A800E]
inline constexpr const char *kActiveServerMarkerFileName = "activesrvr.txt";
inline constexpr const char *kActiveServerMarkerText =
		"\nThis directory has a server running in it that did not exit cleanly or is running\n";

// Deletes `path` and writes the marker text there, binary (LF only). False
// when it cannot open.
bool write_active_server_marker(const std::string &path);

// Deletes the marker at `path` (a missing file is not an error, as
// DeleteFileA's result is unchecked).
void remove_active_server_marker(const std::string &path);

} // namespace opennova::gamecfg
