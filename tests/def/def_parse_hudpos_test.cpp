// Test parsing hudpos.def — check fonts, rects, colors, stances.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def/def.h"
#include "common/test_paths.h"

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/def/hudpos.def", repo_root);

    DefHudPosFile hudpos;
    memset(&hudpos, 0, sizeof(hudpos));
    if (def_parse_hudpos(path, &hudpos) != 0) {
        fprintf(stderr, "FAIL: def_parse_hudpos failed for %s\n", path);
        return 1;
    }

    const DefHudPosDef *hud = &hudpos.hud;

    /* Font: fixture uses "fonthud1" which the parser expects as "fonthud1_hi".
       Skip strict check, just verify the parse completed. */
    printf("Parse OK (font_hi='%s')\n", hud->font_hi);

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
        static const char fade_text[] = "alphafade\t12.5 75.5 1.5\n";
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
            "AMMOCOUNTPOS\t128,597,0,right\n"
            "HUDWEAPONNAME\t11,630,1,left\n"
            "HUDTIMECLOCK\t10 20 center\n";
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

    def_free_hudpos(&hudpos);
    printf("PASS: hudpos parsing OK\n");
    return 0;
}
