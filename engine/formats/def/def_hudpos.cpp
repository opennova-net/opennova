#include <formats/def/def.h>

// HUDPOS.DEF: the HUD element placement table.

#include "def_scan.h"

#include <base/io/strutil.h>

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
    size_t stance_cap = 0, declut_cap = 0, sf_cap = 0, veh_cap = 0;

    /* Every line reaches the parser as the retail tokenizer cuts it
       (defscan::for_each_def_line); every key is the whole first token compared
       without case, and the values are the tokens after it (space, tab and comma
       separate them) [orig: HUD_ParseHudposToken @0x59F370, _stricmp on
       tokens[1] throughout, atof/strcpy of tokens[2]..]. */
    for_each_def_line(buf, file_len, [&](const io::ConfigTokens &tokens, const char *, size_t, size_t) {
        const char *key = tokens.tokens[0];
        Token vals[kMaxValueTokens];
        const int nvals = value_tokens(tokens, vals, kMaxValueTokens);

        /* VEHICLE_HUD blocks. */
        if (key_is(key, "vehicle_hud")) {
            in_vehicle_block = 1;
            memset(&veh, 0, sizeof(veh));   /* [orig: the 0xDC memset reset] */
            return;
        }
        if (key_is(key, "vehicle_end")) {
            /* Commit. Retail copies the block into every item whose alias
               matches its sid without case, an empty sid included (it matches
               an item whose alias stayed empty: one a nested `begin` or the
               file's end closed), and a later block overwrites an earlier one
               [orig: the VEHICLE_END loop @0x59F3DA..0x59F40C, `_stricmp(alias,
               byte_2723DC4)` @0x59F402]. The parse keeps every block in file
               order and the consumer makes the join (HudPos::get_vehicle_hud
               takes the last match). */
            if (in_vehicle_block)
                DA_PUSH(hud->vehicle_huds, hud->vehicle_huds_count, veh_cap, veh);
            in_vehicle_block = 0;
            memset(&veh, 0, sizeof(veh));
            return;
        }
        /* The seven block keys fill the staging block; retail matches them on
           every line, but outside a block the next VEHICLE_HUD wipes what they
           wrote before any VEHICLE_END commits it, so only a block's count. Every
           other key is the HUD's wherever it stands, inside a block too
           [orig: VEHICLE_HUD @0x59F380, VEHICLE_END @0x59F3B8, the block keys
           @0x59F5CE..0x59F74E, ahead of the HUD chain from @0x59F7CE]. */
        const bool vehicle_key = key_is(key, "sid") || key_is(key, "icon") ||
                key_is(key, "interface") || key_is(key, "statictexture") ||
                key_is(key, "driver") || key_is(key, "emplace") || key_is(key, "seats");
        if (vehicle_key) {
            if (in_vehicle_block) {
                const Token *vv = vals;
                const int nvv = nvals;
                if (key_is(key, "sid")) {
                    if (nvv >= 1) safe_copy(veh.sid, sizeof(veh.sid), vv[0].s, vv[0].len);
                } else if (key_is(key, "icon")) {
                    if (nvv >= 1) safe_copy(veh.icon, sizeof(veh.icon), vv[0].s, vv[0].len);
                } else if (key_is(key, "interface")) {
                    if (nvv >= 1)
                        safe_copy(veh.interface_texture, sizeof(veh.interface_texture),
                                  vv[0].s, vv[0].len);
                } else if (key_is(key, "statictexture")) {
                    if (nvv >= 1)
                        safe_copy(veh.static_texture, sizeof(veh.static_texture),
                                  vv[0].s, vv[0].len);
                } else if (key_is(key, "driver")) {
                    if (nvv >= 2) {
                        veh.driver_x = hud_number(vv[0].s, vv[0].len);
                        veh.driver_y = hud_number(vv[1].s, vv[1].len);
                    }
                } else if (key_is(key, "emplace")) {
                    /* [count][x y]... -- retail caps the COUNT, then reads that
                       many pairs; a short line yields fewer. */
                    if (nvv >= 1) {
                        int n = hud_number(vv[0].s, vv[0].len);
                        if (n > DEF_VEHICLE_HUD_MAX_EMPLACE) n = DEF_VEHICLE_HUD_MAX_EMPLACE;
                        if (n < 0) n = 0;
                        int got = 0;
                        for (int i = 0; i < n && 1 + 2 * i + 1 < nvv; ++i) {
                            veh.emplace_x[i] = hud_number(vv[1 + 2 * i].s, vv[1 + 2 * i].len);
                            veh.emplace_y[i] = hud_number(vv[2 + 2 * i].s, vv[2 + 2 * i].len);
                            got = i + 1;
                        }
                        veh.emplace_count = got;
                    }
                } else if (key_is(key, "seats")) {
                    if (nvv >= 1) {
                        int n = hud_number(vv[0].s, vv[0].len);
                        if (n > DEF_VEHICLE_HUD_MAX_SEATS) n = DEF_VEHICLE_HUD_MAX_SEATS;
                        if (n < 0) n = 0;
                        int got = 0;
                        for (int i = 0; i < n && 1 + 2 * i + 1 < nvv; ++i) {
                            veh.seat_x[i] = hud_number(vv[1 + 2 * i].s, vv[1 + 2 * i].len);
                            veh.seat_y[i] = hud_number(vv[2 + 2 * i].s, vv[2 + 2 * i].len);
                            got = i + 1;
                        }
                        veh.seat_count = got;
                    }
                }
            }
            return;
        }

        /* Fonts */
        if (key_is(key, "fonthud1_hi")) {
            if (nvals >= 1) safe_copy(hud->font_hi, sizeof(hud->font_hi), vals[0].s, vals[0].len);
        } else if (key_is(key, "fonthud1_lo")) {
            if (nvals >= 1) safe_copy(hud->font_lo, sizeof(hud->font_lo), vals[0].s, vals[0].len);
        }
        /* No bare `fonthud1` key exists: JO and DFX2 match only the two suffixed
           keys with _stricmp [orig: HUD_ParseHudposToken @0x59F370], and DFX's
           binary carries only those two strings, so such a line falls through as
           an unknown token, as it does in retail; the HUD slot then takes the
           bold label font (HUD_SelectHudposFont @0x591890, hud_frame.cpp). */
        /* Rects */
        else if (key_is(key, "mrclippynormal")) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->mrclippy_normal[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "mrclippyalternate")) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->mrclippy_alternate[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudhealth")) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->health[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudheat")) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->heat[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudpowerbar")) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->powerbar[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "starttimer")) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->starttimer[i] = hud_number(vals[i].s, vals[i].len);
        }
        /* Border colors: a, r, g, b, the first field the alpha byte
           [orig: HUDHEALTHBORDER @0x5A13E9..0x5A143A -> dword_27237C4,
           HUDHEATBORDER @0x5A14C9..0x5A151A -> g_HUDHeatBorderColor @0x27237D8,
           each `ftol << 24` first] */
        else if (key_is(key, "hudhealthborder")) {
            hud->health_border = parse_hud_color_argb(vals, nvals);
        } else if (key_is(key, "hudheatborder")) {
            hud->heat_border = parse_hud_color_argb(vals, nvals);
        }
        /* Text colors */
        else if (key_is(key, "hud_textcolor")) {
            hud->hud_textcolor = parse_hud_color(vals, nvals);
        } else if (key_is(key, "weapon_textcolor")) {
            hud->weapon_textcolor = parse_hud_color(vals, nvals);
        }
        /* Tag colors */
        else if (key_is(key, "tagcolor_blueteam")) {
            hud->tagcolor_blueteam = parse_hud_color(vals, nvals);
        } else if (key_is(key, "tagcolor_redteam")) {
            hud->tagcolor_redteam = parse_hud_color(vals, nvals);
        } else if (key_is(key, "tagcolor_good")) {
            hud->tagcolor_good = parse_hud_color(vals, nvals);
        } else if (key_is(key, "tagcolor_middle")) {
            hud->tagcolor_middle = parse_hud_color(vals, nvals);
        } else if (key_is(key, "tagcolor_bad")) {
            hud->tagcolor_bad = parse_hud_color(vals, nvals);
        }
        /* Stance colors (ARGB) */
        else if (key_is(key, "stanceicon_color")) {
            hud->stanceicon_color = parse_hud_color_argb(vals, nvals);
        } else if (key_is(key, "stancecolor_good")) {
            hud->stancecolor_good = parse_hud_color_argb(vals, nvals);
        } else if (key_is(key, "stancecolor_middle")) {
            hud->stancecolor_middle = parse_hud_color_argb(vals, nvals);
        } else if (key_is(key, "stancecolor_bad")) {
            hud->stancecolor_bad = parse_hud_color_argb(vals, nvals);
        }
        /* AGL colors */
        else if (key_is(key, "destaglcolor")) {
            hud->dest_agl_color = parse_hud_color_argb(vals, nvals);
        } else if (key_is(key, "aglcolor")) {
            hud->agl_color = parse_hud_color_argb(vals, nvals);
        }
        /* Spinmap */
        else if (key_is(key, "hudspinmapx1")) {
            if (nvals >= 1) hud->spinmap_x1 = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudspinmapx2")) {
            if (nvals >= 1) hud->spinmap_x2 = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudspinmapy1")) {
            if (nvals >= 1) hud->spinmap_y1 = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudspinmapy2")) {
            if (nvals >= 1) hud->spinmap_y2 = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "spinmapwpdistoff")) {
            if (nvals >= 1) hud->spinmap_wp_dist_off = hud_number(vals[0].s, vals[0].len);
        }
        /* Positioned text with alignment */
        else if (key_is(key, "hudflagcarrier")) {
            parse_pos_aligned(vals, nvals, hud->flag_carrier);
        } else if (key_is(key, "gameinfo")) {
            parse_pos_aligned(vals, nvals, hud->game_info);
        } else if (key_is(key, "hudwpdinfo")) {
            parse_pos_aligned(vals, nvals, hud->wpd_info);
        } else if (key_is(key, "zoneinfo")) {
            /* x, y, alignment word [orig: @0x5A0642..0x5A0676] */
            parse_pos_align3(vals, nvals, hud->zone_info);
        } else if (key_is(key, "exppoints")) {
            parse_pos_aligned(vals, nvals, hud->exp_points);
        } else if (key_is(key, "connectstatus")) {
            parse_pos_aligned(vals, nvals, hud->connect_status);
        } else if (key_is(key, "hudteamxy")) {
            parse_pos_aligned(vals, nvals, hud->team_xy);
        } else if (key_is(key, "hudplayercount")) {
            parse_pos_aligned(vals, nvals, hud->player_count);
        } else if (key_is(key, "ammocountpos")) {
            parse_pos_aligned(vals, nvals, hud->ammo_count_pos);
        } else if (key_is(key, "hudweaponname")) {
            parse_pos_aligned(vals, nvals, hud->weapon_name_pos);
        } else if (key_is(key, "mapcoords")) {
            parse_pos_aligned(vals, nvals, hud->map_coords);
            // The authored 3rd value is the grid-label suppressor; retail
            // atof()s a missing/word token to 0 (shown) — "center" in the
            // wild parses 0 too. [orig: @0x5a0920 mapcoords -> screenX/
            //  screenY/dword_27236FC]
            if (nvals < 3) hud->map_coords[2] = 0;
        } else if (key_is(key, "hudtimeclock")) {
            parse_pos_aligned(vals, nvals, hud->time_clock);
        } else if (key_is(key, "breathtime")) {
            parse_pos_align3(vals, nvals, hud->breath_time);
        }
        /* HUDLS — the weapon slot bar (def.h DefHudPosDef carries the field
           map). [orig: HUD_ParseHudposToken @0x59FE41..0x59FF9C] */
        else if (key_is(key, "hudls_system")) {
            if (nvals >= 1) hud->hudls_system = hud_number(vals[0].s, vals[0].len); /* @0x59FE66 */
        } else if (key_is(key, "hudls_bracket")) {
            if (nvals >= 1)
                safe_copy(hud->hudls_bracket, sizeof(hud->hudls_bracket), vals[0].s,
                          vals[0].len); /* the strcpy @0x59FE90 */
        } else if (key_is(key, "hudls_keyofst")) {
            for (int i = 0; i < 2 && i < nvals; ++i) /* @0x59FEC7 / @0x59FEDF */
                hud->hudls_keyofst[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudls_moreav")) {
            if (nvals >= 1)
                safe_copy(hud->hudls_moreav, sizeof(hud->hudls_moreav), vals[0].s,
                          vals[0].len); /* the strcpy @0x59FF05 */
            /* the ftol'd offsets keep their LOW BYTE [orig: `mov byte_2723733, al`
               @0x59FF21, `mov byte_2723734, al` @0x59FF39] */
            for (int i = 0; i < 2 && i + 1 < nvals; ++i)
                hud->hudls_moreav_off[i] =
                    (int8_t)(uint8_t)hud_number(vals[i + 1].s, vals[i + 1].len);
        } else if (key_is(key, "hudls_slot")) {
            if (nvals >= 1) {
                const int n = hud_number(vals[0].s, vals[0].len);
                /* n outside 1..10 authors nothing [orig: `sub edi,1; cmp edi,9; ja`
                   @0x59FF6A..0x59FF70] */
                if ((unsigned)(n - 1) <= 9u) {
                    if (nvals >= 2) hud->hudls_slot[n - 1][0] = hud_number(vals[1].s, vals[1].len);
                    if (nvals >= 3) hud->hudls_slot[n - 1][1] = hud_number(vals[2].s, vals[2].len);
                }
            }
        }
        /* XY positions */
        else if (key_is(key, "hudtitlex")) {
            if (nvals >= 1) hud->title_x = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudtitley")) {
            if (nvals >= 1) hud->title_y = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudpingx")) {
            if (nvals >= 1) hud->ping_x = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudpingy")) {
            if (nvals >= 1) hud->ping_y = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudpingright")) {
            if (nvals >= 1) hud->ping_right = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudorders")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->orders[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "specmode_label")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->spec_mode_label[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "lfp_flags")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->lfp_flags[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "lfp_takeoverdlg")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->lfp_takeover_dlg[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "cargopos")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->cargo_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "pausedpos")) {
            /* [orig: @0x59FC8D..0x59FCC8 -> dword_272360C / dword_2723610] */
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->paused_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "networkindicator")) {
            /* [orig: @0x59F981 _stricmp "NETWORKINDICATOR", six atof/ftol stores
               @0x59F9A8..0x59FA0C -> g_NetQuality +0x40..+0x54] */
            for (int i = 0; i < 6 && i < nvals; ++i)
                hud->network_indicator[i] = hud_number(vals[i].s, vals[i].len);
            hud->network_indicator_present = 1;
        } else if (key_is(key, "roomtkpos")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->roomtk_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "roomtktxtpos")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->roomtk_txt_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudstancepos")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->stance_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudvehstancepos")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->veh_stance_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudgeartext")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->gear_text[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudwpnicon")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->wpn_icon[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudclip")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->clip_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudscoperangexy")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_range[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudscopezeroxy")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_zero[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudscopemagxy")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_mag[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "showimpactdistpos")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->impact_dist_pos[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudchattext")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->chat_text[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudsystext")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->sys_text[i] = hud_number(vals[i].s, vals[i].len);
        }
        /* Single values */
        else if (key_is(key, "hudchline")) {
            if (nvals >= 1) hud->hud_chline = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudaglradius")) {
            if (nvals >= 1) hud->agl_radius = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "hudroclen")) {
            if (nvals >= 1) hud->roc_len = hud_number(vals[0].s, vals[0].len);
        } else if (key_is(key, "alphafade")) {
            /* atof per field — fractional values survive into the original's
               x2.55/x62 converts [orig: @0x5a0882..0x5a08c2], which the HUD
               layout makes (hud_layout_from_hudpos). */
            for (int i = 0; i < 3 && i < nvals; ++i)
                hud->alpha_fade[i] = hud_double(vals[i].s, vals[i].len);
        }
        /* AGL settings */
        else if (key_is(key, "hudagltlrx")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->agl_tlrx[i] = hud_number(vals[i].s, vals[i].len);
        } else if (key_is(key, "hudaglylen")) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->agl_ylen[i] = hud_number(vals[i].s, vals[i].len);
        }
        /* HUDSTANCE */
        else if (key_is(key, "hudstance")) {
            if (nvals >= 5) {
                DefHudStance st;
                memset(&st, 0, sizeof(st));
                st.id = hud_number(vals[0].s, vals[0].len);
                st.offset_x = hud_number(vals[1].s, vals[1].len);
                st.offset_y = hud_number(vals[2].s, vals[2].len);
                safe_copy(st.texture, sizeof(st.texture), vals[3].s, vals[3].len);
                safe_copy(st.name, sizeof(st.name), vals[4].s, vals[4].len);
                DA_PUSH(hud->stances, hud->stances_count, stance_cap, st);
            }
        }
        /* HUDDECLUT_<TOKEN> v0 v1 v2 v3 — the declutter table rows. Retail
           parses one arm per known token and builds a mask byte where bit i
           (1/2/4/8 for hud_detail level 0..3) is set iff atof(value i) != 0.0
           [orig: `fcomp dbl_7D0188` (0.0) @0x5A2174], stored
           into the 24-slot table; a token WITHOUT an arm authors nothing (the
           retail JOX file ships a 25th row, HUDDECLUT_CTAPE, that is exactly
           such a dead token — the binary has no arm for it).
           [orig: HUD_ParseHudposToken @0x59F370 -> byte_2723CE0[slot]]
           This parser keeps every row generically (flags normalized to 0/1);
           the consumer resolves token -> slot and drops unknown tokens
           (engine/runtime/hud/hud_declutter.cpp), which reproduces the
           no-arm behavior. */
        else if (strutil::starts_with_icase(key, "huddeclut_")) {
            /* The row's name is the key past the prefix, as authored. */
            if (nvals >= 4) {
                DefDeclutterEntry de;
                memset(&de, 0, sizeof(de));
                safe_copy(de.name, sizeof(de.name), key + 10, strlen(key + 10));
                for (int i = 0; i < 4; ++i)
                    de.flags[i] = hud_double(vals[i].s, vals[i].len) != 0.0 ? 1 : 0;
                DA_PUSH(hud->declutter, hud->declutter_count, declut_cap, de);
            }
        }
        /* Graphics */
        else if (key_is(key, "staticframe")) {
            /* The texture is token 1 as the tokenizer cut it: a `//` there is a
               comment, never part of the name [orig: the name copy @0x5a0a4e..0x5a0a62,
               x @0x5a0a72, y @0x5a0a8a]. */
            if (nvals >= 1) {
                DefHudGraphic gfx;
                memset(&gfx, 0, sizeof(gfx));
                safe_copy(gfx.texture, sizeof(gfx.texture), vals[0].s, vals[0].len);
                if (nvals >= 2) gfx.x = hud_number(vals[1].s, vals[1].len);
                if (nvals >= 3) gfx.y = hud_number(vals[2].s, vals[2].len);
                DA_PUSH(hud->static_frames, hud->static_frames_count, sf_cap, gfx);
            }
        } else if (key_is(key, "parachuteicon")) {
            if (nvals >= 3) {
                safe_copy(hud->parachute_icon.texture, sizeof(hud->parachute_icon.texture), vals[0].s, vals[0].len);
                hud->parachute_icon.x = hud_number(vals[1].s, vals[1].len);
                hud->parachute_icon.y = hud_number(vals[2].s, vals[2].len);
            }
        } else if (key_is(key, "armoricon")) {
            if (nvals >= 3) {
                safe_copy(hud->armor_icon.texture, sizeof(hud->armor_icon.texture), vals[0].s, vals[0].len);
                hud->armor_icon.x = hud_number(vals[1].s, vals[1].len);
                hud->armor_icon.y = hud_number(vals[2].s, vals[2].len);
            }
        }
        /* Any other key has no arm: the game reads past it, and so does the
           model (the writer's form leaves it out). */
    });

    return 0;
}

int def_parse_hudpos(const char *path, DefHudPosFile *out) {
    memset(out, 0, sizeof(*out));
    /* HUDORDERS defaults to -1 / -1, the globals' static value
       [orig: dword_2723D84 / dword_2723D88] */
    out->hud.orders[0] = out->hud.orders[1] = -1;
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_hudpos_buf(buf, file_len, out);
    free(buf);
    return rc;
}

int def_parse_hudpos_memory(const uint8_t *data, size_t size, DefHudPosFile *out) {
    memset(out, 0, sizeof(*out));
    /* HUDORDERS defaults to -1 / -1, the globals' static value
       [orig: dword_2723D84 / dword_2723D88] */
    out->hud.orders[0] = out->hud.orders[1] = -1;
    if (!data) return -1;
    return parse_hudpos_buf((const char *)data, size, out);
}

void def_free_hudpos(DefHudPosFile *f) {
    if (!f) return;
    free(f->hud.stances);
    free(f->hud.declutter);
    free(f->hud.static_frames);
    free(f->hud.vehicle_huds);
    memset(f, 0, sizeof(*f));
}

} // namespace opennova::def
