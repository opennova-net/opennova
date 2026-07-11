#include <opennova/parity_tool/tool.h>
#include <parity/parity.h>

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace parity = opennova::parity;
namespace tool = opennova::parity_tool;
namespace fs = std::filesystem;
using namespace opennova;

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

std::vector<std::uint8_t> outer_encode(
    std::uint8_t opcode,
    std::vector<std::uint8_t> body) {
    if (!body.empty()) {
        nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
    }
    std::vector<std::uint8_t> stripped;
    stripped.reserve(body.size() + 1);
    stripped.push_back(opcode);
    stripped.insert(stripped.end(), body.begin(), body.end());
    std::vector<std::uint8_t> raw(stripped.size() + 16);
    std::size_t output_size = 0;
    if (napi_envelope_encode(
            stripped.data(),
            stripped.size(),
            raw.data(),
            raw.size(),
            &output_size) != 0) {
        return {};
    }
    raw.resize(output_size);
    return raw;
}

std::vector<std::uint8_t> protocol_payload(
    std::uint8_t opcode,
    std::uint8_t tag,
    const std::string& scrk) {
    opennova::ProtocolMessage message =
        opennova::make_protocol_message(tag, {0x11, 0x22, 0x33});
    opennova::ProtocolPacketHeader header{};
    header.session_id = 0x1234;
    std::vector<std::uint8_t> encoded;
    opennova::encode_protocol_packet_plaintext(
        header, {message}, scrk, encoded);
    return outer_encode(opcode, std::move(encoded));
}

parity::ProducerIdentity identity(const char* stream) {
    return {
        parity::SourceKind::retail,
        std::string{stream} == "stream-a" ? parity::RunRole::host
                                          : parity::RunRole::client,
        stream,
    };
}

parity::RunMetadata metadata(const char* stream) {
    parity::RunMetadata value{};
    value.run_id = "network-test-run";
    value.producer = "network-test";
    value.build_id = "synthetic";
    value.scenario = "player-combat-loop";
    value.identity = identity(stream);
    value.title = "joint-operations";
    value.expansion = "revx02";
    value.mission = "ASH_G3D.bms";
    return value;
}

parity::Checkpoint checkpoint(const char* stream,
                              const char* name,
                              std::uint64_t frame_index) {
    parity::Checkpoint value{name, 0, frame_index};
    value.identity = identity(stream);
    value.lane = parity::StateLane::presented;
    return value;
}

parity::FrameSnapshot frame(const char* stream,
                            std::uint64_t frame_index,
                            double x) {
    parity::FrameSnapshot value{};
    value.identity = identity(stream);
    value.lane = parity::StateLane::presented;
    value.frame_index = frame_index;
    parity::PlayerState player{};
    player.present = true;
    player.identity.name = "PlayerOne";
    player.transform = parity::EntityTransform{};
    player.transform->x = x;
    value.player = player;
    parity::WeaponState weapon{};
    weapon.present = true;
    weapon.name = "WPN_M4AUTO";
    value.weapon = weapon;
    return value;
}

parity::NetworkDatagram datagram(
    const char* stream,
    std::uint64_t timestamp,
    std::uint16_t source_port,
    std::uint16_t destination_port,
    parity::DatagramDirection direction,
    std::vector<std::uint8_t> payload) {
    parity::NetworkDatagram value{};
    value.identity = identity(stream);
    value.timestamp_ns = timestamp;
    value.socket_id = 1;
    value.total_size = static_cast<std::uint32_t>(payload.size());
    value.direction = direction;
    value.source = {"192.0.2.10", source_port};
    value.destination = {"198.51.100.20", destination_port};
    value.payload = std::move(payload);
    return value;
}

fs::path write_network_trace() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const fs::path path = fs::temp_directory_path() /
                          ("opennova-parity-network-" +
                           std::to_string(nonce) + ".ontrace");
    std::error_code ignored;
    fs::remove(path, ignored);

    const std::string key_a = "STREAM_A_KEY";
    const std::string key_b = "STREAM_B_KEY";
    opennova::ClientAuth client_a{};
    client_a.na = "alpha";
    client_a.ci = 1;
    client_a.ck = 2;
    client_a.scrk = key_a;
    opennova::ClientAuth client_b{};
    client_b.na = "bravo";
    client_b.ci = 3;
    client_b.ck = 4;
    client_b.scrk = key_b;
    const opennova::ServerAuth server_a = opennova::build_server_auth(
        client_a, 0x7f000001U, 32768, 0x55, key_a);
    const opennova::ServerAuth server_b = opennova::build_server_auth(
        client_b, 0x7f000001U, 32768, 0x56, key_b);

    parity::TraceWriter writer;
    CHECK(writer.append(metadata("stream-a")));
    CHECK(writer.append(metadata("stream-b")));
    CHECK(writer.append(checkpoint("stream-a", "capture-start", 1)));
    CHECK(writer.append(frame("stream-a", 1, 1.0)));
    CHECK(writer.append(checkpoint("stream-b", "capture-start", 1)));
    CHECK(writer.append(frame("stream-b", 1, 1.0)));

    // The two identities deliberately reuse the same ports and interleave
    // different SCRKs. A decoder keyed only by ports clobbers stream A's key.
    CHECK(writer.append(datagram(
        "stream-a", 100, 32768, 32769, parity::DatagramDirection::inbound,
        outer_encode(SESSION_OPCODE_SERVER_AUTH,
                     opennova::server_auth_to_bytes(server_a)))));
    CHECK(writer.append(datagram(
        "stream-b", 110, 32768, 32769, parity::DatagramDirection::inbound,
        outer_encode(SESSION_OPCODE_SERVER_AUTH,
                     opennova::server_auth_to_bytes(server_b)))));
    CHECK(writer.append(datagram(
        "stream-a", 200, 32769, 32768, parity::DatagramDirection::outbound,
        outer_encode(SESSION_OPCODE_CLIENT_AUTH,
                     opennova::client_auth_to_bytes(client_a)))));
    CHECK(writer.append(datagram(
        "stream-b", 210, 32769, 32768, parity::DatagramDirection::outbound,
        outer_encode(SESSION_OPCODE_CLIENT_AUTH,
                     opennova::client_auth_to_bytes(client_b)))));
    CHECK(writer.append(datagram(
        "stream-a", 300, 32768, 32769, parity::DatagramDirection::inbound,
        protocol_payload(
            SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0x5a, key_a))));
    CHECK(writer.append(datagram(
        "stream-b", 310, 32768, 32769, parity::DatagramDirection::inbound,
        protocol_payload(
            SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0x0a, key_b))));
    CHECK(writer.append(datagram(
        "stream-b", 400, 40000, 40001,
        parity::DatagramDirection::outbound, {0xde, 0xad, 0xbe, 0xef})));
    CHECK(writer.append(frame("stream-a", 2, 2.0)));
    CHECK(writer.append(frame("stream-b", 2, 2.0)));
    CHECK(writer.append(checkpoint("stream-a", "capture-end", 2)));
    CHECK(writer.append(checkpoint("stream-b", "capture-end", 2)));

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    const auto& bytes = writer.bytes();
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return path;
}

void dump_enriches_each_identity_in_stream_order_without_raw_comparison() {
    const fs::path path = write_network_trace();
    std::ostringstream output;
    std::ostringstream error;
    CHECK(tool::run({"dump", path.string()}, output, error) == 0);
    const std::string text = output.str();

    const std::size_t stream_a = text.find(
        "type=decoded-network source=1 role=2 stream=stream-a");
    const std::size_t stream_b = text.find(
        "type=decoded-network source=1 role=1 stream=stream-b");
    CHECK(stream_a != std::string::npos);
    CHECK(stream_b != std::string::npos);
    CHECK(stream_a < stream_b);
    CHECK(text.find("message_id=90 name=weapon-loadout", stream_a) !=
          std::string::npos);
    CHECK(text.find("message_id=10 name=per-frame-update", stream_b) !=
          std::string::npos);
    CHECK(text.find("capture.timestamp_ns=300", stream_a) !=
          std::string::npos);
    CHECK(text.find("source.port=32768", stream_a) != std::string::npos);
    CHECK(text.find("destination.port=32769", stream_a) !=
          std::string::npos);
    CHECK(text.find("decode.coverage=decoded", stream_a) !=
          std::string::npos);
    CHECK(text.find("de ad be ef") == std::string::npos);
    CHECK(error.str().empty());

    std::error_code ignored;
    fs::remove(path, ignored);
}

void validate_surfaces_raw_but_undecodable_coverage_as_a_warning() {
    const fs::path path = write_network_trace();
    std::ostringstream output;
    std::ostringstream error;
    CHECK(tool::run({"validate", path.string()}, output, error) == 0);
    CHECK(output.str().find("status=warnings") != std::string::npos);
    CHECK(output.str().find("raw_datagrams=7") != std::string::npos);
    CHECK(output.str().find("decoded_network_events=2") != std::string::npos);
    CHECK(output.str().find("raw_without_completed_message=5") !=
          std::string::npos);
    CHECK(output.str().find(
              "stream=stream-b raw=1 decoded=0 raw_without_completed_message=1 ports=40000<->40001") !=
          std::string::npos);
    CHECK(output.str().find("guided_action.stance-change=missing") !=
          std::string::npos);
    CHECK(output.str().find("guided_action.fired-round=missing") !=
          std::string::npos);
    CHECK(output.str().find(
              "guided_action.weapon-reload-request=missing") !=
          std::string::npos);
    CHECK(error.str().empty());

    std::error_code ignored;
    fs::remove(path, ignored);
}

}  // namespace

int main() {
    dump_enriches_each_identity_in_stream_order_without_raw_comparison();
    validate_surfaces_raw_but_undecodable_coverage_as_a_warning();
    std::printf("parity_tool_network: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
