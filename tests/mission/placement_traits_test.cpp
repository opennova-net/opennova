// Placement render-policy predicates (engine/runtime/mission/placement_traits.h):
// the witnessed shadow and water-mirror admission rules, pinned engine-side so
// the Godot placer tests exercise wiring, not policy.
// [orig: Entity_InitFromModel @ 0x40e1bc..0x40e236 (dynamic slots + vehicle
//  reflect flag); Terrain_CollectAndRenderTileModels @ 0x60d421..0x60d463
//  (static eligibility); Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b
//  (BMS attrib 0x800000 -> entity flags 0x400);
//  Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0 (mask 0x400 above
//  water applied by every collector, buildings included).]
#include <cstdio>

#include <runtime/mission/placement_traits.h>

using namespace opennova::mission;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static void test_dynamic_shadow_admission() {
    // Persons always; otherwise the item attrib2 dynamic-shadow bit.
    CHECK(item_casts_dynamic_shadow(kItemTypePerson, 0));
    CHECK(item_casts_dynamic_shadow(kItemTypeVehicle, kItemAttrib2DynamicShadow));
    CHECK(!item_casts_dynamic_shadow(kItemTypeVehicle, 0));
    CHECK(!item_casts_dynamic_shadow(kItemTypeBuilding, kItemAttrib2StaticShadow));
}

static void test_static_shadow_admission() {
    // Pool-2 buildings admitted unless a NoShadow veto; pool-1 items need the
    // attrib2 StaticShadow bit; both veto bits apply everywhere.
    CHECK(item_casts_static_terrain_shadow(kEntityKindBuilding, 0, 0, 0));
    CHECK(!item_casts_static_terrain_shadow(kEntityKindBuilding,
            kEntityAttribNoShadow, 0, 0));
    CHECK(!item_casts_static_terrain_shadow(kEntityKindBuilding, 0,
            kItemAttribNoShadow, 0));
    CHECK(!item_casts_static_terrain_shadow(kEntityKindItem, 0, 0, 0));
    CHECK(item_casts_static_terrain_shadow(kEntityKindItem, 0, 0,
            kItemAttrib2StaticShadow));
    CHECK(!item_casts_static_terrain_shadow(kEntityKindItem,
            kEntityAttribNoShadow, 0, kItemAttrib2StaticShadow));
}

static void test_mirror_admission() {
    // Vehicles by item type; any pool via the authored BMS Reflective
    // attribute; nothing else.
    CHECK(item_is_mirror_reflected(kItemTypeVehicle));
    CHECK(!item_is_mirror_reflected(kItemTypeBuilding));
    CHECK(!item_is_mirror_reflected(kItemTypePerson));

    CHECK(placement_is_mirror_reflected(0, kItemTypeVehicle));
    CHECK(!placement_is_mirror_reflected(0, kItemTypeBuilding));
    CHECK(placement_is_mirror_reflected(kEntityAttribMirrorReflect,
            kItemTypeBuilding));
    CHECK(placement_is_mirror_reflected(kEntityAttribMirrorReflect, 0));
    // The shipped combo observed in 00TRa: Reflective | NoShadow |
    // Indestructible still reflects.
    CHECK(placement_is_mirror_reflected(0x01a00000u, kItemTypeBuilding));
    // Neighbouring authored bits never leak into the reflect decision.
    CHECK(!placement_is_mirror_reflected(kEntityAttribNoShadow,
            kItemTypeBuilding));
    CHECK(!placement_is_mirror_reflected(0x00400000u, kItemTypeBuilding));
}

static void test_individual_node_admission() {
    // Persons, dynamic casters, animated items and occluders leave the batch;
    // a plain static item rides the population.
    CHECK(needs_individual_node(kItemTypePerson, 0, false, false));
    CHECK(needs_individual_node(kItemTypeVehicle, kItemAttrib2DynamicShadow, false, false));
    CHECK(needs_individual_node(kItemTypeBuilding, 0, true, false));
    CHECK(needs_individual_node(kItemTypeBuilding, 0, false, true));
    CHECK(!needs_individual_node(kItemTypeBuilding, 0, false, false));
    CHECK(!needs_individual_node(kItemTypeBuilding, kItemAttrib2StaticShadow, false, false));
}

static void test_visual_item_resolution() {
    // The catalog carries the authored visual item and two authored ids.
    const auto catalog = [](int id) {
        return id == kPlayerVisualItemId || id == 100166 || id == 100200;
    };
    CHECK(resolve_visual_item_id(kPlayerRuntimeTypeId, catalog) == kPlayerVisualItemId);
    CHECK(resolve_visual_item_id(100166, catalog) == 100166);
    CHECK(resolve_visual_item_id(166, catalog) == 100166);
    CHECK(resolve_visual_item_id(200, catalog) == 100200);
    CHECK(resolve_visual_item_id(300, catalog) == 300);
    CHECK(resolve_visual_item_id(0, catalog) == 0);
    // Without the visual item in the catalog the player type falls through.
    const auto bare = [](int) { return false; };
    CHECK(resolve_visual_item_id(kPlayerRuntimeTypeId, bare) == kPlayerRuntimeTypeId);
}

// A placed record is a building to the renderer and the light spawner only by
// its items.def type: a pool-2 decoration (def type 2) is an entity, a
// Building-type def in either family is a building, and a record without a
// resolved def keeps its family. [orig: Entity_BuildProximityLists_Pool2
// @ 0x4b946e / 0x4b9502; Entity_SpawnGlowEffects @ 0x56c7e8..0x56c7ec]
static void test_building_identity_is_the_def_type() {
    CHECK(placed_record_is_building(kEntityKindBuilding, true, kItemTypeBuilding));
    CHECK(!placed_record_is_building(kEntityKindBuilding, true, 2)); // decoration / foliage
    CHECK(placed_record_is_building(kEntityKindItem, true, kItemTypeBuilding));
    CHECK(!placed_record_is_building(kEntityKindItem, true, kItemTypeVehicle));
    CHECK(placed_record_is_building(kEntityKindBuilding, false, 0));
    CHECK(!placed_record_is_building(kEntityKindItem, false, 0));
}

// The sway and submodel renderers are bone-table rows, bound by the
// render_function's first four characters with their case kept: the sway
// renderer is the `tree` row's world callback (the table has no `sway` row),
// the submodel renderer the `lndm` row's.
// [orig: EntityDef_InitAllCallbacks packs def+0x13C..0x13F @ 0x4a5ab8..0x4a5af1;
//  BoneCallback_LookupByTag `cmp [ecx], esi` @ 0x4e32c6; rows 'tree' @ 0x82cf70
//  -> BoneCallback_Sway_World @ 0x4e2b10, 'lndm' @ 0x82cfc0 ->
//  Entity_RenderBoneAttachments @ 0x441660]
static void test_bone_row_predicates() {
    CHECK(uses_sway_renderer("tree"));
    CHECK(uses_sway_renderer("treeline")); // the tag is the first four characters
    CHECK(!uses_sway_renderer("sway"));
    CHECK(!uses_sway_renderer("Tree"));
    CHECK(!uses_sway_renderer("tre"));
    CHECK(!uses_sway_renderer(""));
    CHECK(uses_submodel_renderer("lndm"));
    CHECK(uses_submodel_renderer("lndmX"));
    CHECK(!uses_submodel_renderer("LNDM"));
    CHECK(!uses_submodel_renderer("LnDm"));
    CHECK(!uses_submodel_renderer("lnd"));
}

// The section renderer's rows, each field by its own lookup: the render tag's
// psec and cesp bone rows (first four bytes, case kept), the ai_function's
// palm and towr event rows and the move_function's psec physics row (whole
// name, case ignored). A name on another field binds no section row.
// [orig: rows 'psec' @ 0x82d010 / 'cesp' @ 0x82d020 -> BoneCallback_psec_World
//  @ 0x53c130; Entity_LookupRenderCallbacks stricmp @ 0x407dd8, rows 'palm'
//  @ 0x813270 / 'towr' @ 0x813240; EntityDef_LookupPhysicsCallback stricmp
//  @ 0x4a9258, row 'psec' @ 0x82ad54]
static void test_section_renderer_rows() {
    CHECK(uses_section_renderer("", "", "psec"));
    CHECK(uses_section_renderer("", "", "cesp"));
    CHECK(uses_section_renderer("", "", "psecX")); // the tag is the first four bytes
    CHECK(!uses_section_renderer("", "", "PSEC"));
    CHECK(!uses_section_renderer("", "", "palm")); // no bone row: row 0's callbacks
    CHECK(!uses_section_renderer("", "", "towr"));
    CHECK(uses_section_renderer("palm", "", ""));
    CHECK(uses_section_renderer("Palm", "", ""));
    CHECK(uses_section_renderer("TOWR", "", ""));
    CHECK(!uses_section_renderer("palmX", "", ""));
    CHECK(!uses_section_renderer("psec", "", "")); // the event row binds no section callback
    CHECK(uses_section_renderer("", "psec", ""));
    CHECK(uses_section_renderer("", "Psec", ""));
    CHECK(!uses_section_renderer("", "psecX", ""));
    CHECK(!uses_section_renderer("", "towr", "")); // the physics row is the floating init
    CHECK(!uses_section_renderer("", "palm", ""));
    CHECK(!uses_section_renderer("gnrc", "genx", "gnrc"));
}

int main() {
    test_dynamic_shadow_admission();
    test_static_shadow_admission();
    test_mirror_admission();
    test_individual_node_admission();
    test_visual_item_resolution();
    test_building_identity_is_the_def_type();
    test_bone_row_predicates();
    test_section_renderer_rows();
    if (failures == 0) {
        std::printf("placement_traits_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
