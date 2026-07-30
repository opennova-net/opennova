// Pins every def.h flag/attrib macro to its witnessed hex value, so a later
// refactor cannot silently renumber a witnessed bit. The parser tables in
// def_scan.cpp / def_ammo.cpp initialize from the same macros, so this file +
// the parse tests together prove name<->bit<->token integrity.
// Witnesses: weapon table [orig: @0x830bf0], item attrib tables
// [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md:147-160],
// ammo table [orig: @0x813500].
#include "def/def.h"

#include <cstdio>

// --- weapon.def flags (dword 1) ---
static_assert(DEF_WEAPON_FLAG_SCOPED == 0x00000001u);
static_assert(DEF_WEAPON_FLAG_SIGHTED == 0x00000002u);
static_assert(DEF_WEAPON_FLAG_UNDERWATER == 0x00000004u);
static_assert(DEF_WEAPON_FLAG_SHOWCOMANDER == 0x00000008u);
static_assert(DEF_WEAPON_FLAG_NOCLIPSNODRAW == 0x00000010u);
static_assert(DEF_WEAPON_FLAG_BURST == 0x00000020u);
static_assert(DEF_WEAPON_FLAG_NOTDROPABLE == 0x00000040u);
static_assert(DEF_WEAPON_FLAG_EMPLACED == 0x00000080u);
static_assert(DEF_WEAPON_FLAG_AUTO == 0x00000100u);
static_assert(DEF_WEAPON_FLAG_NORANGECHECK == 0x00000200u);
static_assert(DEF_WEAPON_FLAG_SHOWRANGE == 0x00000400u);
static_assert(DEF_WEAPON_FLAG_SHOWELEVATION == 0x00000800u);
static_assert(DEF_WEAPON_FLAG_ARMOR == 0x00001000u);
static_assert(DEF_WEAPON_FLAG_OKWHILEJUMPING == 0x00002000u);
static_assert(DEF_WEAPON_FLAG_ONLYFIRESCOPED == 0x00004000u);
static_assert(DEF_WEAPON_FLAG_LOLLYPOP == 0x00008000u);
static_assert(DEF_WEAPON_FLAG_ABSORBPITCH == 0x00010000u);
static_assert(DEF_WEAPON_FLAG_NOMOVE == 0x00020000u);
static_assert(DEF_WEAPON_FLAG_FORCECROUCH == 0x00040000u);
static_assert(DEF_WEAPON_FLAG_ONLYSCOPED == 0x00080000u);
static_assert(DEF_WEAPON_FLAG_2DIMPACT == 0x00100000u);
static_assert(DEF_WEAPON_FLAG_USEDESIGNATOR == 0x00200000u);
static_assert(DEF_WEAPON_FLAG_USESPREADTWO == 0x00400000u);
static_assert(DEF_WEAPON_FLAG_SHOWIMPACTDIST == 0x00800000u);
static_assert(DEF_WEAPON_FLAG_WHILESWIMMING == 0x01000000u);
static_assert(DEF_WEAPON_FLAG_NOCARDSWITCH == 0x02000000u);
static_assert(DEF_WEAPON_FLAG_HANDGUNUP == 0x04000000u);
static_assert(DEF_WEAPON_FLAG_QUICKSWITCH == 0x08000000u);
static_assert(DEF_WEAPON_FLAG_ONLYFIRELOCKED == 0x10000000u);
static_assert(DEF_WEAPON_FLAG_FORCESCOPED == 0x20000000u);
static_assert(DEF_WEAPON_FLAG_LASERBEAM == 0x40000000u);
static_assert(DEF_WEAPON_FLAG_POWERTHROW == 0x80000000u);

// --- weapon.def flags2 (dword 2) ---
static_assert(DEF_WEAPON_FLAG2_NOSELECT == 0x00000001u);
static_assert(DEF_WEAPON_FLAG2_PARACHUTE == 0x00000002u);
static_assert(DEF_WEAPON_FLAG2_THERMAL == 0x00000004u);
static_assert(DEF_WEAPON_FLAG2_MONITOR == 0x00000008u);
static_assert(DEF_WEAPON_FLAG2_VIEWLOCK == 0x00000010u);
static_assert(DEF_WEAPON_FLAG2_ONLYLOCKSCOPED == 0x00000020u);
static_assert(DEF_WEAPON_FLAG2_NOAMMOTYPES == 0x00000040u);
static_assert(DEF_WEAPON_FLAG2_SHOWHUDPIP == 0x00000080u);
static_assert(DEF_WEAPON_FLAG2_FIXVERTICALOFST == 0x00000100u);
static_assert(DEF_WEAPON_FLAG2_INSET == 0x00000200u);
static_assert(DEF_WEAPON_FLAG2_NOAUTOZERO == 0x00000400u);
static_assert(DEF_WEAPON_FLAG2_INVISIBLE == 0x00000800u);

// --- items.def attrib (ItemDefAttrib +0x54) ---
static_assert(DEF_ITEM_ATTRIB_MOVECB == 0x00000001u);
static_assert(DEF_ITEM_ATTRIB_POWERUP == 0x00000002u);
static_assert(DEF_ITEM_ATTRIB_NOMOVESHOOT == 0x00000004u);
static_assert(DEF_ITEM_ATTRIB_NOTOOL == 0x00000008u);
static_assert(DEF_ITEM_ATTRIB_SNAP == 0x00000010u);
static_assert(DEF_ITEM_ATTRIB_EWEAP == 0x00000020u);
static_assert(DEF_ITEM_ATTRIB_PLAYERCONTROL == 0x00000040u);
static_assert(DEF_ITEM_ATTRIB_DOOR == 0x00000080u);
static_assert(DEF_ITEM_ATTRIB_NOTARGET == 0x00000100u);
static_assert(DEF_ITEM_ATTRIB_LANDABLE == 0x00000200u);
static_assert(DEF_ITEM_ATTRIB_MISSILE == 0x00000400u);
static_assert(DEF_ITEM_ATTRIB_TIRE == 0x00000800u);
static_assert(DEF_ITEM_ATTRIB_FASTROPE == 0x00001000u);
static_assert(DEF_ITEM_ATTRIB_TAKEABLE == 0x00002000u);
static_assert(DEF_ITEM_ATTRIB_EASY == 0x00004000u);
static_assert(DEF_ITEM_ATTRIB_4TEAM == 0x00010000u);
static_assert(DEF_ITEM_ATTRIB_CHANGETEAM == 0x00020000u);
static_assert(DEF_ITEM_ATTRIB_SPAWNPOINT == 0x00040000u);
static_assert(DEF_ITEM_ATTRIB_ARMORY == 0x00080000u);
static_assert(DEF_ITEM_ATTRIB_AIDATA == 0x00100000u);
static_assert(DEF_ITEM_ATTRIB_LEAVECORPSE == 0x00400000u);
static_assert(DEF_ITEM_ATTRIB_NODISMEMBER == 0x00800000u);
static_assert(DEF_ITEM_ATTRIB_NOWEAPON == 0x01000000u);
static_assert(DEF_ITEM_ATTRIB_REFLECT == 0x02000000u);
static_assert(DEF_ITEM_ATTRIB_NOSHADOW == 0x04000000u);
static_assert(DEF_ITEM_ATTRIB_CONCAVE == 0x08000000u);
static_assert(DEF_ITEM_ATTRIB_NOSCAR == 0x10000000u);
static_assert(DEF_ITEM_ATTRIB_NOHUD == 0x20000000u);
static_assert(DEF_ITEM_ATTRIB_NODIE == 0x40000000u);

// --- items.def attrib2 (ItemDefAttrib2 +0x58) ---
static_assert(DEF_ITEM_ATTRIB2_VEHICLEBAY == 0x00000001u);
static_assert(DEF_ITEM_ATTRIB2_AUTOINHERITTEAM == 0x00000002u);
static_assert(DEF_ITEM_ATTRIB2_VEHICLESPAWN == 0x00000004u);
static_assert(DEF_ITEM_ATTRIB2_DYNAMICSHADOW == 0x00000010u);
static_assert(DEF_ITEM_ATTRIB2_STATICSHADOW == 0x00000020u);
static_assert(DEF_ITEM_ATTRIB2_TUNNELPIECE == 0x00000040u);
static_assert(DEF_ITEM_ATTRIB2_USEVK == 0x00000080u);
static_assert(DEF_ITEM_ATTRIB2_STATICDEATH == 0x00000100u);
static_assert(DEF_ITEM_ATTRIB2_ONTURRET == 0x00000400u);
static_assert(DEF_ITEM_ATTRIB2_HASTURRET == 0x00000800u);
static_assert(DEF_ITEM_ATTRIB2_ISTURRET == 0x00001000u);
static_assert(DEF_ITEM_ATTRIB2_FARP == 0x00002000u);
static_assert(DEF_ITEM_ATTRIB2_LANDMINE == 0x00004000u);

// --- ammo.def flags (pre-existing macros, pinned the same way) ---
static_assert(DEF_AMMO_FLAG_IGNOREDMG == 0x00000001u);
static_assert(DEF_AMMO_FLAG_IGNORE == 0x00000002u);
static_assert(DEF_AMMO_FLAG_SHRAPNEL == 0x00000004u);
static_assert(DEF_AMMO_FLAG_SILENCED == 0x00000008u);
static_assert(DEF_AMMO_FLAG_WATER == 0x00000010u);
static_assert(DEF_AMMO_FLAG_DETONATESATCHELS == 0x00000020u);
static_assert(DEF_AMMO_FLAG_NOSMOKE == 0x00000040u);
static_assert(DEF_AMMO_FLAG_NOCOLLIDE == 0x00000080u);
static_assert(DEF_AMMO_FLAG_NOGRAVITY == 0x00000100u);
static_assert(DEF_AMMO_FLAG_HASITEM == 0x00000200u);
static_assert(DEF_AMMO_FLAG_INSTANTKILLZONE == 0x00000400u);
static_assert(DEF_AMMO_FLAG_OWNERIMMUNE == 0x00000800u);
static_assert(DEF_AMMO_FLAG_USEOWNMOVE == 0x00002000u);
static_assert(DEF_AMMO_FLAG_NOAGE == 0x00004000u);
static_assert(DEF_AMMO_FLAG_FORCETRACER == 0x00008000u);
static_assert(DEF_AMMO_FLAG_SHOTGUN == 0x00010000u);
static_assert(DEF_AMMO_FLAG_CLAYMORE == 0x00020000u);
static_assert(DEF_AMMO_FLAG_NOOITEMS == 0x00080000u);
static_assert(DEF_AMMO_FLAG_NOMITEMS == 0x00100000u);
static_assert(DEF_AMMO_FLAG_NODITEMS == 0x00200000u);
static_assert(DEF_AMMO_FLAG_PRIORITY == 0x00800000u);
static_assert(DEF_AMMO_FLAG_CLIPWATER == 0x01000000u);
static_assert(DEF_AMMO_FLAG_DESIGNATETARGET == 0x02000000u);
static_assert(DEF_AMMO_FLAG_IGNORFOILAGE == 0x04000000u);
static_assert(DEF_AMMO_FLAG_LAWR == 0x08000000u);
static_assert(DEF_AMMO_FLAG_FGRENADE == 0x10000000u);
static_assert(DEF_AMMO_FLAG_CLIPWATERFX == 0x20000000u);

int main() {
	std::printf("def flag/attrib macros pinned to witnessed values\n");
	return 0;
}
