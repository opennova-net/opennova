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
    // The second loop: every team zone with a wave entry (NO secured gate)
    // inserts its members after the row valued index + 1, else at position 0
    // [orig: @0x553c5f..0x553de3].
    for (const DeployZoneRow &z : in.zones) {
        if (z.occupants.empty()) continue;
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
    v.medic = in.revive_seconds != 0 && !in.local_mounted;
    return v;
}

} // namespace opennova::world
