#pragma once

// The wire form of a model's surfaces (ADR 0046 S17, Models): what the `model_surfaces` query answers
// (every material's bullet-face surface and flags in words, the faces of a material that differ) beside
// the edits it plans, which the query's `planned` form gives back in the batch form an edit_record takes.

#include <base/io/json.h>
#include <editor/documents/model_surfaces.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

class ModelDocument;

// {surfaces: [{surface, name, tag, note}], flags: [{flag, bit, label, tip}], faces, without_material,
// collision_lod, count, materials: [{id, index, title, faces, surface, words, mixed, counts: [{surface,
// name, faces}], flags: [{flag, on}], differing}]}: the surfaces and flags a material takes by name, and a
// page of the model's materials, each its bullet faces' surface in words.
io::JsonValue model_surfaces_to_json(const ModelDocument &model, const JsonPage &page);
// {material, count, faces: [{id, index, surface, name}]}: a page of the faces of a material whose surface
// is not its common one.
io::JsonValue model_differing_faces_to_json(const ModelDocument &model, NodeId material, const JsonPage &page);

} // namespace opennova::editor
