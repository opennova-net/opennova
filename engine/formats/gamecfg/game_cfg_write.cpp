// game.cfg: the writer (Game_SaveConfig), the CRT's fixed-point float print it
// relies on, and the activesrvr.txt marker. Built from the model alone, never
// from read bytes (ADR 0003). The record is docs/gamecfg/game-cfg-re.md.
#include "game_cfg.h"

#include <base/io/os_path.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace opennova::gamecfg {

namespace {

// fprintf into the text: the integer and string conversions are the
// standard ones, so the host's vsnprintf prints them as the game's CRT does.
// Floats go through format_fixed instead, whose rounding the host's printf
// does not share.
void emit(std::string &out, const char *format, ...) {
	char stack[512];
	va_list args;
	va_start(args, format);
	va_list again;
	va_copy(again, args);
	const int n = std::vsnprintf(stack, sizeof(stack), format, args);
	va_end(args);
	if (n < 0) {
		va_end(again);
		return;
	}
	if (static_cast<size_t>(n) < sizeof(stack)) {
		out.append(stack, static_cast<size_t>(n));
	} else {
		std::vector<char> heap(static_cast<size_t>(n) + 1);
		std::vsnprintf(heap.data(), heap.size(), format, again);
		out.append(heap.data(), static_cast<size_t>(n));
	}
	va_end(again);
}

// _I10_OUTPUT's special digit strings for an 80-bit value with the all-ones
// exponent, from the double the CRT widens (_dtold keeps the quiet bit)
// [orig: _I10_OUTPUT @0x78B3D5..0x78B453: a clear quiet bit -> "1#SNAN"; the
// sign with only the quiet bit -> "1#IND"; no mantissa past the integer bit ->
// "1#INF"; else "1#QNAN"; the decimal point after the first character].
const char *special_digits(double value) {
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	const uint64_t mantissa = bits & ((uint64_t{1} << 52) - 1);
	const uint64_t extended = (uint64_t{1} << 63) | (mantissa << 11);
	const uint32_t hi = static_cast<uint32_t>(extended >> 32);
	const uint32_t lo = static_cast<uint32_t>(extended);
	if ((hi != 0x80000000u || lo != 0) && (hi & 0x40000000u) == 0) return "1#SNAN";
	if (std::signbit(value) && hi == 0xC0000000u && lo == 0) return "1#IND";
	if (hi == 0x80000000u && lo == 0) return "1#INF";
	return "1#QNAN";
}

// Prints `%.<precision>f` with only the integer conversions of the host.
std::string emit_float(const char *prefix, double value, int precision) {
	return std::string(prefix) + format_fixed(value, precision) + "\n";
}

} // namespace

std::string format_fixed(double value, int precision) {
	// __fltout2: the sign, the decimal-point position and the digits, 17
	// significant [orig: __fltout2 @0x788E2D, _I10_OUTPUT(…, 17, 0) @0x788E74].
	const bool negative = std::signbit(value);
	std::string digits;
	int decpt = 0;
	if (std::isnan(value) || std::isinf(value)) {
		digits = special_digits(value);
		decpt = 1;
	} else if (value == 0.0) {
		digits = "0"; // _I10_OUTPUT's zero: exponent 0, the one digit "0" @0x78B3AA..0x78B3C4
		decpt = 0;
	} else {
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.16e", std::fabs(value));
		// "d.dddddddddddddddde±XX"
		digits.push_back(buf[0]);
		const char *p = buf + 2;
		while (*p >= '0' && *p <= '9') digits.push_back(*p++);
		const int exponent = (*p == 'e') ? std::atoi(p + 1) : 0;
		decpt = exponent + 1;
	}

	// _fptostr: a leading '0', `precision + decpt` digit characters (padded
	// with '0' past the string), then a next character of '5' or more rounds
	// the copied run up, carrying through '9's into the leading '0'; a carry
	// that reached it moves the decimal point right, else the '0' is dropped
	// [orig: _fptostr @0x788CB5: the copy @0x788D1A..0x788D2F, the `>= '5'`
	// test @0x788D3D, the carry @0x788D41..0x788D4A, the '1' test @0x788D4F].
	const int count = precision + decpt;
	std::string run = "0";
	size_t next = 0;
	for (int i = 0; i < count; ++i) {
		if (next < digits.size())
			run.push_back(digits[next++]);
		else
			run.push_back('0');
	}
	const char following = next < digits.size() ? digits[next] : 0;
	if (count >= 0 && following >= '5') {
		size_t at = run.size() - 1;
		while (run[at] == '9') run[at--] = '0';
		run[at] = static_cast<char>(run[at] + 1);
	}
	if (run[0] == '1')
		++decpt;
	else
		run.erase(0, 1);

	// __cftof2_l: the sign, the integer digits (or "0"), and with a precision
	// the point, the zeros a negative exponent leaves (at most `precision`),
	// and the rest [orig: __cftof2_l @0x778A4E: the sign @0x778AC4, the "0"
	// shift @0x778AD8, the point @0x778AED..0x778AFF, the zero fill
	// @0x778B07..0x778B27].
	std::string out;
	if (negative) out.push_back('-');
	size_t used = 0;
	if (decpt > 0) {
		out.append(run, 0, static_cast<size_t>(decpt));
		used = static_cast<size_t>(decpt);
	} else {
		out.push_back('0');
	}
	if (precision > 0) {
		out.push_back('.');
		if (decpt < 0) {
			const int zeros = -decpt < precision ? -decpt : precision;
			out.append(static_cast<size_t>(zeros), '0');
		}
		if (used < run.size()) out.append(run, used, std::string::npos);
	}
	return out;
}

std::string write(const GameCfg &cfg, const WeaponRoster &weapons) {
	// [orig: Game_SaveConfig @0x54C490]: every format below is the literal the
	// call at that site pushes; `\n` becomes CR LF at the end, the "w" stream's
	// text mode (fopen @0x54C4AF).
	std::string out;
	emit(out, "// %s\n", "game.cfg"); // @0x54C4C2
	emit(out, "//\n");
	emit(out, "\n");
	emit(out, "// GENERAL\n");
	emit(out, "version           = %i\n", cfg.version); // @0x54C4F9
	emit(out, "\n");
	emit(out, "// DISPLAY\n");
	emit(out, "windowed             = %i\n", cfg.windowed);
	emit(out, "hw3d_deviceno        = %i\n", cfg.hw3d_deviceno);
	emit(out, "hw3d_name            = \"%s\"\n", cfg.hw3d_name.c_str());
	emit(out, "hw3d_guid            = \"%s\"\n", cfg.hw3d_guid.c_str());
	emit(out, "video_res            = %s\n", cfg.video_res.c_str());
	out += emit_float("gamma                = ", cfg.gamma, 1); // "%.1f" @0x54C57D, of flt_B4C298
	emit(out, "terrain_polydetail   = %i\n", cfg.terrain_polydetail);
	emit(out, "terrain_texdetail    = %i\n", cfg.terrain_texdetail);
	emit(out, "object_polydetail    = %i\n", cfg.object_polydetail);
	emit(out, "object_texdetail     = %i\n", cfg.object_texdetail);
	emit(out, "display_16x9         = %i\n", cfg.display_16x9);
	emit(out, "water_quality        = %i\n", cfg.water_quality);
	emit(out, "shadow_quality       = %i\n", cfg.shadow_quality);
	emit(out, "particle_density     = %i\n", cfg.particle_density);
	emit(out, "antialias_mode       = %i\n", cfg.antialias_mode);
	emit(out, "texfilter_level      = %i\n", cfg.texfilter_level);
	emit(out, "fbeffects_level      = %i\n", cfg.fbeffects_level);
	emit(out, "shader_usage_level   = %i\n", cfg.shader_usage_level);
	emit(out, "texcompression_level = %i\n", cfg.texcompression_level);
	emit(out, "lock_framerate       = %i\n", cfg.lock_framerate);
	emit(out, "force_vsync          = %i\n", cfg.force_vsync);
	emit(out, "reduce_mouselag      = %i\n", cfg.reduce_mouselag);
	emit(out, "enable_keyboardtips  = %i\n", cfg.enable_keyboardtips);
	emit(out, "enable_gameplaytips  = %i\n", cfg.enable_gameplaytips);
	emit(out, "\n");
	emit(out, "// AUDIO\n"); // @0x54C6DA
	emit(out, "windows_volume       = %i\n", cfg.windows_volume);
	emit(out, "music_volume         = %i\n", cfg.music_volume);
	emit(out, "SFXLevel             = %i\n", cfg.sfx_level);
	emit(out, "DialogueLevel        = %i\n", cfg.dialogue_level);
	emit(out, "rotor_volume         = %i\n", cfg.rotor_volume);
	emit(out, "audio_channels       = %i\n", cfg.audio_channels);
	emit(out, "audio_rate           = %i\n", cfg.audio_rate);
	emit(out, "NoBlood              = %i\n", cfg.no_blood);
	emit(out, "NoCasings            = %i\n", cfg.no_casings);
	emit(out, "NoSmoke              = %i\n", cfg.no_smoke);
	emit(out, "\n");
	emit(out, "// CONTROLS\n"); // @0x54C7A6
	emit(out, "crosshairs           = %i\n", cfg.crosshairs);
	emit(out, "crosshairs_color     = %i\n", cfg.crosshairs_color);
	emit(out, "hitfeedback          = %i\n", cfg.hitfeedback);
	emit(out, "showgun              = %i\n", cfg.showgun);
	emit(out, "hud_color_index      = %i\n", cfg.hud_color_index);
	emit(out, "hud_detail           = %i\n", cfg.hud_detail);
	emit(out, "\n");

	// The cfg table: a row with no storage is a `// <name>` comment, every
	// other row `<name> = "<value>"`, the value as its type prints it
	// [orig: @0x54C82C..0x54C87B; NapiConfigVar_FormatValueToString @0x635370:
	// String copies, Int32/UInt32 "%ld" @0x635402, Bool "1"/"0" @0x63551B].
	for (size_t i = 0; i < kConfigVarRowCount; ++i) {
		const ConfigVarRow &row = kConfigVarRows[i];
		switch (row.type) {
			case ConfigVarType::None:
				emit(out, "// %s\n", row.name);
				break;
			case ConfigVarType::String:
				emit(out, "%s = \"%s\"\n", row.name, (cfg.*row.string_field).c_str());
				break;
			case ConfigVarType::Int32:
			case ConfigVarType::UInt32:
				emit(out, "%s = \"%ld\"\n", row.name, static_cast<long>(cfg.*row.int_field));
				break;
			case ConfigVarType::Bool:
				emit(out, "%s = \"%s\"\n", row.name, (cfg.*row.int_field) != 0 ? "1" : "0");
				break;
		}
	}

	emit(out, "mp_eula_accepted        = %i\n", cfg.mp_eula_accepted); // @0x54C892
	emit(out, "mpattrib                = %i\n", cfg.mpattrib);
	emit(out, "join_password           = \"%s\"\n", cfg.join_password.c_str());
	emit(out, "side_password           = \"%s\"\n", cfg.side_password.c_str());
	emit(out, "game_name               = \"%s\"\n", cfg.game_name.c_str());
	emit(out, "internet_address        = \"%s\"\n", cfg.internet_address.c_str());
	emit(out, "networkconnecttype      = %i\n", cfg.networkconnecttype);
	emit(out, "mp_difficulty           = %i\n", cfg.mp_difficulty);
	emit(out, "mp_gametype             = %i\n", cfg.mp_gametype);
	emit(out, "dedicated               = %i\n", cfg.dedicated); // @0x54C92D
	emit(out, "max_team_lives          = %i\n", cfg.max_team_lives);
	emit(out, "max_kills               = %i\n", cfg.max_kills);
	emit(out, "max_score               = %i\n", cfg.max_score);
	emit(out, "side_req                = %i\n", cfg.side_req);
	emit(out, "startdelay              = %i\n", cfg.startdelay);
	emit(out, "destroybuild            = %i\n", cfg.destroybuild);
	emit(out, "autobalanceonmissionrecycleenabled = %ld\n", static_cast<long>(cfg.autobalance_on_recycle_enabled));
	emit(out, "autobalanceonmissionrecyclenumplayerdiffmin = %ld\n", static_cast<long>(cfg.autobalance_on_recycle_diff_min));
	emit(out, "autobalanceonmissionrecyclenumplayerdiffmax = %ld\n", static_cast<long>(cfg.autobalance_on_recycle_diff_max));
	emit(out, "oneshotonekill          = %ld\n", static_cast<long>(cfg.oneshotonekill));
	emit(out, "fatbullets              = %ld\n", static_cast<long>(cfg.fatbullets));
	emit(out, "nwisptype               = %ld\n", static_cast<long>(cfg.nwisptype));
	emit(out, "lanmode                 = %ld\n", static_cast<long>(cfg.lanmode));
	emit(out, "allowcustomskins        = %ld\n", static_cast<long>(cfg.allowcustomskins));
	emit(out, "servermsg               = \"%s\"\n", cfg.servermsg.c_str());
	emit(out, "minping                 = %ld\n", static_cast<long>(cfg.minping));
	emit(out, "dominpingcheck          = %ld\n", static_cast<long>(cfg.dominpingcheck));
	emit(out, "maxping                 = %ld\n", static_cast<long>(cfg.maxping));
	emit(out, "domaxpingcheck          = %ld\n", static_cast<long>(cfg.domaxpingcheck));
	emit(out, "dirtyupstream           = %ld\n", static_cast<long>(cfg.dirtyupstream));
	emit(out, "dirtyupstreampost       = %ld\n", static_cast<long>(cfg.dirtyupstreampost));
	emit(out, "deathmes                = %i\n", cfg.deathmes);
	emit(out, "numallowablefriendlykills = %ld\n", static_cast<long>(cfg.numallowablefriendlykills));
	emit(out, "replay                  = %i\n", cfg.replay);
	emit(out, "time_limit              = %i\n", cfg.time_limit);
	emit(out, "koth_limit              = %i\n", cfg.koth_limit);
	emit(out, "koth_delta              = %i\n", cfg.koth_delta);
	emit(out, "timeout                 = %i\n", cfg.timeout);
	emit(out, "mp_numteams             = %i\n", cfg.mp_numteams);
	emit(out, "contest                 = %i\n", cfg.contest);
	// The reader's key is mp_flagreturntime: this line is never read back
	// [orig: "flagreturntime" @0x54CB5E vs the compare @0x550665].
	emit(out, "flagreturntime          = %i\n", cfg.flag_return_time);
	emit(out, "ping                    = %i\n", cfg.ping);
	emit(out, "sendplayerlist          = %ld\n", static_cast<long>(cfg.sendplayerlist));
	emit(out, "rememberlogin           = %i\n", cfg.rememberlogin);
	emit(out, "teamchange_time         = %ld\n", static_cast<long>(cfg.teamchange_time));
	emit(out, "mp_lfp_takeoverspeed    = %ld\n", static_cast<long>(cfg.mp_lfp_takeoverspeed));
	emit(out, "spawnregulator_psp      = %ld\n", static_cast<long>(cfg.spawnregulator_psp));
	emit(out, "spawnregulator_lfp      = %ld\n", static_cast<long>(cfg.spawnregulator_lfp));
	emit(out, "unlimited_vehicles      = %ld\n", static_cast<long>(cfg.unlimited_vehicles));
	emit(out, "choosespawnonbegin      = %ld\n", static_cast<long>(cfg.choosespawnonbegin));
	emit(out, "nodefaultspawnpoints    = %ld\n", static_cast<long>(cfg.nodefaultspawnpoints));
	emit(out, "mp_allowsniperscopezoom = %i\n", cfg.mp_allowsniperscopezoom);
	emit(out, "mp_permanent_death      = %i\n", cfg.mp_permanent_death);
	emit(out, "mp_3rdperson_driver     = %i\n", cfg.mp_3rdperson_driver);
	emit(out, "mpvoting                = %ld\n", static_cast<long>(cfg.mpvoting));
	emit(out, "mpvoting_min_players    = %ld\n", static_cast<long>(cfg.mpvoting_min_players));
	out += emit_float("mpvoting_percent        = ", cfg.mpvoting_percent, 6); // "%f" @0x54CC87
	emit(out, "mpvoting_period         = %ld\n", static_cast<long>(cfg.mpvoting_period));
	emit(out, "mpchangeteam            = %ld\n", static_cast<long>(cfg.mpchangeteam));
	emit(out, "mpchangeteam_interval   = %ld\n", static_cast<long>(cfg.mpchangeteam_interval));
	emit(out, "mpchangeteam_penalty    = %ld\n", static_cast<long>(cfg.mpchangeteam_penalty));
	emit(out, "mp_verbose              = %i\n", cfg.mp_verbose);
	emit(out, "mp_NoCharAbilities      = %i\n", cfg.mp_no_char_abilities);
	emit(out, "mp_NoCrossHairSpread    = %i\n", cfg.mp_no_crosshair_spread);
	emit(out, "mp_NoScopeDrift         = %i\n", cfg.mp_no_scope_drift);
	emit(out, "mp_NoWeaponRecoil       = %i\n", cfg.mp_no_weapon_recoil);
	emit(out, "mp_gpsicons             = %i\n", cfg.mp_gpsicons);
	emit(out, "mp_wind                 = %i\n", cfg.mp_wind);
	emit(out, "mp_DroppedWeaponDisappear = %i\n", cfg.mp_dropped_weapon_disappear);
	emit(out, "mp_NoDropWeapons        = %i\n", cfg.mp_no_drop_weapons);
	emit(out, "mp_NoRespawnWithPrimary = %i\n", cfg.mp_no_respawn_with_primary);
	emit(out, "enable_ai               = %i\n", cfg.enable_ai);
	// movzx of the 16-bit word [orig: @0x54CDA1..0x54CDAF]
	emit(out, "remote_admin_port       = %u\n", static_cast<unsigned>(cfg.remote_admin_port));
	emit(out, "cl_punkbuster           = %i\n", cfg.cl_punkbuster);
	emit(out, "sv_punkbuster           = %i\n", cfg.sv_punkbuster);
	emit(out, "xhair_appearance        = %i\n", cfg.xhair_appearance);
	emit(out, "xhair_color             = %i\n", cfg.xhair_color);
	emit(out, "xhair_spread            = %i\n", cfg.xhair_spread);
	emit(out, "balance_join            = %i\n", cfg.balance_join);
	out += emit_float("balance_join_percent    = ", cfg.balance_join_percent, 6); // "%f" @0x54CE30
	emit(out, "armory_reuse_time       = %i\n", cfg.armory_reuse_time);
	emit(out, "farp_reuse_time         = %i\n", cfg.farp_reuse_time);

	// [orig: @0x54CE5E..0x54CEBA]
	emit(out, "\n// MULTIPLAYER CLASS AVAILABILITY (0=NEVER, 1=ALWAYS, 2=MISSION DEFAULT)\n");
	emit(out, "avail_class_rifleman    = %i\n", cfg.class_availability[kClassRifleman]);
	emit(out, "avail_class_sniper      = %i\n", cfg.class_availability[kClassSniper]);
	emit(out, "avail_class_medic       = %i\n", cfg.class_availability[kClassMedic]);
	emit(out, "avail_class_gunner      = %i\n", cfg.class_availability[kClassGunner]);
	emit(out, "avail_class_engineer    = %i\n", cfg.class_availability[kClassEngineer]);

	// Every loadout_selectable weapon.def row, its name from the fifth
	// character [orig: @0x54CEC5..0x54CF0F, "avail_wpn_%-16s = %d" @0x54CEF2].
	// A name shorter than four characters prints as empty here, where retail
	// reads past its NUL (D-GAMECFG-1).
	emit(out, "\n// MULTIPLAYER WEAPON AVAILABILITY (0=NEVER, 1=ALWAYS, 2=ARMORY ONLY, 3=MISSION DEFAULT)\n");
	for (size_t i = 0; i < weapons.size(); ++i) {
		if (weapons[i].loadout_selectable == 0) continue;
		const int32_t *slot = weapon_slot(cfg, static_cast<int>(i));
		if (slot == nullptr) continue;
		const std::string &name = weapons[i].name;
		const char *tail = name.size() >= 4 ? name.c_str() + 4 : "";
		emit(out, "avail_wpn_%-16s = %d\n", tail, *slot);
	}
	emit(out, "\n");
	emit(out, "// MAP\n"); // @0x54CF1D
	emit(out, "map_infrared            = %i\n", cfg.map_infrared);
	emit(out, "map_iff                 = %i\n", cfg.map_iff);
	emit(out, "map_AWAC                = %i\n", cfg.map_awac);
	emit(out, "\n");
	emit(out, "// SYSTEM\n"); // @0x54CF68
	emit(out, "player_index            = %i\n", cfg.player_index);
	emit(out, "preempt_pff             = %i\n", cfg.preempt_pff);
	emit(out, "country                 = \"%s\"\n", cfg.country.c_str());
	emit(out, "msg                     = \"%s\"\n", cfg.msg.c_str());
	emit(out, "play_exit_credits       = %i\n", cfg.play_exit_credits);
	emit(out, "no_anim                 = %i\n", cfg.no_anim);
	emit(out, "AltLock                 = %i\n", cfg.alt_lock);
	emit(out, "AltLockType             = %i\n", cfg.alt_lock_type);
	emit(out, "\n"); // @0x54D003

	std::string text;
	text.reserve(out.size() + out.size() / 16);
	for (const char c : out) {
		if (c == '\n') text.push_back('\r');
		text.push_back(c);
	}
	return text;
}

bool save_file(const std::string &path, const GameCfg &cfg, const WeaponRoster &weapons, std::string &error) {
	// fopen "w": a file that does not open writes nothing [orig: @0x54C4AF..
	// 0x54C4BB]. The text is already CR LF, so it goes out in binary.
	std::FILE *f = io::fopen_utf8(path.c_str(), "wb");
	if (f == nullptr) {
		error = "cannot open " + path + " for writing";
		return false;
	}
	const std::string text = write(cfg, weapons);
	const size_t put = std::fwrite(text.data(), 1, text.size(), f);
	const bool closed = std::fclose(f) == 0;
	if (put != text.size() || !closed) {
		error = "short write to " + path;
		return false;
	}
	return true;
}

void remove_active_server_marker(const std::string &path) {
	// DeleteFileA, its result unchecked [orig: Game_Run @0x4A800E]
	std::error_code ec;
	std::filesystem::remove(io::os_path(path), ec);
}

bool write_active_server_marker(const std::string &path) {
	// [orig: Game_HostMultiplayerSession @0x4A65C1..0x4A65F1: DeleteFileA,
	// fopen "wb", fprintf of the line, fclose]
	remove_active_server_marker(path);
	std::FILE *f = io::fopen_utf8(path.c_str(), "wb");
	if (f == nullptr) return false;
	const size_t len = std::strlen(kActiveServerMarkerText);
	const bool ok = std::fwrite(kActiveServerMarkerText, 1, len, f) == len;
	return std::fclose(f) == 0 && ok;
}

} // namespace opennova::gamecfg
