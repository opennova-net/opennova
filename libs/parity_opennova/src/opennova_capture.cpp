#include <parity/opennova_capture.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#include <netsim/entity_wire_bridge.h>
#include <world/geom.h>

namespace opennova::parity {

NetworkDatagram make_open_nova_datagram(
    ProducerIdentity identity,
    DatagramDirection direction,
    std::uint64_t timestamp_ns,
    std::uint64_t socket_id,
    NetworkEndpoint local,
    NetworkEndpoint remote,
    const std::uint8_t* payload,
    std::size_t payload_size) {
    NetworkDatagram datagram{};
    datagram.identity = std::move(identity);
    datagram.timestamp_ns = timestamp_ns;
    datagram.socket_id = socket_id;
    datagram.total_size = static_cast<std::uint32_t>(std::min<std::size_t>(
        payload_size,
        std::numeric_limits<std::uint32_t>::max()));
    datagram.truncated = payload_size > datagram.total_size ||
        (payload == nullptr && payload_size != 0);
    datagram.direction = direction;
    if (direction == DatagramDirection::outbound) {
        datagram.source = std::move(local);
        datagram.destination = std::move(remote);
    } else {
        datagram.source = std::move(remote);
        datagram.destination = std::move(local);
    }
    if (payload != nullptr) {
        datagram.payload.assign(payload, payload + datagram.total_size);
    }
    return datagram;
}

namespace {

constexpr double kDegreesPerBam32 = 360.0 / 4294967296.0;
constexpr std::int32_t kBamPerDegree = 11930464;

EntityTransform to_transform(const world::Entity& entity) {
    const GameEntitySnapshot wire = netsim::snapshot_of(entity);
    EntityTransform transform{};
    transform.raw_x = wire.x;
    transform.raw_y = wire.y;
    transform.raw_z = wire.z;
    transform.raw_heading = wire.euler_z;
    transform.raw_pitch = wire.pitch_bam;
    transform.raw_roll =
        static_cast<std::int32_t>(entity.roll * kBamPerDegree);
    transform.x = world::from_fixed(wire.x);
    transform.y = world::from_fixed(wire.y);
    transform.z = world::from_fixed(wire.z);
    transform.yaw_deg = wire.euler_z * kDegreesPerBam32;
    transform.pitch_deg = wire.pitch_bam * kDegreesPerBam32;
    transform.roll_deg = transform.raw_roll * kDegreesPerBam32;
    transform.raw_encoding =
        RawTransformEncoding::fixed_point_16_16_bam32;
    return transform;
}

EntityIdentity to_identity(const world::Entity& entity) {
    EntityIdentity identity{};
    identity.pool = entity.handle.pool();
    identity.slot = entity.handle.slot();
    identity.wire_handle = entity.handle.packed;
    identity.bms_id = entity.bms_id;
    identity.ssn = entity.net_id;
    identity.net_id = entity.minimap_net_id;
    identity.owner_connection_id = entity.owner_connection_id;
    if (entity.item_id >= 0) {
        identity.type_id = static_cast<std::uint32_t>(entity.item_id);
    }
    identity.name = entity.name;
    return identity;
}

EntityKind to_kind(const world::Entity& entity) {
    switch (netsim::entity_class_of(entity)) {
    case EntityClass::Player:
        return EntityKind::player;
    case EntityClass::Infantry:
        return EntityKind::organic;
    case EntityClass::Vehicle:
        return EntityKind::vehicle;
    case EntityClass::Guided:
        return EntityKind::projectile;
    case EntityClass::Unknown:
    case EntityClass::NoNetworkCallback:
        break;
    }
    switch (entity.kind) {
    case world::EntityKind::Marker:
        return EntityKind::marker;
    case world::EntityKind::Item:
        return EntityKind::item;
    case world::EntityKind::Building:
        return EntityKind::building;
    case world::EntityKind::Organic:
        return EntityKind::organic;
    }
    return EntityKind::unknown;
}

EntityState to_entity_state(const world::World& live_world,
                            const world::Entity& entity) {
    EntityState state{};
    state.identity = to_identity(entity);
    state.transform = to_transform(entity);
    state.kind = to_kind(entity);
    state.flags = entity.flags;
    state.team = entity.team;
    state.player_class = entity.player_class;
    state.health = entity.health;
    if (entity.health_max > 0) state.max_health = entity.health_max;
    state.alive = entity.alive && entity.health > 0;
    state.hidden = entity.hidden;
    state.held = entity.held;
    state.disabled = entity.disabled;
    state.body_anim_slot = entity.anim_slot;
    state.anim_state = entity.net_anim_pending != 0
        ? entity.net_anim_pending
        : entity.net_anim_state;
    state.anim_phase_ticks = entity.net_anim_phase;
    state.equipped_adm_index = entity.equipped_adm_index;

    if (live_world.ai != nullptr) {
        if (const world::AiEntity* ai =
                live_world.ai->for_handle(entity.handle);
            ai != nullptr && ai->inf.active) {
            state.anim_state = ai->inf.anim_state;
            state.anim_phase_ticks = ai->inf.clip_phase;
        }
    }
    return state;
}

PlayerState to_player_state(const world::World& live_world,
                            const world::Entity& entity) {
    const EntityState observed = to_entity_state(live_world, entity);
    PlayerState player{};
    player.present = true;
    player.identity = observed.identity;
    player.transform = observed.transform;
    player.health = observed.health;
    player.max_health = observed.max_health;
    player.armor = observed.armor;
    player.team = observed.team;
    player.player_class = observed.player_class;
    player.equipped_adm_index = observed.equipped_adm_index;
    return player;
}

double canonical_bam_angle(double degrees) {
    double normalized = std::fmod(degrees, 360.0);
    if (normalized >= 180.0) normalized -= 360.0;
    if (normalized < -180.0) normalized += 360.0;
    const auto raw = static_cast<std::int64_t>(
        std::llround(normalized * static_cast<double>(kBamPerDegree)));
    return raw * kDegreesPerBam32;
}

PoseState to_pose(const std::array<double, 6>& values) {
    return {
        values[0],
        values[1],
        values[2],
        canonical_bam_angle(values[3]),
        canonical_bam_angle(values[4]),
        canonical_bam_angle(values[5]),
    };
}

const world::Entity* authoritative_player(
    const OpenNovaCaptureState& state) {
    if (state.world == nullptr || !state.authoritative_player.valid()) {
        return nullptr;
    }
    return state.world->registry.get(state.authoritative_player);
}

void add_local_observations(FrameSnapshot& frame,
                            const OpenNovaCaptureState& state) {
    const world::Entity* player = authoritative_player(state);
    if (state.input != nullptr) {
        const world::PlayerInput& observed = *state.input;
        InputState input{};
        input.move_order = observed.prone ? 0x100u
            : (observed.crouch ? 0x200u : 0u);
        if (player != nullptr) {
            input.analog_x = player->net_analog_x;
            input.analog_y = player->net_analog_y;
            input.analog_z = player->net_analog_z;
        }
        input.forward = observed.forward;
        input.back = observed.back;
        input.left = observed.left;
        input.right = observed.right;
        input.run = observed.run;
        input.crouch = observed.crouch;
        input.prone = observed.prone;
        input.jump = observed.jump;
        input.look_yaw_deg = observed.look_heading * kDegreesPerBam32;
        input.look_pitch_deg = observed.look_pitch * kDegreesPerBam32;
        frame.input = input;
    }

    if (state.weapon_def != nullptr && state.weapon_slot != nullptr &&
        state.weapon_presentation != nullptr) {
        const OpenNovaWeaponPresentation& presentation =
            *state.weapon_presentation;
        WeaponState weapon{};
        weapon.present = true;
        weapon.name = presentation.name;
        weapon.special_hold = presentation.special_hold;
        weapon.attack_anim = presentation.attack_anim;
        weapon.primary = to_pose(presentation.primary);
        weapon.alternate = to_pose(presentation.alternate);
        weapon.render_fov = presentation.render_fov;
        const int action = state.weapon_slot->current;
        if (action >= 0 && action < world::weapon_action::kCount) {
            weapon.action = world::kWeaponActionSuffixes[action];
        }
        weapon.clip = state.weapon_slot->clip;
        weapon.reserve = state.weapon_slot->reserve;
        frame.weapon = std::move(weapon);
    }

    if (state.player_view != nullptr) {
        const world::PlayerViewState& view = *state.player_view;
        CameraState camera{};
        camera.present = true;
        camera.third_person = view.third_person;
        camera.scope_engaged = view.scope_engaged;
        camera.scope_fraction = world::player_view_scope_fraction(view);
        camera.fov_deg = world::player_view_fov_h_deg(
            view,
            state.weapon_def != nullptr ? state.weapon_def->flags : 0,
            state.scope_max_magnification);
        if (player != nullptr) {
            EntityTransform transform{};
            const bool use_third_person =
                view.third_person && view.tp_anchor_valid;
            const double x = use_third_person
                ? view.tp_anchor[0] : player->position.x;
            const double y = use_third_person
                ? view.tp_anchor[1] : player->position.y;
            const double z = use_third_person
                ? view.tp_anchor[2] : player->position.z + 1.0;
            transform.raw_x = world::to_fixed(x);
            transform.raw_y = world::to_fixed(y);
            transform.raw_z = world::to_fixed(z);
            if (state.input != nullptr) {
                transform.raw_heading = state.input->look_heading;
                transform.raw_pitch = state.input->look_pitch;
            }
            transform.x = x;
            transform.y = y;
            transform.z = z;
            transform.yaw_deg =
                transform.raw_heading * kDegreesPerBam32;
            transform.pitch_deg =
                transform.raw_pitch * kDegreesPerBam32;
            transform.raw_encoding =
                RawTransformEncoding::fixed_point_16_16_bam32;
            camera.transform = transform;
        }
        frame.camera = std::move(camera);
    }
}

CaptureSample capture_authoritative(const RunMetadata& metadata,
                                    const OpenNovaCaptureState& state,
                                    const CaptureRequest& request) {
    if (state.world == nullptr) {
        return {std::nullopt, "OpenNova authoritative World is unavailable"};
    }
    const world::World& live_world = *state.world;
    FrameSnapshot frame{};
    frame.identity = metadata.identity;
    frame.lane = request.lane;
    frame.frame_index = request.frame_index;
    frame.simulation_tick = live_world.logic_tick;
    frame.timestamp_ns = request.timestamp_ns;

    std::array<std::uint32_t, world::EntityRegistry::kPoolCount> used{};
    std::array<std::uint32_t, world::EntityRegistry::kPoolCount> occupied{};
    live_world.registry.for_each([&](const world::Entity& entity) {
        const int pool = entity.handle.pool();
        if (pool < 0 || pool >= world::EntityRegistry::kPoolCount) return;
        const std::size_t index = static_cast<std::size_t>(pool);
        const std::uint32_t high_water =
            static_cast<std::uint32_t>(entity.handle.slot() + 1);
        if (high_water > used[index]) used[index] = high_water;
        ++occupied[index];
        // Retail's current common-header overlay exhaustively observes pool 0
        // players only. Preserve the same evidence coverage until the retail
        // producer gains typed overlays for the remaining entity pools.
        if (pool == 0) frame.entities.push_back(
            to_entity_state(live_world, entity));
    });

    for (int pool = 0; pool < world::EntityRegistry::kPoolCount; ++pool) {
        const std::size_t capacity = live_world.registry.pool_capacity(pool);
        if (capacity == 0) continue;
        PoolState observed{};
        observed.index = static_cast<std::uint32_t>(pool);
        observed.stride = sizeof(world::Entity);
        observed.capacity = static_cast<std::uint32_t>(capacity);
        observed.used_count = used[static_cast<std::size_t>(pool)];
        observed.live_count = occupied[static_cast<std::size_t>(pool)];
        frame.pools.push_back(observed);
    }
    if (live_world.registry.pool_capacity(0) > 0) {
        frame.complete_entity_pools.push_back(0);
    }

    if (state.authoritative_player.valid()) {
        if (const world::Entity* player =
                live_world.registry.get(state.authoritative_player)) {
            frame.player = to_player_state(live_world, *player);
        }
    }
    add_local_observations(frame, state);
    return {std::move(frame), {}};
}

EntityKind to_kind(EntityClass wire_class) {
    switch (wire_class) {
    case EntityClass::Player:
        return EntityKind::player;
    case EntityClass::Infantry:
        return EntityKind::organic;
    case EntityClass::Vehicle:
        return EntityKind::vehicle;
    case EntityClass::Guided:
        return EntityKind::projectile;
    case EntityClass::Unknown:
    case EntityClass::NoNetworkCallback:
        return EntityKind::unknown;
    }
    return EntityKind::unknown;
}

EntityTransform to_transform(const netsim::ClientEntityState& decoded) {
    EntityTransform transform{};
    transform.raw_x = decoded.x;
    transform.raw_y = decoded.y;
    transform.raw_z = decoded.z;
    transform.raw_heading = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(decoded.yaw_byte) << 24);
    transform.x = world::from_fixed(decoded.x);
    transform.y = world::from_fixed(decoded.y);
    transform.z = world::from_fixed(decoded.z);
    transform.yaw_deg = transform.raw_heading * kDegreesPerBam32;
    transform.raw_encoding =
        RawTransformEncoding::fixed_point_16_16_bam32;
    return transform;
}

const world::Entity* matching_world_entity(
    const OpenNovaCaptureState& state,
    std::uint16_t handle) {
    if (!state.resolve_presented_from_world || state.world == nullptr) {
        return nullptr;
    }
    return state.world->registry.get(world::EntityHandle{handle});
}

EntityState to_presented_entity(const OpenNovaCaptureState& state,
                                const netsim::ClientEntityState& decoded) {
    const world::Entity* authoritative =
        matching_world_entity(state, decoded.handle);
    EntityState entity = authoritative != nullptr
        ? to_entity_state(*state.world, *authoritative)
        : EntityState{};
    if (authoritative == nullptr) {
        entity.identity.pool = (decoded.handle >> 12) & 0xF;
        entity.identity.slot = decoded.handle & 0xFFF;
        entity.identity.wire_handle = decoded.handle;
        entity.alive = true;
    }
    entity.identity.type_id = decoded.type_id;
    entity.transform = to_transform(decoded);
    entity.kind = to_kind(decoded.cls);
    if (decoded.handle == state.presented_player_wire_handle &&
        state.presented != nullptr) {
        entity.health = state.presented->local_health;
        entity.alive = entity.health > 0;
    }
    return entity;
}

CaptureSample capture_presented(const RunMetadata& metadata,
                                const OpenNovaCaptureState& state,
                                const CaptureRequest& request) {
    if (state.presented == nullptr) {
        return {std::nullopt, "OpenNova presented ClientState is unavailable"};
    }
    const netsim::ClientState& client = *state.presented;
    FrameSnapshot frame{};
    frame.identity = metadata.identity;
    frame.lane = request.lane;
    frame.frame_index = request.frame_index;
    frame.simulation_tick = client.frames_applied;
    frame.timestamp_ns = request.timestamp_ns;

    std::array<std::uint32_t, world::EntityRegistry::kPoolCount> used{};
    std::array<std::uint32_t, world::EntityRegistry::kPoolCount> occupied{};
    for (const netsim::ClientEntityState& decoded : client.entities) {
        const int pool = (decoded.handle >> 12) & 0xF;
        if (pool < 0 || pool >= world::EntityRegistry::kPoolCount) continue;
        const std::size_t index = static_cast<std::size_t>(pool);
        const std::uint32_t high_water =
            static_cast<std::uint32_t>((decoded.handle & 0xFFF) + 1);
        if (high_water > used[index]) used[index] = high_water;
        ++occupied[index];
        if (pool == 0) {
            frame.entities.push_back(to_presented_entity(state, decoded));
        }
    }

    for (int pool = 0; pool < world::EntityRegistry::kPoolCount; ++pool) {
        const std::size_t index = static_cast<std::size_t>(pool);
        const std::size_t known_capacity = state.world != nullptr
            ? state.world->registry.pool_capacity(pool)
            : 0;
        if (known_capacity == 0 && occupied[index] == 0) continue;
        PoolState observed{};
        observed.index = static_cast<std::uint32_t>(pool);
        observed.capacity = static_cast<std::uint32_t>(known_capacity);
        observed.used_count = used[index];
        observed.live_count = occupied[index];
        frame.pools.push_back(observed);
    }
    if (state.world == nullptr ||
        state.world->registry.pool_capacity(0) > 0 || occupied[0] > 0) {
        frame.complete_entity_pools.push_back(0);
    }

    for (const EntityState& entity : frame.entities) {
        if (entity.identity.wire_handle !=
            state.presented_player_wire_handle) {
            continue;
        }
        PlayerState player{};
        player.present = true;
        player.identity = entity.identity;
        player.transform = entity.transform;
        player.health = entity.health;
        player.max_health = entity.max_health;
        player.armor = entity.armor;
        player.team = entity.team;
        player.player_class = entity.player_class;
        player.equipped_adm_index = entity.equipped_adm_index;
        frame.player = std::move(player);
        break;
    }
    add_local_observations(frame, state);
    return {std::move(frame), {}};
}

}  // namespace

OpenNovaCaptureSource::OpenNovaCaptureSource(
    RunMetadata metadata,
    OpenNovaCaptureState state)
    : metadata_(std::move(metadata)), state_(state) {}

RunMetadata OpenNovaCaptureSource::metadata() const {
    return metadata_;
}

CaptureSample OpenNovaCaptureSource::capture(
    const CaptureRequest& request) {
    if (request.lane == StateLane::authoritative) {
        return capture_authoritative(metadata_, state_, request);
    }
    if (request.lane == StateLane::presented) {
        return capture_presented(metadata_, state_, request);
    }
    return {std::nullopt, "OpenNova capture lane is not available"};
}

OpenNovaCaptureRecorder::OpenNovaCaptureRecorder(
    RunMetadata metadata,
    IEventSink& sink)
    : metadata_(std::move(metadata)), sink_(&sink) {}

bool OpenNovaCaptureRecorder::start() {
    if (started_) return !finished_;
    if (metadata_.identity.source == SourceKind::unknown ||
        metadata_.identity.role == RunRole::unknown ||
        metadata_.identity.stream_id.empty() || metadata_.run_id.empty() ||
        metadata_.scenario.empty() || metadata_.title.empty() ||
        metadata_.expansion.empty() || metadata_.mission.empty()) {
        last_error_ = "OpenNova capture metadata is incomplete";
        return false;
    }
    if (!sink_->append(Event{metadata_})) {
        last_error_ = sink_->last_error();
        return false;
    }
    started_ = true;
    return true;
}

bool OpenNovaCaptureRecorder::record_tick(
    ICaptureSource& source,
    const std::vector<StateLane>& lanes,
    std::uint64_t frame_index,
    std::uint64_t timestamp_ns) {
    if (!started_ || finished_) {
        last_error_ = "OpenNova capture recorder is not active";
        return false;
    }
    std::array<bool, 5> seen{};
    for (const StateLane lane : lanes) {
        const std::size_t index = static_cast<std::size_t>(lane);
        if (lane == StateLane::unknown || index >= seen.size() || seen[index]) {
            last_error_ = "OpenNova capture tick contains an invalid lane";
            return false;
        }
        seen[index] = true;

        CaptureSample sample = source.capture(
            CaptureRequest{lane, frame_index, timestamp_ns});
        if (!sample.frame) {
            last_error_ = sample.detail.empty()
                ? "OpenNova capture source returned no frame"
                : std::move(sample.detail);
            return false;
        }
        sample.frame->identity = metadata_.identity;
        sample.frame->lane = lane;
        sample.frame->frame_index = frame_index;
        sample.frame->timestamp_ns = timestamp_ns;

        if (!lane_started_[index]) {
            Checkpoint checkpoint{};
            checkpoint.name = "capture-start";
            checkpoint.occurrence = 0;
            checkpoint.frame_index = frame_index;
            checkpoint.identity = metadata_.identity;
            checkpoint.lane = lane;
            if (!sink_->append(Event{std::move(checkpoint)})) {
                last_error_ = sink_->last_error();
                return false;
            }
        }
        if (!sink_->append(Event{std::move(*sample.frame)})) {
            last_error_ = sink_->last_error();
            return false;
        }
        lane_started_[index] = true;
        lane_last_frame_[index] = frame_index;
    }
    return true;
}

bool OpenNovaCaptureRecorder::finish() {
    if (finished_) return last_error_.empty();
    if (!started_) {
        last_error_ = "OpenNova capture recorder was never started";
        return false;
    }
    for (std::size_t index = 1; index < lane_started_.size(); ++index) {
        if (!lane_started_[index]) continue;
        Checkpoint checkpoint{};
        checkpoint.name = "capture-end";
        checkpoint.occurrence = 0;
        checkpoint.frame_index = lane_last_frame_[index];
        checkpoint.identity = metadata_.identity;
        checkpoint.lane = static_cast<StateLane>(index);
        if (!sink_->append(Event{std::move(checkpoint)})) {
            last_error_ = sink_->last_error();
            return false;
        }
    }
    finished_ = true;
    return true;
}

const std::string& OpenNovaCaptureRecorder::last_error() const noexcept {
    return last_error_;
}

OpenNovaQueuedEventSink::OpenNovaQueuedEventSink(
    std::unique_ptr<IEventSink> downstream,
    std::size_t capacity)
    : downstream_(std::move(downstream)), capacity_(capacity) {
    if (!downstream_) {
        last_error_ = "OpenNova retry sink has no downstream";
    } else if (capacity_ == 0) {
        last_error_ = "OpenNova retry sink capacity is zero";
    }
}

bool OpenNovaQueuedEventSink::append(const Event& event) {
    if (!downstream_ || capacity_ == 0) return false;
    (void)drain();
    if (pending_.empty() && downstream_->append(event)) return true;
    if (pending_.size() >= capacity_) {
        last_error_ = "OpenNova retry sink is full after downstream backpressure: " +
            downstream_->last_error();
        return false;
    }
    pending_.push_back(event);
    return true;
}

const std::string& OpenNovaQueuedEventSink::last_error() const noexcept {
    return last_error_;
}

bool OpenNovaQueuedEventSink::drain() {
    if (!downstream_) return false;
    while (!pending_.empty()) {
        if (!downstream_->append(pending_.front())) return false;
        pending_.pop_front();
    }
    return true;
}

std::size_t OpenNovaQueuedEventSink::pending_count() const noexcept {
    return pending_.size();
}

}  // namespace opennova::parity
