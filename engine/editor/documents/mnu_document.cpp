#include "mnu_document.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/model/staged_rows.h>
#include <editor/project/project_files.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_screen_inputs.h>
#include <runtime/menu/menu_text_tables.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
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

using mnu::SchemaApplies;
using mnu::SchemaRecord;
using mnu::SchemaShape;

constexpr NodeKind kScreen = node_kind(MenuKind::Screen);
constexpr NodeKind kWindow = node_kind(MenuKind::Window);
constexpr size_t kRoots = SIZE_MAX;
const char *const kByteOrderMark = "\xEF\xBB\xBF";

MenuScreen &screen_of(Node &node) { return static_cast<MenuScreen &>(node); }
const MenuScreen &screen_of(const Node &node) { return static_cast<const MenuScreen &>(node); }

bool is_window_shape(SchemaShape shape) { return shape == SchemaShape::Window || shape == SchemaShape::Part; }

// --- the kinds -----------------------------------------------------------------------

// Screen 0, Window 1, then one kind per list of the property table, named by the list's
// element path (a list path two owners share, "attribute" and "element", is one kind).
struct KindEntry {
	const char *token;
	const char *label;
	SchemaShape shape;
};
const std::vector<KindEntry> &kind_entries() {
	static const std::vector<KindEntry> table = [] {
		std::vector<KindEntry> out = {{"screen", "Screen", SchemaShape::Screen}, {"window", "Window", SchemaShape::Window}};
		for (const SchemaShape owner : {SchemaShape::Window, SchemaShape::Row, SchemaShape::Element})
			for (const mnu::SchemaList &list : mnu::schema_lists(owner)) {
				bool known = false;
				for (const KindEntry &kind : out) known = known || std::strcmp(kind.token, list.path) == 0;
				if (!known) out.push_back({list.path, list.record_label, list.shape});
			}
		return out;
	}();
	return table;
}

NodeKind kind_of(const char *token) {
	const std::vector<KindEntry> &table = kind_entries();
	for (size_t i = 0; i < table.size(); ++i)
		if (std::strcmp(table[i].token, token) == 0) return NodeKind(i);
	return -1;
}

// The index of the child windows in a window's lists.
size_t children_list() {
	static const size_t index = [] {
		const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(SchemaShape::Window);
		for (size_t i = 0; i < lists.size(); ++i)
			if (std::strcmp(lists[i].path, "window") == 0) return i;
		return lists.size();
	}();
	return index;
}

Applicability applicability(SchemaApplies applies) {
	switch (applies) {
	case SchemaApplies::Reads: return Applicability::Reads;
	case SchemaApplies::Ignored: return Applicability::Ignored;
	case SchemaApplies::Unverified: return Applicability::Unverified;
	}
	return Applicability::Unverified;
}

ReferenceKind reference_of(mnu::SchemaReference reference) {
	switch (reference) {
	case mnu::SchemaReference::Font: return ReferenceKind::Font;
	case mnu::SchemaReference::MenuTexture: return ReferenceKind::MenuTexture;
	case mnu::SchemaReference::StyleVar: return ReferenceKind::StyleVar;
	case mnu::SchemaReference::TextTable: return ReferenceKind::TextTable;
	case mnu::SchemaReference::TextId: return ReferenceKind::TextId;
	case mnu::SchemaReference::Menu: return ReferenceKind::Menu;
	case mnu::SchemaReference::Sound: return ReferenceKind::SoundBank;
	case mnu::SchemaReference::Credits: return ReferenceKind::Credits;
	case mnu::SchemaReference::Screen: return ReferenceKind::MenuScreen;
	case mnu::SchemaReference::Window: return ReferenceKind::MenuWindow;
	default: return ReferenceKind::None;
	}
}

// A style colour's text is the hex AARRGGBB word the parse reads with wcstoul (a %VAR% the
// stylesheet resolves first) [orig: CRT_wcstoxl @ 0x76e93b through the APPEARANCE COLOR /
// OUTLINE arm @ 0x648562, the FONT colours @ 0x648d14..0x648e64 and the spin ITEM
// @ 0x64bd10]: every field naming a style variable holds one (mnu::schema_reference).
FieldColor colour_of(ReferenceKind reference) {
	return reference == ReferenceKind::StyleVar ? FieldColor::HexArgb : FieldColor::None;
}

const std::vector<FieldChoice> &yes_no() {
	static const std::vector<FieldChoice> choices = {{"no", 0}, {"yes", 1}};
	return choices;
}

// --- what the editor shows -------------------------------------------------------------

// The readable names the inspector shows for the table's element paths (the path itself
// is the tooltip). A window's and a part's fields share one table.
struct PathLabel {
	const char *path;
	const char *label;
};

const PathLabel kWindowLabels[] = {
	{"name", "Name"}, {"type", "Type"}, {"hidden", "Hidden"}, {"disable", "Disabled"}, {"checked", "Checked"},
	{"draw_frame", "Draw the frame"}, {"modal", "Modal"}, {"readonly", "Read only"},
	{"as_button", "Acts as a button"}, {"number", "Numbers only"}, {"global_var", "Global variable"},
	{"password", "Password"}, {"group", "Group"}, {"form", "Form"}, {"minval", "Minimum"}, {"maxval", "Maximum"},
	{"maxchar", "Maximum characters"}, {"position.left", "Left"}, {"position.top", "Top"},
	{"position.right", "Right"}, {"position.bottom", "Bottom"}, {"frame.stencil", "Stencil"},
	{"frame.stencil_size", "Stencil size"}, {"frame.insetx", "Horizontal inset"}, {"frame.insety", "Vertical inset"},
	{"frame.brush", "Brush"}, {"frame.monogram", "Monogram"}, {"scroll_extent", "Scroll size"},
	{"scroll_extent_width", "Scroll size is a width"}, {"orientation", "Orientation"},
	{"text_rsrc", "String table"}, {"private_data", "Private data"}, {"cursor.file", "Pointer image"},
	{"cursor.flags", "Pointer flags"}, {"font.name", "Font"}, {"font.default_fg", "Text colour"},
	{"font.default_bg", "Background colour"}, {"font.mouseover_fg", "Mouse-over text colour"},
	{"font.mouseover_bg", "Mouse-over background"}, {"font.selected_fg", "Selected text colour"},
	{"font.selected_bg", "Selected background"}, {"font.disabled_fg", "Disabled text colour"},
	{"font.disabled_bg", "Disabled background"}, {"string", "Text"}, {"string.type", "Text is"},
	{"string.justify", "Horizontal alignment"}, {"string.vjustify", "Vertical alignment"},
	{"string.edge", "Edge padding"}, {"string.wrap", "Wrap lines"}, {"string.value", "Text"},
	{"toggle_string", "Toggle text"}, {"toggle_string.type", "Text is"}, {"toggle_string.value", "Text"},
	{"items", "Items"}, {"items.multiselect", "Multiple selection"}, {"items.justify", "Horizontal alignment"},
	{"items.vjustify", "Vertical alignment"}, {"list_box", "List box"},
	{"list_box.sb_edge_pad", "Scrollbar edge padding"}, {"spinup", "Spin up"}, {"spindown", "Spin down"},
	{"column.count", "Columns"}, {"column.spacing", "Column spacing"}, {"column.primary_sort", "Primary sort column"},
	{"column.primary_sort_token", "Primary sort written as"}, {"column.secondary_sort", "Secondary sort column"},
	{"column.tertiary_sort", "Tertiary sort column"}, {"min_item_height", "Minimum row height"},
	{"fixed_header_height", "Header height"}, {"scrollbar", "Scrollbar"},
};
// The groups a window's dotted paths form.
const PathLabel kWindowSections[] = {
	{"position", "Position"}, {"frame", "Frame"}, {"cursor", "Pointer"}, {"font", "Font and colours"},
	{"string", "Text"}, {"toggle_string", "Toggle text"}, {"items", "Items"}, {"list_box", "List box"},
	{"spinup", "Spin up"}, {"spindown", "Spin down"}, {"column", "Table columns"}, {"scrollbar", "Scrollbar"},
};
const PathLabel kScreenLabels[] = {{"name", "Name"}, {"music_var", "Music"}};
const PathLabel kAppearanceLabels[] = {{"state", "State"}, {"type", "Kind"}, {"value", "Image or colour"},
                                       {"map_state", "Sprite row"}, {"height", "Frame height"}, {"flags", "Texture flags"}};
const PathLabel kSoundLabels[] = {{"state", "When"}, {"trigger", "Sound"}, {"file", "Sound bank"}};
const PathLabel kActionLabels[] = {{"type", "Action"}, {"state", "Window state"}, {"file", "Menu file"},
                                   {"field", "Source control"}, {"field_attr", "Source written as"},
                                   {"target_form", "Target form"}, {"toggle", "Toggle"}, {"test", "Test"},
                                   {"target", "Target"}, {"external_browser", "Open in the web browser"}};
const PathLabel kHotkeyLabels[] = {{"value", "Key"}, {"virtual", "Virtual key"}};
const PathLabel kDatasourceLabels[] = {{"value", "File"}};
const PathLabel kItemLabels[] = {{"type", "Kind"}, {"value", "Value"}, {"text", "Text"},
                                 {"justify", "Horizontal alignment"}, {"vjustify", "Vertical alignment"},
                                 {"pairs_list", "Pairs list"}, {"column", "Column"}};
const PathLabel kHeaderLabels[] = {{"justify", "Horizontal alignment"}, {"vjustify", "Vertical alignment"},
                                   {"column", "Column"}, {"sort", "Sort"}, {"width", "Width"}, {"type", "Text is"},
                                   {"text", "Text"}};
const PathLabel kBodyLabels[] = {{"justify", "Horizontal alignment"}, {"vjustify", "Vertical alignment"},
                                 {"column", "Column"}, {"display", "Draw"}, {"custom_draw", "Custom draw"},
                                 {"bitmap_draw", "Bitmap"}, {"bitmap_text", "Bitmap and text"},
                                 {"bitmap_flags", "Bitmap flags"}, {"scale_bitmap", "Scale the bitmap"}};
const PathLabel kSubstLabels[] = {{"column", "Column"}, {"value", "Value"}, {"is_url", "Is a web address"},
                                  {"is_file", "Is a file"}, {"file", "Substitute"}};
const PathLabel kElementLabels[] = {{"tag", "Tag"}, {"text", "Text"}};
const PathLabel kAttributeLabels[] = {{"name", "Name"}, {"value", "Value"}};

template <size_t N> const char *find_label(const PathLabel (&table)[N], const std::string &path) {
	for (const PathLabel &entry : table)
		if (path == entry.path) return entry.label;
	return "";
}

const char *field_label(SchemaShape shape, const std::string &path) {
	switch (shape) {
	case SchemaShape::Screen: return find_label(kScreenLabels, path);
	case SchemaShape::Window:
	case SchemaShape::Part: return find_label(kWindowLabels, path);
	case SchemaShape::Appearance: return find_label(kAppearanceLabels, path);
	case SchemaShape::Sound: return find_label(kSoundLabels, path);
	case SchemaShape::Action: return find_label(kActionLabels, path);
	case SchemaShape::Hotkey: return find_label(kHotkeyLabels, path);
	case SchemaShape::Datasource: return find_label(kDatasourceLabels, path);
	case SchemaShape::Item: return find_label(kItemLabels, path);
	case SchemaShape::Row: return "";
	case SchemaShape::Header: return find_label(kHeaderLabels, path);
	case SchemaShape::Body: return find_label(kBodyLabels, path);
	case SchemaShape::Subst: return find_label(kSubstLabels, path);
	case SchemaShape::Element: return find_label(kElementLabels, path);
	case SchemaShape::Attribute: return find_label(kAttributeLabels, path);
	}
	return "";
}

// The group heading of a window field: its path's first step's ("" for one outside any
// group; a block's toggle is named as its group).
const char *section_label(SchemaShape shape, const std::string &path) {
	return is_window_shape(shape) ? find_label(kWindowSections, path.substr(0, path.find('.'))) : "";
}

// Whether a text field holds prose that may run over several lines.
bool multiline(SchemaShape shape, const std::string &path) {
	if (is_window_shape(shape)) return path == "string.value" || path == "toggle_string.value" || path == "private_data";
	return (shape == SchemaShape::Item || shape == SchemaShape::Header || shape == SchemaShape::Element) && path == "text";
}

// The readable name of a choice: the token's meaning, the token itself the tooltip. The
// verbs follow docs/mnu/menu-re.md "Authored Actions"; a token with no plainer name keeps
// its own ("" = the token).
struct ChoiceLabel {
	const char *field; // a field's path, or the last step of one ("justify")
	const char *token;
	const char *label;
};
const ChoiceLabel kWindowChoices[] = {
	{"type", "window", "Window (generic)"}, {"type", "static", "Static"}, {"type", "button", "Button"},
	{"type", "edit", "Text box"}, {"type", "multiline_edit", "Multi-line text box"}, {"type", "list", "List"},
	{"type", "checkbox", "Check box"}, {"type", "radio", "Radio button"}, {"type", "combobox", "Drop-down list"},
	{"type", "scroll", "Scrollbar"}, {"type", "table", "Table"}, {"type", "spinlist", "Spin list"},
	{"type", "marquee_wnd", "Marquee"}, {"type", "glb_table", "Server table"},
	{"type", "radioedit", "Radio button with a text box"}, {"type", "lan_list", "LAN game list"},
	{"type", "gopher", "Gopher browser"},
	{"orientation", "", "Not set"}, {"orientation", "HORIZONTAL", "Horizontal"}, {"orientation", "VERTICAL", "Vertical"},
	{"type", "ID", "A string id"}, {"type", "", "Plain text"},
	{"column.primary_sort_token", "", "Not set (PRIMARY_SORT)"}, {"cursor.flags", "", "Not written"},
};
const ChoiceLabel kAlignLabels[] = {
	{"justify", "", "Not set"}, {"justify", "LEFT", "Left"}, {"justify", "CENTER", "Centre"},
	{"justify", "RIGHT", "Right"}, {"vjustify", "", "Not set"}, {"vjustify", "TOP", "Top"},
	{"vjustify", "CENTER", "Middle"}, {"vjustify", "BOTTOM", "Bottom"},
};
const ChoiceLabel kAppearanceChoices[] = {
	{"state", "DEFAULT", "Normal"}, {"state", "DISABLED", "Disabled"}, {"state", "MOUSEOVER", "Mouse over"},
	{"state", "SELECTED", "Selected"}, {"type", "", "None (marks the state)"}, {"type", "IMAGE", "Image"},
	{"type", "COLOR", "Colour"}, {"type", "CUSTOM", "Custom"}, {"type", "OUTLINE", "Outline"},
	{"type", "IMAGEROW", "Image row"}, {"flags", "", "Not written"},
};
const ChoiceLabel kSoundChoices[] = {
	{"state", "MOUSEIN", "Mouse enters"}, {"state", "MOUSEOUT", "Mouse leaves"}, {"state", "SELECTED", "Selected"},
};
const ChoiceLabel kActionChoices[] = {
	{"type", "SCREEN", "Go to a screen"}, {"type", "WINDOW", "Show, hide, enable or disable a window"},
	{"type", "URL", "Open a web page"}, {"type", "FORM_POST", "Post the login form"},
	{"type", "GLB_LOAD", "Server list: load"}, {"type", "GLB_LOADANDPING", "Server list: load and ping"},
	{"type", "GLB_FILTER", "Server list: filter by text"}, {"type", "GLB_FILTER_NUM", "Server list: filter by number"},
	{"type", "GLB_PING", "Server list: ping"}, {"type", "GLB_JOIN", "Server list: join"},
	{"type", "TAB", "Tab to a control"}, {"type", "POP_SCREEN", "Go back"},
	{"type", "APPMSG", "Application message"}, {"type", "LAN_SEARCH", "LAN: search"},
	{"type", "LAN_JOIN", "LAN: join"}, {"type", "MNX", "MNX service"},
	{"state", "", "Not set"}, {"state", "HIDE", "Hide"}, {"state", "SHOW", "Show"}, {"state", "ENABLE", "Enable"},
	{"state", "DISABLE", "Disable"}, {"test", "", "Not set (less than)"}, {"test", "LT", "Less than"},
	{"test", "LE", "At most"}, {"test", "EQ", "Equal to"}, {"test", "GE", "At least"}, {"test", "GT", "Greater than"},
	{"field_attr", "", "Not set (FIELD)"},
};
const ChoiceLabel kItemChoices[] = {
	{"type", "", "Plain text"}, {"type", "ID", "A string id"}, {"type", "IMAGE", "Image"},
	{"type", "COLOR", "Colour"}, {"type", "BITMAP", "Bitmap"},
};
const ChoiceLabel kHeaderChoices[] = {{"type", "", "Plain text"}, {"type", "ID", "A string id"}};
const ChoiceLabel kBodyChoices[] = {
	{"display", "", "Not set"}, {"display", "CUSTOM_DRAW", "Custom draw"}, {"display", "BITMAP_DRAW", "Bitmap"},
	{"display", "BITMAP_TEXT", "Bitmap and text"},
};

template <size_t N> const char *find_choice(const ChoiceLabel (&table)[N], const std::string &field, const char *token) {
	for (const ChoiceLabel &entry : table)
		if (field == entry.field && std::strcmp(token, entry.token) == 0) return entry.label;
	return "";
}

const char *choice_label(SchemaShape shape, const std::string &path, const char *token) {
	const std::string step = path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1);
	if (step == "justify" || step == "vjustify") return find_choice(kAlignLabels, step, token);
	switch (shape) {
	case SchemaShape::Window:
	case SchemaShape::Part:
		// STRING's and TOGGLE_STRING's TYPE: plain text or a string id, beside the window's own.
		return find_choice(kWindowChoices, path == "string.type" || path == "toggle_string.type" ? "type" : path, token);
	case SchemaShape::Appearance: return find_choice(kAppearanceChoices, path, token);
	case SchemaShape::Sound: return find_choice(kSoundChoices, path, token);
	case SchemaShape::Action: return find_choice(kActionChoices, path, token);
	case SchemaShape::Item: return find_choice(kItemChoices, path, token);
	case SchemaShape::Header: return find_choice(kHeaderChoices, path, token);
	case SchemaShape::Body: return find_choice(kBodyChoices, path, token);
	default: return "";
	}
}

// The property table's fields as the neutral schema: a flag is a yes / no integer, a
// Bit field is optional, a field whose reference a sibling decides references nothing
// until field_on resolves it for one record; each with the name, the group and the choice
// names the editor shows.
const std::vector<FieldSchema> &fields_of(SchemaShape shape) {
	static const std::vector<std::vector<FieldSchema>> tables = [] {
		std::vector<std::vector<FieldSchema>> out;
		for (int s = 0; s <= int(SchemaShape::Attribute); ++s) {
			std::vector<FieldSchema> fields;
			for (const mnu::SchemaField &field : mnu::schema_fields(SchemaShape(s))) {
				FieldSchema schema;
				schema.id = field.path;
				schema.type = field.type == mnu::SchemaType::Text ? FieldType::Text : FieldType::Integer;
				schema.width = field.width;
				schema.reference = reference_of(field.reference);
				if (field.type == mnu::SchemaType::Flag) schema.choices = yes_no();
				for (const mnu::SchemaChoice &choice : field.choices)
					schema.choices.push_back({choice.name, choice.value, choice_label(SchemaShape(s), field.path, choice.name)});
				schema.open_choices = field.open;
				schema.color = colour_of(schema.reference);
				schema.optional = field.presence == mnu::SchemaPresence::Bit;
				// A screen's and a window's NAME is what the by-name lookups find them by
				// (lookup_names); field_on takes it from a part, whose NAME no lookup reads.
				if (std::strcmp(field.path, "name") == 0) {
					if (SchemaShape(s) == SchemaShape::Screen) schema.defines = ReferenceKind::MenuScreen;
					if (is_window_shape(SchemaShape(s))) schema.defines = ReferenceKind::MenuWindow;
				}
				schema.label = field_label(SchemaShape(s), field.path);
				schema.section = section_label(SchemaShape(s), field.path);
				schema.multiline = field.type == mnu::SchemaType::Text && multiline(SchemaShape(s), field.path);
				fields.push_back(std::move(schema));
			}
			out.push_back(std::move(fields));
		}
		return out;
	}();
	return tables[size_t(shape)];
}

// --- where a record sits ---------------------------------------------------------------

// A record as the paths read it: the nearest window or part at or above it (a window's
// own), the element path from that window to the record's list ("" for a window or a
// part), and whether the lists above it are read and written.
struct Context {
	const mnu::Window *window = nullptr;
	std::string prefix;
	SchemaApplies applies = SchemaApplies::Reads;
	bool present = true;
	// The root window it hangs under, and the window whose TEXT_RSRC its string ids fall
	// back to (the root, or for a part parsed before it is attached the part itself:
	// menu::window_text_rsrc).
	const mnu::Window *root = nullptr;
	const mnu::Window *text_fallback = nullptr;
};

Context root_context(const mnu::Window &root) {
	Context c;
	c.window = &root;
	c.root = &root;
	c.text_fallback = &root;
	return c;
}

std::string list_path(const Context &owner, const mnu::SchemaList &list) {
	return owner.prefix.empty() ? std::string(list.path) : owner.prefix + "." + list.path;
}

// Whether the owner's window type reads one of the owner's lists.
SchemaApplies list_applies(const Context &owner, const mnu::SchemaList &list) {
	return mnu::schema_applies_both(owner.applies, mnu::schema_reads(owner.window->type, list_path(owner, list)));
}

Document::CollectionSpec list_spec(const Context &owner, const mnu::SchemaList &list) {
	return {kind_of(list.path), list.label, list.name_field, false,
	        applicability(list_applies(owner, list)), list.max};
}

Document::CollectionSpec roots_spec() {
	return {kWindow, "Windows", "name", false, Applicability::Reads};
}

// The context of the record at `index` of the owner's list.
Context step_into(const Context &owner, const SchemaRecord &owner_record, size_t list, const SchemaRecord &record) {
	const mnu::SchemaList &spec = mnu::schema_lists(owner_record.shape)[list];
	Context c;
	c.applies = list_applies(owner, spec);
	if (is_window_shape(owner_record.shape) && std::strcmp(spec.path, "element") == 0)
		c.applies = mnu::schema_applies_both(
		        c.applies, mnu::schema_element_reads(owner.window->type, static_cast<const mnu::Element *>(record.data)->tag));
	c.present = owner.present && mnu::schema_list_present(owner_record, list);
	c.root = owner.root;
	c.text_fallback = owner.text_fallback;
	if (is_window_shape(record.shape)) {
		c.window = static_cast<const mnu::Window *>(record.data);
		// A combo's LIST_BOX is attached before its parse (its own TEXT_RSRC, else the
		// root's); a spin arrow and a scrollbar are parsed before they are attached (their
		// own only) [orig: CComboWnd_ParseXMLDefinition @ 0x65c0d0; CSpinListWnd_Create
		// @ 0x64bc40].
		if (record.shape == SchemaShape::Part)
			c.text_fallback = std::strcmp(spec.path, "list_box") == 0 ? owner.root : c.window;
	} else {
		c.window = owner.window;
		c.prefix = list_path(owner, spec);
	}
	return c;
}

SchemaRecord root_record(const MenuScreen &screen, size_t index) {
	return mnu::schema_window(const_cast<mnu::Window &>(screen.screen.roots[index]));
}

// A nested record found by identity, with its owner (the screen for a root window).
struct Located {
	SchemaRecord record;
	RecordIds *ids = nullptr;
	Context context;
	SchemaRecord owner;           // shape Screen for a root window
	RecordIds *owner_ids = nullptr; // null for a root window
	Context owner_context;        // unset for a root window
	size_t list = kRoots;
	size_t index = 0;
};

// A nested record found by its path in the screen (Document::path_in): the root's index (the
// screen's one collection), then each list and index down to it.
bool locate_steps(const MenuScreen &screen, const Document::PathStep *steps, size_t size, Located &out) {
	if (!size || steps[0].index >= screen.roots.size() || steps[0].index >= screen.screen.roots.size())
		return false;
	MenuScreen &mutable_screen = const_cast<MenuScreen &>(screen);
	out = Located();
	out.owner = mnu::schema_screen(mutable_screen.screen);
	out.index = steps[0].index;
	out.record = root_record(screen, steps[0].index);
	out.ids = &mutable_screen.roots[steps[0].index];
	out.context = root_context(screen.screen.roots[steps[0].index]);
	for (size_t i = 1; i < size; ++i) {
		const size_t list = steps[i].collection, index = steps[i].index;
		const SchemaRecord next = mnu::schema_list_at(out.record, list, index);
		if (!next || list >= out.ids->lists.size() || index >= out.ids->lists[list].size()) return false;
		out.owner = out.record;
		out.owner_ids = out.ids;
		out.owner_context = out.context;
		out.list = list;
		out.index = index;
		out.context = step_into(out.owner_context, out.owner, out.list, next);
		out.record = next;
		out.ids = &out.ids->lists[list][index];
	}
	return true;
}

// A nested record of `screen` (a row `document` holds, or a batch's version of one) by identity.
bool locate(const Document &document, const MenuScreen &screen, NodeId id, Located &out) {
	const Document::RecordPath path = document.path_in(screen, id);
	return locate_steps(screen, path.begin(), path.size(), out);
}

bool locate(const Document &document, const NodeAddress &address, Located &out) {
	if (!address.child || address.kind == kScreen) return false;
	const Node *node = document.row(address.row);
	if (!node || node->kind != kScreen) return false;
	return locate(document, screen_of(*node), address.child, out);
}

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

// Fresh identities for the record at `index` of the owner's list, inserted beside it.
void insert_ids(std::vector<RecordIds> &ids, size_t index, const SchemaRecord &record, const Document::IdAllocator &allocate) {
	RecordIds fresh = shape_ids(record);
	for_each_identity(fresh, [&](NodeId &id) { id = allocate(); });
	ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(std::min(index, ids.size())), std::move(fresh));
}

// The window list an owner takes windows into: a window's or a part's children, the
// screen's roots.
bool window_list_of(const SchemaRecord &owner, size_t &list) {
	if (owner.shape == SchemaShape::Screen) { list = 0; return true; }
	if (is_window_shape(owner.shape)) { list = children_list(); return true; }
	return false;
}

// The document windows of a screen in pre-order (the roots and their children; never a
// part or what a part holds), each with its identity.
void walk_document_windows(const MenuScreen &screen, const std::function<bool(const RecordIds &)> &visit) {
	const size_t children = children_list();
	std::function<bool(const RecordIds &)> step = [&](const RecordIds &ids) {
		if (!visit(ids)) return false;
		for (const RecordIds &child : ids.lists[children])
			if (!step(child)) return false;
		return true;
	};
	for (const RecordIds &root : screen.roots)
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
bool window_keeps(const SchemaRecord &record, std::string &error) {
	if (record.shape == SchemaShape::Attribute) {
		const auto &attribute = *static_cast<const mnu::ElementAttribute *>(record.data);
		if (!mnu::known_token(attribute.name, mnu::kExtraAttributes)) { error = kWindowAttributes; return false; }
		if (attribute.has_value) { error = kFlagsTakeNoValue; return false; }
	} else if (record.shape == SchemaShape::Element) {
		if (!mnu::known_token(static_cast<const mnu::Element *>(record.data)->tag, mnu::kExtraTags)) {
			error = window_elements_error();
			return false;
		}
	}
	return true;
}

} // namespace

// --- the row -------------------------------------------------------------------------

NodeKind menu_kind(const std::string &token) { return kind_of(token.c_str()); }

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

size_t ids_content(const RecordIds &ids) {
	size_t bytes = footprint_of(ids.lists);
	for (const std::vector<RecordIds> &list : ids.lists) {
		bytes += footprint_of(list);
		for (const RecordIds &item : list) bytes += ids_content(item);
	}
	return bytes;
}

} // namespace

size_t MenuScreen::footprint() const {
	size_t bytes = sizeof(MenuScreen) + collections_footprint() + footprint_of(screen.name) +
	               list_content(screen.roots) + footprint_of(roots);
	for (const RecordIds &root : roots) bytes += ids_content(root);
	return bytes;
}

std::shared_ptr<Node> MenuScreen::clone() const { return std::make_shared<MenuScreen>(*this); }

void MenuScreen::for_each_identity(const std::function<void(NodeId &)> &fn) {
	for (RecordIds &root : roots) editor::for_each_identity(root, fn);
}

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

const std::vector<RecordKindRow> &MnuDocument::kinds() const {
	static const std::vector<RecordKindRow> table = [] {
		std::vector<RecordKindRow> out;
		const std::vector<KindEntry> &entries = kind_entries();
		for (size_t i = 0; i < entries.size(); ++i) {
			const bool screen = i == size_t(kScreen);
			const char *add = screen ? "Add screen" : "";
			out.push_back({NodeKind(i), entries[i].token, entries[i].label, add, screen});
		}
		return out;
	}();
	return table;
}

std::vector<Document::Collection> MnuDocument::collections(const Node &row, const NodeAddress &owner) const {
	if (row.kind != kScreen) return {};
	const MenuScreen &screen = screen_of(row);
	if (!owner.child) {
		Collection roots{roots_spec(), {}};
		for (const RecordIds &root : screen.roots) roots.ids.push_back(root.id);
		return {roots};
	}
	Located at;
	if (!locate(*this, screen, owner.child, at)) return {};
	std::vector<Collection> out;
	const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(at.record.shape);
	for (size_t list = 0; list < lists.size(); ++list) {
		Collection collection{list_spec(at.context, lists[list]), {}};
		for (const RecordIds &ids : at.ids->lists[list]) collection.ids.push_back(ids.id);
		out.push_back(std::move(collection));
	}
	return out;
}

// One pass over the screen's records in file order, the identities read beside them.
void MnuDocument::walk_records(const Node &row, const RecordVisitor &visit) const {
	if (row.kind != kScreen) return;
	const MenuScreen &screen = screen_of(row);
	std::function<bool(const SchemaRecord &, const RecordIds &, const NodeAddress &, const Context &)> step =
	        [&](const SchemaRecord &record, const RecordIds &ids, const NodeAddress &self, const Context &context) {
		        const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(record.shape);
		        for (size_t list = 0; list < lists.size() && list < ids.lists.size(); ++list) {
			        const CollectionSpec spec = list_spec(context, lists[list]);
			        for (size_t i = 0; i < ids.lists[list].size(); ++i) {
				        const SchemaRecord child = mnu::schema_list_at(record, list, i);
				        if (!child) return false;
				        const RecordIds &child_ids = ids.lists[list][i];
				        const NodeAddress address{row.id, spec.kind, child_ids.id};
				        if (!visit(address, Placement{self, spec, i, list})) return false;
				        if (!step(child, child_ids, address, step_into(context, record, list, child))) return false;
			        }
		        }
		        return true;
	        };
	const NodeAddress top{row.id, kScreen, 0};
	const CollectionSpec roots = roots_spec();
	for (size_t i = 0; i < screen.roots.size() && i < screen.screen.roots.size(); ++i) {
		const NodeAddress address{row.id, kWindow, screen.roots[i].id};
		if (!visit(address, Placement{top, roots, i, 0})) return;
		if (!step(root_record(screen, i), screen.roots[i], address, root_context(screen.screen.roots[i]))) return;
	}
}

const std::vector<FieldSchema> &MnuDocument::schema(NodeKind kind) {
	static const std::vector<FieldSchema> none;
	if (kind < 0 || size_t(kind) >= kind_entries().size()) return none;
	return fields_of(kind_entries()[size_t(kind)].shape);
}

void MnuDocument::refine_field(const NodeAddress &address, FieldUse &out) const {
	const FieldSchema &field = *out.schema;
	// A screen's NAME is looked up in its file, a window's on its screen (lookup_names).
	if (out.defines == ReferenceKind::MenuScreen) out.scope = menu_screen_scope(Document::path());
	Located at;
	if (!locate(*this, address, at)) return;
	const std::string path = at.context.prefix.empty() ? field.id : at.context.prefix + "." + field.id;
	SchemaApplies applies = mnu::schema_applies_both(at.context.applies, mnu::schema_reads(at.context.window->type, path));
	if (at.record.shape == SchemaShape::Action)
		applies = mnu::schema_applies_both(applies, mnu::schema_action_reads(*static_cast<const mnu::Action *>(at.record.data), field.id));
	out.applies = applicability(applies);
	const mnu::SchemaField *schema = mnu::schema_field(at.record.shape, field.id);
	if (schema && schema->reference == mnu::SchemaReference::Dynamic) {
		out.reference = reference_of(mnu::schema_reference(at.record, field.id));
		out.color = colour_of(out.reference);
	}
	// A string id resolves in the "menu" section of the table the window reads: its own
	// TEXT_RSRC, else the one it falls back to (the runtime's rule, menu_screen_inputs.h).
	// With neither, the scope names no table: the game shows the id.
	if (out.reference == ReferenceKind::TextId) {
		const std::string *table = menu::window_text_rsrc(*at.context.window, at.context.text_fallback);
		out.scope = menu_text_scope(table ? *table : std::string());
	}
	// A SCREEN target is a screen of the file the ACTION loads first (no FILE names none: a
	// write issue, retail's fault); every other target a window of the acting window's own
	// screen, looked up by that screen's NAME (lookup_names).
	if (out.reference == ReferenceKind::MenuScreen) {
		const std::string &file = static_cast<const mnu::Action *>(at.record.data)->file;
		if (file.empty()) out.reference = ReferenceKind::None;
		else out.scope = menu_screen_scope(file);
	} else if (out.reference == ReferenceKind::MenuWindow) {
		out.scope = menu_window_scope(Document::path(), screen_of(*row(address.row)).screen.name);
	}
	// A part is named by its owner when retail makes it: no lookup finds it by its NAME.
	if (at.record.shape == SchemaShape::Part) out.defines = ReferenceKind::None;
	if (out.defines == ReferenceKind::MenuWindow)
		out.scope = menu_window_scope(Document::path(), screen_of(*row(address.row)).screen.name);
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

bool MnuDocument::read_present(const Node &row, const NodeAddress &address, const std::string &field) const {
	if (row.kind != kScreen) return false;
	if (!address.child) {
		if (field.empty()) return true;
		return mnu::schema_present(mnu::schema_screen(const_cast<mnu::Screen &>(screen_of(row).screen)), field);
	}
	Located at;
	if (address.kind == kScreen || !locate(*this, screen_of(row), address.child, at)) return false;
	if (field.empty()) return at.context.present;
	return mnu::schema_present(at.record, field);
}

bool MnuDocument::read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const {
	if (row.kind != kScreen) return false;
	if (!address.child) {
		if (address.kind != kScreen) return false;
		return mnu::schema_get(mnu::schema_screen(const_cast<mnu::Screen &>(screen_of(row).screen)), field, out);
	}
	Located at;
	return address.kind != kScreen && locate(*this, screen_of(row), address.child, at) &&
	       mnu::schema_get(at.record, field, out);
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
	if (!window || !locate(*this, screen_of(screen), window, at)) return {};
	if (list >= at.ids->lists.size() || index >= at.ids->lists[list].size()) return {};
	const mnu::SchemaList &spec = mnu::schema_lists(at.record.shape)[list];
	return {screen.id, kind_of(spec.path), at.ids->lists[list][index].id};
}

bool MnuDocument::identities_match() const {
	for (const auto &node : rows()) {
		const MenuScreen &screen = screen_of(*node);
		if (screen.roots.size() != screen.screen.roots.size()) return false;
		for (size_t i = 0; i < screen.roots.size(); ++i)
			if (!ids_match(root_record(screen, i), screen.roots[i])) return false;
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
	const size_t children = children_list();
	std::vector<size_t> parts; // the part lists, in the order the parts attach
	const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(SchemaShape::Window);
	for (size_t i = 0; i < lists.size(); ++i)
		if (lists[i].shape == SchemaShape::Part) parts.push_back(i);
	for (const auto &node : rows()) {
		const MenuScreen &screen = screen_of(*node);
		const bool shadowed = newest[strutil::to_upper(screen.screen.name)] != node->id;
		std::map<std::string, NodeAddress> first; // the window each NAME finds on this screen
		// [orig: CWnd_FindChildByName @ 0x646850: a window with no NAME returns nothing,
		// its own NAME is compared, then its children in order]
		std::function<void(const SchemaRecord &, const RecordIds &, bool, bool)> visit =
		        [&](const SchemaRecord &record, const RecordIds &ids, bool reached, bool part) {
			        const auto &window = *static_cast<const mnu::Window *>(record.data);
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
			        if (children < ids.lists.size())
				        for (size_t i = 0; i < ids.lists[children].size(); ++i)
					        visit(mnu::schema_list_at(record, children, i), ids.lists[children][i], open, false);
			        // A part carries the name its owner gives it, so the search goes on inside.
			        for (const size_t list : parts)
				        if (list < ids.lists.size() && !ids.lists[list].empty() && mnu::schema_list_present(record, list))
					        visit(mnu::schema_list_at(record, list, 0), ids.lists[list][0], open, true);
		        };
		for (size_t i = 0; i < screen.roots.size() && i < screen.screen.roots.size(); ++i)
			visit(root_record(screen, i), screen.roots[i], true, false);
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
		result.issues.push_back({true, 0, "", "", error});
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
				saved_.issues.push_back({true, 0, std::string(), std::string(), "The game cannot read the menu back: " + error});
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
	const Node *node = row(records.front().row);
	if (!node || node->kind != kScreen) return std::string();
	const MenuScreen &screen = screen_of(*node);
	std::set<NodeId> wanted;
	for (const NodeAddress &record : records) {
		if (record.row != node->id || record.kind != kWindow || !record.child) return std::string();
		wanted.insert(record.child);
	}
	// The windows in document order: the clipboard's roots. The walk meets every window
	// (the roots, the children and the windows a part holds, not a part itself); a selected
	// window's windows come with it, so a selected one inside it is covered.
	mnu::Document clip;
	if (const auto *state = dynamic_cast<const MenuFileState *>(file_state())) clip.source_encoding = state->source_encoding;
	clip.screens.emplace_back();
	clip.screens.back().name = "CLIPBOARD";
	size_t found = 0;
	std::function<void(const SchemaRecord &, const RecordIds &, bool, bool)> visit =
	        [&](const SchemaRecord &record, const RecordIds &ids, bool window, bool covered) {
		        const bool selected = window && wanted.count(ids.id) != 0;
		        if (selected) {
			        ++found;
			        if (!covered) clip.screens.back().roots.push_back(*static_cast<const mnu::Window *>(record.data));
		        }
		        const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(record.shape);
		        for (size_t list = 0; list < lists.size() && list < ids.lists.size(); ++list) {
			        if (!is_window_shape(lists[list].shape)) continue;
			        for (size_t i = 0; i < ids.lists[list].size(); ++i)
				        visit(mnu::schema_list_at(record, list, i), ids.lists[list][i],
				              lists[list].shape == SchemaShape::Window, covered || selected);
		        }
	        };
	for (size_t i = 0; i < screen.roots.size(); ++i) visit(root_record(screen, i), screen.roots[i], true, false);
	if (found != wanted.size()) return std::string();
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
		for (mnu::Window &root : row->screen.roots) row->roots.push_back(shape_ids(mnu::schema_window(root)));
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
	row->roots.push_back(shape_ids(mnu::schema_window(row->screen.roots.back())));
	return row;
}

// --- editing ---------------------------------------------------------------------------

bool MnuDocument::set_field(Node &node, const NodeAddress &address, const std::string &field, const Value &value,
                            std::string &error) {
	MenuScreen &screen = screen_of(node);
	if (!address.child) return mnu::schema_set(mnu::schema_screen(screen.screen), field, value, error);
	Located at;
	if (!locate(*this, screen, address.child, at)) { error = "The record no longer exists."; return false; }
	// A name the reader would not keep here is refused: a window keeps only its PLAYERLIST
	// and SERVERLIST attributes, and only the extra elements its parses read at its top level.
	if (is_window_shape(at.owner.shape) && at.record.shape == SchemaShape::Attribute && field == "name") {
		if (!text_width(value, 64, error)) return false;
		if (!mnu::known_token(std::get<std::string>(value), mnu::kExtraAttributes)) {
			error = kWindowAttributes;
			return false;
		}
	}
	if (is_window_shape(at.owner.shape) && at.record.shape == SchemaShape::Attribute && field == "value") {
		Value current;
		if (!mnu::schema_get(at.record, field, current) || current != value) { error = kFlagsTakeNoValue; return false; }
	}
	if (is_window_shape(at.owner.shape) && at.record.shape == SchemaShape::Element && field == "tag") {
		if (!text_width(value, 64, error)) return false;
		if (!mnu::known_token(std::get<std::string>(value), mnu::kExtraTags)) {
			error = window_elements_error();
			return false;
		}
	}
	return mnu::schema_set(at.record, field, value, error);
}

// Clear and Write flip the presence bit (the authored state) and leave the latent value in
// place: a field written again reads what it read while left out.
bool MnuDocument::set_present(Node &node, const NodeAddress &address, const std::string &field, bool present,
                              std::string &error) {
	MenuScreen &screen = screen_of(node);
	if (!address.child) return mnu::schema_set_present(mnu::schema_screen(screen.screen), field, present, error);
	Located at;
	if (!locate(*this, screen, address.child, at)) { error = "The record no longer exists."; return false; }
	if (present && is_window_shape(at.owner.shape) && at.record.shape == SchemaShape::Attribute && field == "value") {
		error = kFlagsTakeNoValue;
		return false;
	}
	return mnu::schema_set_present(at.record, field, present, error);
}

bool MnuDocument::edit_collection(Node &node, const Edit &edit, const IdAllocator &allocate, NodeId &added,
                                  std::string &error) {
	MenuScreen &screen = screen_of(node);
	const SchemaRecord screen_record = mnu::schema_screen(screen.screen);
	// The list of `kind` an owner holds: a located record's, or (null) the screen's roots.
	auto list_of = [&](const Located *at, NodeKind kind, SchemaRecord &owner, std::vector<RecordIds> *&ids,
	                   size_t &list) {
		if (!at) {
			if (kind != kWindow) { error = "A screen holds windows only."; return false; }
			owner = screen_record;
			ids = &screen.roots;
			list = 0;
			return true;
		}
		const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(at->record.shape);
		for (size_t i = 0; i < lists.size(); ++i) {
			if (kind_of(lists[i].path) != kind) continue;
			owner = at->record;
			ids = &at->ids->lists[i];
			list = i;
			return true;
		}
		error = "This record holds no such records.";
		return false;
	};
	// The owner a record goes into (0: the screen's roots), and its list of `kind`.
	auto owner_list = [&](NodeId parent, NodeKind kind, SchemaRecord &owner, std::vector<RecordIds> *&ids,
	                      size_t &list) {
		if (!parent) return list_of(nullptr, kind, owner, ids, list);
		Located at;
		if (!locate(*this, screen, parent, at)) { error = "The record to add into no longer exists."; return false; }
		return list_of(&at, kind, owner, ids, list);
	};
	SchemaRecord owner;
	std::vector<RecordIds> *ids = nullptr;
	size_t list = 0;
	if (edit.operation == EditOperation::Add) {
		if (!owner_list(edit.parent, edit.address.kind, owner, ids, list)) return false;
		const size_t position = std::min(edit.position, mnu::schema_list_size(owner, list));
		if (!mnu::schema_list_insert(owner, list, position, nullptr, error)) return false;
		const SchemaRecord made = mnu::schema_list_at(owner, list, position);
		if (edit.address.kind == kWindow) {
			// New windows are named uniquely within their screen.
			mnu::Window &window = *static_cast<mnu::Window *>(made.data);
			window.name = unique_name(screen_names(screen.screen), "WINDOW1");
		}
		insert_ids(*ids, position, made, allocate);
		added = (*ids)[position].id;
		return true;
	}
	Located at;
	if (!locate(*this, screen, edit.address.child, at)) { error = "The record no longer exists."; return false; }
	std::vector<RecordIds> &source_ids = at.list == kRoots ? screen.roots : at.owner_ids->lists[at.list];
	const size_t source_list = at.list == kRoots ? 0 : at.list;
	switch (edit.operation) {
	case EditOperation::Duplicate: {
		mnu::SchemaDetached copy = mnu::schema_list_copy(at.owner, source_list, at.index);
		if (!copy.data) { error = "The record no longer exists."; return false; }
		if (copy.shape == SchemaShape::Window) {
			std::set<std::string> taken = screen_names(screen.screen);
			make_names_unique(*static_cast<mnu::Window *>(copy.data.get()), taken);
		}
		const size_t position = std::min(edit.position, mnu::schema_list_size(at.owner, source_list));
		if (!mnu::schema_list_insert(at.owner, source_list, position, &copy, error)) return false;
		insert_ids(source_ids, position, mnu::schema_list_at(at.owner, source_list, position), allocate);
		added = source_ids[position].id;
		return true;
	}
	case EditOperation::Remove:
		if (at.list == kRoots && screen.screen.roots.size() == 1) { error = "A screen keeps at least one root window."; return false; }
		mnu::schema_list_erase(at.owner, source_list, at.index);
		source_ids.erase(source_ids.begin() + static_cast<std::ptrdiff_t>(at.index));
		return true;
	case EditOperation::Move: {
		// The destination is checked before anything moves: a screen keeps a root window, a
		// part goes only where its window has none, and a window takes an attribute or an
		// extra element only as it keeps one (an element's own may be anything).
		const bool leaves_roots = at.list == kRoots && edit.parent != 0;
		if (leaves_roots && screen.screen.roots.size() == 1) { error = "A screen keeps at least one root window."; return false; }
		SchemaRecord destination;
		std::vector<RecordIds> *destination_ids = nullptr;
		size_t destination_list = 0;
		if (!owner_list(edit.parent, edit.address.kind, destination, destination_ids, destination_list)) return false;
		const mnu::SchemaList &spec = mnu::schema_lists(destination.shape)[destination_list];
		if (spec.max && destination.data != at.owner.data && mnu::schema_list_size(destination, destination_list) >= spec.max) {
			error = std::string("That window already holds a ") + spec.record_label + ".";
			return false;
		}
		if (is_window_shape(destination.shape) && !window_keeps(at.record, error)) return false;
		// The destination's path as the edit found it (path_in, the row as the batch has left it so
		// far), then as the record's removal leaves it: a step through the list the record leaves,
		// past the record's index, moves up one. The row's index answers for the row as the edit
		// found it, so the place after the removal is worked out here.
		std::vector<Document::PathStep> to;
		if (edit.parent) {
			const Document::RecordPath from = path_in(screen, edit.address.child);
			const Document::RecordPath there = path_in(screen, edit.parent);
			to.assign(there.begin(), there.end());
			const size_t depth = from.size() - 1; // the record's own step
			bool through = !from.empty() && to.size() > depth;
			for (size_t d = 0; through && d < depth; ++d)
				through = to[d].collection == from[d].collection && to[d].index == from[d].index;
			if (through && to[depth].collection == from[depth].collection && to[depth].index > from[depth].index)
				--to[depth].index;
		}
		// The record and its identities come out; the destination, found by that path, takes them
		// at the position.
		mnu::SchemaDetached moved = mnu::schema_list_copy(at.owner, source_list, at.index);
		RecordIds moved_ids = std::move(source_ids[at.index]);
		mnu::schema_list_erase(at.owner, source_list, at.index);
		source_ids.erase(source_ids.begin() + static_cast<std::ptrdiff_t>(at.index));
		Located there;
		if (edit.parent && !locate_steps(screen, to.data(), to.size(), there)) {
			error = "The destination no longer exists.";
			return false;
		}
		if (!list_of(edit.parent ? &there : nullptr, edit.address.kind, destination, destination_ids, destination_list))
			return false;
		const size_t position = std::min(edit.position, mnu::schema_list_size(destination, destination_list));
		if (!mnu::schema_list_insert(destination, destination_list, position, &moved, error)) return false;
		destination_ids->insert(destination_ids->begin() + static_cast<std::ptrdiff_t>(position), std::move(moved_ids));
		return true;
	}
	default:
		error = "This collection cannot accept that edit.";
		return false;
	}
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
	SchemaRecord owner;
	std::vector<RecordIds> *ids = nullptr;
	size_t list = 0;
	if (!edit.parent) {
		owner = mnu::schema_screen(screen.screen);
		ids = &screen.roots;
	} else {
		Located at;
		if (!locate(*this, screen, edit.parent, at)) {
			error = "The record to paste into no longer exists.";
			return false;
		}
		if (!window_list_of(at.record, list)) { error = "Windows go into a window or a screen."; return false; }
		owner = at.record;
		ids = &at.ids->lists[list];
	}
	std::set<std::string> taken = screen_names(screen.screen);
	size_t position = std::min(edit.position, mnu::schema_list_size(owner, list));
	for (mnu::Window &window : clip.screens[0].roots) {
		make_names_unique(window, taken);
		const mnu::SchemaDetached detached = mnu::schema_detach_window(window);
		if (!mnu::schema_list_insert(owner, list, position, &detached, error)) return false;
		insert_ids(*ids, position, mnu::schema_list_at(owner, list, position), allocate);
		added.push_back((*ids)[position].id);
		++position;
	}
	return true;
}

// Retail's by-name lookups find the last of two screens of one name (docs/mnu/menu-re.md,
// "the last of a name is the one found"), so a copy keeping the original's name would take
// its place for every ACTION naming it: the copy is renamed, compared as the lookups
// compare names.
void MnuDocument::prepare_duplicate(Node &copy,
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
	const NodeKind action_kind = kind_of("action");
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
