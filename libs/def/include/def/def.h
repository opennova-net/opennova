/* DEF file parser — pure C API.
 * Flat structs suitable for FFI (ctypes, etc.).
 * Parses Novalogic .def files: weapon.def, items.def, ammo.def, hudpos.def.
 */

#ifndef DEF_H
#define DEF_H

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
#define DEF_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

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
#define DEF_AMMO_FLAG_IGNOREDMG 0x00000001u
#define DEF_AMMO_FLAG_IGNORE 0x00000002u
#define DEF_AMMO_FLAG_SHRAPNEL 0x00000004u
#define DEF_AMMO_FLAG_SILENCED 0x00000008u
#define DEF_AMMO_FLAG_WATER 0x00000010u
#define DEF_AMMO_FLAG_DETONATESATCHELS 0x00000020u
#define DEF_AMMO_FLAG_NOSMOKE 0x00000040u
#define DEF_AMMO_FLAG_NOCOLLIDE 0x00000080u
#define DEF_AMMO_FLAG_NOGRAVITY 0x00000100u
#define DEF_AMMO_FLAG_HASITEM 0x00000200u
#define DEF_AMMO_FLAG_INSTANTKILLZONE 0x00000400u
#define DEF_AMMO_FLAG_OWNERIMMUNE 0x00000800u
#define DEF_AMMO_FLAG_USEOWNMOVE 0x00002000u
#define DEF_AMMO_FLAG_NOAGE 0x00004000u
#define DEF_AMMO_FLAG_FORCETRACER 0x00008000u
#define DEF_AMMO_FLAG_SHOTGUN 0x00010000u
#define DEF_AMMO_FLAG_CLAYMORE 0x00020000u
#define DEF_AMMO_FLAG_NOOITEMS 0x00080000u
#define DEF_AMMO_FLAG_NOMITEMS 0x00100000u
#define DEF_AMMO_FLAG_NODITEMS 0x00200000u
#define DEF_AMMO_FLAG_PRIORITY 0x00800000u
#define DEF_AMMO_FLAG_CLIPWATER 0x01000000u
#define DEF_AMMO_FLAG_DESIGNATETARGET 0x02000000u
#define DEF_AMMO_FLAG_IGNORFOILAGE 0x04000000u
#define DEF_AMMO_FLAG_LAWR 0x08000000u
#define DEF_AMMO_FLAG_FGRENADE 0x10000000u
#define DEF_AMMO_FLAG_CLIPWATERFX 0x20000000u

/* Ammo kill-zone types, record word +44 — drives the RoundData_SpawnRound ammo-class
 * dispatch (1 Knife = the immediate raycast, 6 Bullets = the standard projectile).
 * [orig: 8-name table @0x8133E0 rounds_kz_*; §5.60] */
#define DEF_AMMO_KZ_NULL 0
#define DEF_AMMO_KZ_KNIFE 1
#define DEF_AMMO_KZ_STANDARD 2
#define DEF_AMMO_KZ_MEDIC 3
#define DEF_AMMO_KZ_RADIUSBLAST 4
#define DEF_AMMO_KZ_C4 5
#define DEF_AMMO_KZ_BULLETS 6
#define DEF_AMMO_KZ_SLASH 7

typedef struct DefAmmoDef {
    char name[64];
    int velocity;            /* +4, integer units/s [orig: 'velocity' branch @0x40a2d0] */
    int min_damage;          /* +188 */
    int max_damage;          /* +192 */
    int penetration_impact;  /* +196 — must reach the target itemDef+400 armor threshold */
    int penetration_kz;      /* +200 */
    int recoil[3];           /* bytes +227..229 */
    /* Authoritative round-sim fields (docs/net/novaworld-net-re.md §5.60). */
    unsigned int flags;      /* +0, DEF_AMMO_FLAG_* OR-mask */
    int max_age_ticks;       /* +8: 'max_age' seconds -> 62 Hz ticks [orig: sub_40A0F0 @0x40a0f0] */
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
    DefEffectTableEntry *effects_table;
    size_t effects_table_count;
    char (*raw_lines)[512];
    size_t raw_lines_count;
    /* Kill-zone blast geometry (appended; FFI mirror stability). The explosion
     * queue's blast radius is kz_maxradius (or the entry's float override); the
     * linear damage falloff starts at kz_minradius; kz_pieslice != 0 makes the
     * blast a cone around the entry direction. [orig: AmmoDef_ParseProperty
     * 'kz_minradius'/'kz_maxradius' -> +52/+56 fp16, 'kz_pieslice' -> +60
     * deg * 11930464 BAM; consumers Projectile_ProcessExplosionQueue @0x4ead80,
     * Entity_ApplyWeaponDamage @0x4e6931/@0x4e695a] */
    int kz_minradius_fp16;   /* +52 */
    int kz_maxradius_fp16;   /* +56 */
    int kz_pieslice_bam;     /* +60 */
} DefAmmoDef;

typedef struct DefAmmoFile {
    DefAmmoDef *entries;
    size_t count;
} DefAmmoFile;

/* ========================================================================= */
/* Weapon Definitions                                                        */
/* ========================================================================= */

typedef struct DefSightEntry {
    char texture[128];
    int x1, y1, x2, y2;
    int blend;        /* 0=Blend, 1=Add, 2=BlendAt */
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
    char (*raw_lines)[512];
    size_t raw_lines_count;
} DefWeaponAction;

/* DefWeaponDef.flags bits — the weapon.def `flags <name>` OR-mask (dword 1 of the
 * flag pair). Token spellings are witnessed as-is (showcomander, notdropable,
 * fixverticalofst are the original's).
 * [orig: the 16-B-stride {name, 0, flags1 bit, flags2 bit} table @0x830bf0;
 * def_scan.cpp's flag_table initializes from these] */
#define DEF_WEAPON_FLAG_SCOPED 0x00000001u
#define DEF_WEAPON_FLAG_SIGHTED 0x00000002u
#define DEF_WEAPON_FLAG_UNDERWATER 0x00000004u
#define DEF_WEAPON_FLAG_SHOWCOMANDER 0x00000008u
#define DEF_WEAPON_FLAG_NOCLIPSNODRAW 0x00000010u
#define DEF_WEAPON_FLAG_BURST 0x00000020u
#define DEF_WEAPON_FLAG_NOTDROPABLE 0x00000040u
#define DEF_WEAPON_FLAG_EMPLACED 0x00000080u
#define DEF_WEAPON_FLAG_AUTO 0x00000100u
#define DEF_WEAPON_FLAG_NORANGECHECK 0x00000200u
#define DEF_WEAPON_FLAG_SHOWRANGE 0x00000400u
#define DEF_WEAPON_FLAG_SHOWELEVATION 0x00000800u
#define DEF_WEAPON_FLAG_ARMOR 0x00001000u
#define DEF_WEAPON_FLAG_OKWHILEJUMPING 0x00002000u
#define DEF_WEAPON_FLAG_ONLYFIRESCOPED 0x00004000u
#define DEF_WEAPON_FLAG_LOLLYPOP 0x00008000u
#define DEF_WEAPON_FLAG_ABSORBPITCH 0x00010000u
#define DEF_WEAPON_FLAG_NOMOVE 0x00020000u
#define DEF_WEAPON_FLAG_FORCECROUCH 0x00040000u
#define DEF_WEAPON_FLAG_ONLYSCOPED 0x00080000u
#define DEF_WEAPON_FLAG_2DIMPACT 0x00100000u
#define DEF_WEAPON_FLAG_USEDESIGNATOR 0x00200000u
#define DEF_WEAPON_FLAG_USESPREADTWO 0x00400000u
#define DEF_WEAPON_FLAG_SHOWIMPACTDIST 0x00800000u
#define DEF_WEAPON_FLAG_WHILESWIMMING 0x01000000u
#define DEF_WEAPON_FLAG_NOCARDSWITCH 0x02000000u
#define DEF_WEAPON_FLAG_HANDGUNUP 0x04000000u
#define DEF_WEAPON_FLAG_QUICKSWITCH 0x08000000u
#define DEF_WEAPON_FLAG_ONLYFIRELOCKED 0x10000000u
#define DEF_WEAPON_FLAG_FORCESCOPED 0x20000000u
#define DEF_WEAPON_FLAG_LASERBEAM 0x40000000u
#define DEF_WEAPON_FLAG_POWERTHROW 0x80000000u

/* DefWeaponDef.flags2 bits — dword 2 of the same witnessed table @0x830bf0. */
#define DEF_WEAPON_FLAG2_NOSELECT 0x00000001u
#define DEF_WEAPON_FLAG2_PARACHUTE 0x00000002u
#define DEF_WEAPON_FLAG2_THERMAL 0x00000004u
#define DEF_WEAPON_FLAG2_MONITOR 0x00000008u
#define DEF_WEAPON_FLAG2_VIEWLOCK 0x00000010u
#define DEF_WEAPON_FLAG2_ONLYLOCKSCOPED 0x00000020u
#define DEF_WEAPON_FLAG2_NOAMMOTYPES 0x00000040u
#define DEF_WEAPON_FLAG2_SHOWHUDPIP 0x00000080u
#define DEF_WEAPON_FLAG2_FIXVERTICALOFST 0x00000100u
#define DEF_WEAPON_FLAG2_INSET 0x00000200u
#define DEF_WEAPON_FLAG2_NOAUTOZERO 0x00000400u
#define DEF_WEAPON_FLAG2_INVISIBLE 0x00000800u

typedef struct DefWeaponDef {
    char weapon_name[64];
    int category;
    int rank;
    int clipsize;
    int startrounds;
    /* Loadout/armory keys (docs/net/novaworld-net-re.md §5.57); absent key = 0/empty.
       charfilter/teamfilter hold the raw file tokens; the packed masks the original
       producer builds land in charfilter_mask/teamfilter_mask below. */
    int statid;
    int maxclips;
    int ammobucket;
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
    char launch_user_point[64];
    char gfx1[128];
    char gfx1a[128];
    char gfx1b[128];
    char gfx3[128];
    char crosshair[128];
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
    float pos[6];
    float tpos[6];
    DefSightEntry *sights;
    size_t sights_count;
    char (*raw_lines)[512];
    size_t raw_lines_count;
    /* PLAYER_INFO loadout fields. [orig: WeaponDef_ParseProperty @ 0x54d730;
       consumer populate_weapon_slot_lists @ 0x560430]. Appended to keep the leading
       struct offsets (and the FFI mirrors) stable. loadout_selectable (+32: a row
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
       @ 0x4dc6b0; g_cameraFovDeg @ 0x26C6848]. 0 = key absent. */
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
       the token table's fourth column. Appended (FFI mirror stability); `flags`
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
       key) -> +0x3A0; consumer draw_vehicle_seat_and_armory_labels @ 0x5a3538]. */
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
} DefWeaponDef;

typedef struct DefWeaponsFile {
    char (*ammo_class_lines)[512];
    size_t ammo_class_lines_count;
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
 * NOT witnessed (stay raw at use sites): 0x8000, 0x80000000.
 * [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md:147-155;
 * def_scan.cpp's item_attrib_table initializes from these] */
#define DEF_ITEM_ATTRIB_MOVECB 0x00000001u
#define DEF_ITEM_ATTRIB_POWERUP 0x00000002u
#define DEF_ITEM_ATTRIB_NOMOVESHOOT 0x00000004u
#define DEF_ITEM_ATTRIB_NOTOOL 0x00000008u
#define DEF_ITEM_ATTRIB_SNAP 0x00000010u
#define DEF_ITEM_ATTRIB_EWEAP 0x00000020u
#define DEF_ITEM_ATTRIB_PLAYERCONTROL 0x00000040u
#define DEF_ITEM_ATTRIB_DOOR 0x00000080u
#define DEF_ITEM_ATTRIB_NOTARGET 0x00000100u
#define DEF_ITEM_ATTRIB_LANDABLE 0x00000200u
#define DEF_ITEM_ATTRIB_MISSILE 0x00000400u
#define DEF_ITEM_ATTRIB_TIRE 0x00000800u
#define DEF_ITEM_ATTRIB_FASTROPE 0x00001000u
#define DEF_ITEM_ATTRIB_TAKEABLE 0x00002000u
#define DEF_ITEM_ATTRIB_EASY 0x00004000u
#define DEF_ITEM_ATTRIB_4TEAM 0x00010000u
#define DEF_ITEM_ATTRIB_CHANGETEAM 0x00020000u
#define DEF_ITEM_ATTRIB_SPAWNPOINT 0x00040000u
#define DEF_ITEM_ATTRIB_ARMORY 0x00080000u
#define DEF_ITEM_ATTRIB_AIDATA 0x00100000u /* the §5.6 AI-class flag — gates the 0x0D AI-trailer */
#define DEF_ITEM_ATTRIB_LEAVECORPSE 0x00400000u
#define DEF_ITEM_ATTRIB_NODISMEMBER 0x00800000u
#define DEF_ITEM_ATTRIB_NOWEAPON 0x01000000u
#define DEF_ITEM_ATTRIB_REFLECT 0x02000000u
#define DEF_ITEM_ATTRIB_NOSHADOW 0x04000000u
#define DEF_ITEM_ATTRIB_CONCAVE 0x08000000u
#define DEF_ITEM_ATTRIB_NOSCAR 0x10000000u
#define DEF_ITEM_ATTRIB_NOHUD 0x20000000u
#define DEF_ITEM_ATTRIB_NODIE 0x40000000u

/* DefItemDef.attrib2 bits — ItemDefAttrib2 (+0x58). docs/world/itemdef-re.md:157-160. */
#define DEF_ITEM_ATTRIB2_VEHICLEBAY 0x00000001u
#define DEF_ITEM_ATTRIB2_AUTOINHERITTEAM 0x00000002u
#define DEF_ITEM_ATTRIB2_VEHICLESPAWN 0x00000004u
#define DEF_ITEM_ATTRIB2_DYNAMICSHADOW 0x00000010u
#define DEF_ITEM_ATTRIB2_STATICSHADOW 0x00000020u
#define DEF_ITEM_ATTRIB2_TUNNELPIECE 0x00000040u
#define DEF_ITEM_ATTRIB2_USEVK 0x00000080u
#define DEF_ITEM_ATTRIB2_STATICDEATH 0x00000100u
#define DEF_ITEM_ATTRIB2_ONTURRET 0x00000400u
#define DEF_ITEM_ATTRIB2_HASTURRET 0x00000800u
#define DEF_ITEM_ATTRIB2_ISTURRET 0x00001000u
#define DEF_ITEM_ATTRIB2_FARP 0x00002000u
#define DEF_ITEM_ATTRIB2_LANDMINE 0x00004000u

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
    /* §5.10b dispatch tags. ai_function on the player is `plyr` (witnessed
       on items.def id 105305 = wire 0x14B9, matching the orig SerializePlayerState
       callback), so ai_function is the field that drives ItemDef+356 lookup. */
    char ai_function[16];
    char move_function[16];
    char render_function[16];
    char disk_function[16];
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
    int turn_rate;      /* +0x924 = deg/s token * 192426 [orig: @0x49d89a] */
    int turn_rate2;     /* +0x928 = deg/s token * 192426 [orig: @0x49d8dc] */
    int torque;         /* +0x91C raw ("torque") — the collision speed-decay shift count:
                           severity 1/3 decay speed >> (torque+2), severity 2 >> (torque+1)
                           [orig: parse @0x49dcca; consumers @0x47cc13-0x47ccc1] */
    int critical_hp;    /* +0x180 i16 raw ("criticalhp") — the burn threshold the vehicle
                           health state machine reads [docs/world/itemdef-re.md +0x180] */
    int critical_drain; /* +0x182 i16 raw ("criticaldrain") — burn drain per 64 ticks */
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
    /* The person-item anim-fire weapon family (world-wac-ai-re §17.4, D-AI-5).
       Appended (FFI mirror stability). Only the closeattack slot is surfaced: JO
       riflemen author all four ammo_* names to the same rifle round, and the AI
       port's single-ammo stand-in consumes one. 32 bytes = the witnessed def slot
       stride (+0x56B..+0x58B). [orig: ItemDef_ParseProperty 'ammo_closeattack'
       @ 0x4a1823 -> def+0x56B (marker3 +0x58B, easyrocket +0x5AB, advancedrocket
       +0x5CB, launchups_* +0x5EB/+0x5FB)] */
    char ammo_closeattack[32];
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
       (FFI mirror stability). [orig: ItemDef_ParseProperty -> def+0x54B primaryWeapon
       char[32] (docs/world/itemdef-re.md); consumers: the spawn weapon-slot build and
       draw_vehicle_seat_and_armory_labels @ 0x5a351d via slot0->def+0x3A0] */
    char primary_weapon[32];
    /* --- The destruction/husk block (docs/world/world-wac-ai-re.md §24). Appended
       (FFI mirror stability). [orig: ItemDef_ParseProperty @ 0x49eb00] --- */
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
       means the key was absent. Appended for FFI mirror stability.
       [orig: ItemDef_ParseProperty @ 0x49F9DB..0x49FA0A; consumer
       Entity_BuildBoneTransformMatrices @ 0x4B1884] */
    int phrase_set;
    int phrase_set_valid;
    /* Projectile damage traits, appended for normalized-struct/FFI stability.
       Retail storage: damage reduction +0x188/+0x18C, signed armor classes
       +0x190/+0x192. armor_impact is shared with the destruction block above;
       armor_kz is the projectile-facing normalized mirror of armor_blast. */
    float damage_reduc_pp;
    float damage_reduc_max;
    int armor_kz;     /* ItemDef+0x192 signed i16, sign-extended in this ABI */
    /* Ordered items.def addeweap/addeweapG/addeweapC records. Appended for
       normalized-struct/FFI stability. */
    DefItemEmplacementAttachment *emplacement_attachments;
    size_t emplacement_attachments_count;
    /* 1-based stored-slot markers. A later G/C record overwrites its marker;
       zero means that variant was not stored. */
    int emplacement_g_slot;
    int emplacement_c_slot;
    /* Building-interior daylight transfer, appended for normalized-struct/FFI
       stability. `light_transfer` is parsed as atoi clamped 0..100, then x0.01
       into retail ItemDef+0x218. Ihq01 authors 20 -> 0.2.
       [orig: ItemDef_ParseProperty @0x4A19FD..0x4A1A50] */
    float light_transfer;
} DefItemDef;

typedef struct DefItemsFile {
    DefItemDef *entries;
    size_t count;
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

    /* Positioned text tokens carry FOUR fields in the original's global layout:
       x, y, hidden (0 = draw; the element draws only when this is 0), then the
       alignment word (left=0/right=1/center=2). [orig: AMMOCOUNTPOS parse
       @0x59fc3d writes x/y/hidden/align to 0x27235FC/600/604/608; the draw gates
       on the hidden dword, hud_draw_weapon_ammo_and_name @0x5939d0] */
    int flag_carrier[4];
    int game_info[4];
    int wpd_info[4];
    int zone_info[4];
    int exp_points[4];
    int connect_status[4];
    int team_xy[4];
    int player_count[4];
    int ammo_count_pos[4];
    int weapon_name_pos[4];
    int map_coords[4];
    int time_clock[4];
    int breath_time[4];

    int title_x, title_y;
    int ping_x, ping_y;
    int ping_right;
    int orders[2];
    int spec_mode_label[2];
    int lfp_flags[2];
    int lfp_takeover_dlg[2];
    int cargo_pos[2];
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

    char (*raw_lines)[512];
    size_t raw_lines_count;
} DefHudPosDef;

typedef struct DefHudPosFile {
    DefHudPosDef hud;
} DefHudPosFile;

/* ========================================================================= */
/* Legacy Single-Entry API                                                   */
/* ========================================================================= */

typedef struct DefAction {
    char name[64];
    char anim[128];
} DefAction;

typedef struct DefLegacyWeaponDef {
    char weapon_name[64];
    char animadm[128];
    char launch_user_point[64];
    char gfx1[128];
    char gfx1a[128];
    char gfx1b[128];
    char gfx3[128];
    DefAction *actions;
    size_t actions_count;
} DefLegacyWeaponDef;

typedef struct DefGenericDef {
    char display_name[128];
    char type[64];
    char graphic[128];
    char husk[128];
    char anim_def[128];
} DefGenericDef;

enum {
    DEF_KIND_WEAPON = 0,
    DEF_KIND_GENERIC = 1
};

typedef struct DefFile {
    int kind;
    DefLegacyWeaponDef weapon;
    DefGenericDef generic;
} DefFile;

/* ========================================================================= */
/* API                                                                       */
/* ========================================================================= */

DEF_EXPORT int def_parse_ammo(const char *path, DefAmmoFile *out);
/* Parse ammo.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_ammo as usual. Returns 0 on success, -1 on bad input. */
DEF_EXPORT int def_parse_ammo_memory(const uint8_t *data, size_t size, DefAmmoFile *out);
DEF_EXPORT void def_free_ammo(DefAmmoFile *f);

DEF_EXPORT int def_parse_weapons(const char *path, DefWeaponsFile *out);
/* Parse weapon.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_weapons as usual. Returns 0 on success, -1 on bad input. */
DEF_EXPORT int def_parse_weapons_memory(const uint8_t *data, size_t size, DefWeaponsFile *out);
DEF_EXPORT void def_free_weapons(DefWeaponsFile *f);

DEF_EXPORT int def_parse_items(const char *path, DefItemsFile *out);
/* Parse items.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_items as usual. Returns 0 on success, -1 on bad input. */
DEF_EXPORT int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out);
DEF_EXPORT void def_free_items(DefItemsFile *f);

DEF_EXPORT int def_parse_hudpos(const char *path, DefHudPosFile *out);
/* Parse hudpos.def from an in-memory buffer (e.g. a PFF/VFS entry). `out` is zeroed by the
   call; free with def_free_hudpos as usual. Returns 0 on success, -1 on bad input. */
DEF_EXPORT int def_parse_hudpos_memory(const uint8_t *data, size_t size, DefHudPosFile *out);
DEF_EXPORT void def_free_hudpos(DefHudPosFile *f);

DEF_EXPORT int def_parse_def(const char *path, DefFile *out);
DEF_EXPORT void def_free_def(DefFile *f);

/* PLAYER_INFO loadout weight + encumbrance [orig: calculate_loadout_weight
   @ 0x55f1f0; update_player_info_weight_and_weapon_icons @ 0x55f480]. */
typedef enum DefEncumbrance {
    DEF_ENCUMBRANCE_LIGHT = 0,
    DEF_ENCUMBRANCE_NORMAL = 1,
    DEF_ENCUMBRANCE_HEAVY = 2,
} DefEncumbrance;

/* Total loadout weight over a set of equipped weapons [orig: calculate_loadout_weight
   @ 0x55f1f0]. Per weapon: weaponweight (+120) plus its ammo weight —
   (ammo_count > 0 ? ammo_count : maxclips) * clipweight (maxclips +136, clipweight
   +140). `ammo_counts[i] <= 0` selects the weapon's default clip count (maxclips),
   matching the engine's `<=0 -> maxclips` branch. Returns the summed weight. */
DEF_EXPORT double def_loadout_weight(const DefWeaponDef *weapons, const int *ammo_counts, size_t n);

/* Encumbrance class for a loadout weight [orig: @ 0x55f480]: >= 66.6 HEAVY,
   >= 33.3 NORMAL, else LIGHT (the witnessed thresholds, exact). */
DEF_EXPORT DefEncumbrance def_encumbrance_class(double weight);

#ifdef __cplusplus
}
#endif

#endif /* DEF_H */
