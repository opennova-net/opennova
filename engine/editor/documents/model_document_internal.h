#pragma once

// What model_document.cpp and model_document_edits.cpp share: the record kinds, the record a
// field is read and written on, and what an index field names.

#include <cstdint>
#include <string>

#include <editor/documents/model_document.h>
#include <formats/threedi/threedi_schema.h>

namespace opennova::editor::model_document_detail {

struct KindRow {
	ModelKind kind;
	const char *token;
	const char *label;
	threedi::ThreediSchemaShape shape;
};

inline const KindRow kKinds[] = {
	{ModelKind::Model, "model", "Model", threedi::ThreediSchemaShape::Model},
	{ModelKind::Collision, "collision", "Collision", threedi::ThreediSchemaShape::Model},
	{ModelKind::Lod, "lod", "LOD", threedi::ThreediSchemaShape::Lod},
	{ModelKind::PartAnimation, "part_animation", "Part animation", threedi::ThreediSchemaShape::PartAnimation},
	{ModelKind::Material, "material", "Material", threedi::ThreediSchemaShape::Material},
	{ModelKind::Texture, "texture", "Texture", threedi::ThreediSchemaShape::Texture},
	{ModelKind::Light, "light", "Light", threedi::ThreediSchemaShape::Light},
	{ModelKind::UserPoint, "user_point", "User point", threedi::ThreediSchemaShape::UserPoint},
	{ModelKind::Register, "register", "Register", threedi::ThreediSchemaShape::Register},
	{ModelKind::Frame, "frame", "Rotation frame", threedi::ThreediSchemaShape::Frame},
	{ModelKind::Section, "section", "Section", threedi::ThreediSchemaShape::Section},
	{ModelKind::Volume, "volume", "Volume", threedi::ThreediSchemaShape::Volume},
	{ModelKind::Face, "face", "Bullet face", threedi::ThreediSchemaShape::Face},
	{ModelKind::Occlusion, "occlusion", "Occlusion record", threedi::ThreediSchemaShape::Occlusion},
};

// The model row's collections, in the order collections() gives them (a record path's first
// step): its LODs, materials, lights, user points, CTRL registers and MTRX rows. A LOD's part
// animations and a material's texture rows are each the one collection of their owner.
constexpr uint32_t kLods = 0, kMaterials = 1, kLights = 2, kUserPoints = 3, kRegisters = 4, kFrames = 5;

// The native record an address names in `row` (a model row's own address: its header), found
// by its path in the row (Document::path_in: the row as `document` holds it, or as a batch has
// left it); empty when the row does not hold it.
threedi::ThreediSchemaRecord record_of(const ModelDocument &document, Node &row, const NodeAddress &address);

// What an index field names on a record, given its table entry (`field`, threedi_schema_field's)
// and the reference the record makes of it (`ref`, threedi_schema_reference's): a CTRL register, a
// part of LOD 0 or an MTRX row; a frame byte names the MTRX rows wherever its row turns through
// one (a spinner or Euler row: threedi_schema_reads), whatever it names now, so "none" is no dead
// end. None for any other field, without a lookup.
threedi::ThreediSchemaReference index_reference(const threedi::ThreediSchemaRecord &record,
                                                const threedi::ThreediSchemaField *field,
                                                const std::string &path,
                                                threedi::ThreediSchemaReference ref);
// The Record reference an index makes (index_reference's): a CTRL register (ModelRegister) or an
// MTRX row (ModelFrame); None for a part (no record: LOD 0's geometry is the base's) and for any
// other field.
ReferenceKind record_reference(threedi::ThreediSchemaReference index);
// What a field's value names by index on this record whether the game reads it there or not, what
// the renumbering keeps naming its record: a CTRL register where its style makes the byte one (a
// generator's, a track's or a light's parameter above style 0x70, a flipbook's time on the register
// clock: threedi_schema_reference), an MTRX row on any part animation (a frame byte is never
// anything else). None for any other field.
ReferenceKind named_by_index(const threedi::ThreediSchemaRecord &record, const std::string &path);

// The engine's shader table: whether it knows a tag (and its capability flags), and
// whether the tag draws in a blended pass.
bool shader_flags(const char *tag, uint32_t &flags);
bool shader_blends(const char *tag);
// Whether a strip of the base draws with the base's material `source`.
bool material_is_drawn(const ModelRow &row, int source);

} // namespace opennova::editor::model_document_detail
