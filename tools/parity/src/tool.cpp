#include <opennova/parity_tool/tool.h>

#include "evidence_compare.h"
#include "network_enrichment.h"
#include "pcapng_export.h"

#include <parity/parity.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace opennova::parity_tool {
namespace {

namespace parity = opennova::parity;

struct LoadedTrace {
    parity::TraceReadResult read{};
    bool io_error{};
    std::string detail{};
};

void print_usage(std::ostream& stream) {
    stream << "usage:\n"
           << "  opennova_parity_tool validate <trace.ontrace>\n"
           << "  opennova_parity_tool dump <trace.ontrace> [output.txt]\n"
           << "  opennova_parity_tool export-pcapng <trace.ontrace> <output.pcapng>\n"
           << "  opennova_parity_tool compare <reference.ontrace> <candidate.ontrace>\n";
}

LoadedTrace load_trace(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {{}, true, "could not read trace file: " + path.string()};
    }
    const std::streamsize size = stream.tellg();
    if (size < 0) {
        return {{}, true, "could not read trace file size: " + path.string()};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (size != 0) {
        stream.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!stream && !stream.eof()) {
        return {{}, true, "could not read trace file: " + path.string()};
    }
    return {parity::read_trace(bytes), false, {}};
}

std::string trace_status_detail(const parity::TraceReadResult& read) {
    switch (read.status) {
    case parity::TraceReadStatus::complete:
        return "complete";
    case parity::TraceReadStatus::truncated_tail:
        return "incomplete trace: " + read.detail;
    case parity::TraceReadStatus::invalid_header:
        return "invalid trace: " + read.detail;
    case parity::TraceReadStatus::unsupported_version:
        return "invalid trace version: " + read.detail;
    case parity::TraceReadStatus::checksum_mismatch:
        return "invalid trace checksum: " + read.detail;
    case parity::TraceReadStatus::malformed_chunk:
        return "invalid trace event: " + read.detail;
    }
    return "invalid trace";
}

bool require_complete(const LoadedTrace& loaded, std::ostream& error) {
    if (loaded.io_error) {
        error << "error: " << loaded.detail << '\n';
        return false;
    }
    if (!loaded.read.complete()) {
        error << "error: " << trace_status_detail(loaded.read)
              << " offset=" << loaded.read.error_offset << '\n';
        return false;
    }
    return true;
}

std::string identity_text(const parity::ProducerIdentity& identity) {
    std::ostringstream stream;
    stream << "source=" << static_cast<unsigned>(identity.source)
           << " role=" << static_cast<unsigned>(identity.role)
           << " stream=" << identity.stream_id;
    return stream.str();
}

std::string scalar_text(const parity::ScalarValue& value) {
    return std::visit(
        [](const auto& scalar) {
            using Scalar = std::decay_t<decltype(scalar)>;
            std::ostringstream stream;
            if constexpr (std::is_same_v<Scalar, bool>) {
                stream << (scalar ? "true" : "false");
            } else if constexpr (std::is_same_v<Scalar, parity::ByteString>) {
                stream << "<bytes:" << scalar.size() << '>';
            } else {
                stream << scalar;
            }
            return stream.str();
        },
        value);
}

void dump_fields(const std::vector<parity::Field>& fields,
                 std::ostream& output) {
    for (const parity::Field& field : fields) {
        output << ' ' << field.name << '=' << scalar_text(field.value);
    }
}

const char* direction_text(parity::DatagramDirection direction) {
    return direction == parity::DatagramDirection::inbound ? "inbound"
                                                            : "outbound";
}

const char* classification_text(parity::ComparisonClassification value) {
    switch (value) {
    case parity::ComparisonClassification::clean:
        return "clean";
    case parity::ComparisonClassification::warnings:
        return "warnings";
    case parity::ComparisonClassification::mismatch:
        return "mismatch";
    case parity::ComparisonClassification::invalid_evidence:
        return "invalid-evidence";
    }
    return "invalid-evidence";
}

const char* severity_text(parity::Severity value) {
    switch (value) {
    case parity::Severity::info:
        return "info";
    case parity::Severity::warning:
        return "warning";
    case parity::Severity::error:
        return "error";
    case parity::Severity::fatal:
        return "fatal";
    }
    return "fatal";
}

void dump_transform(const char* prefix,
                    const parity::EntityTransform& transform,
                    std::ostream& output) {
    output << ' ' << prefix << ".position=(" << transform.x << ','
           << transform.y << ',' << transform.z << ')'
           << ' ' << prefix << ".rotation=(" << transform.yaw_deg << ','
           << transform.pitch_deg << ',' << transform.roll_deg << ')';
    if (transform.raw_encoding != parity::RawTransformEncoding::unavailable) {
        output << ' ' << prefix << ".raw_position=(" << transform.raw_x
               << ',' << transform.raw_y << ',' << transform.raw_z << ')'
               << ' ' << prefix << ".raw_rotation=("
               << transform.raw_heading << ',' << transform.raw_pitch << ','
               << transform.raw_roll << ')';
    }
}

void dump_pose(const char* prefix,
               const parity::PoseState& pose,
               std::ostream& output) {
    output << ' ' << prefix << "=(" << pose.x << ',' << pose.y << ','
           << pose.z << ';' << pose.yaw_deg << ',' << pose.pitch_deg << ','
           << pose.roll_deg << ')';
}

void dump_frame_state(const parity::FrameSnapshot& frame,
                      std::ostream& output) {
    output << " pools=" << frame.pools.size();
    for (const parity::PoolState& pool : frame.pools) {
        output << " pool[" << pool.index << "].stride=" << pool.stride
               << " pool[" << pool.index << "].capacity=" << pool.capacity
               << " pool[" << pool.index << "].used=" << pool.used_count
               << " pool[" << pool.index << "].live=";
        if (pool.live_count) {
            output << *pool.live_count;
        } else {
            output << "unavailable";
        }
    }
    output << " complete_pools=";
    if (frame.complete_entity_pools.empty()) {
        output << "<none>";
    } else {
        for (std::size_t index = 0;
             index < frame.complete_entity_pools.size();
             ++index) {
            if (index != 0) {
                output << ',';
            }
            output << frame.complete_entity_pools[index];
        }
    }
    output << " entities=" << frame.entities.size();

    if (!frame.player) {
        output << " player.coverage=unavailable";
    } else {
        const parity::PlayerState& player = *frame.player;
        output << " player.present=" << (player.present ? "true" : "false")
               << " player.pool=" << player.identity.pool
               << " player.slot=" << player.identity.slot
               << " player.wire_handle=" << player.identity.wire_handle
               << " player.bms_id=" << player.identity.bms_id
               << " player.ssn=" << player.identity.ssn
               << " player.net_id=" << player.identity.net_id
               << " player.owner=" << player.identity.owner_connection_id
               << " player.name=" << player.identity.name;
        if (player.identity.type_id) {
            output << " player.type_id=" << *player.identity.type_id;
        }
        if (player.transform) {
            dump_transform("player", *player.transform, output);
        } else {
            output << " player.transform=unavailable";
        }
        output << " player.health=" << player.health
               << " player.max_health=";
        if (player.max_health) {
            output << *player.max_health;
        } else {
            output << "unavailable";
        }
        output << " player.armor=";
        if (player.armor) {
            output << *player.armor;
        } else {
            output << "unavailable";
        }
        output << " player.team=" << player.team
               << " player.class=" << player.player_class
               << " player.adm=" << player.equipped_adm_index;
    }

    if (!frame.weapon) {
        output << " weapon.coverage=unavailable";
    } else {
        const parity::WeaponState& weapon = *frame.weapon;
        output << " weapon.present=" << (weapon.present ? "true" : "false")
               << " weapon.name=" << weapon.name
               << " weapon.special_hold=" << weapon.special_hold
               << " weapon.attack_anim=" << weapon.attack_anim;
        dump_pose("weapon.primary", weapon.primary, output);
        dump_pose("weapon.alternate", weapon.alternate, output);
        output << " weapon.fov=" << weapon.render_fov
               << " weapon.action=";
        if (weapon.action) {
            output << *weapon.action;
        } else {
            output << "unavailable";
        }
        output << " weapon.clip=";
        if (weapon.clip) {
            output << *weapon.clip;
        } else {
            output << "unavailable";
        }
        output << " weapon.reserve=";
        if (weapon.reserve) {
            output << *weapon.reserve;
        } else {
            output << "unavailable";
        }
    }

    if (!frame.camera) {
        output << " camera.coverage=unavailable";
    } else {
        const parity::CameraState& camera = *frame.camera;
        output << " camera.present=" << (camera.present ? "true" : "false")
               << " camera.fov=" << camera.fov_deg
               << " camera.third_person="
               << (camera.third_person ? "true" : "false")
               << " camera.scope="
               << (camera.scope_engaged ? "true" : "false")
               << " camera.scope_fraction=" << camera.scope_fraction;
        if (camera.transform) {
            dump_transform("camera", *camera.transform, output);
        }
    }

    if (!frame.input) {
        output << " input.coverage=unavailable";
    } else {
        const parity::InputState& input = *frame.input;
        output << " input.present=true input.move_order=" << input.move_order
               << " input.analog=(" << input.analog_x << ',' << input.analog_y
               << ',' << input.analog_z << ')'
               << " input.forward=" << (input.forward ? "true" : "false")
               << " input.back=" << (input.back ? "true" : "false")
               << " input.left=" << (input.left ? "true" : "false")
               << " input.right=" << (input.right ? "true" : "false")
               << " input.run=" << (input.run ? "true" : "false")
               << " input.crouch=" << (input.crouch ? "true" : "false")
               << " input.prone=" << (input.prone ? "true" : "false")
               << " input.jump=" << (input.jump ? "true" : "false")
               << " input.look=(" << input.look_yaw_deg << ','
               << input.look_pitch_deg << ')';
    }
}

void dump_event(const parity::Event& event,
                std::size_t index,
                std::ostream& output) {
    std::visit(
        [&](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            output << "event=" << index << ' ';
            if constexpr (std::is_same_v<Value, parity::RunMetadata>) {
                output << "type=run " << identity_text(value.identity)
                       << " scenario=" << value.scenario
                       << " mission=" << value.mission;
            } else if constexpr (std::is_same_v<Value, parity::FrameSnapshot>) {
                output << "type=frame " << identity_text(value.identity)
                       << " lane=" << static_cast<unsigned>(value.lane)
                       << " frame=" << value.frame_index
                       << " tick=" << value.simulation_tick;
                dump_frame_state(value, output);
            } else if constexpr (std::is_same_v<Value, parity::Checkpoint>) {
                output << "type=checkpoint " << identity_text(value.identity)
                       << " lane=" << static_cast<unsigned>(value.lane)
                       << " name=" << value.name
                       << " occurrence=" << value.occurrence;
            } else if constexpr (std::is_same_v<Value, parity::NetworkDatagram>) {
                output << "type=datagram " << identity_text(value.identity)
                       << " direction=" << direction_text(value.direction)
                       << " timestamp_ns=" << value.timestamp_ns
                       << " source=" << value.source.address << ':'
                       << value.source.port
                       << " destination=" << value.destination.address << ':'
                       << value.destination.port
                       << " payload_bytes=" << value.payload.size()
                       << " truncated=" << (value.truncated ? "true" : "false");
            } else if constexpr (std::is_same_v<Value, parity::DecodedNetworkEvent>) {
                output << "type=decoded-network "
                       << identity_text(value.identity)
                       << " direction=" << direction_text(value.direction)
                       << " message_id=" << value.message_id
                       << " name=" << value.name
                       << " fields=" << value.fields.size();
                dump_fields(value.fields, output);
            } else if constexpr (std::is_same_v<Value, parity::MutationAudit>) {
                output << "type=mutation " << identity_text(value.identity)
                       << " target=" << value.target
                       << " operation=" << value.operation
                       << " result=" << static_cast<unsigned>(value.result)
                       << " tick=" << value.simulation_tick
                       << " timestamp_ns=" << value.timestamp_ns
                       << " before=" << scalar_text(value.before)
                       << " after=" << scalar_text(value.after)
                       << " detail=" << value.detail;
            } else {
                static_assert(std::is_same_v<Value, parity::DiagnosticEvent>);
                output << "type=diagnostic " << identity_text(value.identity)
                       << " severity=" << static_cast<unsigned>(value.severity)
                       << " code=" << value.code
                       << " message=" << value.message;
                dump_fields(value.context, output);
            }
            output << '\n';
        },
        event);
}

int validate_command(const std::vector<std::string>& args,
                     std::ostream& output,
                     std::ostream& error) {
    if (args.size() != 2) {
        print_usage(error);
        return kExitUsageOrIo;
    }
    LoadedTrace loaded = load_trace(args[1]);
    if (!require_complete(loaded, error)) {
        return loaded.io_error ? kExitUsageOrIo : kExitInvalidEvidence;
    }
    detail::EnrichmentResult enriched =
        detail::enrich_network_events(std::move(loaded.read.trace));
    const detail::EvidenceValidationResult validation =
        detail::validate_evidence(enriched.trace);
    if (!validation.valid) {
        error << "error: invalid evidence: " << validation.detail << '\n';
        return kExitInvalidEvidence;
    }
    const bool has_warnings = enriched.has_undecodable_stream ||
                              validation.warning_diagnostics != 0 ||
                              validation.guided_action_warnings != 0;
    output << "status=" << (has_warnings ? "warnings" : "clean")
           << " events=" << enriched.trace.events.size()
           << " raw_datagrams=" << enriched.raw_datagrams
           << " decoded_network_events=" << enriched.decoded_events
           << " raw_without_completed_message="
           << enriched.raw_without_completed_message
           << " producers=" << validation.producers
           << " checkpoints=" << validation.checkpoints
           << " capture_intervals=" << validation.capture_intervals
           << " warning_diagnostics=" << validation.warning_diagnostics
           << " guided_action_warnings="
           << validation.guided_action_warnings
           << " path=" << args[1] << '\n';
    if (!validation.client_semantic_available) {
        output << "guided_actions=unavailable\n";
    } else {
        output << "guided_action.stance-change="
               << (validation.stance_change_observed ? "observed" : "missing")
               << " guided_action.fired-round="
               << (validation.fired_round_observed ? "observed" : "missing")
               << " guided_action.weapon-reload-request="
               << (validation.reload_request_observed ? "observed" : "missing")
               << '\n';
    }
    for (const detail::StreamCoverage& stream : enriched.streams) {
        output << "stream=" << stream.identity.stream_id
               << " raw=" << stream.raw_datagrams
               << " decoded=" << stream.decoded_events
               << " raw_without_completed_message="
               << stream.raw_without_completed_message()
               << " ports=" << stream.low_port << "<->" << stream.high_port
               << " typed_events_supplied="
               << (stream.typed_events_supplied ? "true" : "false")
               << '\n';
    }
    for (const parity::Event& event : enriched.trace.events) {
        const auto* diagnostic = std::get_if<parity::DiagnosticEvent>(&event);
        if (diagnostic != nullptr &&
            diagnostic->severity == parity::Severity::warning) {
            output << "diagnostic=" << diagnostic->code
                   << " severity=warning message=" << diagnostic->message
                   << '\n';
        }
    }
    return kExitClean;
}

int dump_command(const std::vector<std::string>& args,
                 std::ostream& output,
                 std::ostream& error) {
    if (args.size() != 2 && args.size() != 3) {
        print_usage(error);
        return kExitUsageOrIo;
    }
    LoadedTrace loaded = load_trace(args[1]);
    if (!require_complete(loaded, error)) {
        return loaded.io_error ? kExitUsageOrIo : kExitInvalidEvidence;
    }

    std::ofstream file;
    std::ostream* destination = &output;
    if (args.size() == 3) {
        file.open(args[2], std::ios::out | std::ios::trunc);
        if (!file) {
            error << "error: could not write dump file: " << args[2] << '\n';
            return kExitUsageOrIo;
        }
        destination = &file;
    }
    detail::EnrichmentResult enriched =
        detail::enrich_network_events(std::move(loaded.read.trace));
    for (std::size_t index = 0;
         index < enriched.trace.events.size();
         ++index) {
        dump_event(enriched.trace.events[index], index, *destination);
    }
    if (!*destination) {
        error << "error: could not write parity dump\n";
        return kExitUsageOrIo;
    }
    return kExitClean;
}

int export_pcapng_command(const std::vector<std::string>& args,
                          std::ostream& output,
                          std::ostream& error) {
    if (args.size() != 3) {
        print_usage(error);
        return kExitUsageOrIo;
    }
    LoadedTrace loaded = load_trace(args[1]);
    if (!require_complete(loaded, error)) {
        return loaded.io_error ? kExitUsageOrIo : kExitInvalidEvidence;
    }
    const detail::PcapngWriteResult result =
        detail::write_pcapng(loaded.read.trace, args[2]);
    if (!result.complete()) {
        error << "error: " << result.detail << '\n';
        return result.status == detail::PcapngWriteStatus::io_error
                   ? kExitUsageOrIo
                   : kExitInvalidEvidence;
    }
    output << "status=clean datagrams=" << result.datagrams
           << " path=" << args[2] << '\n';
    return kExitClean;
}

int compare_command(const std::vector<std::string>& args,
                    std::ostream& output,
                    std::ostream& error) {
    if (args.size() != 3) {
        print_usage(error);
        return kExitUsageOrIo;
    }
    LoadedTrace reference = load_trace(args[1]);
    if (!require_complete(reference, error)) {
        return reference.io_error ? kExitUsageOrIo : kExitInvalidEvidence;
    }
    LoadedTrace candidate = load_trace(args[2]);
    if (!require_complete(candidate, error)) {
        return candidate.io_error ? kExitUsageOrIo : kExitInvalidEvidence;
    }

    detail::EnrichmentResult enriched_reference =
        detail::enrich_network_events(std::move(reference.read.trace));
    detail::EnrichmentResult enriched_candidate =
        detail::enrich_network_events(std::move(candidate.read.trace));
    parity::ComparisonReport report = detail::compare_evidence(
        std::move(enriched_reference.trace),
        std::move(enriched_candidate.trace));
    if (report.classification ==
        parity::ComparisonClassification::invalid_evidence) {
        error << "error: invalid evidence";
        if (!report.differences.empty()) {
            error << ": " << report.differences.front().actual;
        }
        error << '\n';
        return kExitInvalidEvidence;
    }

    output << "status=" << classification_text(report.classification)
           << " differences=" << report.differences.size()
           << " reference=" << args[1]
           << " candidate=" << args[2] << '\n';
    for (const parity::Difference& difference : report.differences) {
        output << "severity=" << severity_text(difference.severity)
               << " checkpoint="
               << (difference.checkpoint.empty() ? "-"
                                                 : difference.checkpoint)
               << " path=" << difference.path
               << " expected=" << difference.expected
               << " actual=" << difference.actual;
        if (difference.absolute_tolerance != 0.0) {
            output << " tolerance=" << difference.absolute_tolerance;
        }
        output << '\n';
    }
    return report.exit_code();
}

}  // namespace

int run(const std::vector<std::string>& args,
        std::ostream& output,
        std::ostream& error) {
    if (args.empty()) {
        print_usage(error);
        return kExitUsageOrIo;
    }
    if (args[0] == "validate") {
        return validate_command(args, output, error);
    }
    if (args[0] == "dump") {
        return dump_command(args, output, error);
    }
    if (args[0] == "export-pcapng") {
        return export_pcapng_command(args, output, error);
    }
    if (args[0] == "compare") {
        return compare_command(args, output, error);
    }
    print_usage(error);
    return kExitUsageOrIo;
}

}  // namespace opennova::parity_tool
