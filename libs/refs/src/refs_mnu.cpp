// .mnu menu -> textures, fonts, sound banks, target menus, string tables,
// string keys, datasources, stylesheet variables.
//
// Field model follows libs/mnu structs, with the runtime's own consumption
// rules: %VAR% values in ANY field resolve through the .mns stylesheet (the
// original expands them over the raw buffer before the XML parse), so they
// are never emitted as ASSET edges — they become "style_var" edges naming the
// stylesheet variable instead (no file semantics; the Menu Styles workspace's
// "Used by" reads them); a SOUND element's FILE names a .lwf bank (the
// trigger is a set INSIDE it, so the bank is the file-level edge); and string
// keys (STRING/ITEM/HEADER type="id") are emitted as "string_id" edges with no
// file semantics — per-key statusing needs the resolving table, which is the
// caller's context (the resolve() layer reports them "unprobed").
#include <string>
#include <vector>

#include "extractors.h"
#include "mnu/mnu.h"

namespace opennova::refs::detail {

namespace {

// %VAR% stylesheet indirection (e.g. %DEF_FONTNAME_LG%, %TRIM_COLOR%): resolves
// through menu_style.mns at runtime, not to an asset name. The original
// expands variables over the WHOLE raw buffer before the XML parse
// [orig: NapiXML_ExpandVariablesInText @ 0x63a000], so any field can carry
// one - an unexpanded %NAME% is never a literal asset reference, whatever the
// slot. It IS a reference to the stylesheet variable itself.
bool is_style_variable(const std::string& value) {
    return !value.empty() && value.front() == '%';
}

// "%TRIM_COLOR%" -> "TRIM_COLOR"; "" when the value is not a %VAR% token.
std::string style_variable_name(const std::string& value) {
    if (!is_style_variable(value)) {
        return std::string();
    }
    std::string name = value.substr(1);
    if (!name.empty() && name.back() == '%') {
        name.pop_back();
    }
    return name;
}

// Emits a "style_var" edge when value is a %VAR% token; no-op otherwise. The
// direct call sites are VALUE fields (colors, literal text) where a variable
// is the only kind of reference the field can carry.
void add_style_var(EdgeSink& sink, const std::string& value, const std::string& site) {
    sink.add(style_variable_name(value), "style_var", site);
}

// Every asset emission funnels through here so the %VAR% redirect is universal:
// a variable-valued field references the stylesheet variable, not an asset.
void add_ref(EdgeSink& sink, const std::string& value, const char* kind, const std::string& site) {
    if (is_style_variable(value)) {
        add_style_var(sink, value, site);
        return;
    }
    sink.add(value, kind, site);
}

// Attribute tokens are authored in mixed case; the runtime compares
// case-insensitively (CRT _wcsicmp), so the extractor does too.
bool token_is(const std::string& value, const char* want) {
    return lower_ascii(value) == want;
}

void add_appearances(EdgeSink& sink, const std::vector<mnu::Appearance>& appearances,
                     const std::string& site) {
    for (const mnu::Appearance& appearance : appearances) {
        if (token_is(appearance.type, "image")) {
            add_ref(sink, appearance.value, "texture", site);
        } else {
            // Non-image appearances carry color values; only a %VAR% is a reference.
            add_style_var(sink, appearance.value, site);
        }
    }
}

// Font color slots are values, not asset names; emit only %VAR% references.
void add_font_colors(EdgeSink& sink, const mnu::Font& font, const std::string& site) {
    add_style_var(sink, font.default_fg, site + ".default_fg");
    add_style_var(sink, font.default_bg, site + ".default_bg");
    add_style_var(sink, font.mouseover_fg, site + ".mouseover_fg");
    add_style_var(sink, font.mouseover_bg, site + ".mouseover_bg");
    add_style_var(sink, font.selected_fg, site + ".selected_fg");
    add_style_var(sink, font.selected_bg, site + ".selected_bg");
    add_style_var(sink, font.disabled_fg, site + ".disabled_fg");
    add_style_var(sink, font.disabled_bg, site + ".disabled_bg");
}

void add_sounds(EdgeSink& sink, const std::vector<mnu::Sound>& sounds, const std::string& site) {
    for (const mnu::Sound& sound : sounds) {
        // File-less SOUND nodes never play in the original (docs/mnu/menu-re.md
        // D-MNU-3); EdgeSink already drops empty names.
        add_ref(sink, sound.file, "sound", site + ".sound[" + sound.trigger + "]");
    }
}

void add_items(EdgeSink& sink, const mnu::Items& items, const std::string& site) {
    add_style_var(sink, items.selection_color, site + ".selection_color");
    for (const mnu::Item& item : items.items) {
        if (token_is(item.type, "image")) {
            add_ref(sink, item.text, "texture", site + ".item");
        } else if (token_is(item.type, "id")) {
            add_ref(sink, item.text, "string_id", site + ".item");
        } else {
            // Color values and literal item text: only a %VAR% is a reference.
            add_style_var(sink, item.text, site + ".item");
        }
    }
}

void walk_window(EdgeSink& sink, const mnu::Window& window, const std::string& parent_site) {
    const std::string site = parent_site + ".window[" + window.name + "]";

    add_appearances(sink, window.appearances, site);
    add_appearances(sink, window.shuttle, site + ".shuttle");
    add_appearances(sink, window.scrollup, site + ".scrollup");
    add_appearances(sink, window.scrolldown, site + ".scrolldown");
    add_appearances(sink, window.spinup.appearances, site + ".spinup");
    add_appearances(sink, window.spindown.appearances, site + ".spindown");
    add_sounds(sink, window.sounds, site);

    for (const mnu::Action& action : window.actions) {
        // A cross-file screen action names the target .mnu; the screen name
        // inside it stays site detail (entry-level granularity comes later).
        if (token_is(action.type, "screen") && !action.file.empty()) {
            add_ref(sink, action.file, "menu", site + ".action[" + action.target + "]");
        }
    }

    if (token_is(window.string_data.type, "id")) {
        add_ref(sink, window.string_data.value, "string_id", site + ".string");
    } else {
        // Literal text substitutes %VAR% through the stylesheet at build time
        // (ADR 0005); a whole-field token references the variable.
        add_style_var(sink, window.string_data.value, site + ".string");
    }
    add_ref(sink, window.font.name, "font", site + ".font");
    add_font_colors(sink, window.font, site + ".font");

    add_ref(sink, window.frame.stencil, "texture", site + ".frame.stencil");
    add_ref(sink, window.frame.brush, "texture", site + ".frame.brush");
    add_ref(sink, window.frame.monogram, "texture", site + ".frame.monogram");
    add_ref(sink, window.cursor.file, "texture", site + ".cursor");
    add_ref(sink, window.text_rsrc, "strings", site + ".text_rsrc");
    add_ref(sink, window.datasource, "datasource", site + ".datasource");

    add_items(sink, window.items, site);
    add_items(sink, window.list_box.items, site + ".list_box");
    add_appearances(sink, window.list_box.appearances, site + ".list_box");
    add_appearances(sink, window.list_box.scrollbar.track, site + ".list_box.scrollbar");
    add_appearances(sink, window.list_box.scrollbar.shuttle, site + ".list_box.scrollbar");
    add_appearances(sink, window.list_box.scrollbar.scrollup, site + ".list_box.scrollbar");
    add_appearances(sink, window.list_box.scrollbar.scrolldown, site + ".list_box.scrollbar");
    add_sounds(sink, window.list_box.scrollbar.sounds, site + ".list_box.scrollbar");

    add_style_var(sink, window.table_data.outline_color, site + ".table.outline_color");
    add_style_var(sink, window.table_data.selection_color, site + ".table.selection_color");
    for (const mnu::TableHeader& header : window.table_data.column.headers) {
        // type="id" headers resolve through the string table
        // [orig: CUIStringTable_LookupString, see mnu.h TableHeader].
        if (token_is(header.type, "id")) {
            add_ref(sink, header.text, "string_id", site + ".table.header");
        } else {
            add_style_var(sink, header.text, site + ".table.header");
        }
    }
    for (const mnu::TableSubst& subst : window.table_data.column.substitutions) {
        if (subst.is_file) {
            add_ref(sink, subst.file, "texture", site + ".table.subst");
        }
    }
    add_appearances(sink, window.table_data.scrollbar.track, site + ".table.scrollbar");
    add_appearances(sink, window.table_data.scrollbar.shuttle, site + ".table.scrollbar");
    add_appearances(sink, window.table_data.scrollbar.scrollup, site + ".table.scrollbar");
    add_appearances(sink, window.table_data.scrollbar.scrolldown, site + ".table.scrollbar");
    add_sounds(sink, window.table_data.scrollbar.sounds, site + ".table.scrollbar");

    for (const mnu::Window& child : window.children) {
        walk_window(sink, child, site);
    }
}

}  // namespace

bool extract_mnu(const std::string& source_path, const uint8_t* data, size_t size,
                 std::vector<Reference>& out, std::string& error) {
    mnu::Document doc;
    if (!mnu::parse(data, size, doc, error)) {
        return false;
    }
    EdgeSink sink(source_path, "menu", out);
    for (const mnu::Screen& screen : doc.screens) {
        const std::string site = "screen[" + screen.name + "]";
        // The parser lifts a root-window TEXT_RSRC onto the screen; reading
        // both is duplicate-safe (EdgeSink dedupes on kind+name).
        add_ref(sink, screen.text_rsrc, "strings", site + ".text_rsrc");
        add_ref(sink, screen.cursor_file, "texture", site + ".cursor");
        walk_window(sink, screen.root_window, site);
    }
    return true;
}

}  // namespace opennova::refs::detail
