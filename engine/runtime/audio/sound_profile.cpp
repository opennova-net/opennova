#include <runtime/audio/sound_profile.h>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <cstdio>
#include <cstdlib>

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
// an earlier, longer line left (io::ConfigTokens::token): JO:CA's
// SndProf.def gives some 190 bare slot lines a param3 of 1.2 that way.
size_t SoundProfileTable::parse(const char *text, size_t len) {
    const size_t before = entries_.size();
    if (text == nullptr) return 0;
    SoundProfile *cur = nullptr; // inside a begin..end block [orig: dword_24E0894]
    io::for_each_config_line(text, len, [&](const io::ConfigTokens &t) {
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
            // A name of 64 characters or more is cut to its first 64 [orig: the
            // strlen >= 0x40 test @ 0x527043, the terminator stored at [64]
            // @ 0x527045]. Further columns are free comment text, unread.
            std::string_view name = t.token(1);
            if (name.size() > 64) name = name.substr(0, 64);
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
            cur->param4[slot] = static_cast<int32_t>(std::strtol(t.token(4), nullptr, 10));
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
