#include <opennova/retail_hook/parity_adapter.h>

#include <opennova/retail_abi/jo_1_7_5_7/entity.h>

#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace opennova::retail_hook {
namespace {

namespace abi = opennova::retail::jo_1_7_5_7;

constexpr double kFixedScale = 65536.0;
constexpr double kBamDegrees =
    360.0 / 4294967296.0;

[[nodiscard]] parity::EntityTransform to_transform(
    const PlayerObservation& player) {
    parity::EntityTransform result{};
    result.raw_x = player.position_x_raw;
    result.raw_y = player.position_y_raw;
    result.raw_z = player.position_z_raw;
    result.raw_heading = player.yaw_raw;
    result.raw_pitch = player.pitch_raw;
    result.raw_roll = player.roll_raw;
    result.x = player.position_x_raw / kFixedScale;
    result.y = player.position_y_raw / kFixedScale;
    result.z = player.position_z_raw / kFixedScale;
    result.yaw_deg = player.yaw_raw * kBamDegrees;
    result.pitch_deg = player.pitch_raw * kBamDegrees;
    result.roll_deg = player.roll_raw * kBamDegrees;
    result.raw_encoding =
        parity::RawTransformEncoding::fixed_point_16_16_bam32;
    return result;
}

[[nodiscard]] parity::EntityIdentity to_identity(
    const PlayerObservation& player) {
    parity::EntityIdentity identity{};
    identity.pool = 0;
    identity.slot = static_cast<std::int32_t>(player.slot);
    identity.bms_id = player.dcb_id >
            static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())
        ? -1
        : static_cast<std::int32_t>(player.dcb_id);
    identity.ssn = player.ssn;
    identity.net_id = player.net_id;
    identity.owner_connection_id = player.owner_connection_id;
    identity.name = player.name;
    return identity;
}

[[nodiscard]] parity::WeaponState to_weapon(
    const WeaponObservation& weapon) {
    parity::WeaponState state{};
    state.present = true;
    state.name = weapon.name;
    state.special_hold = weapon.special_hold;
    state.attack_anim = weapon.attack_anim;
    state.primary = parity::PoseState{
        weapon.position_x / 256.0,
        weapon.position_y / 256.0,
        weapon.position_z / 256.0,
        weapon.rotation_yaw_raw * kBamDegrees,
        weapon.rotation_pitch_raw * kBamDegrees,
        weapon.rotation_roll_raw * kBamDegrees,
    };
    state.alternate = parity::PoseState{
        weapon.alternate_position_x / 256.0,
        weapon.alternate_position_y / 256.0,
        weapon.alternate_position_z / 256.0,
        weapon.alternate_rotation_yaw_raw * kBamDegrees,
        weapon.alternate_rotation_pitch_raw * kBamDegrees,
        weapon.alternate_rotation_roll_raw * kBamDegrees,
    };
    state.render_fov = weapon.render_fov;
    return state;
}

[[nodiscard]] std::string pose_text(const WeaponPoseValue& pose) {
    std::ostringstream output;
    output << std::setprecision(9)
           << pose.position_x << ',' << pose.position_y << ','
           << pose.position_z << ';'
           << pose.rotation_yaw_raw << ',' << pose.rotation_pitch_raw << ','
           << pose.rotation_roll_raw;
    return output.str();
}

[[nodiscard]] std::string target_text(
    MutationKind kind,
    ProcessAddress address) {
    const char* name = "unknown";
    switch (kind) {
    case MutationKind::player_health:
        name = "player.health";
        break;
    case MutationKind::player_team:
        name = "player.team";
        break;
    case MutationKind::equipped_adm_index:
        name = "player.equipped_adm_index";
        break;
    case MutationKind::active_weapon_pose:
        name = "active_weapon.pose";
        break;
    case MutationKind::active_weapon_render_fov:
        name = "active_weapon.render_fov";
        break;
    }
    std::ostringstream output;
    output << name << "@0x" << std::hex << std::uppercase << address;
    return output.str();
}

}  // namespace

std::optional<parity::Checkpoint>
CaptureCheckpointTracker::accepted_frame(
    const parity::FrameSnapshot& frame) {
    const bool ready =
        frame.player.has_value() && frame.player->present &&
        frame.weapon.has_value() && frame.weapon->present;
    if (!ready) {
        return std::nullopt;
    }
    if (started_ &&
        (frame.identity.source != identity_.source ||
         frame.identity.role != identity_.role ||
         frame.identity.stream_id != identity_.stream_id ||
         frame.lane != lane_)) {
        return std::nullopt;
    }
    last_ready_frame_ = frame.frame_index;
    if (started_) {
        return std::nullopt;
    }
    started_ = true;
    identity_ = frame.identity;
    lane_ = frame.lane;
    parity::Checkpoint checkpoint{};
    checkpoint.name = "capture-start";
    checkpoint.occurrence = 0;
    checkpoint.frame_index = frame.frame_index;
    checkpoint.identity = identity_;
    checkpoint.lane = lane_;
    return checkpoint;
}

std::optional<parity::Checkpoint>
CaptureCheckpointTracker::finish() const {
    if (!started_) {
        return std::nullopt;
    }
    parity::Checkpoint checkpoint{};
    checkpoint.name = "capture-end";
    checkpoint.occurrence = 0;
    checkpoint.frame_index = last_ready_frame_;
    checkpoint.identity = identity_;
    checkpoint.lane = lane_;
    return checkpoint;
}

parity::FrameSnapshot to_parity_snapshot(
    const ValidationSnapshot& snapshot,
    const parity::ProducerIdentity& identity,
    parity::StateLane lane,
    std::uint64_t frame_index,
    std::uint64_t simulation_tick,
    std::uint64_t timestamp_ns) {
    parity::FrameSnapshot frame{};
    frame.identity = identity;
    frame.lane = lane;
    frame.frame_index = frame_index;
    frame.simulation_tick = simulation_tick;
    frame.timestamp_ns = timestamp_ns;

    frame.pools.reserve(snapshot.pool_descriptors.size());
    for (const PoolDescriptorObservation& descriptor :
         snapshot.pool_descriptors) {
        parity::PoolState pool{};
        pool.index = descriptor.index;
        pool.base_address = descriptor.data_address;
        pool.stride = descriptor.element_size;
        pool.capacity = descriptor.capacity;
        pool.used_count = descriptor.used;
        frame.pools.push_back(pool);
    }
    frame.complete_entity_pools.push_back(0);
    frame.entities.reserve(snapshot.players.size());

    for (const PlayerObservation& player : snapshot.players) {
        parity::EntityState entity{};
        entity.identity = to_identity(player);
        entity.transform = to_transform(player);
        entity.kind =
            player.is_local || player.owner_connection_id != 0
            ? parity::EntityKind::player
            : parity::EntityKind::organic;
        entity.flags = player.flags;
        entity.team = player.team;
        entity.player_class = player.player_class;
        entity.health = player.health;
        entity.armor = player.armor;
        entity.alive = player.health > 0;
        entity.body_anim_slot = player.animation_slot;
        entity.equipped_adm_index = player.equipped_adm_index;
        frame.entities.push_back(std::move(entity));

        if (player.is_local) {
            parity::PlayerState local{};
            local.present = true;
            local.identity = to_identity(player);
            local.transform = to_transform(player);
            local.health = player.health;
            local.armor = player.armor;
            local.team = player.team;
            local.player_class = player.player_class;
            local.equipped_adm_index = player.equipped_adm_index;
            frame.player = std::move(local);
            if (player.weapon.has_value()) {
                frame.weapon = to_weapon(*player.weapon);
            }
        }
    }
    return frame;
}

parity::MutationAudit to_parity_mutation_audit(
    const MutationResult& result,
    const parity::ProducerIdentity& identity,
    std::uint64_t simulation_tick,
    std::uint64_t timestamp_ns) {
    parity::MutationAudit audit{};
    audit.identity = identity;
    audit.simulation_tick = simulation_tick;
    audit.timestamp_ns = timestamp_ns;
    audit.target = target_text(
        result.audit.kind, result.audit.target_address);
    switch (result.audit.stage) {
    case MutationStage::verified:
        audit.result = parity::MutationResult::applied;
        break;
    case MutationStage::write_attempted:
    case MutationStage::written:
        audit.result = parity::MutationResult::unverified;
        break;
    case MutationStage::rejected:
    case MutationStage::validated:
        audit.result = parity::MutationResult::rejected;
        break;
    }
    audit.detail = result.check.detail;

    std::visit(
        [&audit](const auto& request) {
            using Request = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<Request, SetPlayerHealth>) {
                audit.operation = "set_player_health";
                audit.before =
                    static_cast<std::int64_t>(request.expected_value);
                audit.after = static_cast<std::int64_t>(request.value);
            } else if constexpr (std::is_same_v<Request, SetPlayerTeam>) {
                audit.operation = "set_player_team";
                audit.before =
                    static_cast<std::int64_t>(request.expected_value);
                audit.after = static_cast<std::int64_t>(request.value);
            } else if constexpr (
                std::is_same_v<Request, SetEquippedAdmIndex>) {
                audit.operation = "set_equipped_adm_index";
                audit.before =
                    static_cast<std::uint64_t>(request.expected_value);
                audit.after = static_cast<std::uint64_t>(request.value);
            } else if constexpr (
                std::is_same_v<Request, SetActiveWeaponPose>) {
                audit.operation = request.pose == ActiveWeaponPose::hip
                    ? "set_active_weapon_hip_pose"
                    : "set_active_weapon_aimed_pose";
                audit.before = pose_text(request.expected_value);
                audit.after = pose_text(request.value);
            } else {
                static_assert(
                    std::is_same_v<Request, SetActiveWeaponFov>);
                audit.operation = "set_active_weapon_render_fov";
                audit.before = static_cast<double>(request.expected_value);
                audit.after = static_cast<double>(request.value);
            }
        },
        result.audit.request);
    return audit;
}

}  // namespace opennova::retail_hook
