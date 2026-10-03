#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/mnu_table.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <editor/project/project_document.h>
#include <formats/mnu/mnu.h>

namespace opennova::menu {
enum class MenuFrameNoteCode : uint16_t;
} // namespace opennova::menu

namespace opennova::editor {

// A menu file (ADR 0046 S6c, S9h): `mnu::Document`'s screens as rows, every record the
// format holds a record here at its own depth, through the menu's table (documents/mnu_table.h,
// rows of the one table shape since S13 D10). A screen holds its root windows; a window holds its
// lists in the order the file writes them (its attributes, hotkeys, actions, appearances, scroll
// parts, data sources, sounds, ITEMS rows, parts, table rows, extra elements) and its child
// windows last; a table row holds its cells, an extra element its attributes and elements.
// The kinds are Screen, Window, and one kind per list, named by the list's element path
// ("action", "items.item", "list_box", "column.header"). A field is named by its element
// path ("position.left", "string.value", "font.default_fg"); which fields and lists a
// window's type reads is the format's witnessed applicability (formats/mnu/mnu_schema.h), which
// the table's fields apply where each record sits (by the records it lies in, menu_context). What
// retail's reader does not read is a
// non-blocking source issue (the reader's notes); what the file cannot hold is a blocking
// serialize issue on the record and field that cause it. Comments do not survive: the parser
// drops them (D-MNU-22).

// A screen: its native screen, and the identities of its root windows and everything they hold
// (TableRow::ids, its one list the roots). Where a record sits (the root's index, then each list and
// index down to it) is its path in the core's index of the row (Document::path_in, S13 D8), never
// the row's own.
struct MenuScreen : TableRow {
	mnu::Screen screen;
	MenuScreen();
	std::shared_ptr<Node> clone() const override;
	std::string name() const override { return screen.name; }
	RecordHandle record() const override;
	// The screen's windows, all they hold and their identities.
	size_t footprint() const override;
};

// A NAME retail's by-name lookups find a record by (docs/mnu/menu-re.md, "Names and the
// lookups"). A screen: the loaded screens are searched newest first, so of two screens of one
// name the later is found [orig: CUIScene_SelectNodeByName @ 0x63b6b0]. A window: on the
// screen of its NAME (found the same way), its root windows in document order, each searched
// pre-order, a window before its children and its children before the windows its parts hold
// (a part attaches after the parse); a window with no NAME ends its branch, and a part is
// never matched by its own NAME (retail names it when it makes it), so of two windows of one
// name the earlier is found [orig: UI_FindScreenControl @ 0x63ae80; CWnd_FindChildByName @
// 0x646850]. The menu runtime's find_control keeps the same first-match rule.
struct MenuLookupName {
	enum class Found {
		Yes,
		LaterScreen,    // a screen: a later screen of the file has its NAME
		EarlierWindow,  // a window: an earlier one of its screen has its NAME
		UnderNameless,  // a window: a window above it has no NAME, which ends the search
		ShadowedScreen, // a window: on a screen a later screen of the file shadows
	};
	NodeAddress address;       // the screen row, or the window
	std::string name;          // as authored
	NodeId screen = 0;         // the screen row it is on
	Found found = Found::Yes;
	NodeAddress found_instead; // what the lookup returns for the NAME (LaterScreen, EarlierWindow)
};

struct MenuFileState : FileState {
	mnu::SourceEncoding source_encoding = mnu::SourceEncoding::CodePage;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<MenuFileState>(*this); }
	size_t footprint() const override { return sizeof(MenuFileState); }
};

class MnuDocument : public TableDocument {
public:
	const RecordTable &table() const override { return menu_table(); }
	// A kind's fields without a document (DocumentType::fields, S13 V3): the table fields()
	// answers, the type's own for the process.
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return menu_table().fields(kind); }
	// A screen or window no by-name lookup returns (lookup_names) is inert.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
	// Windows (with everything they hold) as the menu text of one SCREEN whose roots they
	// are, in document order (of one screen or of several: the screens in the file's order),
	// UTF-8 after a byte order mark (a window selected with a window that holds it comes with
	// that one); "" when a record is not a window or the windows hold a value the format cannot
	// carry back.
	std::string copy(const std::vector<NodeAddress> &records) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<MnuDocument>(*this);
	}

	// The document as the runtime reads it, rebuilt from the rows.
	mnu::Document native() const;
	// The menu the game would read were the document saved now: serialize(), read back by
	// the game's reader, once per state (its load generation and revision: a revision names one
	// state of a load, undo included).
	// Null when it cannot be written (`issues` gets serialize()'s) or does not read back.
	std::shared_ptr<const mnu::Document> saved_image(std::vector<SourceIssue> *issues = nullptr) const;
	// What serialize() makes of the current state, once per state: the text a Save writes and the
	// issues that keep it from writing. The saved image reads the menu back from it; the menu's
	// validation and the render check's variables read it without the read back.
	const SerializeResult &saved_serialization() const;
	// A screen row's position among the rows: the screen at that position of the saved
	// image (screen names may repeat). SIZE_MAX for a row the document does not have.
	size_t screen_position(NodeId row) const;
	// The pre-order index of a window among its screen's windows, roots and children, not
	// the parts or what they hold (-1 for anything else): what the frame compiler and the
	// runtime index number widgets by.
	int window_index(const NodeAddress &address) const;
	// The window at a pre-order index of a screen (a preview hit), or 0.
	NodeId window_at(const Node &screen, size_t preorder) const;
	// The record at `index` of list `list` of the window at `preorder` (a compiler note's
	// record), or an empty address.
	NodeAddress record_at(const Node &screen, size_t preorder, size_t list, size_t index) const;
	// Every row's identities have the shape of its native screen (the invariant every
	// structural edit keeps).
	bool identities_match() const;
	// Every named screen, then every named window of each screen in its lookup's order, each
	// with whether a by-name lookup returns it.
	std::vector<MenuLookupName> lookup_names() const;

protected:
	// Whether the record's window type reads the field (and an ACTION's verb, an extra
	// element's tag), and what it references given the record's other fields.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new screen, named as no screen of `rows` (the rows as its batch has left them) is.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// Whether the window type a list's owner sits under reads the list (the format's per-type
	// parse chains, formats/mnu/mnu_schema.h).
	Applicability list_applies(const Node &row, const Located &owner, size_t list) const override;
	// A text of a code-page menu read as UTF-8 (the model holds the code page's bytes).
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	// A text stored as the model holds it (a code-page menu's in Windows-1252, a character it lacks
	// refused), its width counted in the game's characters (the reader narrows every text to the code
	// page); a name the reader would not keep where the record sits is refused: a window keeps only
	// its PLAYERLIST and SERVERLIST attributes, which take no value, and only the extra elements its
	// parses read at its top level.
	bool set_value(Node &row, const Located &at, size_t field, const Value &value, std::string &error) override;
	bool set_written(Node &row, const Located &at, size_t field, bool present, std::string &error) override;
	// A screen keeps a root window (a Remove of its last, a Move of its last elsewhere); a part goes
	// only where its window has none; a window takes an attribute or an extra element only as it
	// keeps one (an element's own may be anything).
	bool accept_list_edit(const Node &row, const ListChange &change, std::string &error) const override;
	// A duplicated window and every window it holds take names no window of the screen has.
	void prepare_record(const Node &row, const ListChange &change, DetachedRecord &record) const override;
	// A new window is named uniquely WINDOW<n> within its screen.
	void after_add(Node &row, const ListChange &change, const RecordHandle &made) override;
	// The windows `copy` made, into the owner edit.parent (0 = the screen's roots) at
	// position, fresh identities, names made unique within the screen.
	bool paste_records(Node &row, const Edit &edit, const IdAllocator &allocate, std::vector<NodeId> &added,
	                   std::string &error) override;
	// A duplicated screen takes a name no other screen of `rows` has (OPTIONS then OPTIONS2).
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// A menu keeps at least one screen: a step that removes its last one is refused.
	bool accept_step(const EditStep &step, const StagedRows &rows,
	                 std::string &error) const override;

private:
	// Whether the model holds the code page's bytes (a menu with no byte order mark, mnu.h's
	// SourceEncoding::CodePage) rather than UTF-8.
	bool code_page_model() const;
	struct SavedImage {
		bool made = false;
		uint64_t load_generation = 0, revision = 0;
		std::shared_ptr<const mnu::Document> image;
		std::vector<SourceIssue> issues;
	};
	mutable SavedImage saved_;
	// What saved_serialization made, and of which state (its load generation and revision).
	struct Serialized {
		bool made = false;
		uint64_t load_generation = 0, revision = 0;
		SerializeResult result;
	};
	mutable Serialized serialized_;
	// The screens and windows (their identities) no by-name lookup returns, and why, once per
	// load generation and revision.
	struct Lookups {
		bool made = false;
		uint64_t load_generation = 0, revision = 0;
		std::unordered_map<NodeId, MenuLookupName::Found> unfound;
	};
	mutable Lookups lookups_;
};

bool is_menu_kind(AssetKind kind);

// The scopes a menu's names resolve in (FieldUse::scope, GraphSymbol::scope; the asset graph's
// scope_matches compares them). A string id: the "menu" section of the table its window reads
// (menu::window_text_rsrc; "" = none, so the scope matches no symbol and the game shows the id)
// [orig: CUIStringTable_LookupString @ 0x6527c0]. A screen: its menu file's flat name, upper case
// ("MAIN.MNU"). A window: its menu file and its screen's NAME, upper case ("MAIN.MNU/STARTUP");
// screens of one name share it, the lookup finding the later one.
std::string menu_text_scope(const std::string &table);
std::string menu_screen_scope(const std::string &menu_file);
std::string menu_window_scope(const std::string &menu_file, const std::string &screen);

// The menu document type's validator over one menu (DocumentType::validate_file), an open
// document standing in for its file: what its reader leaves out and what it cannot hold or
// write; two screens or two windows of a screen of one NAME (menu.duplicate_screen /
// menu.duplicate_window: the lookups find one of them) and an ACTION the game never runs or
// ignores (menu.action_inert: on a window with no NAME, a TYPE none of the sixteen, a WINDOW
// row with no STATE it acts on) are warnings; the references a menu makes (fonts and colors
// through the stylesheet, textures, sound banks, other menus and their screens, windows,
// string tables and string ids) are the asset graph's.
std::vector<Diagnostic> validate_menu_file(const DocumentBase &document);

// The menu type's own finding codes (DocumentType::findings), each a row of its table
// (mnu_document.cpp, static_asserted into this order): input the reader leaves out, which the game
// ignores and a rewrite drops, or which the typed model cannot carry; a menu the writer cannot
// write; two screens, or two windows of a screen, of one NAME; an ACTION the game never runs; and
// the render check's (preview/menu_render_check.h): a screen whose compiled windows are not the
// document's, and after these one row per compiler note (finding_code(menu::MenuFrameNoteCode):
// menu.render. and the note's token, in the note codes' order).
enum class MenuFinding {
	InvalidInput,
	IgnoredInput,
	Unserializable,
	DuplicateScreen,
	DuplicateWindow,
	ActionInert,
	RenderMapping,
	kCount
};
const FindingCodeRow &finding_code(MenuFinding code);
const FindingCodeRow &finding_code(menu::MenuFrameNoteCode code);
FindingTable menu_finding_codes();

} // namespace opennova::editor
