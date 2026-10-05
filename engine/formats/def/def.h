/* DEF file parser — pure C API.
 * Stable plain C structs for native runtime consumers.
 * Parses Novalogic .def files: weapon.def, items.def, ammo.def, hudpos.def.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>


namespace opennova::def {

/* ========================================================================= */
/* Ammo Definitions                                                          */
/* ========================================================================= */

typedef struct DefEffectTableEntry {
    char surface_type[64];
    char hit_effect[128];
    char impact_sound[128];
    int value;
} DefEffectTableEntry;

/* DefAmmoDef.flags bits — the ammo.def `flag <name>` OR-mask, record dword +0.
 * [orig: the 30-entry name/bit table @0x813500 walked first-match by
 * AmmoDef_ParseProperty @0x40a2d0; docs/net/novaworld-net-re.md §5.60] */
inline constexpr uint32_t DEF_AMMO_FLAG_IGNOREDMG = 0x00000001u;
inline constexpr uint32_t DEF_AMMO_FLAG_IGNORE = 0x00000002u;
inline constexpr uint32_t DEF_AMMO_FLAG_SHRAPNEL = 0x00000004u;
inline constexpr uint32_t DEF_AMMO_FLAG_SILENCED = 0x00000008u;
inline constexpr uint32_t DEF_AMMO_FLAG_WATER = 0x00000010u;
inline constexpr uint32_t DEF_AMMO_FLAG_DETONATESATCHELS = 0x00000020u;
inline constexpr uint32_t DEF_AMMO_FLAG_NOSMOKE = 0x00000040u;
inline constexpr uint32_t DEF_AMMO_FLAG_NOCOLLIDE = 0x00000080u;
inline constexpr uint32_t DEF_AMMO_FLAG_NOGRAVITY = 0x00000100u;
inline constexpr uint32_t DEF_AMMO_FLAG_HASITEM = 0x00000200u;
inline constexpr uint32_t DEF_AMMO_FLAG_INSTANTKILLZONE = 0x00000400u;
inline constexpr uint32_t DEF_AMMO_FLAG_OWNERIMMUNE = 0x00000800u;
inline constexpr uint32_t DEF_AMMO_FLAG_USEOWNMOVE = 0x00002000u;
inline constexpr uint32_t DEF_AMMO_FLAG_NOAGE = 0x00004000u;
inline constexpr uint32_t DEF_AMMO_FLAG_FORCETRACER = 0x00008000u;
inline constexpr uint32_t DEF_AMMO_FLAG_SHOTGUN = 0x00010000u;
inline constexpr uint32_t DEF_AMMO_FLAG_CLAYMORE = 0x00020000u;
inline constexpr uint32_t DEF_AMMO_FLAG_NOOITEMS = 0x00080000u;
inline constexpr uint32_t DEF_AMMO_FLAG_NOMITEMS = 0x00100000u;
inline constexpr uint32_t DEF_AMMO_FLAG_NODITEMS = 0x00200000u;
inline constexpr uint32_t DEF_AMMO_FLAG_PRIORITY = 0x00800000u;
inline constexpr uint32_t DEF_AMMO_FLAG_CLIPWATER = 0x01000000u;
inline constexpr uint32_t DEF_AMMO_FLAG_DESIGNATETARGET = 0x02000000u;
inline constexpr uint32_t DEF_AMMO_FLAG_IGNORFOILAGE = 0x04000000u;
inline constexpr uint32_t DEF_AMMO_FLAG_LAWR = 0x08000000u;
inline constexpr uint32_t DEF_AMMO_FLAG_FGRENADE = 0x10000000u;
inline constexpr uint32_t DEF_AMMO_FLAG_CLIPWATERFX = 0x20000000u;

/* Ammo kill-zone types, record word +44 — drives the RoundData_SpawnRound ammo-class
 * dispatch (1 Knife = the immediate raycast, 6 Bullets = the standard projectile).
 * [orig: 8-name table @0x8133E0 rounds_kz_*; §5.60] */
inline constexpr int DEF_AMMO_KZ_NULL = 0;
inline constexpr int DEF_AMMO_KZ_KNIFE = 1;
inline constexpr int DEF_AMMO_KZ_STANDARD = 2;
inline constexpr int DEF_AMMO_KZ_MEDIC = 3;
inline constexpr int DEF_AMMO_KZ_RADIUSBLAST = 4;
inline constexpr int DEF_AMMO_KZ_C4 = 5;
inline constexpr int DEF_AMMO_KZ_BULLETS = 6;
inline constexpr int DEF_AMMO_KZ_SLASH = 7;

typedef struct DefAmmoDef {
    char name[64];
    int velocity;            /* +4, integer units/s [orig: 'velocity' branch @0x40a2d0] */
    int heat_det_range;      /* +100, signed world-unit word */
    int boresight_maxang;    /* +88, BAM cone */
    int min_damage;          /* +188 */
    int max_damage;          /* +192 */
    int penetration_impact;  /* +196 — must reach the target itemDef+400 armor threshold */
    int penetration_kz;      /* +200 */
    int armor_density[3];    /* +204/+208/+212, indexed by shooter ammo class */
    int secondary_anim;      /* byte +224: 'secondary_anim' (atol narrowed at bake) */
    int kz_physics;          /* byte +225: 'kz_physics' (atol narrowed at bake) */
    int recoil[3];           /* bytes +227..229 */
    /* Authoritative round-sim fields (docs/net/novaworld-net-re.md §5.60). */
    unsigned int flags;      /* +0, DEF_AMMO_FLAG_* OR-mask */
    int max_age_ticks;       /* +8: 'max_age' seconds -> 62 Hz ticks [orig: AmmoDef_ParseSecondsToTicks (ex sub_40A0F0) @0x40a0f0] */
    int arm_age_ticks;       /* +12: 'arm_age' — a hit before arming spawns notarmmed_ammo */
    int error_fp16;          /* +24: ballistic dispersion, 16.16 */
    int drag_fp16;           /* +28: drag, 16.16 */
    int bullet_radius_fp16;  /* +32: hit-test radius, 16.16 */
    int spread_count;        /* +48: shotgun/claymore pellet count */
    int kztype;              /* word +44, DEF_AMMO_KZ_* */
    int kz_damage;           /* word +46 */
    int weight_in_grains;    /* +184 — the kinetic damage mass term [orig: @0x4ecb1a] */
    int min_stable_velocity; /* +176 */
    int tumble_error_fp16;   /* +180: below-stable random deflection, 16.16 */
    int tracer_rate;         /* byte +226 ('tracerRate') */
    char notarmmed_ammo[64]; /* +241: the not-armed child ammo name ('notarmmedammo') */
    /* Embedder fire-presentation fields (world-wac-ai-re §17.4). The original resolves
     * both names at parse time (+64 = SoundBank_FindSetByNameAnyBank set ptr, +68 =
     * CEffectWorld_InternEffectHandle handle [orig: AmmoDef_ParseProperty
     * @0x40a8c8/@0x40a8f6]); we keep the names and resolve in the embedder at play. */
    char ai_launch[64];       /* +64: 'ai_launch' fire sound-set name */
    char ai_launcheffect[64]; /* +68: 'ai_launcheffect' muzzle effect name */
    int mf_light;             /* +36: 'MF_Light' presence flag [orig: @0x40a81b = 1] */
    int mf_light_value;       /* +40: 'MF_Light' value (atol) [orig: @0x40a837] */
    /* Tracer visual styles, 'tracer_type <friendly> [<enemy>]' — witnessed id map in
     * ammo_tracer_type_from_string; one value copies into both [orig: @0x40a78b-0x40a7fa]. */
    int tracer_type_friendly; /* +232 */
    int tracer_type_enemy;    /* +236 */
    /* The tracer round's visible item models, 'frndlyTrcrID <type_id>' /
     * 'foeTrcrID <type_id>'. The original resolves the ITEMS.DEF type id to an item
     * INDEX at parse (ItemList_FindIndexByTypeId, name fallback, warning on miss)
     * [orig: AmmoDef_ParseProperty @0x40a5f8-0x40a68d -> +16/+20]; we keep the raw
     * type id and the embedder resolves at use. 0 = none. */
    int frndly_trcr_type_id; /* +16 (pre-resolve) */
    int foe_trcr_type_id;    /* +20 (pre-resolve) */
    /* The in-flight round glow, 'light_move <radius> <r> <g> <b>' — spawned per round
     * into the light pool, follows the round, cleared on death [orig: parse
     * @0x40a2d0 'light_move' -> +120 radius 16.16 / +124 (r<<16)|(g<<8)|b; consumer
     * RoundData_SpawnRound @0x4ec8a9 -> LightPool_SpawnGlowEffect, handle round+0x1B4]. */
    int light_move_radius_fp16; /* +120 */
    int light_move_color;       /* +124: packed 0xRRGGBB */
    /* The guided-pursuit turn clamps, 'turnrate_maxpit'/'turnrate_maxyaw'
     * (deg/s -> BAM/tick: (192426 * fp16 + 0x8000) >> 16 [orig:
     * AmmoDef_ParseProperty -> AmmoDef_ParseTurnRate @0x40a130, stored +0x50/+0x54];
     * 0 = the flight integrator's 6734910 default —
     * world/guided_missile_flight.h). */
    int turnrate_maxpit; /* +80 */
    int turnrate_maxyaw; /* +84 */
    DefEffectTableEntry *effects_table;
    size_t effects_table_count;
    char (*raw_lines)[512];
    size_t raw_lines_count;
    /* Kill-zone blast geometry (appended; layout stability). The explosion
     * queue's blast radius is kz_maxradius (or the entry's float override); the
     * linear damage falloff starts at kz_minradius; kz_pieslice != 0 makes the
     * blast a cone around the entry direction. [orig: AmmoDef_ParseProperty
     * 'kz_minradius'/'kz_maxradius' -> +52/+56 fp16, 'kz_pieslice' -> +60
     * deg * 11930464 BAM; consumers Projectile_ProcessExplosionQueue @0x4ead80,
     * Entity_ApplyWeaponDamage @0x4e6931/@0x4e695a] */
    int kz_minradius_fp16;   /* +52 */
    int kz_maxradius_fp16;   /* +56 */
    int kz_pieslice_bam;     /* +60 */
    /* The impact flash light, 'light_impact <radius> <r> <g> <b> <seconds>' —
     * a fading pool light at the impact point (appended; layout stability).
     * [orig: AmmoDef_ParseProperty @0x40af79 -> +132 radius ParseFixedPoint16,
     * +128 ((r<<8)+g)<<8 + b, +136 seconds -> 62 Hz ticks (AmmoDef_ParseSecondsToTicks), 0 -> 10
     * @0x40b005; consumer AmmoDef_ProcessImpactEffect @0x40a280 ->
     * LightPool_SpawnGlowEffect(pos + radius/2 up, radius, color, mode 2,
     * ticks)]. */
    int light_impact_radius_fp16; /* +132 */
    int light_impact_color;       /* +128: packed 0xRRGGBB */
    int light_impact_ticks;       /* +136: authored seconds * 62; 0 -> 10 */
    /* The permanent terrain scorch selector, 'scorch_id <n>' (appended for
     * layout stability). Projectile terrain impacts route this word through
     * the permanent terrain-cache scorch registry. [orig: word +0x74;
     * Projectile_HandleTerrainImpact @0x4E9314 -> sub_6060D0] */
    int scorch_id;               /* word +0x74 */
    /* The impact scar kind, 'scar_type <n>' (appended; layout stability):
     * 0 = no mark, 1 = the ordinary ring scar, 2 = glass-only (the projected
     * glass decal leg without the ring fallback). [orig: AmmoDef_ParseProperty
     * @0x40aeea..0x40af11 atol -> word +0x76; consumer AmmoDef_ProcessImpactEffect
     * @0x40a24e..0x40a264 -> Impact_SpawnGlassEffectsOrScar @0x5cf1b0] */
    int scar_type;                /* word +0x76 */
    /* The blast's per-victim presentation (appended; layout stability):
     * 'secondary_effect <name>' and 'kz_sound <set>'. The original resolves
     * both at parse (the interned effect handle +0x48, the sound-set pointer
     * +0x4C); we keep the names. [orig: AmmoDef_ParseProperty
     * @0x40aa15..0x40aa36 / @0x40a92a..0x40a94b; consumer
     * Projectile_ProcessExplosionQueue @0x4EB1DA..0x4EB292] */
    char secondary_effect[64];    /* +0x48 */
    char kz_sound[64];            /* +0x4C */
} DefAmmoDef;

typedef struct DefAmmoFile {
    DefAmmoDef *entries;
    size_t count;
} DefAmmoFile;

/* ========================================================================= */
/* Weapon Definitions                                                        */
/* ========================================================================= */

typedef enum DefSightBlendMode {
    DEF_SIGHT_BLEND_BLEND = 0,
    DEF_SIGHT_BLEND_ADD = 1,
    DEF_SIGHT_BLEND_BLEND_AT = 2,
    DEF_SIGHT_BLEND_MULTIPLY = 3,
    DEF_SIGHT_BLEND_ADD_AT = 4,
    DEF_SIGHT_BLEND_MULTIPLY_AT = 5
} DefSightBlendMode;

typedef struct DefSightEntry {
    char texture[128];
    int x1, y1, x2, y2;
    int blend;        /* DefSightBlendMode */
    int scale;        /* boolean */
    int slide;        /* boolean */
    int slide_frames;
} DefSightEntry;

typedef struct DefWeaponAction {
    char name[64];
    char anim[128];
    char function[128];
    int delaystart;
    int delayend;
    char soundset[128];
    char soundsetend[128];
    char particle[128];
    char particleuserpoint[128];
	int action_value; /* ActionDef+52: mounted tank recoil amplitude [orig: @0x40270F] */
	char (*raw_lines)[512];
	size_t raw_lines_count;
	/* Where the row's live block stands in the parsed text: the 0-based index
	   of the `action` line that opened it and of the `end` that closed it. A
	   later block of the same name replaces the row, lines included. Lines are
	   numbered as the parser splits them, at LF with a CR before it dropped;
	   for CR LF text that is the retail walk's numbering (File_ParseASCIIFile
	   @0x53D810 cuts at CR LF only), but the parser keeps an unterminated tail
	   line whole where retail drops its last byte (@0x53D8E9 / @0x53D8EC).
	   A tool that rewrites the file in place (opennova-3di weapon merge) maps
	   them back onto the text and checks the retail reading of those lines. */
	size_t open_line;
	size_t end_line;
} DefWeaponAction;

/* DefWeaponDef.flags bits — the weapon.def `flags <name>` OR-mask (dword 1 of the
 * flag pair). Token spellings are witnessed as-is (showcomander, notdropable,
 * fixverticalofst are the original's).
 * [orig: the 16-B-stride {name, 0, flags1 bit, flags2 bit} table @0x830bf0;
 * def_scan.cpp's flag_table initializes from these] */
inline constexpr uint32_t DEF_WEAPON_FLAG_SCOPED = 0x00000001u;
inline constexpr uint32_t DEF_WEAPON_FLAG_SIGHTED = 0x00000002u;
inline constexpr uint32_t DEF_WEAPON_FLAG_UNDERWATER = 0x00000004u;
inline constexpr uint32_t DEF_WEAPON_FLAG_SHOWCOMANDER = 0x00000008u;
inline constexpr uint32_t DEF_WEAPON_FLAG_NOCLIPSNODRAW = 0x00000010u;
inline constexpr uint32_t DEF_WEAPON_FLAG_BURST = 0x00000020u;
inline constexpr uint32_t DEF_WEAPON_FLAG_NOTDROPABLE = 0x00000040u;
inline constexpr uint32_t DEF_WEAPON_FLAG_EMPLACED = 0x00000080u;
inline constexpr uint32_t DEF_WEAPON_FLAG_AUTO = 0x00000100u;
inline constexpr uint32_t DEF_WEAPON_FLAG_NORANGECHECK = 0x00000200u;
inline constexpr uint32_t DEF_WEAPON_FLAG_SHOWRANGE = 0x00000400u;
inline constexpr uint32_t DEF_WEAPON_FLAG_SHOWELEVATION = 0x00000800u;
inline constexpr uint32_t DEF_WEAPON_FLAG_ARMOR = 0x00001000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_OKWHILEJUMPING = 0x00002000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_ONLYFIRESCOPED = 0x00004000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_LOLLYPOP = 0x00008000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_ABSORBPITCH = 0x00010000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_NOMOVE = 0x00020000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_FORCECROUCH = 0x00040000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_ONLYSCOPED = 0x00080000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_2DIMPACT = 0x00100000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_USEDESIGNATOR = 0x00200000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_USESPREADTWO = 0x00400000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_SHOWIMPACTDIST = 0x00800000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_WHILESWIMMING = 0x01000000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_NOCARDSWITCH = 0x02000000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_HANDGUNUP = 0x04000000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_QUICKSWITCH = 0x08000000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_ONLYFIRELOCKED = 0x10000000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_FORCESCOPED = 0x20000000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_LASERBEAM = 0x40000000u;
inline constexpr uint32_t DEF_WEAPON_FLAG_POWERTHROW = 0x80000000u;

/* DefWeaponDef.flags2 bits — dword 2 of the same witnessed table @0x830bf0. */
inline constexpr uint32_t DEF_WEAPON_FLAG2_NOSELECT = 0x00000001u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_PARACHUTE = 0x00000002u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_THERMAL = 0x00000004u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_MONITOR = 0x00000008u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_VIEWLOCK = 0x00000010u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_ONLYLOCKSCOPED = 0x00000020u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_NOAMMOTYPES = 0x00000040u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_SHOWHUDPIP = 0x00000080u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_FIXVERTICALOFST = 0x00000100u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_INSET = 0x00000200u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_NOAUTOZERO = 0x00000400u;
inline constexpr uint32_t DEF_WEAPON_FLAG2_INVISIBLE = 0x00000800u;

typedef struct DefWeaponDef {
    char weapon_name[64];
    int category;
    int rank;
    int clipsize;
    int startrounds;
    /* Emplaced turret articulation limits, degrees ('targetyawrange' /
       'targetpitchmax' / 'targetpitchmin'). Azimuth is symmetric
       +-targetyawrange; elevation spans [-targetpitchmin, +targetpitchmax].
       0 = key absent. [orig: consumed through the itemDef turret-limit
       fallback Entity_GetWeaponTurretLimits @0x540e35..0x540e58 (+324
       azimuth, +316/+320 elevation); clamp @0x441228..0x44128c] */
    int targetyawrange;
    int targetpitchmax;
    int targetpitchmin;
    /* Loadout/armory keys (docs/net/novaworld-net-re.md §5.57); absent key = 0/empty.
       charfilter/teamfilter hold the raw file tokens; the packed masks the original
       producer builds land in charfilter_mask/teamfilter_mask below. */
    int statid;
    int maxclips;
    int ammobucket;
    /* 'sameas <weapon>': the weapon a pickup of this one refills when the
       picker already holds that one (empty = key absent; no shipped weapon.def
       authors it). [orig: WeaponDefs_ParseLineCallback @0x544056..0x544072,
       strncpy into AdmDef+0x34 capped at 0x20; consumers
       WeaponSlot_InitFromAvatarDef @0x542779 and WeaponSlot_RecalculateScore
       @0x5424CA] */
    char sameas[33];
    char ammo_class[64];
    int ammo_class_count;
    char charfilter[8][16];
    size_t charfilter_count;
    char teamfilter[4][16];
    size_t teamfilter_count;
    int loadout_selectable;
    int loadout_subclasses;
    char weapon_class[32];
    char round_type[64];
    char animadm[128];
    /* Weapon-level sound-set names, resolved by SoundSet lookup at presentation.
       [orig: WeaponDefs_ParseLineCallback @0x5444c8..0x544578] */
    char soundfireloop[128];
    char soundtrailoff[128];
    char soundhead[128];
    char vmacrotoken[17]; // weapon +0x2D8, contextual radio key
    char soundlockedtone[128];
    char launch_user_point[64];
    char gfx1[128];
    char gfx1a[128];
    char gfx1b[128];
    char gfx3[128];
    char crosshair[128];
	// [orig: crosshair second texture @0x544993; commandersX @0x5449FB]
	char crosshair_secondary[128];
	int splash; /* Designation radius, engine units [orig: AdmDef+0x454 @0x4DEBBD]. */
	char commanders_x[128];
	char hud_loadout_select[128];
    char hudicon[128];
    char hudclipgfx_texture[128];
    int hudclipgfx_offset[2];
    char hudrndgfx_texture[128];
    int hudrndgfx_offset[2];
    int hudrndgfx_layout[3];
    DefWeaponAction *actions;
    size_t actions_count;
    int flags;
    float error[6];
    // The first three authored pose columns are view-position units. Rotation
    // columns retain the parser's Q16 degrees before rounded BAM promotion.
    // A short line (fewer than six values) stores nothing: the original warns
    // "too few params" and returns before the first store.
    // [orig: WeaponDefs_ParseLineCallback @0x543680: pos gate @0x5445EE, pos
    //  @0x544614..0x5446D8; tpos gate @0x544735, tpos @0x54475B..0x544825;
    //  Math_ParseFixedPoint16 @0x6131F0]
    float pos[3];
    int32_t pos_rotation_deg_q16[3];
    float tpos[3];
    int32_t tpos_rotation_deg_q16[3];
    DefSightEntry *sights;
    size_t sights_count;
    char (*raw_lines)[512];
    size_t raw_lines_count;
    /* PLAYER_INFO loadout fields. [orig: WeaponDef_ParseProperty @ 0x54d730;
       consumer PlayerInfo_PopulateWeaponSlotLists @ 0x560430]. Appended to keep the leading
       struct offsets (and native layouts) stable. loadout_selectable (+32: a row
       appears only when non-zero), loadout_subclasses (+36: sub-entry expansion
       count), and maxclips (+136) live above with the §5.57 loadout keys. */
    char loadout_menu_textid[64];  /* +40: GameText "WepDes" key -> display name */
    char loadout_menu_ttdesc[128]; /* +44: tooltip text id */
    char loadout_menu_icon[64];    /* +144: icon texture */
    int weapon_class_slot;         /* +108: slot 0=accessory 1=primary 2=secondary 3=grenade */
    int teamfilter_mask;           /* +112: mask blue/yellow=2, red/violet=1 */
    int charfilter_mask;           /* +116: mask medic1 sniper2 gunner4 rifleman8 engineer16 */
    float weaponweight;            /* +120 */
    float clipweight;              /* +140 */
    /* First-person render fov, HORIZONTAL degrees; the record default is 80.0 and
       no shipped JO weapon.def sets the key (REVX-era defs only comment it out).
       [orig: parser key 'renderfov' @ 0x54482a; default flt_7D1898 = 80.0 stored by
       AdmDef_InitEntryDefaults @ 0x53ff31; consumer @ 0x4dee71 (WeaponDef+0x148)]. */
    float renderfov;               /* +148 */
    /* ADS zoom magnification ('scope_max_mag'; the JOX AK-47 ships 2). The scoped
       camera FOV divides the 80-degree default by the clamped zoom
       [orig: Player_ToggleWeaponScope @ 0x4df401 -> 80.0 / Player_GetClampedWeaponElevation
       @ 0x4dc6b0; g_CameraFovTargetQ16 @ 0x26C6848]. 0 = key absent. */
    float scope_max_mag;
    /* Third-person body-channel kinds, plain integers, 0 = key absent (rifle).
       special_hold (record +0xA4, read @ 0x4b5dba): 1..8 selects the body hold-pose
       ladder 50-61 (1 knife family, 2 pistol — also selects reload2 66, 3 grenade,
       4 stinger/AT4/RPG, 5 designator, 6 P90, 7 MP7, 8 javelin; 5-8 +1 when scoped).
       attack_anim (record +0xA8, read @ 0x542bbc): 1 stamps body state 62 knife_attack
       on fire, 2 stamps 63 grenade_attack; 0 = rifle, NO body stamp on fire.
       [orig: WeaponDefs_ParseLineCallback keys 'special_hold' @ 0x543cb7 /
       'attack_anim' @ 0x543ce9; docs/world/world-wac-ai-re.md section 14.8]. */
    int special_hold;
    int attack_anim;
    /* The second FLAGS dword (NoSelect/Parachute/.../Inset/NoAutoZero/Invisible) —
       the token table's fourth column. Appended (layout stability); `flags`
       above stays the flags1 dword. [orig: the 16-B-stride token table @ 0x830bf0] */
    int flags2;
    /* Run-gait class (record +0xAC, plain atol, 0 = key absent). The player body's
       forward-walk promotion adds this to the pitch tier: tier 1 -> state 9 run_2,
       tier >= 2 -> state 10 run_3 (fallback 9), <= 0 stays walk. The pitch-tier term
       (entity+0x37C) has no writer in the retail image, so it contributes the
       constant 2 — JO data ships only run_anim 0/1, both landing on run_3.
       [orig: parser key 'run_anim' @ 0x543d15 -> +0xAC; promotion @ 0x4b729d-0x4b731b]. */
    int run_anim;
    /* The floating attach-label text key ('attachtextid attach_50cal', emplaced
       guns/turrets only). The original resolves it against the Gametext "Overlays"
       section AT PARSE and stores the char* at AdmDef+0x3A0; we keep the key and the
       HUD resolves at draw. Empty = key absent -> the STROVER_USEGUN default label.
       [orig: WeaponDefs_ParseLineCallback @ 0x544d6c -> GameText_GetString("overlays",
       key) -> +0x3A0; consumer HUD_DrawVehicleSeatAndArmoryLabels @ 0x5a3538]. */
    char attach_text_id[32];
    /* Per-char-class STARTROUNDS overrides ('classrounds <class> <n>', repeatable).
       Kept at the original's raw table indices: the class token resolves through the
       char-class value table (medic=1, sniper=2, gunner=3, rifleman=5, engineer=6;
       4 unused) and the value stores at AdmDef+0x60 + value*4 — so classrounds[1] is
       the medic override and index 0/4 never ship. 0 = absent (the entry memset).
       Consumer: the spawn pool seeder picks classrounds[value(playerClass)] else
       startrounds. [orig: handler @ 0x543ab0 -> +0x60+value*4; class table
       @ 0x830EE8; consumer WeaponSlots_SeedAmmoPoolsFromDefs @ 0x5416c4]. */
    int classrounds[7];
    /* 'switchcategory <N>': after the RECOIL action completes, auto-switch to weapon
       category N rank 0 (grenade/LAW switchback). Two fields exactly as stored:
       the flag dword and the category. [orig: handler @ 0x5445a8 -> +0x168 flag = 1,
       +0x164 category; consumer WeaponAction_Recoil tail @ 0x543062 ->
       Player_SwitchToWeaponByHandle(category*65)]. */
    int switchcategory;
    int has_switchcategory;
    /* The weapon heat model (emplaced/vehicle heavy guns). Both keys store 16.16
       fixed point exactly as the original computes it, because the runtime divides
       them against each other as integers and a re-quantized float would shift the
       per-shot tick count.

       'heat_values <pctPerShot>,<pctPerSecondCooldown>' — each value goes through
       the engine's own digit parser (NOT atof) and is then integer-divided:
       heat_per_shot = parse16_16(v0) / 100 (percent -> a 0..0x10000 fraction),
       heat_decay_per_tick = parse16_16(v1) / 6200 (percent-per-second -> per-tick
       at the 62 Hz logic rate). 0 in heat_per_shot disables the whole model.
       'heat_effect <fxName>,<threshold>,...' — the overheat glow effect name and
       the 16.16 heat level it starts at (stored raw, no divide). Shipped JO data
       writes 'heat_effect heat, .5, 30, 60': the parser consumes only the first
       two values, so the trailing pair is authored-but-unread in retail too.
       'heat_sound' (+0x368) is a third key in the original parser that no shipped
       weapon.def authors; deliberately unparsed here.
       [orig: WeaponDefs_ParseLineCallback keys 'heat_effect' @ 0x543e36 -> +0x358
        name / +0x374 threshold, 'heat_sound' @ 0x543e85 -> +0x368, 'heat_values'
        @ 0x543eb7 -> +0x36C / +0x370; Math_ParseFixedPoint16 @ 0x6131f0;
        consumers WeaponSlot_CalcAccumulatedHeat @ 0x53f780,
        WeaponAction_Recoil @ 0x542f8b, WeaponAction_ProcessFrame @ 0x541031] */
    int heat_per_shot;         /* +0x36C, 16.16; 0 = no heat model */
    int heat_decay_per_tick;   /* +0x370, 16.16 per logic tick */
    int heat_glow_threshold;   /* +0x374, 16.16; heat above this spawns the glow */
    char heat_effect[64];      /* +0x358, the glow effect name ("heat") */
    /* Exact spread and weapon-weight carriers. Appended so every preceding C ABI
       field keeps its offset; the float mirrors above remain available to existing
       consumers while parity-sensitive simulation uses the original integers.
       ERROR stores six consecutive 16.16 values at AdmDef+0xB0..+0xC4, followed by
       the two optional theta values at +0xCC/+0xD0. [orig:
       WeaponDefs_ParseLineCallback ERROR @ 0x543B21, error_hipTheta @ 0x543BC0,
       error_upTheta @ 0x543BF2; Math_ParseFixedPoint16 @ 0x6131F0] */
    int error_fp16[6];
    int error_hip_theta_fp16;
    int error_up_theta_fp16;
    /* These authored weights are also parsed as 16.16 before the infantry-body
       instability accumulator consumes their sum. [orig:
       WeaponDefs_ParseLineCallback clipweight store @ 0x5440DB,
       weaponweight store @ 0x54410D] */
    int weaponweight_fp16;
    int clipweight_fp16;
    /* Scoped aim drift multipliers, prone/crouch/stand, in 16.16. All three
       default to one even when weapon.def omits the key. [orig:
       AdmDef_InitEntryDefaults @ 0x53FF61; WeaponDefs_ParseLineCallback
       stability @ 0x544118, stores +0x158/+0x15C/+0x160] */
    int stability_fp16[3];
    /* 'scope_max_zero <maxSteps> <stepMetres> <defaultMetres> [<extra>]': the
       scope-zero table the SIGHTS card's `slide` rows and Weapon_GetScopeZoomLevel
       read. Three atol'd ints in order -> AdmDef+0x84 (the zero-step cap), +0x9C
       (metres per zero step), +0xA0 (the default zero distance, metres); a fourth
       value, stored only when the line carries four (`cmp dword ptr [esi],4; jle` —
       the count includes the key), -> +0x88 (0 or 1 in every shipped JOX row; its
       consumer is not traced). 0 = key absent (the entry memset). Shipped forms:
       the M16/M203 `10 50 0 0` (the one def with a `slide` row), `10 100 200 0/1`,
       `1 100 100 1`, `1 300 300`, `10 100 300 1`.
       [orig: WeaponDefs_ParseLineCallback @ 0x544e8b..0x544efd — stores @ 0x544eac /
        @ 0x544ec1 / @ 0x544ed9, the count gate @ 0x544edf, the fourth store
        @ 0x544efd; consumers HUD_DrawWeaponSightOverlays @ 0x4dcf57..0x4dcff7
        (runtime/hud/sight_overlay.h sight_slide_multiplier),
        Weapon_GetScopeZoomLevel @ 0x422ff3] */
    int scope_max_zero_steps;  /* +0x84 */
    int scope_zero_step;       /* +0x9C */
    int scope_zero_default;    /* +0xA0 */
    int scope_zero_extra;      /* +0x88, the optional fourth value */
    /* 'scope_paralax_distance <metres>': the sight's parallax height, atof *
       65535.0 (dbl_7D0958 -- 65535, not 65536) then ftol -> +0x8C; 0 = key absent
       (the entry memset). The zero-yaw term atan2(+0x8C, zero distance) reads it
       at the slot install and the zero adjust (runtime/world/weapon_scope_zero.h).
       Shipped JOX rows: the M1 turret `.814`, the T80 turret `-.574`.
       [orig: WeaponDefs_ParseLineCallback @ 0x544e4e..0x544e80 — the key compare
        @ 0x544e4e, atof @ 0x544e64, `fmul dbl_7D0958` @ 0x544e69, ftol @ 0x544e72,
        the store @ 0x544e80] */
    int scope_paralax_distance_fp16; /* +0x8C */
    /* 'scope_max_mag <max> [<initial>]' second value and 'scope_min_mag <min>': the
       scope ZOOM range the +/-2 zoom step walks. Both atol'd ints like the first
       value (+0x90, kept in the float `scope_max_mag` above). +0x94 is the slot's
       INITIAL zoom: WeaponSlot_InitFromDef seeds MountSlot+0xC from it and clamps
       it into [floor, max] (floor = scope_min_mag, or the max under the class-6
       sniper lock), so an absent second value (0) starts every scope at its floor.
       +0x98 is the zoom floor, record default 2. Shipped JOX rows: WPN_EMP50BD
       `8 8` (the one row that also authors `scope_min_mag 2`), WPN_M1TURRET /
       WPN_T80TURRET `10 2`; every other row carries the max alone.
       [orig: WeaponDefs_ParseLineCallback 'scope_max_mag' @ 0x544f08 -> +0x90
        @ 0x544f29 / +0x94 @ 0x544f44, 'scope_min_mag' @ 0x544f4f -> +0x98
        @ 0x544f7a; AdmDef_InitEntryDefaults def[38] = 2 @ 0x53ff73; consumers
        WeaponSlot_InitFromDef @ 0x53ef2d..0x53ef44, Player_AdjustWeaponElevation
        @ 0x4dbe29..0x4dbe57, Player_MountWeaponSlot @ 0x4dfad3..0x4dfb16] */
    int scope_max_mag_arg2;    /* +0x94, the slot's initial zoom; 0 = absent */
    int scope_min_mag;         /* +0x98, the zoom floor; default 2 */
    /* Mounted HUD stance selector; zero uses the carrier/default icon.
       [orig: emplacedstance @0x544174..0x54419B, HUD @0x4B8539..0x4B8549] */
    int emplacedstance;
    /* Where the entry stands in the parsed text: the 0-based index of its
       `weapon` line and of the `end` that closed it (an entry the text never
       closes is not parsed). See DefWeaponAction::open_line for the line
       numbering. */
    size_t open_line;
    size_t end_line;
} DefWeaponDef;

/* One `ammoclass_max_carry <class> <n>` row: the class token and the carry cap,
   the absolute value of atol of the next token (both "" / 0 when the line
   lacks them). A table row wherever it stands in the file.
   [orig: WeaponDefs_ParseLineCallback @0x543680 — the key @0x5437F2, the class
   lookup of tokens[2] @0x5437FE -> sub_540590 @0x540590 (registered when new
   @0x543811..0x54385B), the cap abs(atol(tokens[3])) @0x543862..0x543873 into
   dword_24E7DE0] */
typedef struct DefAmmoClassCarry {
    char name[64];
    int cap;
} DefAmmoClassCarry;

typedef struct DefWeaponsFile {
    DefAmmoClassCarry *ammo_class_carries;
    size_t ammo_class_carries_count;
    DefWeaponDef *entries;
    size_t count;
} DefWeaponsFile;

/* ========================================================================= */
/* Item Definitions                                                          */
/* ========================================================================= */

/* items.def `type` token -> the engine's ItemDefType value stored at
   ItemDef+0x5C [orig: ItemDef_ParseProperty @ 0x49eb00; docs/world/itemdef-re.md
   D-ITEMDEF-1]. The witnessed values are non-sequential and NON-INJECTIVE:
   decoration/foliage share 2 and powerup/object share 6 (duplicate enumerator
   values are deliberate); 0 = unset (unknown token), 7 is unused. */
typedef enum DefItemType {
    DEF_ITEM_TYPE_UNSET      = 0,
    DEF_ITEM_TYPE_VEHICLE    = 1,
    DEF_ITEM_TYPE_DECORATION = 2,
    DEF_ITEM_TYPE_FOLIAGE    = 2,
    DEF_ITEM_TYPE_PERSON     = 3,
    DEF_ITEM_TYPE_MARKER     = 4,
    DEF_ITEM_TYPE_BUILDING   = 5,
    DEF_ITEM_TYPE_POWERUP    = 6,
    DEF_ITEM_TYPE_OBJECT     = 6,
    DEF_ITEM_TYPE_EFFECT     = 8
} DefItemType;

/* One anchored per-item particle-effect slot: the effect spawns at the named
   model userpoint. Slots that take an optional secondary effect (particlefxs,
   particlefxw1/w2) fill secondary_effect only when the line carries a third
   token; particlefx and particlefxw3/w4 never read one. The original copies
   each name unguarded into 32-char slots of the ItemDef+0x278 block; we
   truncate safely. [orig: ItemDef_ParseProperty @ 0x49eb00] */
// Slot-A runtime attach witness: Game_ResolveItemMaterialsAndSpawnBoneTrails
// [orig: @ 0x522ee0 -> Entity_SpawnBoneTrailEffect @ 0x43bef0].
typedef struct DefItemParticleFx {
    char effect[32];
    char userpoint[32];
    char secondary_effect[32];
} DefItemParticleFx;

/* Authored child-emplacement attachment keys. Keep the three spellings distinct:
   packed retail data uses bare addeweap for ordinary vehicle/turret attachments,
   addeweapG for the Apache/Ka-52 gun, and addeweapC for the M1A1/T80 OnTurret
   child. Their exact control/chaining policy is a runtime concern, not a parser
   normalization. The optional four angles are authored in down/up/right/left
   order. */
typedef enum DefItemEmplacementAttachmentKind {
    DEF_ITEM_EMPLACEMENT_ADDEWEAP = 0,
    DEF_ITEM_EMPLACEMENT_ADDEWEAP_G = 1,
    DEF_ITEM_EMPLACEMENT_ADDEWEAP_C = 2
} DefItemEmplacementAttachmentKind;

typedef struct DefItemEmplacementAttachment {
    char userpoint[16];
    int item_id;
    /* Retail-scaled BAM limits: down/right positive, up/left negative.
       One authored degree = 11930464. */
    int down_angle;
    int up_angle;
    int right_angle;
    int left_angle;
    int angle_count; /* 0 when limits are absent; packed retail records use 0 or 4 */
    int kind;        /* DefItemEmplacementAttachmentKind */
} DefItemEmplacementAttachment;

/* DefItemDef.attrib bits — items.def `attrib:` tokens (ItemDefAttrib, +0x54).
 * NOT witnessed (stays raw at use sites): 0x80000000.
 * [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md:147-155;
 * def_scan.cpp's item_attrib_table initializes from these] */
inline constexpr uint32_t DEF_ITEM_ATTRIB_MOVECB = 0x00000001u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_POWERUP = 0x00000002u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NOMOVESHOOT = 0x00000004u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NOTOOL = 0x00000008u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_SNAP = 0x00000010u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_EWEAP = 0x00000020u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_PLAYERCONTROL = 0x00000040u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_DOOR = 0x00000080u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NOTARGET = 0x00000100u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_LANDABLE = 0x00000200u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_MISSILE = 0x00000400u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_TIRE = 0x00000800u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_FASTROPE = 0x00001000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_TAKEABLE = 0x00002000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_EASY = 0x00004000u;
/* items.def token "S&D" (case-insensitive whole token; the tokenizer keeps the '&'):
   the S&D/A&D objective target, counted per team by the round census and immune to
   same-team blast damage. The token string is aSD @0x7C84E8, bytes 53 26 44 00 (the
   IDB typed it as the pointer off_7C84E8 until 2026-10-04). [orig:
   ItemDef_ParseProperty @0x4a084e..0x4a086d, token @0x7C84E8;
   census Server_ResetRoundCounters @0x516d3d/@0x516d89] */
inline constexpr uint32_t DEF_ITEM_ATTRIB_SD = 0x00008000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_4TEAM = 0x00010000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_CHANGETEAM = 0x00020000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_SPAWNPOINT = 0x00040000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_ARMORY = 0x00080000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_AIDATA = 0x00100000u;  /* the §5.6 AI-class flag — gates the 0x0D AI-trailer */
inline constexpr uint32_t DEF_ITEM_ATTRIB_LEAVECORPSE = 0x00400000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NODISMEMBER = 0x00800000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NOWEAPON = 0x01000000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_REFLECT = 0x02000000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NOSHADOW = 0x04000000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_CONCAVE = 0x08000000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NOSCAR = 0x10000000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NOHUD = 0x20000000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB_NODIE = 0x40000000u;

/* DefItemDef.attrib2 bits — ItemDefAttrib2 (+0x58). docs/world/itemdef-re.md:157-160. */
inline constexpr uint32_t DEF_ITEM_ATTRIB2_VEHICLEBAY = 0x00000001u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_AUTOINHERITTEAM = 0x00000002u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_VEHICLESPAWN = 0x00000004u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_DYNAMICSHADOW = 0x00000010u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_STATICSHADOW = 0x00000020u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_TUNNELPIECE = 0x00000040u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_USEVK = 0x00000080u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_STATICDEATH = 0x00000100u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_ONTURRET = 0x00000400u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_HASTURRET = 0x00000800u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_ISTURRET = 0x00001000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_FARP = 0x00002000u;
inline constexpr uint32_t DEF_ITEM_ATTRIB2_LANDMINE = 0x00004000u;

typedef struct DefItemDef {
    char display_name[128];
    int id;
    char sid[64];
    int type;  /* DefItemType — engine values at ItemDef+0x5C: 1=vehicle,
                  2=decoration/foliage, 3=person, 4=marker, 5=building,
                  6=powerup/object, 8=effect; 0=unset, 7 unused
                  [orig: ItemDef_ParseProperty @ 0x49eb00] */
    char graphic[128];
    char anim_def[128];
    char husk[128];
    int hp;              /* ItemDef+0x17C signed i16 healthMax, sign-extended in this ABI */
    char sound_profile[128];
    /* The AI profile name. Authoring it also RAISES the AIData attrib bit --
       retail ORs 0x100000 in the same parse arm, so an item with default_aip is
       an AI item whether or not it lists AIData [orig: ItemDef_ParseProperty
       @0x49eb00 -- the strcpy into itemDef+0x8B8 then `attrib |= 0x100000`]. */
    char default_aip[128];
    /* The female-variant profile name; tracks sound_profile until authored
       explicitly (both resolve to "default" when empty) [orig:
       "sound_profileFemale" @ 0x49fb76 -> def+0x26C; the runtime selects it
       via the character entity's female byte in Entity_GetProfileSlotSound
       @ 0x52831c]. */
    char sound_profile_female[128];
    char soundloops[7][128];
    char nightshot[128];
    char dawnshot[128];
    char duskshot[128];
    char dayshot[128];
    // Dawn/day/dusk/night base and random-range countdowns, in native ticks.
    // particletesttime aliases the dawn pair. [orig: ItemDef_ParseProperty @0x49EB00]
    int32_t shot_delay_ticks[4][2];
    int32_t destroy_timing_ticks[3]; // destroy_timing: initial delay, duration, section stagger
    /* §5.10b dispatch tags. ai_function on the player is `plyr` (witnessed
       on items.def id 105305 = wire 0x14B9, matching the orig SerializePlayerState
       callback), so ai_function is the field that drives ItemDef+356 lookup. */
    char ai_function[16];
    char move_function[16];
    char render_function[16];
    char disk_function[16];
    /* 'powerupdef <name>': the powerup.def row the item binds at mission start
       (world/powerup.h). Authoring it also RAISES the Powerup attrib bit: retail
       ORs 0x2 in the same parse arm, so the shipped med/ammo packs, whose attrib
       line is `notarget` alone, are Powerup items through this key.
       [orig: ItemDef_ParseProperty @0x49F698 -- the strcpy into itemDef+0x890
       @0x49F6C4..0x49F6D0, then `or [edx+54h],2` @0x49F6D2] */
    char powerup_def[32];
    /* 'input_function <class>': the input class row -- null / troop / tank. Its
       first callback is the key handler, its second the mounted first-person
       camera the carrier leg calls. [orig: ItemDef_ParseProperty @0x49F650 ->
       def+0x168; rows @0x829DA8, resolved by Entity_LookupPhysicsCallbacks
       @0x497910 into def+0x170 / def+0x174] */
    char input_function[16];
    /* 'virtualdisplay <model> <userpoint>': the cockpit model the `tank` render
       class draws INSTEAD of the hull for the local first-person driver, and the
       camera userpoint inside it. Stock data: `tankdrvr camera` (M1A1),
       `t80_drvr camera` (T80). [orig: ItemDef_ParseProperty @0x49F4E0 -- model ->
       def+0xD0 @0x49F506, userpoint -> def+0xE0 @0x49F521] */
    char virtual_display[16];
    char virtual_display_userpoint[16];
    unsigned int attrib;   /* ItemDefAttrib (+0x54) bitmask; attrib: tokens -> bits. AIData 0x100000 = AI class. [orig: ItemDef_ParseProperty; docs/world/itemdef-re.md] */
    unsigned int attrib2;  /* ItemDefAttrib2 (+0x58) bitmask. */
    /* Vehicle physics-property block, scaled AT PARSE exactly like the original loader
       [orig: ItemDef_ParsePhysicsProperty @0x49d870]. Scale constants: deg/s -> BAM/tick =
       192426 (2^32/360/62 tps), km/h -> 16.16 world-units/tick = 293 ((1000/3600)*65536/62),
       deg -> BAM = 11930464 (2^32/360), accel/decel raw*4. All 0 when the block is absent. */
    int physics;        /* +0x8DC raw selector; non-zero routes the entity to the vehicle
                           motor [orig: Entity_DispatchPhysics_cveh @0x48efc0] */
    int acceleration;   /* +0x8E0 = token*4 (16.16 u/tick per tick) [orig: @0x49da32] */
    int deceleration;   /* +0x8E4 = token*4; absent -> 2*acceleration [orig: @0x49da4b] */
    int player_speed;   /* +0x8E8 = km/h token * 293 [orig: @0x49d9a2] */
    int water_speed;    /* +0x8EC = km/h token * 293 [orig: @0x49d9e4] */
    int slip_speed;     /* +0x8F0 = token*4 [orig: @0x49dafd] */
    int max_slope;      /* +0x8F4 = deg token * 11930464 [orig: @0x49d91e] */
    int slip_slope;     /* +0x8F8 = deg token * 11930464 [orig: @0x49d960] */
    int climb_speed;    /* +0x920 = km/h token * 293, 16.16 u/tick vertical clamp
                           [orig: 293*atol store @0x49db4a] ["climb_speed"] */
    int turn_roll;      /* +0x90C raw token ("turnroll") — air roll-rate cap, *192426 at use */
    int speed_pitch;    /* +0x910 raw token ("speedpitch") — air pitch-rate cap, *192426 at use */
    int turn_rate;      /* +0x924 = deg/s token * 192426 [orig: @0x49d89a] */
    int turn_rate2;     /* +0x928 = deg/s token * 192426 [orig: @0x49d8dc] */
    int torque;         /* +0x91C raw ("torque") — the collision speed-decay shift count:
                           severity 1/3 decay speed >> (torque+2), severity 2 >> (torque+1)
                           [orig: parse @0x49dcca; consumers @0x47cc13-0x47ccc1] */
    /* Platform-solve tuning block (itemDef +0x908 / +0x92C..+0x948) — all raw
       atol, no parse scale, clamped in place by the consumers
       [orig: ItemDef_ParsePhysicsProperty stores: mass @0x49dc76,
        lean @0x49ddde, lean_velocity @0x49de1a, pitch @0x49de56,
        pitch_velocity @0x49de92, bob @0x49dece (the inline-string slot),
        flip @0x49df82; clamps @0x481ACC..0x481BA3]. Consumers: mass = weight
       class + collision momentum (+0x908); pitch/pitch_velocity/bob = the boat
       bow-lift / porpoise machine; lean/lean_velocity = the planing roll-lean
       machine @0x45AEA0; flip = the ground movers' tip threshold (*0.01). */
    int mass;
    /* Raw authored ints retail keeps verbatim [orig: ItemDef_ParsePhysicsProperty
       @0x49d870 -- `itemDefs[].weathervane = atol(v)` / `.minAI = atol(v)`].
       weathervane is the tail-alignment strength; minAI is the occupant count
       retail compares against Entity_CountMountedEntities. */
    int weathervane;
    int min_ai;
    int lean;
    int lean_velocity;
    int pitch;
    int pitch_velocity;
    int bob;
    int flip;
    /* The handbrake/tire-slip pair — raw atol, defaulted 1 / 5 by the allocator
       [orig: ItemDef_ParsePhysicsProperty keys "hand_brake" @0x7c7d60 -> +0x944,
        "tire_slip" @0x7c7d6c -> +0x940; ItemDef_AllocateWithDefaults @0x49E3B0].
       Consumers: hand_brake gates the vehicle motor's byte-973 stop latch
       (`occupant && Flags & 8 && handBrake` @0x48c03a); tire_slip is the skid
       model's slip threshold (D-NET-161 deferral). */
    int hand_brake;
    int tire_slip;
    /* The suspension spring block — raw atol like the rest of the physics block
       [orig: ItemDef_ParsePhysicsProperty stores: spring @0x49db5c (+0x8FC),
        spring_comp @0x49dbd4 (+0x900), shock @0x49dc10 (+0x904),
        top_heavy @0x49db98 (+0x918)]. Consumers: spring = the per-wheel spring
       constant k of Suspension_CompressWheelQuadratic @0x45CFB0; spring_comp =
       the travel PERCENTAGE (100 - spring_comp scales 0xFFFF) @0x47C51F..0x47C544;
       shock = the landing damp (11 - shock)/11, clamped [0,10] in place by
       Suspension_OscillateWheelFast @0x45D18F..0x45D1A2; top_heavy: DEAD in
       retail — the parser, the def allocator and the debug item editor are the
       only readers of +0x918; parsed for parity, carried by no consumer.
       vehicle-client-movers-re.md §7.3. */
    int spring;
    int spring_comp;
    int shock;
    int top_heavy;
    int critical_hp;    /* +0x180 i16 raw ("criticalhp") — the burn threshold the vehicle
                           health state machine reads [docs/world/itemdef-re.md +0x180] */
    int critical_drain; /* +0x182 i16 raw ("criticaldrain") — burn drain per 64 ticks */
    int non_critical_regen; /* +0x184 i16 raw ("noncriticalregen" @0x7c8620) — the
                               above-critical regen per 64 ticks the aircraft mover
                               applies on the authority [orig: @0x490410..0x490433] */
    int radar_sig;      /* +0x178 u16 raw ("radarsig") — copied to entity+422 as the AI
                           acquisition primary-FOV engage cap [orig: Entity_InitFromModel
                           @0x40e136; AI_FindBestTargetB cap read @0x467277] */
    int heat_sig;       /* +0x17A u16 raw ("heatsig") — entity+420, the secondary-FOV cap
                           [orig: @0x40e144; cap read @0x46723e] */
	char hud_image[128]; // [orig: ItemDef hud_image @0x4A0FB0, sprite +0x94C]
    int unit_type;      /* "unit_type" raw — the minimap icon class selector on vehicles
                           (5..8 helo, 3/4 boat, 12 special, else ground)
                           [orig: Entity_ClassifyForMinimap @0x50FA70 reads itemDef->unitType] */
    /* Per-item particle-effect keys; names copied verbatim. The original copies each
       token UNGUARDED into slots of irregular width (e.g. slot A's userpoint slot is
       22 B at +0x298..+0x2AE); our uniform 32-char fields truncate safely — shipped
       names are all well under either bound [orig: ItemDef_ParseProperty @ 0x49eb00]. */
    DefItemParticleFx particlefx;   /* 'particlefx <effect> <userpoint>' — effect +0x278,
                                       userpoint +0x298 [orig: @ 0x4a13ad] */
    DefItemParticleFx particlefxs;  /* 'particlefxs' + optional secondary — +0x2AE/+0x2EE,
                                       secondary +0x2CE [orig: @ 0x4a140b] */
    DefItemParticleFx particlefxw1; /* 'particlefxw1' + optional secondary — +0x304/+0x344,
                                       secondary +0x324 [orig: @ 0x4a148b] */
    DefItemParticleFx particlefxw2; /* 'particlefxw2' + optional secondary — +0x35A/+0x39A,
                                       secondary +0x37A [orig: @ 0x4a150b] */
    DefItemParticleFx particlefxw3; /* 'particlefxw3 <effect> <userpoint>', NO secondary —
                                       +0x3AE/+0x3CE [orig: @ 0x4a158b] */
    DefItemParticleFx particlefxw4; /* 'particlefxw4 <effect> <userpoint>', NO secondary —
                                       +0x3E2/+0x402 [orig: @ 0x4a15eb] */
    /* Effect-only keys; their spawn anchors are fixed husk-model userpoint names
       (Dead/Fire/Other) resolved at runtime, not parsed data. */
    char particledeath[32];    /* +0x416 [orig: @ 0x4a164b] */
    char particleh2odeath[32]; /* +0x44A [orig: @ 0x4a168e] */
    char particlefire[32];     /* +0x47E [orig: @ 0x4a16d0] */
    char particleother[32];    /* +0x4B2 [orig: @ 0x4a1713] */
    char particlefinale[32];   /* +0x4E4 [orig: @ 0x4a175b] */
    char particlespawn[32];    /* +0x506 [orig: @ 0x4a179d] */
    char (*raw_lines)[512];
    size_t raw_lines_count;
    /* Four organic fire ammo names; lndm also reads closeattack/marker3.
       [orig: ItemDef_ParseProperty @ 0x4A1823, def+0x56B..+0x5CB;
       Entity_InitOrganicAI @ 0x4BFCC0 -> entity+0x358..+0x35B] */
    char ammo_closeattack[32];
    char ammo_marker3[32];
    char ammo_easyrocket[32];     /* def+0x5AB -> organic entity+0x359 */
    char ammo_advancedrocket[32]; /* def+0x5CB -> organic entity+0x35A */
    /* The three organic launch-point names, resolved case-insensitively on
       the person's own model; rocket is shared by easy/advanced ammo.
       [orig: ItemDef_ParseProperty -> def+0x5EB/+0x5FB/+0x60B;
       Entity_InitOrganicAI @ 0x4BFE8F..0x4BFF82 -> entity+0x365..+0x367] */
    char launchups_closeattack[32];
    char launchups_rocket[32];  /* def+0x5FB -> organic entity+0x366 */
    char launchups_marker3[32]; /* def+0x60B -> organic entity+0x367 */
    /* items.def weapon userpoint NAMES — the twelve 16-byte slots at def+0x61B..0x6CB
       in parse order: weaplbup, weaplmup, weaplcup, weaprbup, weaprmup, weaprcup, then
       the `2` variants (weaplbup2 .. weaprcup2). Field b = the FIRE ORIGIN point, m =
       the muzzle-flash/action-effect anchor, c = the recoil/casing anchor; r -> weapon
       slot 0, l -> slot 1, r2 -> slot 2, l2 -> slot 3 (world-wac-ai-re §21.2).
       [orig: ItemDef_ParseProperty @ 0x4a0ff2..0x4a1301 -> def+0x61B..0x6CB; resolved
       on the entity model by Entity_InitBoneReferences @ 0x441470 -> entity+0x327] */
    char weapon_userpoints[12][16];
    /* items.def 'clipsize', plain atol — the respawn magazine reseed source (word
       entity+0x35C = itemDef+0x894). [orig: ItemDef_ParseProperty @ 0x49fa1c ->
       def+0x894; consumer Entity_ResetToSpawnState @ 0x4b97a9/0x4b97b5] */
    int clipsize;
    /* items.def 'deathtime' in TICKS, scaled at parse like the original loader:
       (62*seconds, an explicit 0 -> 496) + 62 grace. 0 = token absent (the def field's
       zero init). The corpse timer's seed at the infantry death edge.
       [orig: ItemDef_ParseProperty @ 0x49fa6c-0x49faa0 -> def+0x890; consumer
       Entity_UpdateInfantryAI @ 0x4b9c97 -> entity+0x148] */
    int deathtime_ticks;
    /* items.def 'primary_weapon' — the weapon.def entry an ewep emplacement mounts
       (the gun entity's slot-0 weapon; the attach label's text source). Appended
       (layout stability). [orig: ItemDef_ParseProperty -> def+0x54B primaryWeapon
       char[32] (docs/world/itemdef-re.md); consumers: the spawn weapon-slot build and
       HUD_DrawVehicleSeatAndArmoryLabels @ 0x5a351d via slot0->def+0x3A0] */
    char primary_weapon[32];
    /* --- The destruction/husk block (docs/world/world-wac-ai-re.md §24). Appended
       (layout stability). [orig: ItemDef_ParseProperty @ 0x49eb00] --- */
    char huskfinal[128];    /* 'huskfinal' -> def+0x80 huskFinal model name — the final
                               (burned-out) wreck stage; death pieces + the dead-wreck
                               effect banks prefer it over husk */
    char sounddeath[32];    /* 'sounddeath' -> def+0x6DB soundDeath name (resolved to
                               +0x860 deathSoundId; played by Entity_InitDeathSounds
                               @ 0x4939b0 unless the silent phase bit) */
    /* 'armor A [B]': +0x192 blastArmor = A, +0x190 impactArmor = A then overwritten
       by B when authored. -1 = invulnerable word 0xFFFF. Bullets zero their damage
       when ammo penetration_impact < impactArmor; blasts when ammo penetration_kz <
       blastArmor. [orig: parse @ 0x4a00e7-0x4a0147; gates @ 0x4e802a / @ 0x4e69b0] */
    int armor_impact;
    int armor_blast;
    float kz;               /* 'kz' -> +0x198 death-blast radius (units): the radius of
                               the kz_OrganicBlast queued at the husk's KZ user points /
                               entity pos when the item dies [orig: parse @ 0x49f0xx;
                               consumers Entity_QueueKzBlastAtUserPoints @ 0x4eabf0,
                               Entity_UpdateFallingDeathPhysics @ 0x4941d4] */
    /* 'husk_swap_at' / 'husk_swap_at_sec': +0x19C/+0x1A0 floats. _sec = seconds*62
       (an authored 0 -> 1.0 tick); husk_swap_at parses as percent*0.01 while +0x1A0
       is still 0, else as seconds*62 (the witnessed dual-unit parse). Runtime
       consumer unwitnessed — parsed for format fidelity (D-ITEM-2).
       [orig: parse @ 0x49f1ce-0x49f2c2; scales dbl 62.0 @ 0x7c88c0 / flt 0.01 @ 0x7c56a8] */
    float husk_swap_at;
    float husk_swap_at_sec;
    int scale_q16;          /* 'scale' -> +0x1B8 signed Q16.16 model scale. The
                               parser multiplies atof(value) by 65536 and truncates
                               toward zero under the temporary x87 control word.
                               Zero is the runtime's unscaled sentinel.
                               [orig: ItemDef_ParseProperty @ 0x49f6e0..0x49f73d;
                               Entity_InitFromModel @ 0x40dc30] */
    float debris_scale;     /* 'debris_scale' -> +0x1BC piece render scale (0 = unset;
                               pieces render at 1.0) [orig: piece[34] = def+0x1BC ?: 1.0
                               @ 0x4936f1] */
    int husk_sub_parts;     /* 'husk_sub_parts' -> +0x100 count byte */
    /* 'husk_sub_part_types NN_NAME ...' -> +0x101[slot] = debris-type table index.
       Each value splits at its FIRST '_': slot = number-1 (0..15 accepted), the
       remainder (internal underscores kept — CHUNK_M) matched case-insensitively
       against the 13-row engine debris-type table (world/destruction.h mirrors it:
       HULL 0, WHEEL 1, CHUNK_S/M/L 2-4, ROCK_S/M/L 5-7, CHUNKNP_S/M/L 8-10,
       CACTUS_ 11, CHUNKSF_M 12). Zero-init like the engine: an unauthored slot
       reads as HULL. [orig: parse @ 0x49f314-0x49f396 via DeathPieceType_FindByName
       @ 0x57b310 over the 80-B table @ 0x8404f0] */
    unsigned char husk_sub_part_types[16];
    /* items.def 'phrase_set', plain signed atol -> target itemDef+0x86C.
       Mounted gunner skeletal selection reads this dword.  Presence is explicit
       because authored zero is a witnessed configuration and zero-init otherwise
       means the key was absent. Appended for layout stability.
       [orig: ItemDef_ParseProperty @ 0x49F9DB..0x49FA0A; consumer
       Entity_BuildBoneTransformMatrices @ 0x4B1884] */
    int phrase_set;
    int phrase_set_valid;
    /* Projectile damage traits, appended for normalized-struct/layout stability.
       Retail storage: damage reduction +0x188/+0x18C, signed armor classes
       +0x190/+0x192. armor_impact is shared with the destruction block above;
       armor_kz is the projectile-facing normalized mirror of armor_blast. */
    float damage_reduc_pp;
    float damage_reduc_max;
    int armor_kz;     /* ItemDef+0x192 signed i16, sign-extended in this ABI */
    /* Ordered items.def addeweap/addeweapG/addeweapC records. Appended for
       normalized-struct/layout stability. */
    DefItemEmplacementAttachment *emplacement_attachments;
    size_t emplacement_attachments_count;
    /* 1-based stored-slot markers. A later G/C record overwrites its marker;
       zero means that variant was not stored. */
    int emplacement_g_slot;
    int emplacement_c_slot;
    /* Building-interior daylight transfer, appended for normalized-struct layout stability. `light_transfer` is parsed as atoi clamped 0..100, then x0.01
       into retail ItemDef+0x218. Ihq01 authors 20 -> 0.2.
       [orig: ItemDef_ParseProperty @0x4A19FD..0x4A1A50] */
    float light_transfer;
    int reverb; /* signed word, ItemDef+432 [orig: @0x4A015A] */
    /* items.def 'shadow <name> <w> <l> <ox> <oy>' — the authored ground-shadow
       blob decal. Appended for layout stability. Retail copies the name
       UNGUARDED into the 16-byte slot at ItemDef+0xA0 (huskshadow starts at
       +0xB0) and atofs the four floats to +0x11C/+0x120/+0x124/+0x128
       (width/length in world units, planar offset x/y); the resolved texture
       lands at +0x114. Consumed by the render-slot drape: a bound shadow slot
       without a silhouette RT draws this decal heading-rotated over its
       terrain patch (docs/render/render-lighting-re.md, the render-slot side).
       [orig: parse ItemDef_ParseProperty @ 0x49f3a5..0x49f44c; consumer
       RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0 — 1/w 1/l UV scale
       itemDef+0x11C/+0x120, UV center offset +0x124/+0x128 + 0.5] */
    char shadow_texture[16];
    float shadow_width;
    float shadow_length;
    float shadow_offset_x;
    float shadow_offset_y;
	/* 'pcvehicle_spawnlist' -> def+2772. Slots are shared across the file,
	   allocated in first-use order, capped at 32. [orig: @0x4A0253; @0x49DFC0] */
	uint32_t vehicle_spawn_mask;
    /* 'music' -> signed word +0x1B2, consumed as the containing building's
       WAC location ID. [orig: ItemDef_ParseProperty @0x49EB00] */
    int music_location;
    int mana; /* signed word +0x17E [orig: ItemDef_ParseProperty @0x49EB00] */
    /* 'score' -> signed word +0x194, the kill value: a victim whose word is 0
       (every Player definition) never enters the kill accounting.
       [orig: ItemDef_ParseProperty @0x4A0213..0x4A0242;
       Score_ProcessKillEvent @0x4FD422] */
    int score;
    /* Door fields appended for ABI stability. num_doors/first_door alias the
       low two bytes of deathtime_ticks (+0x890/+0x891) and first_subobject its
       third (+0x892); rotor_parts/aux_parts write raw bytes across both
       deathtime_ticks and clipsize (+0x890..+0x897); door_dir aliases
       clipsize. [orig: ItemDef_ParseProperty @0x49F748..0x49F980,
       @0x49F992..0x49F9CC, @0x49EF32..0x49F04C] */
    uint32_t door_type;
    int32_t door_open_rate_q16;
    int32_t door_max_angle_bam;
    char door_open_sound[25];
    char door_close_sound[25];
    /* items.def attrib token `Parent` -> ItemDef+0x548 (a byte, not an attrib bit):
       the gunner-attachment gate the vehicle class inits test before
       Entity_SetupGunnerAttachments (VehicleTraits::attrib_parent). Appended
       (layout stability). [orig: ItemDef_ParseProperty @0x4a0cd6..0x4a0ce2] */
    unsigned char attrib_parent;
} DefItemDef;

typedef struct DefItemsFile {
    DefItemDef *entries;
    size_t count;
	int vehicle_spawn_ids[32];
	int vehicle_spawn_id_count;
} DefItemsFile;

/* ========================================================================= */
/* HudPos Definitions                                                        */
/* ========================================================================= */

typedef struct DefHudColor {
    int r, g, b, a;
} DefHudColor;

typedef struct DefHudStance {
    int id;
    int offset_x, offset_y;
    char texture[128];
    char name[64];
} DefHudStance;

typedef struct DefHudGraphic {
    char texture[128];
    int x, y;
} DefHudGraphic;

/* One VEHICLE_HUD ... VEHICLE_END block: the mounted-vehicle panel layout for
   one item, keyed by its items.def `sid`. Retail accumulates these into ONE
   0xDC-byte staging block and commits it at VEHICLE_END against the item table
   [orig: HUD_ParseHudposToken @0x59F370 - the token arms, the commit walk and
   the memset(block, 0, 0xDC) reset].

   Field widths are the staging block's own: a 16-byte sid and three 32-byte
   texture names, derived from the global offsets (sid @+0x04, icon @+0x7C,
   interface @+0x9C, statictexture @+0xBC, block end @+0xDC).

   `emplace_count` / `seat_count` are OURS: retail caps the pair loops at 4 and
   8 but does not store the authored count in the block -- its drawer derives
   seat count from the entity. A consumer of the parsed block has no entity to
   ask, so the authored count is retained here. Declared, not witnessed. */
inline constexpr int DEF_VEHICLE_HUD_MAX_EMPLACE = 4;
inline constexpr int DEF_VEHICLE_HUD_MAX_SEATS = 8;
typedef struct DefVehicleHudBlock {
    char sid[16];              /* +0x04 [orig: the "sid" arm] */
    char icon[32];             /* +0x7C */
    char interface_texture[32];/* +0x9C */
    char static_texture[32];   /* +0xBC */
    int driver_x, driver_y;    /* +0x14 / +0x18 */
    int emplace_count;         /* authored pairs, capped at 4 by retail */
    int emplace_x[DEF_VEHICLE_HUD_MAX_EMPLACE];  /* +0x1C */
    int emplace_y[DEF_VEHICLE_HUD_MAX_EMPLACE];  /* +0x2C */
    int seat_count;            /* authored pairs, capped at 8 by retail */
    int seat_x[DEF_VEHICLE_HUD_MAX_SEATS];       /* +0x3C */
    int seat_y[DEF_VEHICLE_HUD_MAX_SEATS];       /* +0x5C */
} DefVehicleHudBlock;

typedef struct DefDeclutterEntry {
    char name[64];
    int flags[4];
} DefDeclutterEntry;

typedef struct DefHudPosDef {
    char font_hi[128];
    char font_lo[128];

    int mrclippy_normal[4];
    int mrclippy_alternate[4];
    int health[4];
    int heat[4];
    int powerbar[4];
    int starttimer[4];

    DefHudColor health_border;
    DefHudColor heat_border;
    DefHudColor hud_textcolor;
    DefHudColor weapon_textcolor;
    DefHudColor tagcolor_blueteam;
    DefHudColor tagcolor_redteam;
    DefHudColor tagcolor_good;
    DefHudColor tagcolor_middle;
    DefHudColor tagcolor_bad;
    DefHudColor stanceicon_color;
    DefHudColor stancecolor_good;
    DefHudColor stancecolor_middle;
    DefHudColor stancecolor_bad;
    DefHudColor dest_agl_color;
    DefHudColor agl_color;

    int spinmap_x1, spinmap_x2;
    int spinmap_y1, spinmap_y2;
    /* The spinmap waypoint-distance-label SUPPRESSOR. 0 when unauthored —
       the retail global is BSS-zero, so the label draws by default; an
       authored NONZERO value suppresses it. [orig: HUD_ParseHudposToken
       @0x59F370 -> g_SpinmapWpDistLabelOff @0x27237C0 (.data, no file
       bytes); sole read @0x5a7a6a] */
    int spinmap_wp_dist_off;

    /* Positioned text tokens carry FOUR fields in the original's global layout:
       x, y, hidden (0 = draw; the element draws only when this is 0), then the
       alignment word (left=0/right=1/center=2). [orig: AMMOCOUNTPOS parse
       @0x59fc3d writes x/y/hidden/align to 0x27235FC/600/604/608; the draw gates
       on the hidden dword, HUD_DrawWeaponAmmoAndName @0x5939d0] */
    int flag_carrier[4];
    int game_info[4];
    int wpd_info[4];
    /* ZONEINFO is a THREE-field form like BREATHTIME: x, y, then the alignment
       word as the third token, no hidden dword (JOX authors `ZONEINFO
       1013,386,Right`). [orig: HUD_ParseHudposToken @0x5A0642..0x5A0676 ->
       g_HUDZoneInfoX @0x2723DA4 / dword_2723DA8 (atof) / dword_2723DAC
       (HUD_ParseTextAlignment on the third token)] */
    int zone_info[3];
    int exp_points[4];
    int connect_status[4];
    int team_xy[4];
    int player_count[4];
    int ammo_count_pos[4];
    int weapon_name_pos[4];
    int map_coords[4];
    int time_clock[4];
    /* BREATHTIME is the one positioned token with THREE fields: x, y, then
       the alignment word (left=0/right=1/center=2) as the third; there is no
       hidden dword (JO authors `BREATHTIME 512,70,center`). [orig:
       HUD_ParseHudposToken @0x59FB3B..0x59FB84 -> dword_2723810/14/18 via
       atof, atof, HUD_ParseTextAlignment] */
    int breath_time[3];

    /* HUDLS — the weapon slot bar's layout block. Every field keeps the
       retail global's width: the two texture names are the fixed buffers
       the handler strcpy's into (20 and 19 bytes, bounded here), the MOREAV
       offset pair is stored as two SIGNED bytes (the ftol'd value's low
       byte, read back with movsx @0x599e30/@0x599e42), and HUDLS_SLOT n x y
       lands at slot n-1 only for n in 1..10 (anything else is ignored).
       [orig: HUD_ParseHudposToken — HUDLS_SYSTEM @0x59FE53..0x59FE66 ->
       dword_2723700; HUDLS_BRACKET @0x59FE84..0x59FE9C -> byte_2723704;
       HUDLS_KEYOFST @0x59FEB9..0x59FEDF -> dword_2723718/1C; HUDLS_MOREAV
       @0x59FEFD..0x59FF39 -> byte_2723720 + byte_2723733/34; HUDLS_SLOT
       @0x59FF57..0x59FF9C -> dword_2723738[8*(n-1)] / dword_272373C, the
       `sub edi,1; cmp edi,9; ja` range test @0x59FF6A..0x59FF70] */
    int hudls_system;
    char hudls_bracket[20];
    int hudls_keyofst[2];
    char hudls_moreav[19];
    int8_t hudls_moreav_off[2];
    int hudls_slot[10][2];

    int title_x, title_y;
    int ping_x, ping_y;
    int ping_right;
    int orders[2];
    int spec_mode_label[2];
    int lfp_flags[2];
    int lfp_takeover_dlg[2];
    int cargo_pos[2];
    /* PAUSEDPOS x y — the SP pause text's anchor (STROVER7, right-aligned
       Impact38); unauthored the drawer falls back to (1000, 4).
       [orig: HUD_ParseHudposToken @0x59FC8D..0x59FCC8 -> dword_272360C /
       dword_2723610 (atof, ftol); the reader sub_59D650 @0x59D656..0x59D670] */
    int paused_pos[2];
    /* NETWORKINDICATOR x0 y0 x1 y1 x2 y2 — the three connection indicators'
       corners (quality, link error, NovaWorld). Unauthored, the CNetQuality
       reset's (4,4) (20,4) (52,4) stand; `network_indicator_present` tells an
       authored zero from none.
       [orig: HUD_ParseHudposToken @0x59F981..0x59FA0C -> g_NetQuality
       +0x40..+0x54 (atof, ftol); CNetQuality_Reset @0x4C58C0] */
    int network_indicator[6];
    int network_indicator_present;
    int roomtk_pos[2];
    int roomtk_txt_pos[2];
    int stance_pos[2];
    int veh_stance_pos[2];
    int gear_text[2];
    int wpn_icon[2];
    int clip_pos[2];
    int scope_range[2];
    int scope_zero[2];
    int scope_mag[2];
    int impact_dist_pos[2];
    int chat_text[2];
    int sys_text[2];

    int hud_chline;
    int agl_radius;
    int roc_len;
    /* ALPHAFADE raw file fields (base %, max %, seconds) kept as floats: the
       original reads each via atof and the fraction survives into the x2.55 /
       x2.55 / x62 converts before ftol [orig: alphafade parse @0x5a0882..0x5a08c2];
       consumers apply that conversion. */
    float alpha_fade[3];

    int agl_tlrx[2];
    int agl_ylen[2];

    DefHudStance *stances;
    size_t stances_count;

    DefDeclutterEntry *declutter;
    size_t declutter_count;

    DefHudGraphic *static_frames;
    size_t static_frames_count;
    DefHudGraphic parachute_icon;
    DefHudGraphic armor_icon;

    /* VEHICLE_HUD blocks, parsed IN PARALLEL with the raw_lines passthrough
       below so writer round-trip is untouched. */
    DefVehicleHudBlock *vehicle_huds;
    size_t vehicle_huds_count;

    char (*raw_lines)[512];
    size_t raw_lines_count;
} DefHudPosDef;

typedef struct DefHudPosFile {
    DefHudPosDef hud;
} DefHudPosFile;

/* ========================================================================= */
/* Powerup Definitions (powerup.def)                                          */
/* ========================================================================= */

/* One `action "pickup"` / `action "respawn"` block of a powerup row: the
   ActionDef keys the shared action-line parser accepts that the two powerup
   handlers read. Other ActionDef keys (dupsound, ctrlreg, ctrlreginc) are
   accepted and dropped. [orig: PowerUpDef_ParseProperty @0x442EE0 -- the
   `action` open @0x443056..0x4430F4, in-action lines forwarded to
   ActionDef_ParseScriptLine @0x4023C0 @0x443042, the closing `end`
   @0x442FD0..0x443005] */
typedef struct DefPowerupAction {
    int present;                 /* the block was authored */
    char function[128];          /* `function <name>` -> the handler (ActionDef+0) */
    char anim[128];              /* ActionDef+58 */
    char soundset[128];          /* ActionDef+8 */
    char soundsetend[128];       /* ActionDef+12 */
    char particle[128];          /* ActionDef+16 */
    char particleuserpoint[128]; /* ActionDef+186 */
    char texttoken[64];          /* ActionDef+20 */
    int delaystart;              /* ActionDef+36 (ticks; `auto` -> -1) */
    int delayend;                /* ActionDef+40 (`delay` aliases it) */
    int action_value;            /* ActionDef+52 */
} DefPowerupAction;

/* One `ammo <class> <count>` row; the class name resolves against the weapon
   table's ammo classes when the runtime table is built (retail resolves at
   parse time through the weapon-table class lookup, an unknown class logging
   "ammo class error") [orig: @0x443240..0x44327D]. */
typedef struct DefPowerupAmmo {
    char class_name[64];
    int count; /* -1 = fill the class, else the amount added */
} DefPowerupAmmo;

/* One `powerup "<name>"` block (the 576-byte retail row). Every scalar
   defaults to 0 (the parse block is zeroed after each `end`).
   [orig: PowerUpDef_ParseProperty @0x442EE0; the row copy
   PowerUpDef_RegisterNewEntry @0x442C00] */
typedef struct DefPowerupDef {
    char name[17];       /* strncpy(.., 16) into the 16-byte row head [orig: @0x442F3F] */
    int respawn_time;    /* row+0x28, seconds (x62 ticks at pickup) [orig: @0x443103] */
    int max_respawns;    /* row+0x23C [orig: @0x44312C] */
    int hp;              /* row+0x2C: -1 raises to max, >0 adds [orig: @0x443155] */
    int mana;            /* row+0x30: -1 refills ammo class 1, else adds [orig: @0x44317E] */
    char weapon[64];     /* `weapon <name>`; resolved at table build [orig: @0x4431A7] */
    int weapon_all;      /* `weapon all` -> row+0x34 = -1 [orig: @0x4431DA] */
    int allammo;         /* row+0x38 [orig: @0x443220] */
    DefPowerupAmmo *ammo;
    size_t ammo_count;
    DefPowerupAction pickup;  /* row+0x20 */
    DefPowerupAction respawn; /* row+0x24 */
    size_t open_line;
    size_t end_line;
} DefPowerupDef;

typedef struct DefPowerupFile {
    DefPowerupDef *entries;
    size_t count;
} DefPowerupFile;

/* ========================================================================= */
/* API                                                                       */
/* ========================================================================= */

int def_parse_ammo(const char *path, DefAmmoFile *out);
/* Parse ammo.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_ammo as usual. Returns 0 on success, -1 on bad input. */
int def_parse_ammo_memory(const uint8_t *data, size_t size, DefAmmoFile *out);
void def_free_ammo(DefAmmoFile *f);

int def_parse_weapons(const char *path, DefWeaponsFile *out);
/* Parse weapon.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_weapons as usual. Returns 0 on success, -1 on bad input. */
int def_parse_weapons_memory(const uint8_t *data, size_t size, DefWeaponsFile *out);
void def_free_weapons(DefWeaponsFile *f);

int def_parse_powerup(const char *path, DefPowerupFile *out);
/* Parse powerup.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by
   the call; free with def_free_powerup as usual. Returns 0 on success, -1 on bad input. */
int def_parse_powerup_memory(const uint8_t *data, size_t size, DefPowerupFile *out);
void def_free_powerup(DefPowerupFile *f);

int def_parse_items(const char *path, DefItemsFile *out);
/* Parse items.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_items as usual. Returns 0 on success, -1 on bad input. */
int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out);
void def_free_items(DefItemsFile *f);

/* The items.def `attrib:` keyword tables, read-only by index: the SAME tables the
   parser matches tokens against (def_scan.cpp item_attrib_table / item_attrib2_table),
   so a tool that names an ItemDefAttrib bit shares the parser's vocabulary rather
   than carrying a second list. Lowercase token + its bit; NULL / 0 out of range.
   [orig: ItemDef_ParseProperty @0x49eb00] */
int def_item_attrib_keyword_count(void);
const char *def_item_attrib_keyword(int index);
uint32_t def_item_attrib_keyword_bit(int index);
int def_item_attrib2_keyword_count(void);
const char *def_item_attrib2_keyword(int index);
uint32_t def_item_attrib2_keyword_bit(int index);
/* The DefItemType vocabulary by value ("vehicle", "decoration/foliage", ...,
   "unset"; "?" for a value the table never produces) — one home for the
   type names a tool prints. */
const char *def_item_type_name(int type);

int def_parse_hudpos(const char *path, DefHudPosFile *out);
/* Parse hudpos.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_hudpos as usual. Returns 0 on success, -1 on bad input. */
int def_parse_hudpos_memory(const uint8_t *data, size_t size, DefHudPosFile *out);
void def_free_hudpos(DefHudPosFile *f);

/* PLAYER_INFO loadout weight + encumbrance [orig: PlayerInfo_CalculateLoadoutWeight
   @ 0x55f1f0; PlayerInfo_UpdateWeightAndWeaponIcons @ 0x55f480]. */
typedef enum DefEncumbrance {
    DEF_ENCUMBRANCE_LIGHT = 0,
    DEF_ENCUMBRANCE_NORMAL = 1,
    DEF_ENCUMBRANCE_HEAVY = 2,
} DefEncumbrance;

/* Total loadout weight over a set of equipped weapons [orig: PlayerInfo_CalculateLoadoutWeight
   @ 0x55f1f0]. Per weapon: weaponweight (+120) plus its ammo weight —
   (ammo_count > 0 ? ammo_count : maxclips) * clipweight (maxclips +136, clipweight
   +140). `ammo_counts[i] <= 0` selects the weapon's default clip count (maxclips),
   matching the engine's `<=0 -> maxclips` branch. Returns the summed weight. */
double def_loadout_weight(const DefWeaponDef *weapons, const int *ammo_counts, size_t n);

/* One extra-ammo (category-3) weight term [orig: the armory grenade leg
   @ 0x5655c9..0x56561c; the PLAYER_INFO sub-weapon/grenade terms in the
   @ 0x55f1f0 family]: count * clipweight only — no weaponweight. count < 0
   (the untouched sentinel) takes the maxclips default; a chosen zero row
   weighs nothing. */
double def_extra_ammo_weight(const DefWeaponDef *w, int count);

/* Encumbrance class for a loadout weight [orig: @ 0x55f480]: >= 66.6 HEAVY,
   >= 33.3 NORMAL, else LIGHT (the witnessed thresholds, exact). */
DefEncumbrance def_encumbrance_class(double weight);

/* The sub-weapon behind a PLAYER_INFO parent slot [orig: the round-type walk
   over the parent's following table entries @ 0x55def0 / @ 0x55e8b0 /
   @ 0x55f1f0]: candidates are the parent's next loadout_subclasses entries in
   table order; an entry sharing the parent's round_type (ASCII
   case-insensitive) is an ammo expansion and is skipped; the FIRST differing
   entry is the sub-weapon (satchel -> detonator). Returns its absolute index,
   or -1 when no candidate differs or the table ends. */
int def_subclass_weapon_index(const DefWeaponDef *weapons, size_t n,
                                         size_t parent_index);

} // namespace opennova::def
