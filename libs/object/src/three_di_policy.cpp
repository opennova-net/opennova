#include "object/three_di_policy.h"

#include "threedi/threedi.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Event {
    std::string path;
    std::string status;
    std::string reason;
};

struct Report {
    std::vector<Event> events;
    bool failed = false;

    void add(std::string path, std::string status, std::string reason) {
        if (status == "fail") {
            failed = true;
        }
        events.push_back(Event{std::move(path), std::move(status), std::move(reason)});
    }
};

static bool starts_with(const std::string &value, const char *prefix) {
    const size_t n = std::strlen(prefix);
    return value.size() >= n && value.compare(0, n, prefix) == 0;
}

static bool ends_with(const std::string &value, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
}

static std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static Object3diPolicyStatus write_report(const char *path, const Report &report) {
    if (!path || !path[0]) {
        return OBJECT_3DI_POLICY_OK;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return OBJECT_3DI_POLICY_IO_FAILED;
    }
    out << "{\n  \"failed\": " << (report.failed ? "true" : "false")
        << ",\n  \"events\": [\n";
    for (size_t i = 0; i < report.events.size(); ++i) {
        const Event &event = report.events[i];
        out << "    {\"path\": \"" << json_escape(event.path)
            << "\", \"status\": \"" << json_escape(event.status)
            << "\", \"reason\": \"" << json_escape(event.reason) << "\"}";
        if (i + 1 < report.events.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    return out ? OBJECT_3DI_POLICY_OK : OBJECT_3DI_POLICY_IO_FAILED;
}

static uint16_t read_u16(const uint8_t *p) {
    uint16_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static int16_t read_i16(const uint8_t *p) {
    int16_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static uint32_t read_u32(const uint8_t *p) {
    uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static int32_t read_i32(const uint8_t *p) {
    int32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static float read_f32(const uint8_t *p) {
    float value = 0.0f;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static const ThreediChunk *find_child(const ThreediChunk *chunk, const char *id) {
    if (!chunk) {
        return nullptr;
    }
    for (size_t i = 0; i < chunk->child_count; ++i) {
        if (std::strcmp(chunk->children[i].id, id) == 0) {
            return &chunk->children[i];
        }
    }
    return nullptr;
}

static bool is_deferred_geometry_path(const std::string &path, uint32_t flags) {
    if ((flags & OBJECT_3DI_COMPARE_RELAX_GEOMETRY) == 0) {
        return false;
    }
    if (starts_with(path, "CDTA/")) {
        return true;
    }
    if (starts_with(path, "RDTA/RLOD[")) {
        const size_t slash = path.rfind('/');
        if (slash == std::string::npos) {
            return false;
        }
        const std::string leaf = path.substr(slash + 1);
        return leaf == "VERT" || leaf == "INDX" || leaf == "STRP";
    }
    return false;
}

static std::string child_path(const std::string &parent, const ThreediChunk &child, size_t index) {
    if (parent == "RDTA" && std::strcmp(child.id, "RLOD") == 0) {
        std::ostringstream oss;
        oss << "RDTA/RLOD[" << index << "]";
        return oss.str();
    }
    return parent.empty() ? std::string(child.id) : parent + "/" + child.id;
}

static std::string diff_at(const uint8_t *left, const uint8_t *right, size_t index) {
    std::ostringstream oss;
    oss << "diff at byte " << index << ": 0x" << std::hex
        << static_cast<int>(left[index]) << " != 0x" << static_cast<int>(right[index]);
    return oss.str();
}

static bool record_header_valid(const uint8_t *left,
                                const uint8_t *right,
                                size_t len,
                                uint32_t expected_record_size,
                                uint32_t *out_count,
                                uint32_t *out_record_size,
                                std::string &reason) {
    if (len < 8) {
        reason = "record chunk too small";
        return false;
    }
    const uint32_t left_count = read_u32(left);
    const uint32_t right_count = read_u32(right);
    const uint32_t left_record_size = read_u32(left + 4);
    const uint32_t right_record_size = read_u32(right + 4);
    if (left_count != right_count) {
        std::ostringstream oss;
        oss << "record count mismatch: " << left_count << " != " << right_count;
        reason = oss.str();
        return false;
    }
    if (left_record_size != right_record_size) {
        std::ostringstream oss;
        oss << "record size mismatch: " << left_record_size << " != " << right_record_size;
        reason = oss.str();
        return false;
    }
    if (left_record_size != expected_record_size) {
        std::ostringstream oss;
        oss << "unexpected record size: " << left_record_size << " != " << expected_record_size;
        reason = oss.str();
        return false;
    }
    const size_t expected_len = 8u + static_cast<size_t>(left_count) * left_record_size;
    if (len != expected_len) {
        std::ostringstream oss;
        oss << "record payload size mismatch: " << len << " expected " << expected_len;
        reason = oss.str();
        return false;
    }
    if (out_count) {
        *out_count = left_count;
    }
    if (out_record_size) {
        *out_record_size = left_record_size;
    }
    return true;
}

static int32_t ordered_float_bits(float value) {
    int32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint32_t ulp_distance(float left, float right) {
    const int32_t a = ordered_float_bits(left);
    const int32_t b = ordered_float_bits(right);
    if ((a ^ b) < 0) {
        return std::numeric_limits<uint32_t>::max();
    }
    const int64_t diff = static_cast<int64_t>(a) - static_cast<int64_t>(b);
    return static_cast<uint32_t>(diff < 0 ? -diff : diff);
}

static bool strict_float_close(const uint8_t *left, const uint8_t *right, size_t offset) {
    const float a = read_f32(left + offset);
    const float b = read_f32(right + offset);
    if (std::isnan(a) && std::isnan(b)) {
        return true;
    }
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return false;
    }
    if (a == b) {
        return true;
    }
    if (std::fabs(a - b) <= 1e-8f) {
        return true;
    }
    return ulp_distance(a, b) <= 4u;
}

static bool oed_float_close(const uint8_t *left, const uint8_t *right, size_t offset) {
    const float a = read_f32(left + offset);
    const float b = read_f32(right + offset);
    if (std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= 1.0e-6f) {
        return true;
    }
    return strict_float_close(left, right, offset);
}

static bool strict_fixed_close(const uint8_t *left, const uint8_t *right, size_t offset) {
    const int32_t a = read_i32(left + offset);
    const int32_t b = read_i32(right + offset);
    const int64_t diff = static_cast<int64_t>(a) - static_cast<int64_t>(b);
    return (diff < 0 ? -diff : diff) <= 1;
}

enum class NumericKind {
    None,
    Float,
    Fixed,
};

static bool offset_in_list(uint32_t offset, const std::vector<uint32_t> &offsets) {
    for (uint32_t value : offsets) {
        if (value == offset) {
            return true;
        }
    }
    return false;
}

static bool compare_numeric_records(const std::string &path,
                                    const uint8_t *left,
                                    const uint8_t *right,
                                    size_t len,
                                    uint32_t record_size,
                                    const std::vector<uint32_t> &float_offsets,
                                    const std::vector<uint32_t> &fixed_offsets,
                                    const char *label,
                                    bool (*float_close)(const uint8_t *, const uint8_t *, size_t),
                                    Report &report) {
    uint32_t tolerated = 0;
    size_t index = 8;
    while (index < len) {
        if (left[index] == right[index]) {
            ++index;
            continue;
        }
        const uint32_t record_index = static_cast<uint32_t>((index - 8) / record_size);
        const uint32_t rel = static_cast<uint32_t>((index - 8) % record_size);
        const uint32_t field_rel = rel & ~3u;
        const size_t field_start = 8u + static_cast<size_t>(record_index) * record_size + field_rel;
        if (field_start + 4 <= len && offset_in_list(field_rel, float_offsets) &&
            float_close(left, right, field_start)) {
            ++tolerated;
            index = field_start + 4;
            continue;
        }
        if (field_start + 4 <= len && offset_in_list(field_rel, fixed_offsets) &&
            strict_fixed_close(left, right, field_start)) {
            ++tolerated;
            index = field_start + 4;
            continue;
        }
        report.add(path, "fail", diff_at(left, right, index));
        return true;
    }
    if (tolerated > 0) {
        std::ostringstream oss;
        oss << "numeric-tolerated " << tolerated << " " << label << " field(s)";
        report.add(path, "numeric-tolerated", oss.str());
    } else {
        report.add(path, "byte-exact", "exact bytes");
    }
    return true;
}

static bool compare_numeric_records(const std::string &path,
                                    const uint8_t *left,
                                    const uint8_t *right,
                                    size_t len,
                                    uint32_t record_size,
                                    const std::vector<uint32_t> &float_offsets,
                                    const std::vector<uint32_t> &fixed_offsets,
                                    const char *label,
                                    Report &report) {
    return compare_numeric_records(path, left, right, len, record_size,
                                   float_offsets, fixed_offsets, label,
                                   strict_float_close, report);
}

static bool compare_usrp(const std::string &path,
                         const uint8_t *left,
                         const uint8_t *right,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left, right, len, 48, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }
    (void)count;
    // OED's ASE parser/marker-centroid path can drift by one fixed tick in
    // userpoint positions, and it normalizes near-unit marker direction rows
    // such as 65535/65536 back to 1.0. Subobject, type, and name are strict.
    return compare_numeric_records(path, left, right, len, record_size,
                                   {}, {0, 4, 8, 12, 16, 20}, "userpoint fixed-point", report);
}

static bool compare_mtrx(const std::string &path,
                         const uint8_t *left,
                         const uint8_t *right,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left, right, len, 64, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }
    uint32_t tolerated = 0;
    for (uint32_t record = 0; record < count; ++record) {
        const size_t base = 8u + static_cast<size_t>(record) * record_size;
        if (std::memcmp(left + base, right + base, record_size) == 0) {
            continue;
        }
        bool fieldwise = true;
        for (int field = 0; field < 16; ++field) {
            const size_t offset = base + static_cast<size_t>(field) * 4u;
            if (std::memcmp(left + offset, right + offset, 4) != 0 &&
                !strict_float_close(left, right, offset)) {
                fieldwise = false;
                break;
            }
        }
        if (fieldwise) {
            ++tolerated;
            continue;
        }
        report.add(path, "fail", diff_at(left, right, base));
        return true;
    }
    if (tolerated > 0) {
        std::ostringstream oss;
        oss << "fp-tolerated " << tolerated << " matrix record(s)";
        report.add(path, "fp-tolerated", oss.str());
    } else {
        report.add(path, "byte-exact", "exact bytes");
    }
    return true;
}

static bool compare_lght(const std::string &path,
                         const uint8_t *left_data,
                         const uint8_t *right_data,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left_data, right_data, len, 116, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }
    std::vector<uint8_t> left(left_data, left_data + len);
    std::vector<uint8_t> right(right_data, right_data + len);
    for (uint32_t record = 0; record < count; ++record) {
        const size_t flags_offset = 8u + static_cast<size_t>(record) * record_size + 33u;
        if (flags_offset < len) {
            // The 3DP/ASE path only carries the light type bit. OED rebuilds
            // the remaining flag bits from project defaults.
            left[flags_offset] &= 0x08u;
            right[flags_offset] &= 0x08u;
        }
    }
    std::vector<uint32_t> floats = {0, 4, 8, 12, 16};
    for (uint32_t i = 0; i < 4; ++i) {
        floats.push_back(36u + i * 4u);
    }
    for (uint32_t i = 0; i < 16; ++i) {
        floats.push_back(52u + i * 4u);
    }
    return compare_numeric_records(path, left.data(), right.data(), len, record_size,
                                   floats, {}, "light", report);
}

static bool compare_oobj(const std::string &path,
                         const uint8_t *left_data,
                         const uint8_t *right_data,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left_data, right_data, len, 36, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }
    std::vector<uint8_t> left(left_data, left_data + len);
    std::vector<uint8_t> right(right_data, right_data + len);
    for (uint32_t record = 0; record < count; ++record) {
        const size_t base = 8u + static_cast<size_t>(record) * record_size;
        if (base + 3 > len) {
            break;
        }
        const uint8_t occ_type = left[base];
        if (occ_type != 2 && occ_type != 3) {
            left[base + 2] = 0;
            right[base + 2] = 0;
        }
    }
    return compare_numeric_records(path, left.data(), right.data(), len, record_size,
                                   {4, 8, 12, 16}, {}, "occlusion object",
                                   oed_float_close, report);
}

static bool compare_ovrt(const std::string &path,
                         const uint8_t *left,
                         const uint8_t *right,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left, right, len, 12, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }
    (void)count;
    return compare_numeric_records(path, left, right, len, record_size,
                                   {0, 4, 8}, {}, "occlusion vertex",
                                   oed_float_close, report);
}

static bool compare_opln(const std::string &path,
                         const uint8_t *left,
                         const uint8_t *right,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left, right, len, 16, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }
    (void)count;
    return compare_numeric_records(path, left, right, len, record_size,
                                   {0, 4, 8, 12}, {}, "occlusion plane",
                                   oed_float_close, report);
}

static bool robj_float_close(const uint8_t *left, const uint8_t *right, size_t offset) {
    const float a = read_f32(left + offset);
    const float b = read_f32(right + offset);
    if (std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= 2.0e-7f) {
        return true;
    }
    return strict_float_close(left, right, offset);
}

static bool compare_robj(const std::string &path,
                         const uint8_t *left,
                         const uint8_t *right,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left, right, len, 52, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }

    uint32_t tolerated = 0;
    for (uint32_t record = 0; record < count; ++record) {
        const size_t base = 8u + static_cast<size_t>(record) * record_size;
        if (std::memcmp(left + base, right + base, record_size) == 0) {
            continue;
        }
        const bool both_no_strips =
            read_i32(left + base + 0u) == 0 &&
            read_i32(left + base + 4u) == 0 &&
            read_i32(right + base + 0u) == 0 &&
            read_i32(right + base + 4u) == 0;
        for (uint32_t field = 0; field < 13; ++field) {
            const uint32_t rel = field * 4u;
            const size_t offset = base + rel;
            if (std::memcmp(left + offset, right + offset, 4) == 0) {
                continue;
            }
            if (rel == 0u || rel == 4u || rel >= 36u) {
                ++tolerated;
                continue;
            }
            if (rel == 8u) {
                if (both_no_strips) {
                    ++tolerated;
                    continue;
                }
                report.add(path, "fail", diff_at(left, right, offset));
                return true;
            }
            if (rel >= 12u && rel <= 20u && both_no_strips) {
                ++tolerated;
                continue;
            }
            if (robj_float_close(left, right, offset)) {
                ++tolerated;
                continue;
            }
            report.add(path, "fail", diff_at(left, right, offset));
            return true;
        }
    }

    if (tolerated > 0) {
        std::ostringstream oss;
        oss << "numeric-tolerated " << tolerated << " render object field(s)";
        report.add(path, "numeric-tolerated", oss.str());
    } else {
        report.add(path, "byte-exact", "exact bytes");
    }
    return true;
}

static bool compare_panm(const std::string &path,
                         const uint8_t *left,
                         const uint8_t *right,
                         size_t len,
                         Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    std::string reason;
    if (!record_header_valid(left, right, len, 68, &count, &record_size, reason)) {
        report.add(path, "fail", reason);
        return true;
    }

    uint32_t tolerated = 0;
    for (uint32_t record = 0; record < count; ++record) {
        const size_t base = 8u + static_cast<size_t>(record) * record_size;
        if (std::memcmp(left + base, right + base, record_size) == 0) {
            continue;
        }
        const bool disabled =
            read_u32(left + base) == 0u &&
            read_u32(right + base) == 0u;
        for (size_t rel = 0; rel < record_size; ++rel) {
            const size_t offset = base + rel;
            if (left[offset] == right[offset]) {
                continue;
            }
            if (disabled && rel == 4u) {
                ++tolerated;
                continue;
            }
            report.add(path, "fail", diff_at(left, right, offset));
            return true;
        }
    }

    if (tolerated > 0) {
        std::ostringstream oss;
        oss << "numeric-tolerated " << tolerated << " disabled PANM parent field(s)";
        report.add(path, "numeric-tolerated", oss.str());
    } else {
        report.add(path, "byte-exact", "exact bytes");
    }
    return true;
}

static bool compare_leaf_with_policy(const std::string &path,
                                     const uint8_t *left,
                                     const uint8_t *right,
                                     size_t len,
                                     Report &report) {
    if (ends_with(path, "USRP")) {
        return compare_usrp(path, left, right, len, report);
    }
    if (ends_with(path, "MTRX")) {
        return compare_mtrx(path, left, right, len, report);
    }
    if (ends_with(path, "LGHT")) {
        return compare_lght(path, left, right, len, report);
    }
    if (ends_with(path, "OOBJ")) {
        return compare_oobj(path, left, right, len, report);
    }
    if (ends_with(path, "OVRT")) {
        return compare_ovrt(path, left, right, len, report);
    }
    if (ends_with(path, "OPLN")) {
        return compare_opln(path, left, right, len, report);
    }
    if (ends_with(path, "ROBJ")) {
        return compare_robj(path, left, right, len, report);
    }
    if (ends_with(path, "PANM")) {
        return compare_panm(path, left, right, len, report);
    }
    return false;
}

static void compare_chunks(const ThreediChunk *expected,
                           const ThreediChunk *actual,
                           const std::string &path,
                           uint32_t flags,
                           Report &report) {
    if (!expected || !actual) {
        report.add(path.empty() ? "/" : path, "fail", "missing chunk");
        return;
    }
    if (std::strcmp(expected->id, actual->id) != 0) {
        report.add(path, "fail", std::string("id mismatch: ") + expected->id + " != " + actual->id);
        return;
    }
    if (expected->is_parent != actual->is_parent) {
        report.add(path, "fail", "parent flag mismatch");
        return;
    }
    if (is_deferred_geometry_path(path, flags)) {
        report.add(path, "deferred", "deferred geometry path");
        return;
    }
    if (!expected->is_parent) {
        if (expected->data_len != actual->data_len) {
            std::ostringstream oss;
            oss << "size mismatch: " << expected->data_len << " != " << actual->data_len;
            report.add(path, "fail", oss.str());
            return;
        }
        if (expected->data_len == 0 ||
            std::memcmp(expected->data, actual->data, expected->data_len) == 0) {
            report.add(path, "byte-exact", "exact bytes");
            return;
        }
        if (compare_leaf_with_policy(path, expected->data, actual->data, expected->data_len, report)) {
            return;
        }
        report.add(path, "fail", "payload differs");
        return;
    }
    if (expected->child_count != actual->child_count) {
        std::ostringstream oss;
        oss << "child count mismatch: " << expected->child_count << " != " << actual->child_count;
        report.add(path.empty() ? "/" : path, "fail", oss.str());
        return;
    }
    for (size_t i = 0; i < expected->child_count; ++i) {
        compare_chunks(&expected->children[i],
                       &actual->children[i],
                       child_path(path, expected->children[i], i),
                       flags,
                       report);
    }
}

static bool validate_record_chunk(const ThreediChunk *chunk,
                                  const char *path,
                                  const std::vector<uint32_t> &valid_sizes,
                                  uint32_t *out_count,
                                  uint32_t *out_record_size,
                                  bool allow_trailing_padding,
                                  Report &report) {
    if (!chunk) {
        report.add(path, "fail", "missing chunk");
        return false;
    }
    if (chunk->is_parent || chunk->data_len < 8) {
        report.add(path, "fail", "record chunk header missing");
        return false;
    }
    const uint32_t count = read_u32(chunk->data);
    const uint32_t record_size = read_u32(chunk->data + 4);
    bool size_ok = false;
    for (uint32_t valid : valid_sizes) {
        if (record_size == valid || (count == 0 && record_size == 0)) {
            size_ok = true;
            break;
        }
    }
    if (!size_ok) {
        std::ostringstream oss;
        oss << "unexpected record size " << record_size;
        report.add(path, "fail", oss.str());
        return false;
    }
    const size_t expected_len = 8u + static_cast<size_t>(count) * record_size;
    if ((!allow_trailing_padding && chunk->data_len != expected_len) ||
        (allow_trailing_padding && chunk->data_len < expected_len)) {
        std::ostringstream oss;
        oss << "record payload size mismatch: " << chunk->data_len;
        oss << (allow_trailing_padding ? " < " : " != ") << expected_len;
        report.add(path, "fail", oss.str());
        return false;
    }
    if (out_count) {
        *out_count = count;
    }
    if (out_record_size) {
        *out_record_size = record_size;
    }
    report.add(path, "pass", "record header valid");
    return true;
}

static bool validate_cmdl(const ThreediChunk *cdta, Report &report) {
    const ThreediChunk *cmdl = find_child(cdta, "CMDL");
    if (!cmdl || cmdl->is_parent || cmdl->data_len != 64) {
        report.add("CDTA/CMDL", "fail", "CMDL must be a 64-byte leaf");
        return false;
    }
    report.add("CDTA/CMDL", "pass", "CMDL size valid");
    return true;
}

static bool validate_cdta(const ThreediChunk *root, Report &report) {
    const ThreediChunk *cdta = find_child(root, "CDTA");
    if (!cdta || !cdta->is_parent) {
        report.add("CDTA", "fail", "missing CDTA parent");
        return false;
    }
    bool ok = validate_cmdl(cdta, report);
    uint32_t cvrt_count = 0;
    uint32_t cnrm_count = 0;
    uint32_t cfac_count = 0;
    ok &= validate_record_chunk(find_child(cdta, "CVRT"), "CDTA/CVRT", {8}, &cvrt_count, nullptr, true, report);
    ok &= validate_record_chunk(find_child(cdta, "CNRM"), "CDTA/CNRM", {8}, &cnrm_count, nullptr, true, report);
    const ThreediChunk *cfac = find_child(cdta, "CFAC");
    ok &= validate_record_chunk(cfac, "CDTA/CFAC", {44}, &cfac_count, nullptr, false, report);
    ok &= validate_record_chunk(find_child(cdta, "BPLN"), "CDTA/BPLN", {12}, nullptr, nullptr, false, report);
    ok &= validate_record_chunk(find_child(cdta, "BVOL"), "CDTA/BVOL", {36}, nullptr, nullptr, false, report);
    ok &= validate_record_chunk(find_child(cdta, "COBJ"), "CDTA/COBJ", {88}, nullptr, nullptr, false, report);
    ok &= validate_record_chunk(find_child(cdta, "CXLT"), "CDTA/CXLT", {12}, nullptr, nullptr, false, report);

    if (cfac && cfac_count > 0 && cfac->data_len >= 8) {
        const uint32_t record_size = read_u32(cfac->data + 4);
        for (uint32_t i = 0; i < cfac_count; ++i) {
            const uint8_t *record = cfac->data + 8u + static_cast<size_t>(i) * record_size;
            for (int corner = 0; corner < 3; ++corner) {
                const int16_t vi = read_i16(record + corner * 2);
                if (vi < 0 || static_cast<uint32_t>(vi) >= cvrt_count) {
                    report.add("CDTA/CFAC", "fail", "face vertex index out of range");
                    return false;
                }
            }
            const int16_t ni = read_i16(record + 6);
            if (ni >= 0 && static_cast<uint32_t>(ni) >= cnrm_count) {
                report.add("CDTA/CFAC", "fail", "face normal index out of range");
                return false;
            }
        }
        report.add("CDTA/CFAC", "pass", "face indices in range");
    }
    return ok && !report.failed;
}

static bool validate_vert_chunk(const ThreediChunk *vert,
                                const std::string &path,
                                uint32_t *out_count,
                                Report &report) {
    uint32_t count = 0;
    if (!vert) {
        report.add(path, "fail", "missing chunk");
        return false;
    }
    if (vert->is_parent || vert->data_len < 12) {
        report.add(path, "fail", "VERT header missing");
        return false;
    }
    count = read_u32(vert->data);
    const uint32_t stride = read_u32(vert->data + 4);
    const uint32_t flags = read_u32(vert->data + 8);
    const bool is_skinned = (flags & 0x40u) != 0;
    const bool has_tangents = (flags & 0x14u) != 0;
    const uint32_t expected_stride = 40u + (is_skinned ? 16u : 0u) + (has_tangents ? 24u : 0u);
    if (stride != expected_stride) {
        std::ostringstream oss;
        oss << "VERT stride mismatch: " << stride << " != " << expected_stride;
        report.add(path, "fail", oss.str());
        return false;
    }
    const size_t expected_len = 12u + static_cast<size_t>(count) * stride;
    if (vert->data_len != expected_len) {
        std::ostringstream oss;
        oss << "VERT payload size mismatch: " << vert->data_len << " != " << expected_len;
        report.add(path, "fail", oss.str());
        return false;
    }
    report.add(path, "pass", "VERT header valid");
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *record = vert->data + 12u + static_cast<size_t>(i) * stride;
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(read_f32(record + axis * 4))) {
                report.add(path, "fail", "non-finite vertex position");
                return false;
            }
        }
    }
    if (out_count) {
        *out_count = count;
    }
    report.add(path, "pass", "vertex positions finite");
    return true;
}

static bool validate_indx_chunk(const ThreediChunk *indx,
                                const std::string &path,
                                uint32_t vertex_count,
                                uint32_t *out_count,
                                Report &report) {
    uint32_t count = 0;
    bool ok = validate_record_chunk(indx, path.c_str(), {2}, &count, nullptr, false, report);
    if (!ok) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t index = read_u16(indx->data + 8u + static_cast<size_t>(i) * 2u);
        if (index >= vertex_count) {
            report.add(path, "fail", "index references missing vertex");
            return false;
        }
    }
    if (out_count) {
        *out_count = count;
    }
    report.add(path, "pass", "indices in range");
    return true;
}

static bool validate_strp_chunk(const ThreediChunk *strp,
                                const std::string &path,
                                uint32_t vertex_count,
                                uint32_t index_count,
                                uint32_t material_count,
                                Report &report) {
    uint32_t count = 0;
    uint32_t record_size = 0;
    bool ok = validate_record_chunk(strp, path.c_str(), {48, 68}, &count, &record_size, false, report);
    if (!ok) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *record = strp->data + 8u + static_cast<size_t>(i) * record_size;
        const int32_t material = read_i32(record);
        const int32_t index_offset = read_i32(record + 4);
        const uint16_t num_indices = read_u16(record + 8);
        const int32_t start_vertex = read_i32(record + 16);
        const int32_t num_vertices = read_i32(record + 20);
        if (material < 0 || static_cast<uint32_t>(material) >= material_count) {
            report.add(path, "fail", "strip material out of range");
            return false;
        }
        if (index_offset < 0 ||
            static_cast<uint32_t>(index_offset) + num_indices > index_count) {
            report.add(path, "fail", "strip index range out of bounds");
            return false;
        }
        if (start_vertex < 0 || num_vertices < 0 ||
            static_cast<uint32_t>(start_vertex) + static_cast<uint32_t>(num_vertices) > vertex_count) {
            report.add(path, "fail", "strip vertex range out of bounds");
            return false;
        }
        if (record_size == 68) {
            const int32_t bone_count = read_i32(record + 64);
            if (bone_count < 0 || bone_count > 16) {
                report.add(path, "fail", "strip bone table length out of bounds");
                return false;
            }
        }
    }
    report.add(path, "pass", "strip ranges valid");
    return true;
}

static bool validate_rdta(const ThreediChunk *root, Report &report) {
    const ThreediChunk *mtrl = find_child(root, "MTRL");
    uint32_t material_count = 1;
    if (mtrl && !mtrl->is_parent && mtrl->data_len >= 8) {
        material_count = read_u32(mtrl->data);
    }

    const ThreediChunk *rdta = find_child(root, "RDTA");
    if (!rdta || !rdta->is_parent) {
        report.add("RDTA", "fail", "missing RDTA parent");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < rdta->child_count; ++i) {
        const ThreediChunk *rlod = &rdta->children[i];
        std::ostringstream prefix;
        prefix << "RDTA/RLOD[" << i << "]";
        if (std::strcmp(rlod->id, "RLOD") != 0 || !rlod->is_parent) {
            report.add(prefix.str(), "fail", "expected RLOD parent");
            return false;
        }
        uint32_t vertex_count = 0;
        uint32_t index_count = 0;
        const std::string base = prefix.str();
        const ThreediChunk *rmdl = find_child(rlod, "RMDL");
        if (!rmdl || rmdl->is_parent || rmdl->data_len != 12) {
            report.add(base + "/RMDL", "fail", "RMDL must be a 12-byte leaf");
            ok = false;
        } else {
            report.add(base + "/RMDL", "pass", "RMDL size valid");
        }
        ok &= validate_vert_chunk(find_child(rlod, "VERT"), base + "/VERT", &vertex_count, report);
        ok &= validate_indx_chunk(find_child(rlod, "INDX"), base + "/INDX", vertex_count, &index_count, report);
        ok &= validate_strp_chunk(find_child(rlod, "STRP"), base + "/STRP",
                                  vertex_count, index_count, material_count, report);
        ok &= validate_record_chunk(find_child(rlod, "ROBJ"), (base + "/ROBJ").c_str(), {52}, nullptr, nullptr, false, report);
        ok &= validate_record_chunk(find_child(rlod, "PANM"), (base + "/PANM").c_str(), {68}, nullptr, nullptr, false, report);
    }
    return ok && !report.failed;
}

} // namespace

extern "C" Object3diPolicyStatus object_3di_compare_files(const char *expected_path,
                                                         const char *actual_path,
                                                         uint32_t flags,
                                                         const char *report_path) {
    if (!expected_path || !actual_path) {
        return OBJECT_3DI_POLICY_INVALID_ARGUMENT;
    }
    ThreediFile expected{};
    ThreediFile actual{};
    if (threedi_read_file(expected_path, &expected) != 0) {
        return OBJECT_3DI_POLICY_READ_FAILED;
    }
    if (threedi_read_file(actual_path, &actual) != 0) {
        threedi_free_file(&expected);
        return OBJECT_3DI_POLICY_READ_FAILED;
    }

    Report report;
    compare_chunks(expected.root, actual.root, "", flags, report);
    const Object3diPolicyStatus write_status = write_report(report_path, report);
    threedi_free_file(&actual);
    threedi_free_file(&expected);
    if (write_status != OBJECT_3DI_POLICY_OK) {
        return write_status;
    }
    return report.failed ? OBJECT_3DI_POLICY_COMPARE_FAILED : OBJECT_3DI_POLICY_OK;
}

extern "C" Object3diPolicyStatus object_3di_validate_geometry_chunks(const char *path,
                                                                    uint32_t flags,
                                                                    const char *report_path) {
    (void)flags;
    if (!path) {
        return OBJECT_3DI_POLICY_INVALID_ARGUMENT;
    }
    ThreediFile file{};
    if (threedi_read_file(path, &file) != 0) {
        return OBJECT_3DI_POLICY_READ_FAILED;
    }
    Report report;
    validate_cdta(file.root, report);
    validate_rdta(file.root, report);
    const Object3diPolicyStatus write_status = write_report(report_path, report);
    threedi_free_file(&file);
    if (write_status != OBJECT_3DI_POLICY_OK) {
        return write_status;
    }
    return report.failed ? OBJECT_3DI_POLICY_VALIDATION_FAILED : OBJECT_3DI_POLICY_OK;
}
