// engine/runtime/mission seat-spec extraction (S4, ADR 0028) — the typed
// table from def rows + model userpoints, pinned headless: the witnessed name
// prefixes, retail slot layout with first-claim slots, sitexNN pose digits,
// the yaw-zero local conversion ((-y, x, z)/65536 over the raw authored
// ints), the armory attrib gate, addeweap anchors with the parent-root
// fallback, the child recursion, and the runtime-metadata filter.
#include <formats/def/def.h>
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/assets/asset_store.h>
#include <base/resource_index/resource_index.h>

#include "common/test_paths.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace opennova;
using namespace opennova::mission;
using namespace opennova::def;
using namespace opennova::threedi;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) < tol; }

ThreediUserPoint up(const char *name, int x, int y, int z, int rx = 0,
        int ry = 0, int rz = 0, int subobject = 0) {
    ThreediUserPoint p;
    std::memset(&p, 0, sizeof(p));
    p.x = x * 65536;
    p.y = y * 65536;
    p.z = z * 65536;
    p.rot_x = rx;
    p.rot_y = ry;
    p.rot_z = rz;
    p.subobject_index = subobject;
    std::snprintf(p.name, sizeof(p.name), "%s", name);
    return p;
}

DefItemDef def_row(int id, const char *graphic) {
    DefItemDef d;
    std::memset(&d, 0, sizeof(d));
    d.id = id;
    std::snprintf(d.graphic, sizeof(d.graphic), "%s", graphic);
    return d;
}

// Former ItemSeatCard GUT oracle: a real parsed definition and model go
// through the same extractor used by mission boot, without ClassDB records.
void fixture_mount() {
    const std::string root = test_paths_repo_root(__FILE__);
    ResourceIndex index;
    CHECK(index.scan(root + "/fixtures/def", {}, VfsMountMode::LooseOnly));
    std::vector<uint8_t> bytes;
    CHECK(index.read_file("items.def", bytes));
    DefItemsFile items{};
    CHECK(def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0);
    CHECK(index.scan(root + "/fixtures/threedi/synth", {}, VfsMountMode::LooseOnly));
    assets::AssetStore models{&index};
    SeatSpecExtraction out;
    extract_item_seat_specs(items,
            [&models](const std::string &key) { return models.model(key).get(); },
            {101419, 999999}, out);
    CHECK(out.specs.size() == 1); // unknown item contributes no inspection row
    const auto *spec = item_seat_spec_for_type(out.specs, 1419);
    CHECK(spec != nullptr);
    if (spec) {
        CHECK(spec->mount_config_valid && spec->mount_config == 4);
        CHECK(spec->primary_weapon == "WPN_EMPLCD50NA");
        CHECK(spec->seats.size() == 1);
        if (spec->seats.size() == 1) {
            const world::Seat &seat = spec->seats[0];
            CHECK(seat.source_name == "Usegun");
            CHECK(seat.bone_index == 6);
            CHECK(seat.type == world::SeatType::Gunner);
            CHECK(seat.retail_slot == 9);
            CHECK(!seat.occupant.valid());
        }
    }
    def_free_items(&items);
}

} // namespace

int main() {
    fixture_mount();
    // ---- the conversions, standalone ----
    // Raw (1, 2, 3) world-units authored -> mission local (-2, 1, 3).
    const ThreediUserPoint probe = up("sitex", 1, 2, 3);
    const world::Vec3 local = seat_local_from_user_point(probe);
    CHECK(near(local.x, -2.0f) && near(local.y, 1.0f) && near(local.z, 3.0f));
    // Directions: the collapsed chain yields yaw = atan2(-rot_y, rot_x).
    CHECK(seat_yaw_offset_from_user_point(up("s", 0, 0, 0, 65536, 0, 0)) == 0);
    CHECK(seat_yaw_offset_from_user_point(up("s", 0, 0, 0, 0, 65536, 0)) == -90);
    CHECK(seat_yaw_offset_from_user_point(up("s", 0, 0, 0, 0, -65536, 0)) == 90);
    CHECK(seat_yaw_offset_from_user_point(up("s", 0, 0, 0, -65536, 0, 0)) != 0);
    // A zero direction and a vertical-only direction both degrade to 0.
    CHECK(seat_yaw_offset_from_user_point(up("s", 0, 0, 0, 0, 0, 0)) == 0);
    CHECK(seat_yaw_offset_from_user_point(up("s", 0, 0, 0, 0, 0, 65536)) == 0);

    // ---- the carrier model: seats, armory, an attachment anchor ----
    std::vector<ThreediUserPoint> carrier_points = {
        up("sitex06", 1, 2, 3),            // passenger, pose 6, slot 0
        up("sitex", 4, 5, 6),              // passenger, slot 1
        up("CtrlX02", 7, 8, 9),            // controller, pose 2, slot 8
        up("DRVRX", 10, 11, 12),           // driver: slot 8 taken -> none
        up("Usegun07", 13, 14, 15),        // gunner, pose 0 (UseGun never poses)
        up("armory1", 16, 17, 18),         // armory anchor (attrib-gated)
        up("engine", 19, 20, 21),          // not a seat
        up(" GunAnchor ", 22, 23, 24, 0, 65536, 0, 5), // attachment target
    };
    Threedi3di3 carrier_model;
    std::memset(&carrier_model, 0, sizeof(carrier_model));
    carrier_model.user_points = carrier_points.data();
    carrier_model.user_point_count = carrier_points.size();

    std::vector<ThreediUserPoint> gun_points = {
        up("Usegun", 0, 0, 1),
    };
    Threedi3di3 gun_model;
    std::memset(&gun_model, 0, sizeof(gun_model));
    gun_model.user_points = gun_points.data();
    gun_model.user_point_count = gun_points.size();

    // ---- the def rows ----
    DefItemDef defs[3] = {
        def_row(100500, "vehicles/carrier.3di"),
        def_row(100600, "gun"),
        def_row(100700, ""), // graphic-less: attachments-only spec
    };
    DefItemDef &carrier = defs[0];
    carrier.phrase_set = 4;
    carrier.phrase_set_valid = 1;
    std::snprintf(carrier.primary_weapon, sizeof(carrier.primary_weapon),
            "%s", "WPN_50CAL");
    carrier.attrib = DEF_ITEM_ATTRIB_ARMORY;
    DefItemEmplacementAttachment carrier_rows[2];
    std::memset(carrier_rows, 0, sizeof(carrier_rows));
    std::snprintf(carrier_rows[0].userpoint, sizeof(carrier_rows[0].userpoint),
            "%s", "gunanchor"); // case-insensitive whole-name match
    carrier_rows[0].item_id = 100600;
    carrier_rows[0].kind = DEF_ITEM_EMPLACEMENT_ADDEWEAP_G;
    carrier_rows[0].angle_count = 4;
    carrier_rows[0].down_angle = 111;
    carrier_rows[0].up_angle = -222;
    carrier_rows[0].right_angle = 333;
    carrier_rows[0].left_angle = -444;
    std::snprintf(carrier_rows[1].userpoint, sizeof(carrier_rows[1].userpoint),
            "%s", "NoSuchPoint");
    carrier_rows[1].item_id = 100600;
    carrier_rows[1].kind = DEF_ITEM_EMPLACEMENT_ADDEWEAP;
    carrier.emplacement_attachments = carrier_rows;
    carrier.emplacement_attachments_count = 2;
    carrier.emplacement_g_slot = 1;
    carrier.emplacement_c_slot = 2;

    DefItemDef &orphan = defs[2];
    DefItemEmplacementAttachment orphan_rows[1];
    std::memset(orphan_rows, 0, sizeof(orphan_rows));
    std::snprintf(orphan_rows[0].userpoint, sizeof(orphan_rows[0].userpoint),
            "%s", "x");
    orphan_rows[0].item_id = 100600;
    orphan.emplacement_attachments = orphan_rows;
    orphan.emplacement_attachments_count = 1;

    DefItemsFile items;
    items.entries = defs;
    items.count = 3;

    std::unordered_map<std::string, const Threedi3di3 *> models = {
        {"vehicles/carrier.3di", &carrier_model},
        {"gun", &gun_model},
    };
    const ModelLookupFn lookup = [&](const std::string &graphic) {
        const auto it = models.find(graphic);
        return it == models.end() ? nullptr : it->second;
    };

    // Seeds: the carrier + the orphan + an unknown id; the gun arrives via
    // the child recursion only.
    SeatSpecExtraction out;
    extract_item_seat_specs(items, lookup, {100500, 100700, 100999}, out);
    CHECK(out.specs.size() == 3); // carrier 500, gun 600 (recursed), orphan 700
    CHECK(out.graphic_by_type.count(500) == 1 &&
            out.graphic_by_type.at(500) == "vehicles/carrier.3di");
    CHECK(out.graphic_by_type.count(700) == 0); // no model resolved

    const mission::ItemSeatSpec &cs = out.specs[0];
    CHECK(cs.type_id == 500);
    CHECK(cs.mount_config_valid && cs.mount_config == 4);
    CHECK(cs.primary_weapon == "WPN_50CAL");
    // 2 passengers + controller + driver + gunner; the driver row is kept —
    // only its retail slot is unassigned after the controller claimed 8.
    CHECK(cs.seats.size() == 5);
    if (cs.seats.size() == 5) {
        const world::Seat &p0 = cs.seats[0];
        CHECK(p0.type == world::SeatType::Passenger && p0.retail_slot == 0 &&
                p0.bone_index == 1 && p0.pose_index == 6);
        CHECK(near(p0.seat_local.x, -2.0f) && near(p0.seat_local.y, 1.0f) &&
                near(p0.seat_local.z, 3.0f));
        CHECK(p0.source_name == "sitex06");
        const world::Seat &p1 = cs.seats[1];
        CHECK(p1.type == world::SeatType::Passenger && p1.retail_slot == 1);
        const world::Seat &ctrl = cs.seats[2];
        CHECK(ctrl.type == world::SeatType::Controller &&
                ctrl.retail_slot == 8 && ctrl.pose_index == 2);
        const world::Seat &drv = cs.seats[3];
        CHECK(drv.type == world::SeatType::Driver &&
                drv.retail_slot == 0xFF); // slot 8 already claimed
        const world::Seat &gun = cs.seats[4];
        CHECK(gun.type == world::SeatType::Gunner && gun.retail_slot == 9 &&
                gun.pose_index == 0 && gun.bone_index == 5);
    }
    CHECK(cs.armory_points.size() == 1);
    if (!cs.armory_points.empty()) {
        CHECK(near(cs.armory_points[0].x, -17.0f) &&
                near(cs.armory_points[0].y, 16.0f) &&
                near(cs.armory_points[0].z, 18.0f));
    }
    CHECK(cs.emplacement_attachments.size() == 2);
    if (cs.emplacement_attachments.size() == 2) {
        const mission::ItemEmplacementAttachmentSpec &a =
                cs.emplacement_attachments[0];
        CHECK(a.child_type_id == 600);
        CHECK(a.kind == mission::EmplacementAttachmentKind::G);
        CHECK(a.stored_slot == 1);
        CHECK(a.attachment_flags == 2); // slot 1 designated G
        CHECK(a.anchor_found);
        CHECK(a.anchor.bone_index == 8); // the 1-based USRP row
        CHECK(a.anchor.source_name == " GunAnchor ");
        CHECK(a.anchor.attachment_frame);
        CHECK(near(a.anchor.seat_local.x, -23.0f) &&
                near(a.anchor.seat_local.y, 22.0f) &&
                near(a.anchor.seat_local.z, 24.0f));
        CHECK(a.anchor.yaw_offset == -90); // rot_y-only direction
        CHECK(a.angle_count == 4 && a.down_limit_bam == 111 &&
                a.up_limit_bam == -222 && a.right_limit_bam == 333 &&
                a.left_limit_bam == -444);
        const mission::ItemEmplacementAttachmentSpec &b =
                cs.emplacement_attachments[1];
        CHECK(!b.anchor_found);
        CHECK(b.anchor.bone_index == 0); // missing anchor: the parent root
        CHECK(b.stored_slot == 2);
        CHECK(b.attachment_flags == 1); // slot 2 designated C
        CHECK(b.angle_count == 0);
    }

    // The recursed gun child landed with its own UseGun seat.
    const mission::ItemSeatSpec &gs = out.specs[1];
    CHECK(gs.type_id == 600);
    CHECK(gs.seats.size() == 1 &&
            gs.seats[0].type == world::SeatType::Gunner);

    // The graphic-less orphan keeps its authored attachment rows.
    const mission::ItemSeatSpec &os = out.specs[2];
    CHECK(os.type_id == 700);
    CHECK(os.seats.empty());
    CHECK(os.emplacement_attachments.size() == 1 &&
            !os.emplacement_attachments[0].anchor_found);

    // ---- prefix-at-byte-zero negatives + the pose-digit clamp ----
    // Embedded tokens are not seats — the witnessed compare runs at name byte
    // zero [orig: strnicmp(name, "sitex"/"ctrlx"/"UseGun"/"drvrx", 5/6)
    // @ 0x434ED0] — and authored pose digits clamp to the 0..30 sit window.
    // (These two rows were pinned in the retired GDScript extractor's tests.)
    std::vector<ThreediUserPoint> edge_points = {
        up("fooUseGun", 1, 0, 0), // embedded token: never a gunner seat
        up("xctrlx", 2, 0, 0),    // embedded token: never a controller
        up("sitex99", 3, 0, 0),   // pose digits 99 clamp to 30
    };
    Threedi3di3 edge_model;
    std::memset(&edge_model, 0, sizeof(edge_model));
    edge_model.user_points = edge_points.data();
    edge_model.user_point_count = edge_points.size();
    DefItemDef edge_defs[1] = {def_row(100800, "edge")};
    DefItemsFile edge_items;
    edge_items.entries = edge_defs;
    edge_items.count = 1;
    std::unordered_map<std::string, const Threedi3di3 *> edge_models = {
        {"edge", &edge_model},
    };
    const ModelLookupFn edge_lookup = [&](const std::string &graphic) {
        const auto it = edge_models.find(graphic);
        return it == edge_models.end() ? nullptr : it->second;
    };
    SeatSpecExtraction edge_out;
    extract_item_seat_specs(edge_items, edge_lookup, {100800}, edge_out);
    CHECK(edge_out.specs.size() == 1);
    if (!edge_out.specs.empty()) {
        const mission::ItemSeatSpec &es = edge_out.specs[0];
        CHECK(es.seats.size() == 1); // only sitex99 typed; both tokens rejected
        if (es.seats.size() == 1) {
            CHECK(es.seats[0].type == world::SeatType::Passenger);
            CHECK(es.seats[0].pose_index == 30);
            CHECK(es.seats[0].source_name == "sitex99");
        }
    }

    if (failures == 0) std::printf("mission_seat_spec_extract: OK\n");
    return failures == 0 ? 0 : 1;
}
