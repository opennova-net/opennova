#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace opennova::parity {

inline constexpr std::uint16_t kTraceMajorVersion = 1;
inline constexpr std::uint16_t kTraceMinorVersion = 0;

enum class Severity : std::uint8_t {
    info = 0,
    warning = 1,
    error = 2,
    fatal = 3,
};

using ByteString = std::vector<std::uint8_t>;
using ScalarValue = std::variant<std::int64_t,
                                 std::uint64_t,
                                 double,
                                 bool,
                                 std::string,
                                 ByteString>;

struct Field {
    std::string name{};
    ScalarValue value{};
    double absolute_tolerance{};
    Severity mismatch_severity{Severity::error};
};

enum class SourceKind : std::uint8_t {
    unknown = 0,
    retail = 1,
    opennova = 2,
};

enum class RunRole : std::uint8_t {
    unknown = 0,
    client = 1,
    host = 2,
    dedicated_server = 3,
};

struct ProducerIdentity {
    SourceKind source{SourceKind::unknown};
    RunRole role{RunRole::unknown};
    std::string stream_id{};
};

struct RunMetadata {
    std::string run_id{};
    std::string producer{};
    std::string build_id{};
    std::string scenario{};
    std::uint64_t started_unix_ns{};
    ProducerIdentity identity{};
    std::string title{};
    std::string expansion{};
    std::string mission{};
};

enum class StateLane : std::uint8_t {
    unknown = 0,
    authoritative = 1,
    presented = 2,
    local_input = 3,
    wire = 4,
};

enum class EntityKind : std::uint8_t {
    unknown = 0,
    organic = 1,
    player = 2,
    vehicle = 3,
    item = 4,
    building = 5,
    projectile = 6,
    effect = 7,
    marker = 8,
};

struct PoolState {
    std::uint32_t index{};
    std::uint64_t base_address{};
    std::uint32_t stride{};
    std::uint32_t capacity{};
    std::uint32_t used_count{};
    std::optional<std::uint32_t> live_count{};
};

struct EntityIdentity {
    std::int32_t pool{};
    std::int32_t slot{};
    std::uint32_t wire_handle{};
    std::int32_t bms_id{};
    std::uint16_t ssn{};
    std::uint16_t net_id{};
    std::uint32_t owner_connection_id{};
    std::optional<std::uint32_t> type_id{};
    std::string name{};
};

enum class RawTransformEncoding : std::uint8_t {
    unavailable = 0,
    fixed_point_16_16_bam32 = 1,
};

struct EntityTransform {
    std::int32_t raw_x{};
    std::int32_t raw_y{};
    std::int32_t raw_z{};
    std::int32_t raw_heading{};
    std::int32_t raw_pitch{};
    std::int32_t raw_roll{};
    double x{};
    double y{};
    double z{};
    double yaw_deg{};
    double pitch_deg{};
    double roll_deg{};
    RawTransformEncoding raw_encoding{RawTransformEncoding::unavailable};
};

struct EntityState {
    EntityIdentity identity{};
    std::optional<EntityTransform> transform{};
    EntityKind kind{EntityKind::unknown};
    std::uint32_t flags{};
    std::int32_t team{};
    std::uint32_t player_class{};
    std::int32_t health{};
    std::optional<std::int32_t> max_health{};
    std::optional<std::int32_t> armor{};
    bool alive{};
    std::optional<bool> hidden{};
    std::optional<bool> held{};
    std::optional<bool> disabled{};
    std::int32_t body_anim_slot{};
    std::optional<std::int32_t> anim_state{};
    std::optional<std::int32_t> anim_phase_ticks{};
    std::int32_t equipped_adm_index{};
};

struct PlayerState {
    bool present{};
    EntityIdentity identity{};
    std::optional<EntityTransform> transform{};
    std::int32_t health{};
    std::optional<std::int32_t> max_health{};
    std::optional<std::int32_t> armor{};
    std::int32_t team{};
    std::uint32_t player_class{};
    std::int32_t equipped_adm_index{};
};

struct PoseState {
    // Weapon translation uses canonical weapon.def file units. Retail stores
    // these values scaled by 256 in WeaponDef and its adapter divides by 256;
    // OpenNova's DEF parser already retains the canonical file values.
    double x{};
    double y{};
    double z{};
    double yaw_deg{};
    double pitch_deg{};
    double roll_deg{};
};

struct WeaponState {
    bool present{};
    std::string name{};
    std::int32_t special_hold{};
    std::int32_t attack_anim{};
    PoseState primary{};
    PoseState alternate{};
    double render_fov{};
    std::optional<std::string> action{};
    std::optional<std::int32_t> clip{};
    std::optional<std::int32_t> reserve{};
};

struct CameraState {
    bool present{};
    std::optional<EntityTransform> transform{};
    double fov_deg{};
    bool third_person{};
    bool scope_engaged{};
    double scope_fraction{};
};

struct InputState {
    std::uint32_t move_order{};
    std::int32_t analog_x{};
    std::int32_t analog_y{};
    std::int32_t analog_z{};
    bool forward{};
    bool back{};
    bool left{};
    bool right{};
    bool run{};
    bool crouch{};
    bool prone{};
    bool jump{};
    double look_yaw_deg{};
    double look_pitch_deg{};
};

struct FrameSnapshot {
    ProducerIdentity identity{};
    StateLane lane{StateLane::unknown};
    std::uint64_t frame_index{};
    std::uint64_t simulation_tick{};
    std::uint64_t timestamp_ns{};
    std::vector<PoolState> pools{};
    std::vector<EntityState> entities{};
    std::vector<std::uint32_t> complete_entity_pools{};
    std::optional<PlayerState> player{};
    std::optional<WeaponState> weapon{};
    std::optional<CameraState> camera{};
    std::optional<InputState> input{};
    std::vector<Field> extensions{};
};

struct Checkpoint {
    std::string name{};
    std::uint64_t occurrence{};
    std::uint64_t frame_index{};
    ProducerIdentity identity{};
    StateLane lane{StateLane::unknown};
};

enum class DatagramDirection : std::uint8_t {
    inbound = 0,
    outbound = 1,
};

struct NetworkEndpoint {
    std::string address{};
    std::uint16_t port{};
};

struct NetworkDatagram {
    ProducerIdentity identity{};
    std::uint64_t timestamp_ns{};
    std::uint64_t socket_id{};
    std::uint32_t total_size{};
    bool truncated{};
    DatagramDirection direction{DatagramDirection::inbound};
    NetworkEndpoint source{};
    NetworkEndpoint destination{};
    ByteString payload{};
};

struct DecodedNetworkEvent {
    ProducerIdentity identity{};
    std::uint64_t simulation_tick{};
    DatagramDirection direction{DatagramDirection::inbound};
    std::uint32_t message_id{};
    std::string name{};
    std::vector<Field> fields{};
};

enum class MutationResult : std::uint8_t {
    observed = 0,
    applied = 1,
    rejected = 2,
    // A write call was made, but the final target value could not be proven.
    // The mutation may have been applied and must not be blindly retried.
    unverified = 3,
};

struct MutationAudit {
    ProducerIdentity identity{};
    std::uint64_t simulation_tick{};
    std::uint64_t timestamp_ns{};
    std::string target{};
    std::string operation{};
    MutationResult result{MutationResult::observed};
    ScalarValue before{};
    ScalarValue after{};
    std::string detail{};
};

struct DiagnosticEvent {
    ProducerIdentity identity{};
    std::uint64_t simulation_tick{};
    std::uint64_t timestamp_ns{};
    Severity severity{Severity::info};
    std::string code{};
    std::string message{};
    std::vector<Field> context{};
};

using Event = std::variant<RunMetadata,
                           FrameSnapshot,
                           Checkpoint,
                           NetworkDatagram,
                           DecodedNetworkEvent,
                           MutationAudit,
                           DiagnosticEvent>;

class IEventSink {
public:
    virtual ~IEventSink() = default;
    [[nodiscard]] virtual bool append(const Event& event) = 0;
    [[nodiscard]] virtual const std::string& last_error() const noexcept = 0;
};

struct CaptureRequest {
    StateLane lane{StateLane::unknown};
    std::uint64_t frame_index{};
    std::uint64_t timestamp_ns{};
};

struct CaptureSample {
    std::optional<FrameSnapshot> frame{};
    std::string detail{};
};

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;
    [[nodiscard]] virtual RunMetadata metadata() const = 0;
    [[nodiscard]] virtual CaptureSample capture(
        const CaptureRequest& request) = 0;
};

enum class CaptureRecordStatus : std::uint8_t {
    complete,
    invalid_metadata,
    capture_failed,
    sink_failed,
};

struct CaptureRecordResult {
    CaptureRecordStatus status{CaptureRecordStatus::complete};
    std::size_t samples_recorded{};
    std::size_t failed_request_index{static_cast<std::size_t>(-1)};
    std::string detail{};

    [[nodiscard]] bool complete() const noexcept {
        return status == CaptureRecordStatus::complete;
    }
};

[[nodiscard]] CaptureRecordResult record_capture(
    ICaptureSource& source,
    IEventSink& sink,
    const std::vector<CaptureRequest>& requests);

struct Trace {
    std::uint16_t major_version{kTraceMajorVersion};
    std::uint16_t minor_version{kTraceMinorVersion};
    std::vector<Event> events{};
};

enum class TraceReadStatus : std::uint8_t {
    complete,
    truncated_tail,
    invalid_header,
    unsupported_version,
    checksum_mismatch,
    malformed_chunk,
};

struct TraceReadResult {
    Trace trace{};
    TraceReadStatus status{TraceReadStatus::complete};
    std::size_t error_offset{};
    std::string detail{};

    [[nodiscard]] bool complete() const noexcept {
        return status == TraceReadStatus::complete;
    }
};

enum class ComparisonClassification : std::uint8_t {
    clean,
    warnings,
    mismatch,
    invalid_evidence,
};

struct Difference {
    Severity severity{Severity::error};
    std::string checkpoint{};
    std::string path{};
    std::string expected{};
    std::string actual{};
    double absolute_tolerance{};
};

struct ComparisonOptions {
    double normalized_position_alignment_allowance{};
    double normalized_angle_alignment_allowance_deg{};
    double presentation_fov_allowance_deg{};
    double presentation_scope_fraction_allowance{};
};

struct ComparisonReport {
    ComparisonClassification classification{ComparisonClassification::clean};
    std::vector<Difference> differences{};

    [[nodiscard]] int exit_code() const noexcept;
};

[[nodiscard]] ComparisonReport compare_traces(
    const Trace& reference,
    const Trace& candidate,
    const ComparisonOptions& options = {});

[[nodiscard]] ComparisonReport compare_traces(
    const TraceReadResult& reference,
    const TraceReadResult& candidate,
    const ComparisonOptions& options = {});

class TraceWriter final : public IEventSink {
public:
    TraceWriter();

    [[nodiscard]] bool append(const Event& event) override;
    [[nodiscard]] bool append(const RunMetadata& metadata);
    [[nodiscard]] bool append(const FrameSnapshot& snapshot);
    [[nodiscard]] bool append(const Checkpoint& checkpoint);
    [[nodiscard]] bool append(const NetworkDatagram& datagram);
    [[nodiscard]] bool append(const DecodedNetworkEvent& event);
    [[nodiscard]] bool append(const MutationAudit& audit);
    [[nodiscard]] bool append(const DiagnosticEvent& diagnostic);
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept override;

private:
    std::vector<std::uint8_t> bytes_{};
    std::string last_error_{};
};

[[nodiscard]] TraceReadResult read_trace(const std::uint8_t* data,
                                         std::size_t size);

[[nodiscard]] inline TraceReadResult read_trace(
    const std::vector<std::uint8_t>& bytes) {
    return read_trace(bytes.data(), bytes.size());
}

}  // namespace opennova::parity
