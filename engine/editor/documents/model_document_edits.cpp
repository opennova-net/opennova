// The model document's list rules, the renumbering of what names a CTRL register or an MTRX row by its
// index, and its validator (model_document.h).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <base/io/strutil.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_kinds.h>
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
constexpr NodeKind kLight = node_kind(ModelKind::Light);

} // namespace

// A register byte the game reads through the model's CTRL table while it has one and as a global
// register while it has none: a material generator's (the second's included) or a loaded track's
// parameter above style 0x70, a flipbook's time on the register clock [orig: ThreediGp_LoadFromFile
// @ 0x5B5C49, the swaps skipped with no table; ThreediGp_LoadCtrlRegisters @ 0x5B4640 keeps no table
// for a count of 0]. A light's is swapped through the table either way (@ 0x5B5F4D..0x5B5F62). The
// first one met, its record path and field in `where`, its byte in `named`.
bool ModelDocument::reads_register_byte(const Node &node, std::string &where, int64_t &named) const {
	bool found = false;
	walk_records(node, [&](const NodeAddress &record, const Placement &) {
		if (record.kind == kLight) return true;
		const RecordHandle native = record_in(node, record);
		if (!native) return true;
		const TableKind &kind = *model_table().kind(record.kind);
		for (size_t place = 0; place < kind.fields().size(); ++place) {
			const FieldSchema &field = kind.fields()[place];
			if (field.reference != ReferenceKind::ModelRegister ||
			    named_by_index(native, place) != ReferenceKind::ModelRegister || !model_field(record.kind, place).reads(native))
				continue;
			Value value;
			if (!kind.value(place).get(native, value) || !std::holds_alternative<int64_t>(value)) continue;
			where = record_path(record) + " " + field.id;
			named = std::get<int64_t>(value);
			found = true;
			return false;
		}
		return true;
	});
	return found;
}

bool ModelDocument::accept_list_edit(const Node &node, const ListChange &change, std::string &error) const {
	if (node.kind != kModel) {
		error = "The collision records are the model's geometry: export the model again from Blender to change them.";
		return false;
	}
	const ModelRow &row = static_cast<const ModelRow &>(node);
	const NodeKind kind = model_table().kind(change.owner->record.kind)->lists()[change.list].spec.kind;
	switch (static_cast<ModelKind>(kind)) {
	case ModelKind::Material:
		if (change.operation == EditOperation::Remove &&
		    material_is_drawn(row, change.record->record.as<ModelMaterial>().source)) {
			error = "Strips draw with this material: export the model again from Blender to take it off them.";
			return false;
		}
		return true;
	case ModelKind::Register: {
		// Named by its index: what names a register the core has the type renumber after the edit
		// (renumber_references). The first register gives the model a CTRL table and the last takes
		// it away, which changes what every material's and track's register byte names (a global
		// register with no table, an entry of the table with one: reads_register_byte), so either is
		// refused while the game reads such a byte.
		const bool first = change.operation == EditOperation::Add && row.registers.empty();
		const bool last = change.operation == EditOperation::Remove && row.registers.size() == 1;
		std::string where;
		int64_t named = 0;
		if ((first || last) && reads_register_byte(row, where, named)) {
			error = first ? "A first CTRL register gives the model a table, which the load reads every material's "
			                "and track's register through: " +
			                        where + " reads global register " + std::to_string(named) +
			                        " now. Point it at no register first."
			              : "The last CTRL register removed, the load reads every material's and track's register "
			                "as a global one: " +
			                        where + " reads register " + std::to_string(named) +
			                        " of the table now. Point it at no register first.";
			return false;
		}
		return true;
	}
	case ModelKind::PartAnimation: {
		// Row i transforms part i, as every retail table does (threedi_o3d_read's rule): the next
		// part's inert row added at the end (list_position), the last removed.
		const ModelLod &lod = change.owner->record.as<ModelLod>();
		if (change.operation == EditOperation::Add) {
			if (lod.panm.size() >= lod.lod.render_object_count) {
				error = "Every part of this LOD has its part animation.";
				return false;
			}
			return true;
		}
		if (change.operation == EditOperation::Remove && change.record->step().index + 1 == lod.panm.size()) return true;
		error = "Part animations go in part order: add one at the end, or remove the last.";
		return false;
	}
	default:
		// Named by its row as a register is by its index. A frame byte of 0 names no row (the pose
		// reads one only above 0 [orig: Model_TransformBoneMatrices @ 0x58E3FE]), so row 0 is never
		// read: no pin keeps it first, since the renumbering refuses any edit that would leave a frame
		// byte the game reads naming row 0 or a row past 127 (record_index's none).
		return true;
	}
}

size_t ModelDocument::list_position(const Node &, const ListChange &change, size_t position) const {
	const NodeKind kind = model_table().kind(change.owner->record.kind)->lists()[change.list].spec.kind;
	return change.operation == EditOperation::Add && kind == node_kind(ModelKind::PartAnimation) ? SIZE_MAX : position;
}

void ModelDocument::prepare_record(const Node &, const ListChange &change, DetachedRecord &record) const {
	// A copy is drawn by no strip, and its texture rows are its own (their identities fresh).
	if (change.operation == EditOperation::Duplicate && record.kind == node_kind(ModelKind::Material))
		static_cast<ModelMaterial *>(record.data.get())->source = -1;
}

bool ModelDocument::renumber_references(const StagedRows &rows, const RecordShift &shift,
                                        std::vector<Edit> &sites, std::string &error) const {
	const bool registers = shift.reference == ReferenceKind::ModelRegister;
	// A material's and a track's register byte is an entry of the CTRL table only while the model
	// has one (reads_register_byte): an edit that gives or takes the table, which accept_list_edit
	// allows only while the game reads none of them, leaves their bytes as they are.
	const bool table = !registers || (shift.before() > 0 && shift.after > 0);
	for (const std::shared_ptr<const Node> &node : rows.rows()) {
		if (node->kind != kModel) continue;
		const ModelRow &row = static_cast<const ModelRow &>(*node);
		// The second RGB generator names a register above style 0x70 as the others do, and the load
		// swaps it through the table [orig: ThreediGp_LoadFromFile @ 0x5B5D0A..0x5B5D2A], but its
		// words are shown only (rgbgen2.param, read-only): nothing renumbers it, so an edit that
		// would change what it names is refused.
		if (registers && table)
			for (const ModelMaterial &material : row.materials) {
				const ThreediRgbGen &second = material.material.rgb_gen2;
				const int64_t index = static_cast<uint8_t>(second.reg);
				if (!threedi_generator_names_register(second.style)) continue;
				const bool moves = shift.found(index) ? shift.now(index) != size_t(index) : size_t(index) < shift.after;
				if (!moves) continue;
				error = "A material's second RGB generator names register " + std::to_string(index) +
				        ", which the editor shows and never sets: that register stays where it is.";
				return false;
			}
		bool ok = true;
		walk_records(row, [&](const NodeAddress &record, const Placement &) {
			if (registers && !table && record.kind != kLight) return true;
			const RecordHandle native = record_in(*node, record);
			if (!native) return true;
			const TableKind &kind = *model_table().kind(record.kind);
			for (size_t place = 0; place < kind.fields().size(); ++place) {
				const FieldSchema &field = kind.fields()[place];
				// A field whose value names the collection by index on this record, read by the game
				// there or not (model_document_detail::named_by_index: a track the load does not copy,
				// a frame byte on a row that turns through none), so it keeps naming its record should
				// the game read it later. The second RGB generator's, read-only, is above.
				if (field.reference != shift.reference || field.read_only ||
				    named_by_index(native, place) != shift.reference)
					continue;
				Value value;
				int64_t index = 0;
				if (!kind.value(place).get(native, value) || !record_index(shift.reference, value, index)) continue;
				// Its record's place now, or one past the collection as far as before (RecordShift::now).
				const size_t now = shift.now(index);
				if (now == size_t(index)) continue;
				int64_t named = 0;
				const bool holds = now != RecordShift::kRemoved && !(field.ranged && double(now) > field.max) &&
				                   record_index(shift.reference, int64_t(now), named);
				if (!holds) {
					// One the game does not read keeps its value; one it reads refuses the edit.
					if (!model_field(record.kind, place).reads(native)) continue;
					const std::string where = record_path(record) + " " + field.id;
					if (now == RecordShift::kRemoved)
						error = registers ? where + " reads this register: point it at another first."
						                  : where + " turns through this rotation frame: point it at another first.";
					else
						error = std::string(registers ? "Register " : "Rotation frame ") + std::to_string(index) +
						        " would move to " + std::to_string(now) + ", which " + where +
						        (registers ? " cannot hold." : " cannot name (a frame byte names rows 1 to 127).");
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
		if (collection < row->ids.lists.size() && index < row->ids.lists[collection].size()) {
			d.child_id = row->ids.lists[collection][index].id;
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
		// A generator's index is its parameter byte (generator_param in model_table.cpp).
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
		const std::vector<RecordIds> &panm_ids = row->ids.lists[kLods][l].lists[0];
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
				d.child_id = panm_ids[p].id;
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
