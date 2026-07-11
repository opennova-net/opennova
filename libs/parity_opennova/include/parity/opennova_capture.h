#pragma once

#include <parity/parity.h>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include <netsim/client_state.h>
#include <world/entity.h>
#include <world/player_input.h>
#include <world/player_view.h>
#include <world/weapon_fsm.h>
#include <world/world.h>

namespace opennova::parity {

struct OpenNovaWeaponPresentation {
    std::string name{};
    std::int32_t special_hold{};
    std::int32_t attack_anim{};
    std::array<double, 6> primary{};
    std::array<double, 6> alternate{};
    double render_fov{};
};

struct OpenNovaCaptureState {
    const world::World* world{};
    const netsim::ClientState* presented{};
    world::EntityHandle authoritative_player{};
    std::uint16_t presented_player_wire_handle{};
    bool resolve_presented_from_world{};
    const world::PlayerInput* input{};
    const world::WeaponFsmDef* weapon_def{};
    const world::WeaponSlotState* weapon_slot{};
    const OpenNovaWeaponPresentation* weapon_presentation{};
    const world::PlayerViewState* player_view{};
    float scope_max_magnification{};
};

[[nodiscard]] NetworkDatagram make_open_nova_datagram(
    ProducerIdentity identity,
    DatagramDirection direction,
    std::uint64_t timestamp_ns,
    std::uint64_t socket_id,
    NetworkEndpoint local,
    NetworkEndpoint remote,
    const std::uint8_t* payload,
    std::size_t payload_size);

class OpenNovaCaptureSource final : public ICaptureSource {
public:
    OpenNovaCaptureSource(RunMetadata metadata, OpenNovaCaptureState state);

    [[nodiscard]] RunMetadata metadata() const override;
    [[nodiscard]] CaptureSample capture(
        const CaptureRequest& request) override;

private:
    RunMetadata metadata_{};
    OpenNovaCaptureState state_{};
};

class OpenNovaCaptureRecorder final {
public:
    OpenNovaCaptureRecorder(RunMetadata metadata, IEventSink& sink);

    [[nodiscard]] bool start();
    [[nodiscard]] bool record_tick(
        ICaptureSource& source,
        const std::vector<StateLane>& lanes,
        std::uint64_t frame_index,
        std::uint64_t timestamp_ns);
    [[nodiscard]] bool finish();
    [[nodiscard]] const std::string& last_error() const noexcept;

private:
    RunMetadata metadata_{};
    IEventSink* sink_{};
    std::array<bool, 5> lane_started_{};
    std::array<std::uint64_t, 5> lane_last_frame_{};
    bool started_{};
    bool finished_{};
    std::string last_error_{};
};

class OpenNovaQueuedEventSink final : public IEventSink {
public:
    explicit OpenNovaQueuedEventSink(
        std::unique_ptr<IEventSink> downstream,
        std::size_t capacity = 8192);

    [[nodiscard]] bool append(const Event& event) override;
    [[nodiscard]] const std::string& last_error() const noexcept override;
    [[nodiscard]] bool drain();
    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    std::unique_ptr<IEventSink> downstream_{};
    std::size_t capacity_{};
    std::deque<Event> pending_{};
    std::string last_error_{};
};

}  // namespace opennova::parity
