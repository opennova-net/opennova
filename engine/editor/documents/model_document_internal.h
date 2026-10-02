#pragma once

// What model_document.cpp, model_document_edits.cpp and model_table.cpp share: the record kinds' lists,
// what a field of the model's table is to the model's own rules, and what an index field names.

#include <cstdint>
#include <functional>
#include <string>

#include <editor/documents/model_document.h>

namespace opennova::editor::model_document_detail {

// What a field of the table names by an index, as the format's table declares it: a texture file, a
// CTRL register (by its index in the model's table), a part (by its index in LOD 0), an MTRX row.
enum class IndexReference { None, Texture, Register, Part, Frame };

// A field of the model's table as the model's own rules read it, beside its labelled field (by its
// place in its kind): what it declares it names, whether what the game does with it is witnessed, and
// on a record whether the game reads it there (a generator's register only above style 0x70 and its
// phase only at or below [orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640], a PANM track only when its
// flags make it present [orig: PANM_SampleTrack @ 0x5B2270], a spot light's axis) and whether its
// value names what it declares there (a register byte only above style 0x70, a frame byte only on a
// row that turns through one).
struct ModelField {
	IndexReference reference = IndexReference::None;
	bool unverified = false;
	std::function<bool(const RecordHandle &)> reads;
	std::function<bool(const RecordHandle &)> names;
};
const ModelField &model_field(NodeKind kind, size_t place);

// What an index field names on a record, given its place in its kind's table: a CTRL register, a part
// of LOD 0 or an MTRX row; a frame byte names the MTRX rows wherever its row turns through one (a
// spinner or Euler row: the field's reads), whatever it names now, so "none" is no dead end. None for
// any other field, without a lookup.
IndexReference index_reference(const RecordHandle &record, size_t place);
// The Record reference an index makes (index_reference's): a CTRL register (ModelRegister) or an MTRX
// row (ModelFrame); None for a part (no record: LOD 0's geometry is the base's) and for any other field.
ReferenceKind record_reference(IndexReference index);
// What a field's value names by index on this record whether the game reads it there or not, what the
// renumbering keeps naming its record: a CTRL register where its style makes the byte one (a
// generator's, a track's or a light's parameter above style 0x70, a flipbook's time on the register
// clock), an MTRX row on any part animation (a frame byte is never anything else). None for any other
// field.
ReferenceKind named_by_index(const RecordHandle &record, size_t place);

// The engine's shader table: whether it knows a tag (and its capability flags), and
// whether the tag draws in a blended pass.
bool shader_flags(const char *tag, uint32_t &flags);
bool shader_blends(const char *tag);
// Whether a strip of the base draws with the base's material `source`.
bool material_is_drawn(const ModelRow &row, int source);

} // namespace opennova::editor::model_document_detail
