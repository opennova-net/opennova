#include <parity/parity.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace opennova::parity {
namespace {

struct CheckpointSelection {
    std::string key{};
    std::string display{};
    RunRole role{RunRole::unknown};
    StateLane lane{StateLane::unknown};
    const FrameSnapshot* frame{};
};

bool same_identity(const ProducerIdentity& left,
                   const ProducerIdentity& right) {
    return left.source == right.source && left.role == right.role &&
           left.stream_id == right.stream_id;
}

bool roles_compatible(RunRole reference, RunRole candidate) {
    return reference != RunRole::unknown && candidate != RunRole::unknown &&
           reference == candidate;
}

std::string checkpoint_key(const Checkpoint& checkpoint) {
    return std::to_string(static_cast<unsigned>(checkpoint.identity.role)) +
           ":" + std::to_string(static_cast<unsigned>(checkpoint.lane)) +
           ":" + checkpoint.name + "#" +
           std::to_string(checkpoint.occurrence);
}

std::string checkpoint_display(const Checkpoint& checkpoint) {
    return checkpoint.name + "#" + std::to_string(checkpoint.occurrence);
}

bool collect_selections(const Trace& trace,
                        std::vector<CheckpointSelection>& selections) {
    std::vector<const Checkpoint*> checkpoints;
    std::vector<const FrameSnapshot*> frames;
    for (const Event& event : trace.events) {
        if (const auto* checkpoint = std::get_if<Checkpoint>(&event)) {
            if (checkpoint->identity.source == SourceKind::unknown ||
                checkpoint->identity.role == RunRole::unknown ||
                checkpoint->identity.stream_id.empty() ||
                checkpoint->lane == StateLane::unknown) {
                return false;
            }
            const std::string key = checkpoint_key(*checkpoint);
            if (std::any_of(checkpoints.begin(), checkpoints.end(),
                            [&key](const Checkpoint* observed) {
                                return checkpoint_key(*observed) == key;
                            })) {
                return false;
            }
            checkpoints.push_back(checkpoint);
        } else if (const auto* frame = std::get_if<FrameSnapshot>(&event)) {
            frames.push_back(frame);
        }
    }
    if (checkpoints.empty()) return false;

    for (const Checkpoint* checkpoint : checkpoints) {
        const FrameSnapshot* selected = nullptr;
        for (const FrameSnapshot* frame : frames) {
            if (!same_identity(frame->identity, checkpoint->identity) ||
                frame->lane != checkpoint->lane) {
                continue;
            }
            if (frame->frame_index < checkpoint->frame_index) continue;
            if (selected == nullptr ||
                frame->frame_index < selected->frame_index) {
                selected = frame;
            } else if (frame->frame_index == selected->frame_index) {
                return false;
            }
        }
        if (selected == nullptr) return false;
        selections.push_back(CheckpointSelection{checkpoint_key(*checkpoint),
                                                  checkpoint_display(*checkpoint),
                                                  checkpoint->identity.role,
                                                  checkpoint->lane,
                                                  selected});
    }
    return true;
}

template <typename T>
std::string as_text(const T& value) {
    std::ostringstream output;
    if constexpr (std::is_enum_v<T>) {
        output << static_cast<unsigned long long>(
            static_cast<std::underlying_type_t<T>>(value));
    } else {
        output << value;
    }
    return output.str();
}

std::string as_text(bool value) {
    return value ? "true" : "false";
}

void add_difference(ComparisonReport& report,
                    Severity severity,
                    const std::string& checkpoint,
                    std::string path,
                    std::string expected,
                    std::string actual,
                    double absolute_tolerance = 0.0) {
    report.differences.push_back(Difference{
        severity,
        checkpoint,
        std::move(path),
        std::move(expected),
        std::move(actual),
        absolute_tolerance,
    });
    if (severity >= Severity::error) {
        report.classification = ComparisonClassification::mismatch;
    } else if (severity == Severity::warning &&
               report.classification == ComparisonClassification::clean) {
        report.classification = ComparisonClassification::warnings;
    }
}

template <typename T>
void compare_exact(ComparisonReport& report,
                   const std::string& checkpoint,
                   const std::string& path,
                   const T& expected,
                   const T& actual,
                   Severity severity = Severity::error) {
    if (expected != actual) {
        add_difference(report,
                       severity,
                       checkpoint,
                       path,
                       as_text(expected),
                       as_text(actual));
    }
}

template <typename T>
void compare_optional_exact(ComparisonReport& report,
                            const std::string& checkpoint,
                            const std::string& path,
                            const std::optional<T>& expected,
                            const std::optional<T>& actual,
                            Severity value_severity = Severity::error) {
    if (expected.has_value() != actual.has_value()) {
        add_difference(report,
                       Severity::warning,
                       checkpoint,
                       path + ".coverage",
                       expected ? "observed" : "unavailable",
                       actual ? "observed" : "unavailable");
        return;
    }
    if (expected) {
        compare_exact(report,
                      checkpoint,
                      path,
                      *expected,
                      *actual,
                      value_severity);
    }
}

void compare_real(ComparisonReport& report,
                  const std::string& checkpoint,
                  const std::string& path,
                  double expected,
                  double actual,
                  double tolerance,
                  Severity severity);

void compare_transform(ComparisonReport& report,
                       const std::string& checkpoint,
                       const std::string& path,
                       const EntityTransform& expected,
                       const EntityTransform& actual,
                       const ComparisonOptions& options,
                       Severity severity = Severity::error) {
    if (expected.raw_encoding == actual.raw_encoding &&
        expected.raw_encoding != RawTransformEncoding::unavailable) {
        compare_exact(report, checkpoint, path + ".raw_x", expected.raw_x, actual.raw_x, severity);
        compare_exact(report, checkpoint, path + ".raw_y", expected.raw_y, actual.raw_y, severity);
        compare_exact(report, checkpoint, path + ".raw_z", expected.raw_z, actual.raw_z, severity);
        compare_exact(report, checkpoint, path + ".raw_heading", expected.raw_heading, actual.raw_heading, severity);
        compare_exact(report, checkpoint, path + ".raw_pitch", expected.raw_pitch, actual.raw_pitch, severity);
        compare_exact(report, checkpoint, path + ".raw_roll", expected.raw_roll, actual.raw_roll, severity);
    } else if (expected.raw_encoding != actual.raw_encoding) {
        add_difference(report,
                       Severity::warning,
                       checkpoint,
                       path + ".raw.coverage",
                       as_text(expected.raw_encoding),
                       as_text(actual.raw_encoding));
    }
    compare_real(report, checkpoint, path + ".x", expected.x, actual.x,
                 options.normalized_position_alignment_allowance, severity);
    compare_real(report, checkpoint, path + ".y", expected.y, actual.y,
                 options.normalized_position_alignment_allowance, severity);
    compare_real(report, checkpoint, path + ".z", expected.z, actual.z,
                 options.normalized_position_alignment_allowance, severity);
    compare_real(report, checkpoint, path + ".yaw_deg", expected.yaw_deg, actual.yaw_deg,
                 options.normalized_angle_alignment_allowance_deg, severity);
    compare_real(report, checkpoint, path + ".pitch_deg", expected.pitch_deg, actual.pitch_deg,
                 options.normalized_angle_alignment_allowance_deg, severity);
    compare_real(report, checkpoint, path + ".roll_deg", expected.roll_deg, actual.roll_deg,
                 options.normalized_angle_alignment_allowance_deg, severity);
}

void compare_observed_transform(
    ComparisonReport& report,
    const std::string& checkpoint,
    const std::string& path,
    const std::optional<EntityTransform>& expected,
    const std::optional<EntityTransform>& actual,
    const ComparisonOptions& options,
    Severity severity = Severity::error) {
    if (expected.has_value() != actual.has_value()) {
        add_difference(report,
                       Severity::warning,
                       checkpoint,
                       path + ".coverage",
                       expected ? "observed" : "unavailable",
                       actual ? "observed" : "unavailable");
        return;
    }
    if (expected) {
        compare_transform(
            report, checkpoint, path, *expected, *actual, options, severity);
    }
}

void compare_pose(ComparisonReport& report,
                  const std::string& checkpoint,
                  const std::string& path,
                  const PoseState& expected,
                  const PoseState& actual,
                  const ComparisonOptions& options) {
    compare_real(report, checkpoint, path + ".x", expected.x, actual.x,
                 options.normalized_position_alignment_allowance, Severity::error);
    compare_real(report, checkpoint, path + ".y", expected.y, actual.y,
                 options.normalized_position_alignment_allowance, Severity::error);
    compare_real(report, checkpoint, path + ".z", expected.z, actual.z,
                 options.normalized_position_alignment_allowance, Severity::error);
    compare_real(report, checkpoint, path + ".yaw_deg", expected.yaw_deg, actual.yaw_deg,
                 options.normalized_angle_alignment_allowance_deg, Severity::error);
    compare_real(report, checkpoint, path + ".pitch_deg", expected.pitch_deg, actual.pitch_deg,
                 options.normalized_angle_alignment_allowance_deg, Severity::error);
    compare_real(report, checkpoint, path + ".roll_deg", expected.roll_deg, actual.roll_deg,
                 options.normalized_angle_alignment_allowance_deg, Severity::error);
}

void compare_real(ComparisonReport& report,
                  const std::string& checkpoint,
                  const std::string& path,
                  double expected,
                  double actual,
                  double tolerance,
                  Severity severity) {
    if (!std::isfinite(expected) || !std::isfinite(actual) ||
        std::abs(expected - actual) > tolerance) {
        add_difference(report,
                       severity,
                       checkpoint,
                       path,
                       as_text(expected),
                       as_text(actual),
                       tolerance);
    }
}

void compare_identity(ComparisonReport& report,
                      const std::string& checkpoint,
                      const std::string& path,
                      const EntityIdentity& expected,
                      const EntityIdentity& actual);

bool compare_player(ComparisonReport& report,
                    const std::string& checkpoint,
                    const std::optional<PlayerState>& expected,
                    const std::optional<PlayerState>& actual,
                    const ComparisonOptions& options) {
    if (expected.has_value() != actual.has_value()) {
        return false;
    }
    if (!expected) return true;
    compare_exact(report, checkpoint, "player.present", expected->present, actual->present);
    compare_identity(report,
                     checkpoint,
                     "player.identity",
                     expected->identity,
                     actual->identity);
    compare_observed_transform(report, checkpoint, "player.transform", expected->transform, actual->transform, options);
    compare_exact(report, checkpoint, "player.health", expected->health, actual->health);
    compare_optional_exact(report, checkpoint, "player.max_health", expected->max_health, actual->max_health);
    compare_optional_exact(
        report, checkpoint, "player.armor", expected->armor, actual->armor);
    compare_exact(report, checkpoint, "player.team", expected->team, actual->team);
    compare_exact(report, checkpoint, "player.player_class", expected->player_class, actual->player_class);
    compare_exact(report, checkpoint, "player.equipped_adm_index", expected->equipped_adm_index, actual->equipped_adm_index);
    return true;
}

bool compare_weapon(ComparisonReport& report,
                    const std::string& checkpoint,
                    const std::optional<WeaponState>& expected,
                    const std::optional<WeaponState>& actual,
                    const ComparisonOptions& options) {
    if (expected.has_value() != actual.has_value()) {
        return false;
    }
    if (!expected) return true;
    compare_exact(report, checkpoint, "weapon.present", expected->present, actual->present);
    compare_exact(report, checkpoint, "weapon.name", expected->name, actual->name);
    compare_exact(report, checkpoint, "weapon.special_hold", expected->special_hold, actual->special_hold);
    compare_exact(report, checkpoint, "weapon.attack_anim", expected->attack_anim, actual->attack_anim);
    compare_pose(report, checkpoint, "weapon.primary", expected->primary, actual->primary, options);
    compare_pose(report, checkpoint, "weapon.alternate", expected->alternate, actual->alternate, options);
    compare_real(report, checkpoint, "weapon.render_fov", expected->render_fov, actual->render_fov,
                 options.presentation_fov_allowance_deg, Severity::warning);
    compare_optional_exact(report, checkpoint, "weapon.action", expected->action, actual->action);
    compare_optional_exact(report, checkpoint, "weapon.clip", expected->clip, actual->clip);
    compare_optional_exact(report, checkpoint, "weapon.reserve", expected->reserve, actual->reserve);
    return true;
}

void compare_input(ComparisonReport& report,
                   const std::string& checkpoint,
                   const std::optional<InputState>& expected,
                   const std::optional<InputState>& actual,
                   const ComparisonOptions& options) {
    if (expected.has_value() != actual.has_value()) {
        add_difference(report, Severity::warning, checkpoint, "input.coverage",
                       expected ? "observed" : "unavailable",
                       actual ? "observed" : "unavailable");
        return;
    }
    if (!expected) return;
    compare_exact(report, checkpoint, "input.move_order", expected->move_order, actual->move_order);
    compare_exact(report, checkpoint, "input.analog_x", expected->analog_x, actual->analog_x);
    compare_exact(report, checkpoint, "input.analog_y", expected->analog_y, actual->analog_y);
    compare_exact(report, checkpoint, "input.analog_z", expected->analog_z, actual->analog_z);
    compare_exact(report, checkpoint, "input.forward", expected->forward, actual->forward);
    compare_exact(report, checkpoint, "input.back", expected->back, actual->back);
    compare_exact(report, checkpoint, "input.left", expected->left, actual->left);
    compare_exact(report, checkpoint, "input.right", expected->right, actual->right);
    compare_exact(report, checkpoint, "input.run", expected->run, actual->run);
    compare_exact(report, checkpoint, "input.crouch", expected->crouch, actual->crouch);
    compare_exact(report, checkpoint, "input.prone", expected->prone, actual->prone);
    compare_exact(report, checkpoint, "input.jump", expected->jump, actual->jump);
    compare_real(report, checkpoint, "input.look_yaw_deg", expected->look_yaw_deg, actual->look_yaw_deg,
                 options.normalized_angle_alignment_allowance_deg, Severity::error);
    compare_real(report, checkpoint, "input.look_pitch_deg", expected->look_pitch_deg, actual->look_pitch_deg,
                 options.normalized_angle_alignment_allowance_deg, Severity::error);
}

struct IdentityAffinity {
    std::uint8_t persistent_matches{};
    bool type_and_name_match{};
    bool locator_match{};
};

IdentityAffinity identity_affinity(const EntityIdentity& expected,
    const EntityIdentity& actual) {
    IdentityAffinity affinity{};
    if (expected.bms_id > 0 && actual.bms_id > 0 &&
        expected.bms_id == actual.bms_id) {
        ++affinity.persistent_matches;
    }
    if (expected.ssn != 0 && actual.ssn != 0 &&
        expected.ssn == actual.ssn) {
        ++affinity.persistent_matches;
    }
    if (expected.net_id != 0 && actual.net_id != 0 &&
        expected.net_id == actual.net_id) {
        ++affinity.persistent_matches;
    }
    affinity.type_and_name_match =
        expected.type_id.has_value() && actual.type_id.has_value() &&
        *expected.type_id != 0 && expected.type_id == actual.type_id &&
        !expected.name.empty() && expected.name == actual.name;
    const bool wire_match =
        expected.wire_handle != 0 && actual.wire_handle != 0 &&
        expected.wire_handle == actual.wire_handle;
    const bool pool_and_slot_match =
        expected.pool >= 0 && actual.pool >= 0 &&
        expected.slot >= 0 && actual.slot >= 0 &&
        expected.pool == actual.pool && expected.slot == actual.slot;
    affinity.locator_match = wire_match || pool_and_slot_match;
    return affinity;
}

bool has_identity_affinity(const IdentityAffinity& affinity) {
    return affinity.persistent_matches != 0 ||
           affinity.type_and_name_match ||
           affinity.locator_match;
}

bool stronger_identity_affinity(const IdentityAffinity& left,
                                const IdentityAffinity& right) {
    if (left.persistent_matches != right.persistent_matches) {
        return left.persistent_matches > right.persistent_matches;
    }
    if (left.type_and_name_match != right.type_and_name_match) {
        return left.type_and_name_match;
    }
    return left.locator_match && !right.locator_match;
}

bool equal_identity_affinity(const IdentityAffinity& left,
                             const IdentityAffinity& right) {
    return left.persistent_matches == right.persistent_matches &&
           left.type_and_name_match == right.type_and_name_match &&
           left.locator_match == right.locator_match;
}

std::string entity_label(const EntityIdentity& identity) {
    if (identity.bms_id > 0) return "bms:" + as_text(identity.bms_id);
    if (identity.wire_handle != 0) return "wire:" + as_text(identity.wire_handle);
    if (identity.ssn != 0) return "ssn:" + as_text(identity.ssn);
    if (identity.net_id != 0) return "net:" + as_text(identity.net_id);
    return "type:" +
           (identity.type_id ? as_text(*identity.type_id) : "unknown") +
           ":" + identity.name;
}

template <typename T, typename IsObserved>
void compare_observed_identity_value(ComparisonReport& report,
                                     const std::string& checkpoint,
                                     const std::string& path,
                                     const T& expected,
                                     const T& actual,
                                     IsObserved is_observed) {
    const bool expected_observed = is_observed(expected);
    const bool actual_observed = is_observed(actual);
    if (expected_observed != actual_observed) {
        add_difference(report,
                       Severity::warning,
                       checkpoint,
                       path + ".coverage",
                       expected_observed ? "observed" : "unavailable",
                       actual_observed ? "observed" : "unavailable");
        return;
    }
    if (expected_observed) {
        compare_exact(report, checkpoint, path, expected, actual);
    }
}

void compare_identity(ComparisonReport& report,
                      const std::string& checkpoint,
                      const std::string& path,
                      const EntityIdentity& expected,
                      const EntityIdentity& actual) {
    const auto nonnegative = [](std::int32_t value) { return value >= 0; };
    const auto positive_i32 = [](std::int32_t value) { return value > 0; };
    const auto nonzero = [](auto value) { return value != 0; };
    compare_observed_identity_value(
        report, checkpoint, path + ".pool", expected.pool, actual.pool,
        nonnegative);
    compare_observed_identity_value(
        report, checkpoint, path + ".slot", expected.slot, actual.slot,
        nonnegative);
    compare_observed_identity_value(report,
                                    checkpoint,
                                    path + ".wire_handle",
                                    expected.wire_handle,
                                    actual.wire_handle,
                                    nonzero);
    compare_observed_identity_value(report,
                                    checkpoint,
                                    path + ".bms_id",
                                    expected.bms_id,
                                    actual.bms_id,
                                    positive_i32);
    compare_observed_identity_value(
        report, checkpoint, path + ".ssn", expected.ssn, actual.ssn,
        nonzero);
    compare_observed_identity_value(
        report, checkpoint, path + ".net_id", expected.net_id,
        actual.net_id, nonzero);
    compare_exact(report,
                  checkpoint,
                  path + ".owner_connection_id",
                  expected.owner_connection_id,
                  actual.owner_connection_id);
    compare_exact(report,
                  checkpoint,
                  path + ".name",
                  expected.name,
                  actual.name,
                  Severity::warning);
    compare_optional_exact(report,
                           checkpoint,
                           path + ".type_id",
                           expected.type_id,
                           actual.type_id);
}

void compare_entity(ComparisonReport& report,
                    const std::string& checkpoint,
                    const std::string& path,
                    const EntityState& expected,
                    const EntityState& actual,
                    const ComparisonOptions& options) {
    compare_identity(report,
                     checkpoint,
                     path + ".identity",
                     expected.identity,
                     actual.identity);
    compare_observed_transform(report, checkpoint, path + ".transform", expected.transform, actual.transform, options);
    compare_exact(report, checkpoint, path + ".kind", expected.kind, actual.kind);
    compare_exact(report, checkpoint, path + ".flags", expected.flags, actual.flags);
    compare_exact(report, checkpoint, path + ".team", expected.team, actual.team);
    compare_exact(report, checkpoint, path + ".player_class", expected.player_class, actual.player_class);
    compare_exact(report, checkpoint, path + ".health", expected.health, actual.health);
    compare_optional_exact(report, checkpoint, path + ".max_health", expected.max_health, actual.max_health);
    compare_optional_exact(
        report, checkpoint, path + ".armor", expected.armor, actual.armor);
    compare_exact(report, checkpoint, path + ".alive", expected.alive, actual.alive);
    compare_optional_exact(report, checkpoint, path + ".hidden", expected.hidden, actual.hidden);
    compare_optional_exact(report, checkpoint, path + ".held", expected.held, actual.held);
    compare_optional_exact(report, checkpoint, path + ".disabled", expected.disabled, actual.disabled);
    compare_exact(report, checkpoint, path + ".body_anim_slot", expected.body_anim_slot, actual.body_anim_slot);
    compare_optional_exact(report, checkpoint, path + ".anim_state", expected.anim_state, actual.anim_state);
    compare_optional_exact(report, checkpoint, path + ".anim_phase_ticks", expected.anim_phase_ticks, actual.anim_phase_ticks);
    compare_exact(report, checkpoint, path + ".equipped_adm_index", expected.equipped_adm_index, actual.equipped_adm_index);
}

bool compare_entities(ComparisonReport& report,
                      const std::string& checkpoint,
                      const std::vector<EntityState>& expected,
                      const std::vector<EntityState>& actual,
                      const std::vector<std::uint32_t>& expected_complete_pools,
                      const std::vector<std::uint32_t>& actual_complete_pools,
                      const ComparisonOptions& options) {
    const auto duplicate_pool = [](const std::vector<std::uint32_t>& pools) {
        return std::any_of(
            pools.begin(), pools.end(), [&pools](std::uint32_t pool) {
                return std::count(pools.begin(), pools.end(), pool) != 1;
            });
    };
    if (duplicate_pool(expected_complete_pools) ||
        duplicate_pool(actual_complete_pools)) {
        return false;
    }
    const auto is_complete = [](const std::vector<std::uint32_t>& pools,
                                std::int32_t pool) {
        return pool >= 0 &&
               std::find(pools.begin(),
                         pools.end(),
                         static_cast<std::uint32_t>(pool)) != pools.end();
    };
    std::vector<std::uint32_t> coverage_union = expected_complete_pools;
    for (std::uint32_t pool : actual_complete_pools) {
        if (std::find(coverage_union.begin(), coverage_union.end(), pool) ==
            coverage_union.end()) {
            coverage_union.push_back(pool);
        }
    }
    for (std::uint32_t pool : coverage_union) {
        const bool expected_complete = is_complete(
            expected_complete_pools, static_cast<std::int32_t>(pool));
        const bool actual_complete = is_complete(
            actual_complete_pools, static_cast<std::int32_t>(pool));
        if (expected_complete != actual_complete) {
            add_difference(report,
                           Severity::warning,
                           checkpoint,
                           "entities.pool[" + as_text(pool) + "].coverage",
                           expected_complete ? "complete" : "unavailable",
                           actual_complete ? "complete" : "unavailable");
        }
    }
    std::vector<bool> matched(actual.size(), false);
    for (const EntityState& expected_entity : expected) {
        std::size_t found = actual.size();
        IdentityAffinity best_affinity{};
        bool ambiguous = false;
        for (std::size_t index = 0; index < actual.size(); ++index) {
            if (matched[index]) continue;
            const IdentityAffinity affinity =
                identity_affinity(expected_entity.identity,
                                  actual[index].identity);
            if (!has_identity_affinity(affinity)) continue;
            if (found == actual.size() ||
                stronger_identity_affinity(affinity, best_affinity)) {
                found = index;
                best_affinity = affinity;
                ambiguous = false;
            } else if (equal_identity_affinity(affinity, best_affinity)) {
                ambiguous = true;
            }
        }
        if (ambiguous) return false;
        const std::string path = "entities[" + entity_label(expected_entity.identity) + "]";
        if (found == actual.size()) {
            if (is_complete(actual_complete_pools,
                            expected_entity.identity.pool)) {
                add_difference(report, Severity::error, checkpoint, path + ".present", "true", "false");
            }
            continue;
        }
        matched[found] = true;
        compare_entity(report,
                       checkpoint,
                       path,
                       expected_entity,
                       actual[found],
                       options);
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (!matched[index] &&
            is_complete(expected_complete_pools,
                        actual[index].identity.pool)) {
            const std::string path = "entities[" + entity_label(actual[index].identity) + "]";
            add_difference(report, Severity::error, checkpoint, path + ".present", "false", "true");
        }
    }
    return true;
}

bool compare_pools(ComparisonReport& report,
                   const std::string& checkpoint,
                   const std::vector<PoolState>& expected,
                   const std::vector<PoolState>& actual,
                   SourceKind expected_source,
                   SourceKind actual_source) {
    for (const PoolState& expected_pool : expected) {
        const auto found = std::find_if(actual.begin(), actual.end(),
            [&expected_pool](const PoolState& pool) { return pool.index == expected_pool.index; });
        const std::string path = "pools[" + as_text(expected_pool.index) + "]";
        if (found == actual.end()) {
            add_difference(report, Severity::error, checkpoint, path + ".present", "true", "false");
            continue;
        }
        if (std::count_if(actual.begin(), actual.end(),
                          [&expected_pool](const PoolState& pool) { return pool.index == expected_pool.index; }) != 1) {
            return false;
        }
        if (expected_source == actual_source) {
            compare_exact(report, checkpoint, path + ".stride", expected_pool.stride, found->stride);
            compare_exact(report, checkpoint, path + ".capacity", expected_pool.capacity, found->capacity);
        } else {
            compare_exact(report,
                          checkpoint,
                          path + ".stride.source_layout",
                          expected_pool.stride,
                          found->stride,
                          Severity::info);
            compare_exact(report,
                          checkpoint,
                          path + ".capacity.configuration",
                          expected_pool.capacity,
                          found->capacity,
                          Severity::info);
        }
        compare_exact(report, checkpoint, path + ".used_count", expected_pool.used_count, found->used_count);
        compare_optional_exact(report, checkpoint, path + ".live_count", expected_pool.live_count, found->live_count);
    }
    for (const PoolState& actual_pool : actual) {
        if (std::none_of(expected.begin(), expected.end(),
                         [&actual_pool](const PoolState& pool) { return pool.index == actual_pool.index; })) {
            const std::string path = "pools[" + as_text(actual_pool.index) + "]";
            add_difference(report, Severity::error, checkpoint, path + ".present", "false", "true");
        }
    }
    return true;
}

void compare_camera(ComparisonReport& report,
                    const std::string& checkpoint,
                    const std::optional<CameraState>& expected,
                    const std::optional<CameraState>& actual,
                    const ComparisonOptions& options) {
    if (expected.has_value() != actual.has_value()) {
        add_difference(report,
                       Severity::warning,
                       checkpoint,
                       "camera.coverage",
                       expected ? "observed" : "unavailable",
                       actual ? "observed" : "unavailable");
        return;
    }
    if (!expected) return;
    compare_real(report,
                 checkpoint,
                 "camera.fov_deg",
                 expected->fov_deg,
                 actual->fov_deg,
                 options.presentation_fov_allowance_deg,
                 Severity::warning);
    compare_observed_transform(report,
                               checkpoint,
                               "camera.transform",
                               expected->transform,
                               actual->transform,
                               options,
                               Severity::warning);
    compare_exact(report, checkpoint, "camera.third_person", expected->third_person, actual->third_person, Severity::warning);
    compare_exact(report, checkpoint, "camera.scope_engaged", expected->scope_engaged, actual->scope_engaged, Severity::warning);
    compare_real(report,
                 checkpoint,
                 "camera.scope_fraction",
                 expected->scope_fraction,
                 actual->scope_fraction,
                 options.presentation_scope_fraction_allowance,
                 Severity::warning);
}

bool compare_frame(ComparisonReport& report,
                   const std::string& checkpoint,
                   const FrameSnapshot& expected,
                   const FrameSnapshot& actual,
                   const ComparisonOptions& options) {
    compare_exact(report, checkpoint, "lane", expected.lane, actual.lane);
    if (!compare_pools(report,
                       checkpoint,
                       expected.pools,
                       actual.pools,
                       expected.identity.source,
                       actual.identity.source) ||
        !compare_entities(report,
                          checkpoint,
                          expected.entities,
                          actual.entities,
                          expected.complete_entity_pools,
                          actual.complete_entity_pools,
                          options) ||
        !compare_player(report,
                        checkpoint,
                        expected.player,
                        actual.player,
                        options) ||
        !compare_weapon(report,
                        checkpoint,
                        expected.weapon,
                        actual.weapon,
                        options)) {
        return false;
    }
    compare_camera(report, checkpoint, expected.camera, actual.camera, options);
    compare_input(report, checkpoint, expected.input, actual.input, options);
    return true;
}

ComparisonReport invalid_report(std::string detail) {
    ComparisonReport report{};
    report.classification = ComparisonClassification::invalid_evidence;
    report.differences.push_back(Difference{
        Severity::fatal, {}, "trace", {}, std::move(detail), 0.0});
    return report;
}

}  // namespace

int ComparisonReport::exit_code() const noexcept {
    switch (classification) {
    case ComparisonClassification::clean:
    case ComparisonClassification::warnings:
        return 0;
    case ComparisonClassification::mismatch:
        return 2;
    case ComparisonClassification::invalid_evidence:
        return 3;
    }
    return 3;
}

ComparisonReport compare_traces(const Trace& reference,
                                const Trace& candidate,
                                const ComparisonOptions& options) {
    if (reference.major_version != candidate.major_version ||
        reference.major_version != kTraceMajorVersion) {
        return invalid_report("trace major versions are incompatible");
    }

    std::vector<const RunMetadata*> reference_metadata;
    std::vector<const RunMetadata*> candidate_metadata;
    for (const Event& event : reference.events) {
        if (const auto* metadata = std::get_if<RunMetadata>(&event)) {
            reference_metadata.push_back(metadata);
        }
    }
    for (const Event& event : candidate.events) {
        if (const auto* metadata = std::get_if<RunMetadata>(&event)) {
            candidate_metadata.push_back(metadata);
        }
    }
    if (reference_metadata.empty() ||
        reference_metadata.size() != candidate_metadata.size()) {
        return invalid_report("trace producer metadata sets differ");
    }
    const auto metadata_complete = [](const RunMetadata& metadata) {
        return metadata.identity.source != SourceKind::unknown &&
               metadata.identity.role != RunRole::unknown &&
               !metadata.identity.stream_id.empty() && !metadata.title.empty() &&
               !metadata.expansion.empty() && !metadata.mission.empty() &&
               !metadata.scenario.empty();
    };
    for (std::size_t index = 0; index < reference_metadata.size(); ++index) {
        const RunMetadata& expected = *reference_metadata[index];
        if (!metadata_complete(expected)) {
            return invalid_report("reference trace metadata is incomplete");
        }
        if (std::count_if(reference_metadata.begin(),
                          reference_metadata.end(),
                          [&expected](const RunMetadata* metadata) {
                              return metadata->identity.role ==
                                     expected.identity.role;
                          }) != 1) {
            return invalid_report("reference trace producer roles are ambiguous");
        }
        const auto found = std::find_if(
            candidate_metadata.begin(),
            candidate_metadata.end(),
            [&expected](const RunMetadata* metadata) {
                return roles_compatible(expected.identity.role,
                                        metadata->identity.role);
            });
        if (found == candidate_metadata.end() || !metadata_complete(**found)) {
            return invalid_report("trace producer roles are incompatible");
        }
        if (std::count_if(candidate_metadata.begin(),
                          candidate_metadata.end(),
                          [&expected](const RunMetadata* metadata) {
                              return metadata->identity.role ==
                                     expected.identity.role;
                          }) != 1) {
            return invalid_report("candidate trace producer roles are ambiguous");
        }
        const RunMetadata& actual = **found;
        if (expected.title != actual.title ||
            expected.expansion != actual.expansion ||
            expected.mission != actual.mission ||
            expected.scenario != actual.scenario) {
            return invalid_report("trace metadata scenario identity differs");
        }
    }

    std::vector<CheckpointSelection> reference_selections;
    std::vector<CheckpointSelection> candidate_selections;
    if (!collect_selections(reference, reference_selections) ||
        !collect_selections(candidate, candidate_selections)) {
        return invalid_report("trace checkpoint topology is invalid");
    }
    if (reference_selections.size() != candidate_selections.size()) {
        return invalid_report("trace checkpoint sets differ");
    }

    ComparisonReport report{};
    for (const CheckpointSelection& reference_selection :
         reference_selections) {
        const auto found = std::find_if(
            candidate_selections.begin(),
            candidate_selections.end(),
            [&reference_selection](const CheckpointSelection& selection) {
                return selection.key == reference_selection.key;
            });
        if (found == candidate_selections.end()) {
            return invalid_report("trace checkpoint sets differ");
        }
        if (!compare_frame(report,
                           reference_selection.display,
                           *reference_selection.frame,
                           *found->frame,
                           options)) {
            return invalid_report("entity or pool identity mapping is ambiguous");
        }
    }
    return report;
}

ComparisonReport compare_traces(const TraceReadResult& reference,
                                const TraceReadResult& candidate,
                                const ComparisonOptions& options) {
    if (!reference.complete() || !candidate.complete()) {
        return invalid_report("one or both traces are invalid or incomplete");
    }
    return compare_traces(reference.trace, candidate.trace, options);
}

}  // namespace opennova::parity
