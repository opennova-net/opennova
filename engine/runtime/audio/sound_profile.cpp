#include <runtime/audio/sound_profile.h>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace opennova::audio {

namespace {

// The 51 slot keywords, index == slot [orig: the pair table @ 0x82F3B0
// (name ptr, slot index) — the index column runs 0..50 in table order, so the
// table is a plain name list; strings @ 0x7d0598-0x7d07d0 / 0x7c865c-0x7c86a8].
const char *const kSlotKeywords[kSoundProfileSlotCount] = {
    "Soundloop_1", "Soundloop_2", "Soundloop_3", "Soundloop_4", "Soundloop_5",
    "Soundloop_6", "Soundloop_7", "sounddeath", "SSNightDead",
    "door_open_sound_id", "door_close_sound_id", "dawnshot", "dayshot",
    "duskshot", "nightshot", "SSFallDead", "SSFallAlive", "SSLFootGND",
    "SSRFootGND", "SSLFootSnow", "SSRFootSnow", "SSLFootOBJ", "SSRFootOBJ",
    "SSFootWater", "SSAudio1", "SSAudio2", "SSAudio3", "SSAudio4", "SSAudio5",
    "SSAudio6", "enginestart", "enginestop", "enginereverse", "enginehighrev",
    "warning", "sound_impact", "sound_land", "sound_rollover",
    "sound_impactorganic", "sound_impactwater", "rotor_impact", "ChuteOpen",
    "ChuteClose", "ChuteFlap", "FreeFall", "drive_repeat", "swivel_shift",
    "tumble_hithard", "tumble_hitmed", "tumble_hitsoft", "tumble_skid",
};

// The 12 percent keywords, index == SoundProfile::loop_params slot
// [orig: the keyword chain @ 0x5270a9-0x527481 stores dwords 220-231 in this
// order].
const char *const kLoopKeywords[12] = {
    "medloopfadeinstart", "medloopfadeinend", "medloopfadeoutstart",
    "medloopfadeoutend",  "medlooppitchstart", "medlooppitchend",
    "medlooppitchstartp", "medlooppitchendp",  "crsloopfadeinstart",
    "crsloopfadeinend",   "crslooppitchstartp", "crslooppitchendp",
};

// atof then x65536, through _ftol2_sse [orig: fmul dbl_7C3CC0 (65536.0) +
// _ftol2_sse @ 0x527122..0x527140].
int32_t parse_q16(const char *v) {
    return io::retail_ftol_sse2(io::retail_atof(v) * 65536.0);
}

// 655 x _ftol2_sse(atof) [orig: the stores @ 0x5270bd..0x5270dd and on].
int32_t parse_pct(const char *v) {
    return static_cast<int32_t>(655u * static_cast<uint32_t>(io::retail_ftol_sse2(io::retail_atof(v))));
}

} // namespace

const char *sound_profile_slot_keyword(int slot) {
    if (slot < 0 || slot >= kSoundProfileSlotCount) return nullptr;
    return kSlotKeywords[slot];
}

const char *body_model_prefix(int anim_slot) {
    // [orig: Entity_GetBodyModelPrefix @ 0x5280F0 — a null entity or a zero
    // +0x374 byte defaults to type 1; the switch maps the prefix strings
    // @ 0x7D0970-0x7D0994]
    switch (anim_slot == 0 ? 1 : anim_slot) {
        case 2: return "BM2";
        case 3: return "BM3";
        case 4: return "BM4";
        case 5: return "BM5";
        case 6:
        case 9: return "BM6";
        case 7: return "BF1";
        case 8: return "BF2";
        case 10: return "RM2";
        case 11: return "RF1";
        default: return "BM1"; // type 1 and every out-of-table value
    }
}

const char *compose_entity_sound_set(int anim_slot, int type, char *out, size_t out_size) {
    // The {suffix, type} pair table [orig: @ 0x82F548-0x82F58C; the walk
    // compares the type column and composes "%s_%s" @ 0x5281CC].
    static const char *const kTypeSuffixes[kEntitySoundTypeCount] = {
        "DEATH",          "MEDIC_REQUEST", "SURFACE_BREATH",
        "SURFACE_GASP",   "WATER_GAG",     "DEATH_K",
        "RECRUIT_ACCEPT", "RECRUIT",       "TANK_COMAND",
    };
    if (out == nullptr || out_size == 0) return out;
    if (type < 0 || type >= kEntitySoundTypeCount) {
        out[0] = '\0';
        return out;
    }
    std::snprintf(out, out_size, "%s_%s", body_model_prefix(anim_slot),
                  kTypeSuffixes[type]);
    return out;
}

// SoundProfile_LoadAll @ 0x527490 walks the file through File_ParseASCIIFile
// (@ 0x5274DD), so the lines and tokens are the shared walk's
// (io::for_each_config_line: CR LF only, the tokenizer's quotes, commas and
// comments); SoundProfile_ParseLineCallback reads each line's tokens by
// position with no count check, so a short line's tokens[4] and [5] are what
// an earlier, longer line left (io::ConfigTokens::slot). In JO:CA that changes
// 178 slot lines (79 in the base SndProf.def, 99 in jox01's) from a read of
// "": 83 take a param3 of 1.2, the rest another param3 (1.1, 1.3, 1.4, 1.6,
// -26) or a param4, among them 21 soundloop_1..3 rows (12 and 9), the loop
// pitch and gear count the vehicle sound reads.
size_t SoundProfileTable::parse(const char *text, size_t len) {
    const size_t before = entries_.size();
    if (text == nullptr) return 0;
    SoundProfile *cur = nullptr; // inside a begin..end block [orig: dword_24E0894]
    io::for_each_config_line(text, len, [&](io::ConfigTokens &t) {
        const char *key = t.tokens[0];
        // "end" closes the block; every other keyword outside a begin is
        // ignored [orig: the "end" stricmp first @ 0x526fe0, the in-block gate
        // @ 0x52707a].
        if (strutil::iequals(key, "end")) {
            cur = nullptr;
            return;
        }
        if (strutil::iequals(key, "begin")) {
            entries_.emplace_back();
            cur = &entries_.back();
            // A name of 64 characters or more is cut to its first 64 in the
            // line buffer itself, where a later short line's stale slot still
            // reads the cut (io::ConfigTokens::terminate_at) [orig: the strlen
            // >= 0x40 test @ 0x52703F..0x527043, the terminator stored at [64]
            // @ 0x527045]. Further columns are free comment text, unread.
            const char *name = t.token(1);
            if (std::strlen(name) >= 64) t.terminate_at(name + 64);
            cur->name.assign(name);
            return;
        }
        if (cur == nullptr) return;
        for (int slot = 0; slot < kSoundProfileSlotCount; ++slot) {
            if (!strutil::iequals(key, kSlotKeywords[slot])) continue;
            // Column 1 = the sound-set name (24-byte engine slot), columns
            // 2/3 floats x65536, column 4 atol [orig: @ 0x5270f0-0x52718b].
            std::string_view set = t.token(1);
            if (set.size() > 23) set = set.substr(0, 23);
            cur->set_names[slot].assign(set);
            cur->param2_q16[slot] = parse_q16(t.token(2));
            cur->param3_q16[slot] = parse_q16(t.token(3));
            cur->param4[slot] = io::retail_atol(t.token(4));
            return;
        }
        for (int i = 0; i < 12; ++i) {
            if (!strutil::iequals(key, kLoopKeywords[i])) continue;
            cur->loop_params[i] = parse_pct(t.token(1));
            return;
        }
    });
    return entries_.size() - before;
}

namespace {

// The shortest fixed-point decimal parse_q16 reads back as `value`. Sixteen
// places always do: a Q16 word over 65536 is an exact 16-place decimal.
std::string q16_text(int32_t value) {
    char text[64];
    for (int places = 0; places <= 16; ++places) {
        std::snprintf(text, sizeof(text), "%.*f", places, value / 65536.0);
        if (parse_q16(text) == value) break;
    }
    return text;
}

// A token's text in a line: quoted when the tokenizer would split or cut it
// there (a space, tab or comma; a ';' or "//" comment), else as it is.
std::string token_text(const std::string &value) {
    const bool plain = value.find_first_of(" \t,;") == std::string::npos &&
                       value.find("//") == std::string::npos;
    return plain ? value : "\"" + value + "\"";
}

// Whether the walk can carry `value` as one token: a quote would end it, a
// CR or LF the line.
bool tokenizable(const std::string &value) {
    return value.find_first_of("\"\r\n") == std::string::npos;
}

} // namespace

bool write_sound_profiles(const std::vector<SoundProfile> &profiles, std::string &out,
                          std::string &error) {
    std::string text;
    for (const SoundProfile &profile : profiles) {
        const std::string named = "Sound profile \"" + profile.name + "\"";
        if (profile.name.empty() || profile.name.size() >= 64 || !tokenizable(profile.name)) {
            error = profile.name.empty() ? "A sound profile has no name."
                                         : named + " cannot be written: a name holds at most 63 "
                                                   "characters and no quote or line break.";
            return false;
        }
        text += "begin \"" + profile.name + "\"\r\n";
        for (int slot = 0; slot < kSoundProfileSlotCount; ++slot) {
            const std::string &set = profile.set_names[slot];
            const bool params = profile.param2_q16[slot] != 0 || profile.param3_q16[slot] != 0 ||
                                profile.param4[slot] != 0;
            if (set.empty() && !params) continue;
            if (set.empty()) {
                // No set, so no column 1 for the values to follow: a bare
                // keyword line, whose column 2 reads "" (slots 0..2 reset each
                // line) and whose columns 3 and 4 read the tokenizer's slots
                // as the last longer line left them, pointing into the reused
                // line buffer past the short line's terminator. A line whose
                // first token starts with '/' is tokenized but never reaches
                // the callback, so one just ahead, its slot-3 token past where
                // the keyword line ends, carries the two values; shipped files
                // hold such slots (JO's SP_FuelTruck enginehighrev)
                // [orig: Terrain_TokenizeConfigLine @ 0x53cb71..0x53cb81;
                // File_ParseASCIIFile, the '/' test @ 0x53d91e].
                if (profile.param2_q16[slot] != 0) {
                    error = named + " gives " + kSlotKeywords[slot] +
                            " a column-2 value but no sound set: the file reads column 2 "
                            "only after a set.";
                    return false;
                }
                text += "/" + std::string(std::strlen(kSlotKeywords[slot]), '-') + " - - " +
                        q16_text(profile.param3_q16[slot]) + " " +
                        std::to_string(profile.param4[slot]) + "\r\n";
                text += std::string("\t") + kSlotKeywords[slot] + "\r\n";
                continue;
            }
            if (set.size() > 23 || !tokenizable(set)) {
                error = named + "'s " + kSlotKeywords[slot] + " set \"" + set +
                        "\" cannot be written: a set name holds at most 23 characters and no "
                        "quote or line break.";
                return false;
            }
            text += std::string("\t") + kSlotKeywords[slot] + " " + token_text(set) + " " +
                    q16_text(profile.param2_q16[slot]) + " " +
                    q16_text(profile.param3_q16[slot]) + " " +
                    std::to_string(profile.param4[slot]) + "\r\n";
        }
        for (int i = 0; i < 12; ++i) {
            const int32_t value = profile.loop_params[i];
            if (value == 0) continue;
            const std::string percent = std::to_string(value / 655);
            if (value % 655 != 0 || parse_pct(percent.c_str()) != value) {
                error = named + "'s " + kLoopKeywords[i] +
                        " is no whole percent: the file stores a percent times 655.";
                return false;
            }
            text += std::string("\t") + kLoopKeywords[i] + " " + percent + "\r\n";
        }
        text += "end\r\n";
    }
    out = std::move(text);
    return true;
}

const SoundProfile *SoundProfileTable::find(const char *name) const {
    const int i = index_of(name);
    return i < 0 ? nullptr : &entries_[static_cast<size_t>(i)];
}

int SoundProfileTable::index_of(const char *name) const {
    if (entries_.empty()) return -1;
    if (name != nullptr) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (strutil::iequals(entries_[i].name, name)) return static_cast<int>(i);
        }
    }
    // Miss -> the first profile [orig: SoundProfile_FindSlotByName @ 0x526e30
    // returns the array base when no name matches].
    return 0;
}

} // namespace opennova::audio
