#include <opennova/retail_hook/validation_session.h>

#include <opennova/retail_abi/address32.h>
#include <opennova/retail_abi/jo_1_7_5_7/entity.h>
#include <opennova/retail_abi/jo_1_7_5_7/weapon.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace opennova::retail_hook {
namespace {

namespace abi = opennova::retail::jo_1_7_5_7;
using opennova::retail::Address32;

constexpr std::uint32_t kHardMaximumPlayerCapacity = 65536;
constexpr std::uint32_t kHardMaximumPoolIndex = 63;

static_assert(std::is_same_v<decltype(abi::PlayerEntity::owner_connection_id),
                             std::uint32_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::dcb_id), std::uint32_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::ssn), std::uint16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::net_id), std::uint16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::health), std::int16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::armor), std::int16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::team), std::int16_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::player_class), std::uint8_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::equipped_adm_index),
                             std::uint8_t>);
static_assert(std::is_same_v<decltype(abi::PlayerEntity::animation_slot),
                             std::uint8_t>);
static_assert(std::is_same_v<decltype(abi::WeaponDef::hip_pose.position.x), float>);
static_assert(std::is_same_v<
              decltype(abi::WeaponDef::aimed_pose.position.x),
              float>);

[[nodiscard]] bool digest_is_zero(const Sha256Digest& digest) noexcept {
    return std::all_of(digest.begin(), digest.end(), [](std::uint8_t byte) {
        return byte == 0;
    });
}

[[nodiscard]] bool checked_add(ProcessAddress base,
                               std::uint64_t offset,
                               ProcessAddress& result) noexcept {
    const auto sum = static_cast<std::uint64_t>(base) + offset;
    if (sum > std::numeric_limits<ProcessAddress>::max()) {
        return false;
    }
    result = static_cast<ProcessAddress>(sum);
    return true;
}

[[nodiscard]] bool checked_byte_count(std::uint32_t count,
                                      std::uint32_t element_size,
                                      std::size_t& result) noexcept {
    const auto bytes = static_cast<std::uint64_t>(count) * element_size;
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    result = static_cast<std::size_t>(bytes);
    return true;
}

[[nodiscard]] bool checked_range(ProcessAddress start,
                                 std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }

    const auto last = static_cast<std::uint64_t>(start) + size - 1U;
    return last <= std::numeric_limits<ProcessAddress>::max();
}

[[nodiscard]] bool range_within_image(std::uint32_t rva,
                                      std::size_t size,
                                      std::uint32_t image_size) noexcept {
    if (image_size == 0) {
        return true;
    }
    const auto end = static_cast<std::uint64_t>(rva) + size;
    return end <= image_size;
}

[[nodiscard]] std::string player_context(std::uint32_t slot) {
    return "player slot " + std::to_string(slot);
}

template <typename T>
[[nodiscard]] CheckResult read_object(const IMemory& memory,
                                      ProcessAddress address,
                                      T& destination,
                                      const std::string& label) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "retail memory overlays must be trivially copyable");

    if (address == 0) {
        return CheckResult::failure(ValidationError::null_address,
                                    label + " resolved to a null address");
    }
    if (!checked_range(address, sizeof(T))) {
        return CheckResult::failure(ValidationError::address_overflow,
                                    label + " crosses the 32-bit address limit");
    }
    if (!memory.readable(address, sizeof(T))) {
        return CheckResult::failure(ValidationError::unreadable_memory,
                                    label + " is not readable");
    }
    if (!memory.read(address, &destination, sizeof(T))) {
        return CheckResult::failure(ValidationError::memory_read_failed,
                                    label + " changed or failed while reading");
    }
    return CheckResult::success();
}

[[nodiscard]] std::string bounded_name(const abi::PlayerEntity& entity) {
    const auto* characters = reinterpret_cast<const char*>(&entity.name);
    std::size_t length = 0;
    while (length < sizeof(entity.name) && characters[length] != '\0') {
        ++length;
    }
    return std::string(characters, length);
}

[[nodiscard]] std::string bounded_name(const abi::WeaponDef& weapon) {
    const auto* characters = reinterpret_cast<const char*>(&weapon.name);
    std::size_t length = 0;
    while (length < sizeof(weapon.name) && characters[length] != '\0') {
        ++length;
    }
    return std::string(characters, length);
}

struct LoadedPlayerPool {
    abi::EntityPool descriptor{};
    ProcessAddress data{};
    std::size_t used_bytes{};
};

}  // namespace

struct ValidationSession::Impl {
    IMemory* memory{};
    ProcessAddress image_base{};
    BuildProfile profile{};
    SessionOptions options{};
    ProcessAddress player_pool_descriptor{};
    ProcessAddress local_player_global{};

    [[nodiscard]] CheckResult load_player_pool(LoadedPlayerPool& loaded) const {
        auto check = read_object(*memory,
                                 player_pool_descriptor,
                                 loaded.descriptor,
                                 "player pool descriptor");
        if (!check) {
            return check;
        }

        if (loaded.descriptor.element_size != sizeof(abi::PlayerEntity)) {
            std::ostringstream detail;
            detail << "player pool element size is "
                   << loaded.descriptor.element_size << ", expected "
                   << sizeof(abi::PlayerEntity);
            return CheckResult::failure(ValidationError::element_size_mismatch,
                                        detail.str());
        }
        if (loaded.descriptor.used > loaded.descriptor.capacity) {
            return CheckResult::failure(
                ValidationError::corrupt_pool_counts,
                "player pool used count exceeds its capacity");
        }
        if (loaded.descriptor.used > profile.maximum_player_capacity ||
            loaded.descriptor.capacity > profile.maximum_player_capacity) {
            return CheckResult::failure(
                ValidationError::implausible_pool_count,
                "player pool count exceeds the profile's plausibility limit");
        }

        if (loaded.descriptor.capacity == 0) {
            loaded.data = 0;
            loaded.used_bytes = 0;
            return CheckResult::success();
        }
        if (loaded.descriptor.data.is_null()) {
            return CheckResult::failure(ValidationError::null_address,
                                        "allocated player pool has null data");
        }

        loaded.data = loaded.descriptor.data.value();
        std::size_t capacity_bytes{};
        if (!checked_byte_count(loaded.descriptor.capacity,
                                loaded.descriptor.element_size,
                                capacity_bytes) ||
            !checked_range(loaded.data, capacity_bytes)) {
            return CheckResult::failure(ValidationError::address_overflow,
                                        "player pool capacity range overflows");
        }
        if (loaded.descriptor.used == 0) {
            loaded.used_bytes = 0;
            return CheckResult::success();
        }

        if (!checked_byte_count(loaded.descriptor.used,
                                loaded.descriptor.element_size,
                                loaded.used_bytes) ||
            !checked_range(loaded.data, loaded.used_bytes)) {
            return CheckResult::failure(ValidationError::address_overflow,
                                        "player pool byte range overflows");
        }
        if (!memory->readable(loaded.data, loaded.used_bytes)) {
            return CheckResult::failure(ValidationError::unreadable_memory,
                                        "player pool data range is not readable");
        }

        return CheckResult::success();
    }

    [[nodiscard]] CheckResult player_address(const LoadedPlayerPool& loaded,
                                             std::uint32_t slot,
                                             ProcessAddress& result) const {
        if (slot >= loaded.descriptor.used) {
            return CheckResult::failure(ValidationError::invalid_player_slot,
                                        player_context(slot) + " is outside the pool");
        }
        const auto offset = static_cast<std::uint64_t>(slot) *
                            loaded.descriptor.element_size;
        if (!checked_add(loaded.data, offset, result) ||
            !checked_range(result, sizeof(abi::PlayerEntity))) {
            return CheckResult::failure(ValidationError::address_overflow,
                                        player_context(slot) + " address overflows");
        }
        return CheckResult::success();
    }
};

const BuildProfile& jo_1_7_5_7_profile() {
    static const BuildProfile profile{
        "Joint Operations 1.7.5.7",
        ExecutableIdentity{
            Sha256Digest{
                0xa4, 0x2e, 0xe2, 0xd8, 0x89, 0x5f, 0xc5, 0x86,
                0x7d, 0x8a, 0xd6, 0x11, 0xe4, 0xc5, 0x17, 0xf9,
                0x72, 0x5a, 0x4a, 0x14, 0x6c, 0xc5, 0xba, 0x43,
                0xd6, 0x48, 0x05, 0xc0, 0xe1, 0x9b, 0x81, 0xc7,
            },
            0,
        },
        0x00400000,
        SymbolRvas{0x006892e0, 0x00775fc8},
        0,
        4096,
    };
    return profile;
}

const char* validation_error_name(ValidationError error) noexcept {
    switch (error) {
        case ValidationError::none:
            return "none";
        case ValidationError::invalid_profile:
            return "invalid_profile";
        case ValidationError::unsupported_executable:
            return "unsupported_executable";
        case ValidationError::address_overflow:
            return "address_overflow";
        case ValidationError::null_address:
            return "null_address";
        case ValidationError::unreadable_memory:
            return "unreadable_memory";
        case ValidationError::memory_read_failed:
            return "memory_read_failed";
        case ValidationError::invalid_probe:
            return "invalid_probe";
        case ValidationError::element_size_mismatch:
            return "element_size_mismatch";
        case ValidationError::corrupt_pool_counts:
            return "corrupt_pool_counts";
        case ValidationError::implausible_pool_count:
            return "implausible_pool_count";
        case ValidationError::writes_disabled:
            return "writes_disabled";
        case ValidationError::invalid_player_slot:
            return "invalid_player_slot";
        case ValidationError::mutation_precondition_failed:
            return "mutation_precondition_failed";
        case ValidationError::memory_write_failed:
            return "memory_write_failed";
        case ValidationError::write_verification_failed:
            return "write_verification_failed";
    }
    return "unknown";
}

bool CheckResult::ok() const noexcept {
    return error == ValidationError::none;
}

CheckResult::operator bool() const noexcept {
    return ok();
}

CheckResult CheckResult::success() {
    return {};
}

CheckResult CheckResult::failure(ValidationError error, std::string detail) {
    return CheckResult{error, std::move(detail)};
}

bool ValidationSnapshot::ok() const noexcept {
    return check.ok();
}

ValidationSnapshot::operator bool() const noexcept {
    return ok();
}

bool OpenResult::ok() const noexcept {
    return check.ok() && session != nullptr;
}

OpenResult::operator bool() const noexcept {
    return ok();
}

ValidationSession::ValidationSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ValidationSession::~ValidationSession() = default;
ValidationSession::ValidationSession(ValidationSession&&) noexcept = default;
ValidationSession& ValidationSession::operator=(ValidationSession&&) noexcept = default;

OpenResult ValidationSession::open(IMemory& memory,
                                   ProcessAddress image_base,
                                   const ExecutableIdentity& executable,
                                   const BuildProfile& profile,
                                   SessionOptions options) {
    const auto fail = [](ValidationError error, std::string detail) {
        return OpenResult{CheckResult::failure(error, std::move(detail)), nullptr};
    };

    if (profile.name.empty() ||
        profile.preferred_image_base == 0 ||
        profile.symbols.entity_pools == 0 ||
        profile.symbols.local_player_entity == 0 ||
        profile.maximum_player_capacity == 0 ||
        profile.maximum_player_capacity > kHardMaximumPlayerCapacity ||
        profile.player_pool_index > kHardMaximumPoolIndex ||
        digest_is_zero(profile.identity.sha256)) {
        return fail(ValidationError::invalid_profile,
                    "build profile is incomplete or outside safety limits");
    }
    if (image_base == 0) {
        return fail(ValidationError::null_address,
                    "executable image base is null");
    }
    if (digest_is_zero(executable.sha256) ||
        executable.sha256 != profile.identity.sha256 ||
        (profile.identity.image_size != 0 &&
         executable.image_size != profile.identity.image_size)) {
        return fail(ValidationError::unsupported_executable,
                    "executable identity does not match the selected profile");
    }

    const auto pool_offset = static_cast<std::uint64_t>(profile.player_pool_index) *
                             sizeof(abi::EntityPool);
    const auto pool_rva = static_cast<std::uint64_t>(profile.symbols.entity_pools) +
                          pool_offset;
    if (pool_rva > std::numeric_limits<std::uint32_t>::max() ||
        !range_within_image(static_cast<std::uint32_t>(pool_rva),
                            sizeof(abi::EntityPool),
                            executable.image_size) ||
        !range_within_image(profile.symbols.local_player_entity,
                            sizeof(Address32<abi::PlayerEntity>),
                            executable.image_size)) {
        return fail(ValidationError::invalid_profile,
                    "profile symbols fall outside the executable image");
    }

    ProcessAddress player_pool_descriptor{};
    ProcessAddress local_player_global{};
    if (!checked_add(image_base, pool_rva, player_pool_descriptor) ||
        !checked_add(image_base,
                     profile.symbols.local_player_entity,
                     local_player_global)) {
        return fail(ValidationError::address_overflow,
                    "relocating profile symbols overflowed 32-bit addresses");
    }
    if (!memory.readable(player_pool_descriptor, sizeof(abi::EntityPool)) ||
        !memory.readable(local_player_global,
                         sizeof(Address32<abi::PlayerEntity>))) {
        return fail(ValidationError::unreadable_memory,
                    "one or more profile globals are not readable");
    }

    auto impl = std::make_unique<Impl>();
    impl->memory = &memory;
    impl->image_base = image_base;
    impl->profile = profile;
    impl->options = options;
    impl->player_pool_descriptor = player_pool_descriptor;
    impl->local_player_global = local_player_global;

    auto session = std::unique_ptr<ValidationSession>(
        new ValidationSession(std::move(impl)));
    return OpenResult{CheckResult::success(), std::move(session)};
}

ValidationSnapshot ValidationSession::sample(ProbeSet probes) const {
    ValidationSnapshot snapshot{};
    snapshot.profile_name = impl_->profile.name;

    constexpr auto known_probes = static_cast<std::uint32_t>(ProbeSet::all);
    const auto requested = static_cast<std::uint32_t>(probes);
    if ((requested & ~known_probes) != 0) {
        snapshot.check = CheckResult::failure(ValidationError::invalid_probe,
                                              "unknown probe bits were requested");
        return snapshot;
    }
    if ((probes & ProbeSet::players) == ProbeSet::none) {
        snapshot.check = CheckResult::success();
        return snapshot;
    }

    LoadedPlayerPool loaded{};
    snapshot.check = impl_->load_player_pool(loaded);
    if (!snapshot.check) {
        return snapshot;
    }
    snapshot.player_pool_used = loaded.descriptor.used;
    snapshot.player_pool_capacity = loaded.descriptor.capacity;

    Address32<abi::PlayerEntity> local_player{};
    snapshot.check = read_object(*impl_->memory,
                                 impl_->local_player_global,
                                 local_player,
                                 "local player global");
    if (!snapshot.check) {
        return snapshot;
    }

    if (loaded.descriptor.used == 0) {
        snapshot.check = CheckResult::success();
        return snapshot;
    }

    std::vector<abi::PlayerEntity> entities(loaded.descriptor.used);
    if (!impl_->memory->read(loaded.data, entities.data(), loaded.used_bytes)) {
        snapshot.check = CheckResult::failure(
            ValidationError::memory_read_failed,
            "player pool changed or failed while reading");
        return snapshot;
    }

    snapshot.players.reserve(entities.size());
    CheckResult local_weapon_check = CheckResult::success();
    for (std::uint32_t slot = 0; slot < loaded.descriptor.used; ++slot) {
        const auto& entity = entities[slot];
        ProcessAddress entity_address{};
        snapshot.check = impl_->player_address(loaded, slot, entity_address);
        if (!snapshot.check) {
            snapshot.players.clear();
            return snapshot;
        }

        PlayerObservation observation{};
        observation.slot = slot;
        observation.address = entity_address;
        observation.ground_entity_address = entity.ground_entity.value();
        observation.owner_connection_id = entity.owner_connection_id;
        observation.dcb_id = entity.dcb_id;
        observation.ssn = entity.ssn;
        observation.net_id = entity.net_id;
        observation.command_group = entity.command_group;
        observation.name = bounded_name(entity);
        observation.position_x_raw = entity.position.x;
        observation.position_y_raw = entity.position.y;
        observation.position_z_raw = entity.position.z;
        observation.camera_offset_x_raw = entity.camera_offset.x;
        observation.camera_offset_y_raw = entity.camera_offset.y;
        observation.camera_offset_z_raw = entity.camera_offset.z;
        observation.yaw_raw = entity.orientation.yaw;
        observation.pitch_raw = entity.orientation.pitch;
        observation.roll_raw = entity.orientation.roll;
        observation.flags = entity.flags;
        observation.health = entity.health;
        observation.armor = entity.armor;
        observation.team = entity.team;
        observation.player_class = entity.player_class;
        observation.equipped_adm_index = entity.equipped_adm_index;
        observation.animation_slot = entity.animation_slot;
        observation.is_local =
            !local_player.is_null() && local_player.value() == entity_address;

        if (!entity.weapon_def.is_null()) {
            abi::WeaponDef weapon{};
            observation.weapon_check = read_object(
                *impl_->memory,
                entity.weapon_def.value(),
                weapon,
                player_context(slot) + " weapon definition");
            if (observation.weapon_check) {
                observation.weapon = WeaponObservation{
                    entity.weapon_def.value(),
                    bounded_name(weapon),
                    weapon.hip_pose.position.x,
                    weapon.hip_pose.position.y,
                    weapon.hip_pose.position.z,
                    weapon.hip_pose.rotation.yaw,
                    weapon.hip_pose.rotation.pitch,
                    weapon.hip_pose.rotation.roll,
                    weapon.aimed_pose.position.x,
                    weapon.aimed_pose.position.y,
                    weapon.aimed_pose.position.z,
                    weapon.aimed_pose.rotation.yaw,
                    weapon.aimed_pose.rotation.pitch,
                    weapon.aimed_pose.rotation.roll,
                    weapon.render_fov,
                };
            } else if (observation.is_local) {
                local_weapon_check = observation.weapon_check;
            }
        }

        snapshot.players.push_back(std::move(observation));
    }

    snapshot.check = std::move(local_weapon_check);
    return snapshot;
}

CheckResult ValidationSession::apply(const Mutation& mutation) {
    if (!impl_->options.writes_enabled) {
        return CheckResult::failure(ValidationError::writes_disabled,
                                    "session was opened with writes disabled");
    }

    LoadedPlayerPool loaded{};
    auto check = impl_->load_player_pool(loaded);
    if (!check) {
        return check;
    }

    return std::visit(
        [this, &loaded](const auto& requested) -> CheckResult {
            using Request = std::decay_t<decltype(requested)>;

            ProcessAddress player_address{};
            auto result = impl_->player_address(loaded,
                                                requested.player_slot,
                                                player_address);
            if (!result) {
                return result;
            }

            abi::PlayerEntity entity{};
            result = read_object(*impl_->memory,
                                 player_address,
                                 entity,
                                 player_context(requested.player_slot));
            if (!result) {
                return result;
            }
            if (entity.owner_connection_id !=
                requested.expected_owner_connection_id) {
                return CheckResult::failure(
                    ValidationError::mutation_precondition_failed,
                    player_context(requested.player_slot) +
                        " no longer has the expected owner connection");
            }

            ProcessAddress field_address{};
            if constexpr (std::is_same_v<Request, SetPlayerHealth>) {
                if (entity.health != requested.expected_value) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " health no longer has the expected value");
                }
                if (!checked_add(player_address,
                                 offsetof(abi::PlayerEntity, health),
                                 field_address)) {
                    return CheckResult::failure(ValidationError::address_overflow,
                                                "health field address overflows");
                }
            } else if constexpr (std::is_same_v<Request, SetPlayerTeam>) {
                if (entity.team != requested.expected_value) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " team no longer has the expected value");
                }
                if (!checked_add(player_address,
                                 offsetof(abi::PlayerEntity, team),
                                 field_address)) {
                    return CheckResult::failure(ValidationError::address_overflow,
                                                "team field address overflows");
                }
            } else {
                static_assert(std::is_same_v<Request, SetEquippedAdmIndex>);
                if (entity.equipped_adm_index != requested.expected_value) {
                    return CheckResult::failure(
                        ValidationError::mutation_precondition_failed,
                        player_context(requested.player_slot) +
                            " equipped ADM index no longer has the expected value");
                }
                if (!checked_add(player_address,
                                 offsetof(abi::PlayerEntity, equipped_adm_index),
                                 field_address)) {
                    return CheckResult::failure(
                        ValidationError::address_overflow,
                        "equipped ADM index field address overflows");
                }
            }

            if (!impl_->memory->write(field_address,
                                      &requested.value,
                                      sizeof(requested.value))) {
                return CheckResult::failure(ValidationError::memory_write_failed,
                                            "memory adapter rejected the mutation");
            }

            decltype(requested.value) observed{};
            result = read_object(*impl_->memory,
                                 field_address,
                                 observed,
                                 "mutated field");
            if (!result) {
                return result;
            }
            if (observed != requested.value) {
                return CheckResult::failure(
                    ValidationError::write_verification_failed,
                    "mutated field did not retain the requested value");
            }
            return CheckResult::success();
        },
        mutation);
}

}  // namespace opennova::retail_hook
