// What item a model is drawn as, read from the model alone (model_item.h).

#include <editor/documents/model_item.h>

#include <set>
#include <string_view>

#include <base/io/strutil.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>
#include <runtime/world/model_geometry.h>

namespace opennova::editor {

namespace {

// A blink box's collidable type (BB) [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150, the type-8 arm
// @ 0x4aea68]; editor/documents/model_collision_words.cpp names every type.
constexpr int32_t kBlinkBox = 8;

std::string counted(size_t count, const char *one, const char *many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

// The doors the model's parts turn: the distinct DOOR_nn registers its first LOD's part tracks read (a
// register-generator style names a register by the model's own CTRL table [orig: ThreediGp_LoadFromFile
// @ 0x5B5E0B..0x5B5EF6, the PANM fixups]).
int door_count(const threedi::Threedi3di3 &model) {
	if (model.lod_count == 0 || !model.lods) return 0;
	std::set<int> doors;
	const threedi::ThreediLod &lod = model.lods[0];
	for (size_t i = 0; i < lod.part_animation_count; ++i)
		for (const threedi::ThreediTransform *track : threedi::threedi_panm_tracks(lod.part_animations[i])) {
			if (!threedi::threedi_generator_names_register(track->control) || track->control_param >= model.ctrl.count)
				continue;
			const int ordinal = threedi::threedi_ctrl_register_ordinal(model.ctrl.registers[track->control_param].name);
			if (ordinal >= threedi::THREEDI_CTRL_DOOR_00 && ordinal <= threedi::THREEDI_CTRL_DOOR_15) doors.insert(ordinal);
		}
	return int(doors.size());
}

} // namespace

const char *model_item_kind_token(ModelItemKind kind) {
	switch (kind) {
	case ModelItemKind::Decoration: return "decoration";
	case ModelItemKind::Building: return "building";
	case ModelItemKind::Person: return "person";
	case ModelItemKind::Vehicle: return "vehicle";
	case ModelItemKind::MountedWeapon: return "mounted_weapon";
	}
	return "decoration";
}

ModelItemFacts model_item_facts(const threedi::Threedi3di3 &model) {
	ModelItemFacts out;
	out.doors = door_count(model);
	// A vehicle's seats and its driver's place [orig: EntityDef_LoadModelsAndCallbacks @ 0x439F50: `sitex`
	// @ 0x43A4BC, `ctrlx` @ 0x43A50B, `drvrx` @ 0x43A549, each a five-character strnicmp]; a mounted gun's
	// `UseGun`, a whole-name stricmp [orig: @ 0x43A582].
	for (size_t i = 0; i < model.user_point_count; ++i) {
		const std::string_view name = model.user_points[i].name;
		if (threedi::threedi_user_point_is_sitex(name) || strutil::starts_with_icase(name, "ctrlx") ||
		    strutil::starts_with_icase(name, "drvrx")) {
			out.kind = ModelItemKind::Vehicle;
			out.type = def::DEF_ITEM_TYPE_VEHICLE;
			out.makes = false;
			out.because = "its seat or driver's place, user point " + std::string(name);
			return out;
		}
	}
	for (size_t i = 0; i < model.user_point_count; ++i)
		if (strutil::iequals(model.user_points[i].name, "UseGun")) {
			out.kind = ModelItemKind::MountedWeapon;
			out.type = def::DEF_ITEM_TYPE_OBJECT;
			out.makes = false;
			out.because = "its gun mount, user point " + std::string(model.user_points[i].name);
			return out;
		}
	if (world::model_is_skinned(model, 0)) {
		out.kind = ModelItemKind::Person;
		out.type = def::DEF_ITEM_TYPE_PERSON;
		out.makes = false;
		out.because = "its skinned parts";
		return out;
	}
	size_t blink = 0;
	if (model.collision)
		for (size_t i = 0; i < model.collision->volume_count; ++i)
			if (model.collision->volumes[i].collidable_type == kBlinkBox) ++blink;
	if (model.occlusion_object_count > 0 || blink > 0) {
		out.kind = ModelItemKind::Building;
		out.type = def::DEF_ITEM_TYPE_BUILDING;
		std::string parts;
		if (model.occlusion_object_count > 0)
			parts = "its occlusion, " + counted(model.occlusion_object_count, "object", "objects");
		if (blink > 0) parts += (parts.empty() ? "its " : ", and ") + counted(blink, "blink box", "blink boxes");
		out.because = parts;
		return out;
	}
	out.kind = ModelItemKind::Decoration;
	out.type = def::DEF_ITEM_TYPE_DECORATION;
	out.because = "no occlusion, blink box, seat, gun mount or skin";
	return out;
}

std::string model_item_words(const ModelItemFacts &facts) {
	switch (facts.kind) {
	case ModelItemKind::Decoration: return "a decoration (" + facts.because + ")";
	case ModelItemKind::Building: return "a building (" + facts.because + ")";
	case ModelItemKind::Person: return "a person (" + facts.because + ")";
	case ModelItemKind::Vehicle: return "a vehicle (" + facts.because + ")";
	case ModelItemKind::MountedWeapon: return "a mounted weapon (" + facts.because + ")";
	}
	return std::string();
}

} // namespace opennova::editor
