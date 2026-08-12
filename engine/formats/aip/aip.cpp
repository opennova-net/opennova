#include "aip/aip.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace opennova::aip {

namespace {

std::string ascii_lower(const std::string &s) {
    std::string out = s;
    for (char &c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// atoi-shape numeric read: leading sign + digits, junk tails ignored.
int32_t parse_int(const std::string &s) {
    int32_t v = 0;
    bool neg = false;
    std::size_t k = 0;
    if (k < s.size() && (s[k] == '-' || s[k] == '+')) {
        neg = s[k] == '-';
        ++k;
    }
    for (; k < s.size() && s[k] >= '0' && s[k] <= '9'; ++k) v = v * 10 + (s[k] - '0');
    return neg ? -v : v;
}

// The three witnessed float conversions all chop toward zero (the parser
// sets the x87 control word to 0xC00 before every fistp) — matched by the
// C++ double->int32 cast. [orig: AIProfile_ParseProperty fldcw/fistp sites]
int32_t deg_to_bam(const std::string &s) {
    return static_cast<int32_t>(std::atof(s.c_str()) * 11930464.0); // [orig: dbl_7C6E18]
}
int32_t secs_to_ticks(const std::string &s) {
    return static_cast<int32_t>(std::atof(s.c_str()) * 62.5); // [orig: dbl_7C3B48]
}
int32_t units_fixed(const std::string &s) {
    return static_cast<int32_t>(std::atof(s.c_str()) * 65536.0); // [orig: dbl_7C3CC0]
}

// The WEAPON_* flag token loop, shared by primary_flags/secondary_flags.
void apply_weapon_flags(uint32_t &flags, const std::vector<std::string> &toks) {
    for (std::size_t k = 2; k < toks.size(); ++k) {
        const std::string t = ascii_lower(toks[k]);
        if (t == "weapon_slow") flags |= kWeaponSlow;
        else if (t == "weapon_turret") flags |= kWeaponTurret;
        else if (t == "weapon_fast") flags |= kWeaponFast;
        else if (t == "weapon_pitchlocked") flags |= kWeaponPitchLocked;
        else if (t == "weapon_pitchlocked_minus45") flags |= kWeaponPitchLockedMinus45;
    }
}

// The EVADE_FLAGS/COMBAT_FLAGS token loop; the last three names are
// COMBAT_FLAGS-only in retail and simply never appear in evade lines.
void apply_mode_flags(uint32_t &flags, const std::vector<std::string> &toks) {
    for (std::size_t k = 2; k < toks.size(); ++k) {
        const std::string t = ascii_lower(toks[k]);
        if (t == "follow_wp") flags |= 0x1;
        else if (t == "no_action") flags |= 0x2;
        else if (t == "flee") flags |= 0x4;
        else if (t == "no_cap") flags |= 0x8;
        else if (t == "counter") flags |= 0x10;
        else if (t == "ateam") flags |= 0x20;
        else if (t == "ateam_lock") flags |= 0x40;
        else if (t == "rc_fire") flags |= 0x80;
    }
}

// One GROUND-type weapon block's key set; `which` = "primary"/"secondary".
bool apply_weapon_key(WeaponBlock &w, const std::string &key, const std::string &which,
                      const std::vector<std::string> &toks) {
    const std::string &value = toks.size() > 2 ? toks[2] : toks.back();
    if (key == which + "_weap") { w.weapon = toks.size() > 2 ? toks[2] : std::string(); return true; }
    if (key == which + "_ammo") { w.ammo = parse_int(value); return true; }
    if (key == which + "_rate") { w.rate_ticks = secs_to_ticks(value); return true; }
    if (key == which + "_fov") { w.cone_bam = deg_to_bam(value); return true; }
    if (key == which + "_range") { w.range = parse_int(value) << 16; return true; }
    if (key == which + "_facing") { w.facing_bam = deg_to_bam(value); return true; }
    if (key == which + "_pitch") { w.pitch_bam = deg_to_bam(value); return true; }
    if (key == which + "_flags") { apply_weapon_flags(w.flags, toks); return true; }
    return false;
}

}  // namespace

Profile parse_profile(const uint8_t *text, size_t size) {
    // Line-oriented tokenizer (spaces/tabs/CR), keys case-insensitive.
    // Dispatch is gated on the active `type` exactly like retail: GROUND (2)
    // accepts the witnessed set, ORGANIC (3) nothing, HELO (1) unported.
    // [orig: AIProfile_ParseProperty @ 0x45de70]
    Profile prof;
    const char *p = reinterpret_cast<const char *>(text);
    const std::size_t n = size;
    std::size_t i = 0;
    while (i < n) {
        std::size_t end = i;
        while (end < n && p[end] != '\n') ++end;
        std::vector<std::string> toks;
        std::size_t t = i;
        while (t < end) {
            while (t < end && (p[t] == ' ' || p[t] == '\t' || p[t] == '\r')) ++t;
            std::string tok;
            while (t < end && p[t] != ' ' && p[t] != '\t' && p[t] != '\r')
                tok.push_back(p[t++]);
            if (!tok.empty()) toks.push_back(std::move(tok));
        }
        i = end + 1;
        if (toks.size() < 2) continue;
        const std::string key = ascii_lower(toks[0]);
        const std::string &value = toks[1];

        if (key == "type") {
            const std::string v = ascii_lower(value);
            if (v == "helo") prof.type = 1;
            else if (v == "ground") prof.type = 2;
            else if (v == "organic") prof.type = 3;
            continue;
        }
        if (prof.type != 2) continue; // GROUND-only key set (see header note)

        // Re-shape to the retail token layout (toks[1] = key, toks[2] = value)
        // used by the flag/weapon helpers.
        std::vector<std::string> rtoks;
        rtoks.push_back(std::to_string(toks.size()));
        rtoks.insert(rtoks.end(), toks.begin(), toks.end());
        // rtoks[1] = key, rtoks[2] = first value token.

        if (key == "aim_skill") {
            prof.aim_skill = parse_int(value);
            if (prof.aim_skill > 4) prof.aim_skill = 4;   // [orig: clamp 0..4]
            if (prof.aim_skill < 0) prof.aim_skill = 0;
        } else if (key == "view_fov") prof.view_fov_bam = deg_to_bam(value);
        else if (key == "view_dist") prof.view_dist = parse_int(value) << 16;
        else if (key == "radar_fov") prof.radar_fov_bam = deg_to_bam(value);
        else if (key == "radar_dist") prof.radar_dist = parse_int(value) << 16;
        else if (key == "priority_air") prof.priority_air = parse_int(value);
        else if (key == "priority_ground") prof.priority_ground = parse_int(value);
        else if (key == "priority_organics") prof.priority_organics = parse_int(value);
        else if (key == "priority_decorations") prof.priority_decorations = parse_int(value);
        else if (key == "react_time") prof.react_ticks = secs_to_ticks(value);
        else if (key == "tether_dist") prof.tether_dist = parse_int(value) << 16;
        else if (key == "min_chase_dist") prof.min_chase = units_fixed(value);
        else if (key == "max_chase_dist") prof.max_chase = units_fixed(value);
        else if (key == "evade_flags") apply_mode_flags(prof.evade_flags, rtoks);
        else if (key == "combat_flags") apply_mode_flags(prof.combat_flags, rtoks);
        else if (key == "patrol_speed") prof.patrol_speed = parse_int(value);
        else if (key == "combat_speed") prof.combat_speed = parse_int(value);
        else if (!apply_weapon_key(prof.primary, key, "primary", rtoks))
            (void)apply_weapon_key(prof.secondary, key, "secondary", rtoks);
    }
    return prof;
}

}  // namespace opennova::aip
