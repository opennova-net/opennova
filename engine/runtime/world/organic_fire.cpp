#include <runtime/world/organic_fire.h>

#include <base/io/strutil.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/ammo_table.h>

namespace opennova::world {

void resolve_organic_weapons(const char *const ammo_names[kOrganicAmmoSlots],
                             const char *const launch_names[kOrganicLaunchSlots], const AmmoTable &ammo,
                             const threedi::Threedi3di3 *model, OrganicWeapons &out) {
	for (int slot = 0; slot < kOrganicAmmoSlots; ++slot) {
		const char *name = ammo_names[slot];
		if (name == nullptr || name[0] == '\0') continue;
		const int id = ammo.index_of(name);
		out.ammo[size_t(slot)] = static_cast<uint8_t>(id >= 0 ? id : 0);
	}
	// ModelGPM_FindUserpointByName returns the FIRST case-insensitive match. The index-plus-one stores wrap to a
	// byte, as in retail. [orig: Entity_InitOrganicAI @0x4BFE8F..0x4BFF82]
	for (int slot = 0; slot < kOrganicLaunchSlots; ++slot) {
		out.launch[size_t(slot)] = 0;
		const char *name = launch_names[slot];
		if (model == nullptr || model->user_points == nullptr || name == nullptr || name[0] == '\0') continue;
		for (size_t point = 0; point < model->user_point_count; ++point) {
			if (strutil::iequals(model->user_points[point].name, name)) {
				out.launch[size_t(slot)] = static_cast<uint8_t>(point + 1);
				break;
			}
		}
	}
}

} // namespace opennova::world
