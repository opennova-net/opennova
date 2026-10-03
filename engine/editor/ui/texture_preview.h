#pragma once

#include <memory>
#include <string>

#include <editor/documents/texture_load_rules.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document.h>
#include <editor/model/field_use.h>
#include <editor/model/value.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

struct TextureThumbnail;

// A texture's preview wherever the windows name one (ADR 0046 S18): its thumbnail (the session's cache,
// preview/texture_thumbnails: the file a reference's loader opens, what that loader makes of its
// texels) drawn through the Shell's thumbnail device, and a larger picture with what the file is when
// it is hovered. Built only with OPENNOVA_IMGUI.
namespace texture_preview {

// The thumbnail of `file` as `transform` makes it (the session's cache asked: a picture not made yet is
// made by a later poll), in a box `side` pixels a side at the cursor, as one item: the picture with its
// proportions kept over a checkerboard where it has alpha; while it is being made a framed box; the
// game unable to load it, a red cross. The hovered item's tooltip is texture_preview::tooltip's.
void thumbnail(Workspace &workspace, const std::string &file, TextureLoadTransform transform, float side);
// The same box drawn at the cursor as a dummy item (no tooltip of its own: a list's row has its own).
void picture(Workspace &workspace, const std::string &file, TextureLoadTransform transform, float side);
// The facts of `file` under its picture, wrapped to `width`: what a picker's preview panel shows.
void facts_block(Workspace &workspace, const std::string &file, TextureLoadTransform transform, float width);
// While the item just drawn is hovered: a tooltip of the texture `file` (its picture larger, its size,
// format, texels, alpha, what the loader makes of it, why the game cannot load it), after `lead`.
void tooltip(Workspace &workspace, const std::string &file, TextureLoadTransform transform, const std::string &lead);
// What the windows' tooltip of a texture file says, its picture apart: its size, format and texels.
std::string facts(const TextureThumbnail &thumbnail);
// A texture reference's preview under its field (the Inspector's): its thumbnail and, beside it, what
// the loader opens in words, else "Not found" (the project has no file the game loads for it) or the
// game's refusal; compact (a table's cell), a thumbnail a line high alone. Nothing for a field that
// names no texture.
void reference_field(Workspace &workspace, const FieldUse &field, const Value &value, bool compact);
// The tooltip of the item just drawn, while hovered, for a project file: a texture's preview after
// `lead`; true when `file` is a texture (the caller draws its own tooltip otherwise).
bool file_tooltip(Workspace &workspace, const std::string &file, const std::string &lead);
// The same for a record (an outline's row) whose fields name a texture (a model's texture row): the
// first such field's texture, as its loader loads it; false when the record names none (or the item is
// not hovered), the caller drawing its own tooltip then.
bool record_tooltip(Workspace &workspace, const Document &document, const NodeAddress &address, const std::string &lead);

} // namespace texture_preview

} // namespace opennova::editor
