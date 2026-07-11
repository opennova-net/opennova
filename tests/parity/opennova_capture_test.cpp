#include <parity/opennova_capture.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include <world/ai.h>

namespace parity = opennova::parity;
namespace world = opennova::world;

namespace {

class TransientEventSink final : public parity::IEventSink {
public:
    explicit TransientEventSink(int rejections) : rejections_(rejections) {}

    bool append(const parity::Event& event) override {
        if (rejections_ > 0) {
            --rejections_;
            error_ = "transient contention";
            return false;
        }
        events.push_back(event);
        error_.clear();
        return true;
    }

    const std::string& last_error() const noexcept override {
        return error_;
    }

    std::vector<parity::Event> events{};

private:
    int rejections_{};
    std::string error_{};
};

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool near(double actual, double expected, double tolerance = 0.00001) {
    return std::abs(actual - expected) <= tolerance;
}

parity::RunMetadata metadata() {
    parity::RunMetadata result{};
    result.run_id = "candidate-17";
    result.producer = "opennova-godot";
    result.build_id = "build-abc";
    result.scenario = "player-combat-loop";
    result.started_unix_ns = 1234;
    result.identity = {
        parity::SourceKind::opennova,
        parity::RunRole::host,
        "opennova-host",
    };
    result.title = "joint-operations";
    result.expansion = "revx02";
    result.mission = "ASH_G3D.bms";
    return result;
}

bool authoritative_capture_preserves_world_state() {
    world::World live_world;
    live_world.registry.configure_pool(0, 4);
    live_world.registry.configure_pool(1, 2);

    world::Entity seed{};
    seed.net_id = 91;
    seed.bms_id = 37;
    seed.owner_connection_id = 7;
    seed.kind = world::EntityKind::Organic;
    seed.item_id = 0x14B9;
    seed.position = {1.25f, -2.5f, 3.0f};
    seed.yaw = 45;
    seed.pitch = -10;
    seed.roll = 5;
    seed.flags = 0x102;
    seed.team = 2;
    seed.player_class = 8;
    seed.health = 125;
    seed.health_max = 150;
    seed.body_anim_slot = 99;
    seed.anim_slot = 12;
    seed.equipped_adm_index = 4;
    seed.name = "Taylor";
    const world::EntityHandle player =
        live_world.registry.spawn_from(0, 2, seed);
    if (!expect(player.valid(), "player fixture spawned")) return false;

    world::AiSystem ai;
    live_world.ai = &ai;
    ai.attach(player);
    world::AiEntity* ai_player = ai.for_handle(player);
    if (!expect(ai_player != nullptr, "player AI fixture attached")) return false;
    ai_player->inf.active = true;
    ai_player->inf.anim_state = 77;
    ai_player->inf.clip_phase = 19;
    live_world.cached.local_player = player;
    live_world.logic_tick = 77;

    parity::OpenNovaCaptureState state{};
    state.world = &live_world;
    state.authoritative_player = player;
    parity::OpenNovaCaptureSource source(metadata(), state);
    const parity::CaptureSample sample = source.capture({
        parity::StateLane::authoritative,
        41,
        9001,
    });

    if (!expect(sample.frame.has_value(), "authoritative frame captured")) {
        std::cerr << sample.detail << '\n';
        return false;
    }
    const parity::FrameSnapshot& frame = *sample.frame;
    if (!expect(frame.identity.source == parity::SourceKind::opennova,
                "frame carries OpenNova producer identity") ||
        !expect(frame.identity.role == parity::RunRole::host,
                "frame carries host role") ||
        !expect(frame.identity.stream_id == "opennova-host",
                "frame carries stable stream id") ||
        !expect(frame.lane == parity::StateLane::authoritative,
                "frame lane preserved") ||
        !expect(frame.frame_index == 41, "frame index preserved") ||
        !expect(frame.simulation_tick == 77, "World logic tick captured") ||
        !expect(frame.timestamp_ns == 9001, "timestamp preserved")) {
        return false;
    }

    if (!expect(frame.pools.size() == 2, "configured pool topology captured") ||
        !expect(frame.pools[0].index == 0 && frame.pools[0].capacity == 4 &&
                    frame.pools[0].used_count == 3 &&
                    frame.pools[0].live_count == 1,
                "player pool high-water and live count captured") ||
        !expect(frame.pools[1].index == 1 && frame.pools[1].capacity == 2 &&
                    frame.pools[1].used_count == 0 &&
                    frame.pools[1].live_count == 0,
                "empty configured pool captured") ||
        !expect(frame.complete_entity_pools == std::vector<std::uint32_t>{0},
                "player pool is declared exhaustive") ||
        !expect(frame.entities.size() == 1, "live entity captured once")) {
        return false;
    }

    const parity::EntityState& entity = frame.entities.front();
    if (!expect(entity.identity.pool == 0 && entity.identity.slot == 2,
                "pool and slot identity captured") ||
        !expect(entity.identity.wire_handle == 2,
                "packed wire identity captured") ||
        !expect(entity.identity.bms_id == 37 && entity.identity.ssn == 91,
                "BMS and SSN identities captured") ||
        !expect(entity.identity.owner_connection_id == 7,
                "connection identity captured") ||
        !expect(entity.identity.type_id == 0x14B9,
                "type identity captured") ||
        !expect(entity.identity.name == "Taylor", "name captured") ||
        !expect(entity.kind == parity::EntityKind::player,
                "wire-resolved player kind captured") ||
        !expect(entity.transform.has_value(), "entity transform observed")) {
        return false;
    }

    const parity::EntityTransform& transform = *entity.transform;
    if (!expect(transform.raw_encoding ==
                    parity::RawTransformEncoding::fixed_point_16_16_bam32,
                "raw transform encoding declared") ||
        !expect(transform.raw_x == 81920 && transform.raw_y == -163840 &&
                    transform.raw_z == 196608,
                "raw fixed-point position captured") ||
        !expect(transform.raw_heading == 536870880 &&
                    transform.raw_pitch == -119304640 &&
                    transform.raw_roll == 59652320,
                "raw BAM orientation captured") ||
        !expect(near(transform.x, 1.25) && near(transform.y, -2.5) &&
                    near(transform.z, 3.0),
                "normalized position captured") ||
        !expect(near(transform.yaw_deg, 44.9999973) &&
                    near(transform.pitch_deg, -9.9999994) &&
                    near(transform.roll_deg, 4.9999997),
                "normalized BAM orientation captured")) {
        return false;
    }

    if (!expect(entity.flags == 0x102 && entity.team == 2,
                "entity flags and team captured") ||
        !expect(entity.player_class == 8 && entity.health == 125,
                "player class and health captured") ||
        !expect(entity.max_health == 150, "known max health observed") ||
        !expect(!entity.armor.has_value(),
                "unmodeled entity armor remains unavailable") ||
        !expect(entity.alive, "alive state captured") ||
        !expect(entity.body_anim_slot == 12 && entity.anim_state == 77 &&
                    entity.anim_phase_ticks == 19,
                "body animation state captured") ||
        !expect(entity.equipped_adm_index == 4,
                "equipped ADM captured") ||
        !expect(frame.player.has_value(), "local player projection captured") ||
        !expect(frame.player->identity.wire_handle == 2 &&
                    frame.player->health == 125 &&
                    frame.player->max_health == 150 &&
                    !frame.player->armor.has_value(),
                "local player projection uses authoritative entity")) {
        return false;
    }
    return true;
}

bool presented_capture_preserves_decoded_client_state() {
    world::World live_world;
    live_world.registry.configure_pool(0, 4);
    world::Entity seed{};
    seed.net_id = 91;
    seed.bms_id = 37;
    seed.owner_connection_id = 7;
    seed.kind = world::EntityKind::Organic;
    seed.item_id = 0x14B9;
    seed.team = 2;
    seed.player_class = 8;
    seed.health_max = 150;
    seed.name = "Taylor";
    const world::EntityHandle player =
        live_world.registry.spawn_from(0, 2, seed);
    if (!expect(player.valid(), "presented player fixture spawned")) {
        return false;
    }

    opennova::netsim::ClientState client{};
    client.local_health = 88;
    client.frames_applied = 23;
    opennova::netsim::ClientEntityState& decoded =
        client.upsert(player.packed);
    decoded.type_id = 0x14B9;
    decoded.cls = opennova::EntityClass::Player;
    decoded.x = world::to_fixed(10.5);
    decoded.y = world::to_fixed(-20.25);
    decoded.z = world::to_fixed(2.0);
    decoded.yaw_byte = 0x40;
    decoded.seen_this_frame = true;

    parity::OpenNovaCaptureState state{};
    state.world = &live_world;
    state.presented = &client;
    state.presented_player_wire_handle = player.packed;
    state.resolve_presented_from_world = true;
    parity::OpenNovaCaptureSource source(metadata(), state);
    const parity::CaptureSample sample = source.capture({
        parity::StateLane::presented,
        42,
        9002,
    });

    if (!expect(sample.frame.has_value(), "presented frame captured")) {
        std::cerr << sample.detail << '\n';
        return false;
    }
    const parity::FrameSnapshot& frame = *sample.frame;
    if (!expect(frame.lane == parity::StateLane::presented,
                "presented lane preserved") ||
        !expect(frame.frame_index == 42 && frame.simulation_tick == 23 &&
                    frame.timestamp_ns == 9002,
                "presented frame clocks captured") ||
        !expect(frame.pools.size() == 1 &&
                    frame.pools[0].used_count == 3 &&
                    frame.pools[0].live_count == 1,
                "decoded player pool topology captured") ||
        !expect(frame.complete_entity_pools == std::vector<std::uint32_t>{0},
                "decoded player pool is declared exhaustive") ||
        !expect(frame.entities.size() == 1,
                "decoded player captured once")) {
        return false;
    }

    const parity::EntityState& entity = frame.entities.front();
    if (!expect(entity.identity.wire_handle == 2 &&
                    entity.identity.bms_id == 37 && entity.identity.ssn == 91,
                "decoded identity enriched from the matching World entity") ||
        !expect(entity.identity.type_id == 0x14B9,
                "decoded wire type captured") ||
        !expect(entity.kind == parity::EntityKind::player,
                "decoded wire class captured") ||
        !expect(entity.transform.has_value(),
                "decoded transform observed")) {
        return false;
    }
    const parity::EntityTransform& transform = *entity.transform;
    if (!expect(transform.raw_x == 688128 && transform.raw_y == -1327104 &&
                    transform.raw_z == 131072,
                "decoded fixed-point position preserved") ||
        !expect(transform.raw_heading == 0x40000000,
                "decoded coarse BAM heading preserved") ||
        !expect(near(transform.x, 10.5) && near(transform.y, -20.25) &&
                    near(transform.z, 2.0) && near(transform.yaw_deg, 90.0),
                "decoded presentation transform normalized") ||
        !expect(entity.health == 88 && entity.max_health == 150,
                "decoded local health and known max captured") ||
        !expect(frame.player.has_value() && frame.player->health == 88 &&
                    frame.player->identity.wire_handle == 2,
                "decoded local player projection captured")) {
        return false;
    }
    return true;
}

bool capture_includes_typed_input_weapon_and_camera_state() {
    world::World live_world;
    live_world.registry.configure_pool(0, 4);
    world::Entity seed{};
    seed.kind = world::EntityKind::Organic;
    seed.item_id = 0x14B9;
    seed.position = {4.0f, 5.0f, 6.0f};
    seed.net_analog_x = 11;
    seed.net_analog_y = -22;
    seed.net_analog_z = 33;
    const world::EntityHandle player =
        live_world.registry.spawn_from(0, 2, seed);
    if (!expect(player.valid(), "typed local-state fixture spawned")) {
        return false;
    }
    live_world.cached.local_player = player;

    world::PlayerInput input{};
    input.forward = true;
    input.right = true;
    input.run = true;
    input.crouch = true;
    input.jump = true;
    input.look_heading = 0x20000000;
    input.look_pitch = -0x10000000;

    world::WeaponFsmDef weapon_def{};
    weapon_def.flags = 2;
    weapon_def.clip_capacity = 30;
    world::WeaponSlotState weapon_slot{};
    weapon_slot.current = world::weapon_action::kRecoil;
    weapon_slot.clip = 17;
    weapon_slot.reserve = 60;
    parity::OpenNovaWeaponPresentation weapon_presentation{};
    weapon_presentation.name = "WPN_M4AUTO";
    weapon_presentation.special_hold = 6;
    weapon_presentation.attack_anim = 2;
    weapon_presentation.primary = {1.0, -0.5, 0.25, 353.0, 20.0, 30.0};
    weapon_presentation.alternate = {2.0, 1.0, -1.0, -5.0, 15.0, 25.0};
    weapon_presentation.render_fov = 80.0;

    world::PlayerViewState player_view{};
    player_view.scope_engaged = true;
    player_view.scope_step = 6;

    parity::OpenNovaCaptureState state{};
    state.world = &live_world;
    state.authoritative_player = player;
    state.input = &input;
    state.weapon_def = &weapon_def;
    state.weapon_slot = &weapon_slot;
    state.weapon_presentation = &weapon_presentation;
    state.player_view = &player_view;
    state.scope_max_magnification = 2.0f;
    parity::OpenNovaCaptureSource source(metadata(), state);
    const parity::CaptureSample sample = source.capture({
        parity::StateLane::authoritative,
        43,
        9003,
    });
    if (!expect(sample.frame.has_value(), "typed local-state frame captured")) {
        return false;
    }
    const parity::FrameSnapshot& frame = *sample.frame;
    if (!expect(frame.input.has_value(), "typed input observed") ||
        !expect(frame.input->move_order == 0x200,
                "raw crouch move-order bit captured") ||
        !expect(frame.input->analog_x == 11 && frame.input->analog_y == -22 &&
                    frame.input->analog_z == 33,
                "raw analog entity inputs captured") ||
        !expect(frame.input->forward && frame.input->right && frame.input->run &&
                    frame.input->crouch && frame.input->jump,
                "semantic movement input captured") ||
        !expect(near(frame.input->look_yaw_deg, 45.0) &&
                    near(frame.input->look_pitch_deg, -22.5),
                "normalized look input captured")) {
        return false;
    }

    if (!expect(frame.weapon.has_value(), "typed weapon observed") ||
        !expect(frame.weapon->name == "WPN_M4AUTO" &&
                    frame.weapon->special_hold == 6 &&
                    frame.weapon->attack_anim == 2,
                "weapon identity and body kinds captured") ||
        !expect(near(frame.weapon->primary.x, 1.0) &&
                    near(frame.weapon->primary.y, -0.5) &&
                    near(frame.weapon->primary.z, 0.25) &&
                    near(frame.weapon->primary.yaw_deg,
                         -6.99999958276749, 0.0000000001) &&
                    near(frame.weapon->primary.pitch_deg, 20.0) &&
                    near(frame.weapon->primary.roll_deg, 30.0),
                "hip pose converted from raw file units") ||
        !expect(near(frame.weapon->alternate.x, 2.0) &&
                    near(frame.weapon->alternate.y, 1.0) &&
                    near(frame.weapon->alternate.z, -1.0),
                "aimed pose converted from raw file units") ||
        !expect(frame.weapon->action == std::string("recoil") &&
                    frame.weapon->clip == 17 && frame.weapon->reserve == 60,
                "weapon FSM state captured")) {
        return false;
    }

    if (!expect(frame.camera.has_value(), "typed camera observed") ||
        !expect(frame.camera->transform.has_value(),
                "camera transform observed") ||
        !expect(near(frame.camera->transform->x, 4.0) &&
                    near(frame.camera->transform->y, 5.0) &&
                    near(frame.camera->transform->z, 7.0),
                "first-person eye position captured") ||
        !expect(frame.camera->transform->raw_heading == 0x20000000 &&
                    frame.camera->transform->raw_pitch == -0x10000000,
                "camera raw look orientation captured") ||
        !expect(near(frame.camera->fov_deg, 64.0) &&
                    frame.camera->scope_engaged &&
                    near(frame.camera->scope_fraction, 0.4),
                "camera ADS policy captured")) {
        return false;
    }
    return true;
}

bool recorder_frames_each_lane_with_orderly_checkpoints() {
    world::World live_world;
    live_world.registry.configure_pool(0, 4);
    live_world.logic_tick = 9;
    opennova::netsim::ClientState client{};
    client.frames_applied = 8;
    parity::OpenNovaCaptureState state{};
    state.world = &live_world;
    state.presented = &client;
    parity::OpenNovaCaptureSource source(metadata(), state);
    parity::TraceWriter sink;
    parity::OpenNovaCaptureRecorder recorder(metadata(), sink);

    if (!expect(recorder.start(), "capture recorder starts") ||
        !expect(recorder.record_tick(
                    source,
                    {parity::StateLane::authoritative,
                     parity::StateLane::presented},
                    50,
                    1000),
                "same-tick lanes recorded") ||
        !expect(recorder.finish(), "capture recorder finishes")) {
        std::cerr << recorder.last_error() << '\n';
        return false;
    }

    const parity::TraceReadResult decoded = parity::read_trace(sink.bytes());
    if (!expect(decoded.complete(), "recorded trace decodes") ||
        !expect(decoded.trace.events.size() == 7,
                "metadata, two starts, two frames, and two ends emitted")) {
        return false;
    }
    if (!expect(std::holds_alternative<parity::RunMetadata>(
                    decoded.trace.events[0]),
                "metadata emitted first")) {
        return false;
    }
    const auto* auth_start = std::get_if<parity::Checkpoint>(
        &decoded.trace.events[1]);
    const auto* auth_frame = std::get_if<parity::FrameSnapshot>(
        &decoded.trace.events[2]);
    const auto* presented_start = std::get_if<parity::Checkpoint>(
        &decoded.trace.events[3]);
    const auto* presented_frame = std::get_if<parity::FrameSnapshot>(
        &decoded.trace.events[4]);
    const auto* auth_end = std::get_if<parity::Checkpoint>(
        &decoded.trace.events[5]);
    const auto* presented_end = std::get_if<parity::Checkpoint>(
        &decoded.trace.events[6]);
    return expect(auth_start != nullptr && auth_start->name == "capture-start" &&
                      auth_start->occurrence == 0 && auth_start->frame_index == 50 &&
                      auth_start->lane == parity::StateLane::authoritative,
                  "authoritative start checkpoint frames its first sample") &&
        expect(auth_frame != nullptr && auth_frame->frame_index == 50 &&
                   auth_frame->lane == parity::StateLane::authoritative,
               "authoritative frame follows its start") &&
        expect(presented_start != nullptr &&
                   presented_start->name == "capture-start" &&
                   presented_start->frame_index == 50 &&
                   presented_start->lane == parity::StateLane::presented,
               "presented start checkpoint shares the completed tick") &&
        expect(presented_frame != nullptr && presented_frame->frame_index == 50 &&
                   presented_frame->lane == parity::StateLane::presented,
               "presented frame follows its start") &&
        expect(auth_end != nullptr && auth_end->name == "capture-end" &&
                   auth_end->frame_index == 50 &&
                   auth_end->identity.stream_id == "opennova-host",
               "authoritative end preserves producer and last frame") &&
        expect(presented_end != nullptr &&
                   presented_end->name == "capture-end" &&
                   presented_end->frame_index == 50,
               "presented end preserves its last frame");
}

bool datagram_capture_preserves_payload_direction_and_endpoints() {
    const parity::ProducerIdentity identity{
        parity::SourceKind::opennova,
        parity::RunRole::host,
        "opennova-host",
    };
    const std::uint8_t bytes[]{0x81, 0x00, 0x7f, 0x42};
    const parity::NetworkEndpoint local{"0.0.0.0", 64220};
    const parity::NetworkEndpoint remote{"127.0.0.1", 51000};
    const parity::NetworkDatagram outbound =
        parity::make_open_nova_datagram(
            identity,
            parity::DatagramDirection::outbound,
            123456,
            1,
            local,
            remote,
            bytes,
            sizeof(bytes));
    const parity::NetworkDatagram inbound =
        parity::make_open_nova_datagram(
            identity,
            parity::DatagramDirection::inbound,
            123457,
            1,
            local,
            remote,
            bytes,
            sizeof(bytes));
    return expect(outbound.identity.stream_id == "opennova-host" &&
                      outbound.timestamp_ns == 123456 &&
                      outbound.socket_id == 1,
                  "datagram carries producer and clocks") &&
        expect(outbound.total_size == 4 && !outbound.truncated &&
                   outbound.payload == parity::ByteString(bytes, bytes + 4),
               "raw datagram payload is embedded") &&
        expect(outbound.source.address == "0.0.0.0" &&
                   outbound.source.port == 64220 &&
                   outbound.destination.address == "127.0.0.1" &&
                   outbound.destination.port == 51000,
               "outbound endpoint direction is local to remote") &&
        expect(inbound.source.address == "127.0.0.1" &&
                   inbound.source.port == 51000 &&
                   inbound.destination.address == "0.0.0.0" &&
                   inbound.destination.port == 64220,
               "inbound endpoint direction is remote to local");
}

bool queued_sink_retries_transient_backpressure_without_reordering() {
    auto downstream = std::make_unique<TransientEventSink>(2);
    TransientEventSink* observed = downstream.get();
    parity::OpenNovaQueuedEventSink sink(std::move(downstream), 4);
    parity::RunMetadata run = metadata();
    parity::Checkpoint start{};
    start.name = "capture-start";
    start.identity = run.identity;
    start.lane = parity::StateLane::authoritative;
    parity::FrameSnapshot frame{};
    frame.identity = run.identity;
    frame.lane = parity::StateLane::authoritative;

    if (!expect(sink.append(parity::Event{run}),
                "first transient rejection queues metadata") ||
        !expect(sink.append(parity::Event{start}),
                "second transient rejection queues the checkpoint") ||
        !expect(sink.pending_count() == 2,
                "both rejected events remain pending") ||
        !expect(sink.append(parity::Event{frame}),
                "later append drains pending events and remains accepted") ||
        !expect(sink.pending_count() == 0,
                "retry queue drains completely") ||
        !expect(observed->events.size() == 3,
                "downstream eventually receives every event")) {
        return false;
    }
    return expect(std::holds_alternative<parity::RunMetadata>(
                      observed->events[0]) &&
                      std::holds_alternative<parity::Checkpoint>(
                          observed->events[1]) &&
                      std::holds_alternative<parity::FrameSnapshot>(
                          observed->events[2]),
                  "retry preserves producer event order");
}

}  // namespace

int main() {
    if (!authoritative_capture_preserves_world_state()) return 1;
    if (!presented_capture_preserves_decoded_client_state()) return 1;
    if (!capture_includes_typed_input_weapon_and_camera_state()) return 1;
    if (!recorder_frames_each_lane_with_orderly_checkpoints()) return 1;
    if (!datagram_capture_preserves_payload_direction_and_endpoints()) return 1;
    if (!queued_sink_retries_transient_backpressure_without_reordering()) return 1;
    std::cout << "opennova parity authoritative capture OK\n";
    return 0;
}
