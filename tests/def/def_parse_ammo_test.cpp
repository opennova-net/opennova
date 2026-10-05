// Test parsing ammo.def — check ROCKET and AT_NULL entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/def/def.h>
#include <string>

#include "common/retail_paths.h"

using namespace opennova::def;

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    /* The shipped ammo.def from the reference fixture set (OPENNOVA_JO_ASSETS):
       its per-round pins are the SKIP-LEG retail leg; the digit-walker block
       below runs unconditionally. */
    const std::string fixture = retail::reference_fixture("def/ammo.def");
    const bool have_retail = !fixture.empty();
    const char *path = fixture.c_str();

    DefAmmoFile ammo;
    memset(&ammo, 0, sizeof(ammo));
    if (have_retail) {
    if (def_parse_ammo(path, &ammo) != 0) {
        fprintf(stderr, "FAIL: def_parse_ammo failed for %s\n", path);
        return 1;
    }

    if (ammo.count < 5) {
        fprintf(stderr, "FAIL: too few ammo entries: %zu\n", ammo.count);
        def_free_ammo(&ammo);
        return 1;
    }

    printf("Parsed %zu ammo types\n", ammo.count);

    /* Find the ROCKET entry */
    const DefAmmoDef *rocket = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "ROCKET") == 0) {
            rocket = &ammo.entries[i];
            break;
        }
    }

    if (!rocket) {
        fprintf(stderr, "FAIL: could not find ROCKET entry\n");
        def_free_ammo(&ammo);
        return 1;
    }

    if (rocket->velocity != 390) {
        fprintf(stderr, "FAIL: ROCKET velocity mismatch: %d\n", rocket->velocity);
        def_free_ammo(&ammo);
        return 1;
    }

    if (rocket->penetration_impact != 100) {
        fprintf(stderr, "FAIL: ROCKET penetration_impact mismatch: %d\n", rocket->penetration_impact);
        def_free_ammo(&ammo);
        return 1;
    }

    if (rocket->penetration_kz != 100) {
        fprintf(stderr, "FAIL: ROCKET penetration_kz mismatch: %d\n", rocket->penetration_kz);
        def_free_ammo(&ammo);
        return 1;
    }

    if (rocket->scorch_id != 2) {
        fprintf(stderr, "FAIL: ROCKET scorch_id mismatch: %d\n", rocket->scorch_id);
        def_free_ammo(&ammo);
        return 1;
    }

    /* Fire-presentation fields (world-wac-ai-re §17.4): ai_launch sound-set name; a
       one-value `tracer_type rocket` fills BOTH slots (friendly copies into enemy). */
    if (strcmp(rocket->ai_launch, "GS_AT4") != 0) {
        fprintf(stderr, "FAIL: ROCKET ai_launch: got '%s' want 'GS_AT4'\n", rocket->ai_launch);
        def_free_ammo(&ammo);
        return 1;
    }
    if (rocket->tracer_type_friendly != 3 || rocket->tracer_type_enemy != 3) {
        fprintf(stderr, "FAIL: ROCKET tracer_type: got %d/%d want 3/3 (one-value copy)\n",
                rocket->tracer_type_friendly, rocket->tracer_type_enemy);
        def_free_ammo(&ammo);
        return 1;
    }
    /* The tracer round's item graphic id (raw type id kept, embedder resolves) and the
       in-flight glow: `frndlyTrcrID 4502`, `light_move 6.0 128 120 80`
       [orig: AmmoDef_ParseProperty @0x40a5f8 -> +16; light_move -> +120 fp16 /
       +124 = ((r<<8)+g)<<8 + b]. */
    if (rocket->frndly_trcr_type_id != 4502 || rocket->foe_trcr_type_id != 0) {
        fprintf(stderr, "FAIL: ROCKET frndlyTrcrID/foeTrcrID: got %d/%d want 4502/0\n",
                rocket->frndly_trcr_type_id, rocket->foe_trcr_type_id);
        def_free_ammo(&ammo);
        return 1;
    }
    if (rocket->light_move_radius_fp16 != 6 * 65536 ||
        rocket->light_move_color != ((128 << 16) | (120 << 8) | 80)) {
        fprintf(stderr, "FAIL: ROCKET light_move: got %d/0x%X want %d/0x%X\n",
                rocket->light_move_radius_fp16, rocket->light_move_color, 6 * 65536,
                (128 << 16) | (120 << 8) | 80);
        def_free_ammo(&ammo);
        return 1;
    }
    /* The impact flash: `light_impact 10.0 255 192 96 0.2` [orig:
       AmmoDef_ParseProperty @0x40af79 -> +132 fp16 / +128 packed /
       +136 = (62 * 0.2 fp16 + 0x8000) >> 16 = 12 ticks; 0 -> 10 @0x40b005]. */
    if (rocket->light_impact_radius_fp16 != 10 * 65536 ||
        rocket->light_impact_color != ((255 << 16) | (192 << 8) | 96) ||
        rocket->light_impact_ticks != 12) {
        fprintf(stderr, "FAIL: ROCKET light_impact: got %d/0x%X/%d want %d/0x%X/12\n",
                rocket->light_impact_radius_fp16, rocket->light_impact_color,
                rocket->light_impact_ticks, 10 * 65536,
                (255 << 16) | (192 << 8) | 96);
        def_free_ammo(&ammo);
        return 1;
    }

    /* Find the AT_NULL entry — should have 0 velocity */
    const DefAmmoDef *null_ammo = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "AT_NULL") == 0) {
            null_ammo = &ammo.entries[i];
            break;
        }
    }

    if (!null_ammo) {
        fprintf(stderr, "FAIL: could not find AT_NULL entry\n");
        def_free_ammo(&ammo);
        return 1;
    }

    if (null_ammo->velocity != 0) {
        fprintf(stderr, "FAIL: AT_NULL velocity should be 0, got %d\n", null_ammo->velocity);
        def_free_ammo(&ammo);
        return 1;
    }
    if (null_ammo->scorch_id != 0) {
        fprintf(stderr, "FAIL: AT_NULL scorch_id should be 0, got %d\n",
                null_ammo->scorch_id);
        def_free_ammo(&ammo);
        return 1;
    }

    /* The round-sim field set (net-re §5.60) against the real AMMO_CAR15_556MM block:
       velocity 854, max_age 3 s -> 186 ticks, arm_age 0, error 0, drag 0.255 -> 16712
       (16.16), weight 62, penetration 20, kztype rounds_kz_C4 = 5, tracerRate 3,
       bullet_radius 0.00278 -> 182. */
    const DefAmmoDef *car15 = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "AMMO_CAR15_556MM") == 0) {
            car15 = &ammo.entries[i];
            break;
        }
    }
    if (!car15) {
        fprintf(stderr, "FAIL: could not find AMMO_CAR15_556MM entry\n");
        def_free_ammo(&ammo);
        return 1;
    }
    /* The throwable fields against the real claymore block (world-wac-ai-re
       §27): kz_pieslice stores the HALF-angle — (24 / 2) * 11930464 BAM
       [orig: AmmoDef_ParseProperty @0x40ad51 signed div 2 -> imul 0xB60B60] —
       and the TrcrID item ids ride +16/+20. */
    const DefAmmoDef *claymore = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "claymore") == 0) {
            claymore = &ammo.entries[i];
            break;
        }
    }
    if (!claymore) {
        fprintf(stderr, "FAIL: could not find claymore entry\n");
        def_free_ammo(&ammo);
        return 1;
    }
    if (claymore->kz_pieslice_bam != 12 * 11930464) {
        fprintf(stderr, "FAIL: claymore kz_pieslice_bam want %ld got %ld\n",
                (long)(12 * 11930464), (long)claymore->kz_pieslice_bam);
        def_free_ammo(&ammo);
        return 1;
    }
    if (claymore->frndly_trcr_type_id != 1895 || claymore->foe_trcr_type_id != 1895) {
        fprintf(stderr, "FAIL: claymore TrcrIDs want 1895/1895 got %d/%d\n",
                claymore->frndly_trcr_type_id, claymore->foe_trcr_type_id);
        def_free_ammo(&ammo);
        return 1;
    }

    /* The two byte selectors consumed by Entity_ApplyCollisionForce live at
       AmmoDef +224/+225. The shipped flashbang is the one JO row authoring
       both keys. [orig: AmmoDef_ParseProperty @0x40a2d0] */
    const DefAmmoDef *flashbang = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "grenadefb") == 0) {
            flashbang = &ammo.entries[i];
            break;
        }
    }
    if (!flashbang || flashbang->secondary_anim != 2 || flashbang->kz_physics != 3) {
        fprintf(stderr, "FAIL: grenadefb secondary_anim/kz_physics want 2/3 got %d/%d\n",
                flashbang ? flashbang->secondary_anim : -1,
                flashbang ? flashbang->kz_physics : -1);
        def_free_ammo(&ammo);
        return 1;
    }

    /* The smoke row, pinned against the BASE JO ammo.def this fixture is a
       byte-exact copy of: TrcrID 1875, 30-second fuse, five-second pour
       boundary, 30 units/s throw speed. The revx02 expansion overrides the
       fuse and throw speed to 40 s / 20 units/s; a mounted JO+revx02 stack
       therefore resolves those two fields differently, which is a property of
       the mount order and not of this parser. Pin the file we actually ship
       here -- never edit the retail extract to match an expansion recipe
       (docs/adr/0003-no-raw-passthrough-create-from-scratch.md). */
    const DefAmmoDef *smoke = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "grenadesm") == 0) {
            smoke = &ammo.entries[i];
            break;
        }
    }
    if (!smoke || smoke->max_age_ticks != 30 * 62 ||
        smoke->arm_age_ticks != 5 * 62 || smoke->velocity != 30 ||
        smoke->frndly_trcr_type_id != 1875) {
        fprintf(stderr,
                "FAIL: grenadesm want age/arm/velocity/item=1860/310/30/1875; "
                "got found=%d %d/%d/%d/%d\r\n",
                smoke != NULL,
                smoke ? smoke->max_age_ticks : -1,
                smoke ? smoke->arm_age_ticks : -1,
                smoke ? smoke->velocity : -1,
                smoke ? smoke->frndly_trcr_type_id : -1);
        def_free_ammo(&ammo);
        return 1;
    }
    if (smoke->effects_table_count == 0 ||
        strcmp(smoke->effects_table[0].surface_type, "move") != 0 ||
        strcmp(smoke->effects_table[0].hit_effect, "Effect_SmokeToss") != 0) {
        fprintf(stderr, "FAIL: grenadesm move effect row missing/mismatched\n");
        def_free_ammo(&ammo);
        return 1;
    }

    struct { const char *what; long got, want; } checks[] = {
        {"velocity", car15->velocity, 854},
        {"max_age_ticks", car15->max_age_ticks, 186},
        {"arm_age_ticks", car15->arm_age_ticks, 0},
        {"error_fp16", car15->error_fp16, 0},
        {"drag_fp16", car15->drag_fp16, 16712},
        {"tumble_error_fp16", car15->tumble_error_fp16, 655},
        {"weight_in_grains", car15->weight_in_grains, 62},
        {"penetration_impact", car15->penetration_impact, 20},
        {"kztype", car15->kztype, DEF_AMMO_KZ_C4},
        {"tracer_rate", car15->tracer_rate, 3},
        {"bullet_radius_fp16", car15->bullet_radius_fp16, 182},
        {"max_damage", car15->max_damage, 0},
        /* The presentation fields: `Mf_Light 100` sets flag + value [orig: @0x40a81b/+40];
           `tracer_type stdred stdgreen` = the witnessed 1/2 ids. */
        {"mf_light", car15->mf_light, 1},
        {"mf_light_value", car15->mf_light_value, 100},
        {"tracer_type_friendly", car15->tracer_type_friendly, 1},
        {"tracer_type_enemy", car15->tracer_type_enemy, 2},
        {"recoil_prone", car15->recoil[0], 8},
        {"recoil_crouch", car15->recoil[1], 9},
        {"recoil_standing", car15->recoil[2], 11},
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (checks[i].got != checks[i].want) {
            fprintf(stderr, "FAIL: AMMO_CAR15_556MM %s: got %ld want %ld\n", checks[i].what,
                    checks[i].got, checks[i].want);
            def_free_ammo(&ammo);
            return 1;
        }
    }
    /* The name tokens land case-preserved; `ai_Launcheffect` in the data is matched
       case-insensitively and must NOT be eaten by the `ai_launch` prefix branch. */
    if (strcmp(car15->ai_launch, "GS_M4AI") != 0 ||
        strcmp(car15->ai_launcheffect, "Effect_CAR15MF") != 0) {
        fprintf(stderr, "FAIL: AMMO_CAR15_556MM ai_launch/'effect: got '%s'/'%s'\n",
                car15->ai_launch, car15->ai_launcheffect);
        def_free_ammo(&ammo);
        return 1;
    }

    /* Flag bits via a LAW-style entry (flag LAWR / NoGravity / forcetracer). */
    const DefAmmoDef *law = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (ammo.entries[i].flags & DEF_AMMO_FLAG_LAWR) {
            law = &ammo.entries[i];
            break;
        }
    }
    if (!law) {
        fprintf(stderr, "FAIL: no entry carries the LAWR flag\n");
        def_free_ammo(&ammo);
        return 1;
    }
    if ((law->flags & DEF_AMMO_FLAG_NOGRAVITY) == 0 ||
        (law->flags & DEF_AMMO_FLAG_FORCETRACER) == 0) {
        fprintf(stderr, "FAIL: LAWR entry %s missing NoGravity/forcetracer bits (0x%x)\n",
                law->name, law->flags);
        def_free_ammo(&ammo);
        return 1;
    }

    def_free_ammo(&ammo);
    }  /* retail leg */

    /* The 16.16 decimal keys ride the engine's digit walker (Math_ParseFixedPoint16
       @0x6131f0 — per-digit scale 419430/2^22, a hair UNDER 1/10, accumulator seeded
       127, closing >> 8), NOT a clean round-half-up conversion. The two disagree by
       one LSB on ~4% of decimal forms; pin a divergent form on every migrated key:
       "0.07" -> 4587 where round-half-up says 4588 (the D-WPN-30 closure). A leading
       '-' terminates the walker's integer scan and yields 0, exactly like retail. */
    {
        static const char kWalkerBlock[] =
            "ammo AMMO_WALKER_PIN\r\n"
            "  error 0.07\r\n"
            "  drag 0.07\r\n"
            "  bullet_radius 0.07\r\n"
            "  kz_minradius 0.07\r\n"
            "  kz_maxradius 0.07\r\n"
            "  tumble_error 0.07\r\n"
            "  light_move 0.07 128 120 80\r\n"
            "  max_age 0.07\r\n"
            "end\r\n"
            "ammo AMMO_WALKER_NEG\r\n"
            "  drag -1.5\r\n"
            "end\r\n"
            /* The two forms the retail JO corpus actually shifts on (the
               2026-08-12 sweep: 1038 key values, 8 one-LSB shifts, all these
               two decimals): bullet_radius 0.00277 -> 181 (round-half-up 182),
               drag 0.292 -> 19136 (19137). */
            "ammo AMMO_WALKER_CORPUS\r\n"
            "  bullet_radius 0.00277\r\n"
            "  drag 0.292\r\n"
            "end\r\n";
        DefAmmoFile pin;
        if (def_parse_ammo_memory((const uint8_t *)kWalkerBlock, sizeof(kWalkerBlock) - 1,
                                  &pin) != 0 || pin.count != 3) {
            fprintf(stderr, "FAIL: walker pin block did not parse\n");
            return 1;
        }
        const DefAmmoDef *w = &pin.entries[0];
        struct { const char *what; long got; } walker_checks[] = {
            {"error_fp16", w->error_fp16},
            {"drag_fp16", w->drag_fp16},
            {"bullet_radius_fp16", w->bullet_radius_fp16},
            {"kz_minradius_fp16", w->kz_minradius_fp16},
            {"kz_maxradius_fp16", w->kz_maxradius_fp16},
            {"tumble_error_fp16", w->tumble_error_fp16},
            {"light_move_radius_fp16", w->light_move_radius_fp16},
        };
        for (size_t i = 0; i < sizeof(walker_checks) / sizeof(walker_checks[0]); ++i) {
            if (walker_checks[i].got != 4587) {
                fprintf(stderr, "FAIL: walker pin %s: got %ld want 4587\n",
                        walker_checks[i].what, walker_checks[i].got);
                def_free_ammo(&pin);
                return 1;
            }
        }
        /* 0.07 s -> (62 * 4587 + 0x8000) >> 16 = 4 ticks (round-half-up would land
           the same tick here; the age pin guards the walker feed, not the divide). */
        if (w->max_age_ticks != 4) {
            fprintf(stderr, "FAIL: walker pin max_age_ticks: got %d want 4\n",
                    w->max_age_ticks);
            def_free_ammo(&pin);
            return 1;
        }
        if (pin.entries[1].drag_fp16 != 0) {
            fprintf(stderr, "FAIL: negative drag should walk to 0, got %d\n",
                    pin.entries[1].drag_fp16);
            def_free_ammo(&pin);
            return 1;
        }
        if (pin.entries[2].bullet_radius_fp16 != 181 || pin.entries[2].drag_fp16 != 19136) {
            fprintf(stderr, "FAIL: corpus forms want 181/19136 got %d/%d\n",
                    pin.entries[2].bullet_radius_fp16, pin.entries[2].drag_fp16);
            def_free_ammo(&pin);
            return 1;
        }
        def_free_ammo(&pin);
    }


    {
        static const char text[] = "ammo ARMOR\r\n armor_density 125, 250, -3\r\nend\r\n";
        DefAmmoFile parsed{};
        if (def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(text), sizeof(text)-1,
                                  &parsed) != 0 || parsed.count != 1)
            return 1;
        const auto &row = parsed.entries[0];
        const bool correct = row.armor_density[0] == 125 &&
                row.armor_density[1] == 250 && row.armor_density[2] == -3;
        def_free_ammo(&parsed);
        if (!correct) { fprintf(stderr, "FAIL: armor_density class columns\n"); return 1; }
    }

    {
        // The blast's per-victim presentation names [orig: AmmoDef_ParseProperty
        // @0x40aa15 'secondary_effect' -> +0x48, @0x40a92a 'kz_sound' -> +0x4C].
        static const char text[] =
                "ammo BURN\r\n secondary_effect Effect_Burn\r\n kz_sound EXPLO_BURN\r\nend\r\n";
        DefAmmoFile parsed{};
        if (def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(text), sizeof(text)-1,
                                  &parsed) != 0 || parsed.count != 1)
            return 1;
        const auto &row = parsed.entries[0];
        const bool correct = strcmp(row.secondary_effect, "Effect_Burn") == 0 &&
                strcmp(row.kz_sound, "EXPLO_BURN") == 0;
        def_free_ammo(&parsed);
        if (!correct) { fprintf(stderr, "FAIL: secondary_effect / kz_sound\n"); return 1; }
    }

    {
        // Every line as the retail tokenizer cuts it: the `ammo` name is token 1,
        // a comment never sticks to it, a key is the whole first token (`flags`
        // is not `flag`, `KEY,value` binds), a quoted value is one token, and a
        // file's trailing NUL ends the text, so JO:CA's ammo.def, whose last
        // line is `end` + NUL with no CR LF, closes its last def (GRENADE_NOEXP).
        // An `ammo` line while a def is open ends the whole walk, and the def it
        // interrupted stays in the table [orig: File_ParseASCIIFile @0x53D810 ->
        // Terrain_TokenizeConfigLine @0x53CB60; AmmoDef_ParseProperty @0x40A2D0
        // (`ammo` @0x40A347..0x40A397, the return 1 @0x40A37D; File_ParseASCIIFile
        // @0x53D942)].
        static const char text[] =
                "ammo AMMO_A // the rifle round\r\n"
                " flags shotgun\r\n"
                " flag,silenced\r\n"
                " secondary_effect \"Effect_Burn\"\r\n"
                " tracer_type stdred\r\n"
                " tracer_type\r\n"
                "end\r\n"
                "ammo AMMO_B\r\n"
                " velocity 900\r\n"
                "end";
        std::string source(text, sizeof(text) - 1);
        source.push_back('\0');
        DefAmmoFile parsed{};
        if (def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(source.data()), source.size(),
                                  &parsed) != 0 || parsed.count != 2) {
            fprintf(stderr, "FAIL: tokenizer snippet gave %zu defs, expected 2\n", parsed.count);
            def_free_ammo(&parsed);
            return 1;
        }
        const auto &a = parsed.entries[0];
        const auto &b = parsed.entries[1];
        const bool correct = strcmp(a.name, "AMMO_A") == 0 &&
                a.flags == DEF_AMMO_FLAG_SILENCED &&
                strcmp(a.secondary_effect, "Effect_Burn") == 0 &&
                a.tracer_type_friendly == 0 && a.tracer_type_enemy == 0 &&
                strcmp(b.name, "AMMO_B") == 0 && b.velocity == 900;
        if (!correct)
            fprintf(stderr, "FAIL: tokenizer snippet: name '%s' flags 0x%x effect '%s' tracer %d/%d, "
                    "second '%s' velocity %d\n", a.name, a.flags, a.secondary_effect,
                    a.tracer_type_friendly, a.tracer_type_enemy, b.name, b.velocity);
        def_free_ammo(&parsed);
        if (!correct) return 1;

        static const char interrupted[] =
                "ammo AMMO_A\r\n velocity 1\r\nammo AMMO_B\r\n velocity 2\r\nend\r\nammo AMMO_C\r\nend\r\n";
        DefAmmoFile cut{};
        if (def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(interrupted),
                                  sizeof(interrupted) - 1, &cut) != 0) return 1;
        const bool stopped = cut.count == 1 && strcmp(cut.entries[0].name, "AMMO_A") == 0 &&
                cut.entries[0].velocity == 1;
        if (!stopped)
            fprintf(stderr, "FAIL: an `ammo` line inside a def must end the walk (%zu defs)\n",
                    cut.count);
        def_free_ammo(&cut);
        if (!stopped) return 1;
    }

    {
        // A def opens on the allocator's defaults, not zeros: drag 1.0 (0x10000),
        // the kill-zone pie slice 0x7FFFFFFF (no cone: AV_Mine and the land mines
        // author none), recoil 24 per stance, velocity / max_age / both turn
        // rates / the boresight -1; at its `end` a velocity or max_age still -1
        // takes def 0's (the table's first def is the template and keeps its own),
        // and a def the walk stops in never reaches that `end`.
        // [orig: AmmoDef_AllocateSlot @0x409A20 (the defaults @0x409A56..0x409AF6);
        //  AmmoDef_InheritDefaults @0x409EB0, called from the `end` arm @0x40A3E5]
        static const char text[] =
                "ammo AT_NULL\r\n velocity 300\r\n max_age 5\r\nend\r\n"
                "ammo BARE\r\nend\r\n"
                "ammo OWN\r\n velocity 900\r\n max_age 2\r\n drag 0.5\r\n kz_pieslice 24\r\n"
                " recoil 1 2 3\r\nend\r\n"
                "ammo CUT\r\nammo NEVER\r\nend\r\n";
        DefAmmoFile parsed{};
        if (def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(text), sizeof(text) - 1,
                                  &parsed) != 0 || parsed.count != 4) {
            fprintf(stderr, "FAIL: defaults snippet gave %zu defs, expected 4\n", parsed.count);
            def_free_ammo(&parsed);
            return 1;
        }
        const auto &bare = parsed.entries[1];
        const auto &own = parsed.entries[2];
        const auto &cut = parsed.entries[3];
        const bool correct =
                bare.velocity == 300 && bare.max_age_ticks == 310 && bare.drag_fp16 == 0x10000 &&
                bare.kz_pieslice_bam == 0x7FFFFFFF && bare.recoil[0] == 24 &&
                bare.recoil[1] == 24 && bare.recoil[2] == 24 && bare.turnrate_maxpit == -1 &&
                bare.turnrate_maxyaw == -1 && bare.boresight_maxang == -1 &&
                own.velocity == 900 && own.max_age_ticks == 124 && own.drag_fp16 == 32768 &&
                own.kz_pieslice_bam == 12 * 11930464 && own.recoil[2] == 3 &&
                strcmp(cut.name, "CUT") == 0 && cut.velocity == -1 && cut.max_age_ticks == -1;
        if (!correct)
            fprintf(stderr, "FAIL: allocator defaults: bare vel %d age %d drag %d slice %d recoil "
                    "%d/%d/%d turn %d/%d bore %d; cut vel %d age %d\n", bare.velocity,
                    bare.max_age_ticks, bare.drag_fp16, bare.kz_pieslice_bam, bare.recoil[0],
                    bare.recoil[1], bare.recoil[2], bare.turnrate_maxpit, bare.turnrate_maxyaw,
                    bare.boresight_maxang, cut.velocity, cut.max_age_ticks);
        def_free_ammo(&parsed);
        if (!correct) return 1;
    }

    {
        // A table's rows reach the def at the table's `end`, and only while the
        // def has none yet: a second table gives nothing (not even a tag the
        // first lacks), and a table the file never closes gives nothing.
        // [orig: AmmoDef_InitEffectsTable @0x409F20, called only @0x40A433,
        //  installs when def+0x68 and word +0x6C are both 0]
        static const char text[] =
                "ammo TWO_TABLES\r\n"
                " effects_table\r\n"
                "  dirt Effect_A IMP_A 20\r\n"
                " end\r\n"
                " effects_table\r\n"
                "  dirt Effect_B IMP_B 20\r\n"
                "  snow Effect_B IMP_B 20\r\n"
                " end\r\n"
                "end\r\n"
                "ammo OPEN_TABLE\r\n"
                " effects_table\r\n"
                "  dirt Effect_C IMP_C 20\r\n";
        DefAmmoFile parsed{};
        const int rc = def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(text),
                                             sizeof(text) - 1, &parsed);
        const bool correct = rc == 0 && parsed.count == 2 &&
                parsed.entries[0].effects_table_count == 1 &&
                strcmp(parsed.entries[0].effects_table[0].hit_effect, "Effect_A") == 0 &&
                parsed.entries[1].effects_table_count == 0;
        if (!correct)
            fprintf(stderr, "FAIL: effects tables: %zu defs, rows %zu / %zu\n", parsed.count,
                    parsed.count > 0 ? parsed.entries[0].effects_table_count : 0,
                    parsed.count > 1 ? parsed.entries[1].effects_table_count : 0);
        def_free_ammo(&parsed);
        if (!correct) return 1;
    }

    {
        // Lines split at CR LF and nowhere else: an LF alone is a byte of the
        // line (no separator either, so `900\n` is one token and the max_age
        // after it is no key), and a last line with no CR LF loses its final
        // byte, so a closing `end` there reads `en` and closes nothing (TAIL
        // never takes def 0's max_age). [orig: File_ParseASCIIFile @0x53D810,
        //  the CR LF test @0x53D8C7..0x53D8F5, the tail @0x53D8E9 / @0x53D8EC]
        static const char text[] =
                "ammo AT_NULL\r\n max_age 5\r\nend\r\n"
                "ammo LF\r\n velocity 900\n max_age 3\r\nend\r\n"
                "ammo TAIL\r\nend";
        DefAmmoFile parsed{};
        const int rc = def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(text),
                                             sizeof(text) - 1, &parsed);
        const bool correct = rc == 0 && parsed.count == 3 &&
                parsed.entries[1].velocity == 900 && parsed.entries[1].max_age_ticks == 310 &&
                strcmp(parsed.entries[2].name, "TAIL") == 0 &&
                parsed.entries[2].max_age_ticks == -1;
        if (!correct)
            fprintf(stderr, "FAIL: line split: %zu defs, LF vel %d age %d, TAIL age %d\n",
                    parsed.count, parsed.count > 1 ? parsed.entries[1].velocity : 0,
                    parsed.count > 1 ? parsed.entries[1].max_age_ticks : 0,
                    parsed.count > 2 ? parsed.entries[2].max_age_ticks : 0);
        def_free_ammo(&parsed);
        if (!correct) return 1;
    }

    if (!have_retail)
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/def/ammo.def (the shipped ammo table)");
    printf("PASS: ammo parsing OK\n");
    return 0;
}
