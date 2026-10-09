// A model's bullet-face surfaces on its materials (model_surfaces.h).

#include <editor/documents/model_surfaces.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// The game's table plays a face byte's surface as row `byte + 4`.
constexpr int kTagOffset = 4;

// A modder's word for each surface, by its face byte (0 to 23: rows 4 to 27 of the tag table, in the
// words the tools share, world::kImpactEffectTagWords).
const char *surface_name(int64_t poly_type) { return world::kImpactEffectTagWords[poly_type + kTagOffset]; }
static_assert(kModelSurfaceCount + kTagOffset == world::kImpactEffectTagCount, "a face byte names rows 4 to 27");

// The surfaces a round goes on through, and the energy each costs it (Q16): the game's damage step returns
// 0 for these five, so the round's lifetime is kept [orig: Projectile_ProcessDamageOnTarget @ 0x4e8220, the
// compares @ 0x4e823f..0x4e8266], and the impact charges the table's cost against the round's v^2 * mass,
// releasing it when that runs out [orig: the cost table @ 0x82d034 {19: 10, 16: 4, 15: 10, 17: 8, 7: 4};
// Entity_ClampKineticEnergy @ 0x4e9070 from Projectile_HandleEntityImpact @ 0x4e9643;
// docs/world/world-wac-ai-re.md section 15.8, the continuation correction]: world::material_energy_cost,
// nonzero for exactly these five.
std::string passes_words(int32_t cost_q16) {
	char cost[16];
	std::snprintf(cost, sizeof(cost), "%g", cost_q16 / 65536.0);
	return std::string("Rounds go on through it, paying ") + cost +
	       " of their energy (a round left with none stops in it).";
}

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
	if (const int32_t cost_q16 = world::material_energy_cost(static_cast<uint8_t>(poly_type))) {
		out.passes = true;
		out.energy_cost = cost_q16 / 65536.0;
		out.note = passes_words(cost_q16);
	}
	const auto also = [&](const char *words) { out.note += (out.note.empty() ? "" : " ") + std::string(words); };
	switch (poly_type) {
	case 15:
		// [orig: Projectile_HandleEntityImpact @ 0x4e9390, the section break @ 0x4e964f..0x4e9684;
		// docs/world/world-wac-ai-re.md §15.8]
		also("On a building the hit breaks the face's section (the pane goes) with the glass smash sound.");
		break;
	case 17:
		// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0: material 17 skipped for ammo flagged
		// 0x4000000, IgnorFoilage, @ 0x4e5062..0x4e5073: no hit, so no effect and no cost]
		also("Ammo flagged IgnorFoilage does not meet it at all: no effect, no cost.");
		break;
	case 19:
		// [orig: Projectile_HandleEntityImpact @ 0x4e9390, the dead-victim gate @ 0x4e95db..0x4e95e0]
		also("On a dead victim a hit plays nothing.");
		break;
	default: break;
	}
	return out;
}

std::string model_surface_label(int64_t poly_type) {
	const ModelSurfaceWords words = model_surface_words(poly_type);
	return words.passes ? words.name + " (rounds pass)" : words.name;
}

const std::vector<FieldChoice> &model_surface_choices() {
	static const std::vector<FieldChoice> choices = [] {
		std::vector<FieldChoice> out;
		for (int i = 0; i < kModelSurfaceCount; ++i)
			out.push_back({world::kImpactEffectTagNames[i + kTagOffset], i, model_surface_label(i)});
		return out;
	}();
	return choices;
}

// [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0: the 0x100 skip @ 0x4e5055; the direction rule
// @ 0x4e5115..0x4e5139: flag 1 takes the face from either side, else with the back-face argument set a
// face is taken from either side unless 0x800, which takes it only entering from its front (the side its
// normal faces); every caller passes that argument as 1 (Projectile_UpdatePhysics @ 0x4ea4ee, and each of
// Projectile_RaycastProximitySlots' callers pushes 1), so for every ray that reaches the faces flag 1
// matters only beside 0x800]
const std::vector<ModelFaceFlag> &model_face_flags() {
	static const std::vector<ModelFaceFlag> flags = {
		{0x1, "both_sides", "Both sides",
		 "Rounds meet the face from either side even where Front only is set. On its own it changes nothing: the "
		 "game tests every face from both sides."},
		{0x100, "bullets_pass", "Bullets pass",
		 "Every round passes the face with no effect and no energy cost: the game's face test skips it (retail's "
		 "rotor blades). The soft surfaces (Water, Glass, Cloth, Foliage, Flesh) also let rounds through, at an "
		 "energy cost and with their effect."},
		{0x800, "front_only", "Front only",
		 "Rounds meet the face only entering it from its front (the side it faces); from behind they pass. Both "
		 "sides overrides it. No retail face sets it."},
	};
	return flags;
}

// --- the faces' materials ----------------------------------------------------------------------------

namespace {

struct Point {
	double x = 0.0, y = 0.0, z = 0.0;
};

Point sub(const Point &a, const Point &b) { return Point{a.x - b.x, a.y - b.y, a.z - b.z}; }
Point cross(const Point &a, const Point &b) {
	return Point{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(const Point &a, const Point &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// A face's middle and a triangle's are compared on a grid of 1/64 m cells: every triangle whose middle
// lies within 1/64 m of the face's is in the face's cell or a neighbour. The collision corners sit on the
// 8.8 grid (the render corners they were taken from truncated, within 1/256 m on each axis), so a face's
// own triangle is well within it.
constexpr double kCell = 64.0;
constexpr double kNear = 1.0 / 64.0;

uint64_t cell_key(int64_t x, int64_t y, int64_t z) {
	const auto pack = [](int64_t v) { return static_cast<uint64_t>(v + (int64_t(1) << 20)) & 0x1FFFFF; };
	return (pack(x) << 42) | (pack(y) << 21) | pack(z);
}
int64_t cell_of(double v) { return static_cast<int64_t>(std::floor(v * kCell)); }

// A triangle of a LOD, or a bullet face: its middle and its winding's normal (unnormalized), mission
// axes (the collision block's frame on disk).
struct Triangle {
	Point corner[3]; // a LOD triangle's on the collision grid (truncated to 8.8, as a face's were made)
	Point middle;
	Point normal;
	int material = -1;
};

// How far a face's corners lie from a triangle's on the grid: each corner to the triangle's nearest,
// squared, summed; 0 for the triangle the face was made from. Middles alone tell a small part's triangles,
// a few millimetres across, apart no better than the grid step (CQBHelmt's face 874).
double corner_distance(const Triangle &face, const Triangle &triangle) {
	double sum = 0.0;
	for (const Point &c : face.corner) {
		double best = -1.0;
		for (const Point &t : triangle.corner) {
			const Point d = sub(t, c);
			const double d2 = dot(d, d);
			if (best < 0.0 || d2 < best) best = d2;
		}
		sum += best;
	}
	return sum;
}

Triangle triangle_of(const Point corner[3], int material) {
	Triangle t;
	for (int k = 0; k < 3; ++k) t.corner[k] = corner[k];
	for (int k = 0; k < 3; ++k) t.middle = Point{t.middle.x + corner[k].x / 3.0, t.middle.y + corner[k].y / 3.0, t.middle.z + corner[k].z / 3.0};
	t.normal = cross(sub(corner[1], corner[0]), sub(corner[2], corner[0]));
	t.material = material;
	return t;
}

// The LOD's triangles by cell.
struct TriangleGrid {
	std::vector<Triangle> triangles;
	std::unordered_map<uint64_t, std::vector<uint32_t>> cells;

	void add(const Triangle &t) {
		cells[cell_key(cell_of(t.middle.x), cell_of(t.middle.y), cell_of(t.middle.z))].push_back(
				static_cast<uint32_t>(triangles.size()));
		triangles.push_back(t);
	}
	// Every triangle whose middle lies within kNear of `p`, with its squared distance.
	void near(const Point &p, std::vector<std::pair<uint32_t, double>> &out) const {
		out.clear();
		const int64_t cx = cell_of(p.x), cy = cell_of(p.y), cz = cell_of(p.z);
		for (int64_t dx = -1; dx <= 1; ++dx)
			for (int64_t dy = -1; dy <= 1; ++dy)
				for (int64_t dz = -1; dz <= 1; ++dz) {
					const auto found = cells.find(cell_key(cx + dx, cy + dy, cz + dz));
					if (found == cells.end()) continue;
					for (const uint32_t i : found->second) {
						const Point d = sub(triangles[i].middle, p);
						const double d2 = dot(d, d);
						if (d2 <= kNear * kNear) out.push_back({i, d2});
					}
				}
	}
};

void lod_triangles(const Threedi3di3 &model, const ThreediLod &lod, TriangleGrid &grid) {
	std::vector<uint16_t> tris;
	for (size_t s = 0; s < lod.strip_count; ++s) {
		const ThreediTriangleStrip &strip = lod.strips[s];
		if (!threedi_decode_strip_indices(lod, strip, tris)) continue;
		const int material = threedi_material_array_index_for_id(model, strip.material_index);
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			// Retail winds a triangle counter-clockwise in model axes and a face counter-clockwise in
			// mission axes; model axes mirror mission, so the triangle's second and third corners swap to
			// give its outward normal in mission axes (the .o3d lowering's swap, threedi_o3d_lower.cpp).
			Point corner[3];
			for (int k = 0; k < 3; ++k) {
				const float *p = lod.vertices.items[strip.start_vertex + tris[t + size_t(k == 0 ? 0 : 3 - k)]].position;
				const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{p[0], p[1], p[2]});
				corner[k] = Point{m.x, m.y, m.z};
			}
			Triangle triangle = triangle_of(corner, material);
			// The corners a face made from it holds [threedi_build's add_collision_vertex, WriteCVRT's truncation].
			for (Point &c : triangle.corner) c = Point{threedi_q8f_trunc(c.x), threedi_q8f_trunc(c.y), threedi_q8f_trunc(c.z)};
			grid.add(triangle);
		}
	}
}

// One LOD's answer: each face's material, how many found one, how many had triangles of more than one
// material about them, and how many took a triangle wound against them for want of one wound with them.
struct LodMatch {
	std::vector<int> material;
	size_t matched = 0, ambiguous = 0, against = 0;
};

// The faces' triangles in one LOD. A face takes a triangle whose middle lies within 1/64 m of its own:
// the one it was made from where one is there (the triangle's corners truncated to the 8.8 grid are the
// face's, as the exporter made them [threedi_build's add_collision_vertex], and it is wound with the face:
// a sheet stored in both windings, its two sides of two materials, puts two such triangles at one face,
// and the face is its own side's), else the one whose corners lie nearest its own, wound with it before
// against. One face to a triangle, the pairs taken in that order, so a coincident pair's second face takes
// the other triangle; a face made from a triangle that faces made from it took (a face stored twice)
// shares it, and a face whose every triangle another took (more faces than triangles at a place) takes its best one
// anyway. "With" is the model's own relation of the faces' windings (their stored normals, taken before
// the grid moved their corners) to its triangles' (the relation most faces with one triangle about them
// show; retail's and the add-on's agree).
LodMatch match_lod(const TriangleGrid &grid, const std::vector<Triangle> &faces, const std::vector<bool> &valid) {
	LodMatch out;
	out.material.assign(faces.size(), -1);
	std::vector<std::vector<std::pair<uint32_t, double>>> near(faces.size());
	long long agree = 0;
	for (size_t f = 0; f < faces.size(); ++f) {
		if (!valid[f]) continue;
		grid.near(faces[f].middle, near[f]);
		if (near[f].size() == 1) agree += dot(faces[f].normal, grid.triangles[near[f][0].first].normal) >= 0.0 ? 1 : -1;
	}
	const double with = agree >= 0 ? 1.0 : -1.0;
	struct Pair {
		size_t face;
		uint32_t triangle;
		bool wound;
		double d2;
	};
	std::vector<Pair> pairs;
	for (size_t f = 0; f < faces.size(); ++f) {
		int first = -2;
		for (const auto &candidate : near[f]) {
			const uint32_t t = candidate.first;
			const int m = grid.triangles[t].material;
			if (first == -2) first = m;
			else if (m != first && first != -3) {
				++out.ambiguous;
				first = -3;
			}
			pairs.push_back({f, t, with * dot(faces[f].normal, grid.triangles[t].normal) > 0.0,
			                 corner_distance(faces[f], grid.triangles[t])});
		}
	}
	// The triangle the face was made from first (its corners on the grid the face's, wound with it), then
	// one on its corners wound against it, then the nearest by corners, wound with it before against.
	const auto rank = [](const Pair &p) { return p.d2 == 0.0 ? (p.wound ? 0 : 1) : (p.wound ? 2 : 3); };
	std::sort(pairs.begin(), pairs.end(), [&](const Pair &a, const Pair &b) {
		if (rank(a) != rank(b)) return rank(a) < rank(b);
		if (a.d2 != b.d2) return a.d2 < b.d2;
		return a.face != b.face ? a.face < b.face : a.triangle < b.triangle;
	});
	std::vector<bool> taken(grid.triangles.size(), false), placed(faces.size(), false);
	std::vector<int> best(faces.size(), -1);
	std::vector<bool> best_wound(faces.size(), false), made_from(faces.size(), false);
	// A face made from a triangle keeps one it was made from though every such triangle was taken by
	// another (a face stored twice), before any face takes a triangle it was not made from.
	bool shared = false;
	const auto share = [&] {
		shared = true;
		for (size_t f = 0; f < faces.size(); ++f)
			if (!placed[f] && made_from[f]) {
				placed[f] = taken[size_t(best[f])] = true;
				out.material[f] = grid.triangles[size_t(best[f])].material;
			}
	};
	for (const Pair &p : pairs) {
		if (!shared && rank(p) != 0) share();
		if (best[p.face] < 0) {
			best[p.face] = int(p.triangle);
			best_wound[p.face] = p.wound;
			made_from[p.face] = rank(p) == 0;
		}
		if (placed[p.face] || taken[p.triangle]) continue;
		placed[p.face] = taken[p.triangle] = true;
		out.material[p.face] = grid.triangles[p.triangle].material;
		if (!p.wound && best_wound[p.face]) ++out.against;
	}
	if (!shared) share();
	for (size_t f = 0; f < faces.size(); ++f) {
		if (!placed[f] && best[f] >= 0) out.material[f] = grid.triangles[size_t(best[f])].material;
		if (out.material[f] >= 0) ++out.matched;
	}
	return out;
}

std::shared_ptr<const ModelFaceMaterials> make_face_materials(const Threedi3di3 &model) {
	auto out = std::make_shared<ModelFaceMaterials>();
	const ThreediCollisionModel *c = model.collision;
	if (!c || c->face_count == 0) return out;
	out->material.assign(c->face_count, -1);
	// Each face's middle and winding, through its section's vertex run (the prefix sums the loader takes).
	std::vector<ThreediCollisionObjectRun> runs(c->object_count);
	if (!threedi_collision_object_runs(c, runs.data())) return out;
	std::vector<Triangle> faces(c->face_count);
	std::vector<bool> valid(c->face_count, false);
	for (size_t o = 0; o < c->object_count; ++o) {
		const ThreediCollisionObject &object = c->objects[o];
		for (int32_t f = 0; f < object.num_faces; ++f) {
			const size_t face = static_cast<size_t>(runs[o].face_start + f);
			const ThreediCollisionFace &cf = c->faces[face];
			Point corner[3];
			bool inside = true;
			for (int k = 0; k < 3 && inside; ++k) {
				const int32_t at = cf.vert_index[k];
				inside = at >= 0 && at < object.num_vertices;
				if (!inside) break;
				const float *p = c->vertices[static_cast<size_t>(runs[o].vertex_start + at)].position;
				corner[k] = Point{p[0], p[1], p[2]};
			}
			if (inside) {
				faces[face] = triangle_of(corner, -1);
				// Its winding by its stored normal (CNRM), taken from the corners before the 8.8 grid moved
				// them: a face a few millimetres across may wind any way on the grid (threedi_build's add_face).
				const int32_t n = runs[o].normal_start + cf.normal_index;
				if (cf.normal_index >= 0 && n >= 0 && static_cast<size_t>(n) < c->normal_count) {
					const float *stored = c->normals[static_cast<size_t>(n)].normal;
					faces[face].normal = Point{stored[0], stored[1], stored[2]};
				}
			}
			valid[face] = inside;
		}
	}
	// The LOD whose triangles meet the most faces (the first of a tie): the collision LOD.
	for (size_t l = 0; l < model.lod_count; ++l) {
		TriangleGrid grid;
		lod_triangles(model, model.lods[l], grid);
		if (grid.triangles.empty()) continue;
		LodMatch match = match_lod(grid, faces, valid);
		if (match.matched > out->matched) {
			out->matched = match.matched;
			out->ambiguous = match.ambiguous;
			out->against = match.against;
			out->lod = static_cast<int>(l);
			out->material = std::move(match.material);
		}
		if (out->matched == c->face_count) break;
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
	if (!surface.mixed()) return model_surface_label(surface.common());
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
