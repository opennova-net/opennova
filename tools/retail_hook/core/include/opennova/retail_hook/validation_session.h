#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <opennova/retail_hook/memory.h>

namespace opennova::retail_hook {

using Sha256Digest = std::array<std::uint8_t, 32>;

struct ExecutableIdentity {
    Sha256Digest sha256{};

    // Optional PE SizeOfImage. A non-zero value is used to range-check RVAs.
    // SHA-256 remains the authoritative build identity.
    std::uint32_t image_size{};
};

struct SymbolRvas {
    // RVA of the first EntityPool descriptor (the descriptor itself, not a
    // pointer to it).
    std::uint32_t entity_pools{};

    // RVA of the global Address32<PlayerEntity> for the local player.
    std::uint32_t local_player_entity{};
};

struct BuildProfile {
    std::string name{};
    ExecutableIdentity identity{};
    ProcessAddress preferred_image_base{};
    SymbolRvas symbols{};
    std::uint32_t player_pool_index{};
    std::uint32_t maximum_player_capacity{4096};
};

// Exact, SHA-locked profile for the witnessed patched Joint Operations build.
[[nodiscard]] const BuildProfile& jo_1_7_5_7_profile();

enum class ValidationError {
    none = 0,
    invalid_profile,
    unsupported_executable,
    address_overflow,
    null_address,
    unreadable_memory,
    memory_read_failed,
    invalid_probe,
    element_size_mismatch,
    corrupt_pool_counts,
    implausible_pool_count,
    writes_disabled,
    invalid_player_slot,
    mutation_precondition_failed,
    memory_write_failed,
    write_verification_failed,
};

[[nodiscard]] const char* validation_error_name(ValidationError error) noexcept;

struct CheckResult {
    ValidationError error{ValidationError::none};
    std::string detail{};

    [[nodiscard]] bool ok() const noexcept;
    explicit operator bool() const noexcept;

    [[nodiscard]] static CheckResult success();
    [[nodiscard]] static CheckResult failure(ValidationError error,
                                             std::string detail);
};

enum class ProbeSet : std::uint32_t {
    none = 0,
    players = 1U << 0U,
    all = players,
};

[[nodiscard]] constexpr ProbeSet operator|(ProbeSet lhs, ProbeSet rhs) noexcept {
    return static_cast<ProbeSet>(static_cast<std::uint32_t>(lhs) |
                                 static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr ProbeSet operator&(ProbeSet lhs, ProbeSet rhs) noexcept {
    return static_cast<ProbeSet>(static_cast<std::uint32_t>(lhs) &
                                 static_cast<std::uint32_t>(rhs));
}

struct WeaponObservation {
    ProcessAddress address{};
    std::string name{};
    float position_x{};
    float position_y{};
    float position_z{};
    std::int32_t rotation_yaw_raw{};
    std::int32_t rotation_pitch_raw{};
    std::int32_t rotation_roll_raw{};
    float alternate_position_x{};
    float alternate_position_y{};
    float alternate_position_z{};
    std::int32_t alternate_rotation_yaw_raw{};
    std::int32_t alternate_rotation_pitch_raw{};
    std::int32_t alternate_rotation_roll_raw{};
    float render_fov{};
};

struct PlayerObservation {
    std::uint32_t slot{};
    ProcessAddress address{};
    ProcessAddress ground_entity_address{};
    std::uint32_t owner_connection_id{};
    std::uint32_t dcb_id{};
    std::uint16_t ssn{};
    std::uint16_t net_id{};
    std::uint16_t command_group{};
    std::string name{};
    std::int32_t position_x_raw{};
    std::int32_t position_y_raw{};
    std::int32_t position_z_raw{};
    std::int32_t camera_offset_x_raw{};
    std::int32_t camera_offset_y_raw{};
    std::int32_t camera_offset_z_raw{};
    std::int32_t yaw_raw{};
    std::int32_t pitch_raw{};
    std::int32_t roll_raw{};
    std::uint32_t flags{};
    std::int16_t health{};
    std::int16_t armor{};
    std::int16_t team{};
    std::uint8_t player_class{};
    std::uint8_t equipped_adm_index{};
    std::uint8_t animation_slot{};
    bool is_local{};
    CheckResult weapon_check{};
    std::optional<WeaponObservation> weapon{};
};

struct ValidationSnapshot {
    CheckResult check{};
    std::string profile_name{};
    std::uint32_t player_pool_used{};
    std::uint32_t player_pool_capacity{};
    std::vector<PlayerObservation> players{};

    [[nodiscard]] bool ok() const noexcept;
    explicit operator bool() const noexcept;
};

struct SetPlayerHealth {
    std::uint32_t player_slot{};
    std::uint32_t expected_owner_connection_id{};
    std::int16_t expected_value{};
    std::int16_t value{};
};

struct SetPlayerTeam {
    std::uint32_t player_slot{};
    std::uint32_t expected_owner_connection_id{};
    std::int16_t expected_value{};
    std::int16_t value{};
};

struct SetEquippedAdmIndex {
    std::uint32_t player_slot{};
    std::uint32_t expected_owner_connection_id{};
    std::uint8_t expected_value{};
    std::uint8_t value{};
};

using Mutation = std::variant<SetPlayerHealth, SetPlayerTeam, SetEquippedAdmIndex>;

struct SessionOptions {
    // Mutations fail with writes_disabled unless explicitly enabled.
    bool writes_enabled{false};
};

struct OpenResult;

// Deep module for retail-memory validation. No unchecked process pointers or
// ABI overlays escape this interface. The IMemory adapter must outlive the
// session. Apply mutations only from a point where the target pool is stable.
class ValidationSession {
public:
    ~ValidationSession();
    ValidationSession(ValidationSession&&) noexcept;
    ValidationSession& operator=(ValidationSession&&) noexcept;

    ValidationSession(const ValidationSession&) = delete;
    ValidationSession& operator=(const ValidationSession&) = delete;

    [[nodiscard]] static OpenResult open(IMemory& memory,
                                         ProcessAddress image_base,
                                         const ExecutableIdentity& executable,
                                         const BuildProfile& profile,
                                         SessionOptions options = {});

    [[nodiscard]] ValidationSnapshot sample(ProbeSet probes = ProbeSet::all) const;
    [[nodiscard]] CheckResult apply(const Mutation& mutation);

private:
    struct Impl;
    explicit ValidationSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

struct OpenResult {
    CheckResult check{};
    std::unique_ptr<ValidationSession> session{};

    [[nodiscard]] bool ok() const noexcept;
    explicit operator bool() const noexcept;
};

}  // namespace opennova::retail_hook
