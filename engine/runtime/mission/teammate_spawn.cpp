// Mission-owned allocation for the BMS teammate operations.
// [orig: Entity_SpawnHelicopter @0x4521A0; Entity_SpawnFromItemDef @0x452390]
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity_spawn.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstring>

namespace opennova::mission {

world::EntityHandle MissionKernel::spawn_teammate(const world::TeammateSpawn &request) {
    using namespace world;
    const def::DefItemsFile *table = items_table();
    if (!table) return {};
    const def::DefItemDef *item = mission::find_item_def(*table, request.item_type + kItemIdOffset);
    if (!item || (item->attrib & kItemAttribAIData) == 0) return {};

    const aip::Profile *profile = nullptr;
    if (request.helicopter) {
        auto found = std::find_if(ai_profiles.begin(), ai_profiles.end(),
                [](const PromoteOptions::AiProfileRow &row) { return row.profile == "h_bhawkn"; });
        if (found == ai_profiles.end()) {
            std::vector<uint8_t> bytes;
            const bool loaded = files_.valid()
                    ? files_.read_file("H_BHawkN.aip", bytes)
                    : asset_index() && asset_index()->read_file("H_BHawkN.aip", bytes);
            if (!loaded || bytes.empty()) return {};
            ai_profiles.push_back({"h_bhawkn", aip::parse_profile(bytes.data(), bytes.size())});
            profile = &ai_profiles.back().data;
        } else {
            profile = &found->data;
        }
    }

    // Resolve the whole attachment family before allocating any rows. The
    // extractor's cycle/depth handling is shared with ordinary mission boot.
    mission::SeatSpecExtraction extracted;
    mission::extract_item_seat_specs(*table,
            [this](const std::string &graphic) { return assets().model(graphic).get(); },
            {int(request.item_type) + kItemIdOffset}, extracted);
    for (const ItemSeatSpec &spec : extracted.specs) {
        const auto existing = std::find_if(seat_specs.begin(), seat_specs.end(),
                [&](const ItemSeatSpec &row) { return row.type_id == spec.type_id; });
        if (existing == seat_specs.end()) seat_specs.push_back(spec);
        else *existing = spec;
    }
    for (const auto &graphic : extracted.graphic_by_type) mounted_graphics[graphic.first] = graphic.second;
    std::sort(seat_specs.begin(), seat_specs.end(),
            [](const ItemSeatSpec &a, const ItemSeatSpec &b) { return a.type_id < b.type_id; });
    mission::stamp_seat_spec_turret_limits(world, seat_specs);

    Entity seed;
    seed.kind = request.helicopter ? world::EntityKind::Item : world::EntityKind::Organic;
    seed.item_id = request.item_type;
    seed.net_id = request.ssn;
    seed.team = 1;
    seed.position = {request.pos[0] / 65536.0f, request.pos[1] / 65536.0f, request.pos[2] / 65536.0f};
    seed.spawn_position = seed.position;
    seed.yaw = float(mission_yaw_deg_from_bam_heading(request.heading));
    seed.spawn_heading = request.heading;
    seed.spawn_origin = kSpawnOriginNone;
    seed.spawn_phase = request.helicopter ? world.vehicle_ai_spawn_phase : 63;
    initialize_item_seats(seed, seat_specs);
    const EntityHandle handle = world.registry.spawn(request.helicopter ? 1 : 0, seed);
    if (!handle.valid()) return {};
    world.ai.release(handle);
    const int index = world.ai.attach(handle);
    mission::resolve_item_traits(world, *table, item_wire_class_, handle);
    Entity &entity = *world.registry.get(handle);
    AiEntity &ai = *world.ai.at(index);
    std::copy_n(request.pos, 3, ai.pos);
    std::copy_n(request.pos, 3, ai.net_saved_live_pose);
    std::copy_n(request.pos, 3, entity.saved_live_pos);
    entity.saved_live_yaw = request.heading;
    entity.saved_live_valid = true;
    ai.heading = request.heading;
    ai.net_id = request.ssn;
    ai.team = 1;
    ai.health = int16_t(entity.health);
    // Both templates start with a null entity+368 controller. Neither the
    // clone nor these definition callbacks allocates one (@0x4397C0,
    // @0x4BFCC0, @0x4683C0). The ordinary AiEntity fixture default is true.
    ai.has_physics = false;
    ai.def_attrib = entity.item_attrib;
    ai.slot.f[0] = int32_t(handle.packed) + 1;
    if (profile) {
        // CHel's definition callback is @0x4683C0 (the separate @0x461F00
        // respawn init has different flags/slide writes). Its generic allocator
        // starts all three state words at zero and lands the constant block
        // (the shared initialize_vehicle_brain); the callback then writes the
        // move step before calling the enter handler
        // [orig: Entity_InitHelicopterAIFromDef @0x4683C0 — the @0x460200 call
        //  @0x4684bf/@0x4684cf, `[brain+1Ch] = 10h` @0x468645].
        initialize_ai_profile(ai, *profile, world.ai, world::EntityKind::Item);
        initialize_vehicle_brain(ai, world, request.heading);
        ai.brain.f[AiBrain::kStep] = 16;
        std::memcpy(ai.slot.bytes() + 156, "H_BHawkN", 8);
        world.vehicle_ai_spawn_phase = (world.vehicle_ai_spawn_phase + 1) & 15;
    } else {
        // The helper template's exact 172-byte slot. [orig: @0x452390]
        ai.slot.f[12] = ai.slot.f[13] = 0x8000;
        ai.slot.f[18] = ai.slot.f[20] = ai.slot.f[21] = 620;
        ai.slot.bytes()[AiSlot::kAlertByte] = 2;
        ai.slot.f[10] = 100;
        ai.slot.f[15] = 3276800;
        ai.slot.f[33] = 0x4000;
        ai.slot.f[14] = 14;
        ai.slot.f[19] = 124;
        ai.slot.f[22] = 310;
        // Bind this model's ADM before the shared org1 initializer below.
        ai.inf.active = true;
        int adm_id = adm_id_for_runtime_type(request.item_type);
        if (adm_id == -2 && item->anim_def[0]) {
            std::string name = item->anim_def;
            if (!strutil::ends_with_icase(name, ".adm")) name += ".adm";
            adm_id = root_motion.register_adm(adm_assets_ ? adm_assets_ : &assets(), name);
        }
        ai.inf.adm_id = adm_id;
        world.ai.root_motion = root_motion.empty() ? nullptr : &root_motion;
    }
    mission::resolve_ai_weapons(world, *table, handle, &assets());
    if (profile) {
        ai.brain.f[AiBrain::kAmmoA] = ai.profile.fire_a.ammo_index >= 0 ? profile->primary.ammo : 0;
        ai.brain.f[AiBrain::kAmmoB] = ai.profile.fire_b.ammo_index >= 0 ? profile->secondary.ammo : 0;
    }
    ensure_collision_instance(world, handle);
    if (!profile) initialize_organic_ai(world, entity);
    if (profile) {
        AiThinkCtx context{&world.ai, &ai, &world};
        world.ai.row(ai.brain.f[AiBrain::kCurState]).enter(context);
    }

    const ItemAttachmentSpawns children = spawn_item_attachments(world, {handle}, seat_specs);
    for (EntityHandle child : children.handles) {
        mission::resolve_item_traits(world, *table, item_wire_class_, child);
        ensure_collision_instance(world, child);
    }
    world.facials.initialize(world);
    collision.refresh_after_registry_change(world);
    return handle;
}

// Clone the class callback's template, then bind its own definition and model.
// [orig: Entity_CloneFromTemplateByType @ 0x4398A0]
world::EntityHandle MissionKernel::spawn_item_piece(const world::Entity &seed) {
    const auto *table = items_table();
    const auto *def = table ? mission::find_item_def(*table, seed.item_id + kItemIdOffset) : nullptr;
    if (!def || seed.item_id == 0) return {};
    const int pool = def->type == 3 ? 0 :
            (def->type == 1 || def->type == 6) ? 1 :
            (def->type == 2 || def->type == 5) ? 2 : -1;
    if (pool < 0) return {};
    const auto handle = world.registry.spawn(pool, seed);
    if (!handle.valid()) return {};
    mission::resolve_item_traits(world, *table, item_wire_class_, handle);
    // Template values win over definition initialization for these fields.
    auto &piece = *world.registry.get(handle);
    piece.health = seed.health;
    piece.engine_flags = seed.engine_flags;
    piece.alive = seed.alive;
    piece.uniform_scale_q16 = seed.uniform_scale_q16;
    piece.death_motion = seed.death_motion;
    // The retail template is a memset clone that pins its callbacks to the
    // floating-physics pair; every other def-derived class selector stays
    // zero, so the parent's door/squib/sway functions never reach a piece.
    // [orig: Entity_SpawnSectionEntity @0x440322 memset; callbacks pinned
    //  @0x440365 / @0x440370; Entity_CloneFromTemplateByType @0x4398A0 copies
    //  the 0x2B4 bytes verbatim @0x43997D]
    piece.door_event = false;
    piece.door_motion = false;
    piece.door_count = 0;
    piece.squib.motor = false;
    piece.render_sway = false;
    ensure_collision_instance(world, handle);
    collision.refresh_after_registry_change(world);
    return handle;
}
} // namespace opennova::mission
