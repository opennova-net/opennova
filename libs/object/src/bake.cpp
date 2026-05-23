// OED 3DI export adapter.
// Bridges the C API (TdpProject) to the internal C++ export pipeline.

#include "object/bake.h"
#include "object/convert_internal.h"
#include "object/export_3di.h"
#include "object/project_types.h"

#include "ase/ase.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

template <size_t N>
static std::string fixed_string(const char (&value)[N]) {
    size_t len = 0;
    while (len < N && value[len] != '\0') {
        ++len;
    }
    return std::string(value, len);
}

static std::filesystem::path resolve_case_insensitive_path(
    const std::filesystem::path &directory,
    const std::string &name) {
    std::filesystem::path candidate = directory / name;
    if (std::filesystem::exists(candidate)) {
        return candidate;
    }

    std::string wanted = name;
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return candidate;
    }
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        std::string current = entry.path().filename().string();
        std::transform(current.begin(), current.end(), current.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (current == wanted) {
            return entry.path();
        }
    }
    return candidate;
}

static std::vector<std::string> resolve_project_ase_paths(
    const char *project_path,
    const TdpProject &project,
    bool &missing) {
    std::vector<std::string> paths;
    missing = false;
    const std::filesystem::path base =
        std::filesystem::absolute(std::filesystem::path(project_path)).parent_path();
    for (int i = 0; i < TDP_MAX_LODS; ++i) {
        const std::string scene = fixed_string(project.lods[i].scene_file);
        if (scene.empty()) {
            break;
        }
        std::filesystem::path resolved = resolve_case_insensitive_path(base, scene);
        if (!std::filesystem::exists(resolved)) {
            missing = true;
        }
        paths.push_back(resolved.string());
    }
    return paths;
}

static void convert_material(const TdpMaterial &src,
                             opennova::object::project::Material &dst) {
    dst.name = fixed_string(src.name);
    dst.shader_tag = fixed_string(src.shader_tag);
    dst.rattrib = static_cast<int>(src.rattrib);
    dst.pattrib = static_cast<int>(src.pattrib);
    dst.ptype = static_cast<int>(src.ptype);
    dst.geofx = src.geofx;
    dst.geofx_value = src.geofx_value;
    dst.alphatestvalue = src.alphatestvalue;

    for (int channel = 0; channel < 2; ++channel) {
        dst.diffuse_tex[channel] = fixed_string(src.diffuse_tex[channel]);
        dst.diffuse_flags[channel] = src.diffuse_flags[channel];
        dst.normal_tex[channel] = src.normal_tex[channel][0]
            ? fixed_string(src.normal_tex[channel])
            : "0";
        dst.normal_flags[channel] = src.normal_flags[channel];
    }

    dst.anim_frames = src.anim_frames;
    dst.anim_type = src.anim_type;
    dst.anim_frametime = src.anim_frametime;
    dst.anim_ctrlreg = fixed_string(src.anim_ctrlreg);

    const int frame_count = std::min<int>(src.anim_frames, TDP_MAX_ANIM_FRAMES);
    for (int channel = 0; channel < 2; ++channel) {
        for (int frame = 0; frame < frame_count; ++frame) {
            const auto &ad = src.anim_diffuse[channel][frame];
            const auto &an = src.anim_normal[channel][frame];
            dst.anim_diffuse[channel][frame].path = fixed_string(ad.path);
            dst.anim_diffuse[channel][frame].enabled = ad.enabled;
            dst.anim_normal[channel][frame].path = fixed_string(an.path);
            dst.anim_normal[channel][frame].enabled = an.enabled;
        }
    }

    if (dst.diffuse_tex[0].empty() && !dst.anim_diffuse[0][0].path.empty()) {
        dst.diffuse_tex[0] = dst.anim_diffuse[0][0].path;
    }
    if (dst.diffuse_tex[1].empty() && !dst.anim_diffuse[1][0].path.empty()) {
        dst.diffuse_tex[1] = dst.anim_diffuse[1][0].path;
    }
    for (int channel = 0; channel < 2; ++channel) {
        for (int frame = 0; frame < frame_count; ++frame) {
            if (dst.anim_diffuse[channel][frame].path.empty()) {
                dst.anim_diffuse[channel][frame].path = "0";
            }
            if (dst.anim_normal[channel][frame].path.empty()) {
                dst.anim_normal[channel][frame].path = "0";
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        dst.reflect_rgb[i] = src.reflect_rgb[i];
        dst.rgbgen_srgb[i] = src.rgbgen_srgb[i];
        dst.rgbgen_ergb[i] = src.rgbgen_ergb[i];
    }
    dst.rgbgen_style = src.rgbgen_style;
    dst.rgbgen_rate = src.rgbgen_rate;
    dst.rgbgen_phase = src.rgbgen_phase;
    dst.rgbgen_ctrlreg = fixed_string(src.rgbgen_ctrlreg);
    dst.alphagen_style = src.alphagen_style;
    dst.alphagen_rate = src.alphagen_rate;
    dst.alphagen_phase = src.alphagen_phase;
    dst.alphagen_start = src.alphagen_start;
    dst.alphagen_end = src.alphagen_end;
    dst.alphagen_ctrlreg = fixed_string(src.alphagen_ctrlreg);
    dst.mapfunc_u_style = src.mapfunc_u_style;
    dst.mapfunc_u_rate = src.mapfunc_u_rate;
    dst.mapfunc_u_phase = src.mapfunc_u_phase;
    dst.mapfunc_u_start = src.mapfunc_u_start;
    dst.mapfunc_u_end = src.mapfunc_u_end;
    dst.mapfunc_u_ctrlreg = fixed_string(src.mapfunc_u_ctrlreg);
    dst.mapfunc_v_style = src.mapfunc_v_style;
    dst.mapfunc_v_rate = src.mapfunc_v_rate;
    dst.mapfunc_v_phase = src.mapfunc_v_phase;
    dst.mapfunc_v_start = src.mapfunc_v_start;
    dst.mapfunc_v_end = src.mapfunc_v_end;
    dst.mapfunc_v_ctrlreg = fixed_string(src.mapfunc_v_ctrlreg);
}

// Convert a TdpAxisFunc → project::AxisFunc
opennova::object::project::AxisFunc convert_axis(const TdpAxisFunc &src) {
    opennova::object::project::AxisFunc dst;
    dst.func_id = src.func_id;
    dst.param0 = src.param0;
    dst.param1 = src.param1;
    dst.param2 = src.param2;
    dst.param3 = src.param3;
    dst.ctrl_reg = src.ctrl_reg;
    return dst;
}

// Convert a TdpPartAnim → project::PartAnim
opennova::object::project::PartAnim convert_part_anim(const TdpPartAnim &src) {
    opennova::object::project::PartAnim dst;
    dst.rotate_type = src.rotate_type;
    dst.scale_type = src.scale_type;
    dst.trans_type = src.trans_type;
    dst.transform_as = src.transform_as;
    dst.yaw_rate = src.yaw_rate;
    dst.pitch_rate = src.pitch_rate;
    dst.roll_rate = src.roll_rate;
    dst.yaw = convert_axis(src.yaw);
    dst.pitch = convert_axis(src.pitch);
    dst.roll = convert_axis(src.roll);
    dst.reverse_rotate = src.reverse_rotate;
    dst.scale = convert_axis(src.scale);
    dst.scale_x = convert_axis(src.scale_x);
    dst.scale_y = convert_axis(src.scale_y);
    dst.scale_z = convert_axis(src.scale_z);
    dst.trans_x = convert_axis(src.trans_x);
    dst.trans_y = convert_axis(src.trans_y);
    dst.trans_z = convert_axis(src.trans_z);
    return dst;
}

// Convert TdpProject → project::Project
opennova::object::project::Project convert_project(const TdpProject *tdp) {
    opennova::object::project::Project proj;
    proj.version = tdp->version;
    proj.poly_collision_lod = tdp->poly_collision_lod;

    // Materials
    proj.materials.resize(tdp->material_count);
    for (size_t i = 0; i < tdp->material_count; ++i) {
        convert_material(tdp->materials[i], proj.materials[i]);
    }

    // LODs
    for (int li = 0; li < TDP_MAX_LODS; ++li) {
        const TdpLod &slod = tdp->lods[li];
        opennova::object::project::Lod &dlod = proj.lods[li];
        dlod.scene_file = slod.scene_file;
        dlod.attributes = slod.attributes;
        dlod.render_function = slod.render_function;
        dlod.threshold = slod.threshold;
        dlod.part_anim_enabled = slod.part_anim_enabled != 0;
        dlod.part_anims.resize(slod.part_anim_count);
        for (size_t pi = 0; pi < slod.part_anim_count; ++pi) {
            dlod.part_anims[pi] = convert_part_anim(slod.part_anims[pi]);
        }
        dlod.lights.clear();
        if (li == 0) {
            dlod.lights.resize(slod.light_count);
            for (size_t ki = 0; ki < slod.light_count; ++ki) {
                const TdpLight &sl = slod.lights[ki];
                opennova::object::project::Light &dl = dlod.lights[ki];
                dl.name = sl.name;
                dl.colorgen_style = sl.colorgen_style;
                dl.colorgen_rate = sl.colorgen_rate;
                dl.colorgen_phase = sl.colorgen_phase;
                for (int j = 0; j < 3; ++j) {
                    dl.colorgen_start[j] = sl.colorgen_start[j];
                    dl.colorgen_end[j] = sl.colorgen_end[j];
                }
                dl.colorgen_ctrlreg = sl.colorgen_ctrlreg;
                dl.disable_corona = sl.disable_corona;
                dl.disable_lightterrain = sl.disable_lightterrain;
                dl.disable_lightobjects = sl.disable_lightobjects;
            }
        }
    }

    return proj;
}

}  // namespace

// ---------------------------------------------------------------------------
// free_internal — release heap allocations inside OED data structures.
// These were originally in oed_json.cpp in the old codebase.
// ---------------------------------------------------------------------------

namespace opennova::object {

void free_internal(LodHeader& lod) {
    if (lod.subobjects) {
        for (uint32_t i = 0; i < lod.subobjectCount; ++i) {
            delete[] lod.subobjects[i].verts;
            delete[] lod.subobjects[i].uvs;
            delete[] lod.subobjects[i].faces;
            delete[] lod.subobjects[i].colors;
            delete[] lod.subobjects[i].preSmoothedNormals;
        }
        delete[] lod.subobjects;
    }
    delete[] lod.attachPoints;
    delete[] lod.centerPoints;
    if (lod.collisions) {
        for (uint32_t i = 0; i < lod.collisionCount; ++i) {
            delete[] lod.collisions[i].verts;
            delete[] lod.collisions[i].faces;
        }
        delete[] lod.collisions;
    }
    delete[] lod.userPoints;
    delete[] lod.lights;
    delete[] lod.materials;
    std::memset(&lod, 0, sizeof(lod));
}

void free_internal(LodBucketWorkspace& work) {
    free_internal(work.lod);
    std::memset(&work, 0, sizeof(work));
}

void free_internal(InternalState& state) {
    free_internal(state.workspace);
    std::memset(&state.material_table, 0, sizeof(state.material_table));
}

}  // namespace opennova::object

// ---------------------------------------------------------------------------
// BakeSession — holds parsed ASE workspaces for fast exports/re-exports.
// ---------------------------------------------------------------------------
struct BakeSession {
    std::vector<opennova::object::InternalState> states;
    std::vector<float> thresholds;
    int poly_collision_lod = 0;
};

namespace {

void free_session_states(std::vector<opennova::object::InternalState> &states) {
    for (auto &s : states) {
        opennova::object::free_internal(s);
    }
    states.clear();
}

BakeStatus parse_and_convert_ase(const char **ase_paths,
                                int ase_count,
                                const opennova::object::project::Project &proj,
    std::vector<opennova::object::InternalState> &states) {
    states.clear();
    states.resize(static_cast<size_t>(ase_count));
    for (int i = 0; i < ase_count; ++i) {
        if (!ase_paths[i]) {
            free_session_states(states);
            return BAKE_STATUS_INVALID_ARGUMENT;
        }

        auto doc = std::make_unique<ase_Document>();
        if (!doc) {
            free_session_states(states);
            return BAKE_STATUS_ALLOCATION_FAILED;
        }
        if (ase_parse(ase_paths[i], doc.get()) != 0) {
            free_session_states(states);
            return BAKE_STATUS_ASE_PARSE_FAILED;
        }

        opennova::object::ConvertOptions opts;
        opts.project = &proj;
        opts.lod_index = i;
        std::string err;
        if (!opennova::object::convert_to_internal(*doc, states[static_cast<size_t>(i)], opts, &err)) {
            ase_free(doc.get());
            free_session_states(states);
            return BAKE_STATUS_CONVERT_FAILED;
        }
        ase_free(doc.get());
    }
    return BAKE_STATUS_OK;
}

uint8_t normalize_update_mask(uint8_t mask) {
    const uint8_t bits = static_cast<uint8_t>(mask & BAKE_UPDATE_ALL);
    return bits == 0 ? static_cast<uint8_t>(BAKE_UPDATE_ALL) : bits;
}

void build_workspace_ptrs(const std::vector<opennova::object::InternalState> &states,
                          std::vector<const opennova::object::LodBucketWorkspace *> &out) {
    out.clear();
    out.reserve(states.size());
    for (const auto &s : states) {
        out.push_back(&s.workspace);
    }
}

bool update_session_from_project(BakeSession *session,
                                 const opennova::object::project::Project &proj,
                                 uint8_t update_mask) {
    if (!session) return false;
    const int count = static_cast<int>(session->states.size());
    for (int i = 0; i < count; ++i) {
        opennova::object::InternalState &state = session->states[static_cast<size_t>(i)];
        if (update_mask & BAKE_UPDATE_MTRL) {
            opennova::object::apply_material_table_from_project(&proj, state.material_table);
        }
        if (update_mask & BAKE_UPDATE_PANM) {
            opennova::object::populate_part_anim_from_project(&proj, state.workspace, i);
        }
        if ((update_mask & BAKE_UPDATE_LGHT) && i == 0) {
            opennova::object::apply_lights_from_project(&proj, state.workspace.lod, i);
        }
        opennova::object::apply_render_function_from_project(&proj, state.workspace.lod, i);
        session->thresholds[static_cast<size_t>(i)] =
            proj.lods[static_cast<size_t>(i)].threshold;
    }
    session->poly_collision_lod = proj.poly_collision_lod;
    return true;
}

}  // namespace

extern "C" BakeStatus bake_session_create(const char **ase_paths,
                                        int ase_count,
                                        const TdpProject *project,
                                        BakeSession **out_session) {
    if (!ase_paths || ase_count <= 0 || !project || !out_session) {
        return BAKE_STATUS_INVALID_ARGUMENT;
    }
    *out_session = nullptr;

    opennova::object::project::Project proj = convert_project(project);

    auto *session = new (std::nothrow) BakeSession;
    if (!session) {
        return BAKE_STATUS_ALLOCATION_FAILED;
    }
    const BakeStatus parse_status =
        parse_and_convert_ase(ase_paths, ase_count, proj, session->states);
    if (parse_status != BAKE_STATUS_OK) {
        delete session;
        return parse_status;
    }
    session->thresholds.reserve(static_cast<size_t>(ase_count));
    for (int i = 0; i < ase_count; ++i) {
        session->thresholds.push_back(proj.lods[static_cast<size_t>(i)].threshold);
    }
    session->poly_collision_lod = proj.poly_collision_lod;
    *out_session = session;
    return BAKE_STATUS_OK;
}

extern "C" BakeStatus bake_session_export(BakeSession *session,
                                        const BakeExportRequest *request) {
    if (!session || !request || !request->project || !request->output_path) {
        return BAKE_STATUS_INVALID_ARGUMENT;
    }
    if (session->states.empty() || session->thresholds.size() < session->states.size()) {
        return BAKE_STATUS_INVALID_ARGUMENT;
    }

    const uint8_t update_mask = normalize_update_mask(request->update_mask);
    const opennova::object::project::Project proj = convert_project(request->project);
    update_session_from_project(session, proj, update_mask);

    std::vector<const opennova::object::LodBucketWorkspace *> workspaces;
    build_workspace_ptrs(session->states, workspaces);

    opennova::object::Export3diOptions options{};
    if (request->model_name && request->model_name[0] != '\0') {
        options.model_name = request->model_name;
    }
    std::string export_err;
    const bool ok = opennova::object::export_3di(workspaces,
                                    &session->states[0].material_table,
                                    session->thresholds,
                                    session->poly_collision_lod,
                                    std::string(request->output_path),
                                    options,
                                    export_err);
    return ok ? BAKE_STATUS_OK : BAKE_STATUS_EXPORT_FAILED;
}

extern "C" BakeStatus bake_session_build_model(BakeSession *session,
                                             const TdpProject *project,
                                             uint8_t update_mask,
                                             const char *model_name,
                                             Threedi3di3 *out_model) {
    if (!session || !project || !out_model) {
        return BAKE_STATUS_INVALID_ARGUMENT;
    }
    if (session->states.empty() || session->thresholds.size() < session->states.size()) {
        return BAKE_STATUS_INVALID_ARGUMENT;
    }

    std::memset(out_model, 0, sizeof(*out_model));
    const opennova::object::project::Project proj = convert_project(project);
    update_session_from_project(session, proj, normalize_update_mask(update_mask));

    std::vector<const opennova::object::LodBucketWorkspace *> workspaces;
    build_workspace_ptrs(session->states, workspaces);

    opennova::object::Export3diOptions options{};
    if (model_name && model_name[0] != '\0') {
        options.model_name = model_name;
    } else if (!proj.source_path.empty()) {
        options.model_name = std::filesystem::path(proj.source_path).stem().string();
    }

    std::string error;
    return opennova::object::build_3di_model(workspaces,
                                &session->states[0].material_table,
                                session->thresholds,
                                session->poly_collision_lod,
                                options,
                                *out_model,
                                error)
               ? BAKE_STATUS_OK
               : BAKE_STATUS_EXPORT_FAILED;
}

extern "C" void bake_session_destroy(BakeSession *session) {
    if (!session) return;
    free_session_states(session->states);
    delete session;
}

extern "C" BakeStatus bake_project_export(const char *project_path,
                                        const char *output_path,
                                        const char *model_name,
                                        uint8_t update_mask) {
    if (!project_path || !project_path[0] || !output_path || !output_path[0]) {
        return BAKE_STATUS_INVALID_ARGUMENT;
    }

    TdpProject project{};
    tdp_init(&project);
    BakeSession *session = nullptr;
    BakeStatus status = BAKE_STATUS_OK;
    if (tdp_parse(project_path, &project) != 0) {
        tdp_free(&project);
        return BAKE_STATUS_PROJECT_PARSE_FAILED;
    }

    bool missing = false;
    std::vector<std::string> ase_paths =
        resolve_project_ase_paths(project_path, project, missing);
    if (ase_paths.empty()) {
        tdp_free(&project);
        return BAKE_STATUS_INVALID_ARGUMENT;
    }
    if (missing) {
        tdp_free(&project);
        return BAKE_STATUS_MISSING_ASE;
    }

    std::vector<const char *> raw_paths;
    raw_paths.reserve(ase_paths.size());
    for (const std::string &path : ase_paths) {
        raw_paths.push_back(path.c_str());
    }

    status = bake_session_create(raw_paths.data(),
                                 static_cast<int>(raw_paths.size()),
                                 &project,
                                 &session);
    if (status == BAKE_STATUS_OK) {
        BakeExportRequest request{};
        request.project = &project;
        request.output_path = output_path;
        request.update_mask = update_mask;
        request.model_name = model_name;
        status = bake_session_export(session, &request);
    }
    bake_session_destroy(session);
    tdp_free(&project);
    return status;
}
