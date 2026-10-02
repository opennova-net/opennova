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

// bms and wac (blank_mission.cpp): a mission on the terrain and under the environment the request
// names, holding its header alone; an empty script
bool make_blank_mission(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_script(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// mnu (blank_menu.cpp)
bool make_blank_main_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_menu(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// mns (blank_style.cpp)
bool make_blank_menu_style(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_brand_style(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// fnt (blank_font.cpp)
bool make_blank_font(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// def (blank_defs.cpp)
bool make_blank_items_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_weapon_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_ammo_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);
bool make_blank_charattr_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// coo (blank_coo.cpp)
bool make_blank_coo(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// texture (blank_texture.cpp): the game's missing-texture checkerboard, in the name's format
bool make_blank_texture(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error);

// Hand-authored text goes to disk CRLF: retail's text parsers fail silently on LF.
std::string blank_crlf(const std::string &text);
void blank_text_to_bytes(const std::string &text, std::vector<uint8_t> &out);

} // namespace opennova::editor
