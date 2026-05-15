#include "object/three_di_policy.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void i16(std::vector<uint8_t>& out, int16_t value) {
    u16(out, static_cast<uint16_t>(value));
}

void u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void u32_at(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
    out[offset + 0] = static_cast<uint8_t>(value & 0xffu);
    out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    out[offset + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
    out[offset + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
}

void i32(std::vector<uint8_t>& out, int32_t value) {
    u32(out, static_cast<uint32_t>(value));
}

void i32_at(std::vector<uint8_t>& out, size_t offset, int32_t value) {
    u32_at(out, offset, static_cast<uint32_t>(value));
}

void f32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(out, bits);
}

void f32_at(std::vector<uint8_t>& out, size_t offset, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    out[offset + 0] = static_cast<uint8_t>(bits & 0xffu);
    out[offset + 1] = static_cast<uint8_t>((bits >> 8u) & 0xffu);
    out[offset + 2] = static_cast<uint8_t>((bits >> 16u) & 0xffu);
    out[offset + 3] = static_cast<uint8_t>((bits >> 24u) & 0xffu);
}

std::vector<uint8_t> leaf(const char* id, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.insert(out.end(), id, id + 4);
    u32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<uint8_t> parent(const char* id, const std::vector<std::vector<uint8_t>>& children) {
    std::vector<uint8_t> data;
    for (const auto& child : children) {
        data.insert(data.end(), child.begin(), child.end());
    }
    std::vector<uint8_t> out;
    out.insert(out.end(), id, id + 4);
    u32(out, 0x80000000u | static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<uint8_t> record_header(uint32_t count, uint32_t record_size) {
    std::vector<uint8_t> out;
    u32(out, count);
    u32(out, record_size);
    return out;
}

std::vector<uint8_t> chunk_file(const std::vector<uint8_t>& root) {
    std::vector<uint8_t> out;
    out.insert(out.end(), {'3', 'D', 'I', '3'});
    u32(out, 259);
    out.insert(out.end(), root.begin(), root.end());
    return out;
}

std::string temp_path(const char* name) {
    std::string path = test_paths_temp_dir();
    path.push_back(TEST_PATHS_SEP);
    path += name;
    return path;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::vector<uint8_t> simple_compare_file(bool cdta_diff,
                                         bool rdta_diff,
                                         bool info_diff,
                                         bool missing_cdta_child,
                                         bool cdta_order_swap) {
    std::vector<uint8_t> info = leaf("INFO", {static_cast<uint8_t>(info_diff ? 'b' : 'a')});
    std::vector<uint8_t> cvrt = leaf("CVRT", {static_cast<uint8_t>(cdta_diff ? 2 : 1)});
    std::vector<uint8_t> cmdl = leaf("CMDL", std::vector<uint8_t>(64, 0));
    std::vector<uint8_t> cdta;
    if (missing_cdta_child) {
        cdta = parent("CDTA", {});
    } else if (cdta_order_swap) {
        cdta = parent("CDTA", {cvrt, cmdl});
    } else {
        cdta = parent("CDTA", {cmdl, cvrt});
    }
    std::vector<uint8_t> rmdl = leaf("RMDL", std::vector<uint8_t>(12, 0));
    std::vector<uint8_t> vert = leaf("VERT", {static_cast<uint8_t>(rdta_diff ? 2 : 1)});
    std::vector<uint8_t> rdta = parent("RDTA", {parent("RLOD", {rmdl, vert})});
    return chunk_file(parent("ROOT", {info, cdta, rdta}));
}

std::vector<uint8_t> valid_cdta(int16_t cfac_vertex_2 = 2, bool omit_cvrt = false, uint32_t cvrt_record_size = 8) {
    std::vector<uint8_t> cvrt = record_header(3, cvrt_record_size);
    cvrt.resize(8u + 3u * cvrt_record_size, 0);
    std::vector<uint8_t> cnrm = record_header(1, 8);
    cnrm.resize(16, 0);
    std::vector<uint8_t> cfac = record_header(1, 44);
    i16(cfac, 0);
    i16(cfac, 1);
    i16(cfac, cfac_vertex_2);
    i16(cfac, 0);
    cfac.resize(52, 0);
    std::vector<uint8_t> empty = record_header(0, 0);

    std::vector<std::vector<uint8_t>> children = {
        leaf("CMDL", std::vector<uint8_t>(64, 0)),
    };
    if (!omit_cvrt) {
        children.push_back(leaf("CVRT", cvrt));
    }
    children.push_back(leaf("CNRM", cnrm));
    children.push_back(leaf("CFAC", cfac));
    children.push_back(leaf("BPLN", empty));
    children.push_back(leaf("BVOL", empty));
    children.push_back(leaf("COBJ", empty));
    children.push_back(leaf("CXLT", empty));
    return parent("CDTA", children);
}

std::vector<uint8_t> valid_rdta(uint32_t vert_stride = 40,
                                uint16_t bad_index = 2,
                                int32_t strip_material = 0,
                                bool omit_vert = false) {
    std::vector<uint8_t> vert = record_header(3, vert_stride);
    u32(vert, 0x01);
    for (int i = 0; i < 3; ++i) {
        f32(vert, static_cast<float>(i));
        f32(vert, 0.0f);
        f32(vert, 0.0f);
        vert.resize(12u + static_cast<size_t>(i + 1) * vert_stride, 0);
    }

    std::vector<uint8_t> indx = record_header(3, 2);
    u16(indx, 0);
    u16(indx, 1);
    u16(indx, bad_index);

    std::vector<uint8_t> strp = record_header(1, 48);
    i32(strp, strip_material);
    i32(strp, 0);
    u16(strp, 3);
    u16(strp, 0);
    i32(strp, 0);
    i32(strp, 0);
    i32(strp, 3);
    strp.resize(56, 0);

    std::vector<uint8_t> empty = record_header(0, 0);
    std::vector<std::vector<uint8_t>> rlod_children = {
        leaf("RMDL", std::vector<uint8_t>(12, 0)),
    };
    if (!omit_vert) {
        rlod_children.push_back(leaf("VERT", vert));
    }
    rlod_children.push_back(leaf("INDX", indx));
    rlod_children.push_back(leaf("STRP", strp));
    rlod_children.push_back(leaf("ROBJ", empty));
    rlod_children.push_back(leaf("PANM", empty));
    return parent("RDTA", {parent("RLOD", rlod_children)});
}

std::vector<uint8_t> geometry_file(std::vector<uint8_t> cdta, std::vector<uint8_t> rdta) {
    return chunk_file(parent("ROOT", {std::move(cdta), std::move(rdta)}));
}

std::vector<uint8_t> single_leaf_file(const char* id, const std::vector<uint8_t>& payload) {
    return chunk_file(parent("ROOT", {leaf(id, payload)}));
}

std::vector<uint8_t> panm_payload(uint8_t marker, uint8_t parent_subobject = 0) {
    std::vector<uint8_t> out = record_header(1, 68);
    out.resize(76, 0);
    out[8] = marker;
    out[12] = parent_subobject;
    return out;
}

std::vector<uint8_t> robj_payload() {
    std::vector<uint8_t> out = record_header(1, 52);
    i32(out, 1);       // num_strips: stripification-coupled
    i32(out, 0);       // num_alpha_strips: stripification-coupled
    i32(out, -1);      // parent_index
    f32(out, 0.25f);   // rel.x
    f32(out, 0.0f);    // rel.y
    f32(out, -0.5f);   // rel.z
    f32(out, 0.25f);   // abs.x
    f32(out, 0.0f);    // abs.y
    f32(out, -0.5f);   // abs.z
    f32(out, 1.0f);    // bounding_center.x
    f32(out, 2.0f);    // bounding_center.y
    f32(out, 3.0f);    // bounding_center.z
    f32(out, 4.0f);    // bounding_radius
    return out;
}

std::vector<uint8_t> rdta_policy_file(const std::vector<uint8_t>& rmdl,
                                      const std::vector<uint8_t>& panm,
                                      const std::vector<uint8_t>& robj) {
    std::vector<uint8_t> vert = leaf("VERT", {1, 2, 3});
    std::vector<uint8_t> indx = leaf("INDX", {4, 5, 6});
    std::vector<uint8_t> strp = leaf("STRP", {7, 8, 9});
    std::vector<uint8_t> rdta = parent("RDTA", {
        parent("RLOD", {
            leaf("RMDL", rmdl),
            vert,
            indx,
            strp,
            leaf("ROBJ", robj),
            leaf("PANM", panm),
        }),
    });
    return chunk_file(parent("ROOT", {rdta}));
}

std::vector<uint8_t> mtrx_payload(float first_axis_x) {
    std::vector<uint8_t> out = record_header(1, 64);
    f32(out, first_axis_x);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 1.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 1.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 0.0f);
    f32(out, 1.0f);
    return out;
}

std::vector<uint8_t> oobj_payload(float x) {
    std::vector<uint8_t> out = record_header(1, 36);
    out.resize(44, 0);
    out[8] = 2;
    f32_at(out, 12, x);
    return out;
}

std::vector<uint8_t> usrp_payload() {
    std::vector<uint8_t> out = record_header(1, 48);
    i32(out, 100);      // x position: OED parser/centroid can drift by one tick
    i32(out, -200);     // y position: OED parser/centroid can drift by one tick
    i32(out, 300);      // z position: OED parser/centroid can drift by one tick
    i32(out, 65536);    // rot_x remains strict 16.16
    i32(out, 0);        // rot_y remains strict 16.16
    i32(out, 0);        // rot_z remains strict 16.16
    i32(out, 4);        // subobject_index
    i32(out, 'S');      // userpoint_type
    out.resize(56, 0);
    out[40] = 'L';
    out[41] = 'o';
    out[42] = 'o';
    out[43] = 'k';
    return out;
}

} // namespace

int main() {
    const char* repo_root = test_paths_repo_root(__FILE__);
    (void)repo_root;

    const std::string expected = temp_path("three_di_policy_expected.3di");
    const std::string actual = temp_path("three_di_policy_actual.3di");
    const std::string report = temp_path("three_di_policy_report.json");

    TEST_EXPECT(write_file(expected, simple_compare_file(false, false, false, false, false)));
    TEST_EXPECT(write_file(actual, simple_compare_file(false, false, false, false, false)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    TEST_EXPECT(write_file(actual, simple_compare_file(true, false, false, false, false)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    TEST_EXPECT(write_file(actual, simple_compare_file(false, true, false, false, false)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    TEST_EXPECT(write_file(actual, simple_compare_file(false, false, true, false, false)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    TEST_EXPECT(write_file(actual, simple_compare_file(false, false, false, true, false)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    TEST_EXPECT(write_file(actual, simple_compare_file(false, false, false, false, true)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    const std::vector<uint8_t> base_rmdl(12, 0);
    const std::vector<uint8_t> changed_rmdl(12, 1);
    const std::vector<uint8_t> base_panm = panm_payload(0);
    const std::vector<uint8_t> changed_panm = panm_payload(1);
    const std::vector<uint8_t> base_robj = robj_payload();

    TEST_EXPECT(write_file(expected, rdta_policy_file(base_rmdl, base_panm, base_robj)));
    TEST_EXPECT(write_file(actual, rdta_policy_file(changed_rmdl, base_panm, base_robj)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    TEST_EXPECT(write_file(actual, rdta_policy_file(base_rmdl, changed_panm, base_robj)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    TEST_EXPECT(write_file(actual, rdta_policy_file(base_rmdl, panm_payload(0, 7), base_robj)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    std::vector<uint8_t> strip_count_robj = base_robj;
    i32_at(strip_count_robj, 8, 3);
    i32_at(strip_count_robj, 12, 2);
    TEST_EXPECT(write_file(actual, rdta_policy_file(base_rmdl, base_panm, strip_count_robj)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    std::vector<uint8_t> tiny_drift_robj = base_robj;
    f32_at(tiny_drift_robj, 8 + 12, 0.25000018f);
    TEST_EXPECT(write_file(actual, rdta_policy_file(base_rmdl, base_panm, tiny_drift_robj)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    std::vector<uint8_t> parent_drift_robj = base_robj;
    i32_at(parent_drift_robj, 8 + 8, 0);
    TEST_EXPECT(write_file(actual, rdta_policy_file(base_rmdl, base_panm, parent_drift_robj)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    std::vector<uint8_t> large_drift_robj = base_robj;
    f32_at(large_drift_robj, 8 + 12, 0.25001f);
    TEST_EXPECT(write_file(actual, rdta_policy_file(base_rmdl, base_panm, large_drift_robj)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(),
                                         OBJECT_3DI_COMPARE_RELAX_GEOMETRY, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    TEST_EXPECT(write_file(expected, single_leaf_file("MTRX", mtrx_payload(1.0f))));
    TEST_EXPECT(write_file(actual, single_leaf_file("MTRX", mtrx_payload(0.9995f))));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    TEST_EXPECT(write_file(expected, single_leaf_file("OOBJ", oobj_payload(1.0f))));
    TEST_EXPECT(write_file(actual, single_leaf_file("OOBJ", oobj_payload(1.000001f))));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    const std::vector<uint8_t> base_usrp = usrp_payload();
    std::vector<uint8_t> position_tick_usrp = base_usrp;
    i32_at(position_tick_usrp, 8, 101);
    TEST_EXPECT(write_file(expected, single_leaf_file("USRP", base_usrp)));
    TEST_EXPECT(write_file(actual, single_leaf_file("USRP", position_tick_usrp)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    std::vector<uint8_t> rotation_tick_usrp = base_usrp;
    i32_at(rotation_tick_usrp, 8 + 12, 65535);
    TEST_EXPECT(write_file(actual, single_leaf_file("USRP", rotation_tick_usrp)));
    TEST_EXPECT(object_3di_compare_files(expected.c_str(), actual.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_COMPARE_FAILED);

    const std::string geometry = temp_path("three_di_policy_geometry.3di");
    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(), valid_rdta())));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_OK);

    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(99), valid_rdta())));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_VALIDATION_FAILED);

    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(2, true), valid_rdta())));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_VALIDATION_FAILED);

    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(2, false, 12), valid_rdta())));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_VALIDATION_FAILED);

    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(), valid_rdta(44))));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_VALIDATION_FAILED);

    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(), valid_rdta(40, 99))));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_VALIDATION_FAILED);

    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(), valid_rdta(40, 2, 1))));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_VALIDATION_FAILED);

    TEST_EXPECT(write_file(geometry, geometry_file(valid_cdta(), valid_rdta(40, 2, 0, true))));
    TEST_EXPECT(object_3di_validate_geometry_chunks(geometry.c_str(), 0, report.c_str()) ==
                OBJECT_3DI_POLICY_VALIDATION_FAILED);

    return 0;
}
