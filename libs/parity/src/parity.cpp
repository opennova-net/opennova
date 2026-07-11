#include <parity/parity.h>

#include <io/byte_reader.h>
#include <io/byte_writer.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace opennova::parity {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'O', 'N', 'P', 'T'};
constexpr std::uint32_t kRunMetadataChunk = 1;
constexpr std::uint32_t kFrameSnapshotChunk = 2;
constexpr std::uint32_t kCheckpointChunk = 3;
constexpr std::uint32_t kNetworkDatagramChunk = 4;
constexpr std::uint32_t kDecodedNetworkEventChunk = 5;
constexpr std::uint32_t kMutationAuditChunk = 6;
constexpr std::uint32_t kDiagnosticEventChunk = 7;
constexpr std::size_t kFileHeaderSize = 8;
constexpr std::size_t kChunkHeaderSize = 12;
constexpr std::uint32_t kMaximumFieldCount = 1'000'000;

enum class ScalarKind : std::uint8_t {
    signed_integer = 1,
    unsigned_integer = 2,
    real = 3,
    boolean = 4,
    text = 5,
    bytes = 6,
};

void write_u64(io::ByteWriter& writer, std::uint64_t value) {
    writer.write_u32(static_cast<std::uint32_t>(value));
    writer.write_u32(static_cast<std::uint32_t>(value >> 32U));
}

std::uint64_t read_u64(io::ByteReader& reader) {
    const std::uint64_t low = reader.read_u32();
    const std::uint64_t high = reader.read_u32();
    return low | (high << 32U);
}

void write_f64(io::ByteWriter& writer, double value) {
    std::uint64_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    write_u64(writer, bits);
}

double read_f64(io::ByteReader& reader) {
    const std::uint64_t bits = read_u64(reader);
    double value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool write_string(io::ByteWriter& writer,
                  const std::string& value,
                  std::string& error) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "trace string exceeds the 32-bit format limit";
        return false;
    }
    writer.write_u32(static_cast<std::uint32_t>(value.size()));
    writer.write_bytes(reinterpret_cast<const std::uint8_t*>(value.data()),
                       value.size());
    return true;
}

bool read_string(io::ByteReader& reader, std::string& value) {
    if (!reader.has_bytes(sizeof(std::uint32_t))) {
        return false;
    }
    const std::uint32_t size = reader.read_u32();
    if (!reader.has_bytes(size)) {
        return false;
    }
    std::vector<std::uint8_t> bytes;
    reader.read_bytes(bytes, size);
    value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool write_producer_identity(io::ByteWriter& writer,
                             const ProducerIdentity& identity,
                             std::string& error) {
    writer.write_u8(static_cast<std::uint8_t>(identity.source));
    writer.write_u8(static_cast<std::uint8_t>(identity.role));
    return write_string(writer, identity.stream_id, error);
}

bool read_producer_identity(io::ByteReader& reader,
                            ProducerIdentity& identity) {
    if (!reader.has_bytes(2)) return false;
    identity.source = static_cast<SourceKind>(reader.read_u8());
    identity.role = static_cast<RunRole>(reader.read_u8());
    return identity.source <= SourceKind::opennova &&
           identity.role <= RunRole::dedicated_server &&
           read_string(reader, identity.stream_id);
}

bool write_byte_string(io::ByteWriter& writer,
                       const ByteString& value,
                       std::string& error) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "trace byte string exceeds the 32-bit format limit";
        return false;
    }
    writer.write_u32(static_cast<std::uint32_t>(value.size()));
    writer.write_bytes(value);
    return true;
}

bool read_byte_string(io::ByteReader& reader, ByteString& value) {
    if (!reader.has_bytes(sizeof(std::uint32_t))) {
        return false;
    }
    const std::uint32_t size = reader.read_u32();
    if (!reader.has_bytes(size)) {
        return false;
    }
    reader.read_bytes(value, size);
    return true;
}

bool write_field(io::ByteWriter& writer,
                 const Field& field,
                 std::string& error) {
    if (!write_string(writer, field.name, error)) {
        return false;
    }
    const bool wrote = std::visit(
        [&writer, &error](const auto& value) -> bool {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::int64_t>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::signed_integer));
                write_u64(writer, static_cast<std::uint64_t>(value));
            } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::unsigned_integer));
                write_u64(writer, value);
            } else if constexpr (std::is_same_v<Value, double>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::real));
                write_f64(writer, value);
            } else if constexpr (std::is_same_v<Value, bool>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::boolean));
                writer.write_u8(value ? 1U : 0U);
            } else if constexpr (std::is_same_v<Value, std::string>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::text));
                return write_string(writer, value, error);
            } else {
                static_assert(std::is_same_v<Value, ByteString>);
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::bytes));
                return write_byte_string(writer, value, error);
            }
            return true;
        },
        field.value);
    if (!wrote) {
        return false;
    }
    write_f64(writer, field.absolute_tolerance);
    writer.write_u8(static_cast<std::uint8_t>(field.mismatch_severity));
    return true;
}

bool write_fields(io::ByteWriter& writer,
                  const std::vector<Field>& fields,
                  std::string& error) {
    if (fields.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "trace field count exceeds the 32-bit format limit";
        return false;
    }
    writer.write_u32(static_cast<std::uint32_t>(fields.size()));
    for (const Field& field : fields) {
        if (!write_field(writer, field, error)) {
            return false;
        }
    }
    return true;
}

bool read_field(io::ByteReader& reader, Field& field) {
    if (!read_string(reader, field.name) || !reader.has_bytes(1)) {
        return false;
    }
    const auto kind = static_cast<ScalarKind>(reader.read_u8());
    switch (kind) {
    case ScalarKind::signed_integer:
        if (!reader.has_bytes(8)) return false;
        field.value = static_cast<std::int64_t>(read_u64(reader));
        break;
    case ScalarKind::unsigned_integer:
        if (!reader.has_bytes(8)) return false;
        field.value = read_u64(reader);
        break;
    case ScalarKind::real:
        if (!reader.has_bytes(8)) return false;
        field.value = read_f64(reader);
        break;
    case ScalarKind::boolean:
        if (!reader.has_bytes(1)) return false;
        {
            const std::uint8_t value = reader.read_u8();
            if (value > 1) return false;
            field.value = value != 0;
        }
        break;
    case ScalarKind::text: {
        std::string value;
        if (!read_string(reader, value)) return false;
        field.value = std::move(value);
        break;
    }
    case ScalarKind::bytes: {
        ByteString value;
        if (!read_byte_string(reader, value)) return false;
        field.value = std::move(value);
        break;
    }
    default:
        return false;
    }
    if (!reader.has_bytes(9)) {
        return false;
    }
    field.absolute_tolerance = read_f64(reader);
    const auto severity = static_cast<Severity>(reader.read_u8());
    if (severity > Severity::fatal) {
        return false;
    }
    field.mismatch_severity = severity;
    return true;
}

bool read_fields(io::ByteReader& reader, std::vector<Field>& fields) {
    if (!reader.has_bytes(sizeof(std::uint32_t))) {
        return false;
    }
    const std::uint32_t count = reader.read_u32();
    if (count > kMaximumFieldCount) {
        return false;
    }
    fields.clear();
    fields.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        Field field{};
        if (!read_field(reader, field)) {
            return false;
        }
        fields.push_back(std::move(field));
    }
    return true;
}

bool write_scalar(io::ByteWriter& writer,
                  const ScalarValue& scalar,
                  std::string& error) {
    return std::visit(
        [&writer, &error](const auto& value) -> bool {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::int64_t>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::signed_integer));
                write_u64(writer, static_cast<std::uint64_t>(value));
            } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::unsigned_integer));
                write_u64(writer, value);
            } else if constexpr (std::is_same_v<Value, double>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::real));
                write_f64(writer, value);
            } else if constexpr (std::is_same_v<Value, bool>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::boolean));
                writer.write_u8(value ? 1U : 0U);
            } else if constexpr (std::is_same_v<Value, std::string>) {
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::text));
                return write_string(writer, value, error);
            } else {
                static_assert(std::is_same_v<Value, ByteString>);
                writer.write_u8(static_cast<std::uint8_t>(ScalarKind::bytes));
                return write_byte_string(writer, value, error);
            }
            return true;
        },
        scalar);
}

bool read_scalar(io::ByteReader& reader, ScalarValue& scalar) {
    if (!reader.has_bytes(1)) {
        return false;
    }
    const auto kind = static_cast<ScalarKind>(reader.read_u8());
    switch (kind) {
    case ScalarKind::signed_integer:
        if (!reader.has_bytes(8)) return false;
        scalar = static_cast<std::int64_t>(read_u64(reader));
        return true;
    case ScalarKind::unsigned_integer:
        if (!reader.has_bytes(8)) return false;
        scalar = read_u64(reader);
        return true;
    case ScalarKind::real:
        if (!reader.has_bytes(8)) return false;
        scalar = read_f64(reader);
        return true;
    case ScalarKind::boolean: {
        if (!reader.has_bytes(1)) return false;
        const std::uint8_t value = reader.read_u8();
        if (value > 1) return false;
        scalar = value != 0;
        return true;
    }
    case ScalarKind::text: {
        std::string value;
        if (!read_string(reader, value)) return false;
        scalar = std::move(value);
        return true;
    }
    case ScalarKind::bytes: {
        ByteString value;
        if (!read_byte_string(reader, value)) return false;
        scalar = std::move(value);
        return true;
    }
    default:
        return false;
    }
}

void write_bool(io::ByteWriter& writer, bool value) {
    writer.write_u8(value ? 1U : 0U);
}

bool read_bool(io::ByteReader& reader, bool& value) {
    if (!reader.has_bytes(1)) {
        return false;
    }
    const std::uint8_t raw = reader.read_u8();
    if (raw > 1) {
        return false;
    }
    value = raw != 0;
    return true;
}

bool write_count(io::ByteWriter& writer,
                 std::size_t count,
                 std::string& error) {
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        error = "trace collection exceeds the 32-bit format limit";
        return false;
    }
    writer.write_u32(static_cast<std::uint32_t>(count));
    return true;
}

bool read_count(io::ByteReader& reader, std::uint32_t& count) {
    if (!reader.has_bytes(4)) {
        return false;
    }
    count = reader.read_u32();
    return count <= kMaximumFieldCount;
}

template <typename T, typename Writer>
bool write_optional(io::ByteWriter& writer,
                    const std::optional<T>& value,
                    Writer&& write_value,
                    std::string& error) {
    write_bool(writer, value.has_value());
    return !value.has_value() || write_value(writer, *value, error);
}

template <typename T, typename Reader>
bool read_optional(io::ByteReader& reader,
                   std::optional<T>& value,
                   Reader&& read_value) {
    bool present = false;
    if (!read_bool(reader, present)) return false;
    if (!present) {
        value.reset();
        return true;
    }
    T decoded{};
    if (!read_value(reader, decoded)) return false;
    value = std::move(decoded);
    return true;
}

bool write_pool(io::ByteWriter& writer,
                const PoolState& pool,
                std::string& error) {
    writer.write_u32(pool.index);
    write_u64(writer, pool.base_address);
    writer.write_u32(pool.stride);
    writer.write_u32(pool.capacity);
    writer.write_u32(pool.used_count);
    return write_optional(
        writer,
        pool.live_count,
        [](io::ByteWriter& output, std::uint32_t value, std::string&) {
            output.write_u32(value);
            return true;
        },
        error);
}

bool read_pool(io::ByteReader& reader, PoolState& pool) {
    if (!reader.has_bytes(24)) {
        return false;
    }
    pool.index = reader.read_u32();
    pool.base_address = read_u64(reader);
    pool.stride = reader.read_u32();
    pool.capacity = reader.read_u32();
    pool.used_count = reader.read_u32();
    return read_optional(
        reader,
        pool.live_count,
        [](io::ByteReader& input, std::uint32_t& value) {
            if (!input.has_bytes(4)) return false;
            value = input.read_u32();
            return true;
        });
}

bool write_identity(io::ByteWriter& writer,
                    const EntityIdentity& identity,
                    std::string& error) {
    writer.write_i32(identity.pool);
    writer.write_i32(identity.slot);
    writer.write_u32(identity.wire_handle);
    writer.write_i32(identity.bms_id);
    writer.write_u16(identity.ssn);
    writer.write_u16(identity.net_id);
    writer.write_u32(identity.owner_connection_id);
    if (!write_optional(
            writer,
            identity.type_id,
            [](io::ByteWriter& output, std::uint32_t value, std::string&) {
                output.write_u32(value);
                return true;
            },
            error)) {
        return false;
    }
    return write_string(writer, identity.name, error);
}

bool read_identity(io::ByteReader& reader, EntityIdentity& identity) {
    if (!reader.has_bytes(24)) {
        return false;
    }
    identity.pool = reader.read_i32();
    identity.slot = reader.read_i32();
    identity.wire_handle = reader.read_u32();
    identity.bms_id = reader.read_i32();
    identity.ssn = reader.read_u16();
    identity.net_id = reader.read_u16();
    identity.owner_connection_id = reader.read_u32();
    return read_optional(
               reader,
               identity.type_id,
               [](io::ByteReader& input, std::uint32_t& value) {
                   if (!input.has_bytes(4)) return false;
                   value = input.read_u32();
                   return true;
               }) &&
           read_string(reader, identity.name);
}

void write_transform(io::ByteWriter& writer,
                     const EntityTransform& transform) {
    writer.write_i32(transform.raw_x);
    writer.write_i32(transform.raw_y);
    writer.write_i32(transform.raw_z);
    writer.write_i32(transform.raw_heading);
    writer.write_i32(transform.raw_pitch);
    writer.write_i32(transform.raw_roll);
    write_f64(writer, transform.x);
    write_f64(writer, transform.y);
    write_f64(writer, transform.z);
    write_f64(writer, transform.yaw_deg);
    write_f64(writer, transform.pitch_deg);
    write_f64(writer, transform.roll_deg);
    writer.write_u8(static_cast<std::uint8_t>(transform.raw_encoding));
}

bool read_transform(io::ByteReader& reader, EntityTransform& transform) {
    if (!reader.has_bytes(73)) {
        return false;
    }
    transform.raw_x = reader.read_i32();
    transform.raw_y = reader.read_i32();
    transform.raw_z = reader.read_i32();
    transform.raw_heading = reader.read_i32();
    transform.raw_pitch = reader.read_i32();
    transform.raw_roll = reader.read_i32();
    transform.x = read_f64(reader);
    transform.y = read_f64(reader);
    transform.z = read_f64(reader);
    transform.yaw_deg = read_f64(reader);
    transform.pitch_deg = read_f64(reader);
    transform.roll_deg = read_f64(reader);
    transform.raw_encoding =
        static_cast<RawTransformEncoding>(reader.read_u8());
    return transform.raw_encoding <=
           RawTransformEncoding::fixed_point_16_16_bam32;
}

void write_pose(io::ByteWriter& writer, const PoseState& pose) {
    write_f64(writer, pose.x);
    write_f64(writer, pose.y);
    write_f64(writer, pose.z);
    write_f64(writer, pose.yaw_deg);
    write_f64(writer, pose.pitch_deg);
    write_f64(writer, pose.roll_deg);
}

bool read_pose(io::ByteReader& reader, PoseState& pose) {
    if (!reader.has_bytes(48)) {
        return false;
    }
    pose.x = read_f64(reader);
    pose.y = read_f64(reader);
    pose.z = read_f64(reader);
    pose.yaw_deg = read_f64(reader);
    pose.pitch_deg = read_f64(reader);
    pose.roll_deg = read_f64(reader);
    return true;
}

bool write_entity(io::ByteWriter& writer,
                  const EntityState& entity,
                  std::string& error) {
    if (!write_identity(writer, entity.identity, error)) {
        return false;
    }
    if (!write_optional(
            writer,
            entity.transform,
            [](io::ByteWriter& output,
               const EntityTransform& transform,
               std::string&) {
                write_transform(output, transform);
                return true;
            },
            error)) {
        return false;
    }
    writer.write_u8(static_cast<std::uint8_t>(entity.kind));
    writer.write_u32(entity.flags);
    writer.write_i32(entity.team);
    writer.write_u32(entity.player_class);
    writer.write_i32(entity.health);
    if (!write_optional(
            writer,
            entity.max_health,
            [](io::ByteWriter& output, std::int32_t value, std::string&) {
                output.write_i32(value);
                return true;
            },
            error)) {
        return false;
    }
    if (!write_optional(
            writer,
            entity.armor,
            [](io::ByteWriter& output, std::int32_t value, std::string&) {
                output.write_i32(value);
                return true;
            },
            error)) {
        return false;
    }
    write_bool(writer, entity.alive);
    const auto write_optional_bool =
        [](io::ByteWriter& output, bool value, std::string&) {
            write_bool(output, value);
            return true;
        };
    const auto write_optional_i32 =
        [](io::ByteWriter& output, std::int32_t value, std::string&) {
            output.write_i32(value);
            return true;
        };
    if (!write_optional(writer, entity.hidden, write_optional_bool, error) ||
        !write_optional(writer, entity.held, write_optional_bool, error) ||
        !write_optional(writer, entity.disabled, write_optional_bool, error)) {
        return false;
    }
    writer.write_i32(entity.body_anim_slot);
    if (!write_optional(writer, entity.anim_state, write_optional_i32, error) ||
        !write_optional(
            writer, entity.anim_phase_ticks, write_optional_i32, error)) {
        return false;
    }
    writer.write_i32(entity.equipped_adm_index);
    return true;
}

bool read_entity(io::ByteReader& reader, EntityState& entity) {
    if (!read_identity(reader, entity.identity) ||
        !read_optional(
            reader,
            entity.transform,
            [](io::ByteReader& input, EntityTransform& transform) {
                return read_transform(input, transform);
            }) ||
        !reader.has_bytes(17)) {
        return false;
    }
    entity.kind = static_cast<EntityKind>(reader.read_u8());
    if (entity.kind > EntityKind::marker) {
        return false;
    }
    entity.flags = reader.read_u32();
    entity.team = reader.read_i32();
    entity.player_class = reader.read_u32();
    entity.health = reader.read_i32();
    if (!read_optional(
            reader,
            entity.max_health,
            [](io::ByteReader& input, std::int32_t& value) {
                if (!input.has_bytes(4)) return false;
                value = input.read_i32();
                return true;
            }) ||
        !read_optional(
            reader,
            entity.armor,
            [](io::ByteReader& input, std::int32_t& value) {
                if (!input.has_bytes(4)) return false;
                value = input.read_i32();
                return true;
            })) {
        return false;
    }
    const auto read_optional_bool =
        [](io::ByteReader& input, bool& value) {
            return read_bool(input, value);
        };
    const auto read_optional_i32 =
        [](io::ByteReader& input, std::int32_t& value) {
            if (!input.has_bytes(4)) return false;
            value = input.read_i32();
            return true;
        };
    if (!read_bool(reader, entity.alive) ||
        !read_optional(reader, entity.hidden, read_optional_bool) ||
        !read_optional(reader, entity.held, read_optional_bool) ||
        !read_optional(reader, entity.disabled, read_optional_bool) ||
        !reader.has_bytes(4)) {
        return false;
    }
    entity.body_anim_slot = reader.read_i32();
    if (!read_optional(reader, entity.anim_state, read_optional_i32) ||
        !read_optional(
            reader, entity.anim_phase_ticks, read_optional_i32) ||
        !reader.has_bytes(4)) {
        return false;
    }
    entity.equipped_adm_index = reader.read_i32();
    return true;
}

bool write_player(io::ByteWriter& writer,
                  const PlayerState& player,
                  std::string& error) {
    write_bool(writer, player.present);
    if (!write_identity(writer, player.identity, error)) {
        return false;
    }
    if (!write_optional(
            writer,
            player.transform,
            [](io::ByteWriter& output,
               const EntityTransform& transform,
               std::string&) {
                write_transform(output, transform);
                return true;
            },
            error)) {
        return false;
    }
    writer.write_i32(player.health);
    if (!write_optional(
            writer,
            player.max_health,
            [](io::ByteWriter& output, std::int32_t value, std::string&) {
                output.write_i32(value);
                return true;
            },
            error)) {
        return false;
    }
    if (!write_optional(
            writer,
            player.armor,
            [](io::ByteWriter& output, std::int32_t value, std::string&) {
                output.write_i32(value);
                return true;
            },
            error)) {
        return false;
    }
    writer.write_i32(player.team);
    writer.write_u32(player.player_class);
    writer.write_i32(player.equipped_adm_index);
    return true;
}

bool read_player(io::ByteReader& reader, PlayerState& player) {
    if (!read_bool(reader, player.present) ||
        !read_identity(reader, player.identity) ||
        !read_optional(
            reader,
            player.transform,
            [](io::ByteReader& input, EntityTransform& transform) {
                return read_transform(input, transform);
            }) ||
        !reader.has_bytes(4)) {
        return false;
    }
    player.health = reader.read_i32();
    if (!read_optional(
            reader,
            player.max_health,
            [](io::ByteReader& input, std::int32_t& value) {
                if (!input.has_bytes(4)) return false;
                value = input.read_i32();
                return true;
            }) ||
        !read_optional(
            reader,
            player.armor,
            [](io::ByteReader& input, std::int32_t& value) {
                if (!input.has_bytes(4)) return false;
                value = input.read_i32();
                return true;
            }) ||
        !reader.has_bytes(12)) {
        return false;
    }
    player.team = reader.read_i32();
    player.player_class = reader.read_u32();
    player.equipped_adm_index = reader.read_i32();
    return true;
}

bool write_weapon(io::ByteWriter& writer,
                  const WeaponState& weapon,
                  std::string& error) {
    write_bool(writer, weapon.present);
    if (!write_string(writer, weapon.name, error)) {
        return false;
    }
    writer.write_i32(weapon.special_hold);
    writer.write_i32(weapon.attack_anim);
    write_pose(writer, weapon.primary);
    write_pose(writer, weapon.alternate);
    write_f64(writer, weapon.render_fov);
    return write_optional(
               writer,
               weapon.action,
               [](io::ByteWriter& output,
                  const std::string& value,
                  std::string& write_error) {
                   return write_string(output, value, write_error);
               },
               error) &&
           write_optional(
               writer,
               weapon.clip,
               [](io::ByteWriter& output, std::int32_t value, std::string&) {
                   output.write_i32(value);
                   return true;
               },
               error) &&
           write_optional(
               writer,
               weapon.reserve,
               [](io::ByteWriter& output, std::int32_t value, std::string&) {
                   output.write_i32(value);
                   return true;
               },
               error);
}

bool read_weapon(io::ByteReader& reader, WeaponState& weapon) {
    if (!read_bool(reader, weapon.present) ||
        !read_string(reader, weapon.name) ||
        !reader.has_bytes(8)) {
        return false;
    }
    weapon.special_hold = reader.read_i32();
    weapon.attack_anim = reader.read_i32();
    if (!read_pose(reader, weapon.primary) ||
        !read_pose(reader, weapon.alternate) ||
        !reader.has_bytes(8)) {
        return false;
    }
    weapon.render_fov = read_f64(reader);
    return read_optional(
               reader,
               weapon.action,
               [](io::ByteReader& input, std::string& value) {
                   return read_string(input, value);
               }) &&
           read_optional(
               reader,
               weapon.clip,
               [](io::ByteReader& input, std::int32_t& value) {
                   if (!input.has_bytes(4)) return false;
                   value = input.read_i32();
                   return true;
               }) &&
           read_optional(
               reader,
               weapon.reserve,
               [](io::ByteReader& input, std::int32_t& value) {
                   if (!input.has_bytes(4)) return false;
                   value = input.read_i32();
                   return true;
               });
}

bool write_camera(io::ByteWriter& writer,
                  const CameraState& camera,
                  std::string& error) {
    write_bool(writer, camera.present);
    if (!write_optional(
            writer,
            camera.transform,
            [](io::ByteWriter& output,
               const EntityTransform& transform,
               std::string&) {
                write_transform(output, transform);
                return true;
            },
            error)) {
        return false;
    }
    write_f64(writer, camera.fov_deg);
    write_bool(writer, camera.third_person);
    write_bool(writer, camera.scope_engaged);
    write_f64(writer, camera.scope_fraction);
    return true;
}

bool read_camera(io::ByteReader& reader, CameraState& camera) {
    if (!read_bool(reader, camera.present) ||
        !read_optional(
            reader,
            camera.transform,
            [](io::ByteReader& input, EntityTransform& transform) {
                return read_transform(input, transform);
            }) ||
        !reader.has_bytes(18)) {
        return false;
    }
    camera.fov_deg = read_f64(reader);
    if (!read_bool(reader, camera.third_person) ||
        !read_bool(reader, camera.scope_engaged)) {
        return false;
    }
    camera.scope_fraction = read_f64(reader);
    return true;
}

void write_input(io::ByteWriter& writer, const InputState& input) {
    writer.write_u32(input.move_order);
    writer.write_i32(input.analog_x);
    writer.write_i32(input.analog_y);
    writer.write_i32(input.analog_z);
    write_bool(writer, input.forward);
    write_bool(writer, input.back);
    write_bool(writer, input.left);
    write_bool(writer, input.right);
    write_bool(writer, input.run);
    write_bool(writer, input.crouch);
    write_bool(writer, input.prone);
    write_bool(writer, input.jump);
    write_f64(writer, input.look_yaw_deg);
    write_f64(writer, input.look_pitch_deg);
}

bool read_input(io::ByteReader& reader, InputState& input) {
    if (!reader.has_bytes(16)) {
        return false;
    }
    input.move_order = reader.read_u32();
    input.analog_x = reader.read_i32();
    input.analog_y = reader.read_i32();
    input.analog_z = reader.read_i32();
    if (!read_bool(reader, input.forward) ||
        !read_bool(reader, input.back) ||
        !read_bool(reader, input.left) ||
        !read_bool(reader, input.right) ||
        !read_bool(reader, input.run) ||
        !read_bool(reader, input.crouch) ||
        !read_bool(reader, input.prone) ||
        !read_bool(reader, input.jump) ||
        !reader.has_bytes(16)) {
        return false;
    }
    input.look_yaw_deg = read_f64(reader);
    input.look_pitch_deg = read_f64(reader);
    return true;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool append_chunk(std::vector<std::uint8_t>& destination,
                  std::uint32_t kind,
                  const std::vector<std::uint8_t>& payload,
                  std::string& error) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "trace chunk exceeds the 32-bit format limit";
        return false;
    }
    io::ByteWriter chunk;
    chunk.write_u32(kind);
    chunk.write_u32(static_cast<std::uint32_t>(payload.size()));
    chunk.write_u32(crc32(payload.data(), payload.size()));
    chunk.write_bytes(payload);
    const auto& bytes = chunk.data();
    destination.insert(destination.end(), bytes.begin(), bytes.end());
    return true;
}

TraceReadResult failure(Trace trace,
                        TraceReadStatus status,
                        std::size_t offset,
                        std::string detail) {
    return TraceReadResult{
        std::move(trace), status, offset, std::move(detail)};
}

}  // namespace

TraceWriter::TraceWriter() {
    io::ByteWriter header;
    header.write_bytes(kMagic.data(), kMagic.size());
    header.write_u16(kTraceMajorVersion);
    header.write_u16(kTraceMinorVersion);
    bytes_ = header.take();
}

bool TraceWriter::append(const RunMetadata& metadata) {
    last_error_.clear();
    io::ByteWriter payload;
    write_u64(payload, metadata.started_unix_ns);
    if (!write_producer_identity(payload, metadata.identity, last_error_) ||
        !write_string(payload, metadata.run_id, last_error_) ||
        !write_string(payload, metadata.producer, last_error_) ||
        !write_string(payload, metadata.build_id, last_error_) ||
        !write_string(payload, metadata.scenario, last_error_) ||
        !write_string(payload, metadata.title, last_error_) ||
        !write_string(payload, metadata.expansion, last_error_) ||
        !write_string(payload, metadata.mission, last_error_)) {
        return false;
    }
    return append_chunk(bytes_, kRunMetadataChunk, payload.data(), last_error_);
}

bool TraceWriter::append(const FrameSnapshot& snapshot) {
    last_error_.clear();
    io::ByteWriter payload;
    if (!write_producer_identity(payload, snapshot.identity, last_error_)) {
        return false;
    }
    payload.write_u8(static_cast<std::uint8_t>(snapshot.lane));
    write_u64(payload, snapshot.frame_index);
    write_u64(payload, snapshot.simulation_tick);
    write_u64(payload, snapshot.timestamp_ns);
    if (!write_count(payload, snapshot.pools.size(), last_error_)) {
        return false;
    }
    for (const PoolState& pool : snapshot.pools) {
        if (!write_pool(payload, pool, last_error_)) return false;
    }
    if (!write_count(payload, snapshot.entities.size(), last_error_)) {
        return false;
    }
    for (const EntityState& entity : snapshot.entities) {
        if (!write_entity(payload, entity, last_error_)) {
            return false;
        }
    }
    if (!write_count(
            payload, snapshot.complete_entity_pools.size(), last_error_)) {
        return false;
    }
    for (std::uint32_t pool_index : snapshot.complete_entity_pools) {
        payload.write_u32(pool_index);
    }
    if (!write_optional(
            payload,
            snapshot.player,
            [](io::ByteWriter& writer,
               const PlayerState& player,
               std::string& error) {
                return write_player(writer, player, error);
            },
            last_error_) ||
        !write_optional(
            payload,
            snapshot.weapon,
            [](io::ByteWriter& writer,
               const WeaponState& weapon,
               std::string& error) {
                return write_weapon(writer, weapon, error);
            },
            last_error_) ||
        !write_optional(
            payload,
            snapshot.camera,
            [](io::ByteWriter& writer,
               const CameraState& camera,
               std::string& error) {
                return write_camera(writer, camera, error);
            },
            last_error_) ||
        !write_optional(
            payload,
            snapshot.input,
            [](io::ByteWriter& writer,
               const InputState& input,
               std::string&) {
                write_input(writer, input);
                return true;
            },
            last_error_) ||
        !write_fields(payload, snapshot.extensions, last_error_)) {
        return false;
    }
    return append_chunk(bytes_, kFrameSnapshotChunk, payload.data(), last_error_);
}

bool TraceWriter::append(const Checkpoint& checkpoint) {
    last_error_.clear();
    io::ByteWriter payload;
    if (!write_producer_identity(payload, checkpoint.identity, last_error_) ||
        !write_string(payload, checkpoint.name, last_error_)) {
        return false;
    }
    write_u64(payload, checkpoint.occurrence);
    write_u64(payload, checkpoint.frame_index);
    payload.write_u8(static_cast<std::uint8_t>(checkpoint.lane));
    return append_chunk(bytes_, kCheckpointChunk, payload.data(), last_error_);
}

bool TraceWriter::append(const NetworkDatagram& datagram) {
    last_error_.clear();
    io::ByteWriter payload;
    if (!write_producer_identity(payload, datagram.identity, last_error_)) {
        return false;
    }
    write_u64(payload, datagram.timestamp_ns);
    write_u64(payload, datagram.socket_id);
    payload.write_u32(datagram.total_size);
    payload.write_u8(datagram.truncated ? 1U : 0U);
    payload.write_u8(static_cast<std::uint8_t>(datagram.direction));
    if (!write_string(payload, datagram.source.address, last_error_)) {
        return false;
    }
    payload.write_u16(datagram.source.port);
    if (!write_string(payload, datagram.destination.address, last_error_)) {
        return false;
    }
    payload.write_u16(datagram.destination.port);
    if (!write_byte_string(payload, datagram.payload, last_error_)) {
        return false;
    }
    return append_chunk(bytes_, kNetworkDatagramChunk, payload.data(), last_error_);
}

bool TraceWriter::append(const DecodedNetworkEvent& event) {
    last_error_.clear();
    io::ByteWriter payload;
    if (!write_producer_identity(payload, event.identity, last_error_)) {
        return false;
    }
    write_u64(payload, event.simulation_tick);
    payload.write_u8(static_cast<std::uint8_t>(event.direction));
    payload.write_u32(event.message_id);
    if (!write_string(payload, event.name, last_error_) ||
        !write_fields(payload, event.fields, last_error_)) {
        return false;
    }
    return append_chunk(bytes_,
                        kDecodedNetworkEventChunk,
                        payload.data(),
                        last_error_);
}

bool TraceWriter::append(const MutationAudit& audit) {
    last_error_.clear();
    io::ByteWriter payload;
    if (!write_producer_identity(payload, audit.identity, last_error_)) {
        return false;
    }
    write_u64(payload, audit.simulation_tick);
    write_u64(payload, audit.timestamp_ns);
    if (!write_string(payload, audit.target, last_error_) ||
        !write_string(payload, audit.operation, last_error_)) {
        return false;
    }
    payload.write_u8(static_cast<std::uint8_t>(audit.result));
    if (!write_scalar(payload, audit.before, last_error_) ||
        !write_scalar(payload, audit.after, last_error_) ||
        !write_string(payload, audit.detail, last_error_)) {
        return false;
    }
    return append_chunk(bytes_, kMutationAuditChunk, payload.data(), last_error_);
}

bool TraceWriter::append(const DiagnosticEvent& diagnostic) {
    last_error_.clear();
    io::ByteWriter payload;
    if (!write_producer_identity(
            payload, diagnostic.identity, last_error_)) {
        return false;
    }
    write_u64(payload, diagnostic.simulation_tick);
    write_u64(payload, diagnostic.timestamp_ns);
    payload.write_u8(static_cast<std::uint8_t>(diagnostic.severity));
    if (!write_string(payload, diagnostic.code, last_error_) ||
        !write_string(payload, diagnostic.message, last_error_) ||
        !write_fields(payload, diagnostic.context, last_error_)) {
        return false;
    }
    return append_chunk(bytes_, kDiagnosticEventChunk, payload.data(), last_error_);
}

const std::vector<std::uint8_t>& TraceWriter::bytes() const noexcept {
    return bytes_;
}

const std::string& TraceWriter::last_error() const noexcept {
    return last_error_;
}

TraceReadResult read_trace(const std::uint8_t* data, std::size_t size) {
    Trace trace{};
    if (data == nullptr || size < kFileHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), data)) {
        return failure(std::move(trace),
                       TraceReadStatus::invalid_header,
                       0,
                       "missing OpenNova parity trace header");
    }

    io::ByteReader reader(data + kMagic.size(), size - kMagic.size());
    trace.major_version = reader.read_u16();
    trace.minor_version = reader.read_u16();
    if (trace.major_version != kTraceMajorVersion) {
        return failure(std::move(trace),
                       TraceReadStatus::unsupported_version,
                       4,
                       "unsupported parity trace major version");
    }

    std::size_t absolute_offset = kFileHeaderSize;
    while (reader.remaining() != 0) {
        if (!reader.has_bytes(kChunkHeaderSize)) {
            return failure(std::move(trace),
                           TraceReadStatus::truncated_tail,
                           absolute_offset,
                           "incomplete trace chunk header");
        }
        const std::uint32_t kind = reader.read_u32();
        const std::uint32_t payload_size = reader.read_u32();
        const std::uint32_t expected_crc = reader.read_u32();
        if (!reader.has_bytes(payload_size)) {
            return failure(std::move(trace),
                           TraceReadStatus::truncated_tail,
                           absolute_offset,
                           "incomplete trace chunk payload");
        }

        std::vector<std::uint8_t> payload;
        reader.read_bytes(payload, payload_size);
        if (crc32(payload.data(), payload.size()) != expected_crc) {
            return failure(std::move(trace),
                           TraceReadStatus::checksum_mismatch,
                           absolute_offset,
                           "trace chunk checksum does not match its payload");
        }
        io::ByteReader payload_reader(payload.data(), payload.size());
        bool parsed = false;
        if (kind == kRunMetadataChunk) {
            RunMetadata metadata{};
            if (payload_reader.has_bytes(sizeof(std::uint64_t))) {
                metadata.started_unix_ns = read_u64(payload_reader);
                parsed = read_producer_identity(
                             payload_reader, metadata.identity) &&
                         read_string(payload_reader, metadata.run_id) &&
                         read_string(payload_reader, metadata.producer) &&
                         read_string(payload_reader, metadata.build_id) &&
                         read_string(payload_reader, metadata.scenario) &&
                         read_string(payload_reader, metadata.title) &&
                         read_string(payload_reader, metadata.expansion) &&
                         read_string(payload_reader, metadata.mission);
            }
            if (parsed) trace.events.emplace_back(std::move(metadata));
        } else if (kind == kFrameSnapshotChunk) {
            FrameSnapshot snapshot{};
            parsed = read_producer_identity(
                payload_reader, snapshot.identity);
            if (parsed && payload_reader.has_bytes(25)) {
                snapshot.lane =
                    static_cast<StateLane>(payload_reader.read_u8());
                snapshot.frame_index = read_u64(payload_reader);
                snapshot.simulation_tick = read_u64(payload_reader);
                snapshot.timestamp_ns = read_u64(payload_reader);
                parsed = snapshot.lane <= StateLane::wire;
            }
            std::uint32_t pool_count = 0;
            if (parsed) parsed = read_count(payload_reader, pool_count);
            if (parsed) {
                snapshot.pools.reserve(pool_count);
                for (std::uint32_t index = 0; index < pool_count; ++index) {
                    PoolState pool{};
                    if (!read_pool(payload_reader, pool)) {
                        parsed = false;
                        break;
                    }
                    snapshot.pools.push_back(pool);
                }
            }
            std::uint32_t entity_count = 0;
            if (parsed) parsed = read_count(payload_reader, entity_count);
            if (parsed) {
                snapshot.entities.reserve(entity_count);
                for (std::uint32_t index = 0; index < entity_count; ++index) {
                    EntityState entity{};
                    if (!read_entity(payload_reader, entity)) {
                        parsed = false;
                        break;
                    }
                    snapshot.entities.push_back(std::move(entity));
                }
            }
            std::uint32_t coverage_count = 0;
            if (parsed) parsed = read_count(payload_reader, coverage_count);
            if (parsed &&
                !payload_reader.has_bytes(
                    static_cast<std::size_t>(coverage_count) * 4U)) {
                parsed = false;
            }
            if (parsed) {
                snapshot.complete_entity_pools.reserve(coverage_count);
                for (std::uint32_t index = 0; index < coverage_count; ++index) {
                    snapshot.complete_entity_pools.push_back(
                        payload_reader.read_u32());
                }
            }
            if (parsed) {
                parsed = read_optional(
                    payload_reader,
                    snapshot.player,
                    [](io::ByteReader& reader, PlayerState& player) {
                        return read_player(reader, player);
                    });
            }
            if (parsed) {
                parsed = read_optional(
                    payload_reader,
                    snapshot.weapon,
                    [](io::ByteReader& reader, WeaponState& weapon) {
                        return read_weapon(reader, weapon);
                    });
            }
            if (parsed) {
                parsed = read_optional(
                    payload_reader,
                    snapshot.camera,
                    [](io::ByteReader& reader, CameraState& camera) {
                        return read_camera(reader, camera);
                    });
            }
            if (parsed) {
                parsed = read_optional(
                    payload_reader,
                    snapshot.input,
                    [](io::ByteReader& reader, InputState& input) {
                        return read_input(reader, input);
                    });
            }
            if (parsed) {
                parsed = read_fields(payload_reader, snapshot.extensions);
            }
            if (parsed) trace.events.emplace_back(std::move(snapshot));
        } else if (kind == kCheckpointChunk) {
            Checkpoint checkpoint{};
            parsed = read_producer_identity(
                         payload_reader, checkpoint.identity) &&
                     read_string(payload_reader, checkpoint.name);
            if (parsed && payload_reader.has_bytes(17)) {
                checkpoint.occurrence = read_u64(payload_reader);
                checkpoint.frame_index = read_u64(payload_reader);
                checkpoint.lane =
                    static_cast<StateLane>(payload_reader.read_u8());
                parsed = checkpoint.lane <= StateLane::wire;
            } else {
                parsed = false;
            }
            if (parsed) trace.events.emplace_back(std::move(checkpoint));
        } else if (kind == kNetworkDatagramChunk) {
            NetworkDatagram datagram{};
            parsed = read_producer_identity(
                payload_reader, datagram.identity);
            if (parsed && payload_reader.has_bytes(22)) {
                datagram.timestamp_ns = read_u64(payload_reader);
                datagram.socket_id = read_u64(payload_reader);
                datagram.total_size = payload_reader.read_u32();
                const std::uint8_t truncated = payload_reader.read_u8();
                const auto direction =
                    static_cast<DatagramDirection>(payload_reader.read_u8());
                if (truncated <= 1 &&
                    (direction == DatagramDirection::inbound ||
                     direction == DatagramDirection::outbound)) {
                    datagram.truncated = truncated != 0;
                    datagram.direction = direction;
                    parsed = read_string(payload_reader, datagram.source.address);
                }
            }
            if (parsed && payload_reader.has_bytes(2)) {
                datagram.source.port = payload_reader.read_u16();
                parsed = read_string(payload_reader, datagram.destination.address);
            } else {
                parsed = false;
            }
            if (parsed && payload_reader.has_bytes(2)) {
                datagram.destination.port = payload_reader.read_u16();
                parsed = read_byte_string(payload_reader, datagram.payload);
            } else {
                parsed = false;
            }
            if (parsed) trace.events.emplace_back(std::move(datagram));
        } else if (kind == kDecodedNetworkEventChunk) {
            DecodedNetworkEvent event{};
            parsed = read_producer_identity(payload_reader, event.identity);
            if (parsed && payload_reader.has_bytes(13)) {
                event.simulation_tick = read_u64(payload_reader);
                const auto direction =
                    static_cast<DatagramDirection>(payload_reader.read_u8());
                if (direction == DatagramDirection::inbound ||
                    direction == DatagramDirection::outbound) {
                    event.direction = direction;
                    event.message_id = payload_reader.read_u32();
                    parsed = read_string(payload_reader, event.name) &&
                             read_fields(payload_reader, event.fields);
                }
            }
            if (parsed) trace.events.emplace_back(std::move(event));
        } else if (kind == kMutationAuditChunk) {
            MutationAudit audit{};
            parsed = read_producer_identity(payload_reader, audit.identity);
            if (parsed && payload_reader.has_bytes(16)) {
                audit.simulation_tick = read_u64(payload_reader);
                audit.timestamp_ns = read_u64(payload_reader);
                parsed = read_string(payload_reader, audit.target) &&
                         read_string(payload_reader, audit.operation) &&
                         payload_reader.has_bytes(1);
            }
            if (parsed) {
                audit.result =
                    static_cast<MutationResult>(payload_reader.read_u8());
                parsed = audit.result <= MutationResult::unverified &&
                         read_scalar(payload_reader, audit.before) &&
                         read_scalar(payload_reader, audit.after) &&
                         read_string(payload_reader, audit.detail);
            }
            if (parsed) trace.events.emplace_back(std::move(audit));
        } else if (kind == kDiagnosticEventChunk) {
            DiagnosticEvent diagnostic{};
            parsed = read_producer_identity(
                payload_reader, diagnostic.identity);
            if (parsed && payload_reader.has_bytes(17)) {
                diagnostic.simulation_tick = read_u64(payload_reader);
                diagnostic.timestamp_ns = read_u64(payload_reader);
                diagnostic.severity =
                    static_cast<Severity>(payload_reader.read_u8());
                parsed = diagnostic.severity <= Severity::fatal &&
                         read_string(payload_reader, diagnostic.code) &&
                         read_string(payload_reader, diagnostic.message) &&
                         read_fields(payload_reader, diagnostic.context);
            }
            if (parsed) trace.events.emplace_back(std::move(diagnostic));
        } else {
            parsed = true;
        }
        if (!parsed || payload_reader.remaining() != 0) {
            return failure(std::move(trace),
                           TraceReadStatus::malformed_chunk,
                           absolute_offset,
                           "trace event payload is malformed");
        }
        absolute_offset += kChunkHeaderSize + payload_size;
    }
    return TraceReadResult{std::move(trace),
                           TraceReadStatus::complete,
                           size,
                           {}};
}

}  // namespace opennova::parity
