// .mnu menu -> textures, fonts, sound banks, target menus, string tables,
// string keys, datasources.
//
// Field model follows libs/mnu structs, with the runtime's own consumption
// rules: image appearances and font names resolve through the .mns stylesheet
// when written as %VAR% (never file names — skipped, mirroring the builder's
// has_images predicate), a SOUND element's FILE names a .lwf bank (the trigger
// is a set INSIDE it, so the bank is the file-level edge), and string keys
// (STRING/ITEM/HEADER type="id") are emitted as "string_id" edges with no file
// semantics — per-key statusing needs the resolving table, which is the
// caller's context (the resolve() layer reports them "unprobed").
#include <string>
#include <vector>

#include "extractors.h"
#include "mnu/mnu.h"

namespace opennova::refs::detail {

namespace {

// %VAR% stylesheet indirection (e.g. %DEF_FONTNAME_LG%, %TRIM_COLOR%): resolves
// through menu_style.mns at runtime, not to an asset name.
bool is_style_variable(const std::string& value) {
    return !value.empty() && value.front() == '%';
}

// Attribute tokens are authored in mixed case; the runtime compares
// case-insensitively (CRT _wcsicmp), so the extractor does too.
bool token_is(const std::string& value, const char* want) {
    return lower_ascii(value) == want;
}

void add_appearances(EdgeSink& sink, const std::vector<mnu::Appearance>& appearances,
                     const std::string& site) {
    for (const mnu::Appearance& appearance : appearances) {
        if (token_is(appearance.type, "image") && !is_style_variable(appearance.value)) {
            sink.add(appearance.value, "texture", site);
        }
    }
}

void add_sounds(EdgeSink& sink, const std::vector<mnu::Sound>& sounds, const std::string& site) {
    for (const mnu::Sound& sound : sounds) {
        // File-less SOUND nodes never play in the original (docs/mnu/menu-re.md
        // D-MNU-3); EdgeSink already drops empty names.
        sink.add(sound.file, "sound", site + ".sound[" + sound.trigger + "]");
    }
}

void add_items(EdgeSink& sink, const mnu::Items& items, const std::string& site) {
    for (const mnu::Item& item : items.items) {
        if (token_is(item.type, "image") && !is_style_variable(item.text)) {
            sink.add(item.text, "texture", site + ".item");
        } else if (token_is(item.type, "id")) {
            sink.add(item.text, "string_id", site + ".item");
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
            sink.add(action.file, "menu", site + ".action[" + action.target + "]");
        }
    }

    if (token_is(window.string_data.type, "id")) {
        sink.add(window.string_data.value, "string_id", site + ".string");
    }
    if (!is_style_variable(window.font.name)) {
        sink.add(window.font.name, "font", site + ".font");
    }

    sink.add(window.frame.stencil, "texture", site + ".frame.stencil");
    sink.add(window.frame.brush, "texture", site + ".frame.brush");
    sink.add(window.frame.monogram, "texture", site + ".frame.monogram");
    sink.add(window.cursor.file, "texture", site + ".cursor");
    sink.add(window.text_rsrc, "strings", site + ".text_rsrc");
    sink.add(window.datasource, "datasource", site + ".datasource");

    add_items(sink, window.items, site);
    add_items(sink, window.list_box.items, site + ".list_box");
    add_appearances(sink, window.list_box.appearances, site + ".list_box");
    add_appearances(sink, window.list_box.scrollbar.track, site + ".list_box.scrollbar");
    add_appearances(sink, window.list_box.scrollbar.shuttle, site + ".list_box.scrollbar");
    add_appearances(sink, window.list_box.scrollbar.scrollup, site + ".list_box.scrollbar");
    add_appearances(sink, window.list_box.scrollbar.scrolldown, site + ".list_box.scrollbar");
    add_sounds(sink, window.list_box.scrollbar.sounds, site + ".list_box.scrollbar");

    for (const mnu::TableHeader& header : window.table_data.column.headers) {
        // type="id" headers resolve through the string table
        // [orig: CUIStringTable_LookupString, see mnu.h TableHeader].
        if (token_is(header.type, "id")) {
            sink.add(header.text, "string_id", site + ".table.header");
        }
    }
    for (const mnu::TableSubst& subst : window.table_data.column.substitutions) {
        if (subst.is_file) {
            sink.add(subst.file, "texture", site + ".table.subst");
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
        sink.add(screen.text_rsrc, "strings", site + ".text_rsrc");
        sink.add(screen.cursor_file, "texture", site + ".cursor");
        walk_window(sink, screen.root_window, site);
    }
    return true;
}

}  // namespace opennova::refs::detail
