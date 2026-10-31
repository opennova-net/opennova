#include "player/player_viewmodel_def.h"

#include "object/weapon_def.h"
#include "util/record_bind.h"

using namespace godot;

Ref<PlayerViewmodelDef> PlayerViewmodelDef::from_weapon_def(const Ref<WeaponDef> &p_def) {
	if (p_def.is_null()) {
		return Ref<PlayerViewmodelDef>();
	}
	const opennova::renderer::FpViewmodelDef def = opennova::renderer::fp_viewmodel_def(p_def->value());
	Ref<PlayerViewmodelDef> out;
	out.instantiate();
	out->weapon_name_ = String(def.weapon_name.c_str());
	out->gfx1_ = String(def.gfx1.c_str());
	out->gfx3_ = String(def.gfx3.c_str());
	out->animadm_ = String(def.animadm.c_str());
	out->pos_units_ = Vector3(def.pos_units[0], def.pos_units[1], def.pos_units[2]);
	out->rot_bias_deg_ = Vector3(def.rot_bias_deg[0], def.rot_bias_deg[1], def.rot_bias_deg[2]);
	out->tpos_units_ = Vector3(def.tpos_units[0], def.tpos_units[1], def.tpos_units[2]);
	out->renderfov_h_deg_ = def.renderfov_h_deg;
	out->flags_ = def.flags;
	out->scope_max_mag_ = def.scope_max_mag;
	out->clipsize_ = def.clipsize;
	return out;
}

void PlayerViewmodelDef::_bind_methods() {
	ClassDB::bind_static_method("PlayerViewmodelDef", D_METHOD("from_weapon_def", "def"),
			&PlayerViewmodelDef::from_weapon_def);
	PLAYER_VIEWMODEL_DEF_FIELDS(OPENNOVA_RECORD_FIELD)
}
