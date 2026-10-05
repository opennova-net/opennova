// Test parsing hudpos.def — check fonts, rects, colors, stances.
#include <cstdint>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/def/def.h>
#include <string>

#include "common/retail_paths.h"

using namespace opennova::def;

/* The synthetic legs: the HUDLS weapon slot bar tokens and ZONEINFO's
   three-field form and PAUSEDPOS, parsed from authored text (no retail
   bytes). [orig: HUD_ParseHudposToken — HUDLS_* @0x59FE41..0x59FF9C; ZONEINFO
   @0x5A0642..0x5A0676; PAUSEDPOS @0x59FC8D..0x59FCC8] */
static int synthetic_legs(void) {
    int failures = 0;
    static const char text[] =
        "HUDLS_SYSTEM\t1\r\n"
        "HUDLS_BRACKET\tls_brack.tga\r\n"
        "HUDLS_KEYOFST\t4,-6\r\n"
        "HUDLS_MOREAV\tls_more.tga 300 -2\r\n"
        "HUDLS_SLOT\t6 100,700\r\n"
        "HUDLS_SLOT\t10 180 700\r\n"
        "HUDLS_SLOT\t0 11 12\r\n"
        "HUDLS_SLOT\t11 13 14\r\n"
        "ZONEINFO\t1013,386,Right\r\n"
        "PAUSEDPOS\t980 12\r\n"
        "NETWORKINDICATOR\t6,5 30,5 70,5\r\n";
    DefHudPosFile f;
    memset(&f, 0, sizeof(f));
    if (def_parse_hudpos_memory((const unsigned char *)text, sizeof(text) - 1, &f) != 0) {
        fprintf(stderr, "FAIL: synthetic HUDLS parse failed\n");
        return 1;
    }
    const DefHudPosDef *h = &f.hud;
    if (h->hudls_system != 1) {
        fprintf(stderr, "FAIL: HUDLS_SYSTEM = %d\n", h->hudls_system);
        ++failures;
    }
    if (strcmp(h->hudls_bracket, "ls_brack.tga") != 0 || strcmp(h->hudls_moreav, "ls_more.tga") != 0) {
        fprintf(stderr, "FAIL: HUDLS texture names '%s' '%s'\n", h->hudls_bracket, h->hudls_moreav);
        ++failures;
    }
    if (h->hudls_keyofst[0] != 4 || h->hudls_keyofst[1] != -6) {
        fprintf(stderr, "FAIL: HUDLS_KEYOFST = %d,%d\n", h->hudls_keyofst[0], h->hudls_keyofst[1]);
        ++failures;
    }
    /* The MOREAV offsets keep the LOW BYTE, signed: 300 -> 44, -2 -> -2
       [orig: `mov byte_2723733, al` @0x59FF21 / movsx @0x599E30]. */
    if (h->hudls_moreav_off[0] != 44 || h->hudls_moreav_off[1] != -2) {
        fprintf(stderr, "FAIL: HUDLS_MOREAV offsets = %d,%d\n", h->hudls_moreav_off[0],
                h->hudls_moreav_off[1]);
        ++failures;
    }
    /* HUDLS_SLOT n lands at n-1 for n in 1..10; 0 and 11 author nothing. */
    int others = 0;
    for (int i = 0; i < 10; ++i)
        if (i != 5 && i != 9) others |= h->hudls_slot[i][0] | h->hudls_slot[i][1];
    if (h->hudls_slot[5][0] != 100 || h->hudls_slot[5][1] != 700 || h->hudls_slot[9][0] != 180 ||
        h->hudls_slot[9][1] != 700 || others != 0) {
        fprintf(stderr, "FAIL: HUDLS_SLOT placement (%d,%d) (%d,%d) others %d\n",
                h->hudls_slot[5][0], h->hudls_slot[5][1], h->hudls_slot[9][0],
                h->hudls_slot[9][1], others);
        ++failures;
    }
    /* ZONEINFO: x, y, then the alignment word as the THIRD token. */
    if (h->zone_info[0] != 1013 || h->zone_info[1] != 386 || h->zone_info[2] != 1) {
        fprintf(stderr, "FAIL: ZONEINFO = %d,%d,%d\n", h->zone_info[0], h->zone_info[1],
                h->zone_info[2]);
        ++failures;
    }
    /* PAUSEDPOS x y: the pause text's anchor [orig: @0x59FC8D..0x59FCC8]. */
    if (h->paused_pos[0] != 980 || h->paused_pos[1] != 12) {
        fprintf(stderr, "FAIL: PAUSEDPOS = %d,%d\n", h->paused_pos[0], h->paused_pos[1]);
        ++failures;
    }
    /* NETWORKINDICATOR: the three connection-indicator corners, six fields,
       with the presence flag [orig: @0x59F981..0x59FA0C]. */
    if (!h->network_indicator_present || h->network_indicator[0] != 6 ||
        h->network_indicator[1] != 5 || h->network_indicator[2] != 30 ||
        h->network_indicator[3] != 5 || h->network_indicator[4] != 70 ||
        h->network_indicator[5] != 5) {
        fprintf(stderr, "FAIL: NETWORKINDICATOR = %d (%d,%d %d,%d %d,%d)\n",
                h->network_indicator_present, h->network_indicator[0], h->network_indicator[1],
                h->network_indicator[2], h->network_indicator[3], h->network_indicator[4],
                h->network_indicator[5]);
        ++failures;
    }
    def_free_hudpos(&f);
    if (failures == 0)
        printf("HUDLS + ZONEINFO + PAUSEDPOS + NETWORKINDICATOR synthetic legs OK\n");
    return failures;
}

/* Every line as the retail tokenizer cuts it: the key is the whole first
   token (`KEY,value` binds), a quoted value is one token without its quotes,
   and a HUD key inside a VEHICLE_HUD block is the HUD's as anywhere else (the
   block keys are matched ahead of the HUD chain, never instead of it)
   [orig: File_ParseASCIIFile @0x53D810 -> Terrain_TokenizeConfigLine
   @0x53CB60; HUD_ParseHudposToken @0x59F370 (VEHICLE_HUD @0x59F380, the
   block keys @0x59F5CE..0x59F74E, the HUD chain from @0x59F7CE)]. */
static int tokenizer_legs() {
    const char *text =
        "fonthud1_hi,FontHi.fnt\r\n"
        "HUDCLIP \"5\" \"579\"\r\n"
        "VEHICLE_HUD\r\n"
        "  sid dbuggy1\r\n"
        "  HUDCHLINE 7\r\n"
        "VEHICLE_END\r\n";
    DefHudPosFile tok;
    memset(&tok, 0, sizeof(tok));
    if (def_parse_hudpos_memory((const unsigned char *)text, strlen(text), &tok) != 0) {
        fprintf(stderr, "FAIL: tokenizer snippet did not parse\n");
        return 1;
    }
    const bool ok = strcmp(tok.hud.font_hi, "FontHi.fnt") == 0 && tok.hud.clip_pos[0] == 5 &&
            tok.hud.clip_pos[1] == 579 && tok.hud.hud_chline == 7 &&
            tok.hud.vehicle_huds_count == 1;
    if (!ok)
        fprintf(stderr, "FAIL: tokenizer snippet: font '%s' clip %d,%d chline %d blocks %zu\n",
                tok.hud.font_hi, tok.hud.clip_pos[0], tok.hud.clip_pos[1], tok.hud.hud_chline,
                tok.hud.vehicle_huds_count);
    def_free_hudpos(&tok);
    if (ok) printf("hudpos tokenizer lines OK\n");
    return ok ? 0 : 1;
}

/* A line's values are the tokenizer's 29 past the key: an eight-seat VEHICLE_HUD
   row is 17 values, and its last seat is the 16th and 17th [orig:
   Terrain_TokenizeConfigLine @0x53CB60, the 30-token cap @0x53CC8C..0x53CC93;
   HUD_ParseHudposToken's seats arm @0x59F74E, eight pairs at most]. */
static int value_cap_legs() {
    const char *text =
        "VEHICLE_HUD\r\n"
        "  sid dch471\r\n"
        "  seats 8,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16\r\n"
        "VEHICLE_END\r\n";
    DefHudPosFile f;
    memset(&f, 0, sizeof(f));
    if (def_parse_hudpos_memory((const unsigned char *)text, strlen(text), &f) != 0 ||
        f.hud.vehicle_huds_count != 1) {
        fprintf(stderr, "FAIL: eight-seat snippet did not parse\n");
        def_free_hudpos(&f);
        return 1;
    }
    const DefVehicleHudBlock &b = f.hud.vehicle_huds[0];
    const bool ok = b.seat_count == 8 && b.seat_x[7] == 15 && b.seat_y[7] == 16;
    if (!ok)
        fprintf(stderr, "FAIL: eight seats read %d, the last at %d,%d\n", b.seat_count,
                b.seat_x[7], b.seat_y[7]);
    def_free_hudpos(&f);
    if (ok) printf("hudpos 29-value lines OK\n");
    return ok ? 0 : 1;
}

/* Every hudpos number is the CRT's atof of its token, then _ftol2_sse for an
   integer slot [orig: HUD_ParseHudposToken @0x59F370, e.g. HUDSPINMAPX1
   @0x59F7E4..0x59F7EC; _atof @0x76B6A1]: an exponent counts, a value past the
   int range is the integer indefinite, a hex spelling reads 0, and a declutter
   flag is set by any value atof does not read as 0.0 [orig: the `fcomp
   dbl_7D0188` (0.0) @0x5A2174]. ALPHAFADE keeps atof's double for the layout's
   x2.55 / x62 converts. */
static int atof_legs() {
    const char *text =
        "HUDSPINMAPX1 1e2\r\n"
        "HUDSPINMAPX2 3000000000\r\n"
        "HUDSPINMAPY1 0x40\r\n"
        "HUDSPINMAPY2 -7.9\r\n"
        "HUDDECLUT_SPINMAP 0.5 0 2d1 0\r\n"
        "alphafade 20 100.5 0.5\r\n";
    DefHudPosFile f;
    memset(&f, 0, sizeof(f));
    if (def_parse_hudpos_memory((const unsigned char *)text, strlen(text), &f) != 0 ||
        f.hud.declutter_count != 1) {
        fprintf(stderr, "FAIL: atof snippet did not parse\n");
        def_free_hudpos(&f);
        return 1;
    }
    const DefDeclutterEntry &d = f.hud.declutter[0];
    const bool ok = f.hud.spinmap_x1 == 100 && f.hud.spinmap_x2 == INT32_MIN &&
            f.hud.spinmap_y1 == 0 && f.hud.spinmap_y2 == -7 && d.flags[0] == 1 &&
            d.flags[1] == 0 && d.flags[2] == 1 && d.flags[3] == 0 &&
            f.hud.alpha_fade[0] == 20.0 && f.hud.alpha_fade[1] == 100.5 &&
            f.hud.alpha_fade[2] == 0.5;
    if (!ok)
        fprintf(stderr, "FAIL: atof reads: spinmap %d %d %d %d declutter %d%d%d%d alphafade "
                "%g %g %g\n", f.hud.spinmap_x1, f.hud.spinmap_x2, f.hud.spinmap_y1,
                f.hud.spinmap_y2, d.flags[0], d.flags[1], d.flags[2], d.flags[3],
                (double)f.hud.alpha_fade[0], (double)f.hud.alpha_fade[1],
                (double)f.hud.alpha_fade[2]);
    def_free_hudpos(&f);
    if (ok) printf("hudpos atof reads OK\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    if (synthetic_legs() != 0) return 1;
    if (tokenizer_legs() != 0) return 1;
    if (atof_legs() != 0) return 1;
    if (value_cap_legs() != 0) return 1;
    /* Every remaining leg reads the shipped hudpos.def (the memory legs compare
       against its path parse), so they gate on the reference fixture set
       (OPENNOVA_JO_ASSETS). */
    const std::string fixture = retail::reference_fixture("def/hudpos.def");
    if (fixture.empty())
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/def/hudpos.def (the shipped HUD layout table)");
    const char *path = fixture.c_str();

    DefHudPosFile hudpos;
    memset(&hudpos, 0, sizeof(hudpos));
    if (def_parse_hudpos(path, &hudpos) != 0) {
        fprintf(stderr, "FAIL: def_parse_hudpos failed for %s\n", path);
        return 1;
    }

    const DefHudPosDef *hud = &hudpos.hud;

    /* Fonts: this reference hudpos.def (the JOX one) names its face on a bare
       `fonthud1` line, which is no key: HUD_ParseHudposToken @0x59F370 compares
       whole tokens with _stricmp against fonthud1_hi / fonthud1_lo, so both
       names stay empty and retail's HUD slot takes the bold label font
       (HUD_SelectHudposFont @0x591890). */
    if (hud->font_hi[0] != '\0' || hud->font_lo[0] != '\0') {
        fprintf(stderr, "FAIL: a bare fonthud1 line named a font: hi='%s' lo='%s'\n",
                hud->font_hi, hud->font_lo);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("Fonts OK (no suffixed key, both names empty)\n");

    /* Test health rect */
    if (hud->health[0] != 2 || hud->health[1] != 739 ||
        hud->health[2] != 141 || hud->health[3] != 757) {
        fprintf(stderr, "FAIL: health rect mismatch: %d,%d,%d,%d\n",
                hud->health[0], hud->health[1], hud->health[2], hud->health[3]);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("Health rect OK\n");

    /* Test colors (RGB) — hud_textcolor 251,213,5 */
    if (hud->hud_textcolor.r != 251 || hud->hud_textcolor.g != 213 ||
        hud->hud_textcolor.b != 5) {
        fprintf(stderr, "FAIL: hud_textcolor mismatch: %d,%d,%d\n",
                hud->hud_textcolor.r, hud->hud_textcolor.g, hud->hud_textcolor.b);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("Text colors OK\n");

    /* Test colors (RGBA) — stancecolor_good: a=200, r=5, g=249, b=12 */
    if (hud->stancecolor_good.r != 5 || hud->stancecolor_good.g != 249 ||
        hud->stancecolor_good.b != 12 || hud->stancecolor_good.a != 200) {
        fprintf(stderr, "FAIL: stancecolor_good mismatch: r=%d,g=%d,b=%d,a=%d\n",
                hud->stancecolor_good.r, hud->stancecolor_good.g,
                hud->stancecolor_good.b, hud->stancecolor_good.a);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("RGBA colors OK\n");

    /* Test spinmap bounds */
    if (hud->spinmap_x1 != 810 || hud->spinmap_x2 != 1020 ||
        hud->spinmap_y1 != 552 || hud->spinmap_y2 != 762) {
        fprintf(stderr, "FAIL: spinmap bounds mismatch: x1=%d,x2=%d,y1=%d,y2=%d\n",
                hud->spinmap_x1, hud->spinmap_x2, hud->spinmap_y1, hud->spinmap_y2);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("Spinmap bounds OK\n");

    /* The spinmap waypoint-distance suppressor parses its value; an absent
       token keeps the retail BSS-zero initializer (label LIVE) — the sole consumer
       is an ==0 test. [orig: dword_27237C0 (.data, no file
       bytes); parse @0x59fc1f; read @0x5a7a6a] */
    {
        static const char spinmap_text[] = "SPINMAPWPDISTOFF 17\r\n";
        DefHudPosFile sf;
        memset(&sf, 0, sizeof(sf));
        if (def_parse_hudpos_memory((const unsigned char *)spinmap_text,
                                    sizeof(spinmap_text) - 1, &sf) != 0 ||
            sf.hud.spinmap_wp_dist_off != 17) {
            fprintf(stderr, "FAIL: SPINMAPWPDISTOFF mismatch: %d\n",
                    sf.hud.spinmap_wp_dist_off);
            def_free_hudpos(&sf);
            def_free_hudpos(&hudpos);
            return 1;
        }
        def_free_hudpos(&sf);
    }
    if (hud->spinmap_wp_dist_off != 0) {
        fprintf(stderr, "FAIL: absent SPINMAPWPDISTOFF did not retain 0\n");
        def_free_hudpos(&hudpos);
        return 1;
    }

    /* MAPCOORDS: x, y, suppressor. A word 3rd token parses 0 (shown, the
       retail atof), a missing 3rd token writes 0, and an absent key keeps
       the BSS-zero initializer (label LIVE). [orig: mapcoords parse @0x5a0920 ->
       screenX/screenY/dword_27236FC (.data, no file bytes)] */
    {
        static const char mc_text[] = "MAPCOORDS 530,720,center\r\n";
        static const char mc_short[] = "MAPCOORDS 10,20\r\n";
        static const char mc_none[] = "HUDTIMECLOCK 98,32\r\n";
        DefHudPosFile sf;
        memset(&sf, 0, sizeof(sf));
        if (def_parse_hudpos_memory((const unsigned char *)mc_text,
                                    sizeof(mc_text) - 1, &sf) != 0 ||
            sf.hud.map_coords[0] != 530 || sf.hud.map_coords[1] != 720 ||
            sf.hud.map_coords[2] != 0) {
            fprintf(stderr, "FAIL: MAPCOORDS word-suppressor mismatch: %d,%d,%d\n",
                    sf.hud.map_coords[0], sf.hud.map_coords[1],
                    sf.hud.map_coords[2]);
            def_free_hudpos(&sf);
            def_free_hudpos(&hudpos);
            return 1;
        }
        def_free_hudpos(&sf);
        memset(&sf, 0, sizeof(sf));
        if (def_parse_hudpos_memory((const unsigned char *)mc_short,
                                    sizeof(mc_short) - 1, &sf) != 0 ||
            sf.hud.map_coords[2] != 0) {
            fprintf(stderr, "FAIL: 2-value MAPCOORDS suppressor: %d\n",
                    sf.hud.map_coords[2]);
            def_free_hudpos(&sf);
            def_free_hudpos(&hudpos);
            return 1;
        }
        def_free_hudpos(&sf);
        memset(&sf, 0, sizeof(sf));
        if (def_parse_hudpos_memory((const unsigned char *)mc_none,
                                    sizeof(mc_none) - 1, &sf) != 0 ||
            sf.hud.map_coords[2] != 0) {
            fprintf(stderr, "FAIL: absent MAPCOORDS did not retain 0: %d\n",
                    sf.hud.map_coords[2]);
            def_free_hudpos(&sf);
            def_free_hudpos(&hudpos);
            return 1;
        }
        def_free_hudpos(&sf);
    }

    /* Test stances — should have 5 */
    if (hud->stances_count != 5) {
        fprintf(stderr, "FAIL: stances count mismatch: %zu\n", hud->stances_count);
        def_free_hudpos(&hudpos);
        return 1;
    }

    /* Check first stance */
    if (hud->stances[0].id != 0 ||
        strcmp(hud->stances[0].texture, "stance_1.tga") != 0 ||
        strcmp(hud->stances[0].name, "STAND") != 0) {
        fprintf(stderr, "FAIL: stance 0 mismatch: id=%d, texture='%s', name='%s'\n",
                hud->stances[0].id, hud->stances[0].texture, hud->stances[0].name);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("Stances OK (%zu entries)\n", hud->stances_count);

    /* Test alphafade — raw float fields (base %, max %, seconds) */
    if (hud->alpha_fade[0] != 30.0f || hud->alpha_fade[1] != 50.0f ||
        hud->alpha_fade[2] != 3.0f) {
        fprintf(stderr, "FAIL: alphafade mismatch: %g,%g,%g\n",
                hud->alpha_fade[0], hud->alpha_fade[1], hud->alpha_fade[2]);
        def_free_hudpos(&hudpos);
        return 1;
    }

    /* Fractional ALPHAFADE survives the parse: the original reads each field via
       atof and the fraction feeds the x2.55/x62 converts [orig: @0x5a0882..0x5a08c2].
       An integer parse would truncate 1.5 s to a 62-tick ramp instead of 93. */
    {
        static const char fade_text[] = "alphafade\t12.5 75.5 1.5\r\n";
        DefHudPosFile ff;
        memset(&ff, 0, sizeof(ff));
        if (def_parse_hudpos_memory((const unsigned char *)fade_text,
                                    sizeof(fade_text) - 1, &ff) != 0 ||
            ff.hud.alpha_fade[0] != 12.5f || ff.hud.alpha_fade[1] != 75.5f ||
            ff.hud.alpha_fade[2] != 1.5f ||
            (int)(ff.hud.alpha_fade[2] * 62.0) != 93) {
            fprintf(stderr, "FAIL: fractional alphafade mismatch: %g,%g,%g\n",
                    ff.hud.alpha_fade[0], ff.hud.alpha_fade[1], ff.hud.alpha_fade[2]);
            def_free_hudpos(&ff);
            def_free_hudpos(&hudpos);
            return 1;
        }
        def_free_hudpos(&ff);
    }
    printf("Alphafade OK\n");

    /* Test hud_chline */
    if (hud->hud_chline != 8) {
        fprintf(stderr, "FAIL: hud_chline mismatch: %d\n", hud->hud_chline);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("Single values OK\n");

    /* Positioned text: GAMEINFO 1013,430 (2 fields) -> x,y with hidden 0, align 0. */
    if (hud->game_info[0] != 1013 || hud->game_info[1] != 430 ||
        hud->game_info[2] != 0 || hud->game_info[3] != 0) {
        fprintf(stderr, "FAIL: game_info mismatch: %d,%d,%d,%d\n",
                hud->game_info[0], hud->game_info[1], hud->game_info[2], hud->game_info[3]);
        def_free_hudpos(&hudpos);
        return 1;
    }

    /* The 4-field positioned form (x, y, hidden, align) the original parses —
       strictly positional: field 3 is ALWAYS the hidden gate (a word reads 0 via
       atof), field 4 the alignment word, left when missing. Retail 2-field lines
       (GAMEINFO, HUDCHATTEXT) render visible/left in retail JO, pinning missing
       fields to 0. [orig: AMMOCOUNTPOS parse @0x59fc3d; HUD_ParseTextAlignment
       @0x59d6b0] */
    {
        static const char pos_text[] =
            "AMMOCOUNTPOS\t128,597,0,right\r\n"
            "HUDWEAPONNAME\t11,630,1,left\r\n"
            "HUDTIMECLOCK\t10 20 center\r\n";
        DefHudPosFile pf;
        memset(&pf, 0, sizeof(pf));
        if (def_parse_hudpos_memory((const unsigned char *)pos_text,
                                    sizeof(pos_text) - 1, &pf) != 0) {
            fprintf(stderr, "FAIL: positioned-form memory parse failed\n");
            def_free_hudpos(&hudpos);
            return 1;
        }
        const DefHudPosDef *p = &pf.hud;
        /* HUDTIMECLOCK "10 20 center": field 3 = atof("center") = 0 (visible),
           field 4 missing = left — NOT center. */
        int ok = p->ammo_count_pos[0] == 128 && p->ammo_count_pos[1] == 597 &&
                 p->ammo_count_pos[2] == 0 && p->ammo_count_pos[3] == 1 &&
                 p->weapon_name_pos[0] == 11 && p->weapon_name_pos[1] == 630 &&
                 p->weapon_name_pos[2] == 1 && p->weapon_name_pos[3] == 0 &&
                 p->time_clock[0] == 10 && p->time_clock[1] == 20 &&
                 p->time_clock[2] == 0 && p->time_clock[3] == 0;
        if (!ok) {
            fprintf(stderr,
                    "FAIL: positioned form mismatch: ammo %d,%d,%d,%d name %d,%d,%d,%d clock %d,%d,%d,%d\n",
                    p->ammo_count_pos[0], p->ammo_count_pos[1], p->ammo_count_pos[2], p->ammo_count_pos[3],
                    p->weapon_name_pos[0], p->weapon_name_pos[1], p->weapon_name_pos[2], p->weapon_name_pos[3],
                    p->time_clock[0], p->time_clock[1], p->time_clock[2], p->time_clock[3]);
            def_free_hudpos(&pf);
            def_free_hudpos(&hudpos);
            return 1;
        }
        def_free_hudpos(&pf);
    }

    /* BREATHTIME is the three-field form: x, y, then the alignment word as
       the THIRD token (no hidden dword), so JO's "512,70,center" centres the
       breath bar. [orig: HUD_ParseHudposToken @0x59FB3B..0x59FB84 ->
       dword_2723810/14/18 via atof, atof, HUD_ParseTextAlignment] */
    {
        static const char breath_center[] = "BREATHTIME\t\t512,70,center\r\n";
        static const char breath_right[] = "BREATHTIME 10,20,RIGHT\r\n";
        static const char breath_short[] = "BREATHTIME 30,40\r\n";
        const char *texts[] = {breath_center, breath_right, breath_short};
        const size_t lens[] = {sizeof(breath_center) - 1, sizeof(breath_right) - 1,
                               sizeof(breath_short) - 1};
        const int want[3][3] = {{512, 70, 2}, {10, 20, 1}, {30, 40, 0}};
        for (int c = 0; c < 3; ++c) {
            DefHudPosFile bf;
            memset(&bf, 0, sizeof(bf));
            if (def_parse_hudpos_memory((const unsigned char *)texts[c], lens[c], &bf) != 0 ||
                bf.hud.breath_time[0] != want[c][0] || bf.hud.breath_time[1] != want[c][1] ||
                bf.hud.breath_time[2] != want[c][2]) {
                fprintf(stderr, "FAIL: BREATHTIME case %d parsed %d,%d,%d\n", c,
                        bf.hud.breath_time[0], bf.hud.breath_time[1], bf.hud.breath_time[2]);
                def_free_hudpos(&bf);
                def_free_hudpos(&hudpos);
                return 1;
            }
            def_free_hudpos(&bf);
        }
    }
    printf("Positioned text fields OK\n");

    /* Test declutter — MSNTITLE should exist */
    if (hud->declutter_count == 0) {
        fprintf(stderr, "FAIL: declutter is empty\n");
        def_free_hudpos(&hudpos);
        return 1;
    }

    const DefDeclutterEntry *msntitle = NULL;
    for (size_t i = 0; i < hud->declutter_count; ++i) {
        if (strcmp(hud->declutter[i].name, "MSNTITLE") == 0) {
            msntitle = &hud->declutter[i];
            break;
        }
    }
    if (!msntitle) {
        fprintf(stderr, "FAIL: declutter MSNTITLE not found\n");
        def_free_hudpos(&hudpos);
        return 1;
    }
    /* HUDDECLUT_MSNTITLE 0 1 1 0 */
    if (msntitle->flags[0] != 0 || msntitle->flags[1] != 1 ||
        msntitle->flags[2] != 1 || msntitle->flags[3] != 0) {
        fprintf(stderr, "FAIL: declutter MSNTITLE flags mismatch: %d %d %d %d\n",
                msntitle->flags[0], msntitle->flags[1], msntitle->flags[2], msntitle->flags[3]);
        def_free_hudpos(&hudpos);
        return 1;
    }
    printf("Declutter OK (%zu entries)\n", hud->declutter_count);

    /* Memory-variant equivalence: parsing the same bytes via
       def_parse_hudpos_memory must reproduce the path parse field-for-field. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot reopen fixture for memory test\n");
        def_free_hudpos(&hudpos);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *bytes = (unsigned char *)malloc((size_t)sz);
    if (!bytes || fread(bytes, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "FAIL: cannot read fixture bytes\n");
        free(bytes);
        fclose(f);
        def_free_hudpos(&hudpos);
        return 1;
    }
    fclose(f);

    DefHudPosFile hudpos_mem;
    memset(&hudpos_mem, 0, sizeof(hudpos_mem));
    if (def_parse_hudpos_memory(bytes, (size_t)sz, &hudpos_mem) != 0) {
        fprintf(stderr, "FAIL: def_parse_hudpos_memory failed\n");
        free(bytes);
        def_free_hudpos(&hudpos);
        return 1;
    }
    free(bytes);

    const DefHudPosDef *m = &hudpos_mem.hud;
    int mem_ok =
        memcmp(m->health, hud->health, sizeof(hud->health)) == 0 &&
        m->hud_textcolor.r == hud->hud_textcolor.r &&
        m->hud_textcolor.g == hud->hud_textcolor.g &&
        m->hud_textcolor.b == hud->hud_textcolor.b &&
        m->spinmap_x1 == hud->spinmap_x1 && m->spinmap_x2 == hud->spinmap_x2 &&
        m->spinmap_y1 == hud->spinmap_y1 && m->spinmap_y2 == hud->spinmap_y2 &&
        m->stances_count == hud->stances_count &&
        m->declutter_count == hud->declutter_count &&
        m->hud_chline == hud->hud_chline &&
        strcmp(m->font_hi, hud->font_hi) == 0;
    if (mem_ok && hud->stances_count > 0) {
        mem_ok = m->stances[0].id == hud->stances[0].id &&
                 strcmp(m->stances[0].texture, hud->stances[0].texture) == 0 &&
                 strcmp(m->stances[0].name, hud->stances[0].name) == 0;
    }
    if (!mem_ok) {
        fprintf(stderr, "FAIL: memory parse differs from path parse\n");
        def_free_hudpos(&hudpos_mem);
        def_free_hudpos(&hudpos);
        return 1;
    }
    def_free_hudpos(&hudpos_mem);
    printf("Memory-variant equivalence OK\n");

    /* StaticFrame is NOT a list in retail: the handler copies each dispatched
       line over the previous one, so the LAST authored line is the one that
       draws [orig: HUD_ParseHudposToken @0x59F370 - name copy @0x5a0a4e-
       0x5a0a62, x @0x5a0a72, y @0x5a0a8a]. We keep every line so the writer
       round-trips, which makes "which one" a consumer policy -- pinned here so
       the parser side of that contract (all lines recorded, IN ORDER) cannot
       drift under hud_static_frame_index(). */
    {
        const char *two_frames =
            "StaticFrame\tH_BlkHLin.tga  512,720\r\n"
            "StaticFrame\tCompMark.tga  508,685\r\n";
        DefHudPosFile multi;
        memset(&multi, 0, sizeof(multi));
        if (def_parse_hudpos_memory((const unsigned char *)two_frames,
                                    strlen(two_frames), &multi) != 0) {
            fprintf(stderr, "FAIL: two-StaticFrame memory parse failed\n");
            def_free_hudpos(&hudpos);
            return 1;
        }
        if (multi.hud.static_frames_count != 2) {
            fprintf(stderr, "FAIL: expected 2 StaticFrame entries, got %zu\n",
                    multi.hud.static_frames_count);
            def_free_hudpos(&multi);
            def_free_hudpos(&hudpos);
            return 1;
        }
        if (strcmp(multi.hud.static_frames[0].texture, "H_BlkHLin.tga") != 0 ||
            strcmp(multi.hud.static_frames[1].texture, "CompMark.tga") != 0) {
            fprintf(stderr, "FAIL: StaticFrame entries out of authored order\n");
            def_free_hudpos(&multi);
            def_free_hudpos(&hudpos);
            return 1;
        }
        /* The one retail draws is the LAST: 508,685, not 512,720. */
        if (multi.hud.static_frames[1].x != 508 ||
            multi.hud.static_frames[1].y != 685) {
            fprintf(stderr, "FAIL: last StaticFrame position wrong (%d,%d)\n",
                    multi.hud.static_frames[1].x, multi.hud.static_frames[1].y);
            def_free_hudpos(&multi);
            def_free_hudpos(&hudpos);
            return 1;
        }
        def_free_hudpos(&multi);
        printf("StaticFrame last-wins ordering OK\n");
    }

    /* VEHICLE_HUD blocks parse IN PARALLEL with the raw_lines passthrough:
       the typed blocks appear AND every line still round-trips through
       raw_lines [orig: HUD_ParseHudposToken @0x59F370 - the token arms and
       the VEHICLE_END commit]. The tracked fixture carries 28 blocks. */
    if (hud->vehicle_huds_count == 0) {
        fprintf(stderr, "FAIL: no VEHICLE_HUD blocks parsed\n");
        def_free_hudpos(&hudpos);
        return 1;
    }
    {
        const DefVehicleHudBlock *buggy = NULL;
        for (size_t i = 0; i < hud->vehicle_huds_count; ++i) {
            if (strcmp(hud->vehicle_huds[i].sid, "dbuggy1") == 0) {
                buggy = &hud->vehicle_huds[i];
                break;
            }
        }
        if (!buggy) {
            fprintf(stderr, "FAIL: dbuggy1 VEHICLE_HUD block missing\n");
            def_free_hudpos(&hudpos);
            return 1;
        }
        /* sid dbuggy1 / interface h_buggya.tga / driver 16,196 /
           emplace 1,27,220 / seats 1,38,196 -- note the values are
           COMMA-separated, which the shared splitter handles. */
        if (strcmp(buggy->interface_texture, "h_buggya.tga") != 0) {
            fprintf(stderr, "FAIL: interface texture = %s\n",
                    buggy->interface_texture);
            def_free_hudpos(&hudpos);
            return 1;
        }
        if (buggy->driver_x != 16 || buggy->driver_y != 196) {
            fprintf(stderr, "FAIL: driver pos = %d,%d\n",
                    buggy->driver_x, buggy->driver_y);
            def_free_hudpos(&hudpos);
            return 1;
        }
        /* The leading value is a COUNT, not a coordinate: "emplace 1,27,220"
           is one pair at (27,220), not three numbers. */
        if (buggy->emplace_count != 1 || buggy->emplace_x[0] != 27 ||
            buggy->emplace_y[0] != 220) {
            fprintf(stderr, "FAIL: emplace = %d @ %d,%d\n",
                    buggy->emplace_count, buggy->emplace_x[0], buggy->emplace_y[0]);
            def_free_hudpos(&hudpos);
            return 1;
        }
        if (buggy->seat_count != 1 || buggy->seat_x[0] != 38 ||
            buggy->seat_y[0] != 196) {
            fprintf(stderr, "FAIL: seats = %d @ %d,%d\n",
                    buggy->seat_count, buggy->seat_x[0], buggy->seat_y[0]);
            def_free_hudpos(&hudpos);
            return 1;
        }
        printf("VEHICLE_HUD blocks parsed OK (%zu)\n",
               hud->vehicle_huds_count);
    }

    def_free_hudpos(&hudpos);
    printf("PASS: hudpos parsing OK\n");
    return 0;
}
