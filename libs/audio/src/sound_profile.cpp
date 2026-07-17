#include "audio/sound_profile.h"

#include "io/strutil.h"

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

// Whitespace-token scan with surrounding-quote strip (begin "<name>" carries a
// quoted token; the engine's shared ASCII-file tokenizer hands the callback
// unquoted column strings).
struct LineTokens {
    std::string_view tok[5]; // keyword + up to 4 value columns
    int count = 0;
};

LineTokens tokenize(std::string_view line) {
    LineTokens out;
    size_t i = 0;
    while (i < line.size() && out.count < 5) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= line.size()) break;
        size_t start = i;
        size_t end;
        if (line[i] == '"') {
            ++start;
            end = start;
            while (end < line.size() && line[end] != '"') ++end;
            i = (end < line.size()) ? end + 1 : end;
        } else {
            end = i;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') ++end;
            i = end;
        }
        out.tok[out.count++] = line.substr(start, end - start);
    }
    return out;
}

int32_t parse_q16(std::string_view v) {
    if (v.empty()) return 0;
    // atof then x65536 [orig: fmul dbl_7C3CC0 (65536.0) + ftol @ 0x527122].
    return static_cast<int32_t>(std::atof(std::string(v).c_str()) * 65536.0);
}

int32_t parse_pct(std::string_view v) {
    if (v.empty()) return 0;
    // atof then x655 (~65536/100) [orig: the 655 * ftol stores @ 0x5270dd..].
    return static_cast<int32_t>(std::atof(std::string(v).c_str())) * 655;
}

} // namespace

const char *sound_profile_slot_keyword(int slot) {
    if (slot < 0 || slot >= kSoundProfileSlotCount) return nullptr;
    return kSlotKeywords[slot];
}

size_t SoundProfileTable::parse(const char *text, size_t len) {
    const size_t before = entries_.size();
    if (text == nullptr) return 0;
    std::string_view src(text, len);
    SoundProfile *cur = nullptr; // inside a begin..end block [orig: dword_24E0894]
    size_t pos = 0;
    while (pos <= src.size()) {
        size_t eol = src.find('\n', pos);
        if (eol == std::string_view::npos) eol = src.size();
        const LineTokens t = tokenize(src.substr(pos, eol - pos));
        pos = eol + 1;
        if (t.count == 0) continue;
        const std::string_view key = t.tok[0];
        // "end" closes the block; every other keyword outside a begin is
        // ignored [orig: the "end" stricmp first, then the in-block gate
        // @ 0x526fe0/0x52707a].
        if (strutil::iequals(key, "end")) {
            cur = nullptr;
            continue;
        }
        if (strutil::iequals(key, "begin")) {
            entries_.emplace_back();
            cur = &entries_.back();
            // Name cap: the engine truncates a >=64-char begin name to the
            // 64-byte profile name buffer [orig: the strlen >= 0x40 poke
            // @ 0x527043]. Trailing columns are free comment text, unread.
            std::string_view name = t.tok[1];
            if (name.size() > 63) name = name.substr(0, 63);
            cur->name.assign(name);
            continue;
        }
        if (cur == nullptr) continue;
        bool matched = false;
        for (int slot = 0; slot < kSoundProfileSlotCount; ++slot) {
            if (!strutil::iequals(key, kSlotKeywords[slot])) continue;
            // Column 1 = the sound-set name (24-byte engine slot), columns
            // 2/3 floats x65536, column 4 atol [orig: @ 0x5270f0-0x52718b].
            std::string_view set = t.tok[1];
            if (set.size() > 23) set = set.substr(0, 23);
            cur->set_names[slot].assign(set);
            cur->param2_q16[slot] = parse_q16(t.tok[2]);
            cur->param3_q16[slot] = parse_q16(t.tok[3]);
            cur->param4[slot] =
                t.tok[4].empty() ? 0 : static_cast<int32_t>(std::atol(std::string(t.tok[4]).c_str()));
            matched = true;
            break;
        }
        if (matched) continue;
        for (int i = 0; i < 12; ++i) {
            if (!strutil::iequals(key, kLoopKeywords[i])) continue;
            cur->loop_params[i] = parse_pct(t.tok[1]);
            break;
        }
    }
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
