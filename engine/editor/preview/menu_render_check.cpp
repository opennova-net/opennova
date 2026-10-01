#include <editor/preview/menu_render_check.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <variant>

#include <base/io/strutil.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/project_checks.h>
#include <editor/model/diagnostic.h>
#include <editor/preview/make_menu_render_check.h>
#include <formats/mns/mns.h>

namespace opennova::editor {

namespace {

using Code = menu::MenuFrameNoteCode;

std::string quoted(const std::string &text) { return "\"" + text + "\""; }

bool same_stamps(const std::vector<menu::MenuDependency> &a, const std::vector<menu::MenuDependency> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].name != b[i].name || a[i].stamp != b[i].stamp) return false;
	return true;
}

// The variables of two readings of the shell's list that one has and the other lacks, or that
// hold another value (both keyed as the list keys them, upper case).
std::vector<std::string> changed_variables(const std::map<std::string, std::string> &before,
		const std::map<std::string, std::string> &after) {
	std::vector<std::string> out;
	auto was = before.begin();
	auto now = after.begin();
	while (was != before.end() || now != after.end()) {
		if (now == after.end() || (was != before.end() && was->first < now->first)) {
			out.push_back(was->first);
			++was;
		} else if (was == before.end() || now->first < was->first) {
			out.push_back(now->first);
			++now;
		} else {
			if (was->second != now->second)
				out.push_back(now->first);
			++was;
			++now;
		}
	}
	return out;
}

// The variables a menu names: every %NAME% the game's expansion finds in the text its Save would
// write (mns::variable_reference_at, the scan the game runs over a menu's whole text before its
// parse), upper case as the shell's list keys them. Every value the frame compiler resolves
// through the list is one of them, and so is a %NAME% inside a longer text, which the game
// expands too though no StyleVar edge reads it (a whole value is an edge).
std::vector<std::string> variables_named(const MnuDocument &document) {
	const std::string &text = document.saved_serialization().text;
	std::vector<std::string> names;
	for (size_t at = text.find('%'); at != std::string::npos;) {
		const size_t length = mns::variable_reference_at(text, at);
		if (length == 0) {
			at = text.find('%', at + 1);
			continue;
		}
		names.push_back(strutil::to_upper(text.substr(at + 1, length - 2)));
		at = text.find('%', at + length);
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
}

// A closed menu as the game would read it now, for its renders: null when it does not load or a
// source error blocks it (its own findings say why, validate_menu_file). The check reads only a
// menu whose own checks read its records (ValidationCache::records_checked), which the same file
// at the same stamps loads.
std::shared_ptr<const MnuDocument> read_menu(
		const ValidationInput &input, const AssetEntry &asset) {
	auto document = std::make_shared<MnuDocument>();
	Diagnostic error;
	if (!document->load(
				(std::filesystem::path(input.paths.root) / asset.relative_path).generic_string(),
				asset.relative_path, asset.kind, input.project.target_game, error) ||
			document->blocked())
		return nullptr;
	return document;
}

} // namespace

std::string menu_note_message(const menu::MenuFrameNote &note) {
	const std::string &s = note.subject;
	switch (note.code) {
	case Code::AppearanceStateUnknown:
		return "The game does not know the appearance state " + quoted(s) +
		       " (DEFAULT, DISABLED, MOUSEOVER or SELECTED), so this row sets nothing.";
	case Code::AppearanceTypeUnknown:
		return "The game does not know the appearance type " + quoted(s) +
		       " (IMAGE, COLOR, OUTLINE or CUSTOM), so this row only marks its state as present and draws nothing.";
	case Code::AppearanceCustom:
		return "The game hands a CUSTOM appearance (the " + s +
		       " state) to the menu's own code to draw; the preview draws nothing for it.";
	case Code::AppearanceReplaced:
		return "A later row of the same state and type (" + s +
		       ") replaces this row's value, so the game draws the later one.";
	case Code::ColorUnparsed:
		return "The game reads " + quoted(s) +
		       " as a hexadecimal colour only up to the first character that is not a hex digit (with none it reads "
		       "0), so the colour is not the one written.";
	case Code::ColorTransparent:
		return "The game reads " + quoted(s) +
		       " as AARRGGBB, so with fewer than eight digits its alpha is 0 (the preview draws it transparent); "
		       "write eight digits (FF, then RRGGBB) for an opaque one.";
	case Code::StyleVarUnresolved:
		return "No stylesheet the game reads defines " + s + ", so the game uses the text as written.";
	case Code::TypeUnknown:
		return "The game does not know the window type " + quoted(s) +
		       ", so it makes a plain window, which draws only its appearance and frame.";
	case Code::TypeInteriorDeferred:
		return "OpenNova does not draw what is inside a " + s + " yet (D-MNU-13); the preview shows it as a plain "
		       "window.";
	case Code::ItemKindNotDrawn:
		return "OpenNova draws only the text rows of a list or a combo box yet (D-MNU-5), so this " + s +
		       " row is not drawn.";
	case Code::TableCellsDeferred:
		return "OpenNova does not draw a table's " + s + " cells yet (D-MNU-13).";
	case Code::ScrollExtentDefault:
		return "No HEIGHT or WIDTH sets this scroll bar's arrow length, so the game makes each arrow " + s +
		       " long along the bar.";
	case Code::FontMissing:
		return "The font " + s + " is not in the project, so the text here draws with the nearest window above whose "
		       "font loaded (with none, no text).";
	case Code::FontUnreadable:
		return "The font " + s + " is in the project, but the game could not read it, so the text here draws with "
		       "the nearest window above whose font loaded (with none, no text).";
	case Code::TextureMissing:
		return "The texture " + s + " is not in the project, so the game draws nothing where it is used.";
	case Code::TextureUnreadable:
		return "The texture " + s + " is in the project, but the game could not load it, so it draws nothing where "
		       "it is used.";
	case Code::TextTableMissing:
		return "The string table " + s + " is not in the project, so the string ids here show as written.";
	case Code::TextTableUnreadable:
		return "The string table " + s + " is in the project, but the game could not read it, so the string ids "
		       "here show as written.";
	case Code::RectEmpty: {
		// An OUTLINE's edge lines and a braced frame's border pieces still draw: only what
		// fills the rect goes.
		const char *consequence = "what fills its rect (a COLOR or IMAGE appearance, a frame's tiled middle) shows "
		                          "nothing and the mouse cannot reach it.";
		if (s == "image")
			return std::string("The game sizes this window from its images and none of them loaded, so it has no "
			                   "area: ") + consequence;
		if (s == "font")
			return std::string("The game sizes this window from its text and no font loaded to measure it, so it "
			                   "has no area: ") + consequence;
		return std::string("The game gives this window no area (its right edge is not past its left, or its bottom "
		                   "not past its top), so ") + consequence;
	}
	case Code::TextTruncated:
		return "The label does not fit its width: the game draws " + quoted(s) + " and cuts the rest.";
	case Code::TextNoRoom:
		return "The label has no room (the width less twice its EDGE is under 1 pixel), so the game draws none of it.";
	case Code::TextNoFont:
		return "No font loaded for this window or any window above it, so the game neither measures nor draws its "
		       "text.";
	case Code::TextIdMissing:
		return "No string table this window reads defines the id " + s + ", so the game shows the id itself.";
	case Code::ImageBandEmpty:
		return "The " + s + " image's MAP_STATE picks a band past the end of its texture, so the game draws nothing "
		       "for it.";
	case Code::ImageHeightShared:
		return "The game keeps the HEIGHT of a texture's first use (" +
		       (s == "texture" ? std::string("none: the texture's own height") : s) +
		       ") for every later use, so this row's HEIGHT is not the one drawn.";
	case Code::StateFallback:
		return "This window has no " + s +
		       " appearance, so in that state the game draws its default appearance (or none) and its default label "
		       "colour.";
	case Code::CheckedNoArt:
		return "Checked, the game draws the SELECTED appearance, and this window has none, so no appearance is "
		       "drawn while it is checked.";
	case Code::FrameAbsent:
		return "DRAW_FRAME is set, but neither this window nor a window above it has a FRAME, so the game draws "
		       "no frame.";
	case Code::FrameStencilUnloaded:
		return "The frame's STENCIL (" + s + ") did not load, so the game draws no frame here.";
	case Code::FrameNoStencil:
		return "The FRAME of " + s + " names a BRUSH but no STENCIL, so the game sets up no frame and draws none "
		       "here.";
	case Code::FrameTileZero:
		return "The frame of " + s + " has a tile size of 0 (its STENCIL size, else a quarter of the stencil "
		       "texture's width), so the game draws no frame.";
	case Code::SpinArrowEmpty:
		return "The " + s + " arrow has no area, so the game neither draws it nor lets it be clicked.";
	case Code::ListRowsClipped:
		return s + (s == "1" ? " row does" : " rows do") +
		       " not fit the list, and it has no SCROLLBAR, so the game never shows " + (s == "1" ? "it." : "them.");
	case Code::TableNoColumns:
		return "No HEADER sets up a column, so the game draws no header or cell here unless the menu's code fills "
		       "the table.";
	case Code::TableHeaderClipped:
		return "The column " + quoted(s) + " runs past the table's right edge, so the game does not draw its header.";
	case Code::TableHeaderWidthZero:
		return "This HEADER leaves its column " + (s.empty() ? std::string() : quoted(s) + " ") +
		       "0 wide (its WIDTH, or the one it keeps from the HEADER before it), so the game skips the column: "
		       "neither its header nor its cells draw.";
	case Code::MarqueeRuntimeContent:
		return "OpenNova's preview does not roll a marquee's credits (" + s +
		       "); the game loads them when the screen opens.";
	}
	return std::string();
}

bool menu_note_problem(menu::MenuFrameNoteCode code, DiagnosticSeverity *severity) {
	// The note's row says it (the menu type's table, mnu_document.cpp: a render check's row).
	switch (finding_code(code).problem) {
	case FindingProblem::Warning:
		*severity = DiagnosticSeverity::Warning;
		return true;
	case FindingProblem::Info:
		*severity = DiagnosticSeverity::Info;
		return true;
	case FindingProblem::None:
		break;
	}
	return false;
}

NodeAddress menu_note_address(const menu::MenuFrameNote &note, const MnuDocument &document, const Node &screen_row,
                              std::string *field) {
	if (field) *field = note.field;
	const NodeAddress screen{screen_row.id, node_kind(MenuKind::Screen), 0};
	const NodeId window = note.widget >= 0 ? document.window_at(screen_row, size_t(note.widget)) : 0;
	if (!window) {
		if (field) field->clear();
		return screen;
	}
	const NodeAddress owner{screen_row.id, node_kind(MenuKind::Window), window};
	if (note.list.empty()) return owner;
	const size_t list = menu_window_list(note.list);
	if (list != SIZE_MAX) {
		const NodeAddress record =
		        document.record_at(screen_row, size_t(note.widget), list, size_t(std::max(note.record, 0)));
		if (record.child) return record;
	}
	if (field) field->clear();
	return owner;
}

Diagnostic menu_note_diagnostic(const menu::MenuFrameNote &note, const MnuDocument &document, const Node &screen_row,
                                DiagnosticSeverity severity) {
	std::string field;
	const NodeAddress address = menu_note_address(note, document, screen_row, &field);
	Diagnostic d = make_finding(finding_code(note.code), severity, menu_note_message(note), document.path(), field);
	d.row_id = address.row;
	d.child_id = address.child;
	d.record_kind = address.kind;
	d.record = address.child ? document.record_path(address) : screen_row.name();
	return d;
}

void MenuRenderCheck::clear() {
	menus_.clear();
	style_.clear();
	style_stamps_.clear();
	vars_.clear();
	diagnostics_.clear();
	rendered_ = 0;
	cursor_ = Cursor();
	changed_.clear();
	moved_ = false;
}

bool MenuRenderCheck::update(const ProjectCheckInput &input) {
	begin();
	bool moved = false;
	while (!step(input, UINT64_MAX, moved)) {
	}
	return moved;
}

void MenuRenderCheck::begin() {
	cursor_ = Cursor();
}

bool MenuRenderCheck::step(const ProjectCheckInput &input, uint64_t budget, bool &moved) {
	const ValidationInput &validation = input.validation;
	const FileSource &files = input.files;
	if (!cursor_.started) {
		cursor_.started = true;
		rendered_ = 0;
		// The shell's %VAR% list, read again when a stylesheet's stamp moved: the variables that came,
		// went or took another value, which the menus naming one render again.
		cursor_.vars = style_.vars(files);
		std::vector<menu::MenuDependency> stamps;
		style_.dependencies(stamps);
		if (!same_stamps(stamps, style_stamps_)) {
			style_stamps_ = std::move(stamps);
			for (const std::string &name : changed_variables(vars_, cursor_.vars)) changed_.push_back(name);
			std::sort(changed_.begin(), changed_.end());
			changed_.erase(std::unique(changed_.begin(), changed_.end()), changed_.end());
			vars_ = cursor_.vars;
		}
		for (auto &entry : menus_) entry.second.seen = false;
	}
	const std::map<std::string, std::string> &vars = cursor_.vars;
	const auto names_changed = [this](const std::vector<std::string> &names) {
		for (const std::string &name : changed_)
			if (std::binary_search(names.begin(), names.end(), name))
				return true;
		return false;
	};
	uint64_t spent = 0;
	const std::vector<AssetEntry> &entries = validation.scan.entries;
	while (cursor_.next < entries.size()) {
		if (spent >= budget)
			return false;
		const AssetEntry &asset = entries[cursor_.next++];
		if (!is_menu_kind(asset.kind))
			continue;
		spent += kMenuStepCost;
		Menu &kept = menus_[asset.relative_path];
		kept.seen = true;
		const auto open = validation.open_document(asset);
		std::shared_ptr<const MnuDocument> document;
		if (open) {
			// Open: the document stands in for its file, and the file is read again once it closes.
			kept.read = false;
			kept.closed.reset();
			document = std::dynamic_pointer_cast<const MnuDocument>(open);
		} else {
			if (!kept.read || kept.size != asset.size_bytes ||
					kept.modified != asset.modified_ticks || kept.kind != asset.kind ||
					kept.game != validation.project.target_game) {
				kept.read = true;
				kept.closed = input.cache.records_checked(asset.relative_path)
						? read_menu(validation, asset)
						: nullptr;
				kept.size = asset.size_bytes;
				kept.modified = asset.modified_ticks;
				kept.kind = asset.kind;
				kept.game = validation.project.target_game;
			}
			document = kept.closed;
		}
		const MnuDocument *menu = document.get();
		// A menu that does not load or is blocked has its own findings (validate_menu_file).
		if (!menu || menu->blocked()) {
			moved_ = moved_ || !kept.findings.empty();
			kept.document.reset();
			kept.screens.clear();
			kept.dependencies.clear();
			kept.variables.clear();
			kept.findings.clear();
			continue;
		}
		bool stale = !kept.document || kept.identity != menu->identity() ||
				kept.load_generation != menu->load_generation() ||
				kept.revision != menu->revision() || names_changed(kept.variables);
		for (size_t i = 0; !stale && i < kept.dependencies.size(); ++i)
			stale = files.stamp(kept.dependencies[i].name) != kept.dependencies[i].stamp;
		if (!stale) continue;
		kept.document = document;
		kept.identity = menu->identity();
		kept.load_generation = menu->load_generation();
		kept.revision = menu->revision();
		render_menu_(kept, *menu, files, vars);
		++rendered_;
		moved_ = true;
		return false; // a menu a step
	}
	for (auto it = menus_.begin(); it != menus_.end();) {
		if (it->second.seen) {
			++it;
			continue;
		}
		moved_ = moved_ || !it->second.findings.empty();
		it = menus_.erase(it);
	}
	moved = moved_;
	moved_ = false;
	changed_.clear();
	cursor_ = Cursor();
	if (!moved)
		return true;
	diagnostics_.clear();
	for (const auto &entry : menus_)
		diagnostics_.insert(diagnostics_.end(), entry.second.findings.begin(), entry.second.findings.end());
	return true;
}

void MenuRenderCheck::render_menu_(Menu &menu, const MnuDocument &document, const FileSource &files,
                                   const std::map<std::string, std::string> &vars) {
	menu.screens.clear();
	menu.dependencies.clear();
	menu.findings.clear();
	menu.variables = variables_named(document);
	for (const auto &row : document.rows()) {
		Screen screen;
		screen.row = row->id;
		screen.render = std::make_unique<MenuScreenRender>();
		if (screen.render->configure(document, row->id, files, vars) == MenuPreviewStatus::Ready) {
			const menu::MenuFrameCompiler &compiler = screen.render->compiler();
			const std::vector<menu::MenuDependency> &read = screen.render->assets().dependencies();
			menu.dependencies.insert(menu.dependencies.end(), read.begin(), read.end());
			// The compiler's windows are the document's, index for index, or no note maps.
			const int count = compiler.widget_count();
			bool mapped = document.window_at(*row, size_t(count)) == 0;
			for (int index = 0; mapped && index < count; ++index) {
				const NodeId id = document.window_at(*row, size_t(index));
				Value name;
				mapped = id && document.get({row->id, node_kind(MenuKind::Window), id}, "name", name) &&
				         std::holds_alternative<std::string>(name) &&
				         std::get<std::string>(name) == compiler.widget_name(index);
			}
			if (!mapped) {
				Diagnostic d = make_finding(MenuFinding::RenderMapping, DiagnosticSeverity::Error,
				                            "The menu the game would read has other windows on this screen than "
				                            "the editor shows, so the screen's render notes are left out.",
				                            document.path());
				d.row_id = row->id;
				d.record_kind = node_kind(MenuKind::Screen);
				d.record = row->name();
				menu.findings.push_back(std::move(d));
			} else {
				for (const menu::MenuFrameNote &note : screen.render->notes()) {
					DiagnosticSeverity severity = DiagnosticSeverity::Warning;
					if (menu_note_problem(note.code, &severity))
						menu.findings.push_back(menu_note_diagnostic(note, document, *row, severity));
				}
			}
		}
		menu.screens.push_back(std::move(screen));
	}
}

std::unique_ptr<ProjectCheck> make_menu_render_check() { return std::make_unique<MenuRenderCheck>(); }

const MenuRenderCheck *menu_render_check(const ProjectChecks *checks) {
	return checks ? dynamic_cast<const MenuRenderCheck *>(checks->of(DocumentTypeId::Menu)) : nullptr;
}

const MnuDocument *MenuRenderCheck::document(const std::string &path) const {
	const auto found = menus_.find(path);
	return found == menus_.end() ? nullptr : found->second.document.get();
}

const MenuScreenRender *MenuRenderCheck::render(const std::string &path, NodeId screen_row) const {
	const auto found = menus_.find(path);
	if (found == menus_.end()) return nullptr;
	for (const Screen &screen : found->second.screens)
		if (screen.row == screen_row) return screen.render.get();
	return nullptr;
}

} // namespace opennova::editor
