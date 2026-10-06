#include "mnu_document.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/model/staged_rows.h>
#include <editor/project/project_files.h>
#include <formats/mnu/mnu_schema.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_screen_inputs.h>
#include <runtime/menu/menu_text_tables.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace opennova::editor {

namespace {

// A code the render check makes (preview/menu_render_check.h): a compiler note, a screen it could
// not map.
constexpr FindingCodeRow from_render_check(const char *token) {
	FindingCodeRow row;
	row.token = token;
	row.source = FindingSource::RenderCheck;
	return row;
}

constexpr FindingCodeEntry<MenuFinding> kFindingEntries[] = {
	{ MenuFinding::InvalidInput, { "menu.invalid_input", FindingFix::None, nullptr, true } },
	{ MenuFinding::IgnoredInput, { "menu.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	{ MenuFinding::Unserializable, { "menu.unserializable", FindingFix::None, nullptr, true } },
	{ MenuFinding::DuplicateScreen, { "menu.duplicate_screen" } },
	{ MenuFinding::DuplicateWindow, { "menu.duplicate_window" } },
	{ MenuFinding::ActionInert, { "menu.action_inert" } },
	{ MenuFinding::RenderMapping, from_render_check("menu.render.mapping") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(MenuFinding::kCount),
		"every MenuFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the menu's own rows follow MenuFinding's order, each token its own");

// A compiler note's row: the render check's, and whether a note of it is a Problems row and at what
// severity (menu_note_problem reads it): a Warning, or an Info, for a consequence the author may
// not mean; None for a note only the preview shows, a name the project lacks (the asset graph's
// finding, reference.missing: a project check never repeats one) or a note that only explains the
// picture.
constexpr FindingCodeRow note_row(const char *token, FindingProblem problem) {
	FindingCodeRow row = from_render_check(token);
	row.problem = problem;
	return row;
}

using Note = menu::MenuFrameNoteCode;
using P = FindingProblem;
constexpr FindingCodeEntry<Note> kNoteEntries[] = {
	{ Note::AppearanceStateUnknown, note_row("menu.render.appearance_state_unknown", P::Warning) },
	{ Note::AppearanceTypeUnknown, note_row("menu.render.appearance_type_unknown", P::Warning) },
	{ Note::AppearanceCustom, note_row("menu.render.appearance_custom", P::None) },
	{ Note::AppearanceReplaced, note_row("menu.render.appearance_replaced", P::Warning) },
	{ Note::ColorUnparsed, note_row("menu.render.color_unparsed", P::Warning) },
	{ Note::ColorTransparent, note_row("menu.render.color_transparent", P::Warning) },
	{ Note::StyleVarUnresolved, note_row("menu.render.style_var_unresolved", P::None) },
	{ Note::TypeUnknown, note_row("menu.render.type_unknown", P::Warning) },
	{ Note::TypeInteriorDeferred, note_row("menu.render.type_interior_deferred", P::Info) },
	{ Note::ItemKindNotDrawn, note_row("menu.render.item_kind_not_drawn", P::Info) },
	{ Note::TableCellsDeferred, note_row("menu.render.table_cells_deferred", P::Info) },
	{ Note::TableCellsCustom, note_row("menu.render.table_cells_custom", P::None) },
	{ Note::ScrollExtentDefault, note_row("menu.render.scroll_extent_default", P::None) },
	{ Note::FontMissing, note_row("menu.render.font_missing", P::None) },
	{ Note::FontUnreadable, note_row("menu.render.font_unreadable", P::Warning) },
	{ Note::TextureMissing, note_row("menu.render.texture_missing", P::None) },
	{ Note::TextureUnreadable, note_row("menu.render.texture_unreadable", P::Warning) },
	{ Note::TextTableMissing, note_row("menu.render.text_table_missing", P::None) },
	{ Note::TextTableUnreadable, note_row("menu.render.text_table_unreadable", P::Warning) },
	{ Note::RectEmpty, note_row("menu.render.rect_empty", P::Warning) },
	{ Note::TextTruncated, note_row("menu.render.text_truncated", P::Warning) },
	{ Note::TextNoRoom, note_row("menu.render.text_no_room", P::Warning) },
	{ Note::TextNoFont, note_row("menu.render.text_no_font", P::Warning) },
	{ Note::TextIdMissing, note_row("menu.render.text_id_missing", P::None) },
	{ Note::ImageBandEmpty, note_row("menu.render.image_band_empty", P::Warning) },
	{ Note::ImageHeightShared, note_row("menu.render.image_height_shared", P::Info) },
	{ Note::StateFallback, note_row("menu.render.state_fallback", P::None) },
	{ Note::CheckedNoArt, note_row("menu.render.checked_no_art", P::Warning) },
	{ Note::FrameAbsent, note_row("menu.render.frame_absent", P::Warning) },
	{ Note::FrameStencilUnloaded, note_row("menu.render.frame_stencil_unloaded", P::None) },
	{ Note::FrameNoStencil, note_row("menu.render.frame_no_stencil", P::Warning) },
	{ Note::FrameTileZero, note_row("menu.render.frame_tile_zero", P::Warning) },
	{ Note::SpinArrowEmpty, note_row("menu.render.spin_arrow_empty", P::Warning) },
	{ Note::ListRowsClipped, note_row("menu.render.list_rows_clipped", P::Info) },
	{ Note::TableNoColumns, note_row("menu.render.table_no_columns", P::None) },
	{ Note::TableHeaderClipped, note_row("menu.render.table_header_clipped", P::Warning) },
	{ Note::TableHeaderWidthZero, note_row("menu.render.table_header_width_zero", P::Warning) },
	{ Note::MarqueeRuntimeContent, note_row("menu.render.marquee_runtime_content", P::None) },
};
static_assert(std::size(kNoteEntries) == static_cast<size_t>(menu::kMenuFrameNoteCodeCount),
		"every compiler note has exactly one row");
static_assert(finding_entries_well_formed(kNoteEntries),
		"the render check's rows follow the note codes' order, each token its own");

constexpr size_t kOwnFindings = static_cast<size_t>(MenuFinding::kCount);
constexpr size_t kNoteFindings = std::size(kNoteEntries);

// The menu's own rows, then the notes', every one the menu's group.
constexpr std::array<FindingCodeRow, kOwnFindings + kNoteFindings> joined_findings() {
	std::array<FindingCodeRow, kOwnFindings + kNoteFindings> rows{};
	const auto own = finding_rows(kFindingEntries, FindingGroup::Menus);
	const auto noted = finding_rows(kNoteEntries, FindingGroup::Menus);
	for (size_t i = 0; i < kOwnFindings; ++i) rows[i] = own[i];
	for (size_t i = 0; i < kNoteFindings; ++i) rows[kOwnFindings + i] = noted[i];
	return rows;
}
constexpr std::array<FindingCodeRow, kOwnFindings + kNoteFindings> kFindingRows = joined_findings();
static_assert(finding_rows_well_formed(kFindingRows),
		"no note's token is one of the menu's own, every row the menu's group, and only the "
		"render check's rows say whether a finding of theirs is a Problems row");

constexpr NodeKind kScreen = node_kind(MenuKind::Screen);
constexpr NodeKind kWindow = node_kind(MenuKind::Window);
const char *const kByteOrderMark = "\xEF\xBB\xBF";

MenuScreen &screen_of(Node &node) { return static_cast<MenuScreen &>(node); }
const MenuScreen &screen_of(const Node &node) { return static_cast<const MenuScreen &>(node); }


// --- names -----------------------------------------------------------------------------

// The windows a window holds that retail's name lookups reach: its children and the
// windows its parts hold. A part itself is not among them: retail attaches it as a child
// but names it when it creates it (LISTBOX_WND, SPINLISTWND_UP, LISTWND_SCROLL, ...), so
// its authored NAME is never looked up (docs/mnu/menu-re.md, "Which type reads what").
template <class W, class Fn> void each_named_window(W &window, Fn &&fn) {
	for (auto &child : window.children) fn(child);
	for (auto *part : {&window.list_box, &window.spinup, &window.spindown, &window.scrollbar})
		if (auto *held = part->latent()) each_named_window(*held, fn);
}

// The names of a screen's windows as retail's lookups reach them, case-folded as the
// lookups compare them.
void collect_names(const mnu::Window &window, std::set<std::string> &taken) {
	if (!window.name.empty()) taken.insert(strutil::to_upper(window.name));
	each_named_window(window, [&](const mnu::Window &held) { collect_names(held, taken); });
}

std::set<std::string> screen_names(const mnu::Screen &screen) {
	std::set<std::string> taken;
	for (const mnu::Window &root : screen.roots) collect_names(root, taken);
	return taken;
}

// The names of a menu's screens, case-folded as retail's screen lookups compare them.
std::set<std::string> menu_screen_names(const std::vector<std::shared_ptr<const Node>> &rows) {
	std::set<std::string> taken;
	for (const auto &node : rows) taken.insert(strutil::to_upper(screen_of(*node).screen.name));
	return taken;
}

// `name`, or its stem (the name without its trailing digits) with the first number from 2
// that `taken` (a screen's window names, a menu's screen names) does not hold.
std::string unique_name(const std::set<std::string> &taken, const std::string &name) {
	if (name.empty() || !taken.count(strutil::to_upper(name))) return name;
	std::string stem = name;
	while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) stem.pop_back();
	if (stem.empty()) stem = name;
	for (int n = 2;; ++n) {
		const std::string candidate = stem + std::to_string(n);
		if (!taken.count(strutil::to_upper(candidate))) return candidate;
	}
}

void make_names_unique(mnu::Window &window, std::set<std::string> &taken) {
	window.name = unique_name(taken, window.name);
	if (!window.name.empty()) taken.insert(strutil::to_upper(window.name));
	each_named_window(window, [&](mnu::Window &held) { make_names_unique(held, taken); });
}

// The document windows of a screen in pre-order (the roots and their children; never a
// part or what a part holds), each with its identity.
void walk_document_windows(const MenuScreen &screen, const std::function<bool(const RecordIds &)> &visit) {
	const size_t children = menu_children_list();
	std::function<bool(const RecordIds &)> step = [&](const RecordIds &ids) {
		if (!visit(ids)) return false;
		if (children < ids.lists.size())
			for (const RecordIds &child : ids.lists[children])
				if (!step(child)) return false;
		return true;
	};
	if (screen.ids.lists.empty()) return;
	for (const RecordIds &root : screen.ids.lists[0])
		if (!step(root)) return;
}

bool text_width(const Value &value, size_t width, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) { error = "This field takes text."; return false; }
	if (text->size() >= width) { error = "The text is too long."; return false; }
	return true;
}

// The extra elements a window keeps at its top level, and the attributes a window keeps
// (the reader keeps nothing else there, so a save would lose any other): mnu::kExtraTags
// and mnu::kExtraAttributes.
const char *const kWindowAttributes = "A window keeps only the PLAYERLIST and SERVERLIST attributes here.";
const char *const kFlagsTakeNoValue = "PLAYERLIST and SERVERLIST are flags: they take no value.";

const std::string &window_elements_error() {
	static const std::string text = [] {
		std::string out = "A window keeps only these elements: ";
		for (size_t i = 0; mnu::kExtraTags[i]; ++i) out += std::string(i ? ", " : "") + mnu::kExtraTags[i];
		return out + ".";
	}();
	return text;
}

// Whether a window (or a part) keeps `record` at its top level as it is: an attribute
// only as PLAYERLIST or SERVERLIST with no value, an extra element only by a tag its
// parses read. A record that moves into a window meets the rules a Set there meets.
bool window_keeps(const RecordHandle &record, std::string &error) {
	if (menu_shape(record.kind) == mnu::SchemaShape::Attribute) {
		const auto &attribute = record.as<mnu::ElementAttribute>();
		if (!mnu::known_token(attribute.name, mnu::kExtraAttributes)) { error = kWindowAttributes; return false; }
		if (attribute.has_value) { error = kFlagsTakeNoValue; return false; }
	} else if (menu_shape(record.kind) == mnu::SchemaShape::Element) {
		if (!mnu::known_token(record.as<mnu::Element>().tag, mnu::kExtraTags)) {
			error = window_elements_error();
			return false;
		}
	}
	return true;
}

} // namespace

// --- the row -------------------------------------------------------------------------

MenuScreen::MenuScreen() { kind = kScreen; }

namespace {

// What a menu's records hold beyond their objects (MenuScreen::footprint): each word, and each
// list's elements with what each holds. Declared first: an element holds elements, a window
// windows.
size_t content(const std::string &text) { return footprint_of(text); }
size_t content(const mnu::Appearance &appearance);
size_t content(const mnu::Sound &sound);
size_t content(const mnu::Action &action);
size_t content(const mnu::Item &item);
size_t content(const mnu::TableRow &row);
size_t content(const mnu::TableHeader &header);
size_t content(const mnu::TableBody &body);
size_t content(const mnu::TableSubst &substitution);
size_t content(const mnu::Hotkey &hotkey);
size_t content(const mnu::ElementAttribute &attribute);
size_t content(const mnu::Element &element);
size_t content(const mnu::Window &window);

// A list's elements, and what each holds.
template <class T> size_t list_content(const std::vector<T> &items) {
	size_t bytes = footprint_of(items);
	for (const T &item : items) bytes += content(item);
	return bytes;
}

size_t content(const mnu::Appearance &appearance) {
	return footprint_of(appearance.state) + footprint_of(appearance.type) +
	       footprint_of(appearance.value) + footprint_of(appearance.flags);
}

size_t content(const mnu::Sound &sound) {
	return footprint_of(sound.state) + footprint_of(sound.trigger) + footprint_of(sound.file);
}

size_t content(const mnu::Action &action) {
	return footprint_of(action.type) + footprint_of(action.state) + footprint_of(action.file) +
	       footprint_of(action.field) + footprint_of(action.field_attr) +
	       footprint_of(action.test) + footprint_of(action.target);
}

size_t content(const mnu::Item &item) {
	return footprint_of(item.type) + footprint_of(item.value) + footprint_of(item.text) +
	       footprint_of(item.justify) + footprint_of(item.vjustify);
}

size_t content(const mnu::TableRow &row) { return list_content(row.cells); }

size_t content(const mnu::TableHeader &header) {
	return footprint_of(header.justify) + footprint_of(header.vjustify) +
	       footprint_of(header.sort) + footprint_of(header.type) + footprint_of(header.text);
}

size_t content(const mnu::TableBody &body) {
	return footprint_of(body.justify) + footprint_of(body.vjustify) + footprint_of(body.display) +
	       footprint_of(body.bitmap_flags);
}

size_t content(const mnu::TableSubst &substitution) {
	return footprint_of(substitution.value) + footprint_of(substitution.file);
}

size_t content(const mnu::Hotkey &hotkey) { return footprint_of(hotkey.value); }

size_t content(const mnu::ElementAttribute &attribute) {
	return footprint_of(attribute.name) + footprint_of(attribute.value);
}

size_t content(const mnu::Element &element) {
	return footprint_of(element.tag) + list_content(element.attributes) +
	       footprint_of(element.text) + list_content(element.children);
}

// A window: its words, its lists, its STRING, TOGGLE_STRING, FONT, FRAME, CURSOR, ITEMS and table,
// its children and its parts (each window it holds counted whole).
size_t content(const mnu::Window &window) {
	size_t bytes = footprint_of(window.name) + footprint_of(window.type_token) +
	               footprint_of(window.orientation) + footprint_of(window.text_rsrc) +
	               footprint_of(window.private_data);
	bytes += list_content(window.appearances) + list_content(window.sounds) +
	         list_content(window.actions) + list_content(window.datasources) +
	         list_content(window.hotkeys) + list_content(window.shuttle) +
	         list_content(window.scrollup) + list_content(window.scrolldown) +
	         list_content(window.extras) + list_content(window.extra_attributes) +
	         list_content(window.children);
	const mnu::String &label = window.string_data;
	bytes += footprint_of(label.type) + footprint_of(label.justify) +
	         footprint_of(label.vjustify) + footprint_of(label.value);
	bytes += footprint_of(window.toggle_string.type) + footprint_of(window.toggle_string.value);
	const mnu::Font &font = window.font;
	for (const std::string *text :
	     {&font.name, &font.default_fg, &font.default_bg, &font.mouseover_fg, &font.mouseover_bg,
	      &font.selected_fg, &font.selected_bg, &font.disabled_fg, &font.disabled_bg})
		bytes += footprint_of(*text);
	bytes += footprint_of(window.frame.stencil) + footprint_of(window.frame.brush) +
	         footprint_of(window.frame.monogram);
	bytes += footprint_of(window.cursor.file) + footprint_of(window.cursor.flags);
	const mnu::Items &items = window.items;
	bytes += footprint_of(items.justify) + footprint_of(items.vjustify) +
	         list_content(items.appearances) + list_content(items.items) +
	         list_content(items.rows);
	const mnu::TableColumn &column = window.table_data.column;
	bytes += list_content(column.headers) + list_content(column.bodies) +
	         list_content(column.substitutions) + footprint_of(column.primary_sort_token);
	for (const mnu::WindowPart *part :
	     {&window.list_box, &window.spinup, &window.spindown, &window.scrollbar})
		if (const mnu::Window *held = part->latent())
			bytes += sizeof(mnu::Window) + content(*held);
	return bytes;
}

} // namespace

size_t MenuScreen::footprint() const {
	return sizeof(MenuScreen) + collections_footprint() + footprint_of(screen.name) + list_content(screen.roots) +
	       ids_footprint();
}

std::shared_ptr<Node> MenuScreen::clone() const { return std::make_shared<MenuScreen>(*this); }

RecordHandle MenuScreen::record() const { return {kScreen, const_cast<mnu::Screen *>(&screen)}; }

bool is_menu_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::Menu;
}

std::string menu_text_scope(const std::string &table) {
	return strutil::to_upper(basename_of(table)) + "/" + menu::kMenuTextSection;
}

std::string menu_screen_scope(const std::string &menu_file) {
	return strutil::to_upper(basename_of(menu_file));
}

std::string menu_window_scope(const std::string &menu_file, const std::string &screen) {
	return menu_screen_scope(menu_file) + "/" + strutil::to_upper(screen);
}
// --- the declarations ------------------------------------------------------------------

Applicability MnuDocument::list_applies(const Node &, const Located &owner, size_t list) const {
	// The screen's root windows are read; a window's lists by its type, where it sits.
	if (owner.is_row()) return Applicability::Reads;
	return menu_applicability(menu_list_reads(owner.record, owner.owners(), list));
}

void MnuDocument::refine_field(const NodeAddress &address, FieldUse &out) const {
	// Whether the game reads the field where its record sits, and what a field whose sibling decides it
	// names on this record: the table's (menu_context, the format's rules), with the colour that goes
	// with it.
	TableDocument::refine_field(address, out);
	out.color = menu_reference_colour(out.reference);
	const FieldSchema &field = *out.schema;
	// A screen's NAME is looked up in its file, a window's on its screen (lookup_names).
	if (out.defines == ReferenceKind::MenuScreen) out.scope = menu_screen_scope(Document::path());
	const Node *node = address.child ? row(address.row) : nullptr;
	Located at;
	if (!node || node->kind != kScreen || !locate(*node, address.child, at) || at.is_row() ||
	    at.record.kind != address.kind)
		return;
	const MenuContext context = menu_context(at.record, at.owners());
	// A SOUND's trigger is a set of the bank its FILE names, looked up there alone (the sound lane): the
	// window's sound plays from that bank's entry of the menu's bank collection [orig:
	// sound_collection_play_trigger @ 0x652de0 -> SoundBank_FindTriggerAndPlay @ 0x75d010].
	if (field.id == "trigger" && std::string(menu_table().kind(at.record.kind)->row().token) == "sound") {
		const std::string &bank = at.record.as<mnu::Sound>().file;
		if (!bank.empty()) {
			out.reference = ReferenceKind::Sound;
			out.scope = strutil::to_upper(basename_of(bank));
		}
	}
	// A string id resolves in the "menu" section of the table the window reads: its own
	// TEXT_RSRC, else the one it falls back to (the runtime's rule, menu_screen_inputs.h).
	// With neither, the scope names no table: the game shows the id.
	if (out.reference == ReferenceKind::TextId) {
		const std::string *table = menu::window_text_rsrc(*context.window, context.text_fallback);
		out.scope = menu_text_scope(table ? *table : std::string());
	}
	// A SCREEN target is a screen of the file the ACTION loads first (no FILE names none: a
	// write issue, retail's fault); every other target a window of the acting window's own
	// screen, looked up by that screen's NAME (lookup_names).
	if (out.reference == ReferenceKind::MenuScreen) {
		const std::string &file = at.record.as<mnu::Action>().file;
		if (file.empty()) out.reference = ReferenceKind::None;
		else out.scope = menu_screen_scope(file);
	} else if (out.reference == ReferenceKind::MenuWindow) {
		out.scope = menu_window_scope(Document::path(), screen_of(*node).screen.name);
	}
	// A part is named by its owner when retail makes it: no lookup finds it by its NAME.
	if (menu_shape(at.record.kind) == mnu::SchemaShape::Part) out.defines = ReferenceKind::None;
	if (out.defines == ReferenceKind::MenuWindow)
		out.scope = menu_window_scope(Document::path(), screen_of(*node).screen.name);
	// Any text that makes no reference of its own (a NAME, a shown text, an ACTION's target,
	// an extra element's text) takes a whole %NAME% as the stylesheet variable's value: the game
	// expands the menu's whole text before its parse [orig: NapiXML_ExpandVariablesInText @
	// 0x63a000; ADR 0005].
	if (out.reference == ReferenceKind::None && field.type == FieldType::Text)
		out.variable_through = ReferenceKind::MenuText;
}

void MnuDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	if (lookups_.load_generation != load_generation() || lookups_.revision != revision() ||
	    !lookups_.made) {
		lookups_.made = true;
		lookups_.load_generation = load_generation();
		lookups_.revision = revision();
		lookups_.unfound.clear();
		for (const MenuLookupName &name : lookup_names())
			if (name.found != MenuLookupName::Found::Yes)
				lookups_.unfound[name.address.child ? name.address.child : name.address.row] = name.found;
	}
	const auto unfound = lookups_.unfound.find(address.child ? address.child : address.row);
	if (unfound == lookups_.unfound.end()) return;
	facts.inert = true;
	switch (unfound->second) {
	case MenuLookupName::Found::LaterScreen: facts.inert_reason = "a later screen of the file has its NAME, which the lookup finds"; break;
	case MenuLookupName::Found::EarlierWindow: facts.inert_reason = "an earlier window of its screen has its NAME, which the lookup finds"; break;
	case MenuLookupName::Found::UnderNameless: facts.inert_reason = "a window above it has no NAME, where the lookup's search stops"; break;
	case MenuLookupName::Found::ShadowedScreen: facts.inert_reason = "its screen is shadowed by a later screen of the same NAME"; break;
	case MenuLookupName::Found::Yes: break;
	}
}

int MnuDocument::window_index(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node || node->kind != kScreen || address.kind != kWindow || !address.child) return -1;
	int index = -1, at = 0;
	walk_document_windows(screen_of(*node), [&](const RecordIds &ids) {
		if (ids.id == address.child) { index = at; return false; }
		++at;
		return true;
	});
	return index;
}

NodeId MnuDocument::window_at(const Node &screen, size_t preorder) const {
	if (screen.kind != kScreen) return 0;
	NodeId found = 0;
	size_t at = 0;
	walk_document_windows(screen_of(screen), [&](const RecordIds &ids) {
		if (at++ == preorder) { found = ids.id; return false; }
		return true;
	});
	return found;
}

NodeAddress MnuDocument::record_at(const Node &screen, size_t preorder, size_t list, size_t index) const {
	const NodeId window = window_at(screen, preorder);
	Located at;
	if (!window || !locate(screen, window, at)) return {};
	if (list >= at.ids->lists.size() || index >= at.ids->lists[list].size()) return {};
	return {screen.id, menu_table().kind(at.record.kind)->lists()[list].spec.kind, at.ids->lists[list][index].id};
}

bool MnuDocument::identities_match() const {
	for (const auto &node : rows()) {
		const MenuScreen &screen = screen_of(*node);
		if (!ids_match(menu_table(), screen.record(), screen.ids)) return false;
	}
	return true;
}

std::vector<MenuLookupName> MnuDocument::lookup_names() const {
	using Found = MenuLookupName::Found;
	std::vector<MenuLookupName> out;
	// The screen each NAME finds: the last of the name [orig: CUIScene_SelectNodeByName @
	// 0x63b6b0 walks the screens newest first].
	std::map<std::string, NodeId> newest;
	for (const auto &node : rows()) newest[strutil::to_upper(screen_of(*node).screen.name)] = node->id;
	for (const auto &node : rows()) {
		const mnu::Screen &screen = screen_of(*node).screen;
		if (screen.name.empty()) continue;
		MenuLookupName entry;
		entry.address = {node->id, kScreen, 0};
		entry.name = screen.name;
		entry.screen = node->id;
		const NodeId found = newest[strutil::to_upper(screen.name)];
		if (found != node->id) {
			entry.found = Found::LaterScreen;
			entry.found_instead = {found, kScreen, 0};
		}
		out.push_back(std::move(entry));
	}
	const size_t children = menu_children_list();
	std::vector<size_t> parts; // the part lists, in the order the parts attach
	const std::vector<TableList> &lists = menu_table().kind(kWindow)->lists();
	for (size_t i = 0; i < lists.size(); ++i)
		if (menu_shape(lists[i].spec.kind) == mnu::SchemaShape::Part) parts.push_back(i);
	for (const auto &node : rows()) {
		const MenuScreen &screen = screen_of(*node);
		const bool shadowed = newest[strutil::to_upper(screen.screen.name)] != node->id;
		std::map<std::string, NodeAddress> first; // the window each NAME finds on this screen
		// [orig: CWnd_FindChildByName @ 0x646850: a window with no NAME returns nothing,
		// its own NAME is compared, then its children in order]
		std::function<void(const RecordHandle &, const RecordIds &, bool, bool)> visit =
		        [&](const RecordHandle &record, const RecordIds &ids, bool reached, bool part) {
			        const auto &window = record.as<mnu::Window>();
			        bool open = reached;
			        if (!part && window.name.empty()) {
				        open = false;
			        } else if (!part) {
				        MenuLookupName entry;
				        entry.address = {node->id, kWindow, ids.id};
				        entry.name = window.name;
				        entry.screen = node->id;
				        if (shadowed) {
					        entry.found = Found::ShadowedScreen;
				        } else if (!reached) {
					        entry.found = Found::UnderNameless;
				        } else {
					        const auto placed = first.emplace(strutil::to_upper(window.name), entry.address);
					        if (!placed.second) {
						        entry.found = Found::EarlierWindow;
						        entry.found_instead = placed.first->second;
					        }
				        }
				        out.push_back(std::move(entry));
			        }
			        const TableKind &kind = *menu_table().kind(record.kind);
			        if (children < ids.lists.size())
				        for (size_t i = 0; i < ids.lists[children].size(); ++i)
					        visit(kind.lists()[children].ops.at(record, i), ids.lists[children][i], open, false);
			        // A part carries the name its owner gives it, so the search goes on inside.
			        for (const size_t list : parts) {
				        const ListOps &ops = kind.lists()[list].ops;
				        if (list < ids.lists.size() && !ids.lists[list].empty() && ops.present(record))
					        visit(ops.at(record, 0), ids.lists[list][0], open, true);
			        }
		        };
		if (screen.ids.lists.empty()) continue;
		const std::vector<RecordIds> &roots = screen.ids.lists[0];
		for (size_t i = 0; i < roots.size() && i < screen.screen.roots.size(); ++i)
			visit(RecordHandle{kWindow, const_cast<mnu::Window *>(&screen.screen.roots[i])}, roots[i], true, false);
	}
	return out;
}
mnu::Document MnuDocument::native() const {
	mnu::Document document;
	if (const auto *state = dynamic_cast<const MenuFileState *>(file_state())) document.source_encoding = state->source_encoding;
	for (const auto &node : rows()) document.screens.push_back(screen_of(*node).screen);
	return document;
}

SerializeResult MnuDocument::serialize() const {
	SerializeResult result;
	if (blocked()) {
		for (const auto &issue : issues()) if (issue.blocks) result.issues.push_back(issue);
		return result;
	}
	// A value the file cannot hold, or one retail faults on, blocks the save with the
	// retail consequence (docs/mnu/menu-re.md, "The writer"), on the record and the field
	// that cause it (the format names them by the same locator and field path).
	const mnu::Document document = native();
	for (const mnu::WriteIssue &issue : mnu::write_issues(document)) {
		const NodeAddress address = address_at(issue.locator);
		std::string record = address.row ? record_path(address) : std::string();
		if (record.empty()) record = issue.window.empty() ? issue.screen : issue.screen + "/" + issue.window;
		result.issues.push_back({true, 0, record, issue.field, issue.message, issue.locator});
	}
	if (!result.issues.empty()) return result;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!mnu::serialize_bytes(document, bytes, error)) {
		result.issues.push_back({true, 0, "", "", error, ""});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::shared_ptr<const mnu::Document> MnuDocument::saved_image(std::vector<SourceIssue> *issues) const {
	if (!saved_.made || saved_.load_generation != load_generation() ||
	    saved_.revision != revision()) {
		saved_ = SavedImage();
		saved_.made = true;
		saved_.load_generation = load_generation();
		saved_.revision = revision();
		const SerializeResult &result = saved_serialization();
		if (result.ok()) {
			auto image = std::make_shared<mnu::Document>();
			std::string error;
			if (mnu::parse(reinterpret_cast<const uint8_t *>(result.text.data()), result.text.size(), *image, error))
				saved_.image = std::move(image);
			else
				saved_.issues.push_back({true, 0, std::string(), std::string(), "The game cannot read the menu back: " + error,
				                         std::string()});
		} else {
			saved_.issues = result.issues;
		}
	}
	if (issues) *issues = saved_.issues;
	return saved_.image;
}

const SerializeResult &MnuDocument::saved_serialization() const {
	if (!serialized_.made || serialized_.load_generation != load_generation() ||
	    serialized_.revision != revision()) {
		serialized_.made = true;
		serialized_.load_generation = load_generation();
		serialized_.revision = revision();
		serialized_.result = serialize();
	}
	return serialized_.result;
}

size_t MnuDocument::screen_position(NodeId row_id) const {
	for (size_t i = 0; i < rows().size(); ++i)
		if (rows()[i]->id == row_id) return i;
	return SIZE_MAX;
}

std::string MnuDocument::copy(const std::vector<NodeAddress> &records) const {
	if (records.empty()) return std::string();
	// The windows named, by the screen that holds each: windows of several screens copy together.
	std::map<NodeId, std::set<NodeId>> wanted;
	size_t named = 0;
	for (const NodeAddress &record : records) {
		const Node *node = row(record.row);
		if (!node || node->kind != kScreen || record.kind != kWindow || !record.child) return std::string();
		named += wanted[record.row].insert(record.child).second ? 1 : 0;
	}
	// The windows in document order, the screens in the file's and each one's windows in its own: the
	// clipboard's roots, which a paste puts where the tree's rule says, in this order. The walk meets
	// every window (the roots, the children and the windows a part holds, not a part itself); a
	// selected window's windows come with it, so a selected one inside it is covered.
	mnu::Document clip;
	if (const auto *state = dynamic_cast<const MenuFileState *>(file_state())) clip.source_encoding = state->source_encoding;
	clip.screens.emplace_back();
	clip.screens.back().name = "CLIPBOARD";
	size_t found = 0;
	const std::set<NodeId> *screen_wanted = nullptr;
	std::function<void(const RecordHandle &, const RecordIds &, bool, bool)> visit =
	        [&](const RecordHandle &record, const RecordIds &ids, bool window, bool covered) {
		        const bool selected = window && screen_wanted->count(ids.id) != 0;
		        if (selected) {
			        ++found;
			        if (!covered) clip.screens.back().roots.push_back(record.as<mnu::Window>());
		        }
		        const TableKind &kind = *menu_table().kind(record.kind);
		        for (size_t list = 0; list < kind.lists().size() && list < ids.lists.size(); ++list) {
			        const NodeKind held = kind.lists()[list].spec.kind;
			        if (!is_window_kind(held)) continue;
			        for (size_t i = 0; i < ids.lists[list].size(); ++i)
				        visit(kind.lists()[list].ops.at(record, i), ids.lists[list][i], held == kWindow, covered || selected);
		        }
	        };
	for (const auto &node : rows()) {
		const auto of_screen = wanted.find(node->id);
		if (of_screen == wanted.end()) continue;
		screen_wanted = &of_screen->second;
		const MenuScreen &screen = screen_of(*node);
		if (!screen.ids.lists.empty())
			for (size_t i = 0; i < screen.ids.lists[0].size() && i < screen.screen.roots.size(); ++i)
				visit(RecordHandle{kWindow, const_cast<mnu::Window *>(&screen.screen.roots[i])}, screen.ids.lists[0][i],
				      true, false);
	}
	if (found != named) return std::string();
	// The payload must read back as written: a value the format cannot carry (a quote in
	// an attribute, a window with nothing in it) is refused here, not lost in the paste.
	const std::string text = mnu::serialize(clip);
	mnu::Document back;
	std::string error;
	if (!mnu::parse(text, back, error) || mnu::serialize(back) != text) return std::string();
	return kByteOrderMark + (clip.source_encoding == mnu::SourceEncoding::CodePage ? cp1252_to_utf8(text) : text);
}

// --- reading the file ------------------------------------------------------------------

bool MnuDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                        std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_menu_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a menu.", path());
		return false;
	}
	mnu::Document document;
	std::string message;
	std::vector<mnu::ParseNote> notes;
	if (!(bytes.size() == 1 && bytes[0] == 0) && !mnu::parse(bytes.data(), bytes.size(), document, message, &notes)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, message, path());
		return false;
	}
	auto encoding = std::make_shared<MenuFileState>();
	encoding->source_encoding = document.source_encoding;
	state = encoding;
	for (mnu::Screen &screen : document.screens) {
		auto row = std::make_shared<MenuScreen>();
		row->screen = std::move(screen);
		shape(*row);
		rows.push_back(row);
	}
	// What retail's reader does not read is left out of the model: a note (a save writes the
	// menu without it, which retail reads the same). What retail crashes or hangs on blocks:
	// the file as it stands cannot ship, so editing and the build wait for it to be corrected
	// (docs/mnu/menu-re.md, "Crash and hang cases").
	for (const mnu::ParseNote &note : notes)
		issues.push_back({note.fatal, note.line, std::string(), note.key, note.message, note.locator});
	return true;
}

std::shared_ptr<Node> MnuDocument::make_node(NodeKind kind, NodeId id,
                                             const std::vector<std::shared_ptr<const Node>> &rows,
                                             std::string &error) {
	if (kind != kScreen) { error = "A menu adds screens at the top level; windows nest under a screen."; return nullptr; }
	auto row = std::make_shared<MenuScreen>();
	// A name no other screen has: the game finds the last of two screens of one name, so a
	// new screen under an existing one's name would stand in for it (prepare_duplicate).
	row->screen.name = unique_name(menu_screen_names(rows), "SCREEN" + std::to_string(id));
	mnu::Window main;
	main.name = "MAIN";
	main.type = mnu::WindowType::Window;
	main.position = {0, 0, 800, 600, true, true, true, true};
	row->screen.roots.push_back(main);
	shape(*row);
	return row;
}

// --- editing ---------------------------------------------------------------------------

bool MnuDocument::code_page_model() const {
	const auto *state = dynamic_cast<const MenuFileState *>(file_state());
	return !state || state->source_encoding == mnu::SourceEncoding::CodePage;
}

// A code-page menu's model holds its code page's bytes (mnu.h's SourceEncoding): read as UTF-8, as a
// string table's text is, never handed to a widget as raw cp1252.
bool MnuDocument::read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const {
	if (!TableDocument::read(row, address, field, out)) return false;
	if (auto *text = std::get_if<std::string>(&out); text && code_page_model()) *text = cp1252_to_utf8(*text);
	return true;
}

bool MnuDocument::set_value(Node &node, const Located &at, size_t field, const Value &value, std::string &error) {
	// A text as the model holds it, and its width as the game counts it: every text the reader reads
	// is narrowed to the code page by WideCharToMultiByte(CP_ACP) [orig: CUIButtonWidget_ParseXMLAttributes
	// @ 0x657c30, the STRING parse; CUIElement_ParseXMLDefinition @ 0x648120, the ACTION arm @ 0x648ee2;
	// docs/mnu/menu-re.md, the numeric entity's byte], one byte a character
	// on 1252 (one it lacks narrows to '?'), so a field's width counts characters in either encoding. A
	// code-page menu stores the bytes (a character 1252 has no byte for refused, never stored as UTF-8
	// the game would show as other characters); a Unicode menu keeps the UTF-8.
	const TableKind &table_kind = *menu_table().kind(at.record.kind);
	const FieldSchema &schema = table_kind.fields()[field];
	Value stored = value;
	if (const auto *text = std::get_if<std::string>(&value); text && schema.type == FieldType::Text) {
		size_t characters = 0;
		if (code_page_model()) {
			std::string bytes;
			std::u32string unstorable;
			if (!utf8_to_cp1252(*text, bytes, &unstorable)) {
				std::string named;
				for (size_t i = 0; i < unstorable.size() && i < 5; ++i) {
					if (i) named += ", ";
					utf8_append(named, unstorable[i]);
				}
				error = "This menu is in the game's code page (Windows-1252), which has no " + named + ".";
				return false;
			}
			characters = bytes.size();
			stored = std::move(bytes);
		} else {
			for (const char c : *text) characters += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
		}
		if (schema.width && characters >= schema.width) {
			error = "The text is too long.";
			return false;
		}
	}
	// A name the reader would not keep here is refused: a window keeps only its PLAYERLIST
	// and SERVERLIST attributes, and only the extra elements its parses read at its top level.
	if (!at.is_row() && is_window_kind(at.step().owner.kind)) {
		const mnu::SchemaShape shape = menu_shape(at.record.kind);
		const std::string &id = schema.id;
		if (shape == mnu::SchemaShape::Attribute && id == "name") {
			if (!text_width(stored, 64, error)) return false;
			if (!mnu::known_token(std::get<std::string>(stored), mnu::kExtraAttributes)) {
				error = kWindowAttributes;
				return false;
			}
		}
		if (shape == mnu::SchemaShape::Attribute && id == "value") {
			Value current;
			if (!table_kind.value(field).get(at.record, current) || current != stored) { error = kFlagsTakeNoValue; return false; }
		}
		if (shape == mnu::SchemaShape::Element && id == "tag") {
			if (!text_width(stored, 64, error)) return false;
			if (!mnu::known_token(std::get<std::string>(stored), mnu::kExtraTags)) {
				error = window_elements_error();
				return false;
			}
		}
	}
	return TableDocument::set_value(node, at, field, stored, error);
}

// Clear and Write flip the presence bit (the authored state) and leave the latent value in
// place: a field written again reads what it read while left out.
bool MnuDocument::set_written(Node &node, const Located &at, size_t field, bool present, std::string &error) {
	if (present && !at.is_row() && is_window_kind(at.step().owner.kind) &&
	    menu_shape(at.record.kind) == mnu::SchemaShape::Attribute && menu_table().kind(at.record.kind)->fields()[field].id == "value") {
		error = kFlagsTakeNoValue;
		return false;
	}
	return TableDocument::set_written(node, at, field, present, error);
}

bool MnuDocument::accept_list_edit(const Node &node, const ListChange &change, std::string &error) const {
	const MenuScreen &screen = screen_of(node);
	const bool roots = change.owner->is_row();
	switch (change.operation) {
	case EditOperation::Remove:
		if (roots && screen.screen.roots.size() == 1) {
			error = "A screen keeps at least one root window.";
			return false;
		}
		return true;
	case EditOperation::Move: {
		// The destination is checked before anything moves: a screen keeps a root window, a
		// part goes only where its window has none, and a window takes an attribute or an
		// extra element only as it keeps one (an element's own may be anything).
		const Located &destination = *change.destination;
		if (roots && !destination.is_row() && screen.screen.roots.size() == 1) {
			error = "A screen keeps at least one root window.";
			return false;
		}
		const TableList &list = menu_table().kind(destination.record.kind)->lists()[change.destination_list];
		if (list.spec.max && destination.record.data != change.owner->record.data &&
		    list.ops.size(destination.record) >= list.spec.max) {
			error = std::string("That window already holds a ") + menu_table().kind(list.spec.kind)->row().label + ".";
			return false;
		}
		if (is_window_kind(destination.record.kind) && !window_keeps(change.record->record, error)) return false;
		return true;
	}
	default:
		return true;
	}
}

void MnuDocument::prepare_record(const Node &node, const ListChange &change, DetachedRecord &record) const {
	// A copy's windows are named uniquely within their screen.
	if (change.operation != EditOperation::Duplicate || record.kind != kWindow) return;
	std::set<std::string> taken = screen_names(screen_of(node).screen);
	make_names_unique(*static_cast<mnu::Window *>(record.data.get()), taken);
}

void MnuDocument::after_add(Node &node, const ListChange &, const RecordHandle &made) {
	// New windows are named uniquely within their screen.
	if (made.kind != kWindow) return;
	made.as<mnu::Window>().name = unique_name(screen_names(screen_of(node).screen), "WINDOW1");
}

bool MnuDocument::paste_records(Node &node, const Edit &edit, const IdAllocator &allocate, std::vector<NodeId> &added,
                                std::string &error) {
	MenuScreen &screen = screen_of(node);
	const auto *payload = std::get_if<std::string>(&edit.value);
	if (!payload || payload->compare(0, 3, kByteOrderMark) != 0) { error = "The clipboard holds no menu windows."; return false; }
	// The payload is UTF-8; a code-page menu holds its code page's bytes.
	const auto *state = dynamic_cast<const MenuFileState *>(file_state());
	std::string text = *payload;
	if (state && state->source_encoding == mnu::SourceEncoding::CodePage &&
	    !utf8_to_cp1252(std::string_view(*payload).substr(3), text)) {
		error = "The windows hold characters this menu's code page (1252) cannot hold.";
		return false;
	}
	mnu::Document clip;
	std::string message;
	if (!mnu::parse(text, clip, message) || clip.screens.size() != 1 || clip.screens[0].roots.empty()) {
		error = "The clipboard holds no menu windows.";
		return false;
	}
	// The window list the owner takes windows into: a window's or a part's children, the screen's
	// roots.
	Located owner;
	if (!locate(node, edit.parent, owner)) {
		error = "The record to paste into no longer exists.";
		return false;
	}
	size_t list = 0;
	if (!owner.is_row()) {
		if (!is_window_kind(owner.record.kind)) { error = "Windows go into a window or a screen."; return false; }
		list = menu_children_list();
	}
	std::set<std::string> taken = screen_names(screen.screen);
	size_t position = std::min(edit.position, menu_table().kind(owner.record.kind)->lists()[list].ops.size(owner.record));
	for (mnu::Window &window : clip.screens[0].roots) {
		make_names_unique(window, taken);
		NodeId one = 0;
		if (!insert_record(owner, list, position, menu_window_record(window), allocate, one, error)) return false;
		added.push_back(one);
		++position;
	}
	return true;
}
void MnuDocument::prepare_duplicate(Node &copy, const Node &,
                                    const std::vector<std::shared_ptr<const Node>> &rows) const {
	mnu::Screen &screen = screen_of(copy).screen;
	screen.name = unique_name(menu_screen_names(rows), screen.name);
}

// An editor rule, as a screen keeps one root window: a menu with no screen has nothing
// to show, so the last screen stays (the menu view's Remove waits for a second one).
bool MnuDocument::accept_step(const EditStep &step, const StagedRows &rows,
                              std::string &error) const {
	if (rows.size() != 0) return true;
	for (const RowSwap &swap : step.swaps) {
		if (!swap.before || swap.after) continue;
		error = "A menu keeps at least one screen.";
		return false;
	}
	return true;
}


namespace {

Diagnostic on_record(const MnuDocument &menu, const NodeAddress &address, DiagnosticSeverity severity, MenuFinding code,
                     const std::string &message, const std::string &field) {
	Diagnostic d = make_finding(code, severity, message, menu.path(), field);
	d.record = menu.record_path(address);
	d.row_id = address.row;
	d.child_id = address.child;
	d.record_kind = address.kind;
	return d;
}

// Two screens of one NAME in a file, or two windows of one NAME on a screen: the lookups
// return one of them, so the other is never shown, targeted or bound by name
// (MnuDocument::lookup_names).
void name_findings(const MnuDocument &menu, std::vector<Diagnostic> &findings) {
	for (const MenuLookupName &name : menu.lookup_names()) {
		if (name.found == MenuLookupName::Found::LaterScreen)
			findings.push_back(on_record(menu, name.address, DiagnosticSeverity::Warning, MenuFinding::DuplicateScreen,
			                             "A later screen of this menu is also named " + name.name +
			                                     ": the game finds the last screen of a name, so no ACTION ever shows this one.",
			                             "name"));
		else if (name.found == MenuLookupName::Found::EarlierWindow)
			findings.push_back(on_record(menu, name.address, DiagnosticSeverity::Warning, MenuFinding::DuplicateWindow,
			                             "An earlier window of this screen is also named " + name.name +
			                                     ": the game finds the first window of a name, so no ACTION and no "
			                                     "shell control ever reaches this one by name.",
			                             "name"));
	}
}

// An ACTION the game never runs or ignores [orig: CUIWidget_HandleScriptedAction @ 0x6497f0:
// a widget with no NAME runs no row (@ 0x649810); a TYPE none of the sixteen is code 0, the
// jump table's default (CUIElement_ParseXMLDefinition @ 0x648ee2); a WINDOW row's STATE
// switch (@ 0x6498f7) does nothing for a STATE other than HIDE, SHOW, ENABLE and DISABLE].
void action_findings(const MnuDocument &menu, std::vector<Diagnostic> &findings) {
	const NodeKind action_kind = menu_kind("action");
	for (const auto &row : menu.rows()) {
		std::set<NodeId> nameless; // the windows already reported
		menu.walk_records(*row, [&](const NodeAddress &address, const Document::Placement &at) {
			if (address.kind != action_kind || !menu.present(address, std::string())) return true;
			Value owner_name, type, state;
			if (at.owner.kind == kWindow && menu.get(at.owner, "name", owner_name) &&
			    std::get<std::string>(owner_name).empty() && nameless.insert(at.owner.child).second)
				findings.push_back(on_record(menu, at.owner, DiagnosticSeverity::Warning, MenuFinding::ActionInert,
				                             "This window has no NAME: the game runs none of its ACTIONs.", "name"));
			if (!menu.get(address, "type", type) || !menu.get(address, "state", state)) return true;
			const std::string &verb = std::get<std::string>(type);
			if (verb.empty()) return true; // no TYPE: a write issue (retail's parse faults on it)
			if (!mnu::known_token(verb, mnu::kActionTypes))
				findings.push_back(on_record(menu, address, DiagnosticSeverity::Warning, MenuFinding::ActionInert,
				                             "'" + verb + "' is none of the game's sixteen ACTION types: the game ignores this ACTION.",
				                             "type"));
			else if (strutil::iequals(verb, "WINDOW") &&
			         !mnu::known_token(std::get<std::string>(state), mnu::kActionStates))
				findings.push_back(on_record(menu, address, DiagnosticSeverity::Warning, MenuFinding::ActionInert,
				                             "A WINDOW ACTION changes its target only with the STATE HIDE, SHOW, ENABLE or "
				                             "DISABLE: this one does nothing.",
				                             "state"));
			return true;
		});
	}
}

} // namespace

const FindingCodeRow &finding_code(MenuFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

const FindingCodeRow &finding_code(menu::MenuFrameNoteCode code) {
	return kFindingRows[kOwnFindings + static_cast<size_t>(code)];
}

FindingTable menu_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_menu_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *menu = dynamic_cast<const MnuDocument *>(&document);
	if (!menu) return findings;
	// What retail's reader leaves out (a warning: a save drops it) and what the file
	// cannot hold or retail faults on (an error: it blocks the save and the build), on the
	// screen or window the reader found it in (its locator, in the file as loaded: wherever
	// that record is now, source_address; gone since, the file), so Problems selects it,
	// named by its path when the reader named none.
	source_issue_findings(*menu, finding_code(MenuFinding::InvalidInput),
			finding_code(MenuFinding::IgnoredInput), findings,
			[&](Diagnostic &finding) {
				if (finding.row_id && finding.record.empty())
					finding.record = menu->record_path(
							{ finding.row_id, finding.record_kind, finding.child_id });
			});
	if (document.blocked()) return findings;
	// On the record and the field that cause it, so the inspector shows it there and
	// Problems selects it.
	for (const SourceIssue &issue : menu->saved_serialization().issues) {
		auto diagnostic = make_finding(MenuFinding::Unserializable, DiagnosticSeverity::Error, issue.message,
		                               document.path(), issue.field);
		diagnostic.record = issue.record;
		const NodeAddress address = issue.locator.empty() ? NodeAddress() : menu->address_at(issue.locator);
		diagnostic.row_id = address.row;
		diagnostic.child_id = address.child;
		diagnostic.record_kind = address.kind;
		findings.push_back(std::move(diagnostic));
	}
	name_findings(*menu, findings);
	action_findings(*menu, findings);
	return findings;
}

} // namespace opennova::editor
