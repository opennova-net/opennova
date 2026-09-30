#pragma once

// What model_document.cpp and model_document_edits.cpp share: the record kinds, where
// an identity sits, and the record a field is read and written on.

#include <cstdint>

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

// The model row's collection slots past its own six: a LOD's part animations and a
// material's texture rows (ModelPlace::owner names the LOD or the material).
constexpr uint8_t kPanmSlot = 6;
constexpr uint8_t kTextureSlot = 7;

const ModelPlaces &places_of(const Node &row);
bool place_of(const Node &row, NodeId id, ModelPlace &out);
// Every identity's place in a row, made again: when the row is given its identities and after a
// structural edit of a clone, before it commits.
void index_places(Node &row);
// The native record an address names, as the property table's record (a model row's own
// address: its header); empty when the row does not hold it.
threedi::ThreediSchemaRecord record_of(Node &row, const NodeAddress &address);

// The engine's shader table: whether it knows a tag (and its capability flags), and
// whether the tag draws in a blended pass.
bool shader_flags(const char *tag, uint32_t &flags);
bool shader_blends(const char *tag);
// Whether a strip of the base draws with the base's material `source`.
bool material_is_drawn(const ModelRow &row, int source);

} // namespace opennova::editor::model_document_detail
