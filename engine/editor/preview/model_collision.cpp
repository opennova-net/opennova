// The collision a model viewport shows (model_collision.h).

#include <editor/preview/model_collision.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#include <base/io/fixed.h>
#include <editor/documents/model_collision_words.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_surfaces.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <runtime/world/model_geometry.h>
#include <runtime/world/occlusion.h>

namespace opennova::editor {

namespace {

using threedi::ThreediBuildVec3;
using threedi::ThreediMatrix4x4;

// --- the static geometry, mission axes, built once per parsed model ------------------------------

struct Polygon {
	std::vector<ThreediBuildVec3> corners;
};

struct FaceGeometry {
	ThreediBuildVec3 corner[3];
	int section = -1;
	bool valid = false;
};

struct VolumeGeometry {
	int section = -1;
	int64_t type = 0;
	std::vector<Polygon> polygons; // the solid's facets; empty: its box below
	ThreediBuildVec3 min, max;     // its stored box
	bool owned = false;            // a section's run holds it (an unowned trailing volume is dead data)
};

struct SectionGeometry {
	bool sphere = false; // a bound sphere is stored (radius 0 or more, min below max)
	ThreediBuildVec3 center, min, max;
	double radius = 0.0;
	bool person = false; // a person's bone section: its hit sphere (model_section_is_person)
	bool bone = false;   // a bone section of a skinned model no person wears
	bool breaks = false; // a blast breaks it off (model_section_breaks)
	int32_t radius_q16 = 0;
};

struct Geometry {
	std::vector<FaceGeometry> faces;
	std::vector<VolumeGeometry> volumes;
	std::vector<SectionGeometry> sections;
	world::OcclusionModel occlusion;
	bool has_occlusion = false;
	bool has_bounds = false;
	ThreediBuildVec3 box_min, box_max;
	ThreediBuildVec3 sphere_center; // the projection sphere's, mission axes
	double sphere_radius = 0.0;
	double bound_radius = 0.0;
	bool has_probes = false;
	threedi::ThreediCollisionProbeBoxes probes{};
};

constexpr double q16(int32_t v) { return v / 65536.0; }

ThreediBuildVec3 vec(const float p[3]) { return ThreediBuildVec3{p[0], p[1], p[2]}; }

std::shared_ptr<const Geometry> make_geometry(const threedi::Threedi3di3 &model) {
	auto out = std::make_shared<Geometry>();
	const threedi::ThreediCollisionModel *c = model.collision;
	if (c) {
		std::vector<threedi::ThreediCollisionObjectRun> runs(c->object_count);
		const bool linked = threedi::threedi_collision_object_runs(c, runs.data()) != 0;
		const bool skinned = model.header.mesh_type == threedi::THREEDI_MESH_SKINNED;
		// The faces of each section's run, their corners in its vertex run (the loader's prefix sums).
		out->faces.resize(c->face_count);
		if (linked && threedi::threedi_3di3_collision_faces_runtime_safe(c)) {
			for (size_t o = 0; o < c->object_count; ++o) {
				const threedi::ThreediCollisionObject &object = c->objects[o];
				for (int32_t f = 0; f < object.num_faces; ++f) {
					const size_t face = size_t(runs[o].face_start + f);
					FaceGeometry &g = out->faces[face];
					g.section = int(o);
					g.valid = true;
					for (int k = 0; k < 3; ++k)
						g.corner[k] = vec(c->vertices[size_t(runs[o].vertex_start + c->faces[face].vert_index[k])].position);
				}
			}
		}
		// The volumes: each section's run, and the planes in the windows the volumes take in order.
		out->volumes.resize(c->volume_count);
		size_t plane_cursor = 0;
		for (size_t v = 0; v < c->volume_count; ++v) {
			const threedi::ThreediBoundingVolume &bv = c->volumes[v];
			VolumeGeometry &g = out->volumes[v];
			g.type = bv.collidable_type;
			g.min = ThreediBuildVec3{q16(bv.min_x_fp16), q16(bv.min_y_fp16), q16(bv.min_z_fp16)};
			g.max = ThreediBuildVec3{q16(bv.max_x_fp16), q16(bv.max_y_fp16), q16(bv.max_z_fp16)};
			const size_t planes = bv.plane_count > 0 ? size_t(bv.plane_count) : 0;
			if (c->planes && plane_cursor + planes <= c->plane_count)
				for (auto &corners : model_volume_solid(bv, c->planes + plane_cursor))
					g.polygons.push_back(Polygon{std::move(corners)});
			plane_cursor += planes;
		}
		if (linked)
			for (size_t o = 0; o < c->object_count; ++o)
				for (int32_t v = 0; v < c->objects[o].num_bounding_volumes; ++v) {
					VolumeGeometry &g = out->volumes[size_t(runs[o].volume_start + v)];
					g.section = int(o);
					g.owned = true;
				}
		// The sections' bound spheres and boxes (an empty row's inverted sentinel bounds show nothing).
		out->sections.resize(c->object_count);
		for (size_t o = 0; o < c->object_count; ++o) {
			const threedi::ThreediCollisionObject &object = c->objects[o];
			SectionGeometry &g = out->sections[o];
			g.sphere = object.radius >= 0 && object.min[0] <= object.max[0] && object.min[1] <= object.max[1] &&
			           object.min[2] <= object.max[2];
			g.center = ThreediBuildVec3{q16(object.med[0]), q16(object.med[1]), q16(object.med[2])};
			g.min = ThreediBuildVec3{q16(object.min[0]), q16(object.min[1]), q16(object.min[2])};
			g.max = ThreediBuildVec3{q16(object.max[0]), q16(object.max[1]), q16(object.max[2])};
			g.radius_q16 = object.radius;
			g.radius = q16(object.radius);
			g.person = model_section_is_person(model, o);
			g.bone = !g.person && skinned && object.num_faces == 0 && object.num_bounding_volumes == 0;
			g.breaks = model_section_breaks(object);
		}
		// The collision block's box and the sphere the game projects (world::collision_projection_sphere_
		// from_3di, the runtime's own); the bound radius the entity takes from the header, stamped only
		// with a collision block, plus 0x1000 [orig: Entity_InitFromModel @ 0x40dc30].
		const threedi::ThreediCollisionModelData &data = c->model_data;
		out->has_bounds = data.bbox[0] <= data.bbox[3] && data.bbox[1] <= data.bbox[4] && data.bbox[2] <= data.bbox[5];
		out->box_min = ThreediBuildVec3{data.bbox[0], data.bbox[1], data.bbox[2]};
		out->box_max = ThreediBuildVec3{data.bbox[3], data.bbox[4], data.bbox[5]};
		const renderer::ObjectProjectionSphere sphere = world::collision_projection_sphere_from_3di(model);
		out->sphere_center = ThreediBuildVec3{q16(sphere.center_q16[0]), q16(sphere.center_q16[1]), q16(sphere.center_q16[2])};
		out->sphere_radius = q16(sphere.radius_q16);
		out->bound_radius = (double(world::model_bound_radius_q16_from_3di(model)) + 0x1000) / 65536.0;
		out->has_probes = threedi::threedi_3di3_collision_probe_boxes(c, &out->probes) != 0;
	}
	out->has_occlusion = world::occlusion_model_from_3di(model, out->occlusion);
	return out;
}

std::shared_ptr<const Geometry> geometry_of(const assets::Model &model) {
	static std::mutex mutex;
	static std::map<const threedi::Threedi3di3 *,
	                std::pair<std::weak_ptr<const threedi::Threedi3di3>, std::shared_ptr<const Geometry>>>
			cache;
	if (!model) return std::make_shared<Geometry>();
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = cache.find(model.get());
		if (found != cache.end() && found->second.first.lock() == model) return found->second.second;
	}
	std::shared_ptr<const Geometry> made = make_geometry(*model);
	std::lock_guard<std::mutex> lock(mutex);
	for (auto it = cache.begin(); it != cache.end();) it = it->second.first.expired() ? cache.erase(it) : std::next(it);
	cache[model.get()] = {model, made};
	return made;
}

// --- posing ------------------------------------------------------------------------------------

// A mission-axes point through part `part`'s posed matrix (model axes, row-vector) into the preview's
// space, where a PANM node drives the part (its matrix turns about its pivot: the identity at rest); as it
// is where none does, as the runtime gives such a section the entity's matrix alone [orig:
// BoneCallback_Generic @ 0x4e26d0, overriding only the PANM nodes' parts; runtime/world/entity_pose.cpp].
struct Poser {
	const std::vector<ThreediMatrix4x4> *parts = nullptr;
	const std::vector<uint8_t> *driven = nullptr;

	PreviewVec3 operator()(const ThreediBuildVec3 &mission, int part) const {
		const ThreediBuildVec3 m = threedi::threedi_build_to_model(mission);
		float p[3] = {float(m.x), float(m.y), float(m.z)};
		if (parts && driven && part >= 0 && size_t(part) < parts->size() && size_t(part) < driven->size() &&
		    (*driven)[size_t(part)]) {
			const ThreediMatrix4x4 &t = (*parts)[size_t(part)];
			float q[3];
			for (int c = 0; c < 3; ++c) q[c] = p[0] * t.m[c] + p[1] * t.m[4 + c] + p[2] * t.m[8 + c] + t.m[12 + c];
			std::copy(q, q + 3, p);
		}
		return preview_from_model(p);
	}
};

PreviewVec3 model_point(const float p[3], const ThreediMatrix4x4 *pose) {
	float q[3] = {p[0], p[1], p[2]};
	if (pose)
		for (int c = 0; c < 3; ++c) q[c] = p[0] * pose->m[c] + p[1] * pose->m[4 + c] + p[2] * pose->m[8 + c] + pose->m[12 + c];
	return preview_from_model(q);
}

void add_edge(ModelCollisionShape &shape, const PreviewVec3 &a, const PreviewVec3 &b) {
	shape.edges.push_back(a);
	shape.edges.push_back(b);
}

// A box's twelve edges (and, pickable, its twelve triangles), mission axes through the poser.
void add_box(ModelCollisionShape &shape, const ThreediBuildVec3 &lo, const ThreediBuildVec3 &hi, const Poser &pose,
             int part, bool triangles) {
	PreviewVec3 c[8];
	for (int i = 0; i < 8; ++i)
		c[i] = pose(ThreediBuildVec3{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z}, part);
	static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (const auto &e : kEdges) add_edge(shape, c[e[0]], c[e[1]]);
	if (!triangles) return;
	static const int kFaces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
	for (const auto &f : kFaces)
		for (const int t : {1, 2}) {
			shape.triangles.push_back(c[f[0]]);
			shape.triangles.push_back(c[f[t]]);
			shape.triangles.push_back(c[f[t + 1]]);
		}
}

void add_polygon(ModelCollisionShape &shape, const std::vector<PreviewVec3> &corners) {
	for (size_t i = 0; i < corners.size(); ++i) add_edge(shape, corners[i], corners[(i + 1) % corners.size()]);
	for (size_t i = 1; i + 1 < corners.size(); ++i) {
		shape.triangles.push_back(corners[0]);
		shape.triangles.push_back(corners[i]);
		shape.triangles.push_back(corners[i + 1]);
	}
}

std::string volume_name(size_t index, int64_t type) {
	const ModelVolumeType &words = model_volume_type(type);
	return "Volume " + std::to_string(index + 1) + ": " + words.words + (words.code[0] ? std::string(" (") + words.code + ")" : "");
}

} // namespace

const char *model_collision_kind_token(ModelCollisionKind kind) {
	switch (kind) {
	case ModelCollisionKind::Face: return "face";
	case ModelCollisionKind::Volume: return "volume";
	case ModelCollisionKind::Section: return "section";
	case ModelCollisionKind::Occlusion: return "occlusion";
	case ModelCollisionKind::Bounds: return "collision_box";
	case ModelCollisionKind::ProjectionSphere: return "projection_sphere";
	case ModelCollisionKind::BoundRadius: return "bound_radius";
	case ModelCollisionKind::ProbeBox: return "probe_box";
	case ModelCollisionKind::PartSphere: return "part_sphere";
	}
	return "face";
}

const ModelCollisionLayerRow &model_collision_layer(ModelCollisionLayer layer) {
	static const ModelCollisionLayerRow rows[] = {
		{ModelCollisionLayer::BulletFaces, "bullet_faces", "Bullet faces", kModelBulletFaceWords, 0xFF5722},
		{ModelCollisionLayer::Volumes, "volumes", "Volumes", kModelVolumeWords, 0x42A5F5},
		{ModelCollisionLayer::Sections, "sections", "Sections and hit spheres", kModelSectionWords, 0xBDBDBD},
		{ModelCollisionLayer::Bounds, "bounds", "Bounds", kModelBoundsWords, 0xD4E157},
		{ModelCollisionLayer::ProbeBoxes, "probe_boxes", "Vehicle probe boxes", kModelProbeBoxWords, 0xFFCA28},
		{ModelCollisionLayer::Occlusion, "occlusion", "Occlusion", kModelOcclusionWords, 0x7986CB},
		{ModelCollisionLayer::PartSpheres, "part_spheres", "Part spheres", kModelPartSphereWords, 0x90A4AE},
	};
	static_assert(sizeof(rows) / sizeof(rows[0]) == size_t(ModelCollisionLayer::kCount), "a row per layer");
	return rows[size_t(layer) < size_t(ModelCollisionLayer::kCount) ? size_t(layer) : 0];
}

bool model_collision_layer_on(const ModelOverlayOptions &options, ModelCollisionLayer layer) {
	switch (layer) {
	case ModelCollisionLayer::BulletFaces: return options.bullet_faces;
	case ModelCollisionLayer::Volumes: return options.volumes;
	case ModelCollisionLayer::Sections: return options.sections;
	case ModelCollisionLayer::Bounds: return options.bounds;
	case ModelCollisionLayer::ProbeBoxes: return options.probe_boxes;
	case ModelCollisionLayer::Occlusion: return options.occlusion;
	case ModelCollisionLayer::PartSpheres: return options.part_spheres;
	case ModelCollisionLayer::kCount: break;
	}
	return false;
}

void model_collision_layer_set(ModelOverlayOptions &options, ModelCollisionLayer layer, bool on) {
	switch (layer) {
	case ModelCollisionLayer::BulletFaces: options.bullet_faces = on; break;
	case ModelCollisionLayer::Volumes: options.volumes = on; break;
	case ModelCollisionLayer::Sections: options.sections = on; break;
	case ModelCollisionLayer::Bounds: options.bounds = on; break;
	case ModelCollisionLayer::ProbeBoxes: options.probe_boxes = on; break;
	case ModelCollisionLayer::Occlusion: options.occlusion = on; break;
	case ModelCollisionLayer::PartSpheres: options.part_spheres = on; break;
	case ModelCollisionLayer::kCount: break;
	}
}

uint32_t model_volume_rgb(int64_t type) {
	switch (model_volume_type(type).family) {
	case ModelVolumeFamily::Solid: return 0x42A5F5;
	case ModelVolumeFamily::SolidFor: return 0x26C6DA;
	case ModelVolumeFamily::Ladder: return 0x66BB6A;
	case ModelVolumeFamily::Blink: return 0xFFF176;
	case ModelVolumeFamily::Zone: return 0xBA68C8;
	case ModelVolumeFamily::Damage: return 0xEF5350;
	}
	return 0x42A5F5;
}

size_t model_collision_layer_count(const threedi::Threedi3di3 &model, ModelCollisionLayer layer, int lod) {
	const threedi::ThreediCollisionModel *c = model.collision;
	switch (layer) {
	case ModelCollisionLayer::BulletFaces: return c ? c->face_count : 0;
	case ModelCollisionLayer::Volumes: return c ? c->volume_count : 0;
	case ModelCollisionLayer::Sections: {
		size_t n = 0;
		for (size_t o = 0; c && o < c->object_count; ++o) {
			const threedi::ThreediCollisionObject &s = c->objects[o];
			if (s.radius >= 0 && s.min[0] <= s.max[0] && s.min[1] <= s.max[1] && s.min[2] <= s.max[2]) ++n;
		}
		return n;
	}
	case ModelCollisionLayer::Bounds: return c ? 3 : 0;
	case ModelCollisionLayer::ProbeBoxes: {
		threedi::ThreediCollisionProbeBoxes probes{};
		return c && threedi::threedi_3di3_collision_probe_boxes(c, &probes) ? 2 : 0;
	}
	case ModelCollisionLayer::Occlusion: return model.occlusion_object_count;
	case ModelCollisionLayer::PartSpheres:
		return model.lods && lod >= 0 && size_t(lod) < model.lod_count ? model.lods[lod].render_object_count : 0;
	case ModelCollisionLayer::kCount: break;
	}
	return 0;
}

std::vector<ModelCollisionShape> model_collision_shapes(const assets::Model &shown, int lod, uint32_t time_ms,
                                                        const int32_t bus[96], const ModelOverlayOptions &options,
                                                        ModelCollisionPick also) {
	std::vector<ModelCollisionShape> out;
	const bool any = options.bullet_faces || options.volumes || options.sections || options.bounds ||
	                 options.probe_boxes || options.occlusion || options.part_spheres || also.valid();
	if (!shown || !any) return out;
	const threedi::Threedi3di3 &model = *shown;
	const std::shared_ptr<const Geometry> g = geometry_of(shown);
	const bool skinned = model.header.mesh_type == threedi::THREEDI_MESH_SKINNED;
	const std::string part_prefix = skinned ? "BN" : "PN";
	const auto part_word = [&](size_t part) {
		char digits[16];
		std::snprintf(digits, sizeof(digits), "%02zu", part + 1);
		return part_prefix + digits;
	};
	// LOD 0's parts carry the sections while LOD 0 animates (model_overlays' rule for a user point); a
	// person's bones are shown at rest.
	std::vector<ThreediMatrix4x4> first;
	std::vector<uint8_t> driven;
	const bool posed = !skinned && threedi::threedi_panm_lod_has_live(model, 0) &&
	                   threedi::threedi_panm_pose_parts(model, 0, time_ms, bus, first, &driven);
	const Poser pose{posed ? &first : nullptr, posed ? &driven : nullptr};
	const auto wanted = [&](ModelCollisionKind kind, size_t index, bool layer) {
		return layer || (also.kind == kind && also.index == int(index));
	};
	const threedi::ThreediCollisionModel *c = model.collision;

	// The bullet faces, each named by its surface (model_surface_words: the ammo effect a hit plays).
	const uint32_t face_rgb = model_collision_layer(ModelCollisionLayer::BulletFaces).rgb;
	for (size_t f = 0; c && f < g->faces.size() && f < c->face_count; ++f) {
		const FaceGeometry &face = g->faces[f];
		if (!face.valid || !wanted(ModelCollisionKind::Face, f, options.bullet_faces)) continue;
		ModelCollisionShape s;
		s.kind = ModelCollisionKind::Face;
		s.index = int(f);
		s.section = face.section;
		s.rgb = face_rgb;
		s.legend = "Bullet faces";
		s.pickable = true;
		s.name = "Face " + std::to_string(f + 1) + ": " + model_surface_label(c->faces[f].poly_type);
		if (c->faces[f].material_flags & 0x100) s.name += ", bullets pass";
		add_polygon(s, {pose(face.corner[0], face.section), pose(face.corner[1], face.section),
		                pose(face.corner[2], face.section)});
		out.push_back(std::move(s));
	}
	// The volumes: the solid of each one's planes (its box where they bound none), coloured by its type.
	for (size_t v = 0; v < g->volumes.size(); ++v) {
		const VolumeGeometry &volume = g->volumes[v];
		if (!wanted(ModelCollisionKind::Volume, v, options.volumes)) continue;
		ModelCollisionShape s;
		s.kind = ModelCollisionKind::Volume;
		s.index = int(v);
		s.section = volume.section;
		s.rgb = model_volume_rgb(volume.type);
		s.legend = std::string("Volumes: ") + model_volume_family_words(model_volume_type(volume.type).family);
		s.pickable = true;
		s.name = volume_name(v, volume.type);
		if (!volume.owned) s.name += " (no section holds it: the game never reads it)";
		// A box stored inside out (four of the install's volumes, Indonesian bases' and LFP towers' walls: z
		// above z): the quick test against it passes almost no point, so the game barely meets the solid.
		if (volume.min.x > volume.max.x || volume.min.y > volume.max.y || volume.min.z > volume.max.z)
			s.name += ", its box inside out (the game's quick test passes almost nothing)";
		if (volume.polygons.empty()) {
			s.name += ", its box (its planes bound no solid)";
			add_box(s, volume.min, volume.max, pose, volume.section, true);
		}
		for (const Polygon &polygon : volume.polygons) {
			std::vector<PreviewVec3> corners;
			for (const ThreediBuildVec3 &p : polygon.corners) corners.push_back(pose(p, volume.section));
			add_polygon(s, corners);
		}
		out.push_back(std::move(s));
	}
	// The sections: a rigid model's bound sphere and box, a person's hit sphere as a round meets it.
	const uint32_t section_rgb = model_collision_layer(ModelCollisionLayer::Sections).rgb;
	for (size_t o = 0; o < g->sections.size(); ++o) {
		const SectionGeometry &section = g->sections[o];
		if (!section.sphere || !wanted(ModelCollisionKind::Section, o, options.sections)) continue;
		ModelCollisionShape s;
		s.kind = ModelCollisionKind::Section;
		s.index = int(o);
		s.section = int(o);
		s.pickable = true;
		s.sphere = true;
		s.center = pose(section.center, int(o));
		s.breaks = section.breaks;
		if (section.person) {
			s.person = true;
			s.rgb = kModelHitSphereRgb;
			s.legend = "Hit spheres";
			s.radius = float(model_person_hit_radius_q16(int(o), section.radius_q16) / 65536.0);
			s.name = "Hit sphere of " + part_word(o) + (o == 14 ? " (the head)" : "");
		} else if (section.bone) {
			// A skinned model no person wears (a first-person view's arms): its stored sphere, which no round
			// tests (model_section_is_person).
			s.rgb = section_rgb;
			s.legend = "Bone spheres (no round tests them)";
			s.radius = float(section.radius);
			s.name = "Bone sphere of " + part_word(o) + " (no round tests it)";
		} else {
			s.rgb = section_rgb;
			s.legend = "Sections";
			s.radius = float(section.radius);
			s.name = "Section of " + part_word(o);
			add_box(s, section.min, section.max, pose, int(o), false);
		}
		out.push_back(std::move(s));
	}
	// The occlusion records' polygons (model axes: the draw's own space).
	const uint32_t occlusion_rgb = model_collision_layer(ModelCollisionLayer::Occlusion).rgb;
	for (size_t r = 0; g->has_occlusion && r < g->occlusion.records.size(); ++r) {
		if (!wanted(ModelCollisionKind::Occlusion, r, options.occlusion)) continue;
		const world::OcclusionPortalFace &record = g->occlusion.records[r];
		ModelCollisionShape s;
		s.kind = ModelCollisionKind::Occlusion;
		s.index = int(r);
		s.rgb = occlusion_rgb;
		s.legend = "Occlusion";
		s.pickable = true;
		s.name = "Occlusion " + std::to_string(r + 1) + ": " + model_occlusion_type_words(record.type);
		for (int32_t f = 0; f < record.face_count; ++f) {
			const world::OcclusionFaceRec &face = g->occlusion.faces[size_t(record.face_start + f)];
			std::vector<PreviewVec3> corners;
			for (int k = 0; k < 3; ++k)
				corners.push_back(model_point(g->occlusion.vertices[size_t(record.vert_start + face.v[k])].p, nullptr));
			add_polygon(s, corners);
		}
		out.push_back(std::move(s));
	}
	// The bounds the game derives: the collision block's box, the projection sphere, the bound radius.
	const Poser rest{nullptr};
	const uint32_t bounds_rgb = model_collision_layer(ModelCollisionLayer::Bounds).rgb;
	if (options.bounds && c) {
		if (g->has_bounds) {
			ModelCollisionShape box;
			box.kind = ModelCollisionKind::Bounds;
			box.rgb = bounds_rgb;
			box.legend = "Collision box, projection sphere";
			box.name = "Collision box";
			add_box(box, g->box_min, g->box_max, rest, -1, false);
			out.push_back(std::move(box));
		}
		ModelCollisionShape sphere;
		sphere.kind = ModelCollisionKind::ProjectionSphere;
		sphere.rgb = bounds_rgb;
		sphere.legend = "Collision box, projection sphere";
		sphere.name = "Projection sphere";
		sphere.sphere = true;
		sphere.center = rest(g->sphere_center, -1);
		sphere.radius = float(g->sphere_radius);
		out.push_back(std::move(sphere));
		ModelCollisionShape bound;
		bound.kind = ModelCollisionKind::BoundRadius;
		bound.rgb = 0x8BC34A;
		bound.legend = "Bound radius";
		bound.name = "Bound radius";
		bound.sphere = true;
		bound.radius = float(g->bound_radius);
		out.push_back(std::move(bound));
	}
	// A vehicle's probe boxes: the box, then the footprint on its floor.
	if (options.probe_boxes && g->has_probes) {
		const threedi::ThreediCollisionProbeBoxes &p = g->probes;
		const uint32_t probe_rgb = model_collision_layer(ModelCollisionLayer::ProbeBoxes).rgb;
		ModelCollisionShape box;
		box.kind = ModelCollisionKind::ProbeBox;
		box.rgb = probe_rgb;
		box.legend = "Probe boxes";
		box.name = "Probe box";
		add_box(box, ThreediBuildVec3{q16(p.box_x_lo), q16(p.box_y_lo), q16(p.box_z_lo)},
		        ThreediBuildVec3{q16(p.box_x_hi), q16(p.box_y_hi), q16(p.box_z_hi)}, rest, -1, false);
		out.push_back(std::move(box));
		ModelCollisionShape foot;
		foot.kind = ModelCollisionKind::ProbeBox;
		foot.index = 1;
		foot.rgb = probe_rgb;
		foot.legend = "Probe boxes";
		foot.name = "Probe footprint";
		const double z = q16(p.box_z_lo);
		std::vector<PreviewVec3> corners = {rest(ThreediBuildVec3{q16(p.foot_x_lo), q16(p.foot_y_lo), z}, -1),
		                                    rest(ThreediBuildVec3{q16(p.foot_x_hi), q16(p.foot_y_lo), z}, -1),
		                                    rest(ThreediBuildVec3{q16(p.foot_x_hi), q16(p.foot_y_hi), z}, -1),
		                                    rest(ThreediBuildVec3{q16(p.foot_x_lo), q16(p.foot_y_hi), z}, -1)};
		for (size_t i = 0; i < corners.size(); ++i) add_edge(foot, corners[i], corners[(i + 1) % corners.size()]);
		out.push_back(std::move(foot));
	}
	// The drawn LOD's part spheres, each riding its part there (a pivot's rule).
	if (options.part_spheres && model.lods && lod >= 0 && size_t(lod) < model.lod_count) {
		const threedi::ThreediLod &level = model.lods[lod];
		std::vector<ThreediMatrix4x4> drawn;
		std::vector<uint8_t> drawn_driven;
		threedi::threedi_panm_pose_parts(model, lod, time_ms, bus, drawn, &drawn_driven);
		const uint32_t part_rgb = model_collision_layer(ModelCollisionLayer::PartSpheres).rgb;
		for (size_t p = 0; p < level.render_object_count; ++p) {
			const threedi::ThreediRenderObject &part = level.render_objects[p];
			ModelCollisionShape s;
			s.kind = ModelCollisionKind::PartSphere;
			s.index = int(p);
			s.rgb = part_rgb;
			s.legend = "Part spheres";
			s.name = "Part sphere of " + part_word(p);
			s.sphere = true;
			const float at[3] = {part.abs[0] + part.bounding_center[0], part.abs[1] + part.bounding_center[1],
			                     part.abs[2] + part.bounding_center[2]};
			// A part a PANM node drives turns about its pivot; one none drives stands as it is (the Poser's
			// rule).
			const bool moves = p < drawn.size() && p < drawn_driven.size() && drawn_driven[p];
			s.center = model_point(at, moves ? &drawn[p] : nullptr);
			s.radius = part.bounding_radius;
			out.push_back(std::move(s));
		}
	}
	return out;
}

std::vector<ModelCollisionLegendRow> model_collision_legend(const std::vector<ModelCollisionShape> &shapes) {
	std::vector<ModelCollisionLegendRow> out;
	const auto add = [&](uint32_t rgb, const std::string &words) {
		for (const ModelCollisionLegendRow &row : out)
			if (row.rgb == rgb && row.words == words) return;
		out.push_back({rgb, words});
	};
	for (const ModelCollisionShape &s : shapes) add(s.rgb, s.legend);
	return out;
}

int pick_model_collision(const std::vector<ModelCollisionShape> &shapes, const OrbitCamera &camera, int width,
                         int height, float x, float y) {
	int best = -1;
	float best_depth = 0.0f;
	for (size_t i = 0; i < shapes.size(); ++i) {
		const ModelCollisionShape &s = shapes[i];
		if (!s.pickable) continue;
		for (size_t t = 0; t + 2 < s.triangles.size(); t += 3) {
			float px[3], py[3], depth[3];
			bool seen = true;
			for (int k = 0; k < 3 && seen; ++k) seen = camera.project(s.triangles[t + size_t(k)], width, height, px[k], py[k], &depth[k]);
			if (!seen) continue;
			// The pixel on the triangle's side of each edge (either winding).
			const auto side = [&](int a, int b) {
				return (px[b] - px[a]) * (y - py[a]) - (py[b] - py[a]) * (x - px[a]);
			};
			const float d0 = side(0, 1), d1 = side(1, 2), d2 = side(2, 0);
			const bool inside = (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
			if (!inside || (d0 == 0 && d1 == 0 && d2 == 0)) continue;
			const float at = (depth[0] + depth[1] + depth[2]) / 3.0f;
			if (best < 0 || at < best_depth) {
				best = int(i);
				best_depth = at;
			}
		}
	}
	if (best >= 0) return best;
	// No area under the pixel: the smallest sphere whose disc holds it.
	float best_radius = 0.0f;
	for (size_t i = 0; i < shapes.size(); ++i) {
		const ModelCollisionShape &s = shapes[i];
		if (!s.pickable || !s.sphere || !s.triangles.empty()) continue;
		float cx = 0.0f, cy = 0.0f, depth = 0.0f;
		if (!camera.project(s.center, width, height, cx, cy, &depth) || !(depth > 0.0f)) continue;
		const float reach = s.radius * camera.focal(width) / depth;
		if (std::hypot(cx - x, cy - y) > reach) continue;
		if (best < 0 || reach < best_radius) {
			best = int(i);
			best_radius = reach;
		}
	}
	return best;
}

void model_collision_bounds(const ModelCollisionShape &shape, PreviewVec3 &center, float &radius) {
	if (shape.sphere && shape.edges.empty()) {
		center = shape.center;
		radius = shape.radius;
		return;
	}
	const std::vector<PreviewVec3> &points = shape.edges.empty() ? shape.triangles : shape.edges;
	PreviewVec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	for (const PreviewVec3 &p : points) {
		lo = PreviewVec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
		hi = PreviewVec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
	}
	if (points.empty()) {
		center = shape.center;
		radius = std::max(shape.radius, 0.05f);
		return;
	}
	center = PreviewVec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
	radius = 0.5f * std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) + (hi.z - lo.z) * (hi.z - lo.z));
	if (shape.sphere) radius = std::max(radius, shape.radius);
	radius = std::max(radius, 0.05f);
}

namespace {

// The collision row's list a kind's records are in, and their record kind.
bool list_of(ModelCollisionKind kind, size_t &list, NodeKind &record) {
	switch (kind) {
	case ModelCollisionKind::Section: list = kCollisionSections; record = node_kind(ModelKind::Section); return true;
	case ModelCollisionKind::Volume: list = kCollisionVolumes; record = node_kind(ModelKind::Volume); return true;
	case ModelCollisionKind::Face: list = kCollisionFaces; record = node_kind(ModelKind::Face); return true;
	case ModelCollisionKind::Occlusion: list = kCollisionOcclusion; record = node_kind(ModelKind::Occlusion); return true;
	default: return false;
	}
}

} // namespace

NodeAddress model_collision_record(const ModelDocument &document, const ModelCollisionShape &shape) {
	const CollisionRow *row = document.collision_row();
	size_t list = 0;
	NodeKind kind = 0;
	if (!row || !list_of(shape.kind, list, kind) || list >= row->ids.lists.size() || shape.index < 0 ||
	    size_t(shape.index) >= row->ids.lists[list].size())
		return NodeAddress();
	return NodeAddress{row->id, kind, row->ids.lists[list][size_t(shape.index)].id};
}

bool model_collision_of(const ModelDocument &document, const NodeAddress &record, ModelCollisionPick &out) {
	const CollisionRow *row = document.collision_row();
	if (!row || record.row != row->id || !record.child) return false;
	for (const ModelCollisionKind kind : {ModelCollisionKind::Section, ModelCollisionKind::Volume, ModelCollisionKind::Face,
	                                      ModelCollisionKind::Occlusion}) {
		size_t list = 0;
		NodeKind node = 0;
		if (!list_of(kind, list, node) || node != record.kind || list >= row->ids.lists.size()) continue;
		const std::vector<RecordIds> &ids = row->ids.lists[list];
		for (size_t i = 0; i < ids.size(); ++i)
			if (ids[i].id == record.child) {
				out.kind = kind;
				out.index = int(i);
				return true;
			}
	}
	return false;
}

std::vector<std::vector<ThreediBuildVec3>> model_volume_polygons(const threedi::ThreediBoundingPlane *planes,
                                                                 size_t count, bool ladder) {
	std::vector<std::vector<ThreediBuildVec3>> out;
	if (!planes || count < 3) return out;
	struct Plane {
		double n[3];
		double d;
	};
	std::vector<Plane> p(count);
	for (size_t i = 0; i < count; ++i) p[i] = Plane{{planes[i].normal[0], planes[i].normal[1], planes[i].normal[2]}, planes[i].radius};
	// Every point three planes meet in that lies within 1 mm of the solid (retail's stored planes meet a
	// few tenths of a millimetre off where four or more should).
	struct Corner {
		ThreediBuildVec3 at;
		size_t planes[3];
		double excess;
	};
	std::vector<Corner> corners;
	for (size_t i = 0; i < count; ++i)
		for (size_t j = i + 1; j < count; ++j)
			for (size_t k = j + 1; k < count; ++k) {
				const Plane &a = p[i], &b = p[j], &c = p[k];
				const double det = a.n[0] * (b.n[1] * c.n[2] - b.n[2] * c.n[1]) - a.n[1] * (b.n[0] * c.n[2] - b.n[2] * c.n[0]) +
				                   a.n[2] * (b.n[0] * c.n[1] - b.n[1] * c.n[0]);
				if (std::fabs(det) < 1e-9) continue;
				const double r[3] = {-a.d, -b.d, -c.d};
				const double x = (r[0] * (b.n[1] * c.n[2] - b.n[2] * c.n[1]) - a.n[1] * (r[1] * c.n[2] - b.n[2] * r[2]) +
				                  a.n[2] * (r[1] * c.n[1] - b.n[1] * r[2])) / det;
				const double y = (a.n[0] * (r[1] * c.n[2] - b.n[2] * r[2]) - r[0] * (b.n[0] * c.n[2] - b.n[2] * c.n[0]) +
				                  a.n[2] * (b.n[0] * r[2] - r[1] * c.n[0])) / det;
				const double z = (a.n[0] * (b.n[1] * r[2] - r[1] * c.n[1]) - a.n[1] * (b.n[0] * r[2] - r[1] * c.n[0]) +
				                  r[0] * (b.n[0] * c.n[1] - b.n[1] * c.n[0])) / det;
				double excess = -1e30;
				for (const Plane &q : p) excess = std::max(excess, q.n[0] * x + q.n[1] * y + q.n[2] * z + q.d);
				if (excess <= 1e-3) corners.push_back(Corner{ThreediBuildVec3{x, y, z}, {i, j, k}, excess});
			}
	const auto distance2 = [](const ThreediBuildVec3 &a, const ThreediBuildVec3 &b) {
		return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
	};
	std::vector<std::pair<size_t, std::vector<ThreediBuildVec3>>> facets;
	for (size_t pi = 0; pi < count; ++pi) {
		// The corners on this plane, each once, wound about its normal and each sub-millimetre cluster
		// collapsed to the corner deepest inside.
		std::vector<std::pair<ThreediBuildVec3, double>> on;
		for (const Corner &corner : corners) {
			if (corner.planes[0] != pi && corner.planes[1] != pi && corner.planes[2] != pi) continue;
			bool seen = false;
			for (const auto &o : on) seen = seen || distance2(o.first, corner.at) < 1e-12;
			if (!seen) on.push_back({corner.at, corner.excess});
		}
		if (on.size() < 3) continue;
		ThreediBuildVec3 middle;
		for (const auto &o : on) middle = ThreediBuildVec3{middle.x + o.first.x / on.size(), middle.y + o.first.y / on.size(), middle.z + o.first.z / on.size()};
		ThreediBuildVec3 u;
		double far = -1.0;
		for (const auto &o : on) {
			const ThreediBuildVec3 e{o.first.x - middle.x, o.first.y - middle.y, o.first.z - middle.z};
			const double l = e.x * e.x + e.y * e.y + e.z * e.z;
			if (l > far) {
				far = l;
				u = e;
			}
		}
		const double ul = std::sqrt(far);
		if (ul <= 1e-9) continue;
		u = ThreediBuildVec3{u.x / ul, u.y / ul, u.z / ul};
		const double *n = p[pi].n;
		const ThreediBuildVec3 w{n[1] * u.z - n[2] * u.y, n[2] * u.x - n[0] * u.z, n[0] * u.y - n[1] * u.x};
		std::sort(on.begin(), on.end(), [&](const auto &a, const auto &b) {
			const auto angle = [&](const ThreediBuildVec3 &q) {
				const ThreediBuildVec3 e{q.x - middle.x, q.y - middle.y, q.z - middle.z};
				return std::atan2(e.x * w.x + e.y * w.y + e.z * w.z, e.x * u.x + e.y * u.y + e.z * u.z);
			};
			return angle(a.first) < angle(b.first);
		});
		std::vector<std::vector<std::pair<ThreediBuildVec3, double>>> clusters;
		for (const auto &o : on) {
			if (!clusters.empty() && distance2(o.first, clusters.back().back().first) < 1e-6) clusters.back().push_back(o);
			else clusters.push_back({o});
		}
		if (clusters.size() > 1 && distance2(clusters.front().front().first, clusters.back().back().first) < 1e-6) {
			std::vector<std::pair<ThreediBuildVec3, double>> merged = clusters.back();
			merged.insert(merged.end(), clusters.front().begin(), clusters.front().end());
			clusters.front() = std::move(merged);
			clusters.pop_back();
		}
		std::vector<ThreediBuildVec3> polygon;
		for (const auto &cluster : clusters)
			polygon.push_back(std::min_element(cluster.begin(), cluster.end(),
			                                   [](const auto &a, const auto &b) { return a.second < b.second; })
			                          ->first);
		double area = 0.0;
		for (size_t a = 1; a + 1 < polygon.size(); ++a) {
			const ThreediBuildVec3 e1{polygon[a].x - polygon[0].x, polygon[a].y - polygon[0].y, polygon[a].z - polygon[0].z};
			const ThreediBuildVec3 e2{polygon[a + 1].x - polygon[0].x, polygon[a + 1].y - polygon[0].y,
			                          polygon[a + 1].z - polygon[0].z};
			const ThreediBuildVec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
			area += 0.5 * std::sqrt(cr.x * cr.x + cr.y * cr.y + cr.z * cr.z);
		}
		if (polygon.size() >= 3 && area > 1e-8) facets.push_back({pi, std::move(polygon)});
	}
	// A solid has four facets or more; a flat ladder keeps its facing's (plane 0's) alone.
	if (facets.size() < 4) {
		if (ladder)
			for (auto &facet : facets)
				if (facet.first == 0) out.push_back(std::move(facet.second));
		return out;
	}
	for (auto &facet : facets) out.push_back(std::move(facet.second));
	return out;
}

std::vector<std::vector<ThreediBuildVec3>> model_volume_solid(const threedi::ThreediBoundingVolume &volume,
                                                              const threedi::ThreediBoundingPlane *planes) {
	const size_t count = volume.plane_count > 0 ? size_t(volume.plane_count) : 0;
	const bool ladder = volume.collidable_type == 4;
	std::vector<std::vector<ThreediBuildVec3>> own = model_volume_polygons(planes, count, ladder);
	const double lo[3] = {q16(volume.min_x_fp16), q16(volume.min_y_fp16), q16(volume.min_z_fp16)};
	const double hi[3] = {q16(volume.max_x_fp16), q16(volume.max_y_fp16), q16(volume.max_z_fp16)};
	bool past = false;
	for (const auto &polygon : own)
		for (const ThreediBuildVec3 &p : polygon) {
			const double at[3] = {p.x, p.y, p.z};
			for (int k = 0; k < 3; ++k) past = past || at[k] < lo[k] - 0.002 || at[k] > hi[k] + 0.002;
		}
	if (!past || !(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2])) return own;
	// The planes and the box's six (n . p + d <= 0 inside, as the volume's own).
	std::vector<threedi::ThreediBoundingPlane> clipped(planes, planes + count);
	for (int k = 0; k < 3; ++k)
		for (const int side : {1, -1}) {
			threedi::ThreediBoundingPlane plane{};
			plane.normal[k] = float(side);
			plane.radius = float(side > 0 ? -hi[k] : lo[k]);
			clipped.push_back(plane);
		}
	return model_volume_polygons(clipped.data(), clipped.size(), ladder);
}
} // namespace opennova::editor
