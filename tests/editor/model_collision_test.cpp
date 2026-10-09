// A model's collision shown in its viewport (ADR 0046 S17, Models: "we should be able to see the
// collisions").
//
// Synthetic: a box's six planes make the box's solid (six facets, its eight corners); every synthetic
// model's collision becomes shapes: a shape per bullet face whose corners lie on a triangle of the
// collision LOD as the preview draws it (the mission axes taken to the preview's space as the render
// vertices are), a solid per volume inside its stored box, a sphere per section that stores one (a
// person's hit spheres at the game's radius), the bounds; each record's shape maps to its record and
// back; a layer off shows nothing but the selection's; a press picks the front-most face, else the
// smallest sphere; the words name each volume type as the game acts on it; a live part carries its
// section's shapes.
//
// Retail (a leg, OPENNOVA_JO_DIR): every model the game install serves: each volume's planes make a solid
// inside its box (a flat ladder its facing), each face a triangle, how long the shapes take.
#include <editor/documents/model_collision_words.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/preview/model_collision.h>
#include <editor/project/project_document.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/vfs/vfs.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <runtime/assets/asset_store.h>
#include <formats/threedi/threedi_strip_decode.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::editor;
using opennova::pff::normalized_logical_name;
using namespace opennova::threedi;
namespace fs = std::filesystem;

namespace {

std::string synth_dir() { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth"; }

bool load(ModelDocument &document, const std::string &path, const std::string &name) {
	Diagnostic error;
	return document.load(path, name, AssetKind::Model, "jo", error);
}

ModelOverlayOptions every_layer() {
	ModelOverlayOptions options;
	options.user_points = options.lights = false;
	for (size_t l = 0; l < size_t(ModelCollisionLayer::kCount); ++l)
		model_collision_layer_set(options, ModelCollisionLayer(l), true);
	return options;
}

float distance(const PreviewVec3 &a, const PreviewVec3 &b) {
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// How far a volume's solid reaches past its stored box (0 inside).
double past_box(const std::vector<std::vector<ThreediBuildVec3>> &polygons, const ThreediBoundingVolume &v) {
	const double lo[3] = {v.min_x_fp16 / 65536.0, v.min_y_fp16 / 65536.0, v.min_z_fp16 / 65536.0};
	const double hi[3] = {v.max_x_fp16 / 65536.0, v.max_y_fp16 / 65536.0, v.max_z_fp16 / 65536.0};
	double past = 0.0;
	for (const auto &polygon : polygons)
		for (const ThreediBuildVec3 &p : polygon) {
			const double at[3] = {p.x, p.y, p.z};
			for (int k = 0; k < 3; ++k) past = std::max(past, std::max(lo[k] - at[k], at[k] - hi[k]));
		}
	return past;
}

// A volume's solid lies within its stored box (2 mm: retail's planes meet a few tenths of a millimetre
// off, the box is 16.16).
bool inside_box(const std::vector<std::vector<ThreediBuildVec3>> &polygons, const ThreediBoundingVolume &v) {
	return past_box(polygons, v) <= 0.002;
}

int box_planes_make_the_box() {
	// The six planes of the box -1..2 x 0..1 x 0..3 (n . p + d <= 0 inside).
	ThreediBoundingPlane planes[6] = {};
	const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	const float d[6] = {-2, -1, -1, 0, -3, 0};
	for (int i = 0; i < 6; ++i) {
		std::copy(n[i], n[i] + 3, planes[i].normal);
		planes[i].radius = d[i];
	}
	const auto polygons = model_volume_polygons(planes, 6, false);
	TEST_EXPECT(polygons.size() == 6);
	std::set<std::string> corners;
	for (const auto &polygon : polygons) {
		TEST_EXPECT(polygon.size() == 4);
		for (const ThreediBuildVec3 &p : polygon) {
			char key[64];
			// Rounded to the millimetre, + 0.0: a -0 prints as "-0.000".
			const auto mm = [](double v) { return std::round(v * 1000.0) / 1000.0 + 0.0; };
			std::snprintf(key, sizeof(key), "%.3f %.3f %.3f", mm(p.x), mm(p.y), mm(p.z));
			corners.insert(key);
		}
	}
	TEST_EXPECT(corners.size() == 8 && corners.count("-1.000 0.000 0.000") && corners.count("2.000 1.000 3.000"));
	// Five planes bound no solid; a ladder of them keeps its facing (plane 0) when it has a polygon.
	TEST_EXPECT(model_volume_polygons(planes, 5, false).empty());
	// Planes reaching past the volume's box: its solid is the part within the box (the quick test).
	ThreediBoundingVolume within{};
	within.collidable_type = 1;
	within.plane_count = 6;
	within.max_x_fp16 = within.max_y_fp16 = within.max_z_fp16 = 0x10000;
	const auto clipped = model_volume_solid(within, planes);
	TEST_EXPECT(!clipped.empty() && inside_box(clipped, within));
	TEST_EXPECT(model_volume_solid(within, planes).size() >= 6);
	std::printf("volumes: a box's planes make the box (6 facets, 8 corners)\n");
	return 0;
}

int synthetic_shapes() {
	int models = 0;
	size_t frames_checked = 0;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(synth_dir(), ec)) {
		if (entry.path().extension() != ".3di") continue;
		ModelDocument document;
		TEST_EXPECT(load(document, entry.path().generic_string(), entry.path().filename().generic_string()));
		const ModelRow *row = document.model_row();
		const CollisionRow *collision = document.collision_row();
		if (!row || !row->base || !row->base->collision) continue;
		const Threedi3di3 &model = *row->base;
		int32_t bus[96] = {};
		// A model whose LOD 0 animates is taken posed (a quarter second and more in: its parts turned).
		const bool live = threedi_panm_lod_has_live(model, 0);
		const uint32_t at_ms = live ? 700 : 0;
		const std::vector<ModelCollisionShape> shapes = model_collision_shapes(row->base, 0, at_ms, bus, every_layer());
		size_t faces = 0, volumes = 0, sections = 0, occlusion = 0;
		for (const ModelCollisionShape &s : shapes) {
			TEST_EXPECT(!s.name.empty() && !s.legend.empty());
			switch (s.kind) {
			case ModelCollisionKind::Face: ++faces; TEST_EXPECT(s.triangles.size() == 3 && s.edges.size() == 6); break;
			case ModelCollisionKind::Volume: ++volumes; TEST_EXPECT(!s.edges.empty()); break;
			case ModelCollisionKind::Section: ++sections; TEST_EXPECT(s.sphere && s.radius >= 0.0f); break;
			case ModelCollisionKind::Occlusion: ++occlusion; break;
			default: break;
			}
			// A record's shape is its record, and the record its shape.
			if (s.pickable) {
				const NodeAddress record = model_collision_record(document, s);
				TEST_EXPECT(record.child != 0);
				ModelCollisionPick back;
				TEST_EXPECT(model_collision_of(document, record, back) && back.kind == s.kind && back.index == s.index);
			}
		}
		TEST_EXPECT(collision && faces == collision->faces.size() && volumes == collision->volumes.size());
		TEST_EXPECT(occlusion == model.occlusion_object_count);
		TEST_EXPECT(sections == model_collision_layer_count(model, ModelCollisionLayer::Sections, 0));
		// Each volume's solid inside its box; a box volume's solid is its box.
		size_t plane_cursor = 0;
		for (size_t v = 0; v < model.collision->volume_count; ++v) {
			const ThreediBoundingVolume &bv = model.collision->volumes[v];
			const auto polygons = model_volume_solid(bv, model.collision->planes + plane_cursor);
			TEST_EXPECT(inside_box(polygons, bv));
			plane_cursor += size_t(bv.plane_count);
		}
		// A rigid model's faces lie on the collision LOD's triangles as the preview draws those (the
		// render vertices through preview_from_model, each part a PANM node drives turned by its matrix at
		// the same clock): the collision's axes and its pose reach the same place.
		// (A part a noise track moves poses anew at every call: such a model is not compared posed.)
		std::vector<ThreediMatrix4x4> parts, again;
		std::vector<uint8_t> driven;
		if (live) {
			threedi_panm_pose_parts(model, 0, at_ms, bus, parts, &driven);
			threedi_panm_pose_parts(model, 0, at_ms, bus, again, nullptr);
		}
		const bool steady = !live || std::equal(parts.begin(), parts.end(), again.begin(), again.end(),
		                                        [](const ThreediMatrix4x4 &a, const ThreediMatrix4x4 &b) {
			                                        return std::equal(a.m, a.m + 16, b.m);
		                                        });
		if (model.header.mesh_type != THREEDI_MESH_SKINNED && faces > 0 && steady) {
			// Every LOD's triangles, each part turned by LOD 0's part of its number (what the sections ride;
			// the collision LOD need not be LOD 0).
			std::vector<PreviewVec3> middles;
			for (size_t l = 0; l < model.lod_count; ++l) {
				const ThreediLod &lod = model.lods[l];
				std::vector<uint16_t> tris;
				size_t part = 0, left = lod.render_object_count ? size_t(lod.render_objects[0].num_strips +
				                                                         lod.render_objects[0].num_alpha_strips)
				                                                : 0;
				for (size_t st = 0; st < lod.strip_count; ++st) {
					while (left == 0 && part + 1 < lod.render_object_count) {
						++part;
						left = size_t(lod.render_objects[part].num_strips + lod.render_objects[part].num_alpha_strips);
					}
					if (left > 0) --left;
					if (!threedi_decode_strip_indices(lod, lod.strips[st], tris)) continue;
					const bool turns = live && part < driven.size() && driven[part];
					for (size_t t = 0; t + 2 < tris.size(); t += 3) {
						PreviewVec3 m;
						for (int k = 0; k < 3; ++k) {
							const float *v = lod.vertices.items[lod.strips[st].start_vertex + tris[t + k]].position;
							float q[3] = {v[0], v[1], v[2]};
							if (turns)
								for (int c = 0; c < 3; ++c)
									q[c] = v[0] * parts[part].m[c] + v[1] * parts[part].m[4 + c] + v[2] * parts[part].m[8 + c] +
									       parts[part].m[12 + c];
							const PreviewVec3 p = preview_from_model(q);
							m = PreviewVec3{m.x + p.x / 3, m.y + p.y / 3, m.z + p.z / 3};
						}
						middles.push_back(m);
					}
				}
			}
			size_t on = 0;
			for (const ModelCollisionShape &s : shapes) {
				if (s.kind != ModelCollisionKind::Face) continue;
				const PreviewVec3 m{(s.triangles[0].x + s.triangles[1].x + s.triangles[2].x) / 3,
				                    (s.triangles[0].y + s.triangles[1].y + s.triangles[2].y) / 3,
				                    (s.triangles[0].z + s.triangles[1].z + s.triangles[2].z) / 3};
				for (const PreviewVec3 &t : middles)
					if (distance(m, t) <= 1.0f / 64.0f) {
						++on;
						break;
					}
			}
			if (on != faces)
				std::printf("shapes: %s: %zu of %zu faces on a triangle\n", entry.path().filename().string().c_str(), on, faces);
			TEST_EXPECT(on == faces);
		}
		// Every other kind through its own axes, against the raw data by the maps written out here: the
		// collision block (mission axes) at (y, z, x), the occlusion records (model axes) at (-x, y, z).
		if (!live) {
			const auto mission = [](double x, double y, double z) { return PreviewVec3{float(y), float(z), float(x)}; };
			const auto box_of = [](const std::vector<PreviewVec3> &points, PreviewVec3 &lo, PreviewVec3 &hi) {
				lo = PreviewVec3{1e9f, 1e9f, 1e9f};
				hi = PreviewVec3{-1e9f, -1e9f, -1e9f};
				for (const PreviewVec3 &p : points) {
					lo = PreviewVec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
					hi = PreviewVec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
				}
			};
			const auto q = [](int32_t v) { return v / 65536.0; };
			size_t checked = 0;
			for (const ModelCollisionShape &s : shapes) {
				PreviewVec3 lo, hi;
				if (s.kind == ModelCollisionKind::Section) {
					const ThreediCollisionObject &o = model.collision->objects[size_t(s.index)];
					TEST_EXPECT(distance(s.center, mission(q(o.med[0]), q(o.med[1]), q(o.med[2]))) < 1e-4f);
					++checked;
				} else if (s.kind == ModelCollisionKind::Bounds) {
					const float *b = model.collision->model_data.bbox;
					box_of(s.edges, lo, hi);
					TEST_EXPECT(distance(lo, mission(b[0], b[1], b[2])) < 1e-4f && distance(hi, mission(b[3], b[4], b[5])) < 1e-4f);
					++checked;
				} else if (s.kind == ModelCollisionKind::Volume) {
					const ThreediBoundingVolume &v = model.collision->volumes[size_t(s.index)];
					const PreviewVec3 vlo = mission(q(v.min_x_fp16), q(v.min_y_fp16), q(v.min_z_fp16));
					const PreviewVec3 vhi = mission(q(v.max_x_fp16), q(v.max_y_fp16), q(v.max_z_fp16));
					box_of(s.edges, lo, hi);
					TEST_EXPECT(lo.x >= vlo.x - 0.002f && lo.y >= vlo.y - 0.002f && lo.z >= vlo.z - 0.002f &&
					            hi.x <= vhi.x + 0.002f && hi.y <= vhi.y + 0.002f && hi.z <= vhi.z + 0.002f);
					++checked;
				} else if (s.kind == ModelCollisionKind::Occlusion) {
					for (const PreviewVec3 &p : s.edges) {
						bool found = false;
						for (size_t i = 0; i < model.occlusion_vertex_count && !found; ++i) {
							const float *v = model.occlusion_vertices[i].position;
							found = distance(p, PreviewVec3{-v[0], v[1], v[2]}) < 1e-4f;
						}
						TEST_EXPECT(found);
					}
					++checked;
				} else if (s.kind == ModelCollisionKind::ProbeBox && s.index == 0) {
					ThreediCollisionProbeBoxes probes{};
					TEST_EXPECT(threedi_3di3_collision_probe_boxes(model.collision, &probes));
					box_of(s.edges, lo, hi);
					TEST_EXPECT(distance(lo, mission(q(probes.box_x_lo), q(probes.box_y_lo), q(probes.box_z_lo))) < 1e-4f &&
					            distance(hi, mission(q(probes.box_x_hi), q(probes.box_y_hi), q(probes.box_z_hi))) < 1e-4f);
					++checked;
				}
			}
			frames_checked += checked;
		}
		// A layer off shows nothing but the record asked for.
		ModelOverlayOptions none;
		TEST_EXPECT(model_collision_shapes(row->base, 0, 0, bus, none).empty());
		if (faces > 0) {
			const auto one = model_collision_shapes(row->base, 0, 0, bus, none, {ModelCollisionKind::Face, 0});
			TEST_EXPECT(one.size() == 1 && one[0].kind == ModelCollisionKind::Face && one[0].index == 0);
		}
		TEST_EXPECT(!model_collision_legend(shapes).empty());
		++models;
	}
	TEST_EXPECT(models > 5 && frames_checked > 20);
	std::printf("shapes: %d synthetic models' collision as shapes, each record's back to its record; %zu shapes' "
	            "axes checked against the raw data\n",
	            models, frames_checked);
	return 0;
}

int a_person_and_a_live_part() {
	// The person's bones are hit spheres at the game's radius.
	ModelDocument person;
	TEST_EXPECT(load(person, synth_dir() + "/person.3di", "person.3di"));
	int32_t bus[96] = {};
	ModelOverlayOptions sections;
	sections.sections = true;
	const auto spheres = model_collision_shapes(person.model_row()->base, 0, 0, bus, sections);
	size_t hits = 0;
	for (const ModelCollisionShape &s : spheres) {
		if (s.legend != "Hit spheres") continue;
		++hits;
		const ThreediCollisionObject &object = person.model_row()->base->collision->objects[size_t(s.index)];
		TEST_EXPECT(std::fabs(s.radius - model_person_hit_radius_q16(s.index, object.radius) / 65536.0f) < 1e-6f);
		TEST_EXPECT(s.name.rfind("Hit sphere of BN", 0) == 0);
	}
	TEST_EXPECT(hits > 0);
	TEST_EXPECT(model_person_hit_radius_q16(0, 0x10000) == 0xCCC + 0x10000 * 45 / 100);
	TEST_EXPECT(model_person_hit_radius_q16(14, 0x10000) == 0xCCC + 0x10000 * 65 / 100);
	TEST_EXPECT(model_person_hit_radius_q16(15, 0x10000) == 0x3000);

	// A volume type in the game's words; a type the game lists nothing for is solid.
	TEST_EXPECT(std::string(model_volume_type(4).words) == "ladder" && model_volume_type(4).family == ModelVolumeFamily::Ladder);
	TEST_EXPECT(std::string(model_volume_type(8).code) == "BB" && std::string(model_volume_type(19).words) == "player wall");
	TEST_EXPECT(model_volume_type(2).family == ModelVolumeFamily::Solid && model_volume_type(99).family == ModelVolumeFamily::Solid);

	// A live part carries the section that rides it: the shapes move with the clock.
	ModelDocument live;
	TEST_EXPECT(load(live, synth_dir() + "/house_lod0_sine_rotx.3di", "house.3di"));
	const auto at = [&](uint32_t ms) {
		return model_collision_shapes(live.model_row()->base, 0, ms, bus, every_layer());
	};
	const auto rest = at(0), later = at(700);
	TEST_EXPECT(rest.size() == later.size());
	bool moved = false;
	for (size_t i = 0; i < rest.size() && i < later.size(); ++i)
		for (size_t k = 0; k < rest[i].edges.size() && k < later[i].edges.size(); ++k)
			moved = moved || distance(rest[i].edges[k], later[i].edges[k]) > 1e-4f;
	TEST_EXPECT(moved);
	std::printf("a person's hit spheres at the game's radius; a live part carries its section\n");
	return 0;
}

// A part no PANM node drives stands as it is while the others turn (the runtime gives its section the
// entity's matrix): the pump's last part animation removed, part 4 (its pivot 3 m up and 2 m out) keeps
// its section's faces where the file has them, not moved by its pivot.
int a_part_no_node_drives() {
	ModelDocument document;
	TEST_EXPECT(load(document, synth_dir() + "/pump.3di", "pump.3di"));
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && row->base && threedi_panm_lod_has_live(*row->base, 0));
	if (!row || !row->base) return 1;
	const std::vector<RecordIds> &panm = row->ids.lists[kModelLods][0].lists[kModelOwnList];
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = {row->id, node_kind(ModelKind::PartAnimation), panm.back().id};
	Diagnostic error;
	TEST_EXPECT(document.apply(remove, error));
	const SerializeResult written = document.serialize();
	TEST_EXPECT(written.ok());
	const opennova::assets::Model model =
			opennova::assets::parse_model(reinterpret_cast<const uint8_t *>(written.text.data()), written.text.size());
	TEST_EXPECT(model && model->lods[0].part_animation_count == 4 && threedi_panm_lod_has_live(*model, 0));
	if (!model) return 1;
	int32_t bus[96] = {};
	ModelOverlayOptions faces;
	faces.bullet_faces = true;
	const std::vector<ModelCollisionShape> shapes = model_collision_shapes(model, 0, 700, bus, faces);
	const ThreediCollisionModel &c = *model->collision;
	std::vector<ThreediCollisionObjectRun> runs(c.object_count);
	TEST_EXPECT(threedi_collision_object_runs(&c, runs.data()));
	size_t checked = 0;
	for (const ModelCollisionShape &s : shapes) {
		if (s.section != 4) continue;
		for (const PreviewVec3 &p : s.triangles) {
			bool found = false;
			for (int32_t v = 0; v < c.objects[4].num_vertices && !found; ++v) {
				const float *q = c.vertices[size_t(runs[4].vertex_start + v)].position;
				found = distance(p, PreviewVec3{q[1], q[2], q[0]}) < 1e-4f;
			}
			TEST_EXPECT(found);
			++checked;
		}
	}
	TEST_EXPECT(checked == size_t(c.objects[4].num_faces) * 3);
	std::printf("a part no node drives: its section stands where the file has it (%zu corners)\n", checked);
	return 0;
}

int picking() {
	// Two faces under the pixel: the nearer is taken; none: the smallest sphere holding it.
	std::vector<ModelCollisionShape> shapes(3);
	const auto triangle = [](ModelCollisionShape &s, float z) {
		s.kind = ModelCollisionKind::Face;
		s.pickable = true;
		s.triangles = {PreviewVec3{-1, -1, z}, PreviewVec3{1, -1, z}, PreviewVec3{0, 1, z}};
	};
	triangle(shapes[0], -1.0f);
	triangle(shapes[1], 1.0f);
	shapes[2].kind = ModelCollisionKind::Section;
	shapes[2].pickable = shapes[2].sphere = true;
	shapes[2].center = PreviewVec3{5, 0, 0};
	shapes[2].radius = 0.5f;
	OrbitCamera camera;
	camera.yaw = 0.0f;
	camera.pitch = 0.0f;
	camera.distance = 10.0f;
	float x = 0, y = 0, sx = 0, sy = 0;
	TEST_EXPECT(camera.project(PreviewVec3{0, 0, 0}, 800, 600, x, y));
	TEST_EXPECT(pick_model_collision(shapes, camera, 800, 600, x, y) == 1);
	TEST_EXPECT(camera.project(shapes[2].center, 800, 600, sx, sy));
	TEST_EXPECT(pick_model_collision(shapes, camera, 800, 600, sx, sy) == 2);
	TEST_EXPECT(pick_model_collision(shapes, camera, 800, 600, 2.0f, 2.0f) == -1);
	PreviewVec3 center;
	float radius = 0;
	model_collision_bounds(shapes[0], center, radius);
	TEST_EXPECT(radius > 0.9f && std::fabs(center.z + 1.0f) < 1e-5f);
	std::printf("picking: the nearer face, else the smallest sphere\n");
	return 0;
}

// Every model the game install serves: each volume's planes make a solid inside its box, each face a
// triangle; how long the shapes take.
int retail_collision() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every model's collision as shapes)");
	const ProjectDocument project;
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t models = 0, volumes = 0, solids = 0, flat = 0, boxes = 0, outside = 0, clipped = 0, inverted = 0, faces = 0,
	       sections = 0, occlusion = 0;
	double seconds = 0.0;
	size_t persons = 0, not_persons = 0;
	std::vector<std::string> not_person_names;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::gameprofile::gameprofile_scr_policy_for_code(project.target_game.c_str()));
		TEST_EXPECT(game.mount_game(root, expansion) && game.has_mounted_archive());
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(fs::path(file.logical_name).extension().string()) != ".3di") continue;
			if (!seen.insert(file.source_path + "|" + normalized_logical_name(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			if (!game.read_file(file.logical_name, bytes) || bytes.size() < 4 || std::memcmp(bytes.data(), "3DI3", 4) != 0)
				continue;
			ModelDocument document;
			Diagnostic error;
			if (!document.load_bytes(bytes, file.logical_name, AssetKind::Model, project.target_game, error)) continue;
			const ModelRow *row = document.model_row();
			if (!row || !row->base || !row->base->collision) continue;
			++models;
			const ThreediCollisionModel &c = *row->base->collision;
			size_t plane_cursor = 0;
			for (size_t v = 0; v < c.volume_count; ++v) {
				const ThreediBoundingVolume &bv = c.volumes[v];
				const size_t planes = bv.plane_count > 0 ? size_t(bv.plane_count) : 0;
				if (plane_cursor + planes > c.plane_count) break;
				// The planes' own solid, then within the box (the game's quick test): how many reach past it.
				const bool reaches = !inside_box(model_volume_polygons(c.planes + plane_cursor, planes, bv.collidable_type == 4), bv);
				clipped += reaches ? 1 : 0;
				const auto polygons = model_volume_solid(bv, c.planes + plane_cursor);
				plane_cursor += planes;
				++volumes;
				if (polygons.empty()) ++boxes;
				else if (polygons.size() == 1) ++flat;
				else ++solids;
				const bool inside_out = bv.min_x_fp16 > bv.max_x_fp16 || bv.min_y_fp16 > bv.max_y_fp16 ||
				                        bv.min_z_fp16 > bv.max_z_fp16;
				inverted += inside_out ? 1 : 0;
				if (!inside_out && !inside_box(polygons, bv)) {
					++outside;
					if (outside <= 8)
						std::printf("retail collision: %s volume %zu (type %d, %d planes, %zu facets): its solid "
						            "reaches %.3f m past its box %.3f %.3f %.3f .. %.3f %.3f %.3f\n",
						            file.logical_name.c_str(), v, bv.collidable_type, bv.plane_count, polygons.size(),
						            past_box(polygons, bv), bv.min_x_fp16 / 65536.0, bv.min_y_fp16 / 65536.0,
						            bv.min_z_fp16 / 65536.0, bv.max_x_fp16 / 65536.0, bv.max_y_fp16 / 65536.0,
						            bv.max_z_fp16 / 65536.0);
				}
			}
			int32_t bus[96] = {};
			const auto start = std::chrono::steady_clock::now();
			const auto shapes = model_collision_shapes(row->base, 0, 0, bus, every_layer());
			seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			// The skinned models: a person's (its bone sections hit spheres) or not (no face anywhere).
			if (row->base->header.mesh_type == THREEDI_MESH_SKINNED) {
				bool bones = false, person = false;
				for (size_t o = 0; o < c.object_count; ++o) {
					bones = bones || (c.objects[o].num_faces == 0 && c.objects[o].num_bounding_volumes == 0 && c.objects[o].radius > 0);
					person = person || model_section_is_person(*row->base, o);
				}
				if (bones) {
					(person ? persons : not_persons) += 1;
					if (!person && not_person_names.size() < 6) not_person_names.push_back(file.logical_name);
				}
			}
			for (const ModelCollisionShape &s : shapes) {
				faces += s.kind == ModelCollisionKind::Face ? 1 : 0;
				sections += s.kind == ModelCollisionKind::Section ? 1 : 0;
				occlusion += s.kind == ModelCollisionKind::Occlusion ? 1 : 0;
			}
		}
	}
	if (models == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's models in its archives");
	std::printf("retail collision: %zu models with collision: %zu volumes (%zu solids, %zu flat, %zu as their box; %zu "
	            "whose planes reach past their box, clipped to it; %zu boxes inside out; %zu leave it), %zu bullet faces, "
	            "%zu section spheres, %zu occlusion records; shapes made in %.2f s (first time, every layer)\n",
	            models, volumes, solids, flat, boxes, clipped, inverted, outside, faces, sections, occlusion, seconds);
	std::string names;
	for (const std::string &n : not_person_names) names += (names.empty() ? "" : ", ") + n;
	std::printf("retail collision: %zu skinned models with bone spheres: %zu persons (a face mesh row), %zu not (%s...)\n",
	            persons + not_persons, persons, not_persons, names.c_str());
	TEST_EXPECT(outside == 0);
	TEST_EXPECT(boxes * 50 <= volumes);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (box_planes_make_the_box() != 0) return 1;
	if (synthetic_shapes() != 0) return 1;
	if (a_person_and_a_live_part() != 0) return 1;
	if (a_part_no_node_drives() != 0) return 1;
	if (picking() != 0) return 1;
	return retail_collision();
}
