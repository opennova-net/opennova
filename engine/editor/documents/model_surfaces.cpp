// A model's bullet-face surfaces on its materials (model_surfaces.h).

#include <editor/documents/model_surfaces.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

#include <editor/documents/model_document.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_strip_decode.h>
#include <runtime/world/ammo_table.h>

namespace opennova::editor {

using namespace threedi;

namespace {

constexpr NodeKind kMaterial = node_kind(ModelKind::Material);
constexpr NodeKind kFace = node_kind(ModelKind::Face);
// The collision row's lists (CollisionRow): 0 sections, 1 volumes, 2 faces, 3 occlusion records.
constexpr size_t kCollisionFaces = 2;

// The game's table plays a face byte's surface as row `byte + 4`.
constexpr int kTagOffset = 4;

// A modder's word for each surface, by its face byte (0 to 23: rows 4 to 27 of the tag table, in the
// words the tools share, world::kImpactEffectTagWords).
const char *surface_name(int64_t poly_type) { return world::kImpactEffectTagWords[poly_type + kTagOffset]; }
static_assert(kModelSurfaceCount + kTagOffset == world::kImpactEffectTagCount, "a face byte names rows 4 to 27");

} // namespace

ModelSurfaceWords model_surface_words(int64_t poly_type) {
	ModelSurfaceWords out;
	const int64_t tag = poly_type + kTagOffset;
	if (poly_type < 0 || tag >= world::kImpactEffectTagCount) {
		// [orig: AmmoDef_ProcessImpactEffect @ 0x40a170 clamps a tag of 28 or more to 4 @ 0x40a1bf]
		out.name = "Object (" + std::to_string(poly_type) + ")";
		out.tag = world::kImpactEffectTagNames[kTagOffset];
		out.note = "Past the game's surface table: a hit plays the obj row.";
		out.known = false;
		return out;
	}
	out.name = surface_name(poly_type);
	out.tag = world::kImpactEffectTagNames[tag];
	switch (poly_type) {
	case 15:
		// [orig: Projectile_HandleEntityImpact @ 0x4e9390, the section break @ 0x4e964f..0x4e9684;
		// docs/world/world-wac-ai-re.md §15.8]
		out.note = "On a building, a round that hits it breaks the face's section (the pane goes) with the glass "
		           "smash sound.";
		break;
	case 17:
		// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0: material 17 skipped for ammo flagged
		// 0x4000000, IgnorFoilage]
		out.note = "Rounds whose ammo is flagged IgnorFoilage pass through it.";
		break;
	case 19:
		// [orig: Projectile_HandleEntityImpact @ 0x4e9390, the dead-victim gate @ 0x4e95db..0x4e95e0]
		out.note = "On a dead victim a hit plays nothing.";
		break;
	default: break;
	}
	return out;
}

const std::vector<FieldChoice> &model_surface_choices() {
	static const std::vector<FieldChoice> choices = [] {
		std::vector<FieldChoice> out;
		for (int i = 0; i < kModelSurfaceCount; ++i)
			out.push_back({world::kImpactEffectTagNames[i + kTagOffset], i, surface_name(i)});
		return out;
	}();
	return choices;
}

const std::vector<ModelFaceFlag> &model_face_flags() {
	static const std::vector<ModelFaceFlag> flags = {
		{0x1, "both_sides", "Both sides", "A round stops at the face from either side."},
		{0x100, "bullets_pass", "Bullets pass",
		 "Rounds pass through the face: the game's face test skips it (retail's rotor blades)."},
		{0x800, "front_only", "Front only",
		 "Without Both sides, a round stops at the face only crossing it from the front."},
	};
	return flags;
}

// --- the faces' materials ----------------------------------------------------------------------------

namespace {

struct Point {
	double x = 0.0, y = 0.0, z = 0.0;
};

// A face's middle and a triangle's are compared on a grid of 1/128 m: the nearest within 1/64 m of the
// face's, looked for in the cell and its neighbours. The collision corners sit on the 8.8 grid (within
// 1/512 m of the render corners they were taken from), so a face's middle is well within it.
constexpr double kCell = 128.0;
constexpr double kNear = 1.0 / 64.0;

uint64_t cell_key(int64_t x, int64_t y, int64_t z) {
	const auto pack = [](int64_t v) { return static_cast<uint64_t>(v + (int64_t(1) << 20)) & 0x1FFFFF; };
	return (pack(x) << 42) | (pack(y) << 21) | pack(z);
}
int64_t cell_of(double v) { return static_cast<int64_t>(std::floor(v * kCell)); }

struct Triangle {
	Point middle;
	int material = -1;
};

// The middles of a LOD's triangles, by cell, in mission axes (the collision vertices' frame on disk).
struct TriangleGrid {
	std::vector<Triangle> triangles;
	std::unordered_map<uint64_t, std::vector<uint32_t>> cells;

	void add(const Triangle &t) {
		cells[cell_key(cell_of(t.middle.x), cell_of(t.middle.y), cell_of(t.middle.z))].push_back(
				static_cast<uint32_t>(triangles.size()));
		triangles.push_back(t);
	}
	// The material of the triangle whose middle is nearest `p` within kNear, -1 for none.
	int material_at(const Point &p) const {
		const int64_t cx = cell_of(p.x), cy = cell_of(p.y), cz = cell_of(p.z);
		double best = kNear * kNear;
		int material = -1;
		for (int64_t dx = -1; dx <= 1; ++dx)
			for (int64_t dy = -1; dy <= 1; ++dy)
				for (int64_t dz = -1; dz <= 1; ++dz) {
					const auto found = cells.find(cell_key(cx + dx, cy + dy, cz + dz));
					if (found == cells.end()) continue;
					for (const uint32_t i : found->second) {
						const Point &m = triangles[i].middle;
						const double d = (m.x - p.x) * (m.x - p.x) + (m.y - p.y) * (m.y - p.y) + (m.z - p.z) * (m.z - p.z);
						if (d <= best) {
							best = d;
							material = triangles[i].material;
						}
					}
				}
		return material;
	}
};

void lod_triangles(const Threedi3di3 &model, const ThreediLod &lod, TriangleGrid &grid) {
	std::vector<uint16_t> tris;
	for (size_t s = 0; s < lod.strip_count; ++s) {
		const ThreediTriangleStrip &strip = lod.strips[s];
		if (!threedi_decode_strip_indices(lod, strip, tris)) continue;
		const int material = threedi_material_array_index_for_id(model, strip.material_index);
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			Point middle;
			for (int k = 0; k < 3; ++k) {
				const float *p = lod.vertices.items[strip.start_vertex + tris[t + k]].position;
				const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{p[0], p[1], p[2]});
				middle.x += m.x / 3.0;
				middle.y += m.y / 3.0;
				middle.z += m.z / 3.0;
			}
			grid.add({middle, material});
		}
	}
}

std::shared_ptr<const ModelFaceMaterials> make_face_materials(const Threedi3di3 &model) {
	auto out = std::make_shared<ModelFaceMaterials>();
	const ThreediCollisionModel *c = model.collision;
	if (!c || c->face_count == 0) return out;
	out->material.assign(c->face_count, -1);
	// Each face's middle, through its section's vertex run (the prefix sums the loader takes).
	std::vector<ThreediCollisionObjectRun> runs(c->object_count);
	if (!threedi_collision_object_runs(c, runs.data())) return out;
	std::vector<Point> middles(c->face_count);
	std::vector<bool> valid(c->face_count, false);
	for (size_t o = 0; o < c->object_count; ++o) {
		const ThreediCollisionObject &object = c->objects[o];
		for (int32_t f = 0; f < object.num_faces; ++f) {
			const size_t face = static_cast<size_t>(runs[o].face_start + f);
			const ThreediCollisionFace &cf = c->faces[face];
			Point m;
			bool inside = true;
			for (int k = 0; k < 3; ++k) {
				const int32_t corner = cf.vert_index[k];
				if (corner < 0 || corner >= object.num_vertices) {
					inside = false;
					break;
				}
				const float *p = c->vertices[static_cast<size_t>(runs[o].vertex_start + corner)].position;
				m.x += p[0] / 3.0;
				m.y += p[1] / 3.0;
				m.z += p[2] / 3.0;
			}
			middles[face] = m;
			valid[face] = inside;
		}
	}
	// The LOD whose triangles meet the most faces (the first of a tie): the collision LOD.
	for (size_t l = 0; l < model.lod_count; ++l) {
		TriangleGrid grid;
		lod_triangles(model, model.lods[l], grid);
		if (grid.triangles.empty()) continue;
		std::vector<int> material(c->face_count, -1);
		size_t matched = 0;
		for (size_t f = 0; f < c->face_count; ++f) {
			if (!valid[f]) continue;
			material[f] = grid.material_at(middles[f]);
			if (material[f] >= 0) ++matched;
		}
		if (matched > out->matched) {
			out->matched = matched;
			out->lod = static_cast<int>(l);
			out->material = std::move(material);
		}
		if (matched == c->face_count) break;
	}
	return out;
}

} // namespace

std::shared_ptr<const ModelFaceMaterials> model_face_materials(const assets::Model &base) {
	static std::mutex mutex;
	static std::map<const Threedi3di3 *, std::pair<std::weak_ptr<const Threedi3di3>, std::shared_ptr<const ModelFaceMaterials>>>
			cache;
	if (!base) return std::make_shared<ModelFaceMaterials>();
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = cache.find(base.get());
		if (found != cache.end() && found->second.first.lock() == base) return found->second.second;
	}
	std::shared_ptr<const ModelFaceMaterials> made = make_face_materials(*base);
	std::lock_guard<std::mutex> lock(mutex);
	for (auto it = cache.begin(); it != cache.end();) it = it->second.first.expired() ? cache.erase(it) : std::next(it);
	cache[base.get()] = {base, made};
	return made;
}

// --- a material's surface ----------------------------------------------------------------------------

namespace {

// The model row, the collision row and the base index (its MTRL row) of the material `material` names;
// false for a record that is no material of the document.
struct MaterialPlace {
	const ModelRow *model = nullptr;
	const CollisionRow *collision = nullptr;
	int source = -1;
};
bool material_place(const ModelDocument &document, NodeId material, MaterialPlace &out) {
	out.model = document.model_row();
	out.collision = document.collision_row();
	if (!out.model || !out.collision) return false;
	const std::vector<RecordIds> &ids = out.model->ids.lists[kModelMaterials];
	for (size_t i = 0; i < ids.size() && i < out.model->materials.size(); ++i)
		if (ids[i].id == material) {
			out.source = out.model->materials[i].source;
			return true;
		}
	return false;
}

// The collision faces (their indices) a base material made.
std::vector<size_t> faces_of(const MaterialPlace &place) {
	std::vector<size_t> out;
	if (place.source < 0) return out;
	const std::shared_ptr<const ModelFaceMaterials> made = model_face_materials(place.model->base);
	const size_t count = std::min(made->material.size(), place.collision->faces.size());
	for (size_t f = 0; f < count; ++f)
		if (made->material[f] == place.source) out.push_back(f);
	return out;
}

NodeAddress face_address(const CollisionRow &collision, size_t face) {
	const std::vector<RecordIds> &ids = collision.ids.lists[kCollisionFaces];
	return face < ids.size() ? NodeAddress{collision.id, kFace, ids[face].id} : NodeAddress{};
}

} // namespace

bool model_material_surface(const ModelDocument &document, NodeId material, ModelMaterialSurface &out) {
	out = ModelMaterialSurface();
	MaterialPlace place;
	if (!material_place(document, material, place)) return false;
	const std::vector<size_t> faces = faces_of(place);
	out.faces = faces.size();
	std::map<int64_t, size_t> counts;
	for (const ModelFaceFlag &flag : model_face_flags()) out.flags.push_back({flag.bit, 0});
	for (const size_t f : faces) {
		const ThreediCollisionFace &face = place.collision->faces[f];
		++counts[face.poly_type];
		for (ModelFlagCount &flag : out.flags)
			if (face.material_flags & flag.bit) ++flag.on;
	}
	for (const auto &[surface, count] : counts) out.surfaces.push_back({surface, count});
	std::stable_sort(out.surfaces.begin(), out.surfaces.end(),
	                 [](const ModelSurfaceCount &a, const ModelSurfaceCount &b) { return a.faces > b.faces; });
	if (out.mixed())
		for (const size_t f : faces)
			if (place.collision->faces[f].poly_type != out.common()) out.differing.push_back(f);
	return true;
}

std::string model_material_surface_words(const ModelMaterialSurface &surface) {
	if (surface.faces == 0) return "No bullet faces";
	if (!surface.mixed()) return model_surface_words(surface.common()).name;
	std::string out = "Mixed:";
	for (size_t i = 0; i < surface.surfaces.size(); ++i) {
		if (i == 3) {
			out += ", ...";
			break;
		}
		out += (i ? ", " : " ") + std::to_string(surface.surfaces[i].faces) + " " +
		       model_surface_words(surface.surfaces[i].surface).name;
	}
	return out;
}

size_t model_faces_without_material(const ModelDocument &document) {
	const ModelRow *model = document.model_row();
	const CollisionRow *collision = document.collision_row();
	if (!model || !collision) return 0;
	const std::shared_ptr<const ModelFaceMaterials> made = model_face_materials(model->base);
	return collision->faces.size() - std::min(made->matched, collision->faces.size());
}

namespace {

bool plan_faces(const MaterialPlace &place, const std::vector<size_t> &faces, std::string &refusal) {
	if (place.source < 0) {
		refusal = "This material was added here: no bullet face is made from it (export the model from Blender to "
		          "give it faces).";
		return false;
	}
	if (faces.empty()) {
		refusal = "No bullet face is made from this material.";
		return false;
	}
	return true;
}

} // namespace

bool model_surface_edits(const ModelDocument &document, NodeId material, int64_t surface, std::vector<Edit> &out,
                         std::string &refusal) {
	out.clear();
	MaterialPlace place;
	if (!material_place(document, material, place)) {
		refusal = "That is no material of the model.";
		return false;
	}
	if (surface < 0 || surface > 255) {
		refusal = "A surface is a byte, 0 to 255.";
		return false;
	}
	const std::vector<size_t> faces = faces_of(place);
	if (!plan_faces(place, faces, refusal)) return false;
	for (const size_t f : faces) {
		if (place.collision->faces[f].poly_type == surface) continue;
		Edit set;
		set.address = face_address(*place.collision, f);
		set.field = "poly_type";
		set.value = surface;
		out.push_back(std::move(set));
	}
	return true;
}

bool model_face_flag_edits(const ModelDocument &document, NodeId material, uint32_t bit, bool on, std::vector<Edit> &out,
                           std::string &refusal) {
	out.clear();
	MaterialPlace place;
	if (!material_place(document, material, place)) {
		refusal = "That is no material of the model.";
		return false;
	}
	const auto &flags = model_face_flags();
	if (std::none_of(flags.begin(), flags.end(), [&](const ModelFaceFlag &f) { return f.bit == bit; })) {
		refusal = "That is no bullet-face flag a material sets (both sides 1, bullets pass 0x100, front only 0x800).";
		return false;
	}
	const std::vector<size_t> faces = faces_of(place);
	if (!plan_faces(place, faces, refusal)) return false;
	for (const size_t f : faces) {
		const uint32_t now = place.collision->faces[f].material_flags;
		const uint32_t next = on ? (now | bit) : (now & ~bit);
		if (next == now) continue;
		Edit set;
		set.address = face_address(*place.collision, f);
		set.field = "flags";
		set.value = int64_t(next);
		out.push_back(std::move(set));
	}
	return true;
}

std::vector<NodeAddress> model_differing_faces(const ModelDocument &document, NodeId material) {
	std::vector<NodeAddress> out;
	ModelMaterialSurface surface;
	if (!model_material_surface(document, material, surface)) return out;
	for (const size_t f : surface.differing) out.push_back(face_address(*document.collision_row(), f));
	return out;
}

NodeAddress model_face_material(const ModelDocument &document, NodeId face) {
	const ModelRow *model = document.model_row();
	const CollisionRow *collision = document.collision_row();
	if (!model || !collision) return {};
	const std::vector<RecordIds> &faces = collision->ids.lists[kCollisionFaces];
	size_t index = faces.size();
	for (size_t f = 0; f < faces.size(); ++f)
		if (faces[f].id == face) index = f;
	const std::shared_ptr<const ModelFaceMaterials> made = model_face_materials(model->base);
	if (index >= made->material.size() || made->material[index] < 0) return {};
	const int source = made->material[index];
	const std::vector<RecordIds> &ids = model->ids.lists[kModelMaterials];
	for (size_t i = 0; i < model->materials.size() && i < ids.size(); ++i)
		if (model->materials[i].source == source) return {model->id, kMaterial, ids[i].id};
	return {};
}

} // namespace opennova::editor
