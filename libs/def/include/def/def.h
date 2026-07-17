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
    int tracer_rate;         /* byte +226 ('tracerRate') */
    char notarmmed_ammo[64]; /* +241: the not-armed child ammo name ('notarmmedammo') */
    /* Host fire-presentation fields (world-wac-ai-re §17.4). The original resolves
     * both names at parse time (+64 = SoundBank_FindSetByNameAnyBank set ptr, +68 =
     * CEffectWorld_InternEffectHandle handle [orig: AmmoDef_ParseProperty
     * @0x40a8c8/@0x40a8f6]); we keep the names and resolve in the host at play. */
    char ai_launch[64];       /* +64: 'ai_launch' fire sound-set name */
    char ai_launcheffect[64]; /* +68: 'ai_launcheffect' muzzle effect name */
    int mf_light;             /* +36: 'MF_Light' presence flag [orig: @0x40a81b = 1] */
    int mf_light_value;       /* +40: 'MF_Light' value (atol) [orig: @0x40a837] */
    /* Tracer visual styles, 'tracer_type <friendly> [<enemy>]' — witnessed id map in
     * ammo_tracer_type_from_string; one value copies into both [orig: @0x40a78b-0x40a7fa]. */
    int tracer_type_friendly; /* +232 */
    int tracer_type_enemy;    /* +236 */
    DefEffectTableEntry *effects_table;
    size_t effects_table_count;
    char (*raw_lines)[512];
    size_t raw_lines_count;
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
    int hp;
    char sound_profile[128];
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
