// Test parsing items.def — spot-check "dbuggy1" and "Player #1" entries, plus
// the per-item particle-effect keys over an inline snippet.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/def/def.h>
#include "common/test_paths.h"
#include "common/retail_paths.h"

#include <ctype.h>
#include <string>

using namespace opennova::def;

static int expect_str(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s mismatch: expected '%s', got '%s'\n", what, want, got);
        return 1;
    }
    return 0;
}

/* Building interior daylight transfer: atoi, clamp 0..100, then x0.01 into
   ItemDef+0x218. Ihq01 authors 20, so its interior lerp is exactly 0.2.
   [orig: ItemDef_ParseProperty @0x4a19fd..0x4a1a50] */
static int test_light_transfer(void) {
    static const char snippet[] =
        "begin \"Absent\"\n"
        "  id 1\n"
        "end\n"
        "begin \"Ihq01\"\n"
        "  id 101216\n"
        "  light_transfer 20\n"
        "end\n"
        "begin \"Clamped Low\"\n"
        "  id 3\n"
        "  LIGHT_TRANSFER -7\n"
        "end\n"
        "begin \"Clamped High\"\n"
        "  id 4\n"
        "  light_transfer 107\n"
        "end\n";
    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)snippet, sizeof(snippet) - 1,
                               &items) != 0 ||
        items.count != 4) {
        fprintf(stderr, "FAIL: light_transfer snippet did not parse\n");
        def_free_items(&items);
        return 1;
    }
    int fails = 0;
    if (items.entries[0].light_transfer != 0.0f ||
        items.entries[1].light_transfer < 0.1999f ||
        items.entries[1].light_transfer > 0.2001f ||
        items.entries[2].light_transfer != 0.0f ||
        items.entries[3].light_transfer != 1.0f) {
        fprintf(stderr, "FAIL: light_transfer clamp/scale semantics mismatch\n");
        ++fails;
    }
    if (items.entries[1].raw_lines_count != 0 ||
        items.entries[2].raw_lines_count != 0 ||
        items.entries[3].raw_lines_count != 0) {
        fprintf(stderr, "FAIL: light_transfer fell through to raw_lines\n");
        ++fails;
    }
    def_free_items(&items);
    return fails;
}

/* Per-item particle-effect keys [orig: ItemDef_ParseProperty @ 0x49eb00,
   particlefx chain @ 0x4a13ad..0x4a179d]: anchored slots take
   <effect> <userpoint> (particlefxs/particlefxw1/particlefxw2 read an optional
   third <secondary_effect> token; particlefx/particlefxw3/particlefxw4 never
   do), the death/h2odeath/fire/other/spawn/finale keys take the effect name
   only, keys match case-insensitively, and extra tokens are ignored. The
   snippet mirrors the retail "Drivable Dune Buggy" rows (JOX ITEMS.DEF). */
static int test_particle_keys(void) {
    static const char snippet[] =
        "begin \"Drivable Dune Buggy\"\n"
        "  id 101291\n"
        "  sid dbuggy1\n"
        "  type vehicle\n"
        /* Leading tab + multi-space separator + trailing spaces, exactly as the
           retail rows are formatted. */
        "\tparticlefx   Effect_whiteExhaust FX00 stray_token\n"
        "  ParticleFXW1 Effect_W1 FX03 Effect_W1S\n"
        "\tparticlefxw2 Effect_DirtWake FX01  \n"
        "  particlefxw3 Effect_W3 FX02 Effect_IgnoredW3\n"
        "  particlefxw4 Effect_W4 FX04\n"
        "  particlefxs Effect_DirtWakeS FX01 Effect_DirtWakeS2\n"
        "  particledeath Effect_Fuelxp3\n"
        "  particleh2odeath Effect_H2OVeExp\n"
        "  particlefire Effect_VehFire extra junk\n"
        "  particleother Effect_SmkNStemNP\n"
        "  particlespawn Effect_Spawn\n"
        "  particlefinale Effect_VehDrtPuftrk\n"
        "end\n"
        "begin \"No Particles\"\n"
        "  id 5\n"
        "  sid plain1\n"
        "  type object\n"
        "end\n"
        "begin \"FXS Two Arg\"\n"
        "  id 6\n"
        "  sid fxs2\n"
        "  particlefxs Effect_OnlyTwo FX07\n"
        "end\n";

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)snippet, sizeof(snippet) - 1, &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items_memory failed for particle snippet\n");
        return 1;
    }
    int fails = 0;
    if (items.count != 3) {
        fprintf(stderr, "FAIL: particle snippet: expected 3 entries, got %zu\n", items.count);
        def_free_items(&items);
        return 1;
    }

    const DefItemDef *b = &items.entries[0];
    if (b->id != 101291) {
        fprintf(stderr, "FAIL: particle snippet buggy id mismatch: got %d\n", b->id);
        ++fails;
    }
    /* Every line above is a recognized key — none may fall through to raw_lines. */
    if (b->raw_lines_count != 0) {
        fprintf(stderr, "FAIL: particle keys fell through to raw_lines (%zu)\n",
                b->raw_lines_count);
        ++fails;
    }

    /* Slot A two-arg; the third token is IGNORED (particlefx has no secondary). */
    fails += expect_str("particlefx.effect", b->particlefx.effect, "Effect_whiteExhaust");
    fails += expect_str("particlefx.userpoint", b->particlefx.userpoint, "FX00");
    fails += expect_str("particlefx.secondary_effect", b->particlefx.secondary_effect, "");

    /* fxw1 with the optional secondary, matched case-insensitively. */
    fails += expect_str("particlefxw1.effect", b->particlefxw1.effect, "Effect_W1");
    fails += expect_str("particlefxw1.userpoint", b->particlefxw1.userpoint, "FX03");
    fails += expect_str("particlefxw1.secondary_effect", b->particlefxw1.secondary_effect,
                        "Effect_W1S");

    /* fxw2 without the optional secondary. */
    fails += expect_str("particlefxw2.effect", b->particlefxw2.effect, "Effect_DirtWake");
    fails += expect_str("particlefxw2.userpoint", b->particlefxw2.userpoint, "FX01");
    fails += expect_str("particlefxw2.secondary_effect", b->particlefxw2.secondary_effect, "");

    /* fxw3/fxw4 never read a third token [orig: @ 0x4a158b / @ 0x4a15eb]. */
    fails += expect_str("particlefxw3.effect", b->particlefxw3.effect, "Effect_W3");
    fails += expect_str("particlefxw3.userpoint", b->particlefxw3.userpoint, "FX02");
    fails += expect_str("particlefxw3.secondary_effect", b->particlefxw3.secondary_effect, "");
    fails += expect_str("particlefxw4.effect", b->particlefxw4.effect, "Effect_W4");
    fails += expect_str("particlefxw4.userpoint", b->particlefxw4.userpoint, "FX04");
    fails += expect_str("particlefxw4.secondary_effect", b->particlefxw4.secondary_effect, "");

    /* fxs with the third token consumed as the secondary effect. */
    fails += expect_str("particlefxs.effect", b->particlefxs.effect, "Effect_DirtWakeS");
    fails += expect_str("particlefxs.userpoint", b->particlefxs.userpoint, "FX01");
    fails += expect_str("particlefxs.secondary_effect", b->particlefxs.secondary_effect,
                        "Effect_DirtWakeS2");

    /* Effect-only keys; extra tokens after the effect name are ignored. */
    fails += expect_str("particledeath", b->particledeath, "Effect_Fuelxp3");
    fails += expect_str("particleh2odeath", b->particleh2odeath, "Effect_H2OVeExp");
    fails += expect_str("particlefire", b->particlefire, "Effect_VehFire");
    fails += expect_str("particleother", b->particleother, "Effect_SmkNStemNP");
    fails += expect_str("particlespawn", b->particlespawn, "Effect_Spawn");
    fails += expect_str("particlefinale", b->particlefinale, "Effect_VehDrtPuftrk");

    /* An item without any particle keys leaves every field empty. */
    const DefItemDef *plain = &items.entries[1];
    fails += expect_str("plain particlefx.effect", plain->particlefx.effect, "");
    fails += expect_str("plain particlefx.userpoint", plain->particlefx.userpoint, "");
    fails += expect_str("plain particlefxs.effect", plain->particlefxs.effect, "");
    fails += expect_str("plain particlefxs.secondary_effect",
                        plain->particlefxs.secondary_effect, "");
    fails += expect_str("plain particlefxw1.effect", plain->particlefxw1.effect, "");
    fails += expect_str("plain particlefxw2.effect", plain->particlefxw2.effect, "");
    fails += expect_str("plain particlefxw3.effect", plain->particlefxw3.effect, "");
    fails += expect_str("plain particlefxw4.effect", plain->particlefxw4.effect, "");
    fails += expect_str("plain particledeath", plain->particledeath, "");
    fails += expect_str("plain particleh2odeath", plain->particleh2odeath, "");
    fails += expect_str("plain particlefire", plain->particlefire, "");
    fails += expect_str("plain particleother", plain->particleother, "");
    fails += expect_str("plain particlespawn", plain->particlespawn, "");
    fails += expect_str("plain particlefinale", plain->particlefinale, "");

    /* fxs with only two args: the secondary stays empty. */
    const DefItemDef *fxs2 = &items.entries[2];
    fails += expect_str("fxs2 particlefxs.effect", fxs2->particlefxs.effect, "Effect_OnlyTwo");
    fails += expect_str("fxs2 particlefxs.userpoint", fxs2->particlefxs.userpoint, "FX07");
    fails += expect_str("fxs2 particlefxs.secondary_effect",
                        fxs2->particlefxs.secondary_effect, "");

    def_free_items(&items);
    if (fails != 0) {
        fprintf(stderr, "FAIL: %d particle-key check(s) failed\n", fails);
        return 1;
    }
    return 0;
}

/* The mounted-gunner selector reads the target item definition's authored
   phrase_set dword at +0x86c. Zero is a real value, so the portable definition
   record must carry presence separately from the parsed integer.
   [orig: ItemDef_ParseProperty @ 0x49f9db..0x49fa0a] */
static int test_phrase_set_presence(void) {
    static const char snippet[] =
        "begin \"Absent\"\n"
        "  id 1\n"
        "end\n"
        "begin \"Explicit Zero\"\n"
        "  id 2\n"
        "  phrase_set 0\n"
        "end\n"
        "begin \"Signed Value\"\n"
        "  id 3\n"
        "  PHRASE_SET -2\n"
        "end\n";
    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)snippet, sizeof(snippet) - 1,
                               &items) != 0 ||
        items.count != 3) {
        fprintf(stderr, "FAIL: phrase_set snippet did not parse\n");
        def_free_items(&items);
        return 1;
    }
    int fails = 0;
    if (items.entries[0].phrase_set_valid != 0) {
        fprintf(stderr, "FAIL: absent phrase_set became valid\n");
        ++fails;
    }
    if (items.entries[1].phrase_set_valid != 1 || items.entries[1].phrase_set != 0) {
        fprintf(stderr, "FAIL: explicit phrase_set 0 lost validity/value\n");
        ++fails;
    }
    if (items.entries[2].phrase_set_valid != 1 || items.entries[2].phrase_set != -2) {
        fprintf(stderr, "FAIL: phrase_set must preserve signed atol semantics\n");
        ++fails;
    }
    def_free_items(&items);
    return fails;
}

/* weathervane / minai / default_aip: three keys retail parses that we dropped
   until 2026-08-22. The values are the Flyable Blackhawk's own (items.def id
   102010), so a regression here is a regression against shipped data.
   default_aip additionally RAISES the AIData attrib bit -- retail ORs 0x100000
   in the same parse arm, so the first entry below carries AIData without ever
   listing it on an attrib line.
   [orig: ItemDef_ParsePhysicsProperty @0x49d870 weathervane/minai;
    ItemDef_ParseProperty @0x49eb00 default_aip + `attrib |= 0x100000`] */
static int test_weathervane_minai_default_aip(void) {
    static const char snippet[] =
        "begin \"Flyable Blackhawk\"\n"
        "  id 102010\n"
        "  type vehicle\n"
        "  move_function chel\n"
        "    weathervane 30\n"
        "    minai\t\t1\n"
        "    hand_brake 0\n"
        "    tire_slip 7\n"
        "  default_aip H_BHawk\n"
        "  sound_profile SP_Blackhawk1\n"
        "end\n"
        "begin \"No AI Keys\"\n"
        "  id 2\n"
        "end\n";
    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)snippet,
                               sizeof(snippet) - 1, &items) != 0 ||
        items.count != 2) {
        fprintf(stderr, "FAIL: weathervane/minai snippet parse\n");
        return 1;
    }
    int fails = 0;
    if (items.entries[0].weathervane != 30) {
        fprintf(stderr, "FAIL: weathervane %d != 30\n", items.entries[0].weathervane);
        ++fails;
    }
    if (items.entries[0].min_ai != 1) {
        fprintf(stderr, "FAIL: min_ai %d != 1\n", items.entries[0].min_ai);
        ++fails;
    }
    /* hand_brake / tire_slip: authored values land raw; an entry that lists
       neither runs on the allocator's 1 / 5 [orig: ItemDef_AllocateWithDefaults
       @0x49E3B0; keys @0x7c7d60 / @0x7c7d6c]. */
    if (items.entries[0].hand_brake != 0 || items.entries[0].tire_slip != 7) {
        fprintf(stderr, "FAIL: hand_brake %d / tire_slip %d != 0 / 7\n",
                items.entries[0].hand_brake, items.entries[0].tire_slip);
        ++fails;
    }
    if (items.entries[1].hand_brake != 1 || items.entries[1].tire_slip != 5) {
        fprintf(stderr, "FAIL: hand_brake/tire_slip defaults %d / %d != 1 / 5\n",
                items.entries[1].hand_brake, items.entries[1].tire_slip);
        ++fails;
    }
    fails += expect_str("default_aip", items.entries[0].default_aip, "H_BHawk");
    if ((items.entries[0].attrib & DEF_ITEM_ATTRIB_AIDATA) == 0) {
        fprintf(stderr, "FAIL: default_aip did not raise AIData\n");
        ++fails;
    }
    fails += expect_str("sound_profile", items.entries[0].sound_profile,
                        "SP_Blackhawk1");
    if (items.entries[1].weathervane != 0 || items.entries[1].min_ai != 0 ||
        items.entries[1].default_aip[0] != '\0' ||
        (items.entries[1].attrib & DEF_ITEM_ATTRIB_AIDATA) != 0) {
        fprintf(stderr, "FAIL: keys leaked into the next entry\n");
        ++fails;
    }
    if (items.entries[0].raw_lines_count != 0) {
        fprintf(stderr, "FAIL: a parsed key fell through to raw_lines\n");
        ++fails;
    }
    def_free_items(&items);
    return fails;
}

/* Retail stamps a physics-default block on every items.def `begin` BEFORE any
   key is parsed, so an item that declares none of these keys runs on THESE
   values, not on zero. The load-bearing one is spring_comp: DTruck1 declares
   none, so retail gives it 20 -> suspension travel 13108, where a zeroed
   record gives travel 0 and freezes every wheel oscillator for the whole run
   (AI-PARITY-CONCEPT 6.15h).
   [orig: ItemDef_AllocateWithDefaults @0x0049E3B0; the `begin` arm calls it
    at ItemDef_ParseProperty @0x0049EB00] */
static int test_item_def_allocator_defaults(void) {
    static const char snippet[] =
        "begin \"Declares Nothing\"\n"
        "  id 900001\n"
        "end\n"
        "begin \"Overrides Some\"\n"
        "  id 900002\n"
        "  spring_comp 55\n"
        "  mass 11\n"
        "  flip 10\n"
        "end\n";
    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)snippet, sizeof(snippet) - 1,
                               &items) != 0 || items.count != 2) {
        fprintf(stderr, "FAIL: allocator-defaults snippet did not parse\n");
        return 1;
    }
    const DefItemDef *bare = &items.entries[0];
    const DefItemDef *over = &items.entries[1];
    int bad = 0;
    /* The undeclared record carries retail defaults. */
    if (bare->spring_comp != 20) { fprintf(stderr, "FAIL: default spring_comp %d != 20\n", bare->spring_comp); bad = 1; }
    if (bare->climb_speed != 1) { fprintf(stderr, "FAIL: default climb_speed %d != 1\n", bare->climb_speed); bad = 1; }
    if (bare->torque != 3) { fprintf(stderr, "FAIL: default torque %d != 3\n", bare->torque); bad = 1; }
    if (bare->mass != 5) { fprintf(stderr, "FAIL: default mass %d != 5\n", bare->mass); bad = 1; }
    if (bare->shock != 4) { fprintf(stderr, "FAIL: default shock %d != 4\n", bare->shock); bad = 1; }
    if (bare->lean != 5 || bare->lean_velocity != 5) { fprintf(stderr, "FAIL: default lean pair\n"); bad = 1; }
    if (bare->pitch != 1 || bare->pitch_velocity != 5) { fprintf(stderr, "FAIL: default pitch pair\n"); bad = 1; }
    if (bare->flip != 45) { fprintf(stderr, "FAIL: default flip %d != 45\n", bare->flip); bad = 1; }
    /* Retail sets spring/top_heavy to 0 explicitly - same observable as a
       zeroed record, asserted so a later change cannot drift them silently. */
    if (bare->spring != 0 || bare->top_heavy != 0) { fprintf(stderr, "FAIL: spring/top_heavy default\n"); bad = 1; }
    /* An authored key still wins over the default... */
    if (over->spring_comp != 55) { fprintf(stderr, "FAIL: authored spring_comp %d != 55\n", over->spring_comp); bad = 1; }
    if (over->mass != 11) { fprintf(stderr, "FAIL: authored mass %d != 11\n", over->mass); bad = 1; }
    if (over->flip != 10) { fprintf(stderr, "FAIL: authored flip %d != 10\n", over->flip); bad = 1; }
    /* ...while its UNdeclared keys still carry the defaults (per-block reset). */
    if (over->torque != 3 || over->shock != 4) { fprintf(stderr, "FAIL: per-block default reset\n"); bad = 1; }
    def_free_items(&items);
    return bad;
}
/* The per-item particle-effect slots on the retail items.def (SKIP-LEG without
   OPENNOVA_JO_ASSETS): the shipped table authors particlefx rows, and DBuggy1
   (101291) anchors Effect_whiteExhaust at the model's FX00 user point — the row
   the effect-attach pass consumes. [orig: ItemDef_ParseProperty @ 0x49eb00] */
static int test_retail_particlefx_rows(void) {
    const std::string path = retail::asset_file("items.def");
    if (path.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS carrying items.def");
    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items(path.c_str(), &items) != 0) {
        fprintf(stderr, "FAIL: retail items.def did not parse: %s\n", path.c_str());
        return 1;
    }
    size_t with_fx = 0;
    const DefItemDef *buggy = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].particlefx.effect[0] != '\0') ++with_fx;
        if (items.entries[i].id == 101291) buggy = &items.entries[i];
    }
    printf("retail items.def: %zu items, %zu author particlefx\n", items.count, with_fx);
    int rc = 0;
    if (with_fx == 0) {
        fprintf(stderr, "FAIL: no retail item authors particlefx\n");
        rc = 1;
    }
    if (buggy == NULL) {
        fprintf(stderr, "FAIL: DBuggy1 (101291) is not in the retail items.def\n");
        rc = 1;
    } else {
        printf("retail items.def: DBuggy1 particlefx %s @ %s\n", buggy->particlefx.effect,
               buggy->particlefx.userpoint);
        std::string fx(buggy->particlefx.effect);
        for (size_t i = 0; i < fx.size(); ++i) fx[i] = (char)tolower((unsigned char)fx[i]);
        if (fx != "effect_whiteexhaust") {
            fprintf(stderr, "FAIL: DBuggy1 particlefx effect '%s', expected Effect_whiteExhaust\n",
                    buggy->particlefx.effect);
            rc = 1;
        }
        std::string up(buggy->particlefx.userpoint);
        for (size_t i = 0; i < up.size(); ++i) up[i] = (char)toupper((unsigned char)up[i]);
        if (up != "FX00") {
            fprintf(stderr, "FAIL: DBuggy1 particlefx user point '%s', expected FX00\n",
                    buggy->particlefx.userpoint);
            rc = 1;
        }
    }
    def_free_items(&items);
    return rc;
}

static int test_vehicle_spawn_lists() {
	std::string source = "begin First\n id 1\n pcvehicle_spawnlist 100042 100043 100042\nend\n"
						 "begin Second\n id 2\n PCVEHICLE_SPAWNLIST 100043\nend\n"
						 "begin Capacity\n id 3\n pcvehicle_spawnlist";
	for (int id = 0; id < 40; ++id)
		source += " " + std::to_string(200000 + id);
	source += "\nend\nbegin Replace\n id 4\n pcvehicle_spawnlist 100042\n"
			  " pcvehicle_spawnlist 100043\nend\n";
	DefItemsFile file{};
	if (def_parse_items_memory(
				reinterpret_cast<const uint8_t *>(source.data()), source.size(), &file) != 0 ||
			file.count != 4)
		return 1;
	const bool ok = file.vehicle_spawn_id_count == 32 && file.vehicle_spawn_ids[0] == 100042 &&
			file.vehicle_spawn_ids[1] == 100043 && file.entries[0].vehicle_spawn_mask == 3 &&
			file.entries[1].vehicle_spawn_mask == 2 &&
			file.entries[2].vehicle_spawn_mask == 0xFFFFFFFCu &&
			file.entries[3].vehicle_spawn_mask == 2;
	def_free_items(&file);
	if (!ok)
		fprintf(stderr, "FAIL: vehicle spawn lists/cap/replacement\n");
	return ok ? 0 : 1;
}

static int test_regional_sound_delays() {
    const char text[] =
        "begin Env\n id 1\n dawnshot Bird 1.25 -0.125\n dayshot Day 0.016129032258 2\n"
        " duskshot Evening 3.9 4.1\n nightshot Night 0 5.5\n end\n"
        "begin Emit\n id 2\n dawnshot Bird 9 8\n particletesttime 0.5 0.25\n"
        " destroy_timing 1.5 0.016129032258 -0.25\n end\n";
    DefItemsFile items{};
    if (def_parse_items_memory(reinterpret_cast<const unsigned char *>(text),
            sizeof(text) - 1, &items) != 0 || items.count != 2) return 1;
    const auto &first = items.entries[0];
    const auto &second = items.entries[1];
    const bool ok = first.shot_delay_ticks[0][0] == 77 && first.shot_delay_ticks[0][1] == -7 &&
            first.shot_delay_ticks[1][0] == 0 && first.shot_delay_ticks[1][1] == 124 &&
            first.shot_delay_ticks[2][0] == 241 && first.shot_delay_ticks[2][1] == 254 &&
            first.shot_delay_ticks[3][0] == 0 && first.shot_delay_ticks[3][1] == 341 &&
            second.shot_delay_ticks[0][0] == 31 && second.shot_delay_ticks[0][1] == 15 &&
            strcmp(second.dawnshot, "Bird") == 0 &&
            second.destroy_timing_ticks[0] == 93 && second.destroy_timing_ticks[1] == 0 &&
            second.destroy_timing_ticks[2] == -15;
    if (!ok) fprintf(stderr, "FAIL regional sound delay parser\n");
    def_free_items(&items);
    return ok ? 0 : 1;
}

int main(void) {
    if (test_regional_sound_delays() != 0) return 1;
	if (test_vehicle_spawn_lists() != 0)
		return 1;
	if (test_item_def_allocator_defaults() != 0) {
		return 1;
	}
	if (test_light_transfer() != 0) {
        return 1;
    }
    if (test_weathervane_minai_default_aip() != 0) {
        return 1;
    }
    const char *repo_root = test_paths_repo_root(__FILE__);
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/def/items.def", repo_root);

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items(path, &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items failed for %s\n", path);
        return 1;
    }

    if (items.count < 10) {
        fprintf(stderr, "FAIL: too few entries: %zu\n", items.count);
        def_free_items(&items);
        return 1;
    }

    printf("Parsed %zu items\n", items.count);

    /* The smoke grenade's TrcrID is a real throwable item, not an effect-only
       placeholder. The mounted JO+revx02 data resolves id 101875 to the
       Flsh_3rd model with nade/nade callbacks; dropping any of those fields
       makes the loose-fixture LAN path either invisible or fall back to the
       stock ballistic motor. */
    const DefItemDef *smoke_grenade = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].id == 101875) {
            smoke_grenade = &items.entries[i];
            break;
        }
    }
    if (!smoke_grenade ||
        strcmp(smoke_grenade->graphic, "Flsh_3rd") != 0 ||
        strcmp(smoke_grenade->ai_function, "nade") != 0 ||
        strcmp(smoke_grenade->move_function, "nade") != 0) {
        fprintf(stderr,
                "FAIL: smoke grenade item want id=101875 graphic=Flsh_3rd "
                "ai/move=nade; got found=%d graphic='%s' ai='%s' move='%s'\n",
                smoke_grenade != NULL,
                smoke_grenade ? smoke_grenade->graphic : "",
                smoke_grenade ? smoke_grenade->ai_function : "",
                smoke_grenade ? smoke_grenade->move_function : "");
        def_free_items(&items);
        return 1;
    }

    /* Find the Dune Buggy entry by sid */
    const DefItemDef *buggy = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (strcmp(items.entries[i].sid, "dbuggy1") == 0) {
            buggy = &items.entries[i];
            break;
        }
    }

    if (!buggy) {
        fprintf(stderr, "FAIL: could not find dbuggy1 entry\n");
        def_free_items(&items);
        return 1;
    }

    if (strcmp(buggy->display_name, "Drivable Dune Buggy") != 0) {
        fprintf(stderr, "FAIL: buggy display_name mismatch: '%s'\n", buggy->display_name);
        def_free_items(&items);
        return 1;
    }

    /* vehicle = 1, the witnessed engine value [orig: ItemDef_ParseProperty
       @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1] */
    if (buggy->type != DEF_ITEM_TYPE_VEHICLE) {
        fprintf(stderr, "FAIL: buggy type mismatch: expected 1 (vehicle), got %d\n", buggy->type);
        def_free_items(&items);
        return 1;
    }

    if (strcmp(buggy->graphic, "Dbuggy1") != 0) {
        fprintf(stderr, "FAIL: buggy graphic mismatch: '%s'\n", buggy->graphic);
        def_free_items(&items);
        return 1;
    }

    if (strcmp(buggy->husk, "Dbuggy1X") != 0) {
        fprintf(stderr, "FAIL: buggy husk mismatch: '%s'\n", buggy->husk);
        def_free_items(&items);
        return 1;
    }

    if (buggy->hp != 3000) {
        fprintf(stderr, "FAIL: buggy hp mismatch: expected 3000, got %d\n", buggy->hp);
        def_free_items(&items);
        return 1;
    }

    if (buggy->id != 101291) {
        fprintf(stderr, "FAIL: buggy id mismatch: expected 101291, got %d\n", buggy->id);
        def_free_items(&items);
        return 1;
    }

    /* Shipped JOX data marks the drivable buggy PlayerControl and anchors its
       running exhaust at FX00. Keep the fixture chain intact so runtime tests
       exercise the same startup gate as retail data. */
    if ((buggy->attrib & 0x40u) == 0) {
        fprintf(stderr, "FAIL: buggy attrib missing PlayerControl (0x40): got 0x%x\n",
                buggy->attrib);
        def_free_items(&items);
        return 1;
    }
    if (expect_str("buggy particlefx.effect", buggy->particlefx.effect,
                   "Effect_whiteExhaust") != 0 ||
        expect_str("buggy particlefx.userpoint", buggy->particlefx.userpoint, "FX00") != 0) {
        def_free_items(&items);
        return 1;
    }

    /* §5.10b class-tag directives. The buggy uses ai_function chel for its
       event/brain callback, while render_function/move_function = cveh. The
       distinct move tag keeps its per-frame physics on the retail ground mover.
       No disk_function on the buggy. */
    if (strcmp(buggy->ai_function, "chel") != 0) {
        fprintf(stderr, "FAIL: buggy ai_function mismatch: expected 'chel', got '%s'\n",
                buggy->ai_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(buggy->move_function, "cveh") != 0) {
        fprintf(stderr, "FAIL: buggy move_function mismatch: expected 'cveh', got '%s'\n",
                buggy->move_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(buggy->render_function, "cveh") != 0) {
        fprintf(stderr, "FAIL: buggy render_function mismatch: expected 'cveh', got '%s'\n",
                buggy->render_function);
        def_free_items(&items);
        return 1;
    }
    if (buggy->disk_function[0] != '\0') {
        fprintf(stderr, "FAIL: buggy disk_function should be empty, got '%s'\n",
                buggy->disk_function);
        def_free_items(&items);
        return 1;
    }

    /* The destruction/husk block on the Barrel (world-wac-ai-re §24) */
    const DefItemDef *barrel = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].id == 105002) {
            barrel = &items.entries[i];
            break;
        }
    }
    if (!barrel) {
        fprintf(stderr, "FAIL: could not find Barrel (id 105002)\n");
        def_free_items(&items);
        return 1;
    }
    if (strcmp(barrel->huskfinal, "Barrel1XF") != 0) {
        fprintf(stderr, "FAIL: barrel huskfinal mismatch: '%s'\n", barrel->huskfinal);
        def_free_items(&items);
        return 1;
    }
    /* 'armor 12 4': blast word (+0x192) = 12, impact word (+0x190) = the second
       value 4 [orig: the parse order @ 0x4a00e7-0x4a0147] */
    if (barrel->armor_blast != 12 || barrel->armor_impact != 4) {
        fprintf(stderr, "FAIL: barrel armor mismatch: blast %d impact %d\n",
                barrel->armor_blast, barrel->armor_impact);
        def_free_items(&items);
        return 1;
    }
    if (barrel->kz < 3.99f || barrel->kz > 4.01f) {
        fprintf(stderr, "FAIL: barrel kz mismatch: %f\n", (double)barrel->kz);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(barrel->sounddeath, "EXPLO_BARREL") != 0) {
        fprintf(stderr, "FAIL: barrel sounddeath mismatch: '%s'\n", barrel->sounddeath);
        def_free_items(&items);
        return 1;
    }
    if (barrel->unit_type != 6 || barrel->husk_sub_parts != 4) {
        fprintf(stderr, "FAIL: barrel unit_type/husk_sub_parts mismatch: %d/%d\n",
                barrel->unit_type, barrel->husk_sub_parts);
        def_free_items(&items);
        return 1;
    }
    /* '02_CHUNK_M 03_CHUNK_S 04_wheel' -> slots 1..3 = CHUNK_M(3), CHUNK_S(2),
       WHEEL(1) case-insensitively; slot 0 stays 0 = HULL (the zero-init read)
       [orig: the strstr '_' split @ 0x49f33d + DeathPieceType_FindByName @ 0x57b310] */
    if (barrel->husk_sub_part_types[0] != 0 || barrel->husk_sub_part_types[1] != 3 ||
        barrel->husk_sub_part_types[2] != 2 || barrel->husk_sub_part_types[3] != 1) {
        fprintf(stderr, "FAIL: barrel husk_sub_part_types mismatch: %d %d %d %d\n",
                barrel->husk_sub_part_types[0], barrel->husk_sub_part_types[1],
                barrel->husk_sub_part_types[2], barrel->husk_sub_part_types[3]);
        def_free_items(&items);
        return 1;
    }
    /* husk_swap_at_sec 2.0 -> 124 ticks; husk_swap_at AFTER _sec parses as
       seconds too (the witnessed dual-unit order): 3.0 -> 186
       [orig: @ 0x49f1ce-0x49f2c2; scales 62.0 @ 0x7c88c0 / 0.01 @ 0x7c56a8] */
    if (barrel->husk_swap_at_sec < 123.9f || barrel->husk_swap_at_sec > 124.1f ||
        barrel->husk_swap_at < 185.9f || barrel->husk_swap_at > 186.1f) {
        fprintf(stderr, "FAIL: barrel husk_swap mismatch: %f / %f\n",
                (double)barrel->husk_swap_at, (double)barrel->husk_swap_at_sec);
        def_free_items(&items);
        return 1;
    }
    if (barrel->debris_scale < 1.49f || barrel->debris_scale > 1.51f) {
        fprintf(stderr, "FAIL: barrel debris_scale mismatch: %f\n",
                (double)barrel->debris_scale);
        def_free_items(&items);
        return 1;
    }
    /* scale 1.1 -> trunc(1.1 * 65536) = 72089, not round-to-nearest.
       [orig: temporary x87 RC=truncate + fistp @ 0x49f6e0..0x49f73d] */
    if (barrel->scale_q16 != 72089) {
        fprintf(stderr, "FAIL: barrel scale_q16 mismatch: %d\n",
                barrel->scale_q16);
        def_free_items(&items);
        return 1;
    }

    /* Find "Player #1, Single player" by id */
    const DefItemDef *player1 = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].id == 105310) {
            player1 = &items.entries[i];
            break;
        }
    }

    if (!player1) {
        fprintf(stderr, "FAIL: could not find Player #1, Single player (id 105310)\n");
        def_free_items(&items);
        return 1;
    }

    if (strcmp(player1->display_name, "Player #1, Single player") != 0) {
        fprintf(stderr, "FAIL: player1 display_name mismatch: '%s'\n", player1->display_name);
        def_free_items(&items);
        return 1;
    }

    /* person = 3 [orig: ItemDef_ParseProperty @ 0x49eb00] */
    if (player1->type != DEF_ITEM_TYPE_PERSON) {
        fprintf(stderr, "FAIL: player1 type mismatch: expected 3 (person), got %d\n", player1->type);
        def_free_items(&items);
        return 1;
    }

    /* graphic should be "US01", not overridden by "graphicenemy Indo01" */
    if (strcmp(player1->graphic, "US01") != 0) {
        fprintf(stderr, "FAIL: player1 graphic mismatch: expected 'US01', got '%s'\n",
                player1->graphic);
        fprintf(stderr, "  (bug: 'graphicenemy' may have overridden 'graphic')\n");
        def_free_items(&items);
        return 1;
    }

    if (strcmp(player1->anim_def, "US01") != 0) {
        fprintf(stderr, "FAIL: player1 anim_def mismatch: '%s'\n", player1->anim_def);
        def_free_items(&items);
        return 1;
    }

    /* sound_profile should be "SP_JO_SP_PlayerM1", not overridden by sound_profileFemale */
    if (strcmp(player1->sound_profile, "SP_JO_SP_PlayerM1") != 0) {
        fprintf(stderr, "FAIL: player1 sound_profile mismatch: expected 'SP_JO_SP_PlayerM1', got '%s'\n",
                player1->sound_profile);
        def_free_items(&items);
        return 1;
    }

    /* The female variant lands in its own slot [orig: "sound_profileFemale"
       @ 0x49fb76 -> def+0x26C]. */
    if (strcmp(player1->sound_profile_female, "SP_JO_SP_PlayerF1") != 0) {
        fprintf(stderr,
                "FAIL: player1 sound_profile_female mismatch: expected 'SP_JO_SP_PlayerF1', got '%s'\n",
                player1->sound_profile_female);
        def_free_items(&items);
        return 1;
    }

    /* A def authoring only sound_profile mirrors it into the female slot (both
       seed to "default" and track the primary until female is authored
       [orig: the +0x26C == +0x268 rewrite gate @ 0x49fb0f-0x49fb64]). */
    {
        const DefItemDef *soldier = NULL;
        for (size_t i = 0; i < items.count; ++i) {
            if (strcmp(items.entries[i].sound_profile, "SP_JO_SP_SoldierM1") == 0) {
                soldier = &items.entries[i];
                break;
            }
        }
        if (soldier == NULL ||
            strcmp(soldier->sound_profile_female, "SP_JO_SP_SoldierM1") != 0) {
            fprintf(stderr,
                    "FAIL: soldier sound_profile_female should mirror the primary, got '%s'\n",
                    soldier == NULL ? "(no soldier item)" : soldier->sound_profile_female);
            def_free_items(&items);
            return 1;
        }
    }

    if (player1->hp != 150) {
        fprintf(stderr, "FAIL: player1 hp mismatch: expected 150, got %d\n", player1->hp);
        def_free_items(&items);
        return 1;
    }

    /* The player's ai_function is `plyr` — the §5.10b dispatch tag that selects
       NetPacket_SerializePlayerState. move_function is `org2` (a movement-family
       variant); disk_function is the load-class string `PLAYER`. */
    if (strcmp(player1->ai_function, "plyr") != 0) {
        fprintf(stderr, "FAIL: player1 ai_function mismatch: expected 'plyr', got '%s'\n",
                player1->ai_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(player1->move_function, "org2") != 0) {
        fprintf(stderr, "FAIL: player1 move_function mismatch: expected 'org2', got '%s'\n",
                player1->move_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(player1->disk_function, "PLAYER") != 0) {
        fprintf(stderr, "FAIL: player1 disk_function mismatch: expected 'PLAYER', got '%s'\n",
                player1->disk_function);
        def_free_items(&items);
        return 1;
    }

    /* AI infantry: Generic Soldier (id 105311) uses ai_function org1 = the
       §5.10b dispatch tag that selects NetPacket_SerializeInfantryEntityState. */
    const DefItemDef *soldier = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].id == 105311) {
            soldier = &items.entries[i];
            break;
        }
    }
    if (!soldier) {
        fprintf(stderr, "FAIL: could not find Generic Soldier (id 105311)\n");
        def_free_items(&items);
        return 1;
    }
    if (strcmp(soldier->ai_function, "org1") != 0) {
        fprintf(stderr, "FAIL: soldier ai_function mismatch: expected 'org1', got '%s'\n",
                soldier->ai_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(soldier->move_function, "org1") != 0) {
        fprintf(stderr, "FAIL: soldier move_function mismatch: expected 'org1', got '%s'\n",
                soldier->move_function);
        def_free_items(&items);
        return 1;
    }

    /* The anim-fire weapon family + clipsize default: absent keys leave the fields
       zeroed (the fixture predates the JO character-item authoring). */
    if (soldier->ammo_closeattack[0] != '\0' || soldier->ammo_easyrocket[0] != '\0' ||
        soldier->ammo_advancedrocket[0] != '\0' || soldier->ammo_marker3[0] != '\0' ||
        soldier->clipsize != 0 || soldier->launchups_closeattack[0] != '\0' ||
        soldier->launchups_rocket[0] != '\0' || soldier->launchups_marker3[0] != '\0') {
        fprintf(stderr, "FAIL: soldier ammo/clipsize/launchups should be unset, got '%s'/%d/'%s'\n",
                soldier->ammo_closeattack, soldier->clipsize,
                soldier->launchups_closeattack);
        def_free_items(&items);
        return 1;
    }

    def_free_items(&items);

    if (test_particle_keys() != 0) {
        return 1;
    }
    if (test_phrase_set_presence() != 0) {
        return 1;
    }


    /* The person-item anim-fire weapon family (world-wac-ai-re §17.4/§33.35):
       'ammo_closeattack' name -> def+0x56B, 'clipsize' atol -> def+0x894
       [orig: ItemDef_ParseProperty @ 0x4a1823 / @ 0x49fa1c]. The tracked fixture
       has no character items, so pin the parse on an inline JOX-shaped block
       (retail "Indonesian Soldier #1 with AK47", id 101798). */
    static const char rifleman_def[] =
        "begin \"Indonesian Soldier #1 with AK47\"\n"
        "  id 101798\n"
        "  type person\n"
        "  ai_function org1\n"
        "  move_function org1\n"
        "  clipsize 30\n"
        "  ammo_closeattack    AMMO_AK47_556MM\n"
        "  ammo_easyrocket     AMMO_EASY\n"
        "  ammo_advancedrocket AMMO_ADVANCED\n"
        "  ammo_marker3        AMMO_MARKER\n"
        "  launchups_closeattack    mflash01\n"
        "  launchups_rocket         rocket01\n"
        "  launchups_marker3        marker03\n"
        "  weaprbup   gunfire_r\n"
        "  weaplmup   flash_l\n"
        "  weaprcup2  casing_r2\n"
        "end\n";
    DefItemsFile rifle_items;
    memset(&rifle_items, 0, sizeof(rifle_items));
    if (def_parse_items_memory((const uint8_t *)rifleman_def, sizeof(rifleman_def) - 1,
                               &rifle_items) != 0 ||
        rifle_items.count != 1) {
        fprintf(stderr, "FAIL: inline rifleman block did not parse\n");
        def_free_items(&rifle_items);
        return 1;
    }
    if (strcmp(rifle_items.entries[0].ammo_closeattack, "AMMO_AK47_556MM") != 0 ||
        strcmp(rifle_items.entries[0].ammo_easyrocket, "AMMO_EASY") != 0 ||
        strcmp(rifle_items.entries[0].ammo_advancedrocket, "AMMO_ADVANCED") != 0 ||
        strcmp(rifle_items.entries[0].ammo_marker3, "AMMO_MARKER") != 0) {
        fprintf(stderr, "FAIL: rifleman ammo_closeattack mismatch: '%s'\n",
                rifle_items.entries[0].ammo_closeattack);
        def_free_items(&rifle_items);
        return 1;
    }
    /* The def-authored launch userpoint NAME (the AI muzzle; world-wac-ai-re
       §21.2) [orig: ItemDef_ParseProperty launchups_* -> def+0x5EB/+0x5FB]. */
    if (strcmp(rifle_items.entries[0].launchups_closeattack, "mflash01") != 0 ||
        strcmp(rifle_items.entries[0].launchups_rocket, "rocket01") != 0 ||
        strcmp(rifle_items.entries[0].launchups_marker3, "marker03") != 0) {
        fprintf(stderr, "FAIL: rifleman launchups_closeattack mismatch: '%s'\n",
                rifle_items.entries[0].launchups_closeattack);
        def_free_items(&rifle_items);
        return 1;
    }
    if (rifle_items.entries[0].clipsize != 30) {
        fprintf(stderr, "FAIL: rifleman clipsize mismatch: expected 30, got %d\n",
                rifle_items.entries[0].clipsize);
        def_free_items(&rifle_items);
        return 1;
    }
    /* The twelve weap[lr][bmc]up[2] userpoint NAMES land in parse order
       (weaplbup, weaplmup, weaplcup, weaprbup, weaprmup, weaprcup, then the
       `2` variants), absent keys zero-filled [orig: ItemDef_ParseProperty
       @ 0x4a0ff2..0x4a1301 -> def+0x61B..0x6CB]. */
    {
        const DefItemDef *r = &rifle_items.entries[0];
        if (strcmp(r->weapon_userpoints[3], "gunfire_r") != 0 ||
            strcmp(r->weapon_userpoints[1], "flash_l") != 0 ||
            strcmp(r->weapon_userpoints[11], "casing_r2") != 0 ||
            r->weapon_userpoints[0][0] != '\0' ||
            r->weapon_userpoints[5][0] != '\0') {
            fprintf(stderr, "FAIL: weapon userpoint keys mismatch: [3]='%s' [1]='%s' [11]='%s' [0]='%s' [5]='%s'\n",
                    r->weapon_userpoints[3], r->weapon_userpoints[1],
                    r->weapon_userpoints[11], r->weapon_userpoints[0],
                    r->weapon_userpoints[5]);
            def_free_items(&rifle_items);
            return 1;
        }
    }
    def_free_items(&rifle_items);

    /* 'primary_weapon' — the ewep emplacement's mounted weapon.def entry (the attach
       label's text source) [orig: -> ItemDef+0x54B primaryWeapon, itemdef-re.md]. The
       fixture carries the retail "NON-Armored Emplaced 50cal for FAV" block (id 101419). */
    {
        DefItemsFile items2;
        memset(&items2, 0, sizeof(items2));
        snprintf(path, sizeof(path), "%s/fixtures/def/items.def", repo_root);
        if (def_parse_items(path, &items2) != 0) {
            fprintf(stderr, "FAIL: reparse for primary_weapon failed\n");
            return 1;
        }
        const DefItemDef *ewep = NULL;
        for (size_t i = 0; i < items2.count; ++i) {
            if (items2.entries[i].id == 101419) { ewep = &items2.entries[i]; break; }
        }
        if (!ewep || strcmp(ewep->primary_weapon, "WPN_EMPLCD50NA") != 0 ||
            strcmp(ewep->ai_function, "ewep") != 0 ||
            ewep->armor_impact != -1 || ewep->armor_kz != -1 ||
            /* the fixture's authored l/r b/m/c userpoint triplets, `2` slots empty */
            strcmp(ewep->weapon_userpoints[0], "bullet") != 0 ||
            strcmp(ewep->weapon_userpoints[1], "mflash01") != 0 ||
            strcmp(ewep->weapon_userpoints[2], "bcasing") != 0 ||
            strcmp(ewep->weapon_userpoints[3], "bullet") != 0 ||
            strcmp(ewep->weapon_userpoints[4], "mflash01") != 0 ||
            strcmp(ewep->weapon_userpoints[5], "bcasing") != 0 ||
            ewep->weapon_userpoints[6][0] != '\0' ||
            ewep->weapon_userpoints[11][0] != '\0') {
            fprintf(stderr, "FAIL: ewep primary_weapon: '%s' ai='%s' (found=%d)\n",
                    ewep ? ewep->primary_weapon : "", ewep ? ewep->ai_function : "",
                    ewep != NULL);
            def_free_items(&items2);
            return 1;
        }
        def_free_items(&items2);
    }

    /* Vehicle child-emplacement attachments retain authored order, optional
       down/up/right/left limits, and the G/C key variants. */
    static const char attachment_def[] =
        "begin AttachmentCarrier\n"
        "  id 100164\n"
        "  addeweap abcdefghijklmnopq 100166\n"
        "  addeweapG ewep02 100183 70 10 100 100\n"
        "  addeweapC ewep03 100182\n"
        "  addeweapC ewep04 100184 0 0 0 0\n"
        "  addeweapG ignored05 100185\n"
        "end\n"
        "begin PartialAngles\n"
        "  id 100200\n"
        "  addeweap ewep01 100201 15 37\n"
        "end\n";
    DefItemsFile attachment_items;
    memset(&attachment_items, 0, sizeof(attachment_items));
    if (def_parse_items_memory((const uint8_t *)attachment_def,
                               sizeof(attachment_def) - 1,
                               &attachment_items) != 0 ||
        attachment_items.count != 2) {
        fprintf(stderr, "FAIL: inline emplacement-attachment block did not parse\n");
        def_free_items(&attachment_items);
        return 1;
    }
    const DefItemDef *carrier = &attachment_items.entries[0];
    if (carrier->emplacement_attachments_count != 4) {
        fprintf(stderr, "FAIL: expected 4 capped emplacement attachments, got %zu\n",
                carrier->emplacement_attachments_count);
        def_free_items(&attachment_items);
        return 1;
    }
    const DefItemEmplacementAttachment *a0 = &carrier->emplacement_attachments[0];
    const DefItemEmplacementAttachment *a1 = &carrier->emplacement_attachments[1];
    const DefItemEmplacementAttachment *a2 = &carrier->emplacement_attachments[2];
    const DefItemEmplacementAttachment *a3 = &carrier->emplacement_attachments[3];
    static const int bam_per_degree = 11930464;
    if (a0->kind != DEF_ITEM_EMPLACEMENT_ADDEWEAP ||
        strcmp(a0->userpoint, "abcdefghijklmno") != 0 || a0->item_id != 100166 ||
        a0->angle_count != 0 ||
        a1->kind != DEF_ITEM_EMPLACEMENT_ADDEWEAP_G ||
        strcmp(a1->userpoint, "ewep02") != 0 || a1->item_id != 100183 ||
        a1->angle_count != 4 || a1->down_angle != 70 * bam_per_degree ||
        a1->up_angle != -10 * bam_per_degree ||
        a1->right_angle != 100 * bam_per_degree ||
        a1->left_angle != -100 * bam_per_degree ||
        a2->kind != DEF_ITEM_EMPLACEMENT_ADDEWEAP_C ||
        strcmp(a2->userpoint, "ewep03") != 0 || a2->item_id != 100182 ||
        a2->angle_count != 0 ||
        a3->kind != DEF_ITEM_EMPLACEMENT_ADDEWEAP_C ||
        a3->angle_count != 4 || a3->down_angle != 0 || a3->up_angle != 0 ||
        a3->right_angle != 0 || a3->left_angle != 0 ||
        carrier->emplacement_g_slot != 2 || carrier->emplacement_c_slot != 4 ||
        carrier->raw_lines_count != 0 ||
        attachment_items.entries[1].emplacement_attachments_count != 0 ||
        attachment_items.entries[1].raw_lines_count != 1) {
        fprintf(stderr, "FAIL: emplacement attachment parse semantics mismatch\n");
        def_free_items(&attachment_items);
        return 1;
    }
    def_free_items(&attachment_items);

    static const char damage_def[] =
        "begin A\n id 1\n armor 7 11\n damage_reduc_pp .25\nend\n"
        "begin B\n id 2\n armor 5\n damage_reduc_pp .25 .60\nend\n"
        "begin C\n id 3\nend\n"
        "begin D\n id 4\n hp 65535\n armor 32768 65534\nend\n";
    DefItemsFile damage_items;
    memset(&damage_items, 0, sizeof(damage_items));
    if (def_parse_items_memory((const uint8_t *)damage_def, sizeof(damage_def) - 1,
                               &damage_items) != 0 || damage_items.count != 4) {
        fprintf(stderr, "FAIL: inline damage-trait block did not parse\n");
        def_free_items(&damage_items);
        return 1;
    }
    const DefItemDef *d0 = &damage_items.entries[0];
    const DefItemDef *d1 = &damage_items.entries[1];
    const DefItemDef *d2 = &damage_items.entries[2];
    const DefItemDef *d3 = &damage_items.entries[3];
    if (d0->armor_kz != 7 || d0->armor_impact != 11 ||
        d0->damage_reduc_pp != 0.25f || d0->damage_reduc_max != 0.75f ||
        d1->armor_kz != 5 || d1->armor_impact != 5 ||
        d1->damage_reduc_max < 0.599f || d1->damage_reduc_max > 0.601f ||
        d2->armor_kz != 0 || d2->armor_impact != 0 ||
        d2->damage_reduc_pp != 0.0f || d2->damage_reduc_max != 0.0f ||
        d3->hp != -1 || d3->armor_kz != -32768 || d3->armor_impact != -2) {
        fprintf(stderr, "FAIL: damage trait parse semantics mismatch\n");
        def_free_items(&damage_items);
        return 1;
    }
    def_free_items(&damage_items);

    /* The authored ground-shadow blob decal 'shadow <name> <w> <l> <ox> <oy>'
       [orig: ItemDef_ParseProperty @ 0x49f3a5..0x49f44c — name +0xA0, atof
       floats +0x11C..+0x128; absent tokens read as 0]. */
    static const char shadow_def[] =
        "begin Hind\n id 1\n shadow hindshdw.tga 6 8.5 0 -1.25\nend\n"
        "begin Jeep\n id 2\n shadow jeepshdw.tga\nend\n"
        "begin Crate\n id 3\nend\n"
        "begin Long\n id 4\n shadow averyverylongshadowname.tga 1 2 3 4\nend\n";
    DefItemsFile shadow_items;
    memset(&shadow_items, 0, sizeof(shadow_items));
    if (def_parse_items_memory((const uint8_t *)shadow_def, sizeof(shadow_def) - 1,
                               &shadow_items) != 0 || shadow_items.count != 4) {
        fprintf(stderr, "FAIL: inline shadow-decal block did not parse\n");
        def_free_items(&shadow_items);
        return 1;
    }
    const DefItemDef *s0 = &shadow_items.entries[0];
    const DefItemDef *s1 = &shadow_items.entries[1];
    const DefItemDef *s2 = &shadow_items.entries[2];
    const DefItemDef *s3 = &shadow_items.entries[3];
    if (strcmp(s0->shadow_texture, "hindshdw.tga") != 0 ||
        s0->shadow_width != 6.0f || s0->shadow_length != 8.5f ||
        s0->shadow_offset_x != 0.0f || s0->shadow_offset_y != -1.25f ||
        strcmp(s1->shadow_texture, "jeepshdw.tga") != 0 ||
        s1->shadow_width != 0.0f || s1->shadow_length != 0.0f ||
        s1->shadow_offset_x != 0.0f || s1->shadow_offset_y != 0.0f ||
        s2->shadow_texture[0] != '\0' ||
        /* The 16-byte slot keeps 15 chars + NUL (retail copies unguarded into
           ItemDef+0xA0; no shipped items.def authors a name that long). */
        strcmp(s3->shadow_texture, "averyverylongsh") != 0 ||
        s3->shadow_width != 1.0f || s3->shadow_offset_y != 4.0f) {
        fprintf(stderr, "FAIL: shadow decal parse semantics mismatch\n");
        def_free_items(&shadow_items);
        return 1;
    }
    def_free_items(&shadow_items);

    if (test_retail_particlefx_rows() != 0) {
        return 1;
    }

    printf("PASS: items parsing OK\n");
    return 0;
}
