// The model document's structural edits and its validator (model_document.h).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <base/io/strutil.h>
#include <editor/documents/model_document.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>

#include "model_document_internal.h"

namespace opennova::editor {

using namespace threedi;
using namespace model_document_detail;

namespace {

constexpr NodeKind kModel = node_kind(ModelKind::Model);

template <typename T> void insert_at(std::vector<T> &v, size_t at, T value) {
	v.insert(v.begin() + static_cast<std::ptrdiff_t>(std::min(at, v.size())), std::move(value));
}
template <typename T> void erase_at(std::vector<T> &v, size_t at) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(at)); }
template <typename T> void move_within(std::vector<T> &v, size_t from, size_t to) {
	T value = std::move(v[from]);
	erase_at(v, from);
	insert_at(v, to, std::move(value));
}

// A list's record and its identity together: add / duplicate / remove / move.
template <typename T>
bool edit_list(std::vector<T> &records, std::vector<NodeId> &ids, const Edit &edit, size_t index, T fresh,
               const Document::IdAllocator &allocate, NodeId &added, std::string &error) {
	switch (edit.operation) {
	case EditOperation::Add:
	case EditOperation::Duplicate: {
		T value = edit.operation == EditOperation::Duplicate ? records[index] : std::move(fresh);
		const size_t at = std::min(edit.position, records.size());
		insert_at(records, at, std::move(value));
		added = allocate();
		insert_at(ids, at, added);
		return true;
	}
	case EditOperation::Remove:
		erase_at(records, index);
		erase_at(ids, index);
		return true;
	case EditOperation::Move: {
		const size_t to = std::min(edit.position, records.size() - 1);
		move_within(records, index, to);
		move_within(ids, index, to);
		return true;
	}
	default:
		error = "This list cannot take that edit.";
		return false;
	}
}

// What a model's CTRL register index and MTRX row are named by: a track, a generator or
// a light above style 0x70 (threedi_generator_names_register) and a flipbook that reads a
// register (threedi_flipbook_reads_register) name a register by its index; a part
// animation names the frame its pose turns through (threedi_panm_frame_row).
bool register_named(const ModelRow &row, uint32_t index) {
	const auto names = [&](uint8_t style, int64_t param) {
		return threedi_generator_names_register(style) && param == static_cast<int64_t>(index);
	};
	for (const ModelLod &lod : row.lods)
		for (const ThreediPartAnimation &pa : lod.panm)
			for (const ThreediTransform *t : threedi_panm_tracks(pa))
				if (names(t->control, t->control_param)) return true;
	for (const ModelMaterial &m : row.materials) {
		const ThreediMaterial &mt = m.material;
		if (names(mt.alpha_gen.style, mt.alpha_gen.reg) || names(mt.rgb_gen.style, mt.rgb_gen.reg) ||
		    names(mt.rgb_gen2.style, mt.rgb_gen2.reg) || names(mt.u_params.style, mt.u_params.reg) ||
		    names(mt.v_params.style, mt.v_params.reg))
			return true;
		if (threedi_flipbook_reads_register(mt.animation) && mt.animation.cycle_frame_time == static_cast<int16_t>(index))
			return true;
	}
	for (const ThreediLight &l : row.lights)
		if (names(l.style, l.phase)) return true;
	return false;
}

bool frame_named(const ModelRow &row, uint32_t index) {
	for (const ModelLod &lod : row.lods)
		for (const ThreediPartAnimation &pa : lod.panm)
			if (threedi_panm_frame_row(pa) == static_cast<int>(index)) return true;
	return false;
}

ThreediMaterial fresh_material() {
	ThreediBuildModel build;
	build.add_material("FF_ST_OP", nullptr);
	return build.materials.front();
}

ThreediLight fresh_light() {
	ThreediBuildModel build;
	const int white[3] = {255, 255, 255};
	build.add_light(ThreediBuildVec3{0.0, 0.0, 0.0}, 1.0, 10.0, 0, 0, white, white);
	return build.lights.front();
}

ThreediUserPoint fresh_user_point(const ModelRow &row) {
	std::string name;
	for (int n = 1;; ++n) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "POINT%02d", n);
		bool taken = false;
		for (const ThreediUserPoint &u : row.user_points) taken = taken || strutil::iequals(u.name, buf);
		if (!taken) {
			name = buf;
			break;
		}
	}
	ThreediBuildModel build;
	build.add_user_point(name.c_str(), ThreediBuildVec3{0.0, 0.0, 0.0}, ThreediBuildVec3{1.0, 0.0, 0.0}, -1,
	                     THREEDI_USER_POINT_GAMEPLAY);
	return build.user_points.front();
}

} // namespace

bool ModelDocument::edit_collection(Node &node, const Edit &edit, const IdAllocator &allocate, NodeId &added,
                                    std::string &error) {
	if (node.kind != kModel) {
		error = "The collision records are the model's geometry: export the model again from Blender to change them.";
		return false;
	}
	ModelRow &row = static_cast<ModelRow &>(node);
	ModelPlace place{};
	const bool adding = edit.operation == EditOperation::Add;
	if (!adding && !place_of(row, edit.address.child, place)) {
		error = "The record no longer exists.";
		return false;
	}
	// The owner an Add names: 0 the row, else a LOD (part animations) or a material (texture rows).
	ModelPlace owner{};
	if (adding && edit.parent != 0 && edit.parent != row.id && !place_of(row, edit.parent, owner)) {
		error = "The owner no longer exists.";
		return false;
	}
	row.places.reset();
	const size_t index = place.index;
	switch (static_cast<ModelKind>(edit.address.kind)) {
	case ModelKind::Material: {
		if (edit.operation == EditOperation::Remove && material_is_drawn(row, row.materials[index].source)) {
			error = "Strips draw with this material: export the model again from Blender to take it off them.";
			return false;
		}
		ModelMaterial fresh;
		fresh.material = fresh_material();
		if (!edit_list(row.materials, row.collections[1], edit, index, std::move(fresh), allocate, added, error)) return false;
		// A copy is drawn by no strip, and its texture rows are its own.
		if (edit.operation == EditOperation::Duplicate) {
			ModelMaterial &copy = row.materials[std::min(edit.position, row.materials.size() - 1)];
			copy.source = -1;
			for (NodeId &id : copy.texture_ids) id = allocate();
		}
		return true;
	}
	case ModelKind::Texture: {
		const size_t m = adding ? owner.index : place.owner;
		if (adding && owner.collection != 1) {
			error = "A texture row belongs to a material.";
			return false;
		}
		ThreediMaterial &material = row.materials[m].material;
		std::vector<NodeId> &ids = row.materials[m].texture_ids;
		std::vector<ThreediMaterialTexture> list(material.textures, material.textures + ids.size());
		if ((adding || edit.operation == EditOperation::Duplicate) && list.size() >= 24) {
			error = "A material holds 24 texture rows.";
			return false;
		}
		ThreediMaterialTexture fresh{};
		fresh.slot = static_cast<uint8_t>(THREEDI_TEX_SLOT_DIFFUSE);
		if (!edit_list(list, ids, edit, index, fresh, allocate, added, error)) return false;
		std::memset(material.textures, 0, sizeof(material.textures));
		std::copy(list.begin(), list.end(), material.textures);
		material.texture_count = static_cast<uint32_t>(list.size());
		return true;
	}
	case ModelKind::Light:
		return edit_list(row.lights, row.collections[2], edit, index, fresh_light(), allocate, added, error);
	case ModelKind::UserPoint: {
		if (!edit_list(row.user_points, row.collections[3], edit, index, fresh_user_point(row), allocate, added, error))
			return false;
		return true;
	}
	case ModelKind::Register:
	case ModelKind::Frame: {
		// Named by index: added at the end, the last removed while nothing names it.
		const bool reg = edit.address.kind == node_kind(ModelKind::Register);
		std::vector<NodeId> &ids = row.collections[reg ? 4 : 5];
		const size_t count = ids.size();
		if (adding) {
			added = allocate();
			ids.push_back(added);
			if (reg) {
				ThreediControlRegister r{};
				std::snprintf(r.name, sizeof(r.name), "%s", "LOD_FRAC");
				row.registers.push_back(r);
			} else {
				ThreediMatrix4x4 identity;
				threedi_mat4_identity(&identity);
				row.frames.push_back(identity);
			}
			return true;
		}
		if (edit.operation != EditOperation::Remove || index + 1 != count) {
			error = reg ? "A register is named by its index: add one at the end, or remove the last."
			            : "A rotation frame is named by its row: add one at the end, or remove the last.";
			return false;
		}
		if (reg ? register_named(row, static_cast<uint32_t>(index)) : (index == 0 || frame_named(row, static_cast<uint32_t>(index)))) {
			error = reg ? "Something reads this register: point it at another first."
			            : "A part animation turns through this frame (row 0 is the identity every model keeps).";
			return false;
		}
		ids.pop_back();
		if (reg) row.registers.pop_back();
		else row.frames.pop_back();
		return true;
	}
	case ModelKind::PartAnimation: {
		const size_t l = adding ? owner.index : place.owner;
		if (adding && owner.collection != 0) {
			error = "A part animation belongs to a LOD.";
			return false;
		}
		ModelLod &lod = row.lods[l];
		if (adding) {
			// Row i transforms part i, as every retail table does (threedi_o3d_read's rule).
			if (lod.panm.size() >= lod.lod.render_object_count) {
				error = "Every part of this LOD has its part animation.";
				return false;
			}
			const int part = static_cast<int>(lod.panm.size());
			const int parent = part < static_cast<int>(lod.lod.render_object_count) ? lod.lod.render_objects[part].parent_index : -1;
			lod.panm.push_back(threedi_build_inert_panm(part, parent));
			added = allocate();
			lod.panm_ids.push_back(added);
			return true;
		}
		if (edit.operation != EditOperation::Remove || index + 1 != lod.panm.size()) {
			error = "Part animations go in part order: add one at the end, or remove the last.";
			return false;
		}
		lod.panm.pop_back();
		lod.panm_ids.pop_back();
		return true;
	}
	default:
		error = "LODs are the model's geometry: export the model again from Blender to change them.";
		return false;
	}
}

std::vector<Diagnostic> validate_models(const ValidationInput &input, const AssetGraph &) {
	std::vector<Diagnostic> findings;
	for (const auto &asset : input.scan.entries) {
		if (!is_model_kind(asset.kind)) continue;
		Diagnostic error;
		const std::shared_ptr<const Document> document = input.document(asset, error);
		if (!document) {
			findings.push_back(error);
			continue;
		}
		const auto *model = dynamic_cast<const ModelDocument *>(document.get());
		const ModelRow *row = model ? model->model_row() : nullptr;
		if (!row) continue;
		const auto add = [&](DiagnosticSeverity severity, const char *code, const std::string &message, ModelKind kind,
		                     size_t collection, size_t index, const char *field) {
			Diagnostic d = make_diagnostic(severity, code, message, document->path(), field);
			d.row_id = row->id;
			d.record_kind = node_kind(kind);
			if (collection < row->collections.size() && index < row->collections[collection].size()) {
				d.child_id = row->collections[collection][index];
				d.record = document->record_path({row->id, node_kind(kind), d.child_id});
			}
			findings.push_back(std::move(d));
		};
		// A ninth `sitex` seat takes the control seat's slot (the one ctrlx and drvrx fill) and
		// ends the seat scan, as the runtime's seat walk does (mission::extract_seats); the
		// model still loads: a warning (threedi_user_point_is_sitex, THREEDI_SITEX_SEAT_LIMIT).
		// The item-effect attach scan reads the first 16 points [orig:
		// ItemDef_GetBoneMaskByName @ 0x49ea40].
		int seats = 0;
		std::map<std::string, size_t> names;
		for (size_t i = 0; i < row->user_points.size(); ++i) {
			const ThreediUserPoint &u = row->user_points[i];
			if (threedi_user_point_is_sitex(u.name) && ++seats == THREEDI_SITEX_SEAT_LIMIT + 1)
				add(DiagnosticSeverity::Warning, "model.seats",
				    "More than " + std::to_string(THREEDI_SITEX_SEAT_LIMIT) +
				            " sitex seats: the game takes the ninth for the control seat, and its seat scan reads no "
				            "user point after it.",
				    ModelKind::UserPoint, 3, i, "name");
			if (!names.emplace(strutil::to_upper(u.name), i).second)
				add(DiagnosticSeverity::Warning, "model.user_point_duplicate",
				    std::string("Two user points are named '") + u.name + "'; a lookup by name finds the first.",
				    ModelKind::UserPoint, 3, i, "name");
		}
		if (row->user_points.size() > THREEDI_USER_POINT_SCAN_LIMIT)
			add(DiagnosticSeverity::Info, "model.user_points",
			    std::to_string(row->user_points.size()) + " user points: the item-effect attach scan reads the first 16.",
			    ModelKind::Model, SIZE_MAX, 0, "");
		// The CTRL registers the records name by index: a generator's, a light's or a loaded
		// track's above style 0x70, and a flipbook's with frames on the register clock. The load
		// swaps each index for the global register its table entry names, with no check of the
		// table's end [orig: ThreediGp_LoadFromFile @ 0x5B5C7A..0x5B5DA2 (materials),
		// @ 0x5B5E08..0x5B5EF6 (part animations), @ 0x5B5F4D..0x5B5F62 (lights)], so an index
		// past the end reads past the table: an error. A model with no table skips the material
		// and part-animation swaps [orig: ThreediGp_LoadFromFile @ 0x5B5C49]: the index stays as
		// written, and a style that reads a register reads the global one it numbers (a warning
		// naming it; past the 96 global registers, an error), while the other styles above 0x70
		// take it as their waveform's phase (no finding). The light swap runs with no table too
		// and reads through the table it lacks: an error. `whose` names the reference in a
		// finding with no field of its own to show it.
		const auto register_finding = [&](int64_t index, bool reads_register, bool light, const char *whose,
		                                  DiagnosticSeverity &severity, std::string &message) {
			const std::string reference = std::string(whose) + "register " + std::to_string(index);
			if (!row->registers.empty()) {
				if (index < static_cast<int64_t>(row->registers.size())) return false;
				severity = DiagnosticSeverity::Error;
				message = reference + " is not one of the model's " + std::to_string(row->registers.size()) +
				          ": the game reads past the end of its registers.";
				message[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(message[0])));
				return true;
			}
			if (light) {
				severity = DiagnosticSeverity::Error;
				message = "The model has no CTRL registers, and the game crashes loading a light that names one.";
				return true;
			}
			if (!reads_register) return false;
			const char *global = threedi_ctrl_register_name(static_cast<size_t>(index));
			severity = global ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
			message = "The model has no CTRL registers: the game reads " + reference +
			          (global ? std::string(" as the global register ") + global + "."
			                  : " as a global register, past the " + std::to_string(THREEDI_CTRL_REGISTER_COUNT) +
			                            " it has.");
			return true;
		};
		const auto check_register = [&](int64_t index, bool reads_register, ModelKind kind, size_t collection, size_t at,
		                                const char *field, const char *whose) {
			DiagnosticSeverity severity;
			std::string message;
			if (register_finding(index, reads_register, kind == ModelKind::Light, whose, severity, message))
				add(severity, "model.register_missing", message, kind, collection, at, field);
		};
		// Which styles read the register they name is their consumer's rule
		// (threedi_generator_reads_register); a flipbook names one only when it reads it
		// (threedi_flipbook_reads_register).
		for (size_t m = 0; m < row->materials.size(); ++m) {
			const ThreediMaterial &mt = row->materials[m].material;
			// A generator's index is its parameter byte (generator_param in threedi_schema.cpp).
			const auto generator = [&](ThreediGeneratorConsumer consumer, uint8_t style, int32_t reg, const char *field,
			                           const char *whose) {
				if (threedi_generator_names_register(style))
					check_register(static_cast<uint8_t>(reg), threedi_generator_reads_register(consumer, style),
					               ModelKind::Material, 1, m, field, whose);
			};
			generator(THREEDI_GENERATOR_CONSUMER_ALPHA, mt.alpha_gen.style, mt.alpha_gen.reg, "alphagen.param", "");
			generator(THREEDI_GENERATOR_CONSUMER_RGB, mt.rgb_gen.style, mt.rgb_gen.reg, "rgbgen.param", "");
			// The second RGB generator has no field of its own: its finding names it.
			generator(THREEDI_GENERATOR_CONSUMER_RGB, mt.rgb_gen2.style, mt.rgb_gen2.reg, "", "the second RGB generator's ");
			generator(THREEDI_GENERATOR_CONSUMER_UV, mt.u_params.style, mt.u_params.reg, "ugen.param", "");
			generator(THREEDI_GENERATOR_CONSUMER_UV, mt.v_params.style, mt.v_params.reg, "vgen.param", "");
			// The flipbook's index is the word the time field holds.
			if (threedi_flipbook_reads_register(mt.animation))
				check_register(static_cast<uint16_t>(mt.animation.cycle_frame_time), true, ModelKind::Material, 1, m,
				               "texanim.time", "");
			uint32_t flags = 0;
			if (!shader_flags(mt.shader_name, flags))
				add(DiagnosticSeverity::Warning, "model.shader_unknown",
				    std::string("The engine's shader table has no '") + mt.shader_name + "'.", ModelKind::Material, 1, m, "shader");
			if (row->materials[m].source < 0 || !material_is_drawn(*row, row->materials[m].source))
				add(DiagnosticSeverity::Info, "model.material_unused", "No strip draws with this material.",
				    ModelKind::Material, 1, m, "shader");
		}
		for (size_t i = 0; i < row->lights.size(); ++i) {
			const ThreediLight &l = row->lights[i];
			if (threedi_generator_names_register(l.style))
				check_register(l.phase, threedi_generator_reads_register(THREEDI_GENERATOR_CONSUMER_LIGHT, l.style),
				               ModelKind::Light, 2, i, "param", "");
			if (l.subobj_index != 0 && (row->lods.empty() || l.subobj_index >= row->lods[0].lod.render_object_count))
				add(DiagnosticSeverity::Error, "model.light_part", "This light names a part LOD 0 does not have.",
				    ModelKind::Light, 2, i, "part");
		}
		for (size_t i = 0; i < row->registers.size(); ++i)
			if (threedi_ctrl_register_ordinal(row->registers[i].name) == THREEDI_CTRL_REGISTER_NOT_FOUND)
				add(DiagnosticSeverity::Warning, "model.register_unknown",
				    std::string("'") + row->registers[i].name + "' is no CTRL register the engine knows: the game reads LOD_FRAC.",
				    ModelKind::Register, 4, i, "name");
		for (size_t l = 0; l < row->lods.size(); ++l) {
			const ModelLod &lod = row->lods[l];
			if (l > 0 && lod.lod.lod_threshold > row->lods[l - 1].lod.lod_threshold)
				add(DiagnosticSeverity::Warning, "model.lod_order",
				    "LOD " + std::to_string(l) + " takes over at more pixels than LOD " + std::to_string(l - 1) + ".",
				    ModelKind::Lod, 0, l, "threshold");
			for (size_t p = 0; p < lod.panm.size(); ++p) {
				const ThreediPartAnimation &pa = lod.panm[p];
				const auto on_row = [&](DiagnosticSeverity severity, const char *code, const std::string &message,
				                        const std::string &field) {
					Diagnostic d = make_diagnostic(severity, code, message, document->path(), field);
					d.row_id = row->id;
					d.record_kind = node_kind(ModelKind::PartAnimation);
					d.child_id = lod.panm_ids[p];
					d.record = document->record_path({row->id, d.record_kind, d.child_id});
					findings.push_back(std::move(d));
				};
				// The frame the pose turns the part through, by the pose's own rule
				// (threedi_panm_frame_row); the pose fails on a row past the model's MTRX table,
				// the one it is handed whole (threedi_panm_build_node_matrices).
				const int frame = threedi_panm_frame_row(pa);
				if (frame > 0 && static_cast<size_t>(frame) >= row->frames.size())
					on_row(DiagnosticSeverity::Error, "model.frame_missing",
					       "Rotation frame " + std::to_string(frame) + " is not one of the model's " +
					               std::to_string(row->frames.size()) + ".",
					       "matrix");
				for (int t = 0; t < THREEDI_PANM_TRACK_COUNT; ++t) {
					const ThreediTransform &tr = *threedi_panm_tracks(pa)[t];
					DiagnosticSeverity severity;
					std::string message;
					if (threedi_panm_track_loaded(pa, t) && threedi_generator_names_register(tr.control) &&
					    register_finding(tr.control_param,
					                     threedi_generator_reads_register(THREEDI_GENERATOR_CONSUMER_PANM, tr.control),
					                     false, "", severity, message))
						on_row(severity, "model.register_missing", message,
						       std::string(threedi_panm_track_label(t)) + ".param");
				}
			}
		}
	}
	return findings;
}

} // namespace opennova::editor
