/* DEF file parser — pure C API.
 * Flat structs suitable for FFI (ctypes, etc.).
 * Parses Novalogic .def files: weapon.def, items.def, ammo.def, hudpos.def.
 */

#ifndef DEF_H
#define DEF_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define DEF_EXPORT __declspec(dllexport)
#  else
#    define DEF_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define DEF_EXPORT __attribute__((visibility("default")))
#  else
#    define DEF_EXPORT
#  endif
#endif

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

typedef struct DefAmmoDef {
    char name[64];
    int velocity;
    int min_damage;
    int max_damage;
    int penetration_impact;
    int penetration_kz;
    int recoil[3];
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

typedef struct DefItemDef {
    char display_name[128];
    int id;
    char sid[64];
    int type;  /* 0=Unknown,1=Marker,2=Vehicle,3=Person,4=Building,5=Decoration,6=Foliage,7=Object,8=Powerup */
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
    char (*raw_lines)[512];
    size_t raw_lines_count;
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

    int flag_carrier[3];
    int game_info[3];
    int wpd_info[3];
    int zone_info[3];
    int exp_points[3];
    int connect_status[3];
    int team_xy[3];
    int player_count[3];
    int ammo_count_pos[3];
    int weapon_name_pos[3];
    int map_coords[3];
    int time_clock[3];
    int breath_time[3];

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
    int alpha_fade[3];

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
DEF_EXPORT void def_free_ammo(DefAmmoFile *f);

DEF_EXPORT int def_parse_weapons(const char *path, DefWeaponsFile *out);
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

#ifdef __cplusplus
}
#endif

#endif /* DEF_H */
