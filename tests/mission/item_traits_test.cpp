// engine/runtime/mission item-traits fold (ADR 0028) — the def-row -> world
// trait mapping, pinned headless: entity stamps (AI-capable gate, wire-class
// byte, healthMax lift + retail i16 wrap, armor, AS zone attribs, corpse
// timing), the per-item death-trait and 21-field vehicle-trait tables
// (distinct sentinels per field so a transposition cannot pass), the
// throwable class scan with first-wins duplicate ids, and the organic ammo
// seed (§33.35). Parser token semantics are def's own tests; exotic fields are stamped
// post-parse so this file pins only the FOLD's mapping.
#include "common/file_io.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/mission/item_traits.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/item_sections.h>
#include <runtime/world/ai.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::def;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

const char kItemsDef[] = "begin \"S5 Player\"\r\n"
    "  id 105305\r\n"
    "  type person\r\n"
    "  graphic soldier\r\n"
    "  sid s5player\r\n"
    "  hp 100\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Tank\"\r\n"
    "  id 100500\r\n"
    "  type vehicle\r\n"
    "  graphic tank\r\n"
    "  sid s5tank\r\n"
    "  ai_function cveh\r\n"
    "  render_function cveh\r\n"
    "  move_function ctank\r\n"
    "  attrib: AIData PlayerControl\r\n"
    "  hp 40000\r\n"
    "  physics 2\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 APC\"\r\n"
    "  id 100501\r\n"
    "  type vehicle\r\n"
    "  graphic apc\r\n"
    "  sid s5apc\r\n"
    "  ai_function cveh\r\n"
    "  render_function cveh\r\n"
    "  move_function catv\r\n"
    "  attrib: AIData PlayerControl\r\n"
    "  hp 1000\r\n"
    "  physics 1\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Helo\"\r\n"
    "  id 100502\r\n"
    "  type vehicle\r\n"
    "  graphic helo\r\n"
    "  sid s5helo\r\n"
    "  ai_function CHel\r\n"
    "  render_function CHel\r\n"
    "  move_function CHel\r\n"
    "  attrib: AIData PlayerControl\r\n"
    "  hp 2000\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Truck NoPhys\"\r\n"
    "  id 100503\r\n"
    "  type vehicle\r\n"
    "  graphic truck\r\n"
    "  sid s5truck\r\n"
    "  ai_function cveh\r\n"
    "  render_function cveh\r\n"
    "  move_function cveh\r\n"
    "  attrib: AIData\r\n"
    "  hp 800\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Rifleman\"\r\n"
    "  id 100510\r\n"
    "  type person\r\n"
    "  graphic rifleman\r\n"
    "  sid s5rifle\r\n"
    "  hp 30\r\n"
    "  clipsize 30\r\n"
    "  deathtime 5\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Bunker\"\r\n"
    "  id 100520\r\n"
    "  type building\r\n"
    "  graphic bunker\r\n"
    "  sid s5bunker\r\n"
    "  husk bunk_husk\r\n"
    "  huskfinal bunk_burn\r\n"
    "  music 65533\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Bush\"\r\n"
    "  id 100530\r\n"
    "  type decoration\r\n"
    "  graphic bush\r\n"
    "  sid s5bush\r\n"
    "  hp 5\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Frag\"\r\n"
    "  id 100600\r\n"
    "  type powerup\r\n"
    "  graphic frag\r\n"
    "  sid s5frag\r\n"
    "  ai_function nade\r\n"
    "  move_function nade\r\n"
    "  hp 25\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 ATMine\"\r\n"
    "  id 100601\r\n"
    "  type powerup\r\n"
    "  graphic atmine\r\n"
    "  sid s5atmine\r\n"
    "  ai_function vmne\r\n"
    "  move_function schl\r\n"
    "  hp 10\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Dup A\"\r\n"
    "  id 100602\r\n"
    "  type powerup\r\n"
    "  graphic dupa\r\n"
    "  sid s5dupa\r\n"
    "  ai_function clym\r\n"
    "  move_function clym\r\n"
    "  hp 11\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Dup B\"\r\n"
    "  id 100602\r\n"
    "  type powerup\r\n"
    "  graphic dupb\r\n"
    "  sid s5dupb\r\n"
    "  ai_function nade\r\n"
    "  move_function nade\r\n"
    "  hp 12\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Crate\"\r\n"
    "  id 100540\r\n"
    "  type decoration\r\n"
    "  graphic crate\r\n"
    "  sid s5crate\r\n"
    "  ai_function gnrl\r\n"
    "  hp 104\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Oil Tank\"\r\n"
    "  id 100541\r\n"
    "  type decoration\r\n"
    "  graphic oiltank\r\n"
    "  sid s5oil\r\n"
    "  ai_function GNRC\r\n"
    "  hp 200\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Pump\"\r\n"
    "  id 100542\r\n"
    "  type decoration\r\n"
    "  graphic pump\r\n"
    "  sid s5pump\r\n"
    "  ai_function gnl2\r\n"
    "  hp 50\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Gun\"\r\n"
    "  id 100543\r\n"
    "  type decoration\r\n"
    "  graphic b50cal\r\n"
    "  sid s5gun\r\n"
    "  ai_function ewep\r\n"
    "  hp 10\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Palm\"\r\n"
    "  id 100544\r\n"
    "  type decoration\r\n"
    "  graphic palm\r\n"
    "  sid s5palm\r\n"
    "  ai_function tree\r\n"
    "  hp 10\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Radar\"\r\n"
    "  id 100545\r\n"
    "  type building\r\n"
    "  graphic radar\r\n"
    "  sid s5radar\r\n"
    "  ai_function bld2\r\n"
    "  hp 300\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Typo\"\r\n"
    "  id 100546\r\n"
    "  type decoration\r\n"
    "  graphic typo\r\n"
    "  sid s5typo\r\n"
    "  ai_function gnr1\r\n"
    "  hp 10\r\n"
    "end\r\n"
    "\r\n"
    // Tokens that only START with a physics row's name bind the null row
    // [orig: EntityDef_LookupPhysicsCallback @0x4a9240, whole-name stricmp].
    "begin \"S5 Prefix Tank\"\r\n"
    "  id 100504\r\n"
    "  type vehicle\r\n"
    "  graphic ptank\r\n"
    "  sid s5ptank\r\n"
    "  ai_function cvehX\r\n"
    "  render_function cvehX\r\n"
    "  move_function ctan\r\n"
    "  physics 2\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Prefix Helo\"\r\n"
    "  id 100505\r\n"
    "  type vehicle\r\n"
    "  graphic phelo\r\n"
    "  sid s5phelo\r\n"
    "  move_function CHelScout\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Mine Motor\"\r\n"
    "  id 100603\r\n"
    "  type powerup\r\n"
    "  graphic mmotor\r\n"
    "  sid s5mmotor\r\n"
    "  ai_function vmne\r\n"
    "  move_function vmne\r\n"
    "  hp 10\r\n"
    "end\r\n"
    "\r\n"
    // The sway renderer is the bone table's `tree` row; the table has no
    // `sway` row [orig: row 'tree' @0x82cf70 -> BoneCallback_Sway_World
    // @0x4e2b10].
    "begin \"S5 Sway Tree\"\r\n"
    "  id 100547\r\n"
    "  type decoration\r\n"
    "  graphic swaytree\r\n"
    "  sid s5swaytree\r\n"
    "  render_function tree\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Sway Tag\"\r\n"
    "  id 100548\r\n"
    "  type decoration\r\n"
    "  graphic swaytag\r\n"
    "  sid s5swaytag\r\n"
    "  render_function sway\r\n"
    "end\r\n"
    "\r\n"
    // The palm-state section draw is the render tag's psec / cesp bone row;
    // the palm event row writes +0x270 under any render tag and the
    // ai_function's psec row is the null callback [orig: rows 'psec'
    // @0x82d010 / 'cesp' @0x82d020 -> BoneCallback_psec_World @0x53c130;
    // event row 'psec' @0x813288].
    "begin \"S5 Psec Palm\"\r\n"
    "  id 100549\r\n"
    "  type decoration\r\n"
    "  graphic psecpalm\r\n"
    "  sid s5psecpalm\r\n"
    "  ai_function palm\r\n"
    "  render_function psec\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Cesp Tag\"\r\n"
    "  id 100550\r\n"
    "  type decoration\r\n"
    "  graphic cesptag\r\n"
    "  sid s5cesptag\r\n"
    "  render_function cesp\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Gnrc Palm\"\r\n"
    "  id 100551\r\n"
    "  type decoration\r\n"
    "  graphic gnrcpalm\r\n"
    "  sid s5gnrcpalm\r\n"
    "  ai_function palm\r\n"
    "  render_function gnrc\r\n"
    "end\r\n"
    "\r\n"
    "begin \"S5 Psec Event\"\r\n"
    "  id 100552\r\n"
    "  type decoration\r\n"
    "  graphic psecevent\r\n"
    "  sid s5psecevent\r\n"
    "  ai_function psec\r\n"
    "end\r\n";

// The minimal SndProf shape ("default" first, so a real profile lands at
// index 1 and a miss falls back to 0 by the table's own rule).
const char kProfiles[] =
    "begin \"default\"\r\n"
    "     SSLFootGND     DEF_FOOT_L\r\n"
    "end\r\n"
    "begin \"SP_Test\"\r\n"
    "     SSLFootGND     T_DIRT_L\r\n"
    "end\r\n"
    "begin \"SP_TestFemale\"\r\n"
    "     SSLFootGND     T_DIRT_L_F\r\n"
    "end\r\n";

// FIRST entry with the id — the row the fold resolves, so post-parse stamps
// land on the row the fold will read [orig: ItemList_FindIndexByTypeId
// @0x49E100 returns the first match].
DefItemDef *entry_for(DefItemsFile &f, int id) {
    for (size_t i = 0; i < f.count; ++i)
        if (f.entries[i].id == id) return &f.entries[i];
    return nullptr;
}

EntityHandle spawn(World &w, int pool, uint16_t item_id, EntityKind kind) {
    Entity e;
    e.kind = kind;
    e.item_id = item_id;
    e.health = 100;
    e.alive = true;
    return w.registry.spawn(pool, e);
}

// D-ITEMDEF-35: entity+0x30 is what the def's graphic load returned: null for
// a def that names none, for a .3di that fails to load and in a world with no
// model source, so a carrier whose graphic failed is refused admission through
// its loaded child's seat; the Player row's and a throwable class row's slots
// take the same load, and an organic takes its visual def's.
// [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 - the store @0x439FDC;
//  Game_ReloadEntityModelsAndCallbacks @0x522830 - the +0x30 stores @0x5229D7 /
//  @0x522B35 / @0x522CD1; Entity_InitFromItemDef @0x49E598; Entity_CanEnterVehicle
//  @0x435498..0x43549A]
void test_graphic_model_is_the_loads_result() {
    namespace fs = std::filesystem;
    const std::vector<uint8_t> mount = test_io::read_file(
            std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/mount.3di");
    if (mount.empty() || test_io::is_lfs_pointer(mount)) {
        std::printf("SKIP-LEG: needs the LFS fixture fixtures/threedi/synth/mount.3di\n");
        return;
    }
    const std::string root = std::string(test_paths_temp_dir()) + "/" +
            test_paths_unique("opennova_graphic_load");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    CHECK(test_io::write_file(root + "/LoadsHull.3di", mount));
    // Retail's head row; the Player row, its loading model; the visual row,
    // a lost one; a carrier whose graphic is lost and one whose loads; a row
    // with no graphic; a mine class row of each kind.
    static const char kDefs[] =
        "begin \"Null\"\r\n id 100000\r\n type marker\r\nend\r\n"
        "begin \"Player\"\r\n id 105305\r\n type person\r\n graphic LoadsHull\r\n hp 100\r\nend\r\n"
        "begin \"Player visual\"\r\n id 105310\r\n type person\r\n graphic LostBody\r\n hp 100\r\nend\r\n"
        "begin \"Hull lost\"\r\n id 100700\r\n type vehicle\r\n graphic LostHull\r\n hp 100\r\nend\r\n"
        "begin \"Hull kept\"\r\n id 100701\r\n type vehicle\r\n graphic LoadsHull\r\n hp 100\r\nend\r\n"
        "begin \"Bare\"\r\n id 100702\r\n type object\r\n hp 100\r\nend\r\n"
        "begin \"Mine lost\"\r\n id 100703\r\n type powerup\r\n graphic LostMine\r\n ai_function vmne\r\nend\r\n"
        "begin \"Mine kept\"\r\n id 100704\r\n type powerup\r\n graphic LoadsHull\r\n ai_function vmne\r\nend\r\n";
    DefItemsFile defs{};
    CHECK(def_parse_items_memory(reinterpret_cast<const uint8_t *>(kDefs),
            sizeof(kDefs) - 1, &defs) == 0);
    {
        ResourceIndex index;
        CHECK(index.scan(root, std::string(), VfsMountMode::LooseOnly));
        assets::AssetStore models{&index};
        auto wp = std::make_unique<World>();
        World &w = *wp;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 8);
        const EntityHandle lost_h = spawn(w, 1, 700, EntityKind::Item);
        const EntityHandle kept_h = spawn(w, 1, 701, EntityKind::Item);
        const EntityHandle bare_h = spawn(w, 1, 702, EntityKind::Item);
        const EntityHandle body_h = spawn(w, 0, 5305, EntityKind::Organic);
        // Each carrier's ground-attached child has its def, its model and a seat.
        for (const EntityHandle carrier : {lost_h, kept_h}) {
            Entity child;
            child.kind = EntityKind::Item;
            child.item_id = 701;
            child.health = 100;
            child.alive = true;
            child.ground_target = carrier;
            Seat sit;
            sit.type = SeatType::Passenger;
            sit.bone_index = 1;
            child.seats.push_back(sit);
            w.registry.spawn(1, child);
            Entity &c = *w.registry.get(carrier);
            c.spawn_position = c.position; // at its spawn anchor
        }
        mission::resolve_item_traits(w, defs, {}, &models);
        CHECK(!w.registry.get(lost_h)->has_graphic_model);
        CHECK(w.registry.get(kept_h)->has_graphic_model);
        CHECK(!w.registry.get(bare_h)->has_graphic_model);
        CHECK(w.registry.get(body_h)->has_graphic_model); // its runtime row's
        CHECK(w.tables.player.has_graphic_model);
        const ThrowableClassRow *mine_lost = w.throwables.classes.get(703);
        const ThrowableClassRow *mine_kept = w.throwables.classes.get(704);
        CHECK(mine_lost != nullptr && !mine_lost->has_graphic_model);
        CHECK(mine_kept != nullptr && mine_kept->has_graphic_model);
        // The admission refuses the carrier whose graphic failed before its walk
        // reaches the child's seat.
        CHECK(!vehicle_can_enter(w, nullptr, *w.registry.get(lost_h)));
        CHECK(vehicle_can_enter(w, nullptr, *w.registry.get(kept_h)));
        // The collision sweep's load is the organic's visual def's, lost here.
        world::EntityPoseProvider pose;
        pose.set_assets(&models);
        CollisionWorld collision;
        OcclusionWorld occlusion;
        mission::CollisionResolveState state;
        const mission::CollisionResolveDeps deps{collision, occlusion, pose, models};
        mission::resolve_collision_instances(w, defs, state, deps);
        CHECK(!w.registry.get(body_h)->has_graphic_model);
        CHECK(!w.registry.get(lost_h)->has_graphic_model);
        CHECK(w.registry.get(kept_h)->has_graphic_model);
    }
    {
        // No model source loads none: a null store, and one over no mount.
        ResourceIndex unmounted;
        const assets::AssetStore sourceless{&unmounted};
        for (const assets::AssetStore *models : {static_cast<const assets::AssetStore *>(nullptr),
                     &sourceless}) {
            World w;
            w.registry.configure_pool(1, 4);
            const EntityHandle kept_h = spawn(w, 1, 701, EntityKind::Item);
            mission::resolve_item_traits(w, defs, {}, models);
            CHECK(!w.registry.get(kept_h)->has_graphic_model);
            CHECK(!w.tables.player.has_graphic_model);
        }
    }
    def_free_items(&defs);
    fs::remove_all(root, ec);
}

} // namespace

int main() {
    DefItemsFile file = {};
    CHECK(def_parse_items_memory(
                  reinterpret_cast<const uint8_t *>(kItemsDef),
                  sizeof(kItemsDef) - 1, &file) == 0);
    CHECK(file.count == 28);

    // Stamp the fields whose authored-token spellings are the def parser's own
    // test surface: distinct sentinels per vehicle-physics slot so any
    // field-order transposition in the fold fails loudly.
    DefItemDef *player = entry_for(file, 105305);
    DefItemDef *tank = entry_for(file, 100500);
    DefItemDef *rifle = entry_for(file, 100510);
    DefItemDef *bunker = entry_for(file, 100520);
    DefItemDef *bush = entry_for(file, 100530);
    CHECK(player && tank && rifle && bunker && bush);
    bush->attrib |= DEF_ITEM_ATTRIB_NOTARGET; // as items.def 101435 "Metal 50gal Drum"
    DefItemDef *mine_def = entry_for(file, 100601);
    CHECK(mine_def != nullptr);
    if (mine_def != nullptr) { // as items.def 101891 "Satchel Charge"
        mine_def->attrib |= DEF_ITEM_ATTRIB_NOTARGET;
        mine_def->radar_sig = 40;
        mine_def->heat_sig = 30;
    }
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
    tank->armor_impact = 7; // a def with hp keeps its authored armor words
    tank->armor_blast = 8;
    tank->armor_kz = 8;
    tank->attrib_parent = 1; // the `Parent` byte (ItemDef+0x548)
    std::strcpy(tank->particlefxw3.effect, "fx_sml_wk");
    std::strcpy(tank->particlefxw3.userpoint, "FX00");
    std::strcpy(tank->particlefxw4.effect, "fx_sml_wk_f");
    std::strcpy(tank->particlefxw4.userpoint, "FX01");
    std::strcpy(tank->sound_profile, "SP_Tank");
    std::strcpy(tank->soundloops[0], "LP_TANK");
    // Four addeweap slots, the fourth authoring an arc (down 70, up 10, right
    // 30, left 30 degrees, stored as the parser scales them), and a daylight
    // transfer: the tables the subType -1 read lands on.
    tank->light_transfer = 0.2f;
    tank->emplacement_attachments = static_cast<DefItemEmplacementAttachment *>(
            std::calloc(4, sizeof(DefItemEmplacementAttachment)));
    tank->emplacement_attachments_count = 4;
    tank->emplacement_attachments[3].down_angle = 70 * 11930464;
    tank->emplacement_attachments[3].up_angle = -10 * 11930464;
    tank->emplacement_attachments[3].right_angle = 30 * 11930464;
    tank->emplacement_attachments[3].left_angle = -30 * 11930464;
    tank->emplacement_attachments[3].angle_count = 4;
    std::strcpy(rifle->ammo_closeattack, "at_rifle");
    std::strcpy(rifle->ammo_easyrocket, "AT_EASY");
    std::strcpy(rifle->ammo_advancedrocket, "AT_ADVANCED");
    std::strcpy(rifle->ammo_marker3, "AT_MARKER");
    std::strcpy(rifle->sound_profile, "SP_Test");
    std::strcpy(rifle->sound_profile_female, "SP_TestFemale");
    rifle->attrib |= DEF_ITEM_ATTRIB_LEAVECORPSE;
    rifle->score = 10; // the shipped soldiers author `score 10`
    bunker->attrib |= DEF_ITEM_ATTRIB_CHANGETEAM | DEF_ITEM_ATTRIB_SPAWNPOINT |
            DEF_ITEM_ATTRIB_NODIE | DEF_ITEM_ATTRIB_SD; // S&D = the objective target's team-protect
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
    w.registry.configure_pool(1, 10); // items/vehicles
    w.registry.configure_pool(2, 16); // buildings (bunker, bush, unknown, the 7 class rows, the 2 sway rows + the 4 section rows)
    const EntityHandle tank_h = spawn(w, 1, 500, EntityKind::Item);
    const EntityHandle apc_h = spawn(w, 1, 501, EntityKind::Item);
    const EntityHandle helo_h = spawn(w, 1, 502, EntityKind::Item);
    const EntityHandle truck_h = spawn(w, 1, 503, EntityKind::Item);
    const EntityHandle prefix_tank_h = spawn(w, 1, 504, EntityKind::Item);
    const EntityHandle prefix_helo_h = spawn(w, 1, 505, EntityKind::Item);
    const EntityHandle dup_h = spawn(w, 1, 602, EntityKind::Item); // "S5 Dup A" then "S5 Dup B"
    const EntityHandle rifle_h = spawn(w, 0, 510, EntityKind::Organic);
    const EntityHandle player_h = spawn(w, 0, 5305, EntityKind::Organic);
    const EntityHandle bunker_h = spawn(w, 2, 520, EntityKind::Building);
    const EntityHandle bush_h = spawn(w, 2, 530, EntityKind::Building);
    const EntityHandle unknown_h = spawn(w, 2, 999, EntityKind::Building);
    // The hp-0 def as an addeweap child on the tank's second slot, its anchor
    // resolved at promotion.
    const EntityHandle shield_h = spawn(w, 1, 520, EntityKind::Item);
    if (Entity *shield = w.registry.get(shield_h)) {
        shield->emplacement_parent = tank_h;
        shield->emplacement_parent_spawn_id = w.registry.get(tank_h)->registry_spawn_id;
        shield->emplacement_pose_metadata_resolved = true;
        shield->emplacement_slot = 2;
        shield->sub_type = 1;
        shield->emplacement_bone = 4;
        shield->emplacement_anchor_subobject = 0;
        shield->emplacement_local = {2.f, 0.f, 1.f};
        shield->emplacement_yaw_offset = 30;
        shield->emplacement_attachment_flags = 1;
    }
    // The ai_function class rows (one entity each; only the death-trait row
    // is read back).
    for (uint16_t id = 540; id <= 546; ++id) spawn(w, 2, id, EntityKind::Building);
    const EntityHandle sway_tree_h = spawn(w, 2, 547, EntityKind::Building);
    const EntityHandle sway_tag_h = spawn(w, 2, 548, EntityKind::Building);
    const EntityHandle psec_palm_h = spawn(w, 2, 549, EntityKind::Building);
    const EntityHandle cesp_tag_h = spawn(w, 2, 550, EntityKind::Building);
    const EntityHandle gnrc_palm_h = spawn(w, 2, 551, EntityKind::Building);
    const EntityHandle psec_event_h = spawn(w, 2, 552, EntityKind::Building);

    // The wire-class supplier stub: the fold must stamp the returned byte
    // verbatim and default a functor miss to 0 (Unknown, fail closed).
    const mission::ItemWireClassFn wire_class = [](int def_id) -> uint8_t {
        if (def_id == 100500) return 3; // Vehicle
        if (def_id == 100510) return 2; // Infantry
        return 0;
    };
    mission::resolve_item_traits(w, file, wire_class, nullptr);

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
    // The victim's kill value rides the entity; the Player def authors none.
    // [orig: Score_ProcessKillEvent @0x4FD400 (the def+0x194 read @0x4FD422)]
    CHECK(rifle_e->item_score == 10);
    CHECK(w.registry.get(player_h) != nullptr && w.registry.get(player_h)->item_score == 0);

    const Entity *bunker_e = w.registry.get(bunker_h);
    CHECK(bunker_e != nullptr);
    // def hp 0 => indestructible flags [orig: Entity_InitFromModel @0x40dc8e].
    CHECK((bunker_e->engine_flags & 0x4000000u) != 0);
    CHECK(bunker_e->sub_type == 0xFF);
    // hp 0 lifts nothing; the init writes Health 1 [orig: @0x40DCA6].
    CHECK(bunker_e->health == 1);
    CHECK(bunker_e->music_location == -3); // parsed signed word survives trait promotion
    CHECK(bunker_e->is_capture_trigger);
    CHECK(bunker_e->is_spawn_point);
    // The authored 12/34 does not survive: an hp-0 def's two armor words
    // become the invulnerable 0xFFFF [orig: Entity_InitFromModel @0x40DC95 /
    // @0x40DC9F], the pair the AI target walk skips [orig: Entity_FindTargets
    // @0x53AC3F..0x53AC59].
    CHECK(bunker_e->armor_impact == -1);
    CHECK(bunker_e->armor_kz == -1);
    const Entity *tank_armor = w.registry.get(tank_h);
    CHECK(tank_armor != nullptr && tank_armor->armor_impact == 7 && tank_armor->armor_kz == 8);
    CHECK(bunker_e->deathtime_ticks == 0);

    // As an addeweap child, the same def's subType 0xFF lands before the ewep
    // class init reads its anchor at subType -1: no userpoint, no designation,
    // so it rides its carrier's root. [orig: Entity_SpawnWeaponOverlays
    //  @0x40F40E -> Entity_InitFromModel @0x40DCAF -> Entity_InitBoneReferences
    //  @0x4415F1]
    const Entity *shield_e = w.registry.get(shield_h);
    CHECK(shield_e != nullptr && shield_e->sub_type == 0xFF);
    CHECK(shield_e != nullptr && shield_e->emplacement_bone == 0 &&
          shield_e->emplacement_anchor_subobject == -1 &&
          shield_e->emplacement_local.x == 0.f && shield_e->emplacement_local.z == 0.f &&
          shield_e->emplacement_yaw_offset == 0 &&
          shield_e->emplacement_attachment_flags == 0 &&
          shield_e->emplacement_pose_metadata_resolved);
    // Its turret window reads the tank def's four slot tables at -1: down takes
    // light_transfer's float bits (0.2f), up slot 4's down, right slot 4's up and
    // left slot 4's right. [orig: Entity_GetWeaponTurretLimits @0x540DBB..0x540E15]
    CHECK(shield_e != nullptr && shield_e->emplacement_down_limit_bam == 0x3E4CCCCD &&
          shield_e->emplacement_up_limit_bam == 70 * 11930464 &&
          shield_e->emplacement_right_limit_bam == -10 * 11930464 &&
          shield_e->emplacement_left_limit_bam == 30 * 11930464);

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

    // A NoTarget def (attrib 0x100) ORs Flags 0x8000000 at the model init,
    // whatever its type or hp; no other def does, and neither does a missing
    // one. The load stream carries the bit: the bush's S2C 0x10 record streams
    // it in the Flags dword (the record sits at its pool-2 slot).
    // [orig: Entity_InitFromModel @0x40DC3B / @0x40DC4F;
    //  NetPacket_SerializePool2StaticToBuffer @0x5044E6]
    const Entity *bush_e = w.registry.get(bush_h);
    CHECK(bush_e != nullptr && (bush_e->engine_flags & kEntityFlagNoTarget) != 0);
    CHECK((bunker_e->engine_flags & kEntityFlagNoTarget) == 0);
    CHECK((tank_e->engine_flags & kEntityFlagNoTarget) == 0);
    CHECK((unknown_e->engine_flags & kEntityFlagNoTarget) == 0);
    {
        const StaticEntityBatch batch = replication::build_pool2_static_batch(w);
        const size_t bush_slot = static_cast<size_t>(bush_h.slot());
        const size_t bunker_slot = static_cast<size_t>(bunker_h.slot());
        CHECK(bush_slot < batch.records.size() && bunker_slot < batch.records.size());
        if (bush_slot < batch.records.size() && bunker_slot < batch.records.size()) {
            CHECK(batch.records[bush_slot].item_type_id == 530);
            CHECK((batch.records[bush_slot].entity_flags & 0x8000000u) != 0);
            CHECK((batch.records[bunker_slot].entity_flags & 0x8000000u) == 0);
        }
    }

    // ---- the death-trait table ----
    const ItemDeathTraits *bt = w.tables.item_death_traits.get(520);
    CHECK(bt != nullptr);
    if (bt != nullptr) {
        CHECK(bt->unit_type == 5);
        CHECK(bt->kz == 6.5f);
        CHECK(bt->armor_impact == -1); // the def words the damage gates read
        CHECK(bt->armor_blast == -1);
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
    CHECK(tankt != nullptr && tankt->armor_impact == 7 && tankt->armor_blast == 8);

    // ---- the event/death callback row (D-ITEM-7) ----
    // The fold resolves the ai_function tag the way retail's whole-string
    // stricmp walk of g_EntityClassEventCallbackTable @0x813000 does: a
    // mixed-case tag matches, the absent tag / a tag without a row (the
    // shipped "gnr1" typo) / the callback-less nade row resolve the null row
    // (0x406FF0, never dies), and bld2 resolves its collapse callback. [orig:
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
    CHECK(class_of(545) == ItemDeathClass::kCollapsingBuilding);
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
    CHECK(item_death_class_from_tag("towr") == ItemDeathClass::kTower);
    CHECK(item_death_class_from_tag("emit") == ItemDeathClass::kEmitter);
    // The missile/artillery rows share the null row's body [orig: MissileClass_HandleEvent
    // @0x443630 / ArtyClass_HandleEvent @0x443640 == 0x406FF0], the flare rows are a bare
    // retn [orig: AflrClass_HandleEvent @0x443650 / GflrClass_HandleEvent @0x443660] — neither is the
    // tree body.
    CHECK(item_death_class_from_tag("rokt") == ItemDeathClass::kNull);
    CHECK(item_death_class_from_tag("stng") == ItemDeathClass::kNull);
    CHECK(item_death_class_from_tag("hlfr") == ItemDeathClass::kNull);
    CHECK(item_death_class_from_tag("jvln") == ItemDeathClass::kNull);
    CHECK(item_death_class_from_tag("arty") == ItemDeathClass::kNull);
    CHECK(item_death_class_from_tag("aflr") == ItemDeathClass::kNone);
    CHECK(item_death_class_from_tag("gflr") == ItemDeathClass::kNone);

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
        // ctank is the table's own 5-character row -> the tank mover
        // [orig: row @0x82acac ctank -> @0x48f000].
        CHECK(vt->family == VehicleFamily::Tank);
		CHECK(vt->render_family == VehicleRenderFamily::Ground); // render cveh, move ctank
		CHECK(vt->player_control);
		CHECK(vt->attrib_parent); // the Parent byte feeds the gunner-attachment gate
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
        CHECK(!apc_vt->attrib_parent);
    }
    // CHel rows legitimately omit the ground physics selector and still land
    // (direct air mover).
    const VehicleTraits *helo_vt = w.vehicles.traits.get(502);
    CHECK(helo_vt != nullptr);
    if (helo_vt != nullptr) {
        CHECK(helo_vt->family == VehicleFamily::Helicopter);
        CHECK(helo_vt->physics == 0);
        // The ai_function binds the CHel event row in any case; the render
        // tag compares exactly, so `CHel` is not the lowercase chel bone row.
        // [orig: Entity_LookupRenderCallbacks stricmp @0x407de2;
        //  BoneCallback_LookupByTag @0x4e32c6, row 'chel' @0x82cfd0]
        CHECK(helo_vt->brain_class == VehicleBrainClass::Air);
        CHECK(helo_vt->render_family == VehicleRenderFamily::None);
    }
    // A token that only starts with a row's name binds the null row: `ctan`
    // keeps its physics selector's ground mover, never the tank's, and
    // `CHelScout` without a selector is no vehicle at all.
    // [orig: EntityDef_LookupPhysicsCallback @0x4a9240, stricmp @0x4a9262,
    //  row 0 @0x4a9272]
    const VehicleTraits *prefix_tank_vt = w.vehicles.traits.get(504);
    CHECK(prefix_tank_vt != nullptr);
    if (prefix_tank_vt != nullptr) {
        CHECK(prefix_tank_vt->family == VehicleFamily::Ground);
        CHECK(!prefix_tank_vt->amphibian);
        // `cvehX`: the event row is whole-name (no brain), while the render
        // tag is the first four characters (the cveh bone row).
        // [orig: Entity_LookupRenderCallbacks stricmp @0x407de2; the tag
        //  packed from def+0x13C..0x13F @0x4a5ace..0x4a5af1]
        CHECK(prefix_tank_vt->brain_class == VehicleBrainClass::Unset);
        CHECK(prefix_tank_vt->render_family == VehicleRenderFamily::Ground);
    }
    CHECK(w.vehicles.traits.get(505) == nullptr);
    (void)prefix_tank_h;
    (void)prefix_helo_h;
	// Ground selector zero is a real motor, as used by the shipped LCAC.
	const VehicleTraits *simple_vt = w.vehicles.traits.get(503);
	CHECK(simple_vt != nullptr);
	if (simple_vt != nullptr)
		CHECK(simple_vt->physics == 0);
	(void)apc_h;
	(void)helo_h;
	(void)truck_h;
	(void)player_h;

	// The sway renderer binds through the render_function's `tree` bone row;
	// a `sway` tag finds no row and takes row 0's generic callbacks.
	// [orig: BoneCallback_LookupByTag @0x4e32c6, row 'tree' @0x82cf70 ->
	//  BoneCallback_Sway_World @0x4e2b10; the miss @0x4e32d4]
	{
		const Entity *sway_tree = w.registry.get(sway_tree_h);
		const Entity *sway_tag = w.registry.get(sway_tag_h);
		CHECK(sway_tree != nullptr && sway_tree->render_sway);
		CHECK(sway_tag != nullptr && !sway_tag->render_sway);
	}

	// The palm-state (+0x270) section draw binds through the render tag's
	// psec / cesp bone rows alone: a palm event row under the gnrc tag and
	// the ai_function's psec row draw no palm state.
	// [orig: rows 'psec' @0x82d010 / 'cesp' @0x82d020 -> BoneCallback_psec_World
	//  @0x53c130 -> CTerrainMap_BuildSectorTransformMatrices @0x53bf10;
	//  BoneCallback_gnrc_World @0x4e2860; event row 'psec' @0x813288]
	{
		const Entity *psec_palm = w.registry.get(psec_palm_h);
		const Entity *cesp_tag = w.registry.get(cesp_tag_h);
		const Entity *gnrc_palm = w.registry.get(gnrc_palm_h);
		const Entity *psec_event = w.registry.get(psec_event_h);
		CHECK(psec_palm != nullptr && psec_palm->palm_sections);
		CHECK(cesp_tag != nullptr && cesp_tag->palm_sections);
		CHECK(gnrc_palm != nullptr && !gnrc_palm->palm_sections);
		CHECK(psec_event != nullptr && !psec_event->palm_sections);
		// The pool serializer still streams +0x270 for the palm event row.
		CHECK(gnrc_palm != nullptr && gnrc_palm->palm_state_streamed);
		CHECK(cesp_tag != nullptr && !cesp_tag->palm_state_streamed);
	}

	// A def resweep recomputes the draw both ways: the psec item edited to
	// gnrc draws every section, the gnrc palm edited to psec draws the palm
	// state. A palm fragment's draw is its def's too: its spawn stores the
	// sector builder into the model's collision and scar callbacks only, and
	// that flag survives the resolve (D-ITEMDEF-20). A psec draw hides the
	// sector builder's sections alone, never the section or piece masks.
	// [orig: Palm_SpawnFragment @0x53c3b0 / @0x53c3c6; Render_SectorEntity
	//  @0x5c431b..0x5c4372; BoneCallback_psec_World @0x53c130]
	{
		DefItemDef *psec_def = entry_for(file, 100549);
		DefItemDef *gnrc_def = entry_for(file, 100551);
		CHECK(psec_def != nullptr && gnrc_def != nullptr);
		Entity *fragment = w.registry.get(psec_event_h);
		if (psec_def != nullptr && gnrc_def != nullptr && fragment != nullptr) {
			std::snprintf(psec_def->render_function, sizeof(psec_def->render_function), "gnrc");
			std::snprintf(gnrc_def->render_function, sizeof(gnrc_def->render_function), "psec");
			fragment->palm_fragment = true;
			mission::resolve_item_traits(w, file, wire_class, nullptr);
			CHECK(!w.registry.get(psec_palm_h)->palm_sections);
			CHECK(w.registry.get(gnrc_palm_h)->palm_sections);
			// ai_function psec, no section tag: no palm draw, the store kept.
			CHECK(!fragment->palm_sections && fragment->palm_fragment);
			Entity *palm = w.registry.get(psec_palm_h);
			palm->palm_state = 0;
			CHECK(item_hidden_sections(*palm) == palm->section_mask);
			std::snprintf(psec_def->render_function, sizeof(psec_def->render_function), "psec");
			std::snprintf(gnrc_def->render_function, sizeof(gnrc_def->render_function), "gnrc");
			fragment->palm_fragment = false;
			mission::resolve_item_traits(w, file, wire_class, nullptr);
			CHECK(w.registry.get(psec_palm_h)->palm_sections);
			CHECK(!w.registry.get(gnrc_palm_h)->palm_sections);
			CHECK(!fragment->palm_sections);
			palm = w.registry.get(psec_palm_h);
			palm->section_mask = 0x1;
			palm->engine_flags |= kEntityFlagHusk;
			palm->spawned_piece_mask = 0x2;
			CHECK(item_hidden_sections(*palm) == 0x38); // palm state 0: 3,4,5
			palm->engine_flags &= ~kEntityFlagHusk;
			palm->section_mask = palm->spawned_piece_mask = 0;
		}
	}

	// The death-section draw binds through the render tag's cveh, chel, tank
	// and cpln bone rows, each by its first four bytes with their case kept:
	// `cvehX` is the cveh row, `CHel`, `psec` and `bldg` are none of them, and
	// a def edited to `Cveh` or `TANK` loses the draw on the next resolve.
	// [orig: rows 'cveh' @0x82cfe0 -> Entity_BuildDeathSectionTransforms
	//  @0x492af0, 'chel' @0x82cfd0 -> Entity_BuildAndApplyBoneMatrices @0x48f210,
	//  'tank' @0x82cff0 -> Render_DestroyedSections @0x449ef0, 'cpln' @0x82d000
	//  -> BoneCallback_cpln_World @0x48fe90; BoneCallback_LookupByTag @0x4e32c6]
	{
		CHECK(w.registry.get(tank_h)->death_section_pivot);
		CHECK(w.registry.get(prefix_tank_h)->death_section_pivot);
		CHECK(!w.registry.get(helo_h)->death_section_pivot);
		CHECK(!w.registry.get(psec_palm_h)->death_section_pivot);
		DefItemDef *truck_def = entry_for(file, 100503);
		CHECK(truck_def != nullptr);
		if (truck_def != nullptr) {
			// The bldg row's section offset binds the same way: `bldg` and
			// `bldgX` are its row, `Bldg` and `BLDG` none.
			// [orig: row 'bldg' @0x82cf10 -> BoneCallback_bldg_World @0x4e21a0]
			const struct { const char *tag; bool pivots; bool bldg; } tags[] = {
				{"chel", true, false}, {"tank", true, false}, {"cpln", true, false},
				{"cplnX", true, false}, {"CHel", false, false}, {"Cveh", false, false},
				{"TANK", false, false}, {"bldg", false, true}, {"bldgX", false, true},
				{"Bldg", false, false}, {"BLDG", false, false},
			};
			for (const auto &tag : tags) {
				std::snprintf(truck_def->render_function, sizeof(truck_def->render_function),
						"%s", tag.tag);
				mission::resolve_item_traits(w, file, wire_class, nullptr);
				CHECK(w.registry.get(truck_h)->death_section_pivot == tag.pivots);
				CHECK(w.registry.get(truck_h)->bldg_section_pivot == tag.bldg);
			}
			std::snprintf(truck_def->render_function, sizeof(truck_def->render_function), "cveh");
			mission::resolve_item_traits(w, file, wire_class, nullptr);
			CHECK(w.registry.get(truck_h)->death_section_pivot);
			CHECK(!w.registry.get(truck_h)->bldg_section_pivot);
		}
	}

	// ---- the throwable class scan ----
    const ThrowableClassRow *frag = w.throwables.classes.get(600);
    CHECK(frag != nullptr);
    if (frag != nullptr) {
        CHECK(frag->think == ThrowClass::kNade);
        CHECK(frag->motor == ThrowClass::kNade);
        CHECK(frag->health_max == 25);
        // The row's ordinal, the placed device's ItemTypeIndex (S5 Frag is row 8)
        // [orig: Entity_CloneFromTemplateByType @0x4398A0].
        CHECK(frag->item_type_index == 8);
    }
    const ThrowableClassRow *mine = w.throwables.classes.get(601);
    CHECK(mine != nullptr);
    if (mine != nullptr) {
        CHECK(mine->think == ThrowClass::kAVMine);
        CHECK(mine->motor == ThrowClass::kSatchel);
        // The placed entity's model init reads the def's NoTarget bit
        // [orig: Entity_ConvertRoundToPlacedEntity @0x5455B0, the
        //  Entity_InitFromModel call @0x5455D8].
        CHECK(mine->no_target);
        // ...and its type and signatures (the matrix-built leg, the sig
        // copy) [orig: Entity_InitFromModel @0x40E0D7, @0x40E136..0x40E156].
        CHECK(mine->def_type == DEF_ITEM_TYPE_OBJECT); // `type powerup`, the same 6
        CHECK(mine->radar_sig == 40);
        CHECK(mine->heat_sig == 30);
        // ...and the attrib words and graphic the conversion's copy of the
        // round carries into the device [orig: Entity_ConvertRoundToPlacedEntity
        // @0x5455D6].
        CHECK(mine->attrib == DEF_ITEM_ATTRIB_NOTARGET);
        CHECK(mine->attrib2 == 0);
        // Its graphic, atmine, loads from no model source here (the load
        // itself: test_graphic_model_is_the_loads_result).
        CHECK(!mine->has_graphic_model);
    }
    CHECK(frag == nullptr || !frag->no_target);
    // vmne is an event row only: as a move_function it binds the physics
    // table's null row, so the device has its think and no motor.
    // [orig: EntityDef_LookupPhysicsCallback @0x4a9240 over @0x82abc8]
    const ThrowableClassRow *mine_motor = w.throwables.classes.get(603);
    CHECK(mine_motor != nullptr);
    if (mine_motor != nullptr) {
        CHECK(mine_motor->think == ThrowClass::kAVMine);
        CHECK(mine_motor->motor == ThrowClass::kNone);
    }
    // A duplicate definition id resolves to its FIRST row (the earlier clym
    // block), for the class tables and for a placed entity alike: the later
    // row is never reached. [orig: ItemList_FindIndexByTypeId @0x49E100 —
    // `cmp [ecx],esi; jz` @0x49E120..0x49E122 returns the first hit]
    const ThrowableClassRow *dup = w.throwables.classes.get(602);
    CHECK(dup != nullptr);
    if (dup != nullptr) {
        CHECK(dup->think == ThrowClass::kClaymore);
        CHECK(dup->motor == ThrowClass::kClaymore);
        CHECK(dup->health_max == 11);
        CHECK(dup->item_type_index == 10); // "S5 Dup A", the first 100602 row
    }
    const Entity *dup_e = w.registry.get(dup_h);
    CHECK(dup_e != nullptr && dup_e->health_max == 11);
    CHECK(dup_e != nullptr &&
          dup_e->item_type_index == static_cast<int32_t>(entry_for(file, 100602) - file.entries));
    CHECK(w.throwables.classes.get(500) == nullptr);

    // Idempotent re-run: the once-per-id tables must not duplicate or reset.
    mission::resolve_item_traits(w, file, wire_class, nullptr);
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

    CHECK(mission::resolve_ai_weapons(w, file) == 1);
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

    // ---- the vehicle class init's ammo copy, gated on the resolved byte ----
    // A resolved block seeds its count; a miss and the null AT_NULL row (the
    // lookup's 0) seed zero over promote's stand-in capacities.
    // [orig: Entity_InitVehicleAIFromDef @0x468882..0x4688B7]
    {
        AiEntity *tank_b = ai.at(ai.attach(tank_h));
        CHECK(tank_b != nullptr);
        if (tank_b != nullptr) {
            tank_b->profile.fire_a.ammo_name = "AT_RIFLE";
            tank_b->profile.fire_a.ammo_cap = 12;
            tank_b->profile.fire_b.ammo_name = "AT_BOGUS";
            tank_b->profile.fire_b.ammo_cap = 9;
            tank_b->brain.f[AiBrain::kAmmoA] = 12;
            tank_b->brain.f[AiBrain::kAmmoB] = 9;
            mission::resolve_ai_weapons(w, file, tank_h);
            CHECK(tank_b->brain.f[AiBrain::kAmmoA] == 12);
            CHECK(tank_b->brain.f[AiBrain::kAmmoB] == 0);
            tank_b->profile.fire_a.ammo_name = "AT_NULL";
            mission::resolve_ai_weapons(w, file, tank_h);
            CHECK(tank_b->brain.f[AiBrain::kAmmoA] == 0);
        }
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
            mission::resolve_item_traits(w, file, wire_class, nullptr);
            rifle_override = w.registry.get(rifle_h);
            CHECK(rifle_override != nullptr && rifle_override->item_attrib == authored);
            CHECK(rifle_override != nullptr && rifle_override->item_attrib2 == 0u);
            CHECK(rifle_override != nullptr && !rifle_override->is_spawn_point);
            CHECK(rifle_override != nullptr && rifle_override->leave_corpse);
        }
    }

    // ---- the lift leaves a player body alone (D-PWR-9) ----
    // Retail's lift is the mission start's reload over the loaded pools, ahead
    // of the round init's player spawn; a player's health is its spawn's raises
    // to the ceiling. A training start at the difficulty word 1 over a Player
    // def hp of 200 spawns at exactly 100 and keeps it through the boot's later
    // sweeps, while a non-player body of the same row at 100 still lifts.
    // [orig: Game_ReloadEntityModelsAndCallbacks @0x522830, called @0x52550E;
    //  Server_InitAllPlayerEntitiesForRound called @0x525BAF;
    //  Entity_SpawnFromAnimSlotProperty @0x43C557..0x43C56B]
    {
        player->hp = 200;
        World sp;
        sp.registry.configure_pool(0, 8);
        sp.rules.mp_session = false;
        sp.rules.difficulty = 1;
        mission::resolve_item_traits(sp, file, wire_class, nullptr); // the Player row's word
        CHECK(sp.tables.player.item_hp == 200);
        PlayerSpawn ps;
        ps.net_id = 0xFFF0;
        const EntityHandle body_h = spawn_player(sp, ps);
        const EntityHandle npc_h = spawn(sp, 0, 5305, EntityKind::Organic);
        const Entity *body = sp.registry.get(body_h);
        CHECK(body != nullptr && sp.cached.local_player == body_h);
        CHECK(body != nullptr && body->health == 100); // halved at the word 1
        CHECK(body != nullptr && body->health_max == 200);
        mission::resolve_item_traits(sp, file, wire_class, nullptr); // the kernel's item_traits step
        body = sp.registry.get(body_h);
        CHECK(body != nullptr && body->health == 100);
        CHECK(body != nullptr && body->health_max == 200);
        const Entity *npc = sp.registry.get(npc_h);
        CHECK(npc != nullptr && npc->health == 200);
        CHECK(npc != nullptr && npc->health_max == 200);
        player->hp = 100;
    }

    def_free_items(&file);
    test_graphic_model_is_the_loads_result();
    if (failures == 0) std::printf("mission_item_traits: OK\n");
    return failures == 0 ? 0 : 1;
}
