#include <formats/aip/aip.h>
#include <base/io/tick_rate.h>
#include <base/io/strutil.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace opennova::aip {

namespace {

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
    return static_cast<int32_t>(std::atof(s.c_str()) * io::kTickHz); // [orig: dbl_7C3B48]
}
int32_t units_fixed(const std::string &s) {
    return static_cast<int32_t>(std::atof(s.c_str()) * 65536.0); // [orig: dbl_7C3CC0]
}
// The HELO flight set's two unit conversions [orig: the type-1 arms of
// AIProfile_ParseProperty @0x45f684..0x45f9eb]: a speed is km/h -> 16.16 units
// per tick, atof * 1000.0 (dbl_7C6BD0) * 4.444444444444444e-06 (dbl_7C6BC8) *
// 65536.0 (dbl_7C3CC0); a climb is atof * 0.016 (dbl_7C6A80) * 65536.0. Both
// chop through _ftol2_sse. The GROUND branch converts its speeds the same way
// @0x45e6df/@0x45e72d (+0xC0/+0xC4); that pair stays raw here because its
// consumer, the brain seed, applies the identical x65536/225 scale.
int32_t speed_fixed(const std::string &s) {
    return static_cast<int32_t>(std::atof(s.c_str()) * 1000.0 * 4.444444444444444e-06 * 65536.0);
}
int32_t climb_fixed(const std::string &s) {
    return static_cast<int32_t>(std::atof(s.c_str()) * 0.016 * 65536.0);
}

// The WEAPON_* flag token loop, shared by primary_flags/secondary_flags.
void apply_weapon_flags(uint32_t &flags, const std::vector<std::string> &toks) {
    for (std::size_t k = 2; k < toks.size(); ++k) {
        const std::string t = strutil::to_lower(toks[k]);
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
        const std::string t = strutil::to_lower(toks[k]);
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
        const std::string key = strutil::to_lower(toks[0]);
        const std::string &value = toks[1];

        if (key == "type") {
            const std::string v = strutil::to_lower(value);
            if (v == "helo") prof.type = 1;
            else if (v == "ground") prof.type = 2;
            else if (v == "organic") prof.type = 3;
            continue;
        }
        // The HELO (type 1) flight set — shared keys (view/radar/priorities/
        // react/EVADE_FLAGS/weapons) fall through to the common dispatch below;
        // the flight-specific keys are handled here first. GROUND (2) takes the
        // common set; ORGANIC (3) accepts nothing.
        // [orig: the type-1 branch of AIProfile_ParseProperty @0x45f684..0x45f9eb
        //  — profile +200..+236 (+56 for use_waypoint_z); the shared keys are the
        //  same rows both types write]
        if (prof.type == 1) {
            if (key == "patrol_speed") { prof.helo_patrol_speed = speed_fixed(value); continue; }          // @0x45f6cf..0x45f70d -> +200
            if (key == "patrol_altitude") { prof.helo_patrol_altitude = parse_int(value) << 16; continue; } // atol << 16 @0x45f733..0x45f747 -> +204
            if (key == "patrol_climb") { prof.helo_patrol_climb = climb_fixed(value); continue; }          // @0x45f687..0x45f6bf -> +208
            if (key == "combat_speed") { prof.helo_combat_speed = speed_fixed(value); continue; }          // @0x45f79f..0x45f7dd -> +212
            if (key == "combat_altitude") { prof.helo_combat_altitude = parse_int(value) << 16; continue; } // atol << 16 @0x45f803..0x45f817 -> +216
            if (key == "combat_climb") { prof.helo_combat_climb = climb_fixed(value); continue; }          // @0x45f757..0x45f78f -> +220
            // turn_rate: atol * 0xB60B60 (11930464 BAM per degree) then the signed
            // /62 (the 0x84210843 magic + sar 5 + sign fix) @0x45f8b0..0x45f8dc -> +224
            if (key == "turn_rate") { prof.turn_rate_bam_tick = 11930464 * parse_int(value) / io::kTicksPerSecondInt; continue; }
            // accel_time: atol, then (v << 5 - v) * 2 = 62 * v @0x45f902..0x45f91b -> +228
            if (key == "accel_time") { prof.accel_ticks = 62 * parse_int(value); continue; }
            if (key == "use_waypoint_z") { prof.use_waypoint_z = parse_int(value); continue; }             // atol @0x45f941..0x45f952 -> +56
            if (key == "min_agl") { prof.min_agl = units_fixed(value); continue; }                        // atof * 65536 @0x45f975..0x45f991 -> +232
            if (key == "min_speed") { prof.min_speed = speed_fixed(value); continue; }                    // @0x45f9b7..0x45f9df -> +236
        }
        if (prof.type != 2 && prof.type != 1) continue; // see header note

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
