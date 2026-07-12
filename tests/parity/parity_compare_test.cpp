#include <parity/parity.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>

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

parity::FrameSnapshot frame(parity::SourceKind source,
                            std::uint64_t frame_index,
                            std::uint64_t tick,
                            int health,
                            double fov,
                            parity::RunRole role = parity::RunRole::client,
                            std::string stream_id = {}) {
    parity::FrameSnapshot result{};
    if (stream_id.empty()) {
        stream_id = source == parity::SourceKind::retail ? "retail-client"
                                                         : "opennova-client";
    }
    result.identity = {
        source, role, std::move(stream_id)};
    result.lane = parity::StateLane::presented;
    result.frame_index = frame_index;
    result.simulation_tick = tick;
    result.complete_entity_pools = {0, 1};
    result.pools.push_back({0,
                            source == parity::SourceKind::retail
                                ? 0x12340000ULL
                                : 0xabcdef00ULL,
                            source == parity::SourceKind::retail ? 0x388U : 0x240U,
                            256,
                            2});
    parity::PlayerState player{};
    player.present = true;
    player.identity.pool = 0;
    player.identity.slot = 1;
    player.identity.name = "PlayerOne";
    player.health = health;
    player.max_health = 100;
    player.team = 2;
    parity::EntityTransform player_transform{};
    player_transform.raw_encoding =
        parity::RawTransformEncoding::fixed_point_16_16_bam32;
    player_transform.raw_x = 1024;
    player_transform.raw_y = 2048;
    player_transform.raw_z = 256;
    player_transform.raw_heading = 8192;
    player_transform.x = 10.0;
    player_transform.y = 20.0;
    player_transform.z = 2.5;
    player_transform.yaw_deg = 45.0;
    player.transform = player_transform;
    result.player = player;
    parity::WeaponState weapon{};
    weapon.present = true;
    weapon.name = "M4";
    weapon.special_hold = 2;
    weapon.attack_anim = 7;
    weapon.primary = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    weapon.alternate = {7.0, 8.0, 9.0, 10.0, 11.0, 12.0};
    weapon.render_fov = 55.0;
    weapon.action = "idle";
    weapon.clip = 30;
    weapon.reserve = 90;
    result.weapon = weapon;
    parity::CameraState camera{};
    camera.present = true;
    camera.transform = parity::EntityTransform{};
    camera.fov_deg = fov;
    result.camera = camera;
    parity::InputState input{};
    input.move_order = 11;
    input.analog_x = 100;
    input.analog_y = -25;
    input.forward = true;
    input.run = true;
    input.look_yaw_deg = 3.0;
    input.look_pitch_deg = -1.0;
    result.input = input;

    parity::EntityState infantry{};
    infantry.identity.pool = 0;
    infantry.identity.slot = 10;
    infantry.identity.bms_id = 101;
    infantry.identity.name = "Infantry";
    infantry.kind = parity::EntityKind::organic;
    infantry.health = 50;
    infantry.max_health = 50;
    infantry.alive = true;
    infantry.transform = player_transform;
    parity::EntityState crate{};
    crate.identity.pool = 1;
    crate.identity.slot = 20;
    crate.identity.bms_id = 202;
    crate.identity.name = "AmmoCrate";
    crate.kind = parity::EntityKind::item;
    crate.health = 10;
    crate.max_health = 10;
    crate.alive = true;
    crate.transform = player_transform;
    result.entities = {infantry, crate};
    return result;
}

parity::FrameSnapshot& selected_frame(parity::Trace& trace) {
    for (parity::Event& event : trace.events) {
        if (auto* snapshot = std::get_if<parity::FrameSnapshot>(&event)) {
            return *snapshot;
        }
    }
    std::abort();
}

bool has_difference(const parity::ComparisonReport& report,
                    const std::string& path) {
    return std::any_of(
        report.differences.begin(),
        report.differences.end(),
        [&path](const parity::Difference& difference) {
            return difference.path == path;
        });
}

void observe_all_entity_identifiers(parity::EntityIdentity& identity) {
    identity.pool = 0;
    identity.slot = 10;
    identity.wire_handle = 0x1234;
    identity.bms_id = 101;
    identity.ssn = 51;
    identity.net_id = 61;
    identity.owner_connection_id = 71;
}

using IdentityMutation = void (*)(parity::EntityIdentity&);

parity::Trace trace_with_frame(parity::SourceKind source,
                               std::uint64_t frame_index,
                               std::uint64_t tick,
                               int health,
                               double fov,
                               parity::RunRole role = parity::RunRole::client,
                               std::string stream_id = {}) {
    parity::Trace trace{};
    if (stream_id.empty()) {
        stream_id = source == parity::SourceKind::retail ? "retail-client"
                                                         : "opennova-client";
    }
    parity::RunMetadata metadata{};
    metadata.run_id = source == parity::SourceKind::retail ? "retail" : "opennova";
    metadata.producer = "producer";
    metadata.build_id = "build";
    metadata.scenario = "scenario";
    metadata.identity = {
        source, role, stream_id};
    metadata.title = "joint-operations";
    metadata.expansion = "revx02";
    metadata.mission = "mission-a";
    trace.events.emplace_back(std::move(metadata));
    parity::Checkpoint checkpoint{"mission-ready", 0, frame_index};
    checkpoint.identity = {source, role, stream_id};
    checkpoint.lane = parity::StateLane::presented;
    trace.events.emplace_back(std::move(checkpoint));
    trace.events.emplace_back(
        frame(source, frame_index, tick, health, fov, role, stream_id));
    return trace;
}

void check_observed_identity_mismatch(const std::string& path,
                                      IdentityMutation mutate) {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    observe_all_entity_identifiers(
        selected_frame(reference).entities[0].identity);
    observe_all_entity_identifiers(
        selected_frame(candidate).entities[0].identity);
    mutate(selected_frame(candidate).entities[0].identity);

    const parity::ComparisonReport report =
        parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, path));
}

void check_identity_coverage_warning(const std::string& path,
                                     IdentityMutation make_unavailable) {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    observe_all_entity_identifiers(
        selected_frame(reference).entities[0].identity);
    observe_all_entity_identifiers(
        selected_frame(candidate).entities[0].identity);
    make_unavailable(selected_frame(candidate).entities[0].identity);

    const parity::ComparisonReport report =
        parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(has_difference(report, path + ".coverage"));
    CHECK(!has_difference(report, path));
}

void append_trace(parity::Trace& destination, parity::Trace source) {
    destination.events.insert(destination.events.end(),
                              std::make_move_iterator(source.events.begin()),
                              std::make_move_iterator(source.events.end()));
}

void comparator_aligns_by_checkpoint_and_classifies_severity() {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);

    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    parity::ComparisonReport report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::clean);
    CHECK(report.exit_code() == 0);
    CHECK(has_difference(report, "pools[0].stride.source_layout"));

    candidate = trace_with_frame(
        parity::SourceKind::opennova, 700, 9000, 74, 80.0);
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(report.exit_code() == 2);
    const auto health_difference = std::find_if(
        report.differences.begin(),
        report.differences.end(),
        [](const parity::Difference& difference) {
            return difference.path == "player.health";
        });
    CHECK(health_difference != report.differences.end());
    if (health_difference != report.differences.end()) {
        CHECK(health_difference->checkpoint == "mission-ready#0");
        CHECK(health_difference->severity == parity::Severity::error);
    }

    candidate = trace_with_frame(
        parity::SourceKind::opennova, 700, 9000, 75, 81.0);
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(report.exit_code() == 0);
    const auto fov_difference = std::find_if(
        report.differences.begin(),
        report.differences.end(),
        [](const parity::Difference& difference) {
            return difference.path == "camera.fov_deg";
        });
    CHECK(fov_difference != report.differences.end());
    if (fov_difference != report.differences.end()) {
        CHECK(fov_difference->severity == parity::Severity::warning);
    }

    candidate.events.erase(candidate.events.begin() + 1);
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);
    CHECK(report.exit_code() == 3);

    candidate = trace_with_frame(
        parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    std::get<parity::RunMetadata>(candidate.events[0]).expansion = "jox01";
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);
    CHECK(report.exit_code() == 3);

    candidate = trace_with_frame(
        parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    std::get<parity::RunMetadata>(candidate.events[0]).mission = "mission-b";
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);
    CHECK(report.exit_code() == 3);

    candidate = trace_with_frame(
        parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    std::get<parity::RunMetadata>(candidate.events[0]).identity.role =
        parity::RunRole::host;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);
    CHECK(report.exit_code() == 3);

    candidate = trace_with_frame(
        parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    std::get<parity::RunMetadata>(candidate.events[0]).title = "delta-force";
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);
    CHECK(report.exit_code() == 3);

    candidate = trace_with_frame(
        parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    std::get<parity::RunMetadata>(candidate.events[0]).scenario = "scenario-b";
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);
    CHECK(report.exit_code() == 3);
}

void checkpoint_markers_select_one_snapshot_independent_of_sampling_rate() {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    reference.events.insert(reference.events.begin() + 2,
                            frame(parity::SourceKind::retail,
                                  101,
                                  6201,
                                  10,
                                  80.0));

    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    candidate.events.emplace_back(
        frame(parity::SourceKind::opennova, 701, 9001, 20, 80.0));
    candidate.events.emplace_back(
        frame(parity::SourceKind::opennova, 702, 9002, 30, 80.0));

    parity::ComparisonReport report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::clean);
    CHECK(has_difference(report, "pools[0].stride.source_layout"));

    reference =
        trace_with_frame(parity::SourceKind::retail, 101, 6201, 75, 80.0);
    std::get<parity::Checkpoint>(reference.events[1]).frame_index = 100;
    candidate =
        trace_with_frame(parity::SourceKind::opennova, 702, 9002, 75, 80.0);
    std::get<parity::Checkpoint>(candidate.events[1]).frame_index = 700;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::clean);

    std::get<parity::FrameSnapshot>(candidate.events[2]).frame_index = 699;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);
    CHECK(report.exit_code() == 3);
}

void comparator_aligns_repeated_metadata_by_role_lane_and_checkpoint() {
    parity::Trace reference{};
    append_trace(reference,
                 trace_with_frame(parity::SourceKind::retail,
                                  100,
                                  6200,
                                  75,
                                  80.0,
                                  parity::RunRole::host,
                                  "baseline-host"));
    append_trace(reference,
                 trace_with_frame(parity::SourceKind::retail,
                                  200,
                                  6300,
                                  60,
                                  80.0,
                                  parity::RunRole::client,
                                  "baseline-client"));

    parity::Trace candidate{};
    append_trace(candidate,
                 trace_with_frame(parity::SourceKind::opennova,
                                  700,
                                  9000,
                                  75,
                                  80.0,
                                  parity::RunRole::host,
                                  "candidate-host"));
    append_trace(candidate,
                 trace_with_frame(parity::SourceKind::retail,
                                  800,
                                  9100,
                                  60,
                                  80.0,
                                  parity::RunRole::client,
                                  "candidate-client"));

    parity::ComparisonReport report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::clean);

    std::get<parity::FrameSnapshot>(candidate.events[5]).player->health = 59;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, "player.health"));
}

void typed_checkpoint_state_uses_explicit_policies_and_entity_mapping() {
    const parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);

    selected_frame(candidate).player->transform->raw_x += 1;
    parity::ComparisonReport report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, "player.transform.raw_x"));

    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).player->transform->raw_encoding =
        parity::RawTransformEncoding::unavailable;
    selected_frame(candidate).player->transform->raw_x += 1;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(has_difference(report, "player.transform.raw.coverage"));
    CHECK(!has_difference(report, "player.transform.raw_x"));

    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).player->transform->x += 0.25;
    parity::ComparisonOptions options{};
    options.normalized_position_alignment_allowance = 0.5;
    report = parity::compare_traces(reference, candidate, options);
    CHECK(report.classification == parity::ComparisonClassification::clean);

    selected_frame(candidate).player->transform->x += 0.5;
    report = parity::compare_traces(reference, candidate, options);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, "player.transform.x"));
    const auto transform_difference = std::find_if(
        report.differences.begin(),
        report.differences.end(),
        [](const parity::Difference& difference) {
            return difference.path == "player.transform.x";
        });
    if (transform_difference != report.differences.end()) {
        CHECK(transform_difference->absolute_tolerance == 0.5);
    }

    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).weapon->clip = 29;
    report = parity::compare_traces(reference, candidate);
    CHECK(has_difference(report, "weapon.clip"));
    CHECK(report.exit_code() == 2);

    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).input->analog_x = 99;
    report = parity::compare_traces(reference, candidate);
    CHECK(has_difference(report, "input.analog_x"));
    CHECK(report.exit_code() == 2);

    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    std::reverse(selected_frame(candidate).entities.begin(),
                 selected_frame(candidate).entities.end());
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::clean);

    selected_frame(candidate).entities[0].health = 9;
    report = parity::compare_traces(reference, candidate);
    CHECK(has_difference(report, "entities[bms:202].health"));
    CHECK(report.exit_code() == 2);

    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).entities[1].transform->x += 0.75;
    options.normalized_position_alignment_allowance = 0.5;
    report = parity::compare_traces(reference, candidate, options);
    CHECK(has_difference(report, "entities[bms:202].transform.x"));
}

void matching_one_identifier_does_not_hide_observed_identity_mismatches() {
    check_observed_identity_mismatch(
        "entities[bms:101].identity.pool",
        [](parity::EntityIdentity& identity) { identity.pool = 1; });
    check_observed_identity_mismatch(
        "entities[bms:101].identity.slot",
        [](parity::EntityIdentity& identity) { identity.slot = 11; });
    check_observed_identity_mismatch(
        "entities[bms:101].identity.wire_handle",
        [](parity::EntityIdentity& identity) {
            identity.wire_handle = 0x4321;
        });
    check_observed_identity_mismatch(
        "entities[bms:101].identity.bms_id",
        [](parity::EntityIdentity& identity) { identity.bms_id = 102; });
    check_observed_identity_mismatch(
        "entities[bms:101].identity.ssn",
        [](parity::EntityIdentity& identity) { identity.ssn = 52; });
    check_observed_identity_mismatch(
        "entities[bms:101].identity.net_id",
        [](parity::EntityIdentity& identity) { identity.net_id = 62; });
    check_observed_identity_mismatch(
        "entities[bms:101].identity.owner_connection_id",
        [](parity::EntityIdentity& identity) {
            identity.owner_connection_id = 72;
        });
    check_observed_identity_mismatch(
        "entities[bms:101].identity.owner_connection_id",
        [](parity::EntityIdentity& identity) {
            identity.owner_connection_id = 0;
        });
}

void unavailable_entity_identifiers_are_reported_as_coverage() {
    check_identity_coverage_warning(
        "entities[bms:101].identity.pool",
        [](parity::EntityIdentity& identity) { identity.pool = -1; });
    check_identity_coverage_warning(
        "entities[bms:101].identity.slot",
        [](parity::EntityIdentity& identity) { identity.slot = -1; });
    check_identity_coverage_warning(
        "entities[bms:101].identity.wire_handle",
        [](parity::EntityIdentity& identity) { identity.wire_handle = 0; });
    check_identity_coverage_warning(
        "entities[bms:101].identity.bms_id",
        [](parity::EntityIdentity& identity) { identity.bms_id = 0; });
    check_identity_coverage_warning(
        "entities[bms:101].identity.ssn",
        [](parity::EntityIdentity& identity) { identity.ssn = 0; });
    check_identity_coverage_warning(
        "entities[bms:101].identity.net_id",
        [](parity::EntityIdentity& identity) { identity.net_id = 0; });
}

void player_identity_fields_use_the_same_observation_policy() {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).player->identity.slot = 2;

    parity::ComparisonReport report =
        parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, "player.identity.slot"));

    selected_frame(candidate).player->identity.slot = -1;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(has_difference(report, "player.identity.slot.coverage"));
}

void unavailable_optional_observations_are_coverage_not_zero_values() {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(reference).player->armor = 25;
    selected_frame(candidate).player->armor.reset();
    selected_frame(reference).entities[0].armor = 10;
    selected_frame(candidate).entities[0].armor.reset();

    parity::ComparisonReport report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(report.exit_code() == 0);
    CHECK(has_difference(report, "player.armor.coverage"));
    CHECK(!has_difference(report, "player.armor"));
    CHECK(has_difference(report, "entities[bms:101].armor.coverage"));
    CHECK(!has_difference(report, "entities[bms:101].armor"));

    reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(reference).player->max_health = 0;
    selected_frame(candidate).player->max_health.reset();

    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(report.exit_code() == 0);
    CHECK(has_difference(report, "player.max_health.coverage"));
    CHECK(!has_difference(report, "player.max_health"));

    selected_frame(candidate).player->max_health = 0;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::clean);

    selected_frame(candidate).player->max_health = 1;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, "player.max_health"));

    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).weapon.reset();
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification ==
          parity::ComparisonClassification::invalid_evidence);

    reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(candidate).camera.reset();
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(has_difference(report, "camera.coverage"));

    reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);
    selected_frame(reference).entities[0].hidden = false;
    selected_frame(reference).entities[0].anim_state = 0;
    selected_frame(reference).entities[0].anim_phase_ticks = 0;
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(has_difference(report, "entities[bms:101].hidden.coverage"));
    CHECK(has_difference(report, "entities[bms:101].anim_state.coverage"));
    CHECK(has_difference(report,
                         "entities[bms:101].anim_phase_ticks.coverage"));
}

void entity_membership_is_compared_only_for_mutually_complete_pools() {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);

    parity::EntityState extra{};
    extra.identity.pool = 2;
    extra.identity.slot = 1;
    extra.identity.bms_id = 303;
    extra.identity.name = "CandidateOnlyVehicle";
    extra.kind = parity::EntityKind::vehicle;
    extra.health = 100;
    extra.alive = true;
    selected_frame(candidate).entities.push_back(extra);
    selected_frame(candidate).complete_entity_pools.push_back(2);

    parity::ComparisonReport report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::warnings);
    CHECK(report.exit_code() == 0);
    CHECK(has_difference(report, "entities.pool[2].coverage"));

    selected_frame(reference).complete_entity_pools.push_back(2);
    report = parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, "entities[bms:303].present"));
}

void stable_entity_ids_win_over_reordered_pool_slots() {
    parity::Trace reference =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::Trace candidate =
        trace_with_frame(parity::SourceKind::opennova, 700, 9000, 75, 80.0);

    selected_frame(reference).entities[1].identity.pool = 0;
    selected_frame(reference).entities[1].identity.slot = 11;
    selected_frame(candidate).entities[1].identity.pool = 0;
    selected_frame(candidate).entities[1].identity.slot = 10;
    selected_frame(candidate).entities[0].identity.slot = 11;
    selected_frame(reference).entities[0].identity.wire_handle = 10;
    selected_frame(reference).entities[1].identity.wire_handle = 11;
    selected_frame(candidate).entities[0].identity.wire_handle = 11;
    selected_frame(candidate).entities[1].identity.wire_handle = 10;
    for (parity::EntityState& entity : selected_frame(reference).entities) {
        entity.identity.type_id = 5305;
        entity.identity.name = "duplicate-weak-identity";
    }
    for (parity::EntityState& entity : selected_frame(candidate).entities) {
        entity.identity.type_id = 5305;
        entity.identity.name = "duplicate-weak-identity";
    }

    const parity::ComparisonReport report =
        parity::compare_traces(reference, candidate);
    CHECK(report.classification == parity::ComparisonClassification::mismatch);
    CHECK(has_difference(report, "entities[bms:101].identity.slot"));
    CHECK(has_difference(report, "entities[bms:202].identity.slot"));
    CHECK(!has_difference(report, "entities[bms:101].health"));
    CHECK(!has_difference(report, "entities[bms:202].health"));
    CHECK(!has_difference(report, "entities[bms:101].present"));
    CHECK(!has_difference(report, "entities[bms:202].present"));
}

void pool_slot_identity_is_reflexive_for_complete_pool_entities() {
    parity::Trace trace =
        trace_with_frame(parity::SourceKind::retail, 100, 6200, 75, 80.0);
    parity::EntityIdentity& identity =
        selected_frame(trace).entities[0].identity;
    identity.wire_handle = 0;
    identity.bms_id = 0;
    identity.ssn = 0;
    identity.net_id = 0;
    identity.type_id.reset();
    identity.name.clear();

    const parity::ComparisonReport report = parity::compare_traces(trace, trace);
    CHECK(report.classification == parity::ComparisonClassification::clean);
}

void duplicate_bms_ids_are_disambiguated_by_more_specific_identity() {
    parity::Trace trace =
        trace_with_frame(parity::SourceKind::opennova, 100, 6200, 75, 80.0);
    parity::FrameSnapshot& snapshot = selected_frame(trace);
    snapshot.entities[0].identity.pool = 0;
    snapshot.entities[0].identity.slot = 16;
    snapshot.entities[0].identity.bms_id = 10000;
    snapshot.entities[0].identity.net_id = 512;
    snapshot.entities[0].identity.name = "DevUser";
    snapshot.entities[1].identity.pool = 0;
    snapshot.entities[1].identity.slot = 17;
    snapshot.entities[1].identity.bms_id = 10000;
    snapshot.entities[1].identity.net_id = 33287;
    snapshot.entities[1].identity.name = "cdouglass";

    const parity::ComparisonReport report = parity::compare_traces(trace, trace);
    CHECK(report.classification == parity::ComparisonClassification::clean);
}

}  // namespace

int main() {
    comparator_aligns_by_checkpoint_and_classifies_severity();
    checkpoint_markers_select_one_snapshot_independent_of_sampling_rate();
    comparator_aligns_repeated_metadata_by_role_lane_and_checkpoint();
    typed_checkpoint_state_uses_explicit_policies_and_entity_mapping();
    matching_one_identifier_does_not_hide_observed_identity_mismatches();
    unavailable_entity_identifiers_are_reported_as_coverage();
    player_identity_fields_use_the_same_observation_policy();
    unavailable_optional_observations_are_coverage_not_zero_values();
    entity_membership_is_compared_only_for_mutually_complete_pools();
    stable_entity_ids_win_over_reordered_pool_slots();
    pool_slot_identity_is_reflexive_for_complete_pool_entities();
    duplicate_bms_ids_are_disambiguated_by_more_specific_identity();
    std::printf("parity_compare: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
