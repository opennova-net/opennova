#include <parity/parity.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace parity = opennova::parity;

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

void run_metadata_round_trips_through_the_versioned_trace() {
    parity::RunMetadata metadata{};
    metadata.run_id = "retail-001";
    metadata.producer = "retail-hook";
    metadata.build_id = "jo-1.7.5.7";
    metadata.scenario = "training";
    metadata.started_unix_ns = 0x0102030405060708ULL;
    metadata.identity = {
        parity::SourceKind::retail, parity::RunRole::client, "retail-client"};
    metadata.title = "joint-operations";
    metadata.expansion = "revx02";
    metadata.mission = "operation-barracuda";

    parity::TraceWriter writer;
    CHECK(writer.append(metadata));

    const std::vector<std::uint8_t>& bytes = writer.bytes();
    const std::vector<std::uint8_t> expected_header{
        0x4f, 0x4e, 0x50, 0x54, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
    };
    CHECK(bytes.size() > expected_header.size());
    CHECK(std::equal(expected_header.begin(), expected_header.end(), bytes.begin()));

    const parity::TraceReadResult read = parity::read_trace(bytes);
    CHECK(read.status == parity::TraceReadStatus::complete);
    CHECK(read.trace.events.size() == 1);
    CHECK(std::holds_alternative<parity::RunMetadata>(read.trace.events[0]));
    if (read.trace.events.size() == 1 &&
        std::holds_alternative<parity::RunMetadata>(read.trace.events[0])) {
        const auto& observed = std::get<parity::RunMetadata>(read.trace.events[0]);
        CHECK(observed.run_id == "retail-001");
        CHECK(observed.producer == "retail-hook");
        CHECK(observed.build_id == "jo-1.7.5.7");
        CHECK(observed.scenario == "training");
        CHECK(observed.started_unix_ns == 0x0102030405060708ULL);
        CHECK(observed.identity.source == parity::SourceKind::retail);
        CHECK(observed.identity.role == parity::RunRole::client);
        CHECK(observed.identity.stream_id == "retail-client");
        CHECK(observed.title == "joint-operations");
        CHECK(observed.expansion == "revx02");
        CHECK(observed.mission == "operation-barracuda");
    }
}

void checkpoint_and_frame_snapshot_preserve_typed_values() {
    parity::Checkpoint checkpoint{"mission-loaded", 2, 120};
    checkpoint.identity = {
        parity::SourceKind::retail, parity::RunRole::client, "retail-client"};
    checkpoint.lane = parity::StateLane::presented;
    parity::FrameSnapshot frame{};
    frame.identity = checkpoint.identity;
    frame.frame_index = 121;
    frame.simulation_tick = 9001;
    frame.extensions = {
        {"player.health", std::int64_t{75}, 0.0, parity::Severity::error},
        {"player.position.x", 12.5, 0.01, parity::Severity::warning},
        {"player.spawned", true, 0.0, parity::Severity::fatal},
        {"player.name", std::string{"Taylor"}, 0.0, parity::Severity::error},
    };

    parity::TraceWriter writer;
    CHECK(writer.append(checkpoint));
    CHECK(writer.append(frame));

    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 2);
    if (read.trace.events.size() == 2) {
        CHECK(std::holds_alternative<parity::Checkpoint>(read.trace.events[0]));
        CHECK(std::holds_alternative<parity::FrameSnapshot>(read.trace.events[1]));
        if (std::holds_alternative<parity::Checkpoint>(read.trace.events[0])) {
            const auto& observed = std::get<parity::Checkpoint>(read.trace.events[0]);
            CHECK(observed.name == "mission-loaded");
            CHECK(observed.occurrence == 2);
            CHECK(observed.frame_index == 120);
            CHECK(observed.identity.stream_id == "retail-client");
            CHECK(observed.lane == parity::StateLane::presented);
        }
        if (std::holds_alternative<parity::FrameSnapshot>(read.trace.events[1])) {
            const auto& observed = std::get<parity::FrameSnapshot>(read.trace.events[1]);
            CHECK(observed.frame_index == 121);
            CHECK(observed.simulation_tick == 9001);
            CHECK(observed.identity.stream_id == "retail-client");
            CHECK(observed.extensions.size() == 4);
            if (observed.extensions.size() == 4) {
                CHECK(std::get<std::int64_t>(observed.extensions[0].value) == 75);
                CHECK(std::get<double>(observed.extensions[1].value) == 12.5);
                CHECK(observed.extensions[1].absolute_tolerance == 0.01);
                CHECK(observed.extensions[1].mismatch_severity == parity::Severity::warning);
                CHECK(std::get<bool>(observed.extensions[2].value));
                CHECK(std::get<std::string>(observed.extensions[3].value) == "Taylor");
            }
        }
    }
}

void frame_snapshot_round_trips_explicit_runtime_state() {
    static_assert(std::is_same_v<decltype(parity::EntityIdentity::pool), std::int32_t>);
    static_assert(std::is_same_v<decltype(parity::EntityIdentity::slot), std::int32_t>);
    static_assert(std::is_same_v<decltype(parity::EntityIdentity::bms_id), std::int32_t>);
    static_assert(std::is_same_v<decltype(parity::EntityState::body_anim_slot), std::int32_t>);
    static_assert(std::is_same_v<decltype(parity::EntityState::anim_state), std::optional<std::int32_t>>);
    static_assert(std::is_same_v<decltype(parity::EntityState::anim_phase_ticks), std::optional<std::int32_t>>);
    static_assert(std::is_same_v<decltype(parity::EntityState::armor), std::optional<std::int32_t>>);
    static_assert(std::is_same_v<decltype(parity::EntityState::equipped_adm_index), std::int32_t>);
    static_assert(std::is_same_v<decltype(parity::PlayerState::armor), std::optional<std::int32_t>>);
    static_assert(std::is_same_v<decltype(parity::PlayerState::equipped_adm_index), std::int32_t>);

    parity::FrameSnapshot frame{};
    frame.identity = {
        parity::SourceKind::retail, parity::RunRole::client, "retail-client"};
    frame.lane = parity::StateLane::presented;
    frame.frame_index = 100;
    frame.simulation_tick = 6200;
    frame.timestamp_ns = 9000000;
    frame.complete_entity_pools = {0, 1};
    parity::PoolState pool{};
    pool.index = 0;
    pool.base_address = 0x12340000;
    pool.stride = 0x388;
    pool.capacity = 256;
    pool.used_count = 2;
    pool.live_count = 1;
    frame.pools.push_back(pool);

    parity::EntityState entity{};
    entity.identity = {-1, -1, 0x0001, -1, 3, 9, 42, 0x14b9, "OrganicOne"};
    parity::EntityTransform transform{
        0x10000, -0x20000, 0x30000,
        0x40000000, 0, 0,
        1.0, -2.0, 3.0,
        90.0, 0.0, 0.0,
    };
    transform.raw_encoding =
        parity::RawTransformEncoding::fixed_point_16_16_bam32;
    entity.transform = transform;
    entity.kind = parity::EntityKind::organic;
    entity.flags = 0x104;
    entity.team = 2;
    entity.player_class = 8;
    entity.health = 75;
    entity.max_health = 100;
    entity.armor = 25;
    entity.alive = true;
    entity.body_anim_slot = -1;
    entity.anim_state = -1;
    entity.anim_phase_ticks = -1;
    entity.equipped_adm_index = -1;
    frame.entities.push_back(entity);
    parity::EntityState marker = entity;
    marker.identity.name = "SpawnMarker";
    marker.kind = parity::EntityKind::marker;
    marker.armor.reset();
    frame.entities.push_back(marker);

    parity::PlayerState player{};
    player.present = true;
    player.identity = entity.identity;
    player.transform = entity.transform;
    player.health = 75;
    player.max_health = 100;
    player.armor.reset();
    player.team = 2;
    player.player_class = 8;
    player.equipped_adm_index = -1;
    frame.player = player;

    parity::WeaponState weapon{};
    weapon.present = true;
    weapon.name = "WPN_M4AUTO";
    weapon.special_hold = 0;
    weapon.attack_anim = 0;
    weapon.primary = {9.07, 20.74, -183.0, 2.0, -0.5, 0.0};
    weapon.alternate = {-44.98, 44.05, -162.0, 0.0, 0.0, 0.0};
    weapon.render_fov = 80.0;
    weapon.action = "idle";
    weapon.clip = 30;
    weapon.reserve = 300;
    frame.weapon = weapon;

    parity::CameraState camera{};
    camera.present = true;
    camera.transform = entity.transform;
    camera.fov_deg = 80.0;
    camera.third_person = false;
    camera.scope_engaged = true;
    camera.scope_fraction = 0.5;
    frame.camera = camera;

    parity::InputState input{};
    input.move_order = 7;
    input.analog_x = -12;
    input.analog_y = 34;
    input.analog_z = 56;
    input.forward = true;
    input.run = true;
    input.look_yaw_deg = 1.25;
    input.look_pitch_deg = -2.5;
    frame.input = input;

    parity::TraceWriter writer;
    CHECK(writer.append(frame));
    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 1);
    if (read.trace.events.size() == 1 &&
        std::holds_alternative<parity::FrameSnapshot>(read.trace.events[0])) {
        const auto& observed = std::get<parity::FrameSnapshot>(read.trace.events[0]);
        CHECK(observed.identity.source == parity::SourceKind::retail);
        CHECK(observed.lane == parity::StateLane::presented);
        CHECK(observed.timestamp_ns == 9000000);
        CHECK(observed.pools.size() == 1);
        CHECK(observed.entities.size() == 2);
        CHECK(observed.complete_entity_pools ==
              std::vector<std::uint32_t>({0, 1}));
        if (observed.pools.size() == 1) {
            CHECK(observed.pools[0].base_address == 0x12340000);
            CHECK(observed.pools[0].stride == 0x388);
            CHECK(observed.pools[0].used_count == 2);
            CHECK(observed.pools[0].live_count == 1);
        }
        if (observed.entities.size() == 2) {
            CHECK(observed.entities[0].identity.name == "OrganicOne");
            CHECK(observed.entities[0].identity.pool == -1);
            CHECK(observed.entities[0].identity.slot == -1);
            CHECK(observed.entities[0].identity.bms_id == -1);
            CHECK(observed.entities[0].transform.has_value());
            if (observed.entities[0].transform) {
                CHECK(observed.entities[0].transform->raw_encoding ==
                      parity::RawTransformEncoding::fixed_point_16_16_bam32);
                CHECK(observed.entities[0].transform->raw_heading == 0x40000000);
                CHECK(observed.entities[0].transform->yaw_deg == 90.0);
            }
            CHECK(observed.entities[0].kind == parity::EntityKind::organic);
            CHECK(observed.entities[0].health == 75);
            CHECK(observed.entities[0].armor == 25);
            CHECK(observed.entities[0].alive);
            CHECK(observed.entities[0].body_anim_slot == -1);
            CHECK(observed.entities[0].anim_state == -1);
            CHECK(observed.entities[0].anim_phase_ticks == -1);
            CHECK(observed.entities[0].equipped_adm_index == -1);
            CHECK(observed.entities[1].kind == parity::EntityKind::marker);
            CHECK(!observed.entities[1].armor.has_value());
        }
        CHECK(observed.player.has_value());
        if (observed.player) {
            CHECK(!observed.player->armor.has_value());
        }
        CHECK(observed.weapon.has_value());
        CHECK(observed.camera.has_value());
        CHECK(observed.input.has_value());
        if (observed.weapon) {
            CHECK(observed.weapon->name == "WPN_M4AUTO");
            CHECK(observed.weapon->primary.z == -183.0);
            CHECK(observed.weapon->render_fov == 80.0);
            CHECK(observed.weapon->clip == 30);
        }
        if (observed.camera) {
            CHECK(observed.camera->scope_fraction == 0.5);
        }
        if (observed.input) {
            CHECK(observed.input->analog_y == 34);
            CHECK(observed.input->forward);
            CHECK(observed.input->look_pitch_deg == -2.5);
        }
    } else {
        CHECK(false);
    }
}

void network_datagram_round_trips_endpoints_direction_and_bytes() {
    parity::NetworkDatagram datagram{};
    datagram.identity = {
        parity::SourceKind::retail, parity::RunRole::client, "retail-client"};
    datagram.timestamp_ns = 123456789;
    datagram.socket_id = 42;
    datagram.total_size = 1200;
    datagram.truncated = true;
    datagram.direction = parity::DatagramDirection::outbound;
    datagram.source = {"127.0.0.1", 32768};
    datagram.destination = {"192.0.2.10", 7597};
    datagram.payload = {0x41, 0x00, 0xff, 0x7e};

    parity::TraceWriter writer;
    CHECK(writer.append(datagram));
    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 1);
    if (read.trace.events.size() == 1) {
        CHECK(std::holds_alternative<parity::NetworkDatagram>(read.trace.events[0]));
        if (std::holds_alternative<parity::NetworkDatagram>(read.trace.events[0])) {
            const auto& observed = std::get<parity::NetworkDatagram>(read.trace.events[0]);
            CHECK(observed.timestamp_ns == 123456789);
            CHECK(observed.identity.source == parity::SourceKind::retail);
            CHECK(observed.identity.role == parity::RunRole::client);
            CHECK(observed.identity.stream_id == "retail-client");
            CHECK(observed.socket_id == 42);
            CHECK(observed.total_size == 1200);
            CHECK(observed.truncated);
            CHECK(observed.direction == parity::DatagramDirection::outbound);
            CHECK(observed.source.address == "127.0.0.1");
            CHECK(observed.source.port == 32768);
            CHECK(observed.destination.address == "192.0.2.10");
            CHECK(observed.destination.port == 7597);
            CHECK(observed.payload == parity::ByteString({0x41, 0x00, 0xff, 0x7e}));
        }
    }
}

void decoded_network_event_preserves_semantic_fields() {
    parity::DecodedNetworkEvent event{};
    event.identity = {
        parity::SourceKind::retail, parity::RunRole::client, "retail-client"};
    event.simulation_tick = 44;
    event.direction = parity::DatagramDirection::inbound;
    event.message_id = 0x5a;
    event.name = "weapon-loadout";
    event.fields = {
        {"avatar_class", std::uint64_t{8}, 0.0, parity::Severity::error},
        {"raw", parity::ByteString{0x08, 0x09, 0xff}, 0.0, parity::Severity::warning},
    };

    parity::TraceWriter writer;
    CHECK(writer.append(event));
    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 1);
    if (read.trace.events.size() == 1 &&
        std::holds_alternative<parity::DecodedNetworkEvent>(read.trace.events[0])) {
        const auto& observed = std::get<parity::DecodedNetworkEvent>(read.trace.events[0]);
        CHECK(observed.identity.stream_id == "retail-client");
        CHECK(observed.simulation_tick == 44);
        CHECK(observed.direction == parity::DatagramDirection::inbound);
        CHECK(observed.message_id == 0x5a);
        CHECK(observed.name == "weapon-loadout");
        CHECK(observed.fields.size() == 2);
        if (observed.fields.size() == 2) {
            CHECK(std::get<std::uint64_t>(observed.fields[0].value) == 8);
            CHECK(std::get<parity::ByteString>(observed.fields[1].value) ==
                  parity::ByteString({0x08, 0x09, 0xff}));
        }
    } else {
        CHECK(false);
    }
}

void mutation_audit_round_trips_precondition_and_result() {
    parity::MutationAudit audit{};
    audit.identity = {
        parity::SourceKind::retail, parity::RunRole::client, "retail-client"};
    audit.simulation_tick = 88;
    audit.timestamp_ns = 123000;
    audit.target = "pool[0].slot[1].health";
    audit.operation = "set";
    audit.result = parity::MutationResult::unverified;
    audit.before = std::int64_t{75};
    audit.after = std::int64_t{60};
    audit.detail = "write may have occurred; verification failed";

    parity::TraceWriter writer;
    CHECK(writer.append(audit));
    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 1);
    if (read.trace.events.size() == 1 &&
        std::holds_alternative<parity::MutationAudit>(read.trace.events[0])) {
        const auto& observed = std::get<parity::MutationAudit>(read.trace.events[0]);
        CHECK(observed.identity.stream_id == "retail-client");
        CHECK(observed.simulation_tick == 88);
        CHECK(observed.timestamp_ns == 123000);
        CHECK(observed.target == "pool[0].slot[1].health");
        CHECK(observed.operation == "set");
        CHECK(observed.result == parity::MutationResult::unverified);
        CHECK(std::get<std::int64_t>(observed.before) == 75);
        CHECK(std::get<std::int64_t>(observed.after) == 60);
        CHECK(observed.detail ==
              "write may have occurred; verification failed");
    } else {
        CHECK(false);
    }
}

void diagnostic_event_round_trips_severity_and_context() {
    parity::DiagnosticEvent diagnostic{};
    diagnostic.identity = {
        parity::SourceKind::retail, parity::RunRole::client, "retail-client"};
    diagnostic.simulation_tick = 99;
    diagnostic.timestamp_ns = 456000;
    diagnostic.severity = parity::Severity::warning;
    diagnostic.code = "weapon.pointer";
    diagnostic.message = "definition is unreadable";
    diagnostic.context = {
        {"entity.slot", std::uint64_t{7}, 0.0, parity::Severity::info},
    };

    parity::TraceWriter writer;
    CHECK(writer.append(diagnostic));
    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 1);
    if (read.trace.events.size() == 1 &&
        std::holds_alternative<parity::DiagnosticEvent>(read.trace.events[0])) {
        const auto& observed = std::get<parity::DiagnosticEvent>(read.trace.events[0]);
        CHECK(observed.identity.stream_id == "retail-client");
        CHECK(observed.simulation_tick == 99);
        CHECK(observed.timestamp_ns == 456000);
        CHECK(observed.severity == parity::Severity::warning);
        CHECK(observed.code == "weapon.pointer");
        CHECK(observed.message == "definition is unreadable");
        CHECK(observed.context.size() == 1);
        if (observed.context.size() == 1) {
            CHECK(std::get<std::uint64_t>(observed.context[0].value) == 7);
        }
    } else {
        CHECK(false);
    }
}

std::uint32_t read_u32_literal(const std::vector<std::uint8_t>& bytes,
                               std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void checksum_failure_preserves_prior_complete_chunks() {
    parity::TraceWriter writer;
    CHECK(writer.append(parity::RunMetadata{
        "run", "producer", "build", "scenario", 1}));
    CHECK(writer.append(parity::Checkpoint{"ready", 0, 7}));

    std::vector<std::uint8_t> corrupt = writer.bytes();
    const std::uint32_t first_payload_size = read_u32_literal(corrupt, 12);
    const std::size_t second_chunk = 8 + 12 + first_payload_size;
    CHECK(second_chunk + 12 < corrupt.size());
    corrupt.back() ^= 0x80;

    const parity::TraceReadResult read = parity::read_trace(corrupt);
    CHECK(read.status == parity::TraceReadStatus::checksum_mismatch);
    CHECK(read.error_offset == second_chunk);
    CHECK(read.trace.events.size() == 1);
    if (read.trace.events.size() == 1) {
        CHECK(std::holds_alternative<parity::RunMetadata>(read.trace.events[0]));
    }
}

void truncated_tail_preserves_prior_complete_chunks() {
    parity::TraceWriter writer;
    CHECK(writer.append(parity::RunMetadata{
        "run", "producer", "build", "scenario", 1}));
    CHECK(writer.append(parity::Checkpoint{"ready", 0, 7}));

    std::vector<std::uint8_t> truncated = writer.bytes();
    const std::uint32_t first_payload_size = read_u32_literal(truncated, 12);
    const std::size_t second_chunk = 8 + 12 + first_payload_size;
    truncated.resize(truncated.size() - 5);

    const parity::TraceReadResult read = parity::read_trace(truncated);
    CHECK(read.status == parity::TraceReadStatus::truncated_tail);
    CHECK(read.error_offset == second_chunk);
    CHECK(read.trace.events.size() == 1);
    if (read.trace.events.size() == 1) {
        CHECK(std::holds_alternative<parity::RunMetadata>(read.trace.events[0]));
    }
}

}  // namespace

int main() {
    run_metadata_round_trips_through_the_versioned_trace();
    checkpoint_and_frame_snapshot_preserve_typed_values();
    frame_snapshot_round_trips_explicit_runtime_state();
    network_datagram_round_trips_endpoints_direction_and_bytes();
    decoded_network_event_preserves_semantic_fields();
    mutation_audit_round_trips_precondition_and_result();
    diagnostic_event_round_trips_severity_and_context();
    checksum_failure_preserves_prior_complete_chunks();
    truncated_tail_preserves_prior_complete_chunks();
    std::printf("parity_trace: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
