// OED 3DI export adapter.
// Bridges the C API (TdpProject) to the internal C++ export pipeline.

#include "oed/oed.h"
#include "oed/convert_internal.h"
#include "oed/export_3di.h"
#include "oed/project_types.h"

#include "ase/ase.h"

#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
namespace {

template <typename T>
void append_pod(std::string &out, const T &value) {
    out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <size_t N>
void append_chars(std::string &out, const char (&value)[N]) {
    out.append(value, N);
}

void append_axis_func(std::string &out, const TdpAxisFunc &src) {
    append_pod(out, src.func_id);
    append_pod(out, src.param0);
    append_pod(out, src.param1);
    append_pod(out, src.param2);
    append_pod(out, src.param3);
    append_chars(out, src.ctrl_reg);
}

void append_part_anim(std::string &out, const TdpPartAnim &src) {
    append_pod(out, src.rotate_type);
    append_pod(out, src.scale_type);
    append_pod(out, src.trans_type);
    append_pod(out, src.transform_as);
    append_pod(out, src.yaw_rate);
    append_pod(out, src.pitch_rate);
    append_pod(out, src.roll_rate);
    append_axis_func(out, src.yaw);
    append_axis_func(out, src.pitch);
    append_axis_func(out, src.roll);
    append_pod(out, src.reverse_rotate);
    append_axis_func(out, src.scale);
    append_axis_func(out, src.scale_x);
    append_axis_func(out, src.scale_y);
    append_axis_func(out, src.scale_z);
    append_axis_func(out, src.trans_x);
    append_axis_func(out, src.trans_y);
    append_axis_func(out, src.trans_z);
}

void append_project_fingerprint(std::string &out, const TdpProject *project) {
    append_pod(out, project->version);
    append_pod(out, project->poly_collision_lod);
    append_pod(out, project->material_count);
    for (size_t i = 0; i < project->material_count; ++i) {
        const TdpMaterial &src = project->materials[i];
        append_chars(out, src.name);
        append_chars(out, src.shader_tag);
        append_pod(out, src.rattrib);
        append_pod(out, src.pattrib);
        append_pod(out, src.ptype);
        append_pod(out, src.geofx);
        append_pod(out, src.geofx_value);
        append_pod(out, src.alphatestvalue);
        for (int j = 0; j < 2; ++j) {
            append_chars(out, src.diffuse_tex[j]);
            append_pod(out, src.diffuse_flags[j]);
            append_chars(out, src.normal_tex[j]);
            append_pod(out, src.normal_flags[j]);
        }
        append_pod(out, src.anim_frames);
        append_pod(out, src.anim_type);
        append_pod(out, src.anim_frametime);
        append_chars(out, src.anim_ctrlreg);
        for (int ch = 0; ch < 2; ++ch) {
            for (int f = 0; f < TDP_MAX_ANIM_FRAMES; ++f) {
                append_chars(out, src.anim_diffuse[ch][f].path);
                append_pod(out, src.anim_diffuse[ch][f].enabled);
                append_chars(out, src.anim_normal[ch][f].path);
                append_pod(out, src.anim_normal[ch][f].enabled);
            }
        }
        for (int j = 0; j < 3; ++j) {
            append_pod(out, src.reflect_rgb[j]);
        }
        append_pod(out, src.rgbgen_style);
        append_pod(out, src.rgbgen_rate);
        append_pod(out, src.rgbgen_phase);
        for (int j = 0; j < 3; ++j) {
            append_pod(out, src.rgbgen_srgb[j]);
            append_pod(out, src.rgbgen_ergb[j]);
        }
        append_chars(out, src.rgbgen_ctrlreg);
        append_pod(out, src.alphagen_style);
        append_pod(out, src.alphagen_rate);
        append_pod(out, src.alphagen_phase);
        append_pod(out, src.alphagen_start);
        append_pod(out, src.alphagen_end);
        append_chars(out, src.alphagen_ctrlreg);
        append_pod(out, src.mapfunc_u_style);
        append_pod(out, src.mapfunc_u_rate);
        append_pod(out, src.mapfunc_u_phase);
        append_pod(out, src.mapfunc_u_start);
        append_pod(out, src.mapfunc_u_end);
        append_chars(out, src.mapfunc_u_ctrlreg);
        append_pod(out, src.mapfunc_v_style);
        append_pod(out, src.mapfunc_v_rate);
        append_pod(out, src.mapfunc_v_phase);
        append_pod(out, src.mapfunc_v_start);
        append_pod(out, src.mapfunc_v_end);
        append_chars(out, src.mapfunc_v_ctrlreg);
    }

    for (int li = 0; li < TDP_MAX_LODS; ++li) {
        const TdpLod &lod = project->lods[li];
        append_chars(out, lod.scene_file);
        append_pod(out, lod.attributes);
        append_chars(out, lod.render_function);
        append_pod(out, lod.threshold);
        append_pod(out, lod.part_anim_enabled);
        append_pod(out, lod.part_anim_count);
        for (size_t i = 0; i < lod.part_anim_count; ++i) {
            append_part_anim(out, lod.part_anims[i]);
        }
        append_pod(out, lod.light_count);
        for (size_t i = 0; i < lod.light_count; ++i) {
            const TdpLight &src = lod.lights[i];
            append_chars(out, src.name);
            append_pod(out, src.colorgen_style);
            append_pod(out, src.colorgen_rate);
            append_pod(out, src.colorgen_phase);
            for (int j = 0; j < 3; ++j) {
                append_pod(out, src.colorgen_start[j]);
                append_pod(out, src.colorgen_end[j]);
            }
            append_chars(out, src.colorgen_ctrlreg);
            append_pod(out, src.disable_corona);
            append_pod(out, src.disable_lightterrain);
            append_pod(out, src.disable_lightobjects);
        }
    }
}

std::string project_fingerprint(const TdpProject *project) {
    std::string out;
    if (project) {
        append_project_fingerprint(out, project);
    }
    return out;
}

std::string sibling_3di_path(const char *ase_path) {
    if (!ase_path || ase_path[0] == '\0') {
        return {};
    }
    std::filesystem::path path(ase_path);
    path.replace_extension(".3di");
    return std::filesystem::is_regular_file(path) ? path.string() : std::string();
}

// Convert a TdpAxisFunc ? project::AxisFunc
oed::project::AxisFunc convert_axis(const TdpAxisFunc &src) {
    oed::project::AxisFunc dst;
    dst.func_id = src.func_id;
    dst.param0 = src.param0;
    dst.param1 = src.param1;
    dst.param2 = src.param2;
    dst.param3 = src.param3;
    dst.ctrl_reg = src.ctrl_reg;
    return dst;
}

// Convert a TdpPartAnim ? project::PartAnim
oed::project::PartAnim convert_part_anim(const TdpPartAnim &src) {
    oed::project::PartAnim dst;
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

// Convert TdpProject ? project::Project
oed::project::Project convert_project(const TdpProject *tdp) {
    oed::project::Project proj;
    proj.version = tdp->version;
    proj.poly_collision_lod = tdp->poly_collision_lod;

    // Materials
    proj.materials.resize(tdp->material_count);
    for (size_t i = 0; i < tdp->material_count; ++i) {
        const TdpMaterial &src = tdp->materials[i];
        oed::project::Material &dst = proj.materials[i];
        dst.name = src.name;
        dst.shader_tag = src.shader_tag;
        dst.rattrib = src.rattrib;
        dst.pattrib = src.pattrib;
        dst.ptype = src.ptype;
        dst.geofx = src.geofx;
        dst.geofx_value = src.geofx_value;
        dst.alphatestvalue = src.alphatestvalue;
        for (int j = 0; j < 2; ++j) {
            dst.diffuse_tex[j] = src.diffuse_tex[j];
            dst.diffuse_flags[j] = src.diffuse_flags[j];
            dst.normal_tex[j] = src.normal_tex[j];
            dst.normal_flags[j] = src.normal_flags[j];
        }
        dst.anim_frames = src.anim_frames;
        dst.anim_type = src.anim_type;
        dst.anim_frametime = src.anim_frametime;
        dst.anim_ctrlreg = src.anim_ctrlreg;
        for (int ch = 0; ch < 2; ++ch) {
            for (int f = 0; f < TDP_MAX_ANIM_FRAMES; ++f) {
                dst.anim_diffuse[ch][f].path = src.anim_diffuse[ch][f].path;
                dst.anim_diffuse[ch][f].enabled = src.anim_diffuse[ch][f].enabled;
                dst.anim_normal[ch][f].path = src.anim_normal[ch][f].path;
                dst.anim_normal[ch][f].enabled = src.anim_normal[ch][f].enabled;
            }
        }
        for (int j = 0; j < 3; ++j) {
            dst.reflect_rgb[j] = src.reflect_rgb[j];
            dst.rgbgen_srgb[j] = src.rgbgen_srgb[j];
            dst.rgbgen_ergb[j] = src.rgbgen_ergb[j];
        }
        dst.rgbgen_style = src.rgbgen_style;
        dst.rgbgen_rate = src.rgbgen_rate;
        dst.rgbgen_phase = src.rgbgen_phase;
        dst.rgbgen_ctrlreg = src.rgbgen_ctrlreg;
        dst.alphagen_style = src.alphagen_style;
        dst.alphagen_rate = src.alphagen_rate;
        dst.alphagen_phase = src.alphagen_phase;
        dst.alphagen_start = src.alphagen_start;
        dst.alphagen_end = src.alphagen_end;
        dst.alphagen_ctrlreg = src.alphagen_ctrlreg;
        dst.mapfunc_u_style = src.mapfunc_u_style;
        dst.mapfunc_u_rate = src.mapfunc_u_rate;
        dst.mapfunc_u_phase = src.mapfunc_u_phase;
        dst.mapfunc_u_start = src.mapfunc_u_start;
        dst.mapfunc_u_end = src.mapfunc_u_end;
        dst.mapfunc_u_ctrlreg = src.mapfunc_u_ctrlreg;
        dst.mapfunc_v_style = src.mapfunc_v_style;
        dst.mapfunc_v_rate = src.mapfunc_v_rate;
        dst.mapfunc_v_phase = src.mapfunc_v_phase;
        dst.mapfunc_v_start = src.mapfunc_v_start;
        dst.mapfunc_v_end = src.mapfunc_v_end;
        dst.mapfunc_v_ctrlreg = src.mapfunc_v_ctrlreg;
    }

    // LODs
    for (int li = 0; li < TDP_MAX_LODS; ++li) {
        const TdpLod &slod = tdp->lods[li];
        oed::project::Lod &dlod = proj.lods[li];
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
                oed::project::Light &dl = dlod.lights[ki];
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
// free_internal ? release heap allocations inside OED data structures.
// These were originally in oed_json.cpp in the old codebase.
// ---------------------------------------------------------------------------

namespace oed {

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

}  // namespace oed

// ---------------------------------------------------------------------------
// OedSession ? holds parsed ASE workspaces for fast exports/re-exports.
// ---------------------------------------------------------------------------
struct OedSession {
    std::vector<oed::InternalState> states;
    std::vector<float> thresholds;
    int poly_collision_lod = 0;
    std::string initial_project_fingerprint;
    std::string baseline_3di_path;
    std::string last_error;
};

namespace {

void free_session_states(std::vector<oed::InternalState> &states) {
    for (auto &s : states) {
        oed::free_internal(s);
    }
    states.clear();
}

OedStatus parse_and_convert_ase(const char **ase_paths,
                                int ase_count,
                                const oed::project::Project &proj,
                                std::vector<oed::InternalState> &states) {
    states.clear();
    try {
        states.resize(static_cast<size_t>(ase_count));
    } catch (const std::bad_alloc &) {
        return OED_STATUS_ALLOCATION_FAILED;
    }
    for (int i = 0; i < ase_count; ++i) {
        if (!ase_paths[i]) {
            free_session_states(states);
            return OED_STATUS_INVALID_ARGUMENT;
        }

        ase_Document doc{};
        if (ase_parse(ase_paths[i], &doc) != 0) {
            free_session_states(states);
            return OED_STATUS_ASE_PARSE_FAILED;
        }

        oed::ConvertOptions opts;
        opts.project = &proj;
        opts.lod_index = i;
        std::string err;
        if (!oed::convert_to_internal(doc, states[static_cast<size_t>(i)], opts, &err)) {
            ase_free(&doc);
            free_session_states(states);
            return OED_STATUS_CONVERT_FAILED;
        }
        ase_free(&doc);
    }
    return OED_STATUS_OK;
}

uint8_t normalize_update_mask(uint8_t mask) {
    const uint8_t bits = static_cast<uint8_t>(mask & OED_UPDATE_ALL);
    return bits == 0 ? static_cast<uint8_t>(OED_UPDATE_ALL) : bits;
}

void build_workspace_ptrs(const std::vector<oed::InternalState> &states,
                          std::vector<const oed::LodBucketWorkspace *> &out) {
    out.clear();
    out.reserve(states.size());
    for (const auto &s : states) {
        out.push_back(&s.workspace);
    }
}

bool project_matches_initial(const OedSession *session, const TdpProject *project) {
    return session &&
           !session->initial_project_fingerprint.empty() &&
           session->initial_project_fingerprint == project_fingerprint(project);
}

void clear_session_error(OedSession *session) {
    if (session) {
        session->last_error.clear();
    }
}

void set_session_error(OedSession *session, std::string message) {
    if (session) {
        session->last_error = std::move(message);
    }
}

bool has_model_name_override(const char *model_name) {
    return model_name != nullptr && model_name[0] != '\0';
}

std::string default_model_name(const oed::project::Project &proj) {
    if (!proj.lods[0].scene_file.empty()) {
        return std::filesystem::path(proj.lods[0].scene_file).stem().string();
    }
    if (!proj.source_path.empty()) {
        return std::filesystem::path(proj.source_path).stem().string();
    }
    return {};
}

bool update_session_from_project(OedSession *session,
                                 const oed::project::Project &proj,
                                 uint8_t update_mask) {
    if (!session) return false;
    const int count = static_cast<int>(session->states.size());
    for (int i = 0; i < count; ++i) {
        oed::InternalState &state = session->states[static_cast<size_t>(i)];
        if (update_mask & OED_UPDATE_MTRL) {
            oed::apply_material_table_from_project(&proj, state.material_table);
        }
        if (update_mask & OED_UPDATE_PANM) {
            oed::populate_part_anim_from_project(&proj, state.workspace, i);
        }
        if ((update_mask & OED_UPDATE_LGHT) && i == 0) {
            oed::apply_lights_from_project(&proj, state.workspace.lod, i);
        }
        oed::apply_render_function_from_project(&proj, state.workspace.lod, i);
        session->thresholds[static_cast<size_t>(i)] =
            proj.lods[static_cast<size_t>(i)].threshold;
    }
    session->poly_collision_lod = proj.poly_collision_lod;
    return true;
}

}  // namespace

extern "C" OedStatus oed_session_create(const char **ase_paths,
                                        int ase_count,
                                        const TdpProject *project,
                                        OedSession **out_session) {
    if (!ase_paths || ase_count <= 0 || !project || !out_session) {
        return OED_STATUS_INVALID_ARGUMENT;
    }
    *out_session = nullptr;

    oed::project::Project proj = convert_project(project);

    auto *session = new (std::nothrow) OedSession;
    if (!session) {
        return OED_STATUS_ALLOCATION_FAILED;
    }
    const OedStatus parse_status =
        parse_and_convert_ase(ase_paths, ase_count, proj, session->states);
    if (parse_status != OED_STATUS_OK) {
        delete session;
        return parse_status;
    }
    session->thresholds.reserve(static_cast<size_t>(ase_count));
    for (int i = 0; i < ase_count; ++i) {
        session->thresholds.push_back(proj.lods[static_cast<size_t>(i)].threshold);
    }
    session->poly_collision_lod = proj.poly_collision_lod;
    session->initial_project_fingerprint = project_fingerprint(project);
    session->baseline_3di_path = sibling_3di_path(ase_paths[0]);
    *out_session = session;
    return OED_STATUS_OK;
}
extern "C" OedStatus oed_session_export(OedSession *session,
                                        const OedExportRequest *request) {
    if (!session || !request || !request->project || !request->output_path) {
        return OED_STATUS_INVALID_ARGUMENT;
    }
    clear_session_error(session);
    if (session->states.empty() || session->thresholds.size() < session->states.size()) {
        set_session_error(session, "OED session has no converted ASE workspaces");
        return OED_STATUS_INVALID_ARGUMENT;
    }

    if (!has_model_name_override(request->model_name) &&
        !session->baseline_3di_path.empty() &&
        project_matches_initial(session, request->project)) {
        std::error_code ec;
        const std::filesystem::path source(session->baseline_3di_path);
        const std::filesystem::path destination(request->output_path);
        if (std::filesystem::equivalent(source, destination, ec) && !ec) {
            return OED_STATUS_OK;
        }
        ec.clear();
        std::filesystem::copy_file(source,
                                   destination,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            set_session_error(session,
                              "Failed to copy baseline 3DI from '" + source.string() +
                                      "' to '" + destination.string() + "': " + ec.message());
            return OED_STATUS_EXPORT_FAILED;
        }
        return OED_STATUS_OK;
    }

    const uint8_t update_mask = normalize_update_mask(request->update_mask);
    const oed::project::Project proj = convert_project(request->project);
    update_session_from_project(session, proj, update_mask);

    std::vector<const oed::LodBucketWorkspace *> workspaces;
    build_workspace_ptrs(session->states, workspaces);

    oed::Export3diOptions options{};
    if (request->model_name && request->model_name[0] != '\0') {
        options.model_name = request->model_name;
    } else {
        options.model_name = default_model_name(proj);
    }
    std::string export_err;
    const bool ok = oed::export_3di(workspaces,
                                    &session->states[0].material_table,
                                    session->thresholds,
                                    session->poly_collision_lod,
                                    std::string(request->output_path),
                                    options,
                                    export_err);
    if (!ok) {
        set_session_error(session, export_err.empty() ? "Failed to export 3DI" : export_err);
        return OED_STATUS_EXPORT_FAILED;
    }
    return OED_STATUS_OK;
}

extern "C" OedStatus oed_session_build_model(OedSession *session,
                                             const TdpProject *project,
                                             uint8_t update_mask,
                                             const char *model_name,
                                             Threedi3di3 *out_model) {
    if (!session || !project || !out_model) {
        return OED_STATUS_INVALID_ARGUMENT;
    }
    clear_session_error(session);
    if (session->states.empty() || session->thresholds.size() < session->states.size()) {
        set_session_error(session, "OED session has no converted ASE workspaces");
        return OED_STATUS_INVALID_ARGUMENT;
    }

    if (!has_model_name_override(model_name) &&
        !session->baseline_3di_path.empty() &&
        project_matches_initial(session, project)) {
        std::memset(out_model, 0, sizeof(*out_model));
        if (threedi_3di3_read(session->baseline_3di_path.c_str(), out_model) != 0) {
            set_session_error(session,
                              "Failed to read baseline 3DI '" + session->baseline_3di_path + "'");
            return OED_STATUS_EXPORT_FAILED;
        }
        return OED_STATUS_OK;
    }

    std::memset(out_model, 0, sizeof(*out_model));
    const oed::project::Project proj = convert_project(project);
    update_session_from_project(session, proj, normalize_update_mask(update_mask));

    std::vector<const oed::LodBucketWorkspace *> workspaces;
    build_workspace_ptrs(session->states, workspaces);

    oed::Export3diOptions options{};
    if (model_name && model_name[0] != '\0') {
        options.model_name = model_name;
    } else {
        options.model_name = default_model_name(proj);
    }

    std::string error;
    if (!oed::build_3di_model(workspaces,
                              &session->states[0].material_table,
                              session->thresholds,
                              session->poly_collision_lod,
                              options,
                              *out_model,
                              error)) {
        set_session_error(session, error.empty() ? "Failed to build OED model" : error);
        return OED_STATUS_EXPORT_FAILED;
    }
    return OED_STATUS_OK;
}

extern "C" const char *oed_session_last_error(const OedSession *session) {
    if (!session || session->last_error.empty()) {
        return "";
    }
    return session->last_error.c_str();
}

extern "C" void oed_session_destroy(OedSession *session) {
    if (!session) return;
    free_session_states(session->states);
    delete session;
}
