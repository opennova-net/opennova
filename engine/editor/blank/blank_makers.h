#pragma once
// The individual makers behind blank_factory.cpp's table (one TU per format family).
#include <cstdint>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>

namespace opennova::editor {

// rtxt (blank_strings.cpp)
bool make_blank_empty_strings(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_gametext(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_menutxt(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_mission_text(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
// An expansion's text table: [exp_info] EXP_NAME (the project's title) and EXP_DESC, empty.
bool make_blank_expansion_table(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
// An expansion's version text (blank_strings.cpp beside its table): the project's title, one CRLF line.
bool make_blank_expansion_version(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// bms, til and wac (blank_mission.cpp): a mission on the terrain and under the environment the request
// names, holding its header alone; a mission's tile placement with no tile placed; an empty script
bool make_blank_mission(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_tile_info(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_script(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// mnu (blank_menu.cpp)
bool make_blank_main_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
// The MAIN window's FONT element every blank screen authors (the stylesheet's variables), its CURSOR
// naming the game's pointer (blank_pointer_name), and an authored screen's bytes: parsed through the
// document model and written in its canonical form.
const char *blank_menu_font();
std::string blank_menu_cursor();
bool blank_menu_bytes(const std::string &xml, const BlankRequest &request, std::vector<uint8_t> &out,
                      Diagnostic &error);

// The menus a mission's start and play open by name (blank_mission_menus.cpp): each the screen the
// game opens and the controls through which the player leaves it.
bool make_blank_cmap_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_game_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_weapon_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_vehicle_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_stat_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_death_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_mp_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// mns (blank_style.cpp)
bool make_blank_menu_style(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_brand_style(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// fnt (blank_font.cpp)
bool make_blank_font(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// def (blank_defs.cpp)
bool make_blank_items_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_weapon_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_ammo_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_powerup_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_charattr_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
// SndProf.def: the one "default" profile every item binds, every slot silent
bool make_blank_sound_profiles(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// env (blank_environment.cpp): the environment writer's authoring template, named after the file
bool make_blank_environment(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
// The sound lane (blank_sound.cpp, DI-33): a sound bank of no wave and no set; a wave of one sample of
// silence; a dialog bank of no group; a music bank of no stream; the shell's and a mission's music script, idling
bool make_blank_sound_bank(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_wave(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_dialog_bank(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_music_bank(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_menu_music_script(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_game_music_script(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// 3di, bad and adm (blank_model.cpp, DI-33): a model of one triangle; a clip of one bone at rest; an animation
// map of its anim_reset row alone, naming its reset clip (blank_reset_clip_name)
bool make_blank_model(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_animation(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_animation_map(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// fx (blank_shader.cpp): _ffp.fx, the fixed-function effect the renderer opens by name; an object
// effect for the shader tag the request's `tag` names (blank_shader_tags)
bool make_blank_ffp_shader(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_shader(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// coo (blank_coo.cpp)
bool make_blank_coo(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// texture (blank_texture.cpp): the game's missing-texture checkerboard, in the name's format; the
// mission's fixed textures the same checkerboard at the size the game's own file has; the mouse
// pointer the blank menus name (kBlankPointerRole), a TGA
bool make_blank_texture(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_loading_screen(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_monogram(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_boxtile(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_pointer(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// The text families (blank_text.cpp, DI-33): a particle file of no effect; a credits roll of one line; an AI
// profile of no type; the HUD layout with nothing moved; an avatars table of nothing
bool make_blank_particles(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_credits(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_ai_profile(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_hud_layout(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_avatars(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
// The player preview's environment cube (blank_texture.cpp, DI-33): a DDS cube map of the checkerboard
bool make_blank_cube(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// Hand-authored text goes to disk CRLF: retail's text parsers fail silently on LF.
std::string blank_crlf(const std::string &text);
void blank_text_to_bytes(const std::string &text, std::vector<uint8_t> &out);

} // namespace opennova::editor
