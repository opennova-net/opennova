#include "model_document.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/reference_kinds.h>
#include <editor/project/project_files.h>
#include <formats/threedi/threedi_build.h>
#include <runtime/renderer/material_descriptor.h>

#include "model_document_internal.h"

namespace opennova::editor {

using namespace threedi;

namespace model_document_detail {

IndexReference index_reference(const RecordHandle &record, size_t place) {
	const ModelField &field = model_field(record.kind, place);
	// A field the table declares no index of names none: no lookup more for the fields of most records.
	if (field.reference != IndexReference::Register && field.reference != IndexReference::Part &&
	    field.reference != IndexReference::Frame)
		return IndexReference::None;
	IndexReference ref = field.names(record) ? field.reference : IndexReference::None;
	if (field.reference == IndexReference::Frame && field.reads(record)) ref = IndexReference::Frame;
	return ref;
}

ReferenceKind record_reference(IndexReference index) {
	switch (index) {
	case IndexReference::Register: return ReferenceKind::ModelRegister;
	case IndexReference::Frame: return ReferenceKind::ModelFrame;
	default: return ReferenceKind::None;
	}
}

ReferenceKind named_by_index(const RecordHandle &record, size_t place) {
	const ModelField &field = model_field(record.kind, place);
	if (field.reference == IndexReference::Frame) return ReferenceKind::ModelFrame;
	if (field.reference == IndexReference::Register && field.names(record)) return ReferenceKind::ModelRegister;
	return ReferenceKind::None;
}

bool shader_flags(const char *tag, uint32_t &flags) {
	const renderer::MaterialDescriptorRecord *record = renderer::find_material_descriptor(tag);
	flags = record ? record->shader_flags : 0;
	return record != nullptr;
}

bool shader_blends(const char *tag) {
	const renderer::MaterialDescriptorRecord *record = renderer::find_material_descriptor(tag);
	return record != nullptr && record->blend != renderer::MaterialDescriptorBlend::Opaque;
}

bool material_is_drawn(const ModelRow &row, int source) {
	if (source < 0 || !row.base) return false;
	for (size_t l = 0; l < row.base->lod_count; ++l) {
		const ThreediLod &lod = row.base->lods[l];
		for (size_t s = 0; s < lod.strip_count; ++s)
			if (lod.strips[s].material_index == source) return true;
	}
	return false;
}

} // namespace model_document_detail

using model_document_detail::IndexReference;
using model_document_detail::index_reference;
using model_document_detail::model_field;
using model_document_detail::ModelField;
using model_document_detail::record_reference;

namespace {

constexpr NodeKind kModel = node_kind(ModelKind::Model);
constexpr NodeKind kCollision = node_kind(ModelKind::Collision);

// A part index's choices (any other index typed too): the parts of LOD 0, after the value the
// table calls none (a part animation's parent 255, a user point's part -1). Only an index the
// field can hold is offered.
void part_choices(const ModelRow &row, NodeKind record, const FieldSchema &field, std::vector<FieldChoice> &out) {
	const auto offer = [&](int64_t value, std::string name, std::string label) {
		if (field.ranged && (double(value) < field.min || double(value) > field.max)) return;
		out.push_back({std::move(name), value, std::move(label)});
	};
	if (record == node_kind(ModelKind::PartAnimation)) offer(255, "255", "None");
	if (record == node_kind(ModelKind::UserPoint)) offer(-1, "-1", "None");
	// Each by the add-on's name for it (PN01 for part 0, a rig's BN01: model_labels.h).
	const size_t parts = row.base && row.base->lod_count ? row.base->lods[0].render_object_count : 0;
	for (size_t i = 0; i < parts; ++i) offer(int64_t(i), std::to_string(i), model_part_name(row, int64_t(i)));
}

} // namespace

ModelRow::ModelRow() { kind = kModel; }

std::shared_ptr<Node> ModelRow::clone() const { return std::make_shared<ModelRow>(*this); }

RecordHandle ModelRow::record() const { return {kModel, const_cast<ModelRow *>(this)}; }

size_t ModelRow::footprint() const {
	size_t bytes = sizeof(ModelRow) + ids_footprint() + footprint_of(lods) + footprint_of(materials) +
	               footprint_of(lights) + footprint_of(user_points) + footprint_of(registers) + footprint_of(frames);
	for (const ModelLod &lod : lods) bytes += footprint_of(lod.panm);
	return bytes;
}

CollisionRow::CollisionRow() { kind = kCollision; }

RecordHandle CollisionRow::record() const { return {kCollision, const_cast<CollisionRow *>(this)}; }

size_t CollisionRow::footprint() const {
	return sizeof(CollisionRow) + ids_footprint() + footprint_of(sections) + footprint_of(volumes) +
	       footprint_of(faces) + footprint_of(occlusion);
}

bool is_model_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::Model;
}

void ModelDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	// Whether the game reads the field here and what it names here: the table's labelled field (a
	// generator's register only above style 0x70, a loaded track, a spot light's axis; a texture row's
	// file where its slot loads one), which the model's own rules below narrow.
	TableDocument::refine_field(address, use);
	const Node *node = row(address.row);
	Located at;
	if (!node || !locate(*node, address.child, at) || at.record.kind != address.kind) return;
	const size_t place = model_table().kind(at.record.kind)->place_of(*use.schema);
	if (place == TableKind::npos) return;
	// What an index names is decided below by what the model row holds, never by the declaration alone (a
	// texture's file and a material's shader name no index).
	if (use.reference != ReferenceKind::Texture && use.reference != ReferenceKind::Shader)
		use.reference = ReferenceKind::None;
	// An index names what the model row holds: its registers and frames are its records, named
	// by their index (a Record reference, S13 D8: the picker offers them, the core renumbers it);
	// LOD 0's parts are not records (record_choices offers them). A model with no CTRL table skips
	// the material and part-animation swaps, so their bytes stay the global registers they number
	// [orig: ThreediGp_LoadFromFile @ 0x5B5C49], no record of the model; a light's pass runs either
	// way [orig: ThreediGp_LoadFromFile @ 0x5B5F4D..0x5B5F62] (with no table it faults the load,
	// the validator's error).
	if (node->kind == kModel) {
		const IndexReference named = index_reference(at.record, place);
		const bool swapped = !static_cast<const ModelRow &>(*node).registers.empty() ||
		                     at.record.kind == node_kind(ModelKind::Light);
		if (named == IndexReference::Part) use.own_choices = true;
		else if (named == IndexReference::Frame || (named == IndexReference::Register && swapped))
			use.reference = record_reference(named);
	}
	// A texture row's name loads the file its type's loader picks (reference_file_candidates); its slot,
	// its flags and its material's alpha test say what the use makes of it (ADR 0046 S18, the texture
	// checks: TextureRowContext).
	if (use.reference == ReferenceKind::Texture && at.record.kind == node_kind(ModelKind::Texture)) {
		const ThreediMaterialTexture &texture = at.record.as<ThreediMaterialTexture>();
		use.loader_arg = texture.type;
		TextureRowContext context;
		context.slot = texture.slot;
		context.row_flags = texture.flags;
		if (!at.is_row() && at.step().owner.kind == node_kind(ModelKind::Material)) {
			const ThreediMaterial &material = at.step().owner.as<ThreediMaterial>();
			// The alpha test as it falls on this row: the one whose alpha the material's technique cuts out by.
			context.material_flags =
					texture_row_material_flags(material.shader_name, material.material_flags, texture.type, texture.slot);
			context.alpha_ref = material.alpha_test_value_byte;
		}
		use.use_context = pack_texture_row_context(context);
	}
	// A user point is looked up on the model a record names by its file (an item's graphic, a weapon's gfx3):
	// one of the first 16 in their section too, which an item's particle slot reads alone [orig:
	// ItemDef_GetBoneMaskByName @ 0x49ea40, the scan end @ 0x49ea73]; every other lookup reads every point
	// [orig: modelgpm_FindUserpointByName @ 0x5b2170].
	if (use.defines == ReferenceKind::UserPoint) {
		Placement point;
		const bool first_16 = placement(address, point) && point.index < size_t(THREEDI_USER_POINT_SCAN_LIMIT);
		use.scope = user_point_scope(basename_of(path()), first_16);
	}
}

bool ModelDocument::record_choices(const NodeAddress &address, const FieldUse &use,
		std::vector<FieldChoice> &out) const {
	const Node *node = row(address.row);
	Located at;
	if (!node || node->kind != kModel || !locate(*node, address.child, at) || at.record.kind != address.kind)
		return false;
	const size_t place = model_table().kind(at.record.kind)->place_of(*use.schema);
	if (place == TableKind::npos || index_reference(at.record, place) != IndexReference::Part) return false;
	part_choices(static_cast<const ModelRow &>(*node), at.record.kind, *use.schema, out);
	return true;
}

std::string ModelDocument::record_title(const NodeAddress &address) const {
	const std::string title = model_record_label(*this, address, nullptr);
	return title.empty() ? TableDocument::record_title(address) : title;
}

const ModelRow *ModelDocument::model_row() const {
	for (const auto &r : rows())
		if (r && r->kind == kModel) return static_cast<const ModelRow *>(r.get());
	return nullptr;
}

const CollisionRow *ModelDocument::collision_row() const {
	for (const auto &r : rows())
		if (r && r->kind == kCollision) return static_cast<const CollisionRow *>(r.get());
	return nullptr;
}

void ModelDocument::compose(ComposedModel &out) const {
	if (const ModelRow *row = model_row()) compose_model(*row, collision_row(), out);
}

void compose_model(const ModelRow &model, const CollisionRow *collision, ComposedModel &out) {
	const ModelRow *row = &model;
	if (!row->base) return;
	const Threedi3di3 &base = *row->base;
	out.base = row->base;
	out.model = base; // shallow: the geometry is the base's
	out.model.header = row->header;

	// Materials in their new order; a strip follows its material there.
	std::vector<int32_t> renumber(base.material_count, -1);
	out.materials.reserve(row->materials.size());
	for (size_t i = 0; i < row->materials.size(); ++i) {
		ThreediMaterial m = row->materials[i].material;
		m.index = static_cast<int32_t>(i);
		out.materials.push_back(m);
		const int source = row->materials[i].source;
		if (source >= 0 && static_cast<uint32_t>(source) < base.material_count) renumber[source] = static_cast<int32_t>(i);
	}
	out.model.materials = out.materials.empty() ? nullptr : out.materials.data();
	out.model.material_count = static_cast<uint32_t>(out.materials.size());

	// LODs: the base's geometry, the rows' threshold, type and PANM. A LOD whose file
	// carries a PANM chunk keeps a table of its own (an empty one too); one that carries
	// none and gained no row keeps none, and the writer writes the model-level table for
	// it, as it does for the file as read. The model-level table is the first LOD's that
	// carries one, as the reader finds it (find_first_chunk).
	static const ThreediPartAnimation kNoRows[1] = {};
	out.lods.resize(row->lods.size());
	out.strips.resize(row->lods.size());
	out.panm.resize(row->lods.size());
	bool model_level = false;
	for (size_t l = 0; l < row->lods.size(); ++l) {
		ThreediLod &lod = out.lods[l];
		lod = row->lods[l].lod;
		out.strips[l].assign(lod.strips, lod.strips + lod.strip_count);
		for (ThreediTriangleStrip &s : out.strips[l])
			if (s.material_index >= 0 && static_cast<size_t>(s.material_index) < renumber.size())
				s.material_index = renumber[s.material_index];
		lod.strips = out.strips[l].empty() ? nullptr : out.strips[l].data();
		out.panm[l] = row->lods[l].panm;
		const bool own = base.lods[l].part_animations != nullptr || !out.panm[l].empty();
		lod.part_animations = !own ? nullptr
		                      : out.panm[l].empty() ? const_cast<ThreediPartAnimation *>(kNoRows)
		                                            : out.panm[l].data();
		lod.part_animation_count = out.panm[l].size();
		if (own && !model_level) {
			model_level = true;
			out.model.part_animations = lod.part_animations;
			out.model.part_animation_count = lod.part_animation_count;
		}
	}
	if (!model_level) {
		out.model.part_animations = nullptr;
		out.model.part_animation_count = 0;
	}
	out.model.lods = out.lods.empty() ? nullptr : out.lods.data();
	out.model.lod_count = out.lods.size();

	out.lights = row->lights;
	out.model.lights = out.lights.empty() ? nullptr : out.lights.data();
	out.model.light_count = out.lights.size();
	out.user_points = row->user_points;
	out.model.user_points = out.user_points.empty() ? nullptr : out.user_points.data();
	out.model.user_point_count = out.user_points.size();
	out.registers = row->registers;
	out.model.ctrl.registers = out.registers.empty() ? nullptr : out.registers.data();
	out.model.ctrl.count = static_cast<uint32_t>(out.registers.size());
	out.frames = row->frames;
	out.model.mtrx.matrices = out.frames.empty() ? nullptr : out.frames.data();
	out.model.mtrx.count = static_cast<uint32_t>(out.frames.size());

	if (collision && base.collision) {
		out.collision = *base.collision;
		out.volumes = collision->volumes;
		out.faces = collision->faces;
		out.collision.volumes = out.volumes.empty() ? nullptr : out.volumes.data();
		out.collision.faces = out.faces.empty() ? nullptr : out.faces.data();
		out.model.collision = &out.collision;
	}
	if (collision) {
		out.occlusion = collision->occlusion;
		out.model.occlusion_objects = out.occlusion.empty() ? nullptr : out.occlusion.data();
		out.model.occlusion_object_count = out.occlusion.size();
	}
}

namespace {

// One record of the format, and a list of them, alike byte for byte.
template <class T> bool same_bytes(const T &a, const T &b) {
	static_assert(std::is_trivially_copyable<T>::value, "a format record compares as its bytes");
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}
template <class T> bool same_bytes(const std::vector<T> &a, const std::vector<T> &b) {
	static_assert(std::is_trivially_copyable<T>::value, "a format record compares as its bytes");
	return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

} // namespace

bool alike_but_user_points(const ModelRow &a, const ModelRow &b) {
	if (a.base != b.base || !same_bytes(a.header, b.header) || a.lods.size() != b.lods.size() ||
			!same_bytes(a.materials, b.materials) || !same_bytes(a.lights, b.lights) ||
			!same_bytes(a.registers, b.registers) || !same_bytes(a.frames, b.frames))
		return false;
	for (size_t l = 0; l < a.lods.size(); ++l)
		if (!same_bytes(a.lods[l].lod, b.lods[l].lod) || !same_bytes(a.lods[l].panm, b.lods[l].panm)) return false;
	return true;
}

SerializeResult ModelDocument::serialize() const {
	SerializeResult result;
	ComposedModel composed;
	compose(composed);
	std::vector<uint8_t> bytes;
	if (!composed.base || threedi_3di3_write_memory(&composed.model, bytes) != 0) {
		result.issues.push_back({true, 0, "", "", "The model could not be written."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

bool ModelDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                          std::shared_ptr<const FileState> &, std::vector<SourceIssue> &, Diagnostic &error) {
	if (!is_model_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a model.", path());
		return false;
	}
	const assets::Model base = assets::parse_model(bytes.data(), bytes.size());
	if (!base) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, "The model could not be read.", path());
		return false;
	}
	auto row = std::make_shared<ModelRow>();
	row->base = base;
	row->header = base->header;
	for (size_t l = 0; l < base->lod_count; ++l) {
		ModelLod lod;
		lod.lod = base->lods[l];
		const ThreediLod &source = base->lods[l];
		lod.panm.assign(source.part_animations, source.part_animations + source.part_animation_count);
		row->lods.push_back(std::move(lod));
	}
	for (uint32_t m = 0; m < base->material_count; ++m) {
		ModelMaterial material;
		material.material = base->materials[m];
		material.source = static_cast<int>(m);
		row->materials.push_back(std::move(material));
	}
	row->lights.assign(base->lights, base->lights + base->light_count);
	row->user_points.assign(base->user_points, base->user_points + base->user_point_count);
	row->registers.assign(base->ctrl.registers, base->ctrl.registers + base->ctrl.count);
	row->frames.assign(base->mtrx.matrices, base->mtrx.matrices + base->mtrx.count);
	shape(*row);
	rows.push_back(row);

	auto collision = std::make_shared<CollisionRow>();
	if (base->collision) {
		const ThreediCollisionModel &c = *base->collision;
		collision->sections.assign(c.objects, c.objects + c.object_count);
		collision->volumes.assign(c.volumes, c.volumes + c.volume_count);
		collision->faces.assign(c.faces, c.faces + c.face_count);
	}
	collision->occlusion.assign(base->occlusion_objects, base->occlusion_objects + base->occlusion_object_count);
	shape(*collision);
	rows.push_back(collision);
	return true;
}

std::shared_ptr<Node> ModelDocument::make_node(NodeKind, NodeId,
                                               const std::vector<std::shared_ptr<const Node>> &,
                                               std::string &error) {
	error = "A model keeps its model and collision rows; add records inside them.";
	return nullptr;
}

bool ModelDocument::set_value(Node &node, const Located &at, size_t field, const Value &value, std::string &error) {
	const std::string &id = model_table().kind(at.record.kind)->fields()[field].id;
	if (at.record.kind != node_kind(ModelKind::Material) || id != "shader")
		return TableDocument::set_value(node, at, field, value, error);

	// A shader decides the draw pass and the vertex layout (geometry), and the words
	// threedi_build_material_surface derives.
	ModelMaterial &held = at.record.as<ModelMaterial>();
	ThreediMaterial &material = held.material;
	const std::string *tag = std::get_if<std::string>(&value);
	if (tag == nullptr) {
		error = "A shader is a tag.";
		return false;
	}
	const ModelRow &row = static_cast<const ModelRow &>(node);
	const bool drawn = model_document_detail::material_is_drawn(row, held.source);
	uint32_t flags = 0;
	const bool known = model_document_detail::shader_flags(tag->c_str(), flags);
	if (drawn && known) {
		bool tangents = false;
		for (size_t l = 0; l < row.base->lod_count; ++l)
			tangents = tangents || (row.base->lods[l].vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) == THREEDI_VERTEX_FLAG_TANGENTS;
		if ((flags & renderer::MATERIAL_FLAG_TANGENT) && !tangents) {
			error = "This shader reads tangents the model's vertices do not carry: export the model again from Blender with it.";
			return false;
		}
		if (model_document_detail::shader_blends(tag->c_str()) != model_document_detail::shader_blends(material.shader_name)) {
			error = "This shader draws in another pass than the material's strips were built for: export the model again from Blender with it.";
			return false;
		}
	}
	if (!TableDocument::set_value(node, at, field, value, error)) return false;
	threedi_build_material_surface(material, (flags & renderer::MATERIAL_FLAG_GLASS) != 0,
	                               (flags & renderer::MATERIAL_FLAG_EMISSIVE) != 0);
	return true;
}

bool ModelDocument::accept_step(const EditStep &step, const StagedRows &,
                                StepRefusal &refusal) const {
	for (const RowSwap &swap : step.swaps) {
		if (swap.before && swap.after) continue;
		refusal.message = "A model keeps its model and collision rows.";
		return false;
	}
	return true;
}

} // namespace opennova::editor
