// The model document's structural edits, the renumbering of what names a CTRL register or an
// MTRX row by its index, and its validator (model_document.h).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <base/io/strutil.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/id_list.h>
#include <editor/model/staged_rows.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>

#include "model_document_internal.h"

namespace opennova::editor {

using namespace threedi;
using namespace model_document_detail;

namespace {

constexpr NodeKind kModel = node_kind(ModelKind::Model);

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

ThreediControlRegister fresh_register() {
	ThreediControlRegister made{};
	std::snprintf(made.name, sizeof(made.name), "%s", "LOD_FRAC");
	return made;
}

ThreediMatrix4x4 fresh_frame() {
	ThreediMatrix4x4 identity;
	threedi_mat4_identity(&identity);
	return identity;
}

} // namespace

bool ModelDocument::edit_collection(Node &node, const Edit &edit, const IdAllocator &allocate, NodeId &added,
                                    std::string &error) {
	if (node.kind != kModel) {
		error = "The collision records are the model's geometry: export the model again from Blender to change them.";
		return false;
	}
	ModelRow &row = static_cast<ModelRow &>(node);
	const bool adding = edit.operation == EditOperation::Add;
	// Where the record sits (its table's index, and its own index in a LOD's or a material's list),
	// and the LOD or material an Add names (none: the row), by their paths in the row as the edit
	// found it, read before anything changes.
	uint32_t table = 0, own = 0, owner_table = 0, owner = 0;
	bool owned = false;
	if (!adding) {
		const RecordPath at = path_in(row, edit.address.child);
		if (at.empty()) {
			error = "The record no longer exists.";
			return false;
		}
		table = at[0].index;
		own = at[at.size() - 1].index;
	} else if (edit.parent != 0 && edit.parent != row.id) {
		const RecordPath at = path_in(row, edit.parent);
		if (at.size() != 1) {
			error = "The owner no longer exists.";
			return false;
		}
		owned = true;
		owner_table = at[0].collection;
		owner = at[0].index;
	}
	switch (static_cast<ModelKind>(edit.address.kind)) {
	case ModelKind::Material: {
		if (edit.operation == EditOperation::Remove && material_is_drawn(row, row.materials[table].source)) {
			error = "Strips draw with this material: export the model again from Blender to take it off them.";
			return false;
		}
		ModelMaterial fresh;
		fresh.material = fresh_material();
		if (!edit_id_list(row.materials, row.collections[kMaterials], edit, std::move(fresh), allocate, added, error))
			return false;
		// A copy is drawn by no strip, and its texture rows are its own.
		if (edit.operation == EditOperation::Duplicate) {
			ModelMaterial &copy = row.materials[std::min(edit.position, row.materials.size() - 1)];
			copy.source = -1;
			for (NodeId &id : copy.texture_ids) id = allocate();
		}
		return true;
	}
	case ModelKind::Texture: {
		if (adding && (!owned || owner_table != kMaterials)) {
			error = "A texture row belongs to a material.";
			return false;
		}
		ModelMaterial &material = row.materials[adding ? owner : table];
		std::vector<ThreediMaterialTexture> list(material.material.textures,
		                                         material.material.textures + material.texture_ids.size());
		if ((adding || edit.operation == EditOperation::Duplicate) && list.size() >= 24) {
			error = "A material holds 24 texture rows.";
			return false;
		}
		ThreediMaterialTexture fresh{};
		fresh.slot = static_cast<uint8_t>(THREEDI_TEX_SLOT_DIFFUSE);
		if (!edit_id_list(list, material.texture_ids, edit, fresh, allocate, added, error)) return false;
		std::memset(material.material.textures, 0, sizeof(material.material.textures));
		std::copy(list.begin(), list.end(), material.material.textures);
		material.material.texture_count = static_cast<uint32_t>(list.size());
		return true;
	}
	case ModelKind::Light:
		return edit_id_list(row.lights, row.collections[kLights], edit, fresh_light(), allocate, added, error);
	case ModelKind::UserPoint:
		return edit_id_list(row.user_points, row.collections[kUserPoints], edit, fresh_user_point(row), allocate,
		                    added, error);
	case ModelKind::Register:
		// Named by its index: what names a register the core has the type renumber after the edit
		// (renumber_references).
		return edit_id_list(row.registers, row.collections[kRegisters], edit, fresh_register(), allocate, added, error);
	case ModelKind::Frame:
		// MTRX row 0 is the identity every model's table starts with, which no part animation turns
		// through (a frame byte names a row above 0, threedi_panm_frame_row): it stays row 0. Any
		// other row moves as a register does.
		if ((!adding && table == 0) || (edit.operation != EditOperation::Remove && edit.position == 0)) {
			error = "Rotation frame 0 is the identity every model keeps first: no part animation turns through it.";
			return false;
		}
		return edit_id_list(row.frames, row.collections[kFrames], edit, fresh_frame(), allocate, added, error);
	case ModelKind::PartAnimation: {
		if (adding && (!owned || owner_table != kLods)) {
			error = "A part animation belongs to a LOD.";
			return false;
		}
		ModelLod &lod = row.lods[adding ? owner : table];
		// Row i transforms part i, as every retail table does (threedi_o3d_read's rule): the next
		// part's inert row added at the end, the last removed.
		if (adding) {
			if (lod.panm.size() >= lod.lod.render_object_count) {
				error = "Every part of this LOD has its part animation.";
				return false;
			}
			const int part = static_cast<int>(lod.panm.size());
			const int parent = part < static_cast<int>(lod.lod.render_object_count) ? lod.lod.render_objects[part].parent_index : -1;
			Edit at_end = edit;
			at_end.position = SIZE_MAX;
			return edit_id_list(lod.panm, lod.panm_ids, at_end, threedi_build_inert_panm(part, parent), allocate,
			                    added, error);
		}
		if (edit.operation != EditOperation::Remove || own + 1 != lod.panm.size()) {
			error = "Part animations go in part order: add one at the end, or remove the last.";
			return false;
		}
		return edit_id_list(lod.panm, lod.panm_ids, edit, ThreediPartAnimation{}, allocate, added, error);
	}
	default:
		error = "LODs are the model's geometry: export the model again from Blender to change them.";
		return false;
	}
}

bool ModelDocument::renumber_references(const StagedRows &rows, const RecordShift &shift,
                                        std::vector<Edit> &sites, std::string &error) const {
	const bool registers = shift.reference == ReferenceKind::ModelRegister;
	for (const std::shared_ptr<const Node> &node : rows.rows()) {
		if (node->kind != kModel) continue;
		const ModelRow &row = static_cast<const ModelRow &>(*node);
		// The second RGB generator names a register above style 0x70 as the others do (the load
		// swaps it, ThreediGp_LoadFromFile's material pass), but its words are shown only
		// (rgbgen2.param, unwitnessed): no field renumbers it, so a register it names stays where
		// it is.
		if (registers)
			for (const ModelMaterial &material : row.materials) {
				const ThreediRgbGen &second = material.material.rgb_gen2;
				const int64_t index = static_cast<uint8_t>(second.reg);
				if (!threedi_generator_names_register(second.style) || !shift.found(index) ||
				    shift.now(index) == size_t(index))
					continue;
				error = "A material's second RGB generator names register " + std::to_string(index) +
				        ", which the editor shows and never sets: that register stays where it is.";
				return false;
			}
		bool ok = true;
		walk_records(row, [&](const NodeAddress &record, const Placement &) {
			const ThreediSchemaRecord native = record_of(*this, const_cast<Node &>(*node), record);
			if (!native) return true;
			for (const FieldSchema &field : schema(record.kind)) {
				// A field that names the collection here (refine_field's rule), read by the game or
				// not: one it reads only later keeps naming its record.
				if (field.reference != shift.reference || record_reference(native, field.id) != shift.reference)
					continue;
				Value value;
				int64_t index = 0;
				if (!threedi_schema_get(native, field.id, value) || !record_index(shift.reference, value, index) ||
				    !shift.found(index))
					continue;
				const size_t now = shift.now(index);
				if (now == RecordShift::kRemoved) {
					error = registers ? "Something reads this register: point it at another first."
					                  : "A part animation turns through this frame: point it at another first.";
					ok = false;
					return false;
				}
				if (now == size_t(index)) continue;
				int64_t named = 0;
				if ((field.ranged && double(now) > field.max) ||
				    !record_index(shift.reference, int64_t(now), named)) {
					error = std::string(registers ? "Register " : "Rotation frame ") + std::to_string(index) +
					        " would move to " + std::to_string(now) + ", which the field " + field.id +
					        " naming it cannot hold.";
					ok = false;
					return false;
				}
				Edit set;
				set.address = record;
				set.field = field.id;
				set.value = int64_t(now);
				sites.push_back(std::move(set));
			}
			return true;
		});
		if (!ok) return false;
	}
	return true;
}

namespace {

constexpr FindingCodeEntry<ModelFinding> kFindingEntries[] = {
	{ ModelFinding::Seats, { "model.seats" } },
	{ ModelFinding::UserPointDuplicate, { "model.user_point_duplicate" } },
	{ ModelFinding::UserPoints, { "model.user_points" } },
	{ ModelFinding::RegisterMissing, { "model.register_missing" } },
	{ ModelFinding::ShaderUnknown, { "model.shader_unknown" } },
	{ ModelFinding::MaterialUnused, { "model.material_unused" } },
	{ ModelFinding::LightPart, { "model.light_part" } },
	{ ModelFinding::RegisterUnknown, { "model.register_unknown" } },
	{ ModelFinding::LodOrder, { "model.lod_order" } },
	{ ModelFinding::FrameMissing, { "model.frame_missing" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(ModelFinding::kCount),
		"every ModelFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the model's rows follow ModelFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Models);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(ModelFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable model_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_model_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *model = dynamic_cast<const ModelDocument *>(&document);
	const ModelRow *row = model ? model->model_row() : nullptr;
	if (!row) return findings;
	const auto add = [&](DiagnosticSeverity severity, ModelFinding code, const std::string &message, ModelKind kind,
	                     size_t collection, size_t index, const char *field) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = row->id;
		d.record_kind = node_kind(kind);
		if (collection < row->collections.size() && index < row->collections[collection].size()) {
			d.child_id = row->collections[collection][index];
			d.record = model->record_path({row->id, node_kind(kind), d.child_id});
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
			add(DiagnosticSeverity::Warning, ModelFinding::Seats,
			    "More than " + std::to_string(THREEDI_SITEX_SEAT_LIMIT) +
			            " sitex seats: the game takes the ninth for the control seat, and its seat scan reads no "
			            "user point after it.",
			    ModelKind::UserPoint, 3, i, "name");
		if (!names.emplace(strutil::to_upper(u.name), i).second)
			add(DiagnosticSeverity::Warning, ModelFinding::UserPointDuplicate,
			    std::string("Two user points are named '") + u.name + "'; a lookup by name finds the first.",
			    ModelKind::UserPoint, 3, i, "name");
	}
	if (row->user_points.size() > THREEDI_USER_POINT_SCAN_LIMIT)
		add(DiagnosticSeverity::Info, ModelFinding::UserPoints,
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
			add(severity, ModelFinding::RegisterMissing, message, kind, collection, at, field);
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
			add(DiagnosticSeverity::Warning, ModelFinding::ShaderUnknown,
			    std::string("The engine's shader table has no '") + mt.shader_name + "'.", ModelKind::Material, 1, m, "shader");
		if (row->materials[m].source < 0 || !material_is_drawn(*row, row->materials[m].source))
			add(DiagnosticSeverity::Info, ModelFinding::MaterialUnused, "No strip draws with this material.",
			    ModelKind::Material, 1, m, "shader");
	}
	for (size_t i = 0; i < row->lights.size(); ++i) {
		const ThreediLight &l = row->lights[i];
		if (threedi_generator_names_register(l.style))
			check_register(l.phase, threedi_generator_reads_register(THREEDI_GENERATOR_CONSUMER_LIGHT, l.style),
			               ModelKind::Light, 2, i, "param", "");
		if (l.subobj_index != 0 && (row->lods.empty() || l.subobj_index >= row->lods[0].lod.render_object_count))
			add(DiagnosticSeverity::Error, ModelFinding::LightPart, "This light names a part LOD 0 does not have.",
			    ModelKind::Light, 2, i, "part");
	}
	for (size_t i = 0; i < row->registers.size(); ++i)
		if (threedi_ctrl_register_ordinal(row->registers[i].name) == THREEDI_CTRL_REGISTER_NOT_FOUND)
			add(DiagnosticSeverity::Warning, ModelFinding::RegisterUnknown,
			    std::string("'") + row->registers[i].name + "' is no CTRL register the engine knows: the game reads LOD_FRAC.",
			    ModelKind::Register, 4, i, "name");
	for (size_t l = 0; l < row->lods.size(); ++l) {
		const ModelLod &lod = row->lods[l];
		if (l > 0 && lod.lod.lod_threshold > row->lods[l - 1].lod.lod_threshold)
			add(DiagnosticSeverity::Warning, ModelFinding::LodOrder,
			    "LOD " + std::to_string(l) + " takes over at more pixels than LOD " + std::to_string(l - 1) + ".",
			    ModelKind::Lod, 0, l, "threshold");
		for (size_t p = 0; p < lod.panm.size(); ++p) {
			const ThreediPartAnimation &pa = lod.panm[p];
			const auto on_row = [&](DiagnosticSeverity severity, ModelFinding code, const std::string &message,
			                        const std::string &field) {
				Diagnostic d = make_finding(code, severity, message, document.path(), field);
				d.row_id = row->id;
				d.record_kind = node_kind(ModelKind::PartAnimation);
				d.child_id = lod.panm_ids[p];
				d.record = model->record_path({row->id, d.record_kind, d.child_id});
				findings.push_back(std::move(d));
			};
			// The frame the pose turns the part through, by the pose's own rule
			// (threedi_panm_frame_row); the pose fails on a row past the model's MTRX table,
			// the one it is handed whole (threedi_panm_build_node_matrices).
			const int frame = threedi_panm_frame_row(pa);
			if (frame > 0 && static_cast<size_t>(frame) >= row->frames.size())
				on_row(DiagnosticSeverity::Error, ModelFinding::FrameMissing,
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
					on_row(severity, ModelFinding::RegisterMissing, message,
					       std::string(threedi_panm_track_label(t)) + ".param");
			}
		}
	}
	return findings;
}

} // namespace opennova::editor
