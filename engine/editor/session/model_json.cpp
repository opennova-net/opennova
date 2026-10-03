#include <editor/session/model_json.h>

#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>

namespace opennova::editor {

using io::json_number;
using io::json_string;
using io::JsonValue;

namespace {

constexpr NodeKind kMaterial = node_kind(ModelKind::Material);
constexpr size_t kCollisionFaces = 2;

JsonValue number(double value) { return json_number(value); }

} // namespace

JsonValue model_surfaces_to_json(const ModelDocument &model, const JsonPage &page) {
	JsonValue out = JsonValue::make_object();
	JsonValue surfaces = JsonValue::make_array();
	for (const FieldChoice &choice : model_surface_choices()) {
		const ModelSurfaceWords words = model_surface_words(choice.value);
		JsonValue entry = JsonValue::make_object();
		entry.set("surface", number(double(choice.value)));
		entry.set("name", json_string(words.name));
		entry.set("tag", json_string(words.tag));
		if (!words.note.empty()) entry.set("note", json_string(words.note));
		surfaces.push(std::move(entry));
	}
	out.set("surfaces", std::move(surfaces));
	JsonValue flags = JsonValue::make_array();
	for (const ModelFaceFlag &flag : model_face_flags()) {
		JsonValue entry = JsonValue::make_object();
		entry.set("flag", json_string(flag.token));
		entry.set("bit", number(double(flag.bit)));
		entry.set("label", json_string(flag.label));
		entry.set("tip", json_string(flag.tip));
		flags.push(std::move(entry));
	}
	out.set("flags", std::move(flags));
	const ModelRow *row = model.model_row();
	const CollisionRow *collision = model.collision_row();
	out.set("faces", number(collision ? double(collision->faces.size()) : 0.0));
	out.set("without_material", number(double(model_faces_without_material(model))));
	out.set("collision_lod", number(row ? double(model_face_materials(row->base)->lod) : -1.0));
	const size_t count = row ? row->materials.size() : 0;
	set_page(out, page, count);
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(count); i < page.last(count); ++i) {
		const NodeId id = row->ids.lists[kModelMaterials][i].id;
		const NodeAddress address{row->id, kMaterial, id};
		ModelMaterialSurface surface;
		model_material_surface(model, id, surface);
		JsonValue entry = JsonValue::make_object();
		entry.set("id", number(double(id)));
		entry.set("index", number(double(i)));
		entry.set("title", json_string(model_record_label(model, address, nullptr)));
		entry.set("faces", number(double(surface.faces)));
		entry.set("surface", surface.faces && !surface.mixed() ? number(double(surface.common())) : JsonValue::make_null());
		entry.set("words", json_string(model_material_surface_words(surface)));
		entry.set("mixed", JsonValue::make_bool(surface.mixed()));
		JsonValue counts = JsonValue::make_array();
		for (const ModelSurfaceCount &c : surface.surfaces) {
			JsonValue one = JsonValue::make_object();
			one.set("surface", number(double(c.surface)));
			one.set("name", json_string(model_surface_words(c.surface).name));
			one.set("faces", number(double(c.faces)));
			counts.push(std::move(one));
		}
		entry.set("counts", std::move(counts));
		JsonValue on = JsonValue::make_array();
		for (size_t f = 0; f < surface.flags.size() && f < model_face_flags().size(); ++f) {
			JsonValue one = JsonValue::make_object();
			one.set("flag", json_string(model_face_flags()[f].token));
			one.set("on", number(double(surface.flags[f].on)));
			on.push(std::move(one));
		}
		entry.set("flags", std::move(on));
		entry.set("differing", number(double(surface.differing.size())));
		list.push(std::move(entry));
	}
	out.set("materials", std::move(list));
	return out;
}

JsonValue model_differing_faces_to_json(const ModelDocument &model, NodeId material, const JsonPage &page) {
	JsonValue out = JsonValue::make_object();
	out.set("material", number(double(material)));
	ModelMaterialSurface surface;
	model_material_surface(model, material, surface);
	const CollisionRow *collision = model.collision_row();
	const size_t count = collision ? surface.differing.size() : 0;
	set_page(out, page, count);
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(count); i < page.last(count); ++i) {
		const size_t face = surface.differing[i];
		JsonValue entry = JsonValue::make_object();
		entry.set("id", number(double(collision->ids.lists[kCollisionFaces][face].id)));
		entry.set("index", number(double(face)));
		entry.set("surface", number(double(collision->faces[face].poly_type)));
		entry.set("name", json_string(model_surface_words(collision->faces[face].poly_type).name));
		list.push(std::move(entry));
	}
	out.set("faces", std::move(list));
	return out;
}

} // namespace opennova::editor
