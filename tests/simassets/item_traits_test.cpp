// engine/runtime/simassets item-traits fold (ADR 0028) — the def-row -> world
// trait mapping, pinned headless: entity stamps (AI-capable gate, wire-class
// byte, healthMax lift + retail i16 wrap, armor, AS zone attribs, corpse
// timing), the per-item death-trait and 21-field vehicle-trait tables
// (distinct sentinels per field so a transposition cannot pass), the
// throwable class scan with last-wins duplicate ids, and the organic ammo
// seed (§33.35). Parser token semantics are def's own tests; exotic fields are stamped
// post-parse so this file pins only the FOLD's mapping.
#include <formats/def/def.h>
#include <runtime/simassets/item_traits.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::def;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

const char kItemsDef[] = R"(begin "S5 Player"
  id 105305
  type person
  graphic soldier
  sid s5player
  hp 100
end

begin "S5 Tank"
  id 100500
  type vehicle
  graphic tank
  sid s5tank
  ai_function cveh
  render_function cveh
  move_function ctank
  attrib: AIData PlayerControl
  hp 40000
  physics 2
end

begin "S5 APC"
  id 100501
  type vehicle
  graphic apc
  sid s5apc
  ai_function cveh
  render_function cveh
  move_function catv
  attrib: AIData PlayerControl
  hp 1000
  physics 1
end

begin "S5 Helo"
  id 100502
  type vehicle
  graphic helo
  sid s5helo
  ai_function CHel
  render_function CHel
  move_function CHelScout
  attrib: AIData PlayerControl
  hp 2000
end

begin "S5 Truck NoPhys"
  id 100503
  type vehicle
  graphic truck
  sid s5truck
  ai_function cveh
  render_function cveh
  move_function cveh
  attrib: AIData
  hp 800
end

begin "S5 Rifleman"
  id 100510
  type person
  graphic rifleman
  sid s5rifle
  hp 30
  clipsize 30
  deathtime 5
end

begin "S5 Bunker"
  id 100520
  type building
  graphic bunker
  sid s5bunker
  husk bunk_husk
  huskfinal bunk_burn
  music 65533
end

begin "S5 Bush"
  id 100530
  type decoration
  graphic bush
  sid s5bush
  hp 5
end

begin "S5 Frag"
  id 100600
  type powerup
  graphic frag
  sid s5frag
  ai_function nade
  move_function nade
  hp 25
end

begin "S5 ATMine"
  id 100601
  type powerup
  graphic atmine
  sid s5atmine
  ai_function vmne
  move_function schl
  hp 10
end

begin "S5 Dup A"
  id 100602
  type powerup
  graphic dupa
  sid s5dupa
  ai_function clym
  move_function clym
  hp 11
end

begin "S5 Dup B"
  id 100602
  type powerup
  graphic dupb
  sid s5dupb
  ai_function nade
  move_function nade
  hp 12
end

begin "S5 Crate"
  id 100540
  type decoration
  graphic crate
  sid s5crate
  ai_function gnrl
  hp 104
end

begin "S5 Oil Tank"
  id 100541
  type decoration
  graphic oiltank
  sid s5oil
  ai_function GNRC
  hp 200
end

begin "S5 Pump"
  id 100542
  type decoration
  graphic pump
  sid s5pump
  ai_function gnl2
  hp 50
end

begin "S5 Gun"
  id 100543
  type decoration
  graphic b50cal
  sid s5gun
  ai_function ewep
  hp 10
end

begin "S5 Palm"
  id 100544
  type decoration
  graphic palm
  sid s5palm
  ai_function tree
  hp 10
end

begin "S5 Radar"
  id 100545
  type building
  graphic radar
  sid s5radar
  ai_function bld2
  hp 300
end

begin "S5 Typo"
  id 100546
  type decoration
  graphic typo
  sid s5typo
  ai_function gnr1
  hp 10
end
)";

// The minimal SndProf shape ("default" first, so a real profile lands at
// index 1 and a miss falls back to 0 by the table's own rule).
const char kProfiles[] =
    "begin \"default\"\n"
    "     SSLFootGND     DEF_FOOT_L\n"
    "end\n"
    "begin \"SP_Test\"\n"
    "     SSLFootGND     T_DIRT_L\n"
    "end\n"
    "begin \"SP_TestFemale\"\n"
    "     SSLFootGND     T_DIRT_L_F\n"
    "end\n";

// LAST entry with the id — the same row the fold's last-wins index resolves,
// so post-parse stamps land on the row the fold will read.
DefItemDef *entry_for(DefItemsFile &f, int id) {
    DefItemDef *found = nullptr;
    for (size_t i = 0; i < f.count; ++i)
        if (f.entries[i].id == id) found = &f.entries[i];
    return found;
}

EntityHandle spawn(World &w, int pool, uint16_t item_id, EntityKind kind) {
    Entity e;
    e.kind = kind;
    e.item_id = item_id;
    e.health = 100;
    e.alive = true;
    return w.registry.spawn(pool, e);
}

} // namespace

int main() {
    DefItemsFile file = {};
    CHECK(def_parse_items_memory(
                  reinterpret_cast<const uint8_t *>(kItemsDef),
                  sizeof(kItemsDef) - 1, &file) == 0);
    CHECK(file.count == 19);

    // Stamp the fields whose authored-token spellings are the def parser's own
    // test surface: distinct sentinels per vehicle-physics slot so any
    // field-order transposition in the fold fails loudly.
    DefItemDef *player = entry_for(file, 105305);
    DefItemDef *tank = entry_for(file, 100500);
    DefItemDef *rifle = entry_for(file, 100510);
    DefItemDef *bunker = entry_for(file, 100520);
    CHECK(player && tank && rifle && bunker);
    player->armor_impact = 3;
    player->armor_kz = 4;
    player->damage_reduc_pp = 0.25f;
    player->damage_reduc_max = 10.0f;
    tank->player_speed = 201;
    tank->acceleration = 202;
    tank->deceleration = 203;
    tank->turn_rate = 204;
    tank->turn_rate2 = 205;
    tank->unit_type = 7;
    tank->torque = 207;
    tank->water_speed = 208;
    tank->climb_speed = 209;
    tank->turn_roll = 210;
    tank->speed_pitch = 211;
    tank->max_slope = 212;
    tank->slip_slope = 213;
    tank->mass = 214;
    tank->lean = 215;
    tank->lean_velocity = 216;
    tank->pitch = 217;
    tank->pitch_velocity = 218;
    tank->bob = 219;
    tank->flip = 220;
    tank->scale_q16 = 0x18000;
    std::strcpy(tank->particlefxw3.effect, "fx_sml_wk");
    std::strcpy(tank->particlefxw3.userpoint, "FX00");
    std::strcpy(tank->particlefxw4.effect, "fx_sml_wk_f");
    std::strcpy(tank->particlefxw4.userpoint, "FX01");
    std::strcpy(tank->sound_profile, "SP_Tank");
    std::strcpy(tank->soundloops[0], "LP_TANK");
    std::strcpy(rifle->ammo_closeattack, "at_rifle");
    std::strcpy(rifle->ammo_easyrocket, "AT_EASY");
    std::strcpy(rifle->ammo_advancedrocket, "AT_ADVANCED");
    std::strcpy(rifle->ammo_marker3, "AT_MARKER");
    std::strcpy(rifle->sound_profile, "SP_Test");
    std::strcpy(rifle->sound_profile_female, "SP_TestFemale");
    rifle->attrib |= DEF_ITEM_ATTRIB_LEAVECORPSE;
    bunker->attrib |= DEF_ITEM_ATTRIB_CHANGETEAM | DEF_ITEM_ATTRIB_SPAWNPOINT |
            DEF_ITEM_ATTRIB_NODIE | 0x8000u; // 0x8000 = the unwitnessed team-protect bit
    bunker->attrib2 |= DEF_ITEM_ATTRIB2_STATICDEATH;
    bunker->unit_type = 5;
    bunker->kz = 6.5f;
    bunker->armor_impact = 12;
    bunker->armor_blast = 34;
    bunker->armor_kz = 34;
    bunker->debris_scale = 1.5f;
    bunker->husk_sub_parts = 3;
    bunker->husk_sub_part_types[0] = 4;
    bunker->husk_sub_part_types[1] = 1;
    bunker->husk_sub_part_types[2] = 9;
    std::strcpy(bunker->sounddeath, "BUNK_DIE");
    std::strcpy(bunker->particledeath, "pd_bunker");
    std::strcpy(bunker->particleh2odeath, "pd_bunker_h2o");
    std::strcpy(bunker->particlefire, "pf_bunker");
    std::strcpy(bunker->particleother, "po_bunker");

    World w;
    w.registry.configure_pool(0, 8);  // organics
    w.registry.configure_pool(1, 8);  // items/vehicles
    w.registry.configure_pool(2, 16); // buildings (bunker, bush, unknown + the 7 class rows)
    const EntityHandle tank_h = spawn(w, 1, 500, EntityKind::Item);
    const EntityHandle apc_h = spawn(w, 1, 501, EntityKind::Item);
    const EntityHandle helo_h = spawn(w, 1, 502, EntityKind::Item);
    const EntityHandle truck_h = spawn(w, 1, 503, EntityKind::Item);
    const EntityHandle rifle_h = spawn(w, 0, 510, EntityKind::Organic);
    const EntityHandle player_h = spawn(w, 0, 5305, EntityKind::Organic);
    const EntityHandle bunker_h = spawn(w, 2, 520, EntityKind::Building);
    const EntityHandle bush_h = spawn(w, 2, 530, EntityKind::Building);
    const EntityHandle unknown_h = spawn(w, 2, 999, EntityKind::Building);
    // The ai_function class rows (one entity each; only the death-trait row
    // is read back).
    for (uint16_t id = 540; id <= 546; ++id) spawn(w, 2, id, EntityKind::Building);

    // The wire-class supplier stub: the fold must stamp the returned byte
    // verbatim and default a functor miss to 0 (Unknown, fail closed).
    const simassets::ItemWireClassFn wire_class = [](int def_id) -> uint8_t {
        if (def_id == 100500) return 3; // Vehicle
        if (def_id == 100510) return 2; // Infantry
        return 0;
    };
    simassets::resolve_item_traits(w, file, wire_class);

    // ---- the Player template block (D-NET-144) ----
    CHECK(w.tables.player.has_item_def);
    CHECK(w.tables.player.item_hp == 100);
    CHECK(w.tables.player.item_type == 3);
    CHECK(w.tables.player.armor_impact == 3);
    CHECK(w.tables.player.armor_kz == 4);
    CHECK(w.tables.player.damage_reduc_pp == 0.25f);
    CHECK(w.tables.player.damage_reduc_max == 10.0f);

    // ---- entity stamps ----
    const Entity *tank_e = w.registry.get(tank_h);
    CHECK(tank_e != nullptr);
    CHECK(tank_e->has_item_def);
    CHECK(tank_e->item_type == 1);
    CHECK(tank_e->is_ai_capable);
    CHECK(tank_e->net_class_code == 3);
    // hp 40000 wraps through the retail signed-i16 word and lifts the
    // promotion-default 100 [orig: Entity_InitFromItemDef @0x49e550].
    CHECK(tank_e->health_max == -25536);
    CHECK(tank_e->health == -25536);
    CHECK(tank_e->item_unit_type == 7);
    CHECK(tank_e->uniform_scale_q16 == 0x18000);
    // The display-name table: once per distinct id, the def row's name; an
    // unknown id has no row.
    CHECK(w.tables.item_names.get(500) != nullptr && *w.tables.item_names.get(500) == "S5 Tank");
    CHECK(w.tables.item_names.get(510) != nullptr && *w.tables.item_names.get(510) == "S5 Rifleman");
    CHECK(w.tables.item_names.get(999) == nullptr);

    const Entity *rifle_e = w.registry.get(rifle_h);
    CHECK(rifle_e != nullptr);
    CHECK(rifle_e->net_class_code == 2);
    CHECK(rifle_e->health_max == 30);
    CHECK(rifle_e->leave_corpse);
    // deathtime 5 authored -> (62*5)+62 parse-scaled ticks ride the def row.
    CHECK(rifle_e->deathtime_ticks == 372);

    const Entity *bunker_e = w.registry.get(bunker_h);
    CHECK(bunker_e != nullptr);
    // def hp 0 => indestructible flags [orig: Entity_InitFromModel @0x40dc8e].
    CHECK((bunker_e->engine_flags & 0x4000000u) != 0);
    CHECK(bunker_e->sub_type == 0xFF);
    CHECK(bunker_e->health == 100); // hp 0 lifts nothing
    CHECK(bunker_e->music_location == -3); // parsed signed word survives trait promotion
    CHECK(bunker_e->is_capture_trigger);
    CHECK(bunker_e->is_spawn_point);
    CHECK(bunker_e->armor_impact == 12);
    CHECK(bunker_e->armor_kz == 34);
    CHECK(bunker_e->deathtime_ticks == 0);

    const Entity *unknown_e = w.registry.get(unknown_h);
    CHECK(unknown_e != nullptr);
    CHECK(!unknown_e->has_item_def);
    CHECK(unknown_e->item_type == 0);
    CHECK(unknown_e->uniform_scale_q16 == 0);
    CHECK(unknown_e->item_attrib == 0u);
    CHECK(unknown_e->net_class_code == 0);
    CHECK(unknown_e->health == 100);
    CHECK((unknown_e->engine_flags & 0x4000000u) == 0);
    CHECK(unknown_e->sub_type != 0xFF);

    // ---- the death-trait table ----
    const ItemDeathTraits *bt = w.tables.item_death_traits.get(520);
    CHECK(bt != nullptr);
    if (bt != nullptr) {
        CHECK(bt->unit_type == 5);
        CHECK(bt->kz == 6.5f);
        CHECK(bt->armor_impact == 12);
        CHECK(bt->armor_blast == 34);
        CHECK(bt->team_protect);
        CHECK(bt->no_die);
        CHECK(bt->static_death);
        CHECK(bt->has_husk);
        CHECK(!bt->is_decoration);
        CHECK(bt->husk_sub_part_count == 3);
        CHECK(bt->husk_sub_part_types[0] == 4);
        CHECK(bt->husk_sub_part_types[1] == 1);
        CHECK(bt->husk_sub_part_types[2] == 9);
        CHECK(bt->husk_sub_part_types[3] == 0);
        CHECK(bt->husk_sub_part_types[16] == 0);
        CHECK(bt->debris_scale == 1.5f);
        CHECK(bt->sound_death == "BUNK_DIE");
        CHECK(bt->particledeath == "pd_bunker");
        CHECK(bt->particleh2odeath == "pd_bunker_h2o");
        CHECK(bt->particlefire == "pf_bunker");
        CHECK(bt->particleother == "po_bunker");
    }
    const ItemDeathTraits *busht = w.tables.item_death_traits.get(530);
    CHECK(busht != nullptr && busht->is_decoration);
    const ItemDeathTraits *tankt = w.tables.item_death_traits.get(500);
    CHECK(tankt != nullptr && !tankt->has_husk);

    // ---- the event/death callback row (D-ITEM-7) ----
    // The fold resolves the ai_function tag the way retail's whole-string
    // stricmp walk of g_EntityClassEventCallbackTable @0x813000 does: a
    // mixed-case tag matches, the absent tag / a tag without a row (the
    // shipped "gnr1" typo) / the callback-less nade row resolve the null row
    // (0x406FF0, never dies), and a row whose callback is unported (bld2
    // @0x43EEE0) keeps the pre-dispatch tree body. [orig:
    // Entity_LookupRenderCallbacks @0x407dc0; EntityDef_InitAllCallbacks @0x4a5aa9]
    const auto class_of = [&](int32_t id) {
        const ItemDeathTraits *t = w.tables.item_death_traits.get(id);
        return t != nullptr ? t->death_class : ItemDeathClass::kUnwitnessed;
    };
    CHECK(w.tables.item_death_traits.get(540) != nullptr);
    CHECK(class_of(540) == ItemDeathClass::kGnrl);
    CHECK(class_of(541) == ItemDeathClass::kGnrc); // "GNRC": case-insensitive
    CHECK(class_of(542) == ItemDeathClass::kGnl2);
    CHECK(class_of(543) == ItemDeathClass::kEwep);
    CHECK(class_of(544) == ItemDeathClass::kTree);
    CHECK(class_of(545) == ItemDeathClass::kUnwitnessed); // bld2: unported row
    CHECK(class_of(546) == ItemDeathClass::kNull);        // "gnr1": no row
    CHECK(class_of(520) == ItemDeathClass::kNull);        // the bunker authors no tag
    CHECK(class_of(530) == ItemDeathClass::kNull);        // nor the bush
    CHECK(class_of(500) == ItemDeathClass::kUnwitnessed); // cveh: the AI machine's death
    CHECK(item_death_class_from_tag("") == ItemDeathClass::kNull);
    CHECK(item_death_class_from_tag("null") == ItemDeathClass::kNull);
    CHECK(item_death_class_from_tag("nade") == ItemDeathClass::kNull);   // fn1 = 0
    CHECK(item_death_class_from_tag("psec") == ItemDeathClass::kNull);   // row 27 -> 0x406FF0
    CHECK(item_death_class_from_tag("pwrp") == ItemDeathClass::kNull);   // row 36 -> 0x406FF0
    CHECK(item_death_class_from_tag("Tree") == ItemDeathClass::kTree);
    CHECK(item_death_class_from_tag("EWEP") == ItemDeathClass::kEwep);
    CHECK(item_death_class_from_tag("gnrcx") == ItemDeathClass::kNull);  // whole-string, not a prefix
    CHECK(item_death_class_from_tag("towr") == ItemDeathClass::kUnwitnessed);
    CHECK(item_death_class_from_tag("emit") == ItemDeathClass::kUnwitnessed);

    // ---- the vehicle-trait table: every slot's sentinel in its own field ----
    const VehicleTraits *vt = w.vehicles.traits.get(500);
    CHECK(vt != nullptr);
    if (vt != nullptr) {
        CHECK(vt->physics == 2);
        CHECK(vt->player_speed == 201);
        CHECK(vt->acceleration == 202);
        CHECK(vt->deceleration == 203);
        CHECK(vt->turn_rate == 204);
        CHECK(vt->turn_rate2 == 205);
        CHECK(vt->unit_type == 7);
        CHECK(vt->torque == 207);
        CHECK(vt->water_speed == 208);
        CHECK(vt->climb_speed == 209);
        CHECK(vt->turn_roll == 210);
        CHECK(vt->speed_pitch == 211);
        CHECK(vt->max_slope == 212);
        CHECK(vt->slip_slope == 213);
        CHECK(vt->mass == 214);
        CHECK(vt->lean == 215);
        CHECK(vt->lean_velocity == 216);
        // The def "pitch"/"pitch_velocity" pair is the traits' bow-lift pair —
        // NOT speed_pitch (the air pitch-rate cap above).
        CHECK(vt->pitch_lift == 217);
        CHECK(vt->pitch_lift_vel == 218);
        CHECK(vt->bob == 219);
        CHECK(vt->flip == 220);
        // ctank keys the 4-byte ctan row -> the tank mover [orig: @0x82ABC0].
        CHECK(vt->family == VehicleFamily::Tank);
		CHECK(vt->render_family == VehicleRenderFamily::Ground); // render cveh, move ctank
		CHECK(vt->player_control);
		CHECK(!vt->amphibian);
        CHECK(vt->sound_profile == "SP_Tank");
        CHECK(vt->sound_loops[0] == "LP_TANK");
        CHECK(vt->sound_loops[1].empty());
		CHECK(vt->trails[2].effect == "fx_sml_wk");
		CHECK(vt->trails[2].userpoint == "FX00");
		CHECK(vt->trails[3].effect == "fx_sml_wk_f");
		CHECK(vt->trails[3].userpoint == "FX01");
	}
	const VehicleTraits *apc_vt = w.vehicles.traits.get(501);
    CHECK(apc_vt != nullptr);
    if (apc_vt != nullptr) {
        CHECK(apc_vt->family == VehicleFamily::Ground);
        CHECK(apc_vt->amphibian); // catv arms the pad water-support forces
    }
    // CHel rows legitimately omit the ground physics selector and still land
    // (direct air mover); the case-folded fourcc accepts mixed-case CHelScout.
    const VehicleTraits *helo_vt = w.vehicles.traits.get(502);
    CHECK(helo_vt != nullptr);
    if (helo_vt != nullptr) {
        CHECK(helo_vt->family == VehicleFamily::Helicopter);
        CHECK(helo_vt->physics == 0);
    }
	// Ground selector zero is a real motor, as used by the shipped LCAC.
	const VehicleTraits *simple_vt = w.vehicles.traits.get(503);
	CHECK(simple_vt != nullptr);
	if (simple_vt != nullptr)
		CHECK(simple_vt->physics == 0);
	(void)apc_h;
	(void)helo_h;
	(void)truck_h;
	(void)bush_h;
	(void)player_h;

	// ---- the throwable class scan ----
    const ThrowableClassRow *frag = w.throwables.classes.get(600);
    CHECK(frag != nullptr);
    if (frag != nullptr) {
        CHECK(frag->think == ThrowClass::kNade);
        CHECK(frag->motor == ThrowClass::kNade);
        CHECK(frag->health_max == 25);
    }
    const ThrowableClassRow *mine = w.throwables.classes.get(601);
    CHECK(mine != nullptr);
    if (mine != nullptr) {
        CHECK(mine->think == ThrowClass::kAVMine);
        CHECK(mine->motor == ThrowClass::kSatchel);
    }
    // Duplicate definition ids resolve last-wins (the later nade block).
    const ThrowableClassRow *dup = w.throwables.classes.get(602);
    CHECK(dup != nullptr);
    if (dup != nullptr) {
        CHECK(dup->think == ThrowClass::kNade);
        CHECK(dup->health_max == 12);
    }
    CHECK(w.throwables.classes.get(500) == nullptr);

    // Idempotent re-run: the once-per-id tables must not duplicate or reset.
    simassets::resolve_item_traits(w, file, wire_class);
    CHECK(w.vehicles.traits.get(500) != nullptr);
    CHECK(w.registry.get(tank_h)->health == -25536);

    // ---- resolve_ai_weapons: the organic ammo seed + sound-profile bind ----
    AiSystem &ai = w.ai;
    const int rifle_ai = ai.attach(rifle_h);
    const int player_ai = ai.attach(player_h);
    CHECK(w.tables.sound_profiles.parse(kProfiles, sizeof(kProfiles) - 1) == 3);
    AmmoTableEntry at_null;
    at_null.name = "AT_NULL";
    at_null.valid = true;
    AmmoTableEntry at_rifle;
    at_rifle.name = "AT_RIFLE";
    at_rifle.valid = true;
    AmmoTableEntry at_easy = at_rifle, at_advanced = at_rifle, at_marker = at_rifle;
    at_easy.name = "AT_EASY";
    at_advanced.name = "AT_ADVANCED";
    at_marker.name = "AT_MARKER";
    w.tables.ammo.entries = {at_null, at_rifle, at_easy, at_advanced, at_marker};

    CHECK(simassets::resolve_ai_weapons(w, file) == 1);
    AiEntity *rifle_b = ai.at(rifle_ai);
    CHECK(rifle_b != nullptr);
    if (rifle_b != nullptr) {
        CHECK((rifle_b->profile.organic.ammo == std::array<uint8_t, 4>{1, 2, 3, 4}));
        CHECK((rifle_b->profile.organic.launch == std::array<uint8_t, 3>{}));
        CHECK(rifle_b->profile.clip_size == 30);
        CHECK(rifle_b->inf.magazine == 30);
        CHECK(rifle_b->profile.sound_profile == 1); // SP_Test
        CHECK(rifle_b->profile.sound_profile_female == 2); // SP_TestFemale
    }
    // The player def authors no anim-fire round and no profile: unarmed, and
    // the profile binding stays at the emit-side "default" fallback (-1).
    AiEntity *player_b = ai.at(player_ai);
    CHECK(player_b != nullptr);
    if (player_b != nullptr) {
        CHECK((player_b->profile.organic.ammo == std::array<uint8_t, 4>{}));
        CHECK(player_b->profile.sound_profile == -1);
        CHECK(player_b->profile.sound_profile_female == -1);
    }

    // ---- the per-DEF organic binding (the wire body channel's resolve) ----
    // Resolved for every def, not per spawned AiEntity — a joiner world with
    // no mission AI still binds every replicated type; the tank def has no
    // AiEntity here and still binds. [orig: ItemDef_ResolveAllResources
    // @ 0x49e5f0 resolves the sound region for each def]
    const audio::OrganicSoundProfile *rifle_op = w.tables.organic_sound_profiles.get(510);
    CHECK(rifle_op != nullptr);
    if (rifle_op != nullptr) {
        CHECK(rifle_op->primary == 1); // SP_Test
        CHECK(rifle_op->female == 2);  // SP_TestFemale
    }
    const audio::OrganicSoundProfile *tank_op = w.tables.organic_sound_profiles.get(500);
    CHECK(tank_op != nullptr);
    // "SP_Tank" is not in the profile table: the witnessed find-miss binds the
    // array base [orig: SoundProfile_FindSlotByName @ 0x526e30].
    if (tank_op != nullptr) CHECK(tank_op->primary == 0);
    // An unauthored pair stays -1 — the emit side falls to "default".
    const audio::OrganicSoundProfile *player_op =
            w.tables.organic_sound_profiles.get(5305);
    CHECK(player_op != nullptr);
    if (player_op != nullptr) {
        CHECK(player_op->primary == -1);
        CHECK(player_op->female == -1);
    }

    // A per-entity attrib override (the F3 / MCP debug seam) is the sweep's to
    // erase: the next resolve_item_traits re-stamps the def's words and the
    // derived stamps, and the per-item death traits never followed the
    // override in the first place.
    {
        Entity *rifle_override = w.registry.get(rifle_h);
        CHECK(rifle_override != nullptr);
        if (rifle_override != nullptr) {
            const uint32_t authored = rifle_override->item_attrib;
            CHECK(w.commands.set_entity_item_attrib(
                    rifle_h, authored | DEF_ITEM_ATTRIB_NODIE | DEF_ITEM_ATTRIB_SPAWNPOINT, 0x2000u));
            CHECK(rifle_override->is_spawn_point);
            CHECK(w.tables.item_death_traits.get(510) == nullptr || !w.tables.item_death_traits.get(510)->no_die);
            simassets::resolve_item_traits(w, file, wire_class);
            rifle_override = w.registry.get(rifle_h);
            CHECK(rifle_override != nullptr && rifle_override->item_attrib == authored);
            CHECK(rifle_override != nullptr && rifle_override->item_attrib2 == 0u);
            CHECK(rifle_override != nullptr && !rifle_override->is_spawn_point);
            CHECK(rifle_override != nullptr && rifle_override->leave_corpse);
        }
    }

    def_free_items(&file);
    if (failures == 0) std::printf("simassets_item_traits: OK\n");
    return failures == 0 ? 0 : 1;
}
