#include <formats/def/def.h>

// Split out of def.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// HUDPOS.DEF: the HUD element placement table.

#include "def_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace opennova::defscan; // the shared .def scanner, unqualified as before

namespace opennova::def {

/* ========================================================================= */
/* HudPos Parsing                                                            */
/* ========================================================================= */

/* Shared buffer parser for hudpos.def, used by both the path and memory entry
   points (mirrors parse_items_buf). Assumes `out` was zeroed by the caller. */
static int parse_hudpos_buf(const char *buf, size_t file_len, DefHudPosFile *out) {
    DefHudPosDef *hud = &out->hud;
    // Both spinmap label suppressors initialize -1 (suppressed) in retail's
    // .data and only the authored token value can clear them; the sole
    // consumers are ==0 tests. [orig: dword_27237C0/dword_27236FC static
    //  0xFFFFFFFF; writes HUD_ParseHudposToken @0x59fc1f ("SPINMAPWPDISTOFF")
    //  / @0x5a0920 ("mapcoords" 3rd value); reads @0x5a7a6a / @0x5a7a2f]
    // BSS-zero in retail: the waypoint distance label is LIVE unless the
    // token authors a nonzero suppressor. [orig: dword_27237C0 — .data with
    // no file bytes; parse @0x59fc1f]
    hud->spinmap_wp_dist_off = 0;
    // Same BSS-zero polarity as the waypoint suppressor: the grid label is
    // LIVE unless MAPCOORDS' 3rd value authors a nonzero suppressor.
    // [orig: dword_27236FC — .data with no file bytes]
    hud->map_coords[2] = 0;
    /* Set default alpha for all colors */
    hud->health_border.a = 255;
    hud->heat_border.a = 255;
    hud->hud_textcolor.a = 255;
    hud->weapon_textcolor.a = 255;
    hud->tagcolor_blueteam.a = 255;
    hud->tagcolor_redteam.a = 255;
    hud->tagcolor_good.a = 255;
    hud->tagcolor_middle.a = 255;
    hud->tagcolor_bad.a = 255;
    hud->stanceicon_color.a = 255;
    hud->stancecolor_good.a = 255;
    hud->stancecolor_middle.a = 255;
    hud->stancecolor_bad.a = 255;
    hud->dest_agl_color.a = 255;
    hud->agl_color.a = 255;

    int in_vehicle_block = 0;
    /* The VEHICLE_HUD staging block: filled token by token inside a block and
       committed at VEHICLE_END, mirroring retail's single staging global and
       its memset reset [orig: HUD_ParseHudposToken @0x59F370]. */
    DefVehicleHudBlock veh;
    memset(&veh, 0, sizeof(veh));
    size_t raw_cap = 0, stance_cap = 0, declut_cap = 0, sf_cap = 0, veh_cap = 0;

    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;
    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue;

        /* Skip full-line comments */
        if (tlen >= 2 && trimmed[0] == '/' && trimmed[1] == '/') continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        /* VEHICLE_HUD blocks. Parsed IN PARALLEL with the raw_lines passthrough:
           every line still goes to raw_lines so the writer round-trips the file
           byte for byte, and the typed block is built alongside it. */
        if (lower_starts_with(lower, ll, "vehicle_hud", 11)) {
            in_vehicle_block = 1;
            memset(&veh, 0, sizeof(veh));   /* [orig: the 0xDC memset reset] */
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
            continue;
        }
        if (lower_starts_with(lower, ll, "vehicle_end", 11)) {
            /* Commit. Retail resolves the sid against the item table here; the
               parse keeps the sid string and leaves that join to the consumer.
               A block with no sid never had a key, so it is dropped rather than
               stored under an empty name. */
            if (in_vehicle_block && veh.sid[0] != '\0')
                DA_PUSH(hud->vehicle_huds, hud->vehicle_huds_count, veh_cap, veh);
            in_vehicle_block = 0;
            memset(&veh, 0, sizeof(veh));
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
            continue;
        }
        if (in_vehicle_block) {
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
            {
                /* Same split the rest of the file uses (whitespace and commas). */
                size_t vsp = 0;
                while (vsp < tlen && !isspace((unsigned char)trimmed[vsp])) ++vsp;
                size_t vvl;
                const char *vpart = trim_span(trimmed + vsp, tlen - vsp, &vvl);
                for (size_t ci = 0; ci + 1 < vvl; ++ci) {
                    if (vpart[ci] == '/' && vpart[ci + 1] == '/') {
                        vvl = ci;
                        while (vvl > 0 && isspace((unsigned char)vpart[vvl - 1])) --vvl;
                        break;
                    }
                }
                Token vv[MAX_TOKENS];
                int nvv = split_values(vpart, vvl, vv, MAX_TOKENS);
                if (lower_starts_with(lower, ll, "sid", 3)) {
                    if (nvv >= 1) safe_copy(veh.sid, sizeof(veh.sid), vv[0].s, vv[0].len);
                } else if (lower_starts_with(lower, ll, "icon", 4)) {
                    if (nvv >= 1) safe_copy(veh.icon, sizeof(veh.icon), vv[0].s, vv[0].len);
                } else if (lower_starts_with(lower, ll, "interface", 9)) {
                    if (nvv >= 1)
                        safe_copy(veh.interface_texture, sizeof(veh.interface_texture),
                                  vv[0].s, vv[0].len);
                } else if (lower_starts_with(lower, ll, "statictexture", 13)) {
                    if (nvv >= 1)
                        safe_copy(veh.static_texture, sizeof(veh.static_texture),
                                  vv[0].s, vv[0].len);
                } else if (lower_starts_with(lower, ll, "driver", 6)) {
                    if (nvv >= 2) {
                        veh.driver_x = parse_int_n(vv[0].s, vv[0].len);
                        veh.driver_y = parse_int_n(vv[1].s, vv[1].len);
                    }
                } else if (lower_starts_with(lower, ll, "emplace", 7)) {
                    /* [count][x y]... -- retail caps the COUNT, then reads that
                       many pairs; a short line yields fewer. */
                    if (nvv >= 1) {
                        int n = parse_int_n(vv[0].s, vv[0].len);
                        if (n > DEF_VEHICLE_HUD_MAX_EMPLACE) n = DEF_VEHICLE_HUD_MAX_EMPLACE;
                        if (n < 0) n = 0;
                        int got = 0;
                        for (int i = 0; i < n && 1 + 2 * i + 1 < nvv; ++i) {
                            veh.emplace_x[i] = parse_int_n(vv[1 + 2 * i].s, vv[1 + 2 * i].len);
                            veh.emplace_y[i] = parse_int_n(vv[2 + 2 * i].s, vv[2 + 2 * i].len);
                            got = i + 1;
                        }
                        veh.emplace_count = got;
                    }
                } else if (lower_starts_with(lower, ll, "seats", 5)) {
                    if (nvv >= 1) {
                        int n = parse_int_n(vv[0].s, vv[0].len);
                        if (n > DEF_VEHICLE_HUD_MAX_SEATS) n = DEF_VEHICLE_HUD_MAX_SEATS;
                        if (n < 0) n = 0;
                        int got = 0;
                        for (int i = 0; i < n && 1 + 2 * i + 1 < nvv; ++i) {
                            veh.seat_x[i] = parse_int_n(vv[1 + 2 * i].s, vv[1 + 2 * i].len);
                            veh.seat_y[i] = parse_int_n(vv[2 + 2 * i].s, vv[2 + 2 * i].len);
                            got = i + 1;
                        }
                        veh.seat_count = got;
                    }
                }
            }
            continue;
        }

        /* Get value part */
        size_t space_pos = 0;
        while (space_pos < tlen && !isspace((unsigned char)trimmed[space_pos])) ++space_pos;
        const char *val_part = trimmed + space_pos;
        size_t val_len = tlen - space_pos;
        /* Trim val_part */
        size_t vl;
        val_part = trim_span(val_part, val_len, &vl);
        /* Strip comment from value */
        for (size_t ci = 0; ci + 1 < vl; ++ci) {
            if (val_part[ci] == '/' && val_part[ci + 1] == '/') {
                vl = ci;
                while (vl > 0 && isspace((unsigned char)val_part[vl - 1])) --vl;
                break;
            }
        }

        Token vals[MAX_TOKENS];
        int nvals = split_values(val_part, vl, vals, MAX_TOKENS);

        int parsed = 0;

        /* Fonts */
        if (lower_starts_with(lower, ll, "fonthud1_hi", 11)) {
            if (nvals >= 1) safe_copy(hud->font_hi, sizeof(hud->font_hi), vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "fonthud1_lo", 11)) {
            if (nvals >= 1) safe_copy(hud->font_lo, sizeof(hud->font_lo), vals[0].s, vals[0].len);
            parsed = 1;
        }
        /* No bare `fonthud1` key exists: JO and DFX2 match only the two suffixed
           keys with _stricmp [orig: HUD_ParseHudposToken @0x59F370], and DFX's
           binary carries only those two strings, so such a line falls through as
           an unknown token, as it does in retail; the HUD slot then takes the
           bold label font (HUD_SelectHudposFont @0x591890, hud_frame.cpp). */
        /* Rects */
        else if (lower_starts_with(lower, ll, "mrclippynormal", 14)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->mrclippy_normal[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "mrclippyalternate", 17)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->mrclippy_alternate[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudhealth", 9) && !lower_starts_with(lower, ll, "hudhealthborder", 15)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->health[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudheat", 7) && !lower_starts_with(lower, ll, "hudheatborder", 13)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->heat[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpowerbar", 11)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->powerbar[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "starttimer", 10)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->starttimer[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* Border colors */
        else if (lower_starts_with(lower, ll, "hudhealthborder", 15)) {
            hud->health_border = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudheatborder", 13)) {
            hud->heat_border = parse_hud_color(vals, nvals);
            parsed = 1;
        }
        /* Text colors */
        else if (lower_starts_with(lower, ll, "hud_textcolor", 13)) {
            hud->hud_textcolor = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "weapon_textcolor", 16)) {
            hud->weapon_textcolor = parse_hud_color(vals, nvals);
            parsed = 1;
        }
        /* Tag colors */
        else if (lower_starts_with(lower, ll, "tagcolor_blueteam", 17)) {
            hud->tagcolor_blueteam = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_redteam", 16)) {
            hud->tagcolor_redteam = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_good", 13)) {
            hud->tagcolor_good = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_middle", 15)) {
            hud->tagcolor_middle = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_bad", 12)) {
            hud->tagcolor_bad = parse_hud_color(vals, nvals);
            parsed = 1;
        }
        /* Stance colors (ARGB) */
        else if (lower_starts_with(lower, ll, "stanceicon_color", 16)) {
            hud->stanceicon_color = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "stancecolor_good", 16)) {
            hud->stancecolor_good = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "stancecolor_middle", 18)) {
            hud->stancecolor_middle = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "stancecolor_bad", 15)) {
            hud->stancecolor_bad = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        }
        /* AGL colors */
        else if (lower_starts_with(lower, ll, "destaglcolor", 12)) {
            hud->dest_agl_color = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "aglcolor", 8)) {
            hud->agl_color = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        }
        /* Spinmap */
        else if (lower_starts_with(lower, ll, "hudspinmapx1", 12)) {
            if (nvals >= 1) hud->spinmap_x1 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudspinmapx2", 12)) {
            if (nvals >= 1) hud->spinmap_x2 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudspinmapy1", 12)) {
            if (nvals >= 1) hud->spinmap_y1 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudspinmapy2", 12)) {
            if (nvals >= 1) hud->spinmap_y2 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "spinmapwpdistoff", 16)) {
            if (nvals >= 1) hud->spinmap_wp_dist_off = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        }
        /* Positioned text with alignment */
        else if (lower_starts_with(lower, ll, "hudflagcarrier", 14)) {
            parse_pos_aligned(vals, nvals, hud->flag_carrier);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "gameinfo", 8)) {
            parse_pos_aligned(vals, nvals, hud->game_info);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudwpdinfo", 10)) {
            parse_pos_aligned(vals, nvals, hud->wpd_info);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "zoneinfo", 8)) {
            /* x, y, alignment word [orig: @0x5A0642..0x5A0676] */
            parse_pos_align3(vals, nvals, hud->zone_info);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "exppoints", 9)) {
            parse_pos_aligned(vals, nvals, hud->exp_points);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "connectstatus", 13)) {
            parse_pos_aligned(vals, nvals, hud->connect_status);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudteamxy", 9)) {
            parse_pos_aligned(vals, nvals, hud->team_xy);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudplayercount", 14)) {
            parse_pos_aligned(vals, nvals, hud->player_count);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "ammocountpos", 12)) {
            parse_pos_aligned(vals, nvals, hud->ammo_count_pos);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudweaponname", 13)) {
            parse_pos_aligned(vals, nvals, hud->weapon_name_pos);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "mapcoords", 9)) {
            parse_pos_aligned(vals, nvals, hud->map_coords);
            // The authored 3rd value is the grid-label suppressor; retail
            // atof()s a missing/word token to 0 (shown) — "center" in the
            // wild parses 0 too. [orig: @0x5a0920 mapcoords -> screenX/
            //  screenY/dword_27236FC]
            if (nvals < 3) hud->map_coords[2] = 0;
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudtimeclock", 12)) {
            parse_pos_aligned(vals, nvals, hud->time_clock);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "breathtime", 10)) {
            parse_pos_align3(vals, nvals, hud->breath_time);
            parsed = 1;
        }
        /* HUDLS — the weapon slot bar (def.h DefHudPosDef carries the field
           map). [orig: HUD_ParseHudposToken @0x59FE41..0x59FF9C] */
        else if (lower_starts_with(lower, ll, "hudls_system", 12)) {
            if (nvals >= 1) hud->hudls_system = parse_int_n(vals[0].s, vals[0].len); /* @0x59FE66 */
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudls_bracket", 13)) {
            if (nvals >= 1)
                safe_copy(hud->hudls_bracket, sizeof(hud->hudls_bracket), vals[0].s,
                          vals[0].len); /* the strcpy @0x59FE90 */
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudls_keyofst", 13)) {
            for (int i = 0; i < 2 && i < nvals; ++i) /* @0x59FEC7 / @0x59FEDF */
                hud->hudls_keyofst[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudls_moreav", 12)) {
            if (nvals >= 1)
                safe_copy(hud->hudls_moreav, sizeof(hud->hudls_moreav), vals[0].s,
                          vals[0].len); /* the strcpy @0x59FF05 */
            /* the ftol'd offsets keep their LOW BYTE [orig: `mov byte_2723733, al`
               @0x59FF21, `mov byte_2723734, al` @0x59FF39] */
            for (int i = 0; i < 2 && i + 1 < nvals; ++i)
                hud->hudls_moreav_off[i] =
                    (int8_t)(uint8_t)parse_int_n(vals[i + 1].s, vals[i + 1].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudls_slot", 10)) {
            if (nvals >= 1) {
                const int n = parse_int_n(vals[0].s, vals[0].len);
                /* n outside 1..10 authors nothing [orig: `sub edi,1; cmp edi,9; ja`
                   @0x59FF6A..0x59FF70] */
                if ((unsigned)(n - 1) <= 9u) {
                    if (nvals >= 2) hud->hudls_slot[n - 1][0] = parse_int_n(vals[1].s, vals[1].len);
                    if (nvals >= 3) hud->hudls_slot[n - 1][1] = parse_int_n(vals[2].s, vals[2].len);
                }
            }
            parsed = 1;
        }
        /* XY positions */
        else if (lower_starts_with(lower, ll, "hudtitlex", 9)) {
            if (nvals >= 1) hud->title_x = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudtitley", 9)) {
            if (nvals >= 1) hud->title_y = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpingx", 8)) {
            if (nvals >= 1) hud->ping_x = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpingy", 8)) {
            if (nvals >= 1) hud->ping_y = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpingright", 12)) {
            if (nvals >= 1) hud->ping_right = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudorders", 9)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->orders[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "specmode_label", 14)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->spec_mode_label[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "lfp_flags", 9)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->lfp_flags[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "lfp_takeoverdlg", 15)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->lfp_takeover_dlg[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "cargopos", 8)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->cargo_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "roomtkpos", 9) && !lower_starts_with(lower, ll, "roomtktxtpos", 12)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->roomtk_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "roomtktxtpos", 12)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->roomtk_txt_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudstancepos", 12)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->stance_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudvehstancepos", 15)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->veh_stance_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudgeartext", 11)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->gear_text[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudwpnicon", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->wpn_icon[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudclip", 7) && !lower_starts_with(lower, ll, "hudclipgfx", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->clip_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudscoperangexy", 15)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_range[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudscopezeroxy", 14)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_zero[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudscopemagxy", 13)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_mag[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "showimpactdistpos", 17)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->impact_dist_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudchattext", 11)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->chat_text[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudsystext", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->sys_text[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* Single values */
        else if (lower_starts_with(lower, ll, "hudchline", 9)) {
            if (nvals >= 1) hud->hud_chline = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudaglradius", 12)) {
            if (nvals >= 1) hud->agl_radius = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudroclen", 9)) {
            if (nvals >= 1) hud->roc_len = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "alphafade", 9)) {
            /* atof per field — fractional values survive into the original's
               x2.55/x62 converts [orig: @0x5a0882..0x5a08c2]. */
            for (int i = 0; i < 3 && i < nvals; ++i)
                hud->alpha_fade[i] = parse_float_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* AGL settings */
        else if (lower_starts_with(lower, ll, "hudagltlrx", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->agl_tlrx[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudaglylen", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->agl_ylen[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* HUDSTANCE */
        else if (lower_starts_with(lower, ll, "hudstance", 9) && ll > 9 &&
                 isspace((unsigned char)lower[9])) {
            if (nvals >= 5) {
                DefHudStance st;
                memset(&st, 0, sizeof(st));
                st.id = parse_int_n(vals[0].s, vals[0].len);
                st.offset_x = parse_int_n(vals[1].s, vals[1].len);
                st.offset_y = parse_int_n(vals[2].s, vals[2].len);
                safe_copy(st.texture, sizeof(st.texture), vals[3].s, vals[3].len);
                safe_copy(st.name, sizeof(st.name), vals[4].s, vals[4].len);
                DA_PUSH(hud->stances, hud->stances_count, stance_cap, st);
            }
            parsed = 1;
        }
        /* HUDDECLUT_<TOKEN> v0 v1 v2 v3 — the declutter table rows. Retail
           parses one arm per known token and builds a mask byte where bit i
           (1/2/4/8 for hud_detail level 0..3) is set iff value i != 0, stored
           into the 24-slot table; a token WITHOUT an arm authors nothing (the
           retail JOX file ships a 25th row, HUDDECLUT_CTAPE, that is exactly
           such a dead token — the binary has no arm for it).
           [orig: HUD_ParseHudposToken @0x59F370 -> byte_2723CE0[slot]]
           This parser keeps every row generically (flags normalized to 0/1);
           the consumer resolves token -> slot and drops unknown tokens
           (engine/runtime/hud/hud_declutter.cpp), which reproduces the
           no-arm behavior. */
        else if (lower_starts_with(lower, ll, "huddeclut_", 10)) {
            /* Extract name between _ and first whitespace */
            size_t name_start = 10;
            size_t name_end = name_start;
            while (name_end < ll && !isspace((unsigned char)lower[name_end])) ++name_end;
            if (nvals >= 4) {
                DefDeclutterEntry de;
                memset(&de, 0, sizeof(de));
                safe_copy(de.name, sizeof(de.name), trimmed + name_start, name_end - name_start);
                for (int i = 0; i < 4; ++i)
                    de.flags[i] = parse_int_n(vals[i].s, vals[i].len) != 0 ? 1 : 0;
                DA_PUSH(hud->declutter, hud->declutter_count, declut_cap, de);
            }
            parsed = 1;
        }
        /* Graphics */
        else if (lower_starts_with(lower, ll, "staticframe", 11)) {
            if (nvals >= 1) {
                DefHudGraphic gfx;
                memset(&gfx, 0, sizeof(gfx));
                int idx = 0;
                /* Skip leading // in texture name */
                if (vals[idx].len > 2 && vals[idx].s[0] == '/' && vals[idx].s[1] == '/') {
                    safe_copy(gfx.texture, sizeof(gfx.texture), vals[idx].s + 2, vals[idx].len - 2);
                    idx++;
                } else if (!(vals[idx].len == 2 && vals[idx].s[0] == '/' && vals[idx].s[1] == '/')) {
                    safe_copy(gfx.texture, sizeof(gfx.texture), vals[idx].s, vals[idx].len);
                    idx++;
                }
                if (idx < nvals) gfx.x = parse_int_n(vals[idx].s, vals[idx].len), idx++;
                if (idx < nvals) gfx.y = parse_int_n(vals[idx].s, vals[idx].len), idx++;
                if (gfx.texture[0] != '\0') {
                    DA_PUSH(hud->static_frames, hud->static_frames_count, sf_cap, gfx);
                }
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "parachuteicon", 13)) {
            if (nvals >= 3) {
                safe_copy(hud->parachute_icon.texture, sizeof(hud->parachute_icon.texture), vals[0].s, vals[0].len);
                hud->parachute_icon.x = parse_int_n(vals[1].s, vals[1].len);
                hud->parachute_icon.y = parse_int_n(vals[2].s, vals[2].len);
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "armoricon", 9)) {
            if (nvals >= 3) {
                safe_copy(hud->armor_icon.texture, sizeof(hud->armor_icon.texture), vals[0].s, vals[0].len);
                hud->armor_icon.x = parse_int_n(vals[1].s, vals[1].len);
                hud->armor_icon.y = parse_int_n(vals[2].s, vals[2].len);
            }
            parsed = 1;
        }

        if (!parsed) {
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
        }
    }

    return 0;
}

int def_parse_hudpos(const char *path, DefHudPosFile *out) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_hudpos_buf(buf, file_len, out);
    free(buf);
    return rc;
}

int def_parse_hudpos_memory(const uint8_t *data, size_t size, DefHudPosFile *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_hudpos_buf((const char *)data, size, out);
}

void def_free_hudpos(DefHudPosFile *f) {
    if (!f) return;
    free(f->hud.stances);
    free(f->hud.declutter);
    free(f->hud.static_frames);
    free(f->hud.vehicle_huds);
    free(f->hud.raw_lines);
    memset(f, 0, sizeof(*f));
}

} // namespace opennova::def
