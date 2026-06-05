#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <string>
#include <vector>

#include "common/test_paths.h"
#include "threedi/threedi.h"
#include "threedi/threedi_compare.h"

static int expect_true(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

static std::string fixture_path(const char *name)
{
    char path[4096];
    snprintf(path,
             sizeof(path),
             "%s/fixtures/threedi/3di3/%s",
             test_paths_repo_root(__FILE__),
             name);
    return std::string(path);
}

static std::string temp_path(const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", test_paths_temp_dir(), name);
    return std::string(path);
}

static const ThreediChunk *find_first_chunk(const ThreediChunk *chunk, const char id[4])
{
    if (!chunk) {
        return NULL;
    }
    if (memcmp(chunk->id, id, 4) == 0) {
        return chunk;
    }
    for (size_t i = 0; i < chunk->child_count; ++i) {
        const ThreediChunk *found = find_first_chunk(&chunk->children[i], id);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int first_chunk_payload_offset(const std::string &src, const char id[4], size_t *out_offset)
{
    ThreediFile file = {};
    if (threedi_read_file(src.c_str(), &file) != 0) {
        fprintf(stderr, "failed to read %s\n", src.c_str());
        return 0;
    }

    const ThreediChunk *chunk = find_first_chunk(file.root, id);
    if (!chunk) {
        fprintf(stderr, "%.4s not found in %s\n", id, src.c_str());
        threedi_free_file(&file);
        return 0;
    }
    if (chunk->is_parent || chunk->data_len == 0) {
        fprintf(stderr, "%.4s is not a nonempty leaf chunk in %s\n", id, src.c_str());
        threedi_free_file(&file);
        return 0;
    }

    *out_offset = chunk->offset + 8u;
    threedi_free_file(&file);
    return 1;
}

static int first_vert_normal_offset(const std::string &src, size_t vertex_index, size_t component, size_t *out_offset)
{
    ThreediFile file = {};
    if (threedi_read_file(src.c_str(), &file) != 0) {
        fprintf(stderr, "failed to read %s\n", src.c_str());
        return 0;
    }

    const ThreediChunk *chunk = find_first_chunk(file.root, "VERT");
    if (!chunk) {
        fprintf(stderr, "VERT not found in %s\n", src.c_str());
        threedi_free_file(&file);
        return 0;
    }
    if (chunk->is_parent || chunk->data_len < 12 || component > 2) {
        fprintf(stderr, "VERT is not a valid leaf chunk in %s\n", src.c_str());
        threedi_free_file(&file);
        return 0;
    }

    uint32_t count = read_u32_le(chunk->data);
    uint32_t stride = read_u32_le(chunk->data + 4);
    uint32_t flags = read_u32_le(chunk->data + 8);
    int is_skinned = (flags & 0x40u) != 0;
    int has_tangents = (flags & 0x14u) != 0;
    uint32_t expected_stride = 40u + (is_skinned ? 16u : 0u) + (has_tangents ? 24u : 0u);
    size_t expected_len = 12u + (size_t)count * (size_t)stride;
    if (stride != expected_stride || chunk->data_len != expected_len || vertex_index >= count) {
        fprintf(stderr, "VERT layout is not valid in %s\n", src.c_str());
        threedi_free_file(&file);
        return 0;
    }

    size_t normal_offset = 12u + vertex_index * (size_t)stride + 12u + (is_skinned ? 16u : 0u);
    *out_offset = chunk->offset + 8u + normal_offset + component * 4u;
    threedi_free_file(&file);
    return 1;
}

static int first_vert_position_offset(const std::string &src, size_t vertex_index, size_t *out_offset)
{
    ThreediFile file = {};
    if (threedi_read_file(src.c_str(), &file) != 0) {
        fprintf(stderr, "failed to read %s\n", src.c_str());
        return 0;
    }

    const ThreediChunk *chunk = find_first_chunk(file.root, "VERT");
    if (!chunk) {
        fprintf(stderr, "VERT not found in %s\n", src.c_str());
        threedi_free_file(&file);
        return 0;
    }
    if (chunk->is_parent || chunk->data_len < 12) {
        fprintf(stderr, "VERT is not a valid leaf chunk in %s\n", src.c_str());
        threedi_free_file(&file);
        return 0;
    }

    uint32_t count = read_u32_le(chunk->data);
    uint32_t stride = read_u32_le(chunk->data + 4);
    if (vertex_index >= count || chunk->data_len < 12u + (size_t)(vertex_index + 1u) * (size_t)stride) {
        fprintf(stderr, "VERT vertex index is not valid in %s\n", src.c_str());
        threedi_free_file(&file);
        return 0;
    }

    *out_offset = chunk->offset + 8u + 12u + vertex_index * (size_t)stride;
    threedi_free_file(&file);
    return 1;
}

static int first_chunk_header_offset(const std::string &src, const char id[4], size_t *out_offset)
{
    ThreediFile file = {};
    if (threedi_read_file(src.c_str(), &file) != 0) {
        fprintf(stderr, "failed to read %s\n", src.c_str());
        return 0;
    }

    const ThreediChunk *chunk = find_first_chunk(file.root, id);
    if (!chunk) {
        fprintf(stderr, "%.4s not found in %s\n", id, src.c_str());
        threedi_free_file(&file);
        return 0;
    }

    *out_offset = chunk->offset;
    threedi_free_file(&file);
    return 1;
}

static int write_modified_chunk_copy(const std::string &src, const std::string &dst, const char id[4])
{
    std::ifstream in(src.c_str(), std::ios::binary);
    if (!in) {
        fprintf(stderr, "failed to open %s\n", src.c_str());
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    size_t payload_offset = 0;
    if (!first_chunk_payload_offset(src, id, &payload_offset) || payload_offset >= bytes.size()) {
        return 0;
    }
    bytes[payload_offset] ^= 0x01u;

    std::ofstream out(dst.c_str(), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good() ? 1 : 0;
}

static int write_modified_float_copy(const std::string &src, const std::string &dst, size_t byte_offset, float delta)
{
    std::ifstream in(src.c_str(), std::ios::binary);
    if (!in) {
        fprintf(stderr, "failed to open %s\n", src.c_str());
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (byte_offset + sizeof(float) > bytes.size()) {
        return 0;
    }

    float value = 0.0f;
    memcpy(&value, &bytes[byte_offset], sizeof(value));
    value += delta;
    memcpy(&bytes[byte_offset], &value, sizeof(value));

    std::ofstream out(dst.c_str(), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good() ? 1 : 0;
}

static int write_modified_byte_copy(const std::string &src, const std::string &dst, size_t byte_offset)
{
    std::ifstream in(src.c_str(), std::ios::binary);
    if (!in) {
        fprintf(stderr, "failed to open %s\n", src.c_str());
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (byte_offset >= bytes.size()) {
        return 0;
    }
    bytes[byte_offset] ^= 0x01u;

    std::ofstream out(dst.c_str(), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good() ? 1 : 0;
}

static int write_parent_flag_toggled_chunk_copy(const std::string &src, const std::string &dst, const char id[4])
{
    std::ifstream in(src.c_str(), std::ios::binary);
    if (!in) {
        fprintf(stderr, "failed to open %s\n", src.c_str());
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    size_t header_offset = 0;
    if (!first_chunk_header_offset(src, id, &header_offset) || header_offset + 7 >= bytes.size()) {
        return 0;
    }
    bytes[header_offset + 7] ^= 0x80u;

    std::ofstream out(dst.c_str(), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good() ? 1 : 0;
}

static int compare_self_succeeds_for_explicit_ghdr(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR");
}

static int compare_self_succeeds_for_explicit_ghdr_and_usrp(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR and USRP");
}

static int compare_self_succeeds_for_explicit_ghdr_usrp_and_info(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR, USRP, and INFO");
}

static int compare_self_succeeds_for_explicit_ghdr_usrp_info_and_ctrl(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO,CTRL",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR, USRP, INFO, and CTRL");
}

static int compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_and_mtrl(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO,CTRL,MTRL",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR, USRP, INFO, CTRL, and MTRL");
}

static int compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_mtrl_and_occl(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO,CTRL,MTRL,OCCL",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR, USRP, INFO, CTRL, MTRL, and OCCL");
}

static int compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_mtrl_occl_and_lght(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO,CTRL,MTRL,OCCL,LGHT",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR, USRP, INFO, CTRL, MTRL, OCCL, and LGHT");
}

static int compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_mtrl_occl_lght_and_mtrx(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO,CTRL,MTRL,OCCL,LGHT,MTRX",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit GHDR, USRP, INFO, CTRL, MTRL, OCCL, LGHT, and MTRX");
}

static int compare_self_succeeds_for_explicit_top_level_chunks_through_rdta(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO,CTRL,MTRL,OCCL,LGHT,MTRX,RDTA",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit top-level chunks through RDTA");
}

static int compare_self_succeeds_for_explicit_top_level_chunks_through_cdta(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "GHDR,USRP,INFO,CTRL,MTRL,OCCL,LGHT,MTRX,RDTA,CDTA",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 0, "self compare should match explicit top-level chunks through CDTA");
}

static int empty_chunk_list_is_invalid(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "",
                                              report,
                                              sizeof(report));
    return expect_true(rc < 0, "empty chunk list should be invalid")
        && expect_true(strstr(report, "chunk") != NULL,
                       "invalid chunk list report should mention chunk");
}

static int null_chunk_list_is_invalid(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              NULL,
                                              report,
                                              sizeof(report));
    return expect_true(rc < 0, "NULL chunk list should be invalid")
        && expect_true(strstr(report, "chunk") != NULL,
                       "NULL chunk list report should mention chunk");
}

static int requested_chunk_must_exist_in_expected(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string path = fixture_path("Shed.3di");
    int rc = threedi_3di3_compare_file_chunks(path.c_str(),
                                              path.c_str(),
                                              "ZZZZ",
                                              report,
                                              sizeof(report));
    return expect_true(rc == 1, "missing requested chunk should mismatch")
        && expect_true(strstr(report, "ZZZZ") != NULL,
                       "missing requested chunk report should name chunk")
        && expect_true(strstr(report, "not found") != NULL,
                       "missing requested chunk report should say not found");
}

static int changed_ghdr_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_ghdr.3di");
    if (!write_modified_chunk_copy(expected, actual, "GHDR")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "GHDR",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed GHDR should mismatch")
        && expect_true(strstr(report, "GHDR") != NULL,
                       "GHDR mismatch report should name the chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "GHDR mismatch report should include byte detail");
}

static int changed_usrp_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_usrp.3di");
    if (!write_modified_chunk_copy(expected, actual, "USRP")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "USRP",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed USRP should mismatch")
        && expect_true(strstr(report, "USRP") != NULL,
                       "USRP mismatch report should name the chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "USRP mismatch report should include byte detail");
}

static int changed_info_kind_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_info.3di");
    if (!write_parent_flag_toggled_chunk_copy(expected, actual, "INFO")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "INFO",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed INFO kind should mismatch")
        && expect_true(strstr(report, "INFO") != NULL,
                       "INFO mismatch report should name the chunk")
        && expect_true(strstr(report, "kind") != NULL,
                       "INFO mismatch report should include kind detail");
}

static int changed_ctrl_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_ctrl.3di");
    if (!write_modified_chunk_copy(expected, actual, "CTRL")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "CTRL",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed CTRL should mismatch")
        && expect_true(strstr(report, "CTRL") != NULL,
                       "CTRL mismatch report should name the chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "CTRL mismatch report should include byte detail");
}

static int changed_mtrl_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_mtrl.3di");
    if (!write_modified_chunk_copy(expected, actual, "MTRL")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "MTRL",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed MTRL should mismatch")
        && expect_true(strstr(report, "MTRL") != NULL,
                       "MTRL mismatch report should name the chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "MTRL mismatch report should include byte detail");
}

static int changed_occl_child_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_occl_child.3di");
    if (!write_modified_chunk_copy(expected, actual, "OVRT")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "OCCL",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed OCCL child should mismatch")
        && expect_true(strstr(report, "OCCL") != NULL,
                       "OCCL mismatch report should name the parent chunk")
        && expect_true(strstr(report, "OVRT") != NULL,
                       "OCCL mismatch report should name the child chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "OCCL child mismatch report should include byte detail");
}

static int changed_lght_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_lght.3di");
    if (!write_modified_chunk_copy(expected, actual, "LGHT")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "LGHT",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed LGHT should mismatch")
        && expect_true(strstr(report, "LGHT") != NULL,
                       "LGHT mismatch report should name the chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "LGHT mismatch report should include byte detail");
}

static int changed_mtrx_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_mtrx.3di");
    if (!write_modified_chunk_copy(expected, actual, "MTRX")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "MTRX",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed MTRX should mismatch")
        && expect_true(strstr(report, "MTRX") != NULL,
                       "MTRX mismatch report should name the chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "MTRX mismatch report should include byte detail");
}

static int changed_rdta_child_payload_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_rdta_child.3di");
    if (!write_modified_chunk_copy(expected, actual, "RMDL")) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "RDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed RDTA child should mismatch")
        && expect_true(strstr(report, "RDTA") != NULL,
                       "RDTA mismatch report should name the parent chunk")
        && expect_true(strstr(report, "RMDL") != NULL,
                       "RDTA mismatch report should name the child chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "RDTA child mismatch report should include byte detail");
}

static int changed_cdta_child_payload_reports_mismatch(const char child_id[4])
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    char name[128];
    snprintf(name, sizeof(name), "opennova_compare_changed_cdta_%.4s.3di", child_id);
    const std::string actual = temp_path(name);
    if (!write_modified_chunk_copy(expected, actual, child_id)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "CDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    char child_message[96];
    snprintf(child_message, sizeof(child_message), "CDTA mismatch report should name child %.4s", child_id);
    const std::string child_text(child_id, child_id + 4);
    return expect_true(rc == 1, "changed CDTA child should mismatch")
        && expect_true(strstr(report, "CDTA") != NULL,
                       "CDTA mismatch report should name the parent chunk")
        && expect_true(strstr(report, child_text.c_str()) != NULL,
                       child_message)
        && expect_true(strstr(report, "byte") != NULL,
                       "CDTA child mismatch report should include byte detail");
}

static int changed_cdta_cfac_padding_matches(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_cdta_cfac_padding.3di");
    size_t cfac_payload_offset = 0;
    if (!first_chunk_payload_offset(expected, "CFAC", &cfac_payload_offset) ||
        !write_modified_byte_copy(expected, actual, cfac_payload_offset + 8u + 42u)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "CDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 0, "changed CFAC padding byte should match CDTA");
}

static int changed_cdta_cfac_second_record_padding_matches(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_cdta_cfac_second_record_padding.3di");
    size_t cfac_payload_offset = 0;
    if (!first_chunk_payload_offset(expected, "CFAC", &cfac_payload_offset) ||
        !write_modified_byte_copy(expected, actual, cfac_payload_offset + 8u + 44u + 41u)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "CDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 0, "changed second CFAC record padding byte should match CDTA");
}

static int changed_cdta_cfac_plane_dist_reports_field(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_cdta_cfac_plane_dist.3di");
    size_t cfac_payload_offset = 0;
    if (!first_chunk_payload_offset(expected, "CFAC", &cfac_payload_offset) ||
        !write_modified_byte_copy(expected, actual, cfac_payload_offset + 8u + 8u)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "CDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed CFAC plane_dist_fp16 should mismatch CDTA")
        && expect_true(strstr(report, "CFAC") != NULL,
                       "CFAC plane_dist_fp16 mismatch report should name the chunk")
        && expect_true(strstr(report, "face 0") != NULL,
                       "CFAC plane_dist_fp16 mismatch report should name the face")
        && expect_true(strstr(report, "plane_dist_fp16") != NULL,
                       "CFAC plane_dist_fp16 mismatch report should name the field")
        && expect_true(strstr(report, "payload offset 16") != NULL,
                       "CFAC plane_dist_fp16 mismatch report should include payload offset");
}

static int changed_cdta_cfac_poly_type_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_cdta_cfac_poly_type.3di");
    size_t cfac_payload_offset = 0;
    if (!first_chunk_payload_offset(expected, "CFAC", &cfac_payload_offset) ||
        !write_modified_byte_copy(expected, actual, cfac_payload_offset + 8u + 40u)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "CDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed CFAC poly_type should mismatch CDTA")
        && expect_true(strstr(report, "CFAC") != NULL,
                       "CFAC poly_type mismatch report should name the chunk")
        && expect_true(strstr(report, "face 0") != NULL,
                       "CFAC poly_type mismatch report should name the face")
        && expect_true(strstr(report, "poly_type") != NULL,
                       "CFAC poly_type mismatch report should name the field")
        && expect_true(strstr(report, "payload offset 48") != NULL,
                       "CFAC poly_type mismatch report should include payload offset");
}

static int changed_rdta_vert_normal_within_tolerance_matches(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_rdta_vert_normal_within_tolerance.3di");
    size_t normal_offset = 0;
    if (!first_vert_normal_offset(expected, 0, 2, &normal_offset) ||
        !write_modified_float_copy(expected, actual, normal_offset, 0.000005f)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "RDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 0, "changed VERT normal within epsilon should match RDTA");
}

static int changed_rdta_vert_normal_beyond_tolerance_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_rdta_vert_normal_beyond_tolerance.3di");
    size_t normal_offset = 0;
    if (!first_vert_normal_offset(expected, 0, 2, &normal_offset) ||
        !write_modified_float_copy(expected, actual, normal_offset, 0.00005f)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "RDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed VERT normal beyond epsilon should mismatch RDTA")
        && expect_true(strstr(report, "VERT") != NULL,
                       "VERT normal mismatch report should name the chunk")
        && expect_true(strstr(report, "normal") != NULL,
                       "VERT normal mismatch report should name the field")
        && expect_true(strstr(report, "epsilon") != NULL,
                       "VERT normal mismatch report should include epsilon");
}

static int changed_rdta_vert_non_normal_byte_reports_mismatch(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string actual = temp_path("opennova_compare_changed_rdta_vert_position.3di");
    size_t position_offset = 0;
    if (!first_vert_position_offset(expected, 0, &position_offset) ||
        !write_modified_byte_copy(expected, actual, position_offset)) {
        return 0;
    }

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              actual.c_str(),
                                              "RDTA",
                                              report,
                                              sizeof(report));
    remove(actual.c_str());
    return expect_true(rc == 1, "changed non-normal VERT byte should mismatch RDTA")
        && expect_true(strstr(report, "VERT") != NULL,
                       "VERT byte mismatch report should name the chunk")
        && expect_true(strstr(report, "byte") != NULL,
                       "VERT byte mismatch report should include byte detail");
}

static int missing_file_returns_read_error(void)
{
    char report[512];
    memset(report, 0, sizeof(report));
    const std::string expected = fixture_path("Shed.3di");
    const std::string missing = temp_path("opennova_compare_missing.3di");
    remove(missing.c_str());

    int rc = threedi_3di3_compare_file_chunks(expected.c_str(),
                                              missing.c_str(),
                                              "GHDR",
                                              report,
                                              sizeof(report));
    return expect_true(rc < 0, "missing actual file should return an error")
        && expect_true(strstr(report, "read") != NULL,
                       "missing file report should mention read failure");
}

int main(void)
{
    int ok = 1;
    ok &= compare_self_succeeds_for_explicit_ghdr();
    ok &= compare_self_succeeds_for_explicit_ghdr_and_usrp();
    ok &= compare_self_succeeds_for_explicit_ghdr_usrp_and_info();
    ok &= compare_self_succeeds_for_explicit_ghdr_usrp_info_and_ctrl();
    ok &= compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_and_mtrl();
    ok &= compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_mtrl_and_occl();
    ok &= compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_mtrl_occl_and_lght();
    ok &= compare_self_succeeds_for_explicit_ghdr_usrp_info_ctrl_mtrl_occl_lght_and_mtrx();
    ok &= compare_self_succeeds_for_explicit_top_level_chunks_through_rdta();
    ok &= compare_self_succeeds_for_explicit_top_level_chunks_through_cdta();
    ok &= empty_chunk_list_is_invalid();
    ok &= null_chunk_list_is_invalid();
    ok &= requested_chunk_must_exist_in_expected();
    ok &= changed_ghdr_payload_reports_mismatch();
    ok &= changed_usrp_payload_reports_mismatch();
    ok &= changed_info_kind_reports_mismatch();
    ok &= changed_ctrl_payload_reports_mismatch();
    ok &= changed_mtrl_payload_reports_mismatch();
    ok &= changed_occl_child_payload_reports_mismatch();
    ok &= changed_lght_payload_reports_mismatch();
    ok &= changed_mtrx_payload_reports_mismatch();
    ok &= changed_rdta_child_payload_reports_mismatch();
    ok &= changed_cdta_child_payload_reports_mismatch("CMDL");
    ok &= changed_cdta_child_payload_reports_mismatch("CVRT");
    ok &= changed_cdta_child_payload_reports_mismatch("CNRM");
    ok &= changed_cdta_child_payload_reports_mismatch("CFAC");
    ok &= changed_cdta_child_payload_reports_mismatch("BPLN");
    ok &= changed_cdta_child_payload_reports_mismatch("BVOL");
    ok &= changed_cdta_child_payload_reports_mismatch("COBJ");
    ok &= changed_cdta_child_payload_reports_mismatch("CXLT");
    ok &= changed_cdta_cfac_padding_matches();
    ok &= changed_cdta_cfac_second_record_padding_matches();
    ok &= changed_cdta_cfac_plane_dist_reports_field();
    ok &= changed_cdta_cfac_poly_type_reports_mismatch();
    ok &= changed_rdta_vert_normal_within_tolerance_matches();
    ok &= changed_rdta_vert_normal_beyond_tolerance_reports_mismatch();
    ok &= changed_rdta_vert_non_normal_byte_reports_mismatch();
    ok &= missing_file_returns_read_error();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
