#include "evidence_compare.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace opennova::parity_tool::detail {
namespace {

constexpr double kFixedPointPositionAllowance = 1.0 / 65536.0;
constexpr double kBam32AngleAllowanceDegrees =
    360.0 / 4294967296.0;
constexpr double kMinimumGuidedMovementExcursion = 0.25;

struct ProducerKey {
    parity::SourceKind source{parity::SourceKind::unknown};
    parity::RunRole role{parity::RunRole::unknown};
    std::string stream_id{};

    [[nodiscard]] auto as_tuple() const {
        return std::tie(source, role, stream_id);
    }

    [[nodiscard]] bool operator<(const ProducerKey& other) const {
        return as_tuple() < other.as_tuple();
    }

    [[nodiscard]] bool operator==(const ProducerKey& other) const {
        return as_tuple() == other.as_tuple();
    }
};

struct CaptureKey {
    ProducerKey producer{};
    parity::StateLane lane{parity::StateLane::unknown};
    std::uint64_t occurrence{};

    [[nodiscard]] auto as_tuple() const {
        return std::tie(producer.source,
                        producer.role,
                        producer.stream_id,
                        lane,
                        occurrence);
    }

    [[nodiscard]] bool operator<(const CaptureKey& other) const {
        return as_tuple() < other.as_tuple();
    }
};

struct CaptureInterval {
    const parity::Checkpoint* start{};
    const parity::Checkpoint* end{};
    std::size_t start_event{};
    std::size_t end_event{};
};

struct SemanticSegment {
    parity::RunRole role{parity::RunRole::unknown};
    parity::StateLane lane{parity::StateLane::unknown};
    std::uint64_t occurrence{};

    [[nodiscard]] auto as_tuple() const {
        return std::tie(role, lane, occurrence);
    }

    [[nodiscard]] bool operator<(const SemanticSegment& other) const {
        return as_tuple() < other.as_tuple();
    }
};

struct SemanticEvent {
    parity::DatagramDirection direction{parity::DatagramDirection::inbound};
    std::uint32_t message_id{};
    std::string name{};

    [[nodiscard]] auto as_tuple() const {
        return std::tie(direction, message_id, name);
    }

    [[nodiscard]] bool operator==(const SemanticEvent& other) const {
        return as_tuple() == other.as_tuple();
    }

    [[nodiscard]] bool operator<(const SemanticEvent& other) const {
        return as_tuple() < other.as_tuple();
    }
};

ProducerKey producer_key(const parity::ProducerIdentity& identity) {
    return {identity.source, identity.role, identity.stream_id};
}

bool same_identity(const parity::ProducerIdentity& left,
                   const parity::ProducerIdentity& right) {
    return producer_key(left) == producer_key(right);
}

const parity::ProducerIdentity& event_identity(const parity::Event& event) {
    return std::visit(
        [](const auto& value) -> const parity::ProducerIdentity& {
            return value.identity;
        },
        event);
}

std::string core_detail(const parity::ComparisonReport& report) {
    if (report.differences.empty()) {
        return "parity core rejected the trace";
    }
    const parity::Difference& difference = report.differences.front();
    return !difference.actual.empty() ? difference.actual : difference.path;
}

std::optional<std::size_t> selected_frame_index(
    const parity::Trace& trace,
    const parity::Checkpoint& checkpoint) {
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < trace.events.size(); ++index) {
        const auto* frame =
            std::get_if<parity::FrameSnapshot>(&trace.events[index]);
        if (frame == nullptr ||
            !same_identity(frame->identity, checkpoint.identity) ||
            frame->lane != checkpoint.lane ||
            frame->frame_index < checkpoint.frame_index) {
            continue;
        }
        if (!selected ||
            frame->frame_index <
                std::get<parity::FrameSnapshot>(trace.events[*selected])
                    .frame_index) {
            selected = index;
        }
    }
    return selected;
}

bool collect_capture_intervals(const parity::Trace& trace,
                               std::vector<CaptureInterval>& intervals,
                               std::string& detail) {
    std::map<CaptureKey, CaptureInterval> observed;
    for (std::size_t index = 0; index < trace.events.size(); ++index) {
        const auto* checkpoint =
            std::get_if<parity::Checkpoint>(&trace.events[index]);
        if (checkpoint == nullptr ||
            (checkpoint->name != "capture-start" &&
             checkpoint->name != "capture-end")) {
            continue;
        }
        const CaptureKey key{producer_key(checkpoint->identity),
                             checkpoint->lane,
                             checkpoint->occurrence};
        CaptureInterval& interval = observed[key];
        const bool is_start = checkpoint->name == "capture-start";
        const parity::Checkpoint*& phase =
            is_start ? interval.start : interval.end;
        if (phase != nullptr) {
            detail = "capture lifecycle checkpoint phase is duplicated";
            return false;
        }
        phase = checkpoint;
        if (is_start) {
            interval.start_event = index;
        } else {
            interval.end_event = index;
        }
    }
    if (observed.empty()) {
        detail = "capture-start/capture-end lifecycle evidence is missing";
        return false;
    }
    for (const auto& [key, interval] : observed) {
        (void)key;
        if (interval.start == nullptr || interval.end == nullptr) {
            detail = "capture lifecycle is missing its start or end checkpoint";
            return false;
        }
        if (interval.start_event >= interval.end_event) {
            detail = "capture-end must be recorded after capture-start";
            return false;
        }
        const auto start_frame = selected_frame_index(trace, *interval.start);
        const auto end_frame = selected_frame_index(trace, *interval.end);
        if (!start_frame || !end_frame) {
            detail = "capture lifecycle checkpoint does not select a frame";
            return false;
        }
        const auto& start =
            std::get<parity::FrameSnapshot>(trace.events[*start_frame]);
        const auto& end =
            std::get<parity::FrameSnapshot>(trace.events[*end_frame]);
        if (start.frame_index >= end.frame_index) {
            detail = "capture lifecycle requires distinct ordered frames";
            return false;
        }
        intervals.push_back(interval);
    }
    return true;
}

bool finite_transform(const parity::EntityTransform& transform) {
    return std::isfinite(transform.x) && std::isfinite(transform.y) &&
           std::isfinite(transform.z) && std::isfinite(transform.yaw_deg) &&
           std::isfinite(transform.pitch_deg) &&
           std::isfinite(transform.roll_deg);
}

bool has_available_position(const parity::EntityTransform& transform) {
    return transform.x != 0.0 || transform.y != 0.0 || transform.z != 0.0;
}

bool has_meaningful_excursion(const parity::Trace& trace,
                              const CaptureInterval& interval,
                              std::string& detail) {
    const auto start_index = selected_frame_index(trace, *interval.start);
    const auto end_index = selected_frame_index(trace, *interval.end);
    if (!start_index || !end_index) {
        detail = "capture lifecycle frame selection failed";
        return false;
    }
    const auto& start =
        std::get<parity::FrameSnapshot>(trace.events[*start_index]);
    const auto& end =
        std::get<parity::FrameSnapshot>(trace.events[*end_index]);
    if (!start.player || !start.player->transform || !end.player ||
        !end.player->transform) {
        detail = "capture lifecycle requires continuous player transforms";
        return false;
    }
    const parity::EntityTransform& anchor = *start.player->transform;
    if (!finite_transform(anchor)) {
        detail = "capture-start player transform is non-finite";
        return false;
    }

    double maximum_squared_distance = 0.0;
    std::size_t observed_frames = 0;
    for (const parity::Event& event : trace.events) {
        const auto* frame = std::get_if<parity::FrameSnapshot>(&event);
        if (frame == nullptr ||
            !same_identity(frame->identity, interval.start->identity) ||
            frame->lane != interval.start->lane ||
            frame->frame_index < start.frame_index ||
            frame->frame_index > end.frame_index) {
            continue;
        }
        if (!frame->player || !frame->player->transform ||
            !finite_transform(*frame->player->transform)) {
            detail = "capture lifecycle has a frame without a finite player transform";
            return false;
        }
        const auto& transform = *frame->player->transform;
        if (!has_available_position(transform)) {
            continue;
        }
        const double dx = transform.x - anchor.x;
        const double dy = transform.y - anchor.y;
        const double dz = transform.z - anchor.z;
        maximum_squared_distance =
            std::max(maximum_squared_distance, dx * dx + dy * dy + dz * dz);
        ++observed_frames;
    }
    if (observed_frames < 2 ||
        maximum_squared_distance <
            kMinimumGuidedMovementExcursion *
                kMinimumGuidedMovementExcursion) {
        detail = "capture lifecycle has no meaningful movement excursion";
        return false;
    }
    return true;
}

bool shared_bundle_context(const parity::RunMetadata& expected,
                           const parity::RunMetadata& actual,
                           std::string& field) {
    if (expected.run_id != actual.run_id) {
        field = "run_id";
    } else if (expected.title != actual.title) {
        field = "title";
    } else if (expected.expansion != actual.expansion) {
        field = "expansion";
    } else if (expected.mission != actual.mission) {
        field = "mission";
    } else if (expected.scenario != actual.scenario) {
        field = "scenario";
    } else {
        return true;
    }
    return false;
}

void clear_dynamic_transform(std::optional<parity::EntityTransform>& value) {
    value.reset();
}

void normalize_capture_state(parity::Trace& trace, std::string& detail) {
    std::vector<CaptureInterval> intervals;
    if (!collect_capture_intervals(trace, intervals, detail)) {
        return;
    }
    for (const CaptureInterval& interval : intervals) {
        const auto start_index = selected_frame_index(trace, *interval.start);
        const auto end_index = selected_frame_index(trace, *interval.end);
        if (!start_index || !end_index) {
            detail = "capture lifecycle frame selection changed unexpectedly";
            return;
        }
        auto& start =
            std::get<parity::FrameSnapshot>(trace.events[*start_index]);
        auto& end =
            std::get<parity::FrameSnapshot>(trace.events[*end_index]);

        // capture-start's typed local-player projection is the strict spawn
        // anchor. World entities and camera pose are dynamic observations, so
        // they do not become accidental absolute-position anchors.
        for (parity::EntityState& entity : start.entities) {
            clear_dynamic_transform(entity.transform);
        }
        if (start.camera) {
            clear_dynamic_transform(start.camera->transform);
        }

        // Manual path length and endpoint are operator-controlled. Their
        // invariant was established from continuous frames during validation;
        // the final typed state remains comparable except for dynamic poses.
        if (end.player) {
            clear_dynamic_transform(end.player->transform);
        }
        for (parity::EntityState& entity : end.entities) {
            clear_dynamic_transform(entity.transform);
        }
        if (end.camera) {
            clear_dynamic_transform(end.camera->transform);
        }
    }
    detail.clear();
}

parity::ComparisonReport invalid_report(std::string detail) {
    parity::ComparisonReport report{};
    report.classification = parity::ComparisonClassification::invalid_evidence;
    report.differences.push_back({parity::Severity::fatal,
                                  {},
                                  "trace",
                                  {},
                                  std::move(detail),
                                  0.0});
    return report;
}

using SemanticSequences =
    std::map<SemanticSegment, std::vector<SemanticEvent>>;

SemanticSequences semantic_sequences(const parity::Trace& trace) {
    std::vector<CaptureInterval> intervals;
    std::string ignored;
    (void)collect_capture_intervals(trace, intervals, ignored);
    SemanticSequences sequences;
    for (const CaptureInterval& interval : intervals) {
        const SemanticSegment segment{interval.start->identity.role,
                                      interval.start->lane,
                                      interval.start->occurrence};
        std::vector<SemanticEvent>& events = sequences[segment];
        for (std::size_t index = interval.start_event + 1;
             index < interval.end_event;
             ++index) {
            const auto* decoded =
                std::get_if<parity::DecodedNetworkEvent>(&trace.events[index]);
            if (decoded == nullptr ||
                !same_identity(decoded->identity, interval.start->identity)) {
                continue;
            }
            const SemanticEvent semantic{
                decoded->direction, decoded->message_id, decoded->name};
            if (events.empty() || !(events.back() == semantic)) {
                events.push_back(semantic);
            }
        }
    }
    return sequences;
}

bool is_subsequence(const std::vector<SemanticEvent>& shorter,
                    const std::vector<SemanticEvent>& longer) {
    std::size_t next = 0;
    for (const SemanticEvent& event : longer) {
        if (next < shorter.size() && shorter[next] == event) {
            ++next;
        }
    }
    return next == shorter.size();
}

std::set<SemanticEvent> semantic_set(
    const std::vector<SemanticEvent>& events) {
    return {events.begin(), events.end()};
}

std::string sequence_text(const std::vector<SemanticEvent>& events) {
    if (events.empty()) {
        return "<none>";
    }
    std::ostringstream stream;
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (index != 0) {
            stream << " > ";
        }
        stream << (events[index].direction == parity::DatagramDirection::inbound
                       ? "in"
                       : "out")
               << ':' << events[index].message_id << '/' << events[index].name;
    }
    return stream.str();
}

void append_semantic_differences(parity::ComparisonReport& report,
                                 const parity::Trace& reference,
                                 const parity::Trace& candidate) {
    const SemanticSequences expected = semantic_sequences(reference);
    const SemanticSequences actual = semantic_sequences(candidate);
    std::set<SemanticSegment> segments;
    for (const auto& [segment, events] : expected) {
        (void)events;
        segments.insert(segment);
    }
    for (const auto& [segment, events] : actual) {
        (void)events;
        segments.insert(segment);
    }
    const std::vector<SemanticEvent> empty;
    for (const SemanticSegment& segment : segments) {
        const auto expected_found = expected.find(segment);
        const auto actual_found = actual.find(segment);
        const auto& expected_events = expected_found == expected.end()
                                          ? empty
                                          : expected_found->second;
        const auto& actual_events = actual_found == actual.end()
                                        ? empty
                                        : actual_found->second;
        const bool same_vocabulary =
            semantic_set(expected_events) == semantic_set(actual_events);
        const bool compatible_order =
            is_subsequence(expected_events, actual_events) ||
            is_subsequence(actual_events, expected_events);
        if (same_vocabulary && compatible_order) {
            continue;
        }
        report.differences.push_back({
            parity::Severity::error,
            "capture-start#" + std::to_string(segment.occurrence),
            "network.decoded.order",
            sequence_text(expected_events),
            sequence_text(actual_events),
            0.0,
        });
        report.classification = parity::ComparisonClassification::mismatch;
    }
}

}  // namespace

EvidenceValidationResult validate_evidence(const parity::Trace& trace) {
    const parity::ComparisonReport core = parity::compare_traces(trace, trace);
    if (core.classification != parity::ComparisonClassification::clean) {
        return {false, 0, 0, 0, 0, core_detail(core)};
    }

    std::set<ProducerKey> producers;
    std::vector<const parity::RunMetadata*> metadata;
    std::size_t hosts = 0;
    std::size_t clients = 0;
    for (const parity::Event& event : trace.events) {
        const auto* value = std::get_if<parity::RunMetadata>(&event);
        if (value == nullptr) {
            continue;
        }
        if (value->run_id.empty() || value->producer.empty() ||
            value->build_id.empty()) {
            const char* field = value->run_id.empty()
                                    ? "run_id"
                                    : (value->producer.empty() ? "producer"
                                                               : "build_id");
            return {false,
                    0,
                    0,
                    0,
                    0,
                    std::string{"producer metadata "} + field + " is empty"};
        }
        if (!producers.insert(producer_key(value->identity)).second) {
            return {false,
                    producers.size(),
                    0,
                    0,
                    0,
                    "producer metadata is duplicated"};
        }
        metadata.push_back(value);
        hosts += value->identity.role == parity::RunRole::host ? 1U : 0U;
        clients += value->identity.role == parity::RunRole::client ? 1U : 0U;
    }
    if (metadata.empty()) {
        return {false, 0, 0, 0, 0, "producer metadata is missing"};
    }
    for (std::size_t index = 1; index < metadata.size(); ++index) {
        std::string field;
        if (!shared_bundle_context(*metadata.front(), *metadata[index], field)) {
            return {false,
                    producers.size(),
                    0,
                    0,
                    0,
                    "bundle producer metadata context differs at " + field};
        }
    }
    if (producers.size() > 1 &&
        (producers.size() != 2 || hosts != 1 || clients != 1)) {
        return {false,
                producers.size(),
                0,
                0,
                0,
                "a parity bundle must contain one unique host and one unique client"};
    }

    std::size_t warning_diagnostics = 0;
    for (const parity::Event& event : trace.events) {
        if (producers.count(producer_key(event_identity(event))) == 0) {
            return {false,
                    producers.size(),
                    0,
                    0,
                    warning_diagnostics,
                    "event provenance has no matching producer metadata"};
        }
        const auto* diagnostic = std::get_if<parity::DiagnosticEvent>(&event);
        if (diagnostic == nullptr) {
            continue;
        }
        if (diagnostic->severity >= parity::Severity::error) {
            return {false,
                    producers.size(),
                    0,
                    0,
                    warning_diagnostics,
                    "diagnostic " + diagnostic->code + ": " +
                        diagnostic->message};
        }
        if (diagnostic->severity == parity::Severity::warning) {
            ++warning_diagnostics;
        }
    }

    std::set<ProducerKey> checkpoint_producers;
    std::size_t checkpoints = 0;
    for (const parity::Event& event : trace.events) {
        const auto* checkpoint = std::get_if<parity::Checkpoint>(&event);
        if (checkpoint == nullptr) {
            continue;
        }
        ++checkpoints;
        checkpoint_producers.insert(producer_key(checkpoint->identity));
        const auto selected = selected_frame_index(trace, *checkpoint);
        if (!selected) {
            return {false,
                    producers.size(),
                    checkpoints,
                    0,
                    warning_diagnostics,
                    "checkpoint does not select a frame"};
        }
        const auto& frame =
            std::get<parity::FrameSnapshot>(trace.events[*selected]);
        if (!frame.player || !frame.player->present) {
            return {false,
                    producers.size(),
                    checkpoints,
                    0,
                    warning_diagnostics,
                    "checkpoint is missing required player evidence"};
        }
        if (!frame.weapon || !frame.weapon->present) {
            return {false,
                    producers.size(),
                    checkpoints,
                    0,
                    warning_diagnostics,
                    "checkpoint is missing required weapon evidence"};
        }
    }
    if (checkpoint_producers != producers) {
        return {false,
                producers.size(),
                checkpoints,
                0,
                warning_diagnostics,
                "every producer requires checkpointed state evidence"};
    }

    std::vector<CaptureInterval> intervals;
    std::string capture_detail;
    if (!collect_capture_intervals(trace, intervals, capture_detail)) {
        return {false,
                producers.size(),
                checkpoints,
                0,
                warning_diagnostics,
                std::move(capture_detail)};
    }
    std::set<ProducerKey> capture_producers;
    bool guided_client_interval = false;
    for (const CaptureInterval& interval : intervals) {
        capture_producers.insert(producer_key(interval.start->identity));
        const bool is_guided_actor =
            interval.start->identity.role == parity::RunRole::client &&
            interval.start->lane == parity::StateLane::presented;
        guided_client_interval |= is_guided_actor;
        if (is_guided_actor &&
            !has_meaningful_excursion(trace, interval, capture_detail)) {
            return {false,
                    producers.size(),
                    checkpoints,
                    intervals.size(),
                    warning_diagnostics,
                    std::move(capture_detail)};
        }
    }
    if (capture_producers != producers) {
        return {false,
                producers.size(),
                checkpoints,
                intervals.size(),
                warning_diagnostics,
                "every producer requires a complete capture lifecycle"};
    }
    if (clients != 0 && !guided_client_interval) {
        return {false,
                producers.size(),
                checkpoints,
                intervals.size(),
                warning_diagnostics,
                "guided client presented capture lifecycle is missing"};
    }

    if (producers.size() > 1 &&
        metadata.front()->scenario == "player-combat-loop") {
        std::map<ProducerKey, std::size_t> raw_by_producer;
        for (const parity::Event& event : trace.events) {
            if (const auto* datagram =
                    std::get_if<parity::NetworkDatagram>(&event)) {
                ++raw_by_producer[producer_key(datagram->identity)];
            }
        }
        for (const ProducerKey& producer : producers) {
            if (raw_by_producer[producer] == 0) {
                return {false,
                        producers.size(),
                        checkpoints,
                        intervals.size(),
                        warning_diagnostics,
                        "player-combat-loop bundle producer is missing raw network evidence"};
            }
        }
    }

    EvidenceValidationResult result{true,
                                    producers.size(),
                                    checkpoints,
                                    intervals.size(),
                                    warning_diagnostics,
                                    {}};
    for (const CaptureInterval& interval : intervals) {
        if (interval.start->identity.role != parity::RunRole::client ||
            interval.start->lane != parity::StateLane::presented) {
            continue;
        }
        for (std::size_t index = interval.start_event + 1;
             index < interval.end_event;
             ++index) {
            const auto* decoded =
                std::get_if<parity::DecodedNetworkEvent>(&trace.events[index]);
            if (decoded == nullptr ||
                !same_identity(decoded->identity, interval.start->identity)) {
                continue;
            }
            result.client_semantic_available = true;
            if (decoded->direction != parity::DatagramDirection::outbound) {
                continue;
            }
            result.stance_change_observed |= decoded->message_id == 0x1d;
            result.fired_round_observed |= decoded->message_id == 0x06;
            result.reload_request_observed |= decoded->message_id == 0x25;
        }
    }
    if (result.client_semantic_available) {
        result.guided_action_warnings =
            (result.stance_change_observed ? 0U : 1U) +
            (result.fired_round_observed ? 0U : 1U) +
            (result.reload_request_observed ? 0U : 1U);
    }
    return result;
}

parity::ComparisonReport compare_evidence(parity::Trace reference,
                                          parity::Trace candidate) {
    const EvidenceValidationResult reference_validation =
        validate_evidence(reference);
    if (!reference_validation.valid) {
        return invalid_report("reference: " + reference_validation.detail);
    }
    const EvidenceValidationResult candidate_validation =
        validate_evidence(candidate);
    if (!candidate_validation.valid) {
        return invalid_report("candidate: " + candidate_validation.detail);
    }

    std::string detail;
    normalize_capture_state(reference, detail);
    if (!detail.empty()) {
        return invalid_report("reference: " + detail);
    }
    normalize_capture_state(candidate, detail);
    if (!detail.empty()) {
        return invalid_report("candidate: " + detail);
    }

    parity::ComparisonOptions options{};
    options.normalized_position_alignment_allowance =
        kFixedPointPositionAllowance;
    options.normalized_angle_alignment_allowance_deg =
        kBam32AngleAllowanceDegrees;
    parity::ComparisonReport report =
        parity::compare_traces(reference, candidate, options);
    if (report.classification ==
        parity::ComparisonClassification::invalid_evidence) {
        return report;
    }
    append_semantic_differences(report, reference, candidate);
    const std::size_t reference_warnings =
        reference_validation.warning_diagnostics +
        reference_validation.guided_action_warnings;
    const std::size_t candidate_warnings =
        candidate_validation.warning_diagnostics +
        candidate_validation.guided_action_warnings;
    if (reference_warnings != 0 || candidate_warnings != 0) {
        report.differences.push_back({
            parity::Severity::warning,
            {},
            "evidence.warning_count",
            std::to_string(reference_warnings),
            std::to_string(candidate_warnings),
            0.0,
        });
        if (report.classification == parity::ComparisonClassification::clean) {
            report.classification = parity::ComparisonClassification::warnings;
        }
    }
    return report;
}

}  // namespace opennova::parity_tool::detail
