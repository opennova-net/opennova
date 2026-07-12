#include <opennova/parity_tool/tool.h>
#include <parity/parity.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
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

using Message = std::pair<std::uint32_t, std::string>;

fs::path temporary_path(const char* label) {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return fs::temp_directory_path() /
           (std::string{"opennova-parity-compare-"} + label + "-" +
            std::to_string(nonce) + ".ontrace");
}

parity::ProducerIdentity identity(parity::SourceKind source,
                                  std::string stream) {
    return {source, parity::RunRole::client, std::move(stream)};
}

parity::RunMetadata metadata(const parity::ProducerIdentity& producer) {
    parity::RunMetadata value{};
    value.run_id = "synthetic-run";
    value.producer = "parity-tool-compare-test";
    value.build_id = "synthetic";
    value.scenario = "player-combat-loop";
    value.identity = producer;
    value.title = "joint-operations";
    value.expansion = "revx02";
    value.mission = "ASH_G3D.bms";
    return value;
}

parity::EntityTransform transform(double x, double yaw = 0.0) {
    parity::EntityTransform value{};
    value.x = x;
    value.y = 4.0;
    value.z = 2.0;
    value.yaw_deg = yaw;
    return value;
}

parity::FrameSnapshot frame(const parity::ProducerIdentity& producer,
                            std::uint64_t frame_index,
                            double x,
                            double yaw = 0.0) {
    parity::FrameSnapshot value{};
    value.identity = producer;
    value.lane = parity::StateLane::presented;
    value.frame_index = frame_index;
    value.simulation_tick = frame_index * 10;
    parity::PlayerState player{};
    player.present = true;
    player.identity.pool = 0;
    player.identity.slot = 1;
    player.identity.name = "PlayerOne";
    player.transform = transform(x, yaw);
    player.health = 100;
    value.player = player;
    parity::WeaponState weapon{};
    weapon.present = true;
    weapon.name = "WPN_M4AUTO";
    weapon.render_fov = 80.0;
    weapon.action = "idle";
    weapon.clip = 30;
    weapon.reserve = 90;
    value.weapon = weapon;
    return value;
}

parity::Checkpoint checkpoint(const parity::ProducerIdentity& producer,
                              std::string name,
                              std::uint64_t frame_index) {
    parity::Checkpoint value{std::move(name), 0, frame_index};
    value.identity = producer;
    value.lane = parity::StateLane::presented;
    return value;
}

parity::DecodedNetworkEvent decoded(
    const parity::ProducerIdentity& producer,
    const Message& message,
    std::uint64_t capture_noise) {
    parity::DecodedNetworkEvent event{};
    event.identity = producer;
    event.direction = message.first == 0x1d || message.first == 0x06 ||
                              message.first == 0x25
                          ? parity::DatagramDirection::outbound
                          : parity::DatagramDirection::inbound;
    event.message_id = message.first;
    event.name = message.second;
    event.fields = {
        {"capture.frame_index", capture_noise},
        {"capture.timestamp_ns", capture_noise * 1000},
        {"source.address", std::string{"0.0.0.0"}},
        {"source.port", capture_noise + 30000},
        {"payload.size", capture_noise + 1},
    };
    return event;
}

parity::NetworkDatagram raw_datagram(
    const parity::ProducerIdentity& producer,
    std::uint8_t byte,
    std::uint16_t port) {
    parity::NetworkDatagram datagram{};
    datagram.identity = producer;
    datagram.timestamp_ns = byte;
    datagram.total_size = 1;
    datagram.direction = parity::DatagramDirection::inbound;
    datagram.source = {"192.0.2.1", port};
    datagram.destination = {"198.51.100.2",
                            static_cast<std::uint16_t>(port + 1)};
    datagram.payload = {byte};
    return datagram;
}

parity::Trace trace(parity::SourceKind source,
                    std::string stream,
                    const std::vector<double>& manual_path,
                    const std::vector<Message>& messages) {
    const parity::ProducerIdentity producer = identity(source, std::move(stream));
    parity::Trace result{};
    result.events.emplace_back(metadata(producer));
    result.events.emplace_back(checkpoint(producer, "capture-start", 1));
    result.events.emplace_back(frame(producer, 1, 25.0));
    result.events.emplace_back(raw_datagram(producer, 0xaa, 32000));
    std::uint64_t noise = 10;
    for (const Message& message : messages) {
        result.events.emplace_back(decoded(producer, message, noise++));
    }
    std::uint64_t frame_index = 2;
    for (double x : manual_path) {
        result.events.emplace_back(frame(producer, frame_index++, x));
    }
    const std::uint64_t end_frame = frame_index - 1;
    result.events.emplace_back(
        checkpoint(producer, "capture-end", end_frame));
    return result;
}

void write_trace(const fs::path& path, const parity::Trace& trace) {
    parity::TraceWriter writer;
    for (const parity::Event& event : trace.events) {
        CHECK(writer.append(event));
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    const auto& bytes = writer.bytes();
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

int compare(const parity::Trace& reference,
            const parity::Trace& candidate,
            std::string& output_text,
            std::string& error_text) {
    const fs::path reference_path = temporary_path("reference");
    const fs::path candidate_path = temporary_path("candidate");
    write_trace(reference_path, reference);
    write_trace(candidate_path, candidate);
    std::ostringstream output;
    std::ostringstream error;
    const int status = tool::run(
        {"compare", reference_path.string(), candidate_path.string()},
        output,
        error);
    output_text = output.str();
    error_text = error.str();
    std::error_code ignored;
    fs::remove(reference_path, ignored);
    fs::remove(candidate_path, ignored);
    return status;
}

void manual_paths_counts_capture_fields_and_raw_bytes_do_not_mismatch() {
    const std::vector<Message> reference_messages{
        {0x5a, "weapon-loadout"},
        {0x5a, "weapon-loadout"},
        {0x49, "weapon-reload"},
        {0x0a, "per-frame-update"},
        {0x5a, "weapon-loadout"},
        {0x1d, "stance-change"},
        {0x06, "fired-round"},
        {0x25, "weapon-reload-request"},
    };
    const std::vector<Message> candidate_messages{
        {0x5a, "weapon-loadout"},
        {0x49, "weapon-reload"},
        {0x49, "weapon-reload"},
        {0x0a, "per-frame-update"},
        {0x5a, "weapon-loadout"},
        {0x1d, "stance-change"},
        {0x06, "fired-round"},
        {0x25, "weapon-reload-request"},
        {0x0a, "per-frame-update"},
        {0x5a, "weapon-loadout"},
    };
    const parity::Trace reference = trace(parity::SourceKind::retail,
                                          "baseline-client",
                                          {28.0, 31.0, 26.0},
                                          reference_messages);
    parity::Trace candidate = trace(parity::SourceKind::opennova,
                                    "candidate-client",
                                    {40.0, 27.0, 80.0, 29.0},
                                    candidate_messages);
    for (parity::Event& event : candidate.events) {
        if (auto* decoded_event =
                std::get_if<parity::DecodedNetworkEvent>(&event)) {
            decoded_event->fields[0].value = std::uint64_t{7000};
            decoded_event->fields[2].value = std::string{"203.0.113.9"};
        } else if (auto* datagram =
                       std::get_if<parity::NetworkDatagram>(&event)) {
            datagram->payload = {0x11};
            datagram->timestamp_ns = 999999;
            datagram->source.port = 45000;
            datagram->destination.port = 45001;
        }
    }

    std::string output;
    std::string error;
    CHECK(compare(reference, candidate, output, error) == 0);
    CHECK(output.find("status=clean") != std::string::npos);
    CHECK(error.empty());
}

void semantic_phase_order_still_detects_a_mismatch() {
    const std::vector<Message> expected{
        {0x5a, "weapon-loadout"},
        {0x49, "weapon-reload"},
        {0x0a, "per-frame-update"},
        {0x5a, "weapon-loadout"},
    };
    const std::vector<Message> reordered{
        {0x49, "weapon-reload"},
        {0x5a, "weapon-loadout"},
        {0x0a, "per-frame-update"},
        {0x49, "weapon-reload"},
    };
    const parity::Trace reference = trace(
        parity::SourceKind::retail, "baseline-client", {28.0}, expected);
    const parity::Trace candidate = trace(
        parity::SourceKind::opennova, "candidate-client", {29.0}, reordered);
    std::string output;
    std::string error;
    CHECK(compare(reference, candidate, output, error) == 2);
    CHECK(output.find("network.decoded.order") != std::string::npos);
}

void capture_start_anchor_is_strict_and_movement_is_required() {
    const std::vector<Message> messages{{0x5a, "weapon-loadout"}};
    const parity::Trace reference = trace(
        parity::SourceKind::retail, "baseline-client", {28.0}, messages);
    parity::Trace candidate = trace(
        parity::SourceKind::opennova, "candidate-client", {29.0}, messages);
    for (parity::Event& event : candidate.events) {
        auto* snapshot = std::get_if<parity::FrameSnapshot>(&event);
        if (snapshot != nullptr && snapshot->frame_index == 1) {
            snapshot->player->transform->x += 1.0;
        }
    }
    std::string output;
    std::string error;
    CHECK(compare(reference, candidate, output, error) == 2);
    CHECK(output.find("checkpoint=capture-start#0") != std::string::npos);
    CHECK(output.find("player.transform.x") != std::string::npos);

    candidate = trace(parity::SourceKind::opennova,
                      "candidate-client",
                      {25.0, 25.0},
                      messages);
    CHECK(compare(reference, candidate, output, error) == 3);
    CHECK(error.find("movement excursion") != std::string::npos);
}

void zero_position_sentinel_does_not_prove_movement() {
    const std::vector<Message> messages{{0x5a, "weapon-loadout"}};
    const parity::Trace reference = trace(
        parity::SourceKind::retail, "baseline-client", {28.0}, messages);
    parity::Trace candidate = trace(parity::SourceKind::opennova,
                                    "candidate-client",
                                    {25.0, 25.0},
                                    messages);
    for (parity::Event& event : candidate.events) {
        auto* snapshot = std::get_if<parity::FrameSnapshot>(&event);
        if (snapshot != nullptr && snapshot->frame_index == 2) {
            snapshot->player->transform->x = 0.0;
            snapshot->player->transform->y = 0.0;
            snapshot->player->transform->z = 0.0;
        }
    }

    std::string output;
    std::string error;
    CHECK(compare(reference, candidate, output, error) == 3);
    CHECK(error.find("movement excursion") != std::string::npos);
}

}  // namespace

int main() {
    manual_paths_counts_capture_fields_and_raw_bytes_do_not_mismatch();
    semantic_phase_order_still_detects_a_mismatch();
    capture_start_anchor_is_strict_and_movement_is_required();
    zero_position_sentinel_does_not_prove_movement();
    std::printf("parity_tool_compare: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
