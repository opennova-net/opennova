#include <runtime/world/deploy_screen_feed.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace opennova::world {

namespace {

// ListWidget_SortRows(list, 0, 1): the qsort comparator with sortParams
// {0 = string mode, 1 = ascending} is stricmp(a.text, b.text); NULL text
// sorts last [orig: cmp @0x6448a0 — the null tests @0x6448a9..0x6448b6,
// the ascending arm @0x644915].
bool row_text_less(const DeployListRow &a, const DeployListRow &b) {
    return strutil::iless(a.text, b.text);
}

} // namespace

std::vector<DeployListRow> build_deploy_rows(const DeployListInput &in) {
    std::vector<DeployListRow> rows;
    // Row 0: "<color>'<key>' <home>", value 0 [orig: @0x553b6b..0x553b8a].
    {
        DeployListRow def;
        def.text = in.team_color_tag + "'" + in.default_key + "' " + in.default_home;
        def.value = 0;
        rows.push_back(def);
    }
    // The first loop: one row per SECURED team zone, "<color>'<letter>' <name>",
    // value index + 1 [orig: @0x553aef..0x553c55].
    for (const DeployZoneRow &z : in.zones) {
        if (!z.secured) continue;
        DeployListRow row;
        const std::string name = in.zone_name ? in.zone_name(z.name_key) : z.name_key;
        row.text = in.team_color_tag + "'" + std::string(1, z.letter) + "' " + name;
        row.value = z.index + 1;
        rows.push_back(row);
    }
    // ListWidget_SortRows(list, 0, 1) over the whole list — the Default row
    // included [orig: @0x553c5a]. std::stable_sort: the original qsort's
    // order among EQUAL texts is unspecified and two rows never share text.
    std::stable_sort(rows.begin(), rows.end(), row_text_less);
    // The second loop: EVERY team zone (team byte, def attrib 0x40000, in the
    // spawn-zone list — NO secured gate and NO occupant gate) inserts its
    // members after the row valued index + 1, else at position 0, then appends
    // one blank spacer row unconditionally — so a zone with nobody queued still
    // contributes its blank [orig: @0x553c5f..0x553de3; the member gate
    // `cmp dword_A85BC4[ecx*4],0; jle` @0x553d2f..0x553d36 skips only the member
    // run, the blank UIList_AddRow @0x553dbf..0x553dce follows either way].
    for (const DeployZoneRow &z : in.zones) {
        int insert_pos = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].value == z.index + 1) {
                insert_pos = static_cast<int>(r);
                break;
            }
        }
        for (const DeployOccupant &o : z.occupants) {
            DeployListRow row;
            char buf[512];
            if (o.self)
                std::snprintf(buf, sizeof(buf), "<b><cFF4040>** %s **", o.name.c_str());
            else
                std::snprintf(buf, sizeof(buf), "%s", o.name.c_str());
            row.text = buf;
            row.value = -1;
            // UIList_AddRow(text, -1, 0, insert_pos + 1) returns the row it
            // landed on; the next member follows it [orig: @0x553d8b].
            ++insert_pos;
            rows.insert(rows.begin() + std::min<int>(insert_pos, static_cast<int>(rows.size())), row);
        }
        DeployListRow blank;
        blank.value = -1;
        ++insert_pos;
        rows.insert(rows.begin() + std::min<int>(insert_pos, static_cast<int>(rows.size())), blank);
    }
    return rows;
}

DeployStatusLine build_deploy_status(const DeployStatusInput &in) {
    DeployStatusLine out;
    // The penalty timer wins [orig: dword_A85B5C @0x553928..0x553965].
    if (in.penalty_seconds != 0) {
        out.kind = DeployStatusLine::Kind::Penalty;
        out.seconds = in.penalty_seconds;
        return out;
    }
    // Else the wave zone listing the local player, if it is in the list
    // [orig: word_A85BC0 != -1 @0x55396e; SpawnZoneList_IndexOf >= 0 @0x5539a0].
    if (in.self_zone_index >= 0) {
        out.kind = DeployStatusLine::Kind::Wave;
        out.seconds = in.self_zone_countdown;
        out.numbered = in.self_zone_numbered;
        out.zone_index = in.self_zone_index;
    }
    return out;
}

std::string deploy_status_text(const DeployStatusLine &line,
                               const std::string &penalty_label,
                               const std::string &zone_name) {
    char buf[256];
    switch (line.kind) {
    case DeployStatusLine::Kind::Penalty:
        // [orig: sprintf("%s  <cFF4040>%i", STROVER_PENALTYTIMER, penalty)]
        std::snprintf(buf, sizeof buf, "%s  <cFF4040>%i", penalty_label.c_str(), line.seconds);
        return buf;
    case DeployStatusLine::Kind::Wave:
        if (line.numbered) {
            // [orig: sprintf("'%s':  <cFF4040>%d", WPNames name, countdown)]
            std::snprintf(buf, sizeof buf, "'%s':  <cFF4040>%d", zone_name.c_str(), line.seconds);
        } else {
            // [orig: sprintf("%c:  <cFF4040>%d", 'A' + index, countdown) @0x553a5b]
            std::snprintf(buf, sizeof buf, "%c:  <cFF4040>%d",
                          static_cast<char>('A' + line.zone_index), line.seconds);
        }
        return buf;
    case DeployStatusLine::Kind::None:
    default:
        return "";
    }
}

DeployStaticsVisibility deploy_statics_visibility(const DeployStaticsInput &in) {
    DeployStaticsVisibility v;
    // [orig: dword_A85B68 @0x553e2a; dword_A85B60 && !entity+0x1E0 @0x553eb8..0x553ecc]
    v.psp_respawn = in.hold_seconds != 0;
    v.medic = in.revive_seconds != 0 && !in.local_medic_reviving;
    return v;
}

DeployStaticsText deploy_statics_text(const DeployStaticsInput &in,
                                      const std::string &medic_key_label,
                                      const hud::GameTextLookup &lookup) {
    const auto text = [&lookup](const char *key, const char *fallback) {
        return lookup ? lookup("Overlays", key, fallback) : std::string(fallback);
    };
    DeployStaticsText out;
    char buf[256];
    // [orig: sprintf("%s  <cFF4040>%d", STROVER_PSPRESPAWN, dword_A85B68) @0x553e10]
    std::snprintf(buf, sizeof buf, "%s  <cFF4040>%d",
                  text("STROVER_PSPRESPAWN", "Spawn point available in").c_str(),
                  in.hold_seconds);
    out.psp_respawn = buf;
    // [orig: sprintf("%s  <cFF4040>%d", STROVER_MEDICTIMER, dword_A85B60) @0x553e74]
    std::snprintf(buf, sizeof buf, "%s  <cFF4040>%d",
                  text("STROVER_MEDICTIMER", "Medic time remaining").c_str(),
                  in.revive_seconds);
    out.medic_timer = buf;
    // [orig: sprintf(STROVER_CALLMEDIC, KeyBinding_FormatDisplayString(MedicReq))
    //  @0x553f60]; a format without the key slot draws as authored.
    const std::string call_format = text("STROVER_CALLMEDIC", "Press %s to call a medic");
    const size_t slot = call_format.find("%s");
    out.call_medic = slot == std::string::npos
            ? call_format
            : call_format.substr(0, slot) + medic_key_label + call_format.substr(slot + 2);
    return out;
}


DeployInstructions build_deploy_instructions(
        const DeployInstructionsInput &in, const hud::GameTextLookup &lookup) {
    // [orig: UI_UpdateDeathScreenContent @ 0x5536A0]
    const auto text = [&lookup](const char *key, const char *fallback = "") {
        return lookup ? lookup("Overlays", key, fallback) : std::string(fallback);
    };
    DeployInstructions out;
    out.permanent_death = in.permanent_death && in.dead;
    if (out.permanent_death) {
        out.show_first = out.show_second = true;
        out.first_text = text("STROVER_PERMANENTDEATH");
        out.second_text = text(in.spectators_allowed
                ? "STROVER_SPECTATORSPAWN" : "STROVER_NORESPAWN");
        out.show_round_status = in.round_ticks >= 0;
        if (out.show_round_status) {
            const int seconds = in.round_ticks / 62;
            char clock[48];
            std::snprintf(clock, sizeof clock, " <cFF4040>%i:%02i:%02i",
                    seconds / 3600, seconds / 60 % 60, seconds % 60);
            out.round_text = text("STROVER50") + clock;
            const std::string label = lookup ? lookup("Client", "STRCLI25", "") : "";
            out.remaining_players_text = label + " <cFF4040>" + std::to_string(in.alive_players);
        }
        return out;
    }
    const bool assault = (in.game_type & 0xFFFDFFFFu) == 0x10020u;
    const bool full_spawn = in.check_secured_spawn && in.has_full_team_spawn;
    out.show_first = !assault;
    out.show_second = !full_spawn;
    out.replace_first = !full_spawn;
    if (!full_spawn) {
        if (in.dead) {
            out.first_text = in.kill_announcement;
        } else {
            std::string name = in.player_name.substr(0, 255);
            if (!in.clan.empty()) name = (name + "<ch>" + in.clan + "<co>").substr(0, 255);
            const bool team_join = (in.game_type & 0x10000u) != 0 &&
                    (in.game_type & 0x20000u) == 0 && in.team != 0;
            const std::string format = team_join
                    ? text("STROVER_INITIALSPAWN", "%s, you have joined the %s.")
                    : text("STROVER_WELCOMESPAWN", "Welcome to the game, %s!");
            std::string team = "Unknown";
            if (in.team == 1) team = text("STROVER_BLUETEAM", "!Joint Ops Team");
            else if (in.team == 2) team = text("STROVER_REDTEAM", "!Rebel Team");
            // Resolve the authored string arguments without interpreting markup
            // or any format characters inside the player/clan names.
            unsigned argument = 0;
            for (size_t i = 0; i < format.size(); ++i) {
                if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == 's') {
                    out.first_text += argument++ == 0 ? name : team;
                    ++i;
                } else if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == '%') {
                    out.first_text += '%';
                    ++i;
                } else out.first_text += format[i];
            }
        }
    }
    const char *key = "STROVER_RESPAWN1";
    if (!in.has_spawn_zones) {
        if (in.team == 0) key = "STROVER_SPECTATORSPAWN";
        else if (!assault) key = "STROVER_RESPAWN3";
        else key = (in.game_type & 0x20000u) != 0 || !in.dead
                ? "STROVER_RESPAWN4" : "STROVER_RESPAWN5";
    }
    out.second_text = text(key);
    return out;
}

} // namespace opennova::world
