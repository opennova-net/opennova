#include <editor/documents/model_ctrl_words.h>

#include <array>

#include <formats/threedi/threedi_ctrl_catalog.h>

namespace opennova::editor {

namespace {

using namespace opennova::threedi;

// The census's two words for a register no ported writer names.
constexpr const char *kOpen =
		"The game has a dedicated writer for it, not ported yet: the preview holds it at the value set.";
constexpr const char *kNone =
		"Nothing in the game writes it: it reads 0 (a weapon action's ctrlreg could animate it; no shipped "
		"weapon.def does).";
constexpr const char *kCensus = "[orig: global control-register descriptor table @ 0x83DCE8]";
constexpr const char *kGeneric = "[orig: ActionDef_ParseScriptLine @ 0x4027FA; CtrlRegAnimSlot_UpdateAll @ 0x401BF0]";

std::array<CtrlRegisterWords, THREEDI_CTRL_REGISTER_COUNT> make_table() {
	std::array<CtrlRegisterWords, THREEDI_CTRL_REGISTER_COUNT> t{};
	const auto row = [&](int ordinal, const char *group, const char *label, CtrlWriter writer, const char *driven,
	                     const char *cite, int64_t min = -65536, int64_t max = 65536, bool share = false) {
		CtrlRegisterWords &w = t[size_t(ordinal)];
		w.group = group;
		w.label = label;
		w.writer = writer;
		w.driven = driven;
		w.cite = cite;
		w.min = min;
		w.max = max;
		w.share = share;
	};
	const auto open = [&](int ordinal, const char *group, const char *label) {
		row(ordinal, group, label, CtrlWriter::Open, kOpen, kCensus);
	};
	const auto none = [&](int ordinal, const char *group, const char *label) {
		row(ordinal, group, label, CtrlWriter::None, kNone, kGeneric);
	};

	// The level of detail.
	row(THREEDI_CTRL_LOD_FRAC, "lod", "LOD fraction", CtrlWriter::None,
	    "Nothing in the game writes it: it reads 0. The loader points an unknown or empty CTRL name here.",
	    "[orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640]");
	none(THREEDI_CTRL_LOD_FADE_IN, "lod", "LOD fade in");
	none(THREEDI_CTRL_LOD_FADE_OUT, "lod", "LOD fade out");
	// Lights, the sky and the weather.
	row(THREEDI_CTRL_FLICKER, "light", "Flicker", CtrlWriter::Ported,
	    "The weather's wave ring sampled at the model's place, before it draws: a light's flicker.",
	    "[orig: HUD_CacheEntityDisplayInfo @ 0x4A3D90; BoneCallback_gnrc_World @ 0x4E2860]");
	row(THREEDI_CTRL_SWING, "light", "Swing", CtrlWriter::Ported,
	    "The weather's wave oscillation sampled at the model's place, before it draws: its sway in the wind.",
	    "[orig: HUD_CacheEntityDisplayInfo @ 0x4A3D90; BoneCallback_Sway_World @ 0x4E2B22]");
	row(THREEDI_CTRL_UPL_INTENSITY, "light", "Sky body brightness", CtrlWriter::Ported,
	    "A sky body's brightness (the sun's, the moon's), written as the sky draws it.",
	    "[orig: Render_CelestialBodies @ 0x5ACAA0]", 0, 65536, true);
	for (int i = 0; i < 4; ++i) {
		static const char *const kSwitches[] = {"Light switch 0", "Light switch 1", "Light switch 2", "Light switch 3"};
		open(THREEDI_CTRL_LIGHTSWITCH0 + i, "light", kSwitches[i]);
	}
	// A person.
	row(THREEDI_CTRL_TALK, "person", "Talk", CtrlWriter::Ported,
	    "The level of the voice the person is playing, 0 to 1 (the mixer's meter): 0 for anyone else.",
	    "[orig: BoneCallback_org0_Skin @ 0x4E3620]", 0, 65536, true);
	row(THREEDI_CTRL_DEATH, "person", "Death", CtrlWriter::Ported,
	    "A dead person's corpse clock: (its timer - 62) / 186, 0 to 1, while it is under 248 ticks; 0xFFFF "
	    "otherwise (alive, or long dead).",
	    "[orig: BoneCallback_org0_Skin @ 0x4E3620]", 0, 65536, true);
	row(THREEDI_CTRL_NVG_FLIP, "person", "Night-vision flip", CtrlWriter::Ported,
	    "The goggles' flip as they draw on the head: 0xFFFF while the wearer's flag 2 is set, else 0.",
	    "[orig: BoneCallback_org0_World @ 0x4E3940]", 0, 65535);
	row(THREEDI_CTRL_PARA, "person", "Parachute", CtrlWriter::Ported,
	    "The parachute canopy's inflation, twice the word.", "[orig: BoneCallback_org0_World @ 0x4E3940]");
	row(THREEDI_CTRL_PARA_O, "person", "Parachute flap", CtrlWriter::Ported,
	    "The parachute canopy's flap, twice the word.", "[orig: BoneCallback_org0_World @ 0x4E3940]");
	// The team and the textures.
	row(THREEDI_CTRL_TEAMSWING, "team", "Team swing", CtrlWriter::Ported,
	    "A numbered zone's swing, as the generic world callback publishes it.",
	    "[orig: BoneCallback_gnrc_World @ 0x4E2860]");
	row(THREEDI_CTRL_LFP_CAMPPERCENT, "team", "Camp percent", CtrlWriter::Ported,
	    "A numbered zone's capture share: the timer over its limit, 0 to 1.",
	    "[orig: BoneCallback_gnrc_World @ 0x4E2860]", 0, 65536, true);
	row(THREEDI_CTRL_TEX_TEAM, "team", "Team texture", CtrlWriter::Ported,
	    "The team's texture variant (a signed byte), the frame a texture selector shows.",
	    "[orig: Render_SectorEntity @ 0x5C4190]", -128, 127);
	for (int i = 0; i < 3; ++i) {
		static const char *const kCamo[] = {"Camouflage 1", "Camouflage 2", "Camouflage 3"};
		row(THREEDI_CTRL_TEX_CAMO1 + i, "team", kCamo[i], CtrlWriter::Ported,
		    "A player part's camouflage variant (0 to 4), written before the head, the body and the arms each draw.",
		    "[orig: the part writers @ 0x57A370..0x57A3B0]", 0, 255);
	}
	// The head-up display.
	open(THREEDI_CTRL_HUD_HEALTH, "hud", "Health");
	open(THREEDI_CTRL_HUD_MANA, "hud", "Armor");
	none(THREEDI_CTRL_HUD_COMPASS, "hud", "Compass");
	// A weapon and its rounds.
	none(THREEDI_CTRL_WPN_TRIGGER, "weapon", "Trigger");
	none(THREEDI_CTRL_WPN_HAMMER, "weapon", "Hammer");
	open(THREEDI_CTRL_TRACER_SCALE, "weapon", "Tracer scale");
	open(THREEDI_CTRL_TRACER_WIDTH, "weapon", "Tracer width");
	// The doors.
	for (int i = 0; i < 16; ++i) {
		static const char *const kDoors[] = {"Door 0", "Door 1", "Door 2", "Door 3", "Door 4", "Door 5",
		                                     "Door 6", "Door 7", "Door 8", "Door 9", "Door 10", "Door 11",
		                                     "Door 12", "Door 13", "Door 14", "Door 15"};
		row(THREEDI_CTRL_DOOR_00 + i, "doors", kDoors[i], CtrlWriter::Ported,
		    "The item's door, as far open as the door system has swung it.",
		    "[orig: BoneCallback_BuildBoneTransforms @ 0x4E3070]");
	}
	// Particles.
	none(THREEDI_CTRL_PARTICLE_ALPHA, "particle", "Particle alpha");
	none(THREEDI_CTRL_PARTICLE_RGB, "particle", "Particle colour");
	// A helicopter.
	none(THREEDI_CTRL_HELO_REAR_GEAR, "helicopter", "Rear gear");
	none(THREEDI_CTRL_HELO_GEARDOORS, "helicopter", "Gear doors");
	row(THREEDI_CTRL_HELO_GEAR, "helicopter", "Gear", CtrlWriter::Ported,
	    "The landing gear's phase (the low word), as the helicopter class publishes it.",
	    "[orig: the chel CTRL publication @ 0x48F1A0]", 0, 65535);
	none(THREEDI_CTRL_HELO_GEARB, "helicopter", "Gear B");
	none(THREEDI_CTRL_HELO_BAYDOORS, "helicopter", "Bay doors");
	none(THREEDI_CTRL_HELO_PCANOPY, "helicopter", "Pilot canopy");
	none(THREEDI_CTRL_HELO_CPCANOPY, "helicopter", "Copilot canopy");
	row(THREEDI_CTRL_HELO_ROTOR, "helicopter", "Main rotor", CtrlWriter::Ported,
	    "The rotor's turn: the high word of the helicopter's rotor accumulator.",
	    "[orig: Entity_CacheVehicleHUDStats @ 0x4929B0]", 0, 65535);
	row(THREEDI_CTRL_HELO_TAILROTOR, "helicopter", "Tail rotor", CtrlWriter::Ported,
	    "The same rotor accumulator's high word as the main rotor's.",
	    "[orig: Entity_CacheVehicleHUDStats @ 0x4929B0]", 0, 65535);
	none(THREEDI_CTRL_HELO_PILOTYAW, "helicopter", "Pilot yaw");
	none(THREEDI_CTRL_HELO_PILOTPITCH, "helicopter", "Pilot pitch");
	none(THREEDI_CTRL_HELO_CPILOTYAW, "helicopter", "Copilot yaw");
	none(THREEDI_CTRL_HELO_CPILOTPITCH, "helicopter", "Copilot pitch");
	row(THREEDI_CTRL_HELO_GUNYAW, "helicopter", "Gun yaw", CtrlWriter::Ported,
	    "The gunner's aim across, as the helicopter class publishes it.",
	    "[orig: the chel CTRL publication @ 0x48F1A0]");
	row(THREEDI_CTRL_HELO_GUNPITCH, "helicopter", "Gun pitch", CtrlWriter::Ported,
	    "The gunner's aim up and down, as the helicopter class publishes it.",
	    "[orig: the chel CTRL publication @ 0x48F1A0]");
	// An emplaced gun.
	row(THREEDI_CTRL_HEAT_GLOW, "emplaced", "Heat glow", CtrlWriter::Ported,
	    "The emplaced gun's heat, written before every draw of the gun.",
	    "[orig: HUD_CacheWeaponSlotInfo @ 0x440930]");
	row(THREEDI_CTRL_EWEAP_GUNYAW, "emplaced", "Gun yaw", CtrlWriter::Ported,
	    "The emplaced gun's aim across, from its gunner.", "[orig: HUD_CacheWeaponSlotInfo @ 0x440930]");
	row(THREEDI_CTRL_EWEAP_GUNPITCH, "emplaced", "Gun pitch", CtrlWriter::Ported,
	    "The emplaced gun's aim up and down, from its gunner.", "[orig: HUD_CacheWeaponSlotInfo @ 0x440930]");
	row(THREEDI_CTRL_WEAP_SPIN, "emplaced", "Barrel spin", CtrlWriter::Ported,
	    "The emplaced gun's barrel turn: the class's unsigned phase word as it fires.",
	    "[orig: HUD_CacheWeaponSlotInfo @ 0x440930]", 0, 65535);
	// A vehicle.
	row(THREEDI_CTRL_VEHICLE_WHEELS, "vehicle", "Wheels", CtrlWriter::Ported,
	    "The wheels' turn: the high word of the vehicle's wheel phase.",
	    "[orig: Entity_CacheVehicleHUDStats @ 0x4929B0]", 0, 65535);
	row(THREEDI_CTRL_VEHICLE_STEERING, "vehicle", "Steering", CtrlWriter::Ported,
	    "The steering's high word, at most 0x10000.", "[orig: Entity_CacheVehicleHUDStats @ 0x4929B0]", 0,
	    65536, true);
	row(THREEDI_CTRL_VEHICLE_SPEED, "vehicle", "Speed", CtrlWriter::Ported,
	    "The vehicle's speed, its absolute value capped at 0x10000.",
	    "[orig: Entity_CacheVehicleHUDStats @ 0x4929B0]", 0, 65536, true);
	row(THREEDI_CTRL_VEHICLE_GUNYAW, "vehicle", "Turret yaw", CtrlWriter::Ported,
	    "The turret's aim across, as the tank class publishes it.", "[orig: the tank CTRL publication @ 0x449C10]");
	row(THREEDI_CTRL_VEHICLE_GUNPITCH, "vehicle", "Turret pitch", CtrlWriter::Ported,
	    "The turret's aim up and down, as the tank class publishes it.",
	    "[orig: the tank CTRL publication @ 0x449C10]");
	row(THREEDI_CTRL_VEHICLE_SPECIAL1, "vehicle", "Special 1", CtrlWriter::Ported,
	    "The PLAYPARTANIM channel 1 phase (not for an item of attrib 0x1000): it holds the value a command "
	    "set, as nothing advances it in the game.",
	    "[orig: Entity_ApplyCommand @ 0x43AB60; HUD_CacheEntityDisplayInfo @ 0x4A3D90]");
	row(THREEDI_CTRL_VEHICLE_SPECIAL2, "vehicle", "Special 2", CtrlWriter::Ported,
	    "The PLAYPARTANIM channel 2 phase: it holds the value a command set, as nothing advances it in the game.",
	    "[orig: Entity_ApplyCommand @ 0x43AB60; HUD_CacheEntityDisplayInfo @ 0x4A3D90]");
	for (int i = 0; i < 14; ++i) {
		static const char *const kTires[] = {"Tire 0", "Tire 1", "Tire 2", "Tire 3", "Tire 4", "Tire 5", "Tire 6",
		                                     "Tire 7", "Tire 8", "Tire 9", "Tire 10", "Tire 11", "Tire 12", "Tire 13"};
		if (i < 6)
			row(THREEDI_CTRL_VEHICLE_TIRE00 + i, "vehicle", kTires[i], CtrlWriter::Ported,
			    "A wheel's compression, 0 to 1 (front left and right, middle, rear right and left).",
			    "[orig: Entity_CacheVehicleHUDStats @ 0x4929B0]", 0, 65536, true);
		else
			row(THREEDI_CTRL_VEHICLE_TIRE00 + i, "vehicle", kTires[i], CtrlWriter::Ported,
			    "A road wheel's compression, as the tank class publishes it.",
			    "[orig: the tank CTRL publication @ 0x449C10]", 0, 65536, true);
	}
	for (int i = 0; i < 4; ++i) {
		static const char *const kTracks[] = {"Track 0", "Track 1", "Track 2", "Track 3"};
		row(THREEDI_CTRL_VEHICLE_WHEELS00 + i, "vehicle", kTracks[i], CtrlWriter::Ported,
		    "A track's turn (the left one, then the right, again), as the tank class publishes it.",
		    "[orig: the tank CTRL publication @ 0x449C10]", 0, 65535);
	}
	// The destruction: the husk's destroy fade (DI-10, preview/model_damage).
	row(THREEDI_CTRL_OBJECT_DESTROY, "destruction", "Destroy fade", CtrlWriter::Ported,
	    "Once the item is destroyed (its husk drawn), the fade's overall share: elapsed / (duration + 4 x "
	    "stagger), from the item's destroy_timing (0 takes 50 and 25 ticks), after its delay. 0 while intact.",
	    "[orig: Entity_PublishSwapFadePhases @ 0x5C3F40]", 0, 65536, true);
	for (int i = 0; i < 5; ++i) {
		static const char *const kPhases[] = {"Destroy phase 1", "Destroy phase 2", "Destroy phase 3",
		                                      "Destroy phase 4", "Destroy phase 5"};
		static const char *const kDriven[] = {
				"Once the item is destroyed, phase 1 of the fade: elapsed / duration, from the item's destroy_timing "
				"(0 takes 50 ticks), after its delay. 0 while intact.",
				"Once the item is destroyed, phase 2 of the fade: (elapsed - stagger) / duration, from the item's "
				"destroy_timing (0 takes 50 and 25 ticks). 0 while intact.",
				"Once the item is destroyed, phase 3 of the fade: (elapsed - 2 x stagger) / duration, from the "
				"item's destroy_timing. 0 while intact.",
				"Once the item is destroyed, phase 4 of the fade: (elapsed - 3 x stagger) / duration, from the "
				"item's destroy_timing. 0 while intact.",
				"Once the item is destroyed, phase 5 of the fade: (elapsed - 4 x stagger) / duration, from the "
				"item's destroy_timing. 0 while intact."};
		row(THREEDI_CTRL_OBJECT_DESTROY01 + i, "destruction", kPhases[i], CtrlWriter::Ported, kDriven[i],
		    "[orig: Entity_PublishSwapFadePhases @ 0x5C3F40]", 0, 65536, true);
	}
	return t;
}

} // namespace

const std::vector<CtrlRegisterGroup> &ctrl_register_groups() {
	static const std::vector<CtrlRegisterGroup> groups = {
			{"destruction", "Destruction"}, {"doors", "Doors"},
			{"vehicle", "Vehicle"},         {"helicopter", "Helicopter"},
			{"emplaced", "Emplaced gun"},   {"weapon", "Weapon and rounds"},
			{"person", "Person"},           {"team", "Team and camouflage"},
			{"light", "Lights, sky and weather"}, {"particle", "Particles"},
			{"hud", "Head-up display"},     {"lod", "Level of detail"},
	};
	return groups;
}

const char *ctrl_writer_token(CtrlWriter writer) {
	switch (writer) {
	case CtrlWriter::Ported: return "ported";
	case CtrlWriter::Open: return "open";
	case CtrlWriter::None: return "none";
	}
	return "none";
}

const CtrlRegisterWords &ctrl_register_words(int ordinal) {
	static const std::array<CtrlRegisterWords, THREEDI_CTRL_REGISTER_COUNT> table = make_table();
	static const CtrlRegisterWords empty{};
	return ordinal >= 0 && ordinal < THREEDI_CTRL_REGISTER_COUNT ? table[size_t(ordinal)] : empty;
}

bool ctrl_register_is_destroy_phase(int ordinal) {
	return ordinal >= THREEDI_CTRL_OBJECT_DESTROY && ordinal <= THREEDI_CTRL_OBJECT_DESTROY05;
}

} // namespace opennova::editor
