#include "model_document.h"

#include <algorithm>
#include <cstring>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/project/project_files.h>
#include <formats/threedi/threedi_build.h>
#include <runtime/renderer/material_descriptor.h>

#include "model_document_internal.h"

namespace opennova::editor {

using namespace threedi;
using model_document_detail::KindRow;
using model_document_detail::kKinds;
using model_document_detail::place_of;
using Place = ModelPlace;

namespace model_document_detail {

// Every identity of a row by where it sits: made when the row is given its identities
// (for_each_identity) and by each structural edit of a clone (index_places), never inside a
// const query, so a committed row's places are there and never change.
const ModelPlaces &places_of(const Node &node) {
	static const ModelPlaces none;
	const std::shared_ptr<const ModelPlaces> &places = node.kind == node_kind(ModelKind::Model)
			? static_cast<const ModelRow &>(node).places
			: static_cast<const CollisionRow &>(node).places;
	return places ? *places : none;
}

void index_places(Node &node) {
	auto made = std::make_shared<ModelPlaces>();
	for (uint8_t c = 0; c < node.collections.size(); ++c)
		for (uint32_t i = 0; i < node.collections[c].size(); ++i)
			(*made)[node.collections[c][i]] = {c, 0, i};
	if (node.kind != node_kind(ModelKind::Model)) {
		static_cast<CollisionRow &>(node).places = made;
		return;
	}
	ModelRow &row = static_cast<ModelRow &>(node);
	for (uint32_t l = 0; l < row.lods.size(); ++l)
		for (uint32_t i = 0; i < row.lods[l].panm_ids.size(); ++i)
			(*made)[row.lods[l].panm_ids[i]] = {kPanmSlot, l, i};
	for (uint32_t m = 0; m < row.materials.size(); ++m)
		for (uint32_t i = 0; i < row.materials[m].texture_ids.size(); ++i)
			(*made)[row.materials[m].texture_ids[i]] = {kTextureSlot, m, i};
	row.places = made;
}

bool place_of(const Node &row, NodeId id, ModelPlace &out) {
	const ModelPlaces &places = places_of(row);
	const auto found = places.find(id);
	if (found == places.end()) return false;
	out = found->second;
	return true;
}

ThreediSchemaRecord record_of(Node &node, const NodeAddress &address) {
	if (node.kind == node_kind(ModelKind::Model)) {
		ModelRow &row = static_cast<ModelRow &>(node);
		if (address.child == 0) return {ThreediSchemaShape::Model, &row.header};
		Place p;
		if (!place_of(row, address.child, p)) return {};
		switch (p.collection) {
		case 0: return {ThreediSchemaShape::Lod, &row.lods[p.index].lod};
		case 1: return {ThreediSchemaShape::Material, &row.materials[p.index].material};
		case 2: return {ThreediSchemaShape::Light, &row.lights[p.index]};
		case 3: return {ThreediSchemaShape::UserPoint, &row.user_points[p.index]};
		case 4: return {ThreediSchemaShape::Register, &row.registers[p.index]};
		case 5: return {ThreediSchemaShape::Frame, &row.frames[p.index]};
		case kPanmSlot: return {ThreediSchemaShape::PartAnimation, &row.lods[p.owner].panm[p.index]};
		case kTextureSlot: return {ThreediSchemaShape::Texture, &row.materials[p.owner].material.textures[p.index]};
		default: return {};
		}
	}
	CollisionRow &row = static_cast<CollisionRow &>(node);
	Place p;
	if (address.child == 0 || !place_of(row, address.child, p)) return {};
	switch (p.collection) {
	case 0: return {ThreediSchemaShape::Section, &row.sections[p.index]};
	case 1: return {ThreediSchemaShape::Volume, &row.volumes[p.index]};
	case 2: return {ThreediSchemaShape::Face, &row.faces[p.index]};
	case 3: return {ThreediSchemaShape::Occlusion, &row.occlusion[p.index]};
	default: return {};
	}
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

using model_document_detail::kPanmSlot;
using model_document_detail::kTextureSlot;
using model_document_detail::record_of;

namespace {

constexpr NodeKind kModel = node_kind(ModelKind::Model);
constexpr NodeKind kCollision = node_kind(ModelKind::Collision);

// The heading a field's first step groups it under.
const char *section_of(const std::string &path) {
	static const struct {
		const char *step, *label;
	} kSections[] = {
		{"position", "Position"}, {"direction", "Direction"}, {"start", "Start colour"}, {"end", "End colour"},
		{"flags", "Flags"}, {"texanim", "Flipbook"}, {"reflect", "Reflection"}, {"rgbgen", "Colour generator"},
		{"alphagen", "Alpha generator"}, {"ugen", "U generator"}, {"vgen", "V generator"}, {"offset", "Offset"},
		{"rotx", "Rotation X track"}, {"roty", "Rotation Y track"}, {"rotz", "Rotation Z track"},
		{"scalex", "Scale X track"}, {"scaley", "Scale Y track"}, {"scalez", "Scale Z track"},
		{"trans", "Translation track"}, {"rgbgen2", "Second colour generator"}, {"reflect2", "Second reflection"},
	};
	const size_t dot = path.find('.');
	if (dot == std::string::npos) return "";
	const std::string step = path.substr(0, dot);
	for (const auto &s : kSections)
		if (step == s.step) return s.label;
	return "";
}

FieldSchema field_of(const ThreediSchemaField &f) {
	FieldSchema out;
	out.id = f.path;
	out.type = f.type == ThreediSchemaType::Integer ? FieldType::Integer
	           : f.type == ThreediSchemaType::Real  ? FieldType::Real
	                                                 : FieldType::Text;
	out.width = f.width;
	out.reference = f.reference == ThreediSchemaReference::Texture ? ReferenceKind::Texture : ReferenceKind::None;
	for (const ThreediSchemaChoice &c : f.choices) out.choices.push_back({c.name, c.value, c.label});
	out.flags = f.flags;
	out.read_only = f.read_only;
	out.unit = f.unit;
	out.group = f.group;
	out.description = f.note;
	if (f.channel) out.color = FieldColor::Channel;
	if (f.unverified) out.applies = Applicability::Unverified;
	// An index (a CTRL register, a part of LOD 0, an MTRX row) takes any other index typed
	// beside the ones a record offers of its own (ModelDocument::record_choices).
	const ThreediSchemaReference named = f.reference;
	if (named == ThreediSchemaReference::Register || named == ThreediSchemaReference::Part ||
			named == ThreediSchemaReference::Frame)
		out.open_choices = true;
	// An integer keeps to the range its record's word holds (threedi_schema_set refuses past it).
	if (f.type == ThreediSchemaType::Integer && f.min < f.max) {
		out.ranged = true;
		out.min = double(f.min);
		out.max = double(f.max);
	}
	out.label = f.label; // the table's own ("" = the path, which field_title shows)
	out.section = section_of(f.path);
	return out;
}

// The shader tags the engine's table knows, as the shader field's choices.
std::vector<FieldChoice> shader_choices() {
	std::vector<FieldChoice> out;
	for (size_t i = 0; i < renderer::kMaterialDescriptorTableCount; ++i)
		out.push_back({renderer::kMaterialDescriptorTable[i].name, static_cast<int64_t>(i), ""});
	return out;
}

// What an index field names on this record (threedi_schema_reference, `ref`): a CTRL
// register, a part of LOD 0 or an MTRX row; a frame byte names the MTRX rows wherever its row
// turns through one (a spinner or Euler row: threedi_schema_reads), whatever it names now, so
// "none" is no dead end. None for any other field.
ThreediSchemaReference index_reference(const ThreediSchemaRecord &record, const std::string &path,
                                       ThreediSchemaReference ref) {
	const ThreediSchemaField *schema = threedi_schema_field(record.shape, path);
	if (schema && schema->reference == ThreediSchemaReference::Frame &&
			threedi_schema_reads(record, path))
		ref = ThreediSchemaReference::Frame;
	const bool index = ref == ThreediSchemaReference::Register ||
			ref == ThreediSchemaReference::Part || ref == ThreediSchemaReference::Frame;
	return index ? ref : ThreediSchemaReference::None;
}

// What an index field names on this record (index_reference), as its choices (any other index
// typed too): a CTRL register by its name, a part of LOD 0, an MTRX row; with the value the
// table calls none (a part animation's parent 255, a user point's part -1, a frame byte 0).
// Only an index the field can hold is offered (a byte-sized parameter takes registers 0..255).
void index_choices(const ModelRow &row, const ThreediSchemaRecord &record,
		ThreediSchemaReference ref, const FieldSchema &field, std::vector<FieldChoice> &out) {
	const auto offer = [&](int64_t value, std::string name, std::string label) {
		if (field.ranged && (double(value) < field.min || double(value) > field.max)) return;
		out.push_back({std::move(name), value, std::move(label)});
	};
	switch (ref) {
	case ThreediSchemaReference::Register:
		for (size_t i = 0; i < row.registers.size(); ++i) offer(int64_t(i), std::to_string(i), row.registers[i].name);
		break;
	case ThreediSchemaReference::Part: {
		if (record.shape == ThreediSchemaShape::PartAnimation) offer(255, "255", "None");
		if (record.shape == ThreediSchemaShape::UserPoint) offer(-1, "-1", "None");
		const size_t parts = row.base && row.base->lod_count ? row.base->lods[0].render_object_count : 0;
		for (size_t i = 0; i < parts; ++i) offer(int64_t(i), std::to_string(i), "Part " + std::to_string(i));
		break;
	}
	case ThreediSchemaReference::Frame:
		// A row above zero as a signed byte (threedi_panm_frame_row).
		offer(0, "0", "None");
		for (size_t i = 1; i < row.frames.size() && i < 128; ++i)
			offer(int64_t(i), std::to_string(i), "MTRX row " + std::to_string(i));
		break;
	default: return;
	}
}

const Document::CollectionSpec spec(ModelKind kind, const char *label, const char *name_field, bool fixed,
                                    size_t max = 0) {
	Document::CollectionSpec s;
	s.kind = node_kind(kind);
	s.label = label;
	s.name_field = name_field;
	s.fixed = fixed;
	s.max = max;
	return s;
}

} // namespace

ModelRow::ModelRow() {
	kind = kModel;
	collections.resize(6);
}

std::shared_ptr<Node> ModelRow::clone() const { return std::make_shared<ModelRow>(*this); }

size_t ModelRow::footprint() const {
	size_t bytes = sizeof(ModelRow) + collections_footprint() + footprint_of(lods) +
	               footprint_of(materials) + footprint_of(lights) + footprint_of(user_points) +
	               footprint_of(registers) + footprint_of(frames);
	for (const ModelLod &lod : lods) bytes += footprint_of(lod.panm) + footprint_of(lod.panm_ids);
	for (const ModelMaterial &material : materials) bytes += footprint_of(material.texture_ids);
	return bytes;
}

size_t CollisionRow::footprint() const {
	return sizeof(CollisionRow) + collections_footprint() + footprint_of(sections) +
	       footprint_of(volumes) + footprint_of(faces) + footprint_of(occlusion);
}

void ModelRow::for_each_identity(const std::function<void(NodeId &)> &fn) {
	for (auto &collection : collections)
		for (NodeId &id : collection) fn(id);
	for (ModelLod &lod : lods)
		for (NodeId &id : lod.panm_ids) fn(id);
	for (ModelMaterial &material : materials)
		for (NodeId &id : material.texture_ids) fn(id);
	// The identities may be new (a load's, a duplicate's): their places made again.
	model_document_detail::index_places(*this);
}

CollisionRow::CollisionRow() {
	kind = kCollision;
	collections.resize(4);
}

void CollisionRow::for_each_identity(const std::function<void(NodeId &)> &fn) {
	for (auto &collection : collections)
		for (NodeId &id : collection) fn(id);
	model_document_detail::index_places(*this);
}

bool is_model_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::Model;
}

const std::vector<RecordKindRow> &ModelDocument::kinds() const {
	static const std::vector<RecordKindRow> table = [] {
		std::vector<RecordKindRow> out;
		for (const KindRow &row : kKinds) {
			const NodeKind kind = node_kind(row.kind);
			out.push_back({kind, row.token, row.label, "", kind == kModel || kind == kCollision});
		}
		return out;
	}();
	return table;
}

std::vector<Document::Collection> ModelDocument::collections(const Node &node, const NodeAddress &owner) const {
	if (node.kind == kModel) {
		const ModelRow &row = static_cast<const ModelRow &>(node);
		if (owner.child == 0)
			return {{spec(ModelKind::Lod, "LODs", "", true), row.collections[0]},
			        {spec(ModelKind::Material, "Materials", "shader", false), row.collections[1]},
			        {spec(ModelKind::Light, "Lights", "", false), row.collections[2]},
			        {spec(ModelKind::UserPoint, "User points", "name", false), row.collections[3]},
			        {spec(ModelKind::Register, "CTRL registers", "name", false), row.collections[4]},
			        {spec(ModelKind::Frame, "Rotation frames", "", false), row.collections[5]}};
		Place p;
		if (!place_of(row, owner.child, p)) return {};
		if (p.collection == 0)
			return {{spec(ModelKind::PartAnimation, "Part animations", "", false), row.lods[p.index].panm_ids}};
		if (p.collection == 1)
			return {{spec(ModelKind::Texture, "Textures", "name", false, 24), row.materials[p.index].texture_ids}};
		return {};
	}
	if (node.kind != kCollision || owner.child != 0) return {};
	const CollisionRow &row = static_cast<const CollisionRow &>(node);
	return {{spec(ModelKind::Section, "Sections", "", true), row.collections[0]},
	        {spec(ModelKind::Volume, "Volumes", "", true), row.collections[1]},
	        {spec(ModelKind::Face, "Bullet faces", "", true), row.collections[2]},
	        {spec(ModelKind::Occlusion, "Occlusion records", "", true), row.collections[3]}};
}

const std::vector<FieldSchema> &ModelDocument::fields(NodeKind kind) const {
	static const std::vector<std::vector<FieldSchema>> tables = [] {
		std::vector<std::vector<FieldSchema>> out;
		for (const KindRow &row : kKinds) {
			out.emplace_back();
			if (row.kind == ModelKind::Collision) continue; // the collision row holds records, no fields
			for (const ThreediSchemaField &f : threedi_schema_fields(row.shape)) {
				out.back().push_back(field_of(f));
				if (row.kind == ModelKind::Material && out.back().back().id == "shader")
					out.back().back().choices = shader_choices();
				// A user point's name is what an item's particle slot looks it up by.
				if (row.kind == ModelKind::UserPoint && out.back().back().id == "name")
					out.back().back().defines = ReferenceKind::UserPoint;
			}
		}
		return out;
	}();
	static const std::vector<FieldSchema> none;
	for (size_t i = 0; i < std::size(kKinds); ++i)
		if (node_kind(kKinds[i].kind) == kind) return tables[i];
	return none;
}

void ModelDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	const Node *node = row(address.row);
	if (!node) return;
	const ThreediSchemaRecord record = record_of(const_cast<Node &>(*node), address);
	if (!record) return;
	const std::string &id = use.schema->id;
	const ThreediSchemaField *schema = threedi_schema_field(record.shape, id);
	use.applies = schema && schema->unverified        ? Applicability::Unverified
	              : threedi_schema_reads(record, id) ? Applicability::Reads
	                                                 : Applicability::Ignored;
	const ThreediSchemaReference ref = threedi_schema_reference(record, id);
	use.reference =
			ref == ThreediSchemaReference::Texture ? ReferenceKind::Texture : ReferenceKind::None;
	// An index names what the model row holds (record_choices): its registers and frames are
	// its records, LOD 0's parts are not.
	if (node->kind == kModel) {
		const ThreediSchemaReference named = index_reference(record, id, ref);
		if (named != ThreediSchemaReference::None) {
			use.own_choices = true;
			if (named != ThreediSchemaReference::Part) use.record_owner = {node->id, kModel, 0};
		}
	}
	// A texture row's name loads the file its type's loader picks (reference_file_candidates).
	if (use.reference == ReferenceKind::Texture && record.shape == ThreediSchemaShape::Texture)
		use.loader_arg = static_cast<const ThreediMaterialTexture *>(record.data)->type;
	// A user point is looked up on the model an item names by its file (the item's graphic).
	if (use.defines == ReferenceKind::UserPoint)
		use.scope = strutil::to_upper(basename_of(path()));
}

bool ModelDocument::record_choices(const NodeAddress &address, const FieldUse &use,
		std::vector<FieldChoice> &out) const {
	const Node *node = row(address.row);
	if (!node || node->kind != kModel) return false;
	const ThreediSchemaRecord record = record_of(const_cast<Node &>(*node), address);
	if (!record) return false;
	const std::string &id = use.schema->id;
	const ThreediSchemaReference named =
			index_reference(record, id, threedi_schema_reference(record, id));
	if (named == ThreediSchemaReference::None) return false;
	index_choices(static_cast<const ModelRow &>(*node), record, named, *use.schema, out);
	return true;
}

void ModelDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	// An item's particle slot finds a user point among the model's first 16 without case: a
	// later one no lookup reaches [orig: ItemDef_GetBoneMaskByName @ 0x49ea40, the scan end @
	// 0x49ea73].
	Placement at;
	if (address.kind == node_kind(ModelKind::UserPoint) && placement(address, at) &&
	    at.index >= size_t(THREEDI_USER_POINT_SCAN_LIMIT)) {
		facts.inert = true;
		facts.inert_reason = "an item's lookup scans the model's first " +
				std::to_string(THREEDI_USER_POINT_SCAN_LIMIT) + " user points only";
	}
}

bool ModelDocument::read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const {
	const ThreediSchemaRecord record = record_of(const_cast<Node &>(row), address);
	return record && threedi_schema_get(record, field, out);
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
	const ModelRow *row = model_row();
	const CollisionRow *collision = collision_row();
	if (!row || !row->base) return;
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
		error = make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This file is not a model.", path());
		return false;
	}
	const assets::Model base = assets::parse_model(bytes.data(), bytes.size());
	if (!base) {
		error = make_diagnostic(DiagnosticSeverity::Error, "document.parse", "The model could not be read.", path());
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
		lod.panm_ids.resize(lod.panm.size());
		row->lods.push_back(std::move(lod));
	}
	for (uint32_t m = 0; m < base->material_count; ++m) {
		ModelMaterial material;
		material.material = base->materials[m];
		material.source = static_cast<int>(m);
		material.texture_ids.resize(std::min<uint32_t>(material.material.texture_count, 24));
		row->materials.push_back(std::move(material));
	}
	row->lights.assign(base->lights, base->lights + base->light_count);
	row->user_points.assign(base->user_points, base->user_points + base->user_point_count);
	row->registers.assign(base->ctrl.registers, base->ctrl.registers + base->ctrl.count);
	row->frames.assign(base->mtrx.matrices, base->mtrx.matrices + base->mtrx.count);
	row->collections[0].resize(row->lods.size());
	row->collections[1].resize(row->materials.size());
	row->collections[2].resize(row->lights.size());
	row->collections[3].resize(row->user_points.size());
	row->collections[4].resize(row->registers.size());
	row->collections[5].resize(row->frames.size());
	rows.push_back(row);

	auto collision = std::make_shared<CollisionRow>();
	if (base->collision) {
		const ThreediCollisionModel &c = *base->collision;
		collision->sections.assign(c.objects, c.objects + c.object_count);
		collision->volumes.assign(c.volumes, c.volumes + c.volume_count);
		collision->faces.assign(c.faces, c.faces + c.face_count);
	}
	collision->occlusion.assign(base->occlusion_objects, base->occlusion_objects + base->occlusion_object_count);
	collision->collections[0].resize(collision->sections.size());
	collision->collections[1].resize(collision->volumes.size());
	collision->collections[2].resize(collision->faces.size());
	collision->collections[3].resize(collision->occlusion.size());
	rows.push_back(collision);
	return true;
}

std::shared_ptr<Node> ModelDocument::make_node(NodeKind, NodeId,
                                               const std::vector<std::shared_ptr<const Node>> &,
                                               std::string &error) {
	error = "A model keeps its model and collision rows; add records inside them.";
	return nullptr;
}

bool ModelDocument::set_field(Node &node, const NodeAddress &address, const std::string &field, const Value &value,
                              std::string &error) {
	const ThreediSchemaRecord record = record_of(node, address);
	if (!record) {
		error = "The record no longer exists.";
		return false;
	}
	if (record.shape != ThreediSchemaShape::Material || field != "shader")
		return threedi_schema_set(record, field, value, error);

	// A shader decides the draw pass and the vertex layout (geometry), and the words
	// threedi_build_material_surface derives.
	ThreediMaterial &material = *static_cast<ThreediMaterial *>(record.data);
	const std::string *tag = std::get_if<std::string>(&value);
	if (tag == nullptr) {
		error = "A shader is a tag.";
		return false;
	}
	const ModelRow &row = static_cast<const ModelRow &>(node);
	Place p;
	const bool drawn = place_of(row, address.child, p) && model_document_detail::material_is_drawn(row, row.materials[p.index].source);
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
	if (!threedi_schema_set(record, field, value, error)) return false;
	threedi_build_material_surface(material, (flags & renderer::MATERIAL_FLAG_GLASS) != 0,
	                               (flags & renderer::MATERIAL_FLAG_EMISSIVE) != 0);
	return true;
}

bool ModelDocument::accept_step(const EditStep &step, const StagedRows &,
                                std::string &error) const {
	for (const RowSwap &swap : step.swaps) {
		if (swap.before && swap.after) continue;
		error = "A model keeps its model and collision rows.";
		return false;
	}
	return true;
}

} // namespace opennova::editor
