#include <editor/documents/animation_slots.h>

#include <algorithm>
#include <array>

#include <runtime/anim/adm_clip_index.h>
#include <runtime/anim/anim_slot_names.h>

namespace opennova::editor {

namespace {

// The families, in the order the headings list them.
enum Family : int {
	kReset,
	kOnFoot,
	kCrouched,
	kProne,
	kStill,
	kHolds,
	kAttacks,
	kAirLadderWater,
	kRotorWash,
	kEmplaced,
	kSeats,
	kBurning,
	kEmotes,
	kScriptedIdles,
	kDragging,
	kGuard,
	kDeaths,
	kFirstPerson,
	kOther,
	kFamilyCount
};

constexpr AnimSlotFamily kFamilies[kFamilyCount] = {
	{"00", "Reset (the rest pose)"},
	{"01", "Walking and running"},
	{"02", "Crouched"},
	{"03", "Prone"},
	{"04", "Standing still"},
	{"05", "Weapons in hand (upper body)"},
	{"06", "Attacks and cover"},
	{"07", "In the air, on ladders, in water"},
	{"08", "In a helicopter's rotor wash"},
	{"09", "Emplaced guns"},
	{"10", "Seats"},
	{"11", "On fire"},
	{"12", "Emotes"},
	{"13", "Scripted idles"},
	{"14", "Dragging a body"},
	{"15", "Guard"},
	{"16", "Deaths"},
	{"17", "First person (a weapon's view model)"},
	{"18", "Other"},
};
constexpr AnimSlotFamily kUnknownFamily{"99", "Keys naming no slot (the game skips these rows)"};

// A run of slots, its family and, where one sentence says them all, its words.
struct SlotRun {
	int first, last;
	Family family;
};

// The families by slot: the state table's runs as world-wac-ai-re.md 3.4 reads them
// [orig: g_AnimStateNameTable @ 0x8135F0, its 252 entries scanned by AnimMap_FindSlotByName @
// 0x40CFA0], the deaths 19.1's, the holds and attacks 14.8.2's, the first person 240..251.
constexpr SlotRun kRuns[] = {
	{0, 0, kReset},
	{1, 10, kOnFoot},
	{11, 18, kCrouched},
	{19, 26, kProne},
	{27, 29, kRotorWash},
	{30, 40, kAirLadderWater},
	{41, 42, kProne},
	{43, 44, kStill},
	{45, 45, kCrouched},
	{46, 46, kStill},
	{47, 47, kAirLadderWater},
	{48, 48, kProne},
	{49, 49, kStill},
	{50, 61, kHolds},
	{62, 63, kAttacks},
	{64, 66, kHolds},
	{67, 75, kEmplaced},
	{76, 110, kSeats},
	{111, 114, kBurning},
	{115, 124, kEmotes},
	{125, 135, kScriptedIdles},
	{136, 136, kOther},
	{137, 139, kDragging},
	{140, 144, kGuard},
	{145, 149, kOnFoot},
	{150, 150, kAirLadderWater},
	{151, 152, kAttacks},
	{153, 153, kOther},
	{154, 154, kAirLadderWater},
	{155, 167, kAttacks},
	{168, 168, kOnFoot},
	{169, 171, kCrouched},
	{172, 172, kProne},
	{173, 239, kDeaths},
	{240, 251, kFirstPerson},
};

const char *const kDirections[8] = {"forward",    "forward and to the right", "to the right", "back and to the right",
                                    "backward",   "back and to the left",     "to the left",  "forward and to the left"};
const char *const kSides[4] = {"the front", "the right", "the back", "the left"};
const char *const kBodyParts[15] = {"hip",       "torso",     "head",       "right shoulder", "left shoulder",
                                    "right arm", "left arm",  "right hand", "left hand",      "right thigh",
                                    "left thigh", "right calf", "left calf", "right foot",    "left foot"};
const char *const kHoldWords[12] = {"a knife-type weapon (weapon.def special_hold 1)",
                                "a pistol (special_hold 2)",
                                "a grenade (special_hold 3)",
                                "a launcher (special_hold 4)",
                                "a designator (special_hold 5)",
                                "a designator, looking through its sight",
                                "a P90 (special_hold 6)",
                                "a P90, looking through its sight",
                                "an MP7 (special_hold 7)",
                                "an MP7, looking through its sight",
                                "a javelin (special_hold 8)",
                                "a javelin, looking through its sight"};
const char *const kFirstPersonWords[12] = {
	"The view model's rest pose.",
	"Holding the weapon (its idle).",
	"Holding the weapon empty.",
	"Firing.",
	"The recoil after a shot.",
	"Reloading.",
	"Firing on an empty magazine.",
	"Drawing the weapon.",
	"Putting the weapon away.",
	"Changing the fire mode.",
	"Raising the sight.",
	"Lowering the sight.",
};

std::string number_after(const char *name, size_t prefix) {
	const std::string text = name;
	return text.size() > prefix ? text.substr(prefix) : std::string();
}

} // namespace

int animation_slot_family(int slot) {
	for (const SlotRun &run : kRuns)
		if (slot >= run.first && slot <= run.last) return run.family;
	return -1;
}

size_t animation_slot_family_count() { return kFamilyCount; }

const AnimSlotFamily &animation_slot_family_row(int family) {
	return family >= 0 && family < kFamilyCount ? kFamilies[family] : kUnknownFamily;
}

const AnimSlotFamily &animation_slot_unknown_family() { return kUnknownFamily; }

int animation_key_slot(const std::string &key) { return anim::adm_slot_index(key); }

std::string animation_slot_words(int slot) {
	if (slot < 0 || slot >= anim::kAnimSlotCount) return std::string();
	std::string words = anim::kAnimSlotNames[slot];
	std::replace(words.begin(), words.end(), '_', ' ');
	return words;
}

// Who selects each slot, as the RE records witness it; a slot whose selector is not traced says so.
std::string animation_slot_meaning(int slot) {
	if (slot < 0 || slot >= anim::kAnimSlotCount) return std::string();
	const char *name = anim::kAnimSlotNames[slot];
	// The rest pose: the rig's bind is the reset row's last clip that loads, and every slot a
	// map leaves out serves the reset row's first [orig: AnimMap_RegisterBoneNode @ 0x40C2D0, slot 0's
	// head replaced @ 0x40C38B; AnimMap_LoadAdmFile @ 0x40CC40, the bind pinned @ 0x40CE19;
	// AnimMap_RegisterEntity @ 0x40BB60, the backfill @ 0x40BC24, @ 0x40BD2E].
	if (slot == 0)
		return "The rest pose. The rig's skeleton is this row's last clip, and every body slot the map leaves out "
		       "plays this row's first clip.";
	if (slot >= 1 && slot <= 8) return std::string("Walking ") + kDirections[slot - 1] + ".";
	// The stance transitions: from 149, 1, 9 or 10 to prone through 172 [orig: AnimMap_UpdateEntity @
	// 0x40B5F0, the interposition].
	if (slot == 9 || slot == 10)
		return "A run (" + animation_slot_words(slot) + "); dropping prone from it plays run to prone first.";
	if (slot >= 11 && slot <= 18) return std::string("Walking crouched, ") + kDirections[slot - 11] + ".";
	if (slot >= 19 && slot <= 26) return std::string("Crawling prone, ") + kDirections[slot - 19] + ".";
	// The rotor wash: within 15 units of a rotor the walk, the jog and run and the idles turn into
	// 28, 29 and 27 when the map has them; the player's body has no wash run [orig:
	// Entity_UpdateInfantryAI @ 0x4B9910, the site @ 0x4BD78D; Entity_UpdateInfantryPlayerBody @
	// 0x4B40E0, the site @ 0x4B73A9; RotorWash_FindNearbyActiveZone @ 0x5CBE30].
	if (slot == 27) return "Standing in a helicopter's rotor wash: an idle within 15 units of a rotor plays this when the map has it.";
	if (slot == 28) return "Walking in a helicopter's rotor wash: a walk within 15 units of a rotor plays this when the map has it.";
	if (slot == 29)
		return "Running in a helicopter's rotor wash: an NPC's jog or run within 15 units of a rotor plays this when "
		       "the map has it (a player's body has no wash run).";
	if (slot == 30) return "The start of a jump.";
	if (slot == 31) return "In the air, after a jump or in a fall with no parachute.";
	if (slot == 32) return "On a ladder, holding still.";
	if (slot == 33) return "Climbing a ladder.";
	if (slot == 34) return "Climbing down a ladder.";
	if (slot == 35) return "Stepping off the top of a ladder.";
	// In water the gaits become 37, the idles 36 and the attacks 154, each when the map has it
	// [orig: Entity_UpdateInfantryAI @ 0x4BD635..0x4BD6A3].
	if (slot == 36) return "Treading water: in water the idles play this when the map has it.";
	if (slot == 37) return "Swimming forward: in water the walk and the runs play this when the map has it.";
	if (slot == 38) return "Swimming to the left.";
	if (slot == 39) return "Swimming to the right.";
	if (slot == 40) return "Swimming backward.";
	if (slot == 41) return "A combat roll to the left.";
	if (slot == 42) return "A combat roll to the right.";
	if (slot == 43) return "Standing still: the default idle.";
	// An NPC with no target scans in 44, one holding with a target holds in 49; a scoped player's
	// hold is 49 [orig: Entity_UpdateInfantryAI @ 0x4BC34B..0x4BC4BE; Entity_UpdateInfantryPlayerBody,
	// the hold selection @ 0x4B5DAD..0x4B5EA9].
	if (slot == 44) return "An NPC standing and looking about, with no target.";
	if (slot == 45) return "Crouched and still.";
	if (slot == 46) return "A mortar idle; when the game plays it is not traced.";
	if (slot == 47) return "Under a parachute.";
	if (slot == 48) return "Lying prone and still.";
	if (slot == 49) return "Holding position, weapon up: an NPC holding with a target, a player looking through a scope.";
	// The holds by weapon.def special_hold: 1..4 -> 50..53, 5..8 -> 54/56/58/60, +1 scoped; 64
	// binoculars; 65 reload, 66 a pistol's [orig: Entity_UpdateInfantryPlayerBody @ 0x4B5DAD..0x4B5EA9;
	// WeaponDefs_ParseLineCallback @ 0x543CB7].
	if (slot >= 50 && slot <= 61) return std::string("The upper body holding ") + kHoldWords[slot - 50] + ".";
	// The attack stamps by weapon.def attack_anim [orig: WeaponAction_Fire @ 0x542BCB, @ 0x542BE0].
	if (slot == 62) return "The upper body's knife attack: a weapon whose weapon.def attack_anim is 1.";
	if (slot == 63) return "The upper body's grenade throw: a weapon whose weapon.def attack_anim is 2.";
	if (slot == 64) return "The upper body holding binoculars up.";
	// An NPC with an empty magazine reloads in 65, the magazine refilling while it does [orig:
	// Entity_UpdateInfantryAI @ 0x4BD132..0x4BD17D].
	if (slot == 65) return "Reloading (upper body); an NPC with an empty magazine plays it, and the magazine refills while it does.";
	if (slot == 66) return "Reloading a pistol (a weapon whose special_hold is 2), upper body.";
	// An emplaced gunner by the gun's phrase_set [world-wac-ai-re.md 3.4 item 15, the mount pass].
	if (slot >= 67 && slot <= 75)
		return "Manning an emplaced gun: the gun's item picks emplaced 1 to 9 (its phrase_set); this is " +
		       animation_slot_words(slot) + ".";
	// A seat's pose: sit_N with N the seat bone name's number (76 + N); a bike's driver leans 107..110
	// by the steering and the speed [world-wac-ai-re.md 3.4 item 15].
	if (slot == 76) return "Seated: a seat whose bone carries the number 0 (sit N plays for the number N).";
	if (slot >= 77 && slot <= 106)
		return "Seated on a seat whose bone carries the number " + number_after(name, 4) + ".";
	if (slot == 107) return "A bike's rider (seat 24) stopped.";
	if (slot == 108) return "A bike's rider (seat 24) riding backward.";
	if (slot == 109) return "A bike's rider (seat 24) steering left.";
	if (slot == 110) return "A bike's rider (seat 24) steering right.";
	// The burn states 1..4 [orig: Entity_UpdateInfantryAI, the burn selection, world-wac-ai-re.md 17].
	if (slot >= 111 && slot <= 114) return "On fire, burn stage " + std::to_string(slot - 110) + ".";
	// A player's emote N stamps 114 + N on the upper body, and the hold waits for it [orig:
	// NapiNPClientMsg_HandleEmote @ 0x427F12; Entity_UpdateInfantryPlayerBody, the commit @ 0x4B5E72].
	if (slot >= 115 && slot <= 124)
		return "A player's emote " + std::to_string(slot - 114) + " (upper body); the weapon hold waits for it to end.";
	// The scripted idles; in 130..136 the body turns to watch the player [orig: Entity_UpdateInfantryAI
	// @ 0x4BCFF5..0x4BD0F4].
	if (slot >= 125 && slot <= 129) return "A scripted idle.";
	if (slot >= 130 && slot <= 135) return "A scripted idle in which the body turns to watch the player.";
	if (slot == 136) return "Hover: a scripted idle in which the body turns to watch the player.";
	if (slot == 137) return "Dragging a body, standing.";
	if (slot == 138) return "Dragging a body, walking.";
	if (slot == 139) return "Being dragged (the body dragged).";
	// The guard family [orig: Entity_UpdateInfantryAI @ 0x4BD19B..0x4BD231].
	if (slot == 140) return "On guard: an NPC told to guard plays it when the map has it.";
	if (slot == 141) return "On guard, looking; when the game plays it is not traced.";
	if (slot == 142) return "Firing from guard: an NPC on guard that reacts to a target, when the map has it.";
	if (slot == 143) return "Hit while on guard, when the map has it.";
	if (slot == 144) return "Leaving guard: an NPC that stops guarding passes through it, when the map has it.";
	if (slot == 145) return "Walking wounded.";
	if (slot == 146) return "Running wounded.";
	// Turning on the spot, the gaits by distance [orig: Entity_UpdateInfantryAI @ 0x4B9910].
	if (slot == 147) return "Turning on the spot: an NPC turning more than about 45 degrees.";
	if (slot == 148) return "Jogging forward: an NPC's gait by how far it has to go (jog, walk or run).";
	if (slot == 149) return "Running forward: an NPC's gait by how far it has to go (jog, walk or run).";
	if (slot == 150) return "Holding a rope: an NPC that boards by its attach point.";
	if (slot == 151) return "After a kill: an NPC over its dead target, when the map has it.";
	if (slot == 152) return "Before an attack; when the game plays it is not traced.";
	if (slot == 153) return "Out of the ground; when the game plays it is not traced.";
	if (slot == 154) return "Firing while swimming: in water attack and attack 2 play this when the map has it.";
	if (slot >= 155 && slot <= 158) return "An NPC's attack (" + animation_slot_words(slot) + "). A player's rifle fire plays no body clip.";
	if (slot >= 159 && slot <= 162) return "Throwing a grenade (" + animation_slot_words(slot) + ").";
	if (slot == 163) return "In cover, still.";
	if (slot == 164) return "Running to cover.";
	if (slot == 165 || slot == 166) return "Firing from cover.";
	// A body moving to its target in 149 runs and fires in 167 when authored; 168's only producer
	// is unreachable [orig: Entity_UpdateInfantryAI @ 0x4BD748..0x4BD763, @ 0x4BD765..0x4BD780].
	if (slot == 167) return "Firing on the run: an NPC running at its target plays it when the map has it.";
	if (slot == 168) return "Running away; the game never plays it (its only selector is unreachable).";
	if (slot >= 169 && slot <= 172)
		return std::string("Dropping ") + (slot == 172 ? "prone" : "to a crouch") +
		       " from a run: the game plays it between the two (" + animation_slot_words(slot) + ").";
	// The death selector [orig: Entity_ComputeAnimSlotIndex @ 0x43A690; OrganicClass_HandleEvent @
	// 0x407310: the bullet kill by the bone hit and the round's heading].
	if (slot == 173) return "Killed by fire: an incendiary round burns whatever it kills.";
	if (slot == 174) return "Killed with no other cause the game names (its default death).";
	if (slot == 175) return "Drowned.";
	if (slot >= 176 && slot <= 179) return std::string("Killed by an explosion from ") + kSides[slot - 176] + ".";
	if (slot >= 180 && slot <= 239)
		return std::string("Shot dead in the ") + kBodyParts[(slot - 180) / 4] + ", from " + kSides[(slot - 180) % 4] +
		       ": the bone the round hit picks the part, its heading the side.";
	// A weapon's actions play their slots on its own first-person map [orig: ActionSlot_BeginActivePhase
	// @ 0x53F830, AnimMap_PlayAnimBySlot(*(WeaponDef+0x174), action+24)].
	if (slot >= 240 && slot <= 251)
		return std::string("First person: ") + kFirstPersonWords[slot - 240] + " A weapon's map plays it (weapon.def animadm).";
	return std::string();
}

} // namespace opennova::editor
