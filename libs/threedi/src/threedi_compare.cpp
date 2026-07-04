#include "threedi/threedi_compare.h"

#include <io/le.h>

#include "threedi/threedi.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

struct ChunkOccurrence {
    const ThreediChunk *chunk;
    std::string path;
};

static const double kVertNormalEpsilon = 1.0e-5;
static const uint32_t kVertFlagTangents = 0x14u;
static const uint32_t kVertFlagSkinned = 0x40u;

static void set_report(char *report, size_t report_size, const char *fmt, ...)
{
    if (!report || report_size == 0) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(report, report_size, fmt, args);
    va_end(args);
    report[report_size - 1] = '\0';
}

static std::string chunk_id_string(const ThreediChunk *chunk)
{
    return chunk ? std::string(chunk->id, chunk->id + 4) : std::string();
}

using opennova::io::read_u32_le;
using opennova::io::read_f32_le;

static bool parse_chunk_ids(const char *chunk_ids_csv,
                            std::vector<std::string> *out_ids,
                            char *report,
                            size_t report_size)
{
    if (!chunk_ids_csv) {
        set_report(report, report_size, "chunk list is required");
        return false;
    }

    const char *token_start = chunk_ids_csv;
    while (*token_start) {
        while (*token_start && (isspace((unsigned char)*token_start) || *token_start == ',')) {
            ++token_start;
        }
        if (!*token_start) {
            break;
        }

        const char *token_end = token_start;
        while (*token_end && *token_end != ',') {
            ++token_end;
        }

        const char *trimmed_end = token_end;
        while (trimmed_end > token_start && isspace((unsigned char)trimmed_end[-1])) {
            --trimmed_end;
        }

        size_t len = (size_t)(trimmed_end - token_start);
        if (len != 4) {
            set_report(report,
                       report_size,
                       "invalid chunk id '%.*s': chunk ids must be four characters",
                       (int)len,
                       token_start);
            return false;
        }
        out_ids->push_back(std::string(token_start, trimmed_end));
        token_start = token_end;
    }

    if (out_ids->empty()) {
        set_report(report, report_size, "chunk list is required");
        return false;
    }
    return true;
}

static bool compare_payload_byte_range(const uint8_t *expected,
                                       const uint8_t *actual,
                                       size_t begin,
                                       size_t end,
                                       const std::string &path,
                                       char *report,
                                       size_t report_size)
{
    for (size_t i = begin; i < end; ++i) {
        if (expected[i] != actual[i]) {
            set_report(report,
                       report_size,
                       "%s byte mismatch at payload offset %zu: expected 0x%02X actual 0x%02X",
                       path.c_str(),
                       i,
                       (unsigned int)expected[i],
                       (unsigned int)actual[i]);
            return false;
        }
    }
    return true;
}

static const char *cfac_field_name(size_t field_offset)
{
    if (field_offset < 2u) {
        return "vert_index[0]";
    }
    if (field_offset < 4u) {
        return "vert_index[1]";
    }
    if (field_offset < 6u) {
        return "vert_index[2]";
    }
    if (field_offset < 8u) {
        return "normal_index";
    }
    if (field_offset < 12u) {
        return "plane_dist_fp16";
    }
    if (field_offset < 16u) {
        return "min_x_fp16";
    }
    if (field_offset < 20u) {
        return "min_y_fp16";
    }
    if (field_offset < 24u) {
        return "min_z_fp16";
    }
    if (field_offset < 28u) {
        return "max_x_fp16";
    }
    if (field_offset < 32u) {
        return "max_y_fp16";
    }
    if (field_offset < 36u) {
        return "max_z_fp16";
    }
    if (field_offset < 40u) {
        return "material_flags";
    }
    if (field_offset == 40u) {
        return "poly_type";
    }
    if (field_offset == 41u) {
        return "pad[0]";
    }
    if (field_offset == 42u) {
        return "pad[1]";
    }
    if (field_offset == 43u) {
        return "pad[2]";
    }
    return "unknown";
}

static bool compare_cfac_record_byte_range(const uint8_t *expected,
                                           const uint8_t *actual,
                                           size_t begin,
                                           size_t end,
                                           uint32_t face_index,
                                           size_t record_offset,
                                           const std::string &path,
                                           char *report,
                                           size_t report_size)
{
    for (size_t i = begin; i < end; ++i) {
        if (expected[i] != actual[i]) {
            size_t field_offset = i - record_offset;
            set_report(report,
                       report_size,
                       "%s CFAC face %u %s byte mismatch at payload offset %zu (record offset %zu): expected 0x%02X actual 0x%02X",
                       path.c_str(),
                       face_index,
                       cfac_field_name(field_offset),
                       i,
                       field_offset,
                       (unsigned int)expected[i],
                       (unsigned int)actual[i]);
            return false;
        }
    }
    return true;
}

static bool compare_vert_payload(const ThreediChunk *expected,
                                 const ThreediChunk *actual,
                                 const std::string &path,
                                 char *report,
                                 size_t report_size)
{
    if (expected->data_len < 12u) {
        set_report(report,
                   report_size,
                   "%s invalid VERT layout: payload length %zu is smaller than header",
                   path.c_str(),
                   expected->data_len);
        return false;
    }
    if (!compare_payload_byte_range(expected->data, actual->data, 0u, 12u, path, report, report_size)) {
        return false;
    }

    uint32_t count = read_u32_le(expected->data);
    uint32_t stride = read_u32_le(expected->data + 4);
    uint32_t flags = read_u32_le(expected->data + 8);
    int is_skinned = (flags & kVertFlagSkinned) != 0;
    int has_tangents = (flags & kVertFlagTangents) != 0;
    uint32_t expected_stride = 40u + (is_skinned ? 16u : 0u) + (has_tangents ? 24u : 0u);
    size_t expected_len = 12u + (size_t)count * (size_t)stride;
    if (stride != expected_stride || expected->data_len != expected_len) {
        set_report(report,
                   report_size,
                   "%s invalid VERT layout: count %u stride %u flags 0x%08X payload length %zu",
                   path.c_str(),
                   count,
                   stride,
                   flags,
                   expected->data_len);
        return false;
    }

    size_t normal_offset = 12u + (is_skinned ? 16u : 0u);
    static const char components[3] = {'x', 'y', 'z'};
    for (uint32_t vertex_index = 0; vertex_index < count; ++vertex_index) {
        size_t vertex_offset = 12u + (size_t)vertex_index * (size_t)stride;
        size_t normal_start = vertex_offset + normal_offset;
        size_t normal_end = normal_start + 12u;
        if (!compare_payload_byte_range(expected->data,
                                        actual->data,
                                        vertex_offset,
                                        normal_start,
                                        path,
                                        report,
                                        report_size)) {
            return false;
        }
        for (size_t component = 0; component < 3u; ++component) {
            size_t offset = normal_start + component * 4u;
            float expected_value = read_f32_le(expected->data + offset);
            float actual_value = read_f32_le(actual->data + offset);
            double delta = fabs((double)expected_value - (double)actual_value);
            if (delta > kVertNormalEpsilon) {
                set_report(report,
                           report_size,
                           "%s VERT normal.%c mismatch at vertex %u: expected %.9g actual %.9g delta %.9g epsilon %.9g",
                           path.c_str(),
                           components[component],
                           vertex_index,
                           (double)expected_value,
                           (double)actual_value,
                           delta,
                           kVertNormalEpsilon);
                return false;
            }
        }
        if (!compare_payload_byte_range(expected->data,
                                        actual->data,
                                        normal_end,
                                        vertex_offset + (size_t)stride,
                                        path,
                                        report,
                                        report_size)) {
            return false;
        }
    }
    return true;
}

static bool compare_cfac_payload(const ThreediChunk *expected,
                                 const ThreediChunk *actual,
                                 const std::string &path,
                                 char *report,
                                 size_t report_size)
{
    if (expected->data_len < 8u) {
        set_report(report,
                   report_size,
                   "%s invalid CFAC layout: payload length %zu is smaller than header",
                   path.c_str(),
                   expected->data_len);
        return false;
    }
    if (!compare_payload_byte_range(expected->data, actual->data, 0u, 8u, path, report, report_size)) {
        return false;
    }

    uint32_t count = read_u32_le(expected->data);
    uint32_t record_size = read_u32_le(expected->data + 4);
    size_t expected_len = 8u + (size_t)count * (size_t)record_size;
    if (record_size != 44u || expected->data_len != expected_len) {
        set_report(report,
                   report_size,
                   "%s invalid CFAC layout: count %u record size %u payload length %zu",
                   path.c_str(),
                   count,
                   record_size,
                   expected->data_len);
        return false;
    }

    for (uint32_t face_index = 0; face_index < count; ++face_index) {
        size_t record_offset = 8u + (size_t)face_index * (size_t)record_size;
        if (!compare_cfac_record_byte_range(expected->data,
                                            actual->data,
                                            record_offset,
                                            record_offset + 41u,
                                            face_index,
                                            record_offset,
                                            path,
                                            report,
                                            report_size)) {
            return false;
        }
    }
    return true;
}

static void collect_chunk_occurrences(const ThreediChunk *chunk,
                                      const std::string &path,
                                      const std::string &id,
                                      std::vector<ChunkOccurrence> *out)
{
    if (!chunk) {
        return;
    }

    if (memcmp(chunk->id, id.c_str(), 4) == 0) {
        ChunkOccurrence occurrence;
        occurrence.chunk = chunk;
        occurrence.path = path;
        out->push_back(occurrence);
    }

    for (size_t i = 0; i < chunk->child_count; ++i) {
        const ThreediChunk *child = &chunk->children[i];
        char index[32];
        snprintf(index, sizeof(index), "[%zu]", i);
        std::string child_path = path + "/" + chunk_id_string(child) + index;
        collect_chunk_occurrences(child, child_path, id, out);
    }
}

static std::vector<ChunkOccurrence> collect_chunk_occurrences(const ThreediFile &file,
                                                             const std::string &id)
{
    std::vector<ChunkOccurrence> out;
    if (!file.root) {
        return out;
    }
    std::string root_path = chunk_id_string(file.root) + "[0]";
    collect_chunk_occurrences(file.root, root_path, id, &out);
    return out;
}

static bool compare_chunk_tree(const ThreediChunk *expected,
                               const ThreediChunk *actual,
                               const std::string &path,
                               char *report,
                               size_t report_size)
{
    if (memcmp(expected->id, actual->id, 4) != 0) {
        set_report(report,
                   report_size,
                   "%s id mismatch: expected %.4s actual %.4s",
                   path.c_str(),
                   expected->id,
                   actual->id);
        return false;
    }
    if (expected->is_parent != actual->is_parent) {
        set_report(report,
                   report_size,
                   "%s kind mismatch: expected %s actual %s",
                   path.c_str(),
                   expected->is_parent ? "parent" : "leaf",
                   actual->is_parent ? "parent" : "leaf");
        return false;
    }
    if (expected->content_len != actual->content_len) {
        set_report(report,
                   report_size,
                   "%s length mismatch: expected %zu actual %zu",
                   path.c_str(),
                   expected->content_len,
                   actual->content_len);
        return false;
    }

    if (expected->is_parent) {
        if (expected->child_count != actual->child_count) {
            set_report(report,
                       report_size,
                       "%s child count mismatch: expected %zu actual %zu",
                       path.c_str(),
                       expected->child_count,
                       actual->child_count);
            return false;
        }
        for (size_t i = 0; i < expected->child_count; ++i) {
            char index[32];
            snprintf(index, sizeof(index), "[%zu]", i);
            const ThreediChunk *expected_child = &expected->children[i];
            const ThreediChunk *actual_child = &actual->children[i];
            std::string child_path = path + "/" + chunk_id_string(expected_child) + index;
            if (!compare_chunk_tree(expected_child, actual_child, child_path, report, report_size)) {
                return false;
            }
        }
        return true;
    }

    if (expected->data_len != actual->data_len) {
        set_report(report,
                   report_size,
                   "%s payload length mismatch: expected %zu actual %zu",
                   path.c_str(),
                   expected->data_len,
                   actual->data_len);
        return false;
    }
    if (expected->data_len > 0) {
        int same = memcmp(expected->data, actual->data, expected->data_len) == 0;
        if (!same) {
            if (memcmp(expected->id, "VERT", 4) == 0) {
                return compare_vert_payload(expected, actual, path, report, report_size);
            }
            if (memcmp(expected->id, "CFAC", 4) == 0) {
                return compare_cfac_payload(expected, actual, path, report, report_size);
            }
            return compare_payload_byte_range(expected->data,
                                              actual->data,
                                              0u,
                                              expected->data_len,
                                              path,
                                              report,
                                              report_size);
        }
    }
    return true;
}

static int compare_selected_chunk(const ThreediFile &expected,
                                  const ThreediFile &actual,
                                  const std::string &id,
                                  char *report,
                                  size_t report_size)
{
    std::vector<ChunkOccurrence> expected_chunks = collect_chunk_occurrences(expected, id);
    std::vector<ChunkOccurrence> actual_chunks = collect_chunk_occurrences(actual, id);

    if (expected_chunks.empty()) {
        set_report(report,
                   report_size,
                   "%.4s not found in expected file",
                   id.c_str());
        return 1;
    }

    if (expected_chunks.size() != actual_chunks.size()) {
        set_report(report,
                   report_size,
                   "%.4s occurrence count mismatch: expected %zu actual %zu",
                   id.c_str(),
                   expected_chunks.size(),
                   actual_chunks.size());
        return 1;
    }

    for (size_t i = 0; i < expected_chunks.size(); ++i) {
        if (expected_chunks[i].path != actual_chunks[i].path) {
            set_report(report,
                       report_size,
                       "%.4s occurrence %zu path mismatch: expected %s actual %s",
                       id.c_str(),
                       i,
                       expected_chunks[i].path.c_str(),
                       actual_chunks[i].path.c_str());
            return 1;
        }
        if (!compare_chunk_tree(expected_chunks[i].chunk,
                                actual_chunks[i].chunk,
                                expected_chunks[i].path,
                                report,
                                report_size)) {
            return 1;
        }
    }

    return 0;
}

} // namespace

int threedi_3di3_compare_file_chunks(const char *expected_path,
                                     const char *actual_path,
                                     const char *chunk_ids_csv,
                                     char *report,
                                     size_t report_size)
{
    if (report && report_size > 0) {
        report[0] = '\0';
    }
    if (!expected_path || !actual_path) {
        set_report(report, report_size, "expected and actual paths are required");
        return -1;
    }

    std::vector<std::string> chunk_ids;
    if (!parse_chunk_ids(chunk_ids_csv, &chunk_ids, report, report_size)) {
        return -1;
    }

    ThreediFile expected = {};
    ThreediFile actual = {};
    int rc = threedi_read_file(expected_path, &expected);
    if (rc != 0) {
        set_report(report, report_size, "failed to read expected 3DI3 file '%s' (rc=%d)", expected_path, rc);
        return -2;
    }
    rc = threedi_read_file(actual_path, &actual);
    if (rc != 0) {
        threedi_free_file(&expected);
        set_report(report, report_size, "failed to read actual 3DI3 file '%s' (rc=%d)", actual_path, rc);
        return -2;
    }

    int result = 0;
    if (expected.version != actual.version) {
        set_report(report,
                   report_size,
                   "3DI3 version mismatch: expected %u actual %u",
                   expected.version,
                   actual.version);
        result = 1;
    } else {
        for (size_t i = 0; i < chunk_ids.size(); ++i) {
            result = compare_selected_chunk(expected, actual, chunk_ids[i], report, report_size);
            if (result != 0) {
                break;
            }
        }
    }

    threedi_free_file(&actual);
    threedi_free_file(&expected);
    return result;
}
