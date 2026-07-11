#include <opennova/parity_tool/tool.h>
#include <parity/parity.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace parity = opennova::parity;
namespace tool = opennova::parity_tool;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

fs::path temporary_path(const char* label, const char* extension) {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return fs::temp_directory_path() /
           (std::string{"opennova-parity-tool-"} + label + "-" +
            std::to_string(nonce) + extension);
}

void write_bytes(const fs::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

parity::RunMetadata metadata() {
    parity::RunMetadata value{};
    value.run_id = "run";
    value.producer = "test";
    value.build_id = "synthetic";
    value.scenario = "player-combat-loop";
    value.started_unix_ns = 1;
    value.identity = {
        parity::SourceKind::retail,
        parity::RunRole::client,
        "baseline-client",
    };
    value.title = "joint-operations";
    value.expansion = "revx02";
    value.mission = "ASH_G3D.bms";
    return value;
}

parity::FrameSnapshot frame(const parity::ProducerIdentity& producer,
                            std::uint64_t frame_index,
                            double x) {
    parity::FrameSnapshot frame{};
    frame.identity = producer;
    frame.lane = parity::StateLane::presented;
    frame.frame_index = frame_index;
    frame.simulation_tick = frame_index * 10;
    frame.pools.push_back({0, 0x1000, 0x340, 64, 2, 1});
    frame.complete_entity_pools.push_back(0);
    parity::EntityState entity{};
    entity.identity.pool = 0;
    entity.identity.slot = 1;
    entity.identity.wire_handle = 7;
    entity.identity.name = "PlayerOne";
    entity.kind = parity::EntityKind::player;
    entity.transform = parity::EntityTransform{};
    entity.transform->x = x;
    entity.transform->y = 4.0;
    entity.transform->z = 2.0;
    entity.health = 87;
    entity.max_health = 100;
    entity.armor = 12;
    entity.team = 2;
    entity.player_class = 8;
    entity.alive = true;
    entity.equipped_adm_index = 74;
    frame.entities.push_back(entity);
    parity::PlayerState player{};
    player.present = true;
    player.identity = entity.identity;
    player.transform = entity.transform;
    player.health = 87;
    player.max_health = 100;
    player.armor = 12;
    player.team = 2;
    player.player_class = 8;
    player.equipped_adm_index = 74;
    frame.player = player;
    parity::WeaponState weapon{};
    weapon.present = true;
    weapon.name = "WPN_M4AUTO";
    weapon.special_hold = 6;
    weapon.attack_anim = 2;
    weapon.primary = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    weapon.alternate = {7.0, 8.0, 9.0, 10.0, 11.0, 12.0};
    weapon.render_fov = 80.0;
    weapon.action = "idle";
    weapon.clip = 17;
    weapon.reserve = 60;
    frame.weapon = weapon;
    parity::CameraState camera{};
    camera.present = true;
    camera.fov_deg = 80.0;
    camera.scope_engaged = true;
    camera.scope_fraction = 0.5;
    frame.camera = camera;
    parity::InputState input{};
    input.forward = true;
    input.run = true;
    input.look_yaw_deg = 15.0;
    frame.input = input;
    return frame;
}

std::vector<std::uint8_t> complete_trace_bytes(
    bool include_network = true,
    std::optional<parity::Severity> diagnostic_severity = std::nullopt) {
    parity::TraceWriter writer;
    CHECK(writer.append(metadata()));
    parity::Checkpoint checkpoint{"capture-start", 0, 1};
    checkpoint.identity = metadata().identity;
    checkpoint.lane = parity::StateLane::presented;
    CHECK(writer.append(checkpoint));
    CHECK(writer.append(frame(metadata().identity, 1, 1.0)));
    CHECK(writer.append(frame(metadata().identity, 2, 2.0)));
    checkpoint.name = "capture-end";
    checkpoint.frame_index = 2;
    CHECK(writer.append(checkpoint));
    if (diagnostic_severity.has_value()) {
        parity::DiagnosticEvent diagnostic{};
        diagnostic.identity = metadata().identity;
        diagnostic.severity = *diagnostic_severity;
        diagnostic.code = *diagnostic_severity >= parity::Severity::error
                              ? "capture.sample.failed"
                              : "capture.queue.dropped";
        diagnostic.message = "synthetic capture diagnostic";
        CHECK(writer.append(diagnostic));
    }
    if (!include_network) {
        return writer.bytes();
    }
    parity::NetworkDatagram datagram{};
    datagram.identity = metadata().identity;
    datagram.timestamp_ns = 123456789;
    datagram.socket_id = 9;
    datagram.total_size = 4;
    datagram.direction = parity::DatagramDirection::outbound;
    datagram.source = {"192.0.2.10", 32769};
    datagram.destination = {"198.51.100.20", 32768};
    datagram.payload = {0xde, 0xad, 0xbe, 0xef};
    CHECK(writer.append(datagram));
    parity::MutationAudit mutation{};
    mutation.identity = metadata().identity;
    mutation.target = "player.health";
    mutation.operation = "set";
    mutation.result = parity::MutationResult::applied;
    mutation.before = parity::ByteString{0xde, 0xad};
    mutation.after = std::int64_t{42};
    mutation.detail = "explicit write-enabled probe";
    CHECK(writer.append(mutation));
    return writer.bytes();
}

std::vector<std::uint8_t> bundle_trace_bytes(bool include_client_network,
                                             bool mismatch_run_id) {
    parity::RunMetadata host = metadata();
    host.identity.role = parity::RunRole::host;
    host.identity.stream_id = "baseline-host";
    parity::RunMetadata client = metadata();
    client.identity.stream_id = "baseline-client";
    if (mismatch_run_id) {
        client.run_id = "different-run";
    }
    parity::TraceWriter writer;
    CHECK(writer.append(host));
    CHECK(writer.append(client));
    for (const parity::RunMetadata* producer : {&host, &client}) {
        parity::Checkpoint checkpoint{"capture-start", 0, 1};
        checkpoint.identity = producer->identity;
        checkpoint.lane = parity::StateLane::presented;
        CHECK(writer.append(checkpoint));
        CHECK(writer.append(frame(producer->identity, 1, 1.0)));
        const double final_x = producer->identity.role == parity::RunRole::client
                                   ? 2.0
                                   : 1.0;
        CHECK(writer.append(frame(producer->identity, 2, final_x)));
        checkpoint.name = "capture-end";
        checkpoint.frame_index = 2;
        CHECK(writer.append(checkpoint));
    }
    const auto append_raw = [&writer](const parity::ProducerIdentity& producer,
                                      std::uint16_t port) {
        parity::NetworkDatagram datagram{};
        datagram.identity = producer;
        datagram.total_size = 1;
        datagram.source = {"192.0.2.1", port};
        datagram.destination = {
            "198.51.100.2", static_cast<std::uint16_t>(port + 1)};
        datagram.payload = {0xff};
        CHECK(writer.append(datagram));
    };
    append_raw(host.identity, 32000);
    if (include_client_network) {
        append_raw(client.identity, 33000);
    }
    return writer.bytes();
}

std::vector<std::uint8_t> trace_bytes_with_metadata(
    const parity::RunMetadata& value) {
    parity::TraceWriter writer;
    CHECK(writer.append(value));
    parity::Checkpoint checkpoint{"capture-start", 0, 1};
    checkpoint.identity = value.identity;
    checkpoint.lane = parity::StateLane::presented;
    CHECK(writer.append(checkpoint));
    CHECK(writer.append(frame(value.identity, 1, 1.0)));
    CHECK(writer.append(frame(value.identity, 2, 2.0)));
    checkpoint.name = "capture-end";
    checkpoint.frame_index = 2;
    CHECK(writer.append(checkpoint));
    return writer.bytes();
}

void validate_distinguishes_clean_invalid_and_io_evidence() {
    const fs::path complete_path = temporary_path("complete", ".ontrace");
    const fs::path truncated_path = temporary_path("truncated", ".ontrace");
    const fs::path missing_path = temporary_path("missing", ".ontrace");
    std::error_code ignored;
    fs::remove(complete_path, ignored);
    fs::remove(truncated_path, ignored);
    fs::remove(missing_path, ignored);

    const std::vector<std::uint8_t> complete = complete_trace_bytes(false);
    write_bytes(complete_path, complete);
    std::vector<std::uint8_t> truncated = complete;
    truncated.pop_back();
    write_bytes(truncated_path, truncated);

    std::ostringstream output;
    std::ostringstream error;
    CHECK(tool::run({"validate", complete_path.string()}, output, error) == 0);
    CHECK(output.str().find("status=clean") != std::string::npos);
    CHECK(output.str().find("events=5") != std::string::npos);
    CHECK(error.str().empty());

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    CHECK(tool::run({"validate", truncated_path.string()}, output, error) == 3);
    CHECK(error.str().find("incomplete") != std::string::npos);

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    CHECK(tool::run({"validate", missing_path.string()}, output, error) == 64);
    CHECK(error.str().find("could not read") != std::string::npos);

    const fs::path insufficient_path =
        temporary_path("insufficient", ".ontrace");
    parity::TraceWriter insufficient;
    CHECK(insufficient.append(metadata()));
    write_bytes(insufficient_path, insufficient.bytes());
    output.str({});
    output.clear();
    error.str({});
    error.clear();
    CHECK(tool::run({"validate", insufficient_path.string()}, output, error) ==
          3);
    CHECK(error.str().find("evidence") != std::string::npos ||
          error.str().find("checkpoint") != std::string::npos);

    const fs::path warning_path = temporary_path("warning", ".ontrace");
    write_bytes(warning_path,
                complete_trace_bytes(false, parity::Severity::warning));
    output.str({});
    output.clear();
    error.str({});
    error.clear();
    CHECK(tool::run({"validate", warning_path.string()}, output, error) == 0);
    CHECK(output.str().find("status=warnings") != std::string::npos);
    CHECK(output.str().find("capture.queue.dropped") != std::string::npos);

    const fs::path error_path = temporary_path("error", ".ontrace");
    write_bytes(error_path,
                complete_trace_bytes(false, parity::Severity::error));
    output.str({});
    output.clear();
    error.str({});
    error.clear();
    CHECK(tool::run({"validate", error_path.string()}, output, error) == 3);
    CHECK(error.str().find("capture.sample.failed") != std::string::npos);

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    CHECK(tool::run({}, output, error) == 64);
    CHECK(error.str().find("usage:") != std::string::npos);

    fs::remove(complete_path, ignored);
    fs::remove(truncated_path, ignored);
    fs::remove(insufficient_path, ignored);
    fs::remove(warning_path, ignored);
    fs::remove(error_path, ignored);
}

void dump_reports_network_context_without_printing_payload_bytes() {
    const fs::path path = temporary_path("dump", ".ontrace");
    std::error_code ignored;
    fs::remove(path, ignored);
    write_bytes(path, complete_trace_bytes());

    std::ostringstream output;
    std::ostringstream error;
    CHECK(tool::run({"dump", path.string()}, output, error) == 0);
    CHECK(output.str().find("timestamp_ns=123456789") != std::string::npos);
    CHECK(output.str().find("192.0.2.10:32769") != std::string::npos);
    CHECK(output.str().find("198.51.100.20:32768") != std::string::npos);
    CHECK(output.str().find("payload_bytes=4") != std::string::npos);
    CHECK(output.str().find("pool[0].stride=832") != std::string::npos);
    CHECK(output.str().find("complete_pools=0") != std::string::npos);
    CHECK(output.str().find("player.wire_handle=7") != std::string::npos);
    CHECK(output.str().find("player.position=(2,4,2)") != std::string::npos);
    CHECK(output.str().find("player.health=87") != std::string::npos);
    CHECK(output.str().find("weapon.name=WPN_M4AUTO") != std::string::npos);
    CHECK(output.str().find("weapon.primary=(1,2,3;4,5,6)") !=
          std::string::npos);
    CHECK(output.str().find("weapon.clip=17") != std::string::npos);
    CHECK(output.str().find("camera.present=true") != std::string::npos);
    CHECK(output.str().find("input.forward=true") != std::string::npos);
    CHECK(output.str().find("before=<bytes:2> after=42") !=
          std::string::npos);
    CHECK(output.str().find("detail=explicit write-enabled probe") !=
          std::string::npos);
    CHECK(output.str().find("de ad be ef") == std::string::npos);
    CHECK(error.str().empty());

    fs::remove(path, ignored);
}

void bundle_validation_pins_shared_context_and_network_coverage() {
    const fs::path path = temporary_path("bundle", ".ontrace");
    std::error_code ignored;
    fs::remove(path, ignored);
    std::ostringstream output;
    std::ostringstream error;

    write_bytes(path, bundle_trace_bytes(true, false));
    CHECK(tool::run({"validate", path.string()}, output, error) == 0);
    CHECK(output.str().find("status=warnings") != std::string::npos);
    CHECK(output.str().find("producers=2") != std::string::npos);

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    write_bytes(path, bundle_trace_bytes(false, false));
    CHECK(tool::run({"validate", path.string()}, output, error) == 3);
    CHECK(error.str().find("raw network") != std::string::npos);

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    write_bytes(path, bundle_trace_bytes(true, true));
    CHECK(tool::run({"validate", path.string()}, output, error) == 3);
    CHECK(error.str().find("run_id") != std::string::npos ||
          error.str().find("context") != std::string::npos);

    fs::remove(path, ignored);
}

void validation_requires_run_producer_and_build_provenance() {
    const fs::path path = temporary_path("provenance", ".ontrace");
    std::error_code ignored;
    fs::remove(path, ignored);
    std::ostringstream output;
    std::ostringstream error;
    for (const std::string& field : {std::string{"run_id"},
                                     std::string{"producer"},
                                     std::string{"build_id"}}) {
        parity::RunMetadata value = metadata();
        if (field == "run_id") {
            value.run_id.clear();
        } else if (field == "producer") {
            value.producer.clear();
        } else {
            value.build_id.clear();
        }
        write_bytes(path, trace_bytes_with_metadata(value));
        output.str({});
        output.clear();
        error.str({});
        error.clear();
        CHECK(tool::run({"validate", path.string()}, output, error) == 3);
        CHECK(error.str().find(field) != std::string::npos);
    }
    fs::remove(path, ignored);
}

}  // namespace

int main() {
    validate_distinguishes_clean_invalid_and_io_evidence();
    dump_reports_network_context_without_printing_payload_bytes();
    bundle_validation_pins_shared_context_and_network_coverage();
    validation_requires_run_producer_and_build_provenance();
    std::printf("parity_tool_cli: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
