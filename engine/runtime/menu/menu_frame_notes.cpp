// The menu frame compiler's notes (ADR 0046 S9j2): what configure(), the loader and a
// layout pass make of a screen where the picture may not be what the author meant. They
// only observe: the draw list is the same with or without them. The rule each code
// stands for is the compiler's, cited where the compiler applies it; the table of codes,
// bases and witnesses is docs/mnu/menu-re.md ("Compiler notes").

#include <runtime/menu/menu_frame_internal.h>

#include <formats/mns/mns.h>

#include <cstring>
#include <string>
#include <vector>

namespace opennova::menu {

namespace {

using Code = MenuFrameNoteCode;
using Basis = MenuFrameNoteBasis;

struct CodeRow {
	Code code;
	const char *token;
	Basis basis;
};

// In enum order (menu_frame_note_token indexes it).
const CodeRow kCodes[kMenuFrameNoteCodeCount] = {
	{ Code::AppearanceStateUnknown, "appearance_state_unknown", Basis::Witnessed },
	{ Code::AppearanceTypeUnknown, "appearance_type_unknown", Basis::Witnessed },
	{ Code::AppearanceCustom, "appearance_custom", Basis::Witnessed },
	{ Code::AppearanceReplaced, "appearance_replaced", Basis::Witnessed },
	{ Code::ColorUnparsed, "color_unparsed", Basis::Witnessed },
	{ Code::ColorTransparent, "color_transparent", Basis::Witnessed },
	{ Code::StyleVarUnresolved, "style_var_unresolved", Basis::Witnessed },
	{ Code::TypeUnknown, "type_unknown", Basis::Witnessed },
	{ Code::ItemKindAsText, "item_kind_as_text", Basis::Witnessed },
	{ Code::TableCellsDeferred, "table_cells_deferred", Basis::Deferred },
	{ Code::TableCellsCustom, "table_cells_custom", Basis::Witnessed },
	{ Code::ScrollExtentDefault, "scroll_extent_default", Basis::Witnessed },
	{ Code::FontMissing, "font_missing", Basis::Witnessed },
	{ Code::FontUnreadable, "font_unreadable", Basis::Witnessed },
	{ Code::TextureMissing, "texture_missing", Basis::Witnessed },
	{ Code::TextureUnreadable, "texture_unreadable", Basis::Witnessed },
	{ Code::TextTableMissing, "text_table_missing", Basis::Witnessed },
	{ Code::TextTableUnreadable, "text_table_unreadable", Basis::Witnessed },
	{ Code::RectEmpty, "rect_empty", Basis::Witnessed },
	{ Code::TextTruncated, "text_truncated", Basis::Witnessed },
	{ Code::TextNoRoom, "text_no_room", Basis::Witnessed },
	{ Code::TextNoFont, "text_no_font", Basis::Witnessed },
	{ Code::TextIdMissing, "text_id_missing", Basis::Witnessed },
	{ Code::ImageBandEmpty, "image_band_empty", Basis::Witnessed },
	{ Code::ImageHeightShared, "image_height_shared", Basis::Witnessed },
	{ Code::StateFallback, "state_fallback", Basis::Witnessed },
	{ Code::CheckedNoArt, "checked_no_art", Basis::Witnessed },
	{ Code::FrameAbsent, "frame_absent", Basis::Witnessed },
	{ Code::FrameStencilUnloaded, "frame_stencil_unloaded", Basis::Witnessed },
	{ Code::FrameNoStencil, "frame_no_stencil", Basis::Witnessed },
	{ Code::FrameTileZero, "frame_tile_zero", Basis::Witnessed },
	{ Code::SpinArrowEmpty, "spin_arrow_empty", Basis::Witnessed },
	{ Code::ListRowsClipped, "list_rows_clipped", Basis::Witnessed },
	{ Code::TableNoColumns, "table_no_columns", Basis::Witnessed },
	{ Code::TableHeaderClipped, "table_header_clipped", Basis::Witnessed },
	{ Code::TableHeaderWidthZero, "table_header_width_zero", Basis::Witnessed },
	{ Code::MarqueeRuntimeContent, "marquee_runtime_content", Basis::PortPolicy },
};

bool iequals(const std::string &a, const char *b) {
	return strutil::iequals(a, b);
}

const char *state_token(int slot) {
	switch (slot) {
		case kStateDefault: return "default";
		case kStateDisabled: return "disabled";
		case kStateMouseover: return "mouseover";
		case kStateSelected: return "selected";
		default: return "none";
	}
}

// The classes whose parse reads a STRING [orig: CUIButtonWidget_ParseXMLAttributes
// @ 0x657c30, the STATIC parse and every class built on it; grill set A3's S-chain].
bool reads_string(mnu::WindowType type) {
	switch (type) {
		case mnu::WindowType::Static:
		case mnu::WindowType::Button:
		case mnu::WindowType::Edit:
		case mnu::WindowType::MultilineEdit:
		case mnu::WindowType::Radio:
		case mnu::WindowType::CheckBox:
		case mnu::WindowType::SpinList:
		case mnu::WindowType::List:
		case mnu::WindowType::LanList:
		case mnu::WindowType::Table:
		case mnu::WindowType::Combo:
		case mnu::WindowType::RadioEdit:
			return true;
		default:
			return false;
	}
}

bool is_image_or_color_item(const mnu::Item &item) {
	return iequals(item.type, "image") || iequals(item.type, "color");
}

} // namespace

const char *menu_frame_note_token(MenuFrameNoteCode code) {
	const int at = static_cast<int>(code);
	return at >= 0 && at < kMenuFrameNoteCodeCount ? kCodes[at].token : "";
}

MenuFrameNoteBasis menu_frame_note_basis(MenuFrameNoteCode code) {
	const int at = static_cast<int>(code);
	return at >= 0 && at < kMenuFrameNoteCodeCount ? kCodes[at].basis : Basis::Witnessed;
}

const char *menu_frame_note_basis_token(MenuFrameNoteBasis basis) {
	switch (basis) {
		case Basis::Witnessed: return "witnessed";
		case Basis::PortPolicy: return "port_policy";
		case Basis::Deferred: return "deferred";
	}
	return "witnessed";
}

// --- configure ---------------------------------------------------------------

void MenuFrameCompiler::note_(MenuFrameNoteCode code, const std::string &subject, const char *list,
		int record, const char *field) const {
	MenuFrameNote note;
	note.widget = building_;
	note.code = code;
	note.subject = subject;
	note.list = list != nullptr ? list : "";
	note.record = record;
	note.field = field != nullptr ? field : "";
	notes_.push_back(std::move(note));
}

void MenuFrameCompiler::note_var_(const std::string &value, const char *list, int record,
		const char *field) const {
	if (mns::is_variable_reference(value) && resolve_var(value) == value) {
		note_(Code::StyleVarUnresolved, value, list, record, field);
	}
}

// A colour as the parse reads it: wcstoul over the text after %VAR% (a partly valid
// value keeps its valid prefix, garbage reads 0); an APPEARANCE COLOR / OUTLINE keeps the
// word's alpha, so fewer than eight digits (six, typically) leave it 0, while eight
// digits with an alpha of 00 are taken as written; a FONT colour is forced opaque by the
// text sink [orig: CFontCache_DrawTextScaled @ 0x653170].
void MenuFrameCompiler::note_color_(const std::string &value, bool alpha_forced,
		const char *list, int record, const char *field) const {
	if (value.empty()) {
		return;
	}
	if (mns::is_variable_reference(value) && resolve_var(value) == value) {
		note_(Code::StyleVarUnresolved, value, list, record, field);
		return;
	}
	const std::string text = resolve_var(value);
	size_t digits = 0;
	if (!mnu::color_reads_whole(text, &digits)) {
		note_(Code::ColorUnparsed, text, list, record, field);
	} else if (!alpha_forced && digits < 8 && (mnu::color_value(text) >> 24) == 0u) {
		note_(Code::ColorTransparent, text, list, record, field);
	}
}

// One row as build_state_passes reads it [orig: CUIElement_ParseXMLDefinition @ 0x648120,
// the APPEARANCE arm @ 0x6483d4..0x648634]. A row inside a part names the part's record.
void MenuFrameCompiler::note_appearance_row_(const std::vector<mnu::Appearance> &rows, size_t row,
		const char *list, bool part_rows) const {
	const mnu::Appearance &ap = rows[row];
	const int record = part_rows ? 0 : static_cast<int>(row);
	const auto field = [part_rows](const char *name) { return part_rows ? "" : name; };
	const int slot = appearance_state_slot(ap.state);
	if (slot < 0) {
		note_(Code::AppearanceStateUnknown, ap.state, list, record, field("state"));
		return;
	}
	const bool image = iequals(ap.type, "image");
	const bool color = iequals(ap.type, "color");
	const bool outline = iequals(ap.type, "outline");
	const bool custom = iequals(ap.type, "custom");
	if (!image && !color && !outline && !custom) {
		// A table's ITEMS reads IMAGEROW (its row image: D-MNU-13); anywhere else, and any
		// other token, the row marks its state and draws nothing. An empty TYPE is the
		// marker on purpose.
		if (iequals(ap.type, "imagerow") && std::strcmp(list, "items.appearance") == 0) {
			note_(Code::TableCellsDeferred, ap.type, list, record, field("type"));
		} else if (!ap.type.empty()) {
			note_(Code::AppearanceTypeUnknown, ap.type, list, record, field("type"));
		}
		return;
	}
	if (custom) {
		note_(Code::AppearanceCustom, state_token(slot), list, record, field("type"));
		return;
	}
	for (size_t later = row + 1; later < rows.size(); ++later) {
		if (appearance_state_slot(rows[later].state) == slot &&
				strutil::iequals(rows[later].type, ap.type)) {
			note_(Code::AppearanceReplaced, std::string(state_token(slot)) + " " + strutil::to_lower(ap.type), list,
					record, field("value"));
			break;
		}
	}
	if (image) {
		note_var_(ap.value, list, record, field("value"));
	} else {
		note_color_(ap.value, false, list, record, field("value"));
	}
}

// The window's own fields: its TYPE [orig: CUIScene_CreateWidgetByType @ 0x64f630 builds
// a generic CWnd for a token it does not match], its FONT and asset names, a SCROLL's
// arrow length, and the rows a list, a combo or a table holds that the port does not draw.
void MenuFrameCompiler::note_window_(const mnu::Window &w) const {
	if (w.type == mnu::WindowType::Window && !w.type_token.empty() && !iequals(w.type_token, "window")) {
		note_(Code::TypeUnknown, w.type_token, nullptr, -1, "type");
	}
	note_var_(w.font.name, nullptr, -1, "font.name");
	note_color_(w.font.default_fg, true, nullptr, -1, "font.default_fg");
	note_color_(w.font.mouseover_fg, true, nullptr, -1, "font.mouseover_fg");
	note_color_(w.font.selected_fg, true, nullptr, -1, "font.selected_fg");
	note_color_(w.font.disabled_fg, true, nullptr, -1, "font.disabled_fg");
	note_var_(w.frame.stencil, nullptr, -1, "frame.stencil");
	note_var_(w.frame.brush, nullptr, -1, "frame.brush");
	note_var_(w.cursor.file, nullptr, -1, "cursor.file");
	// A SCROLL's arrows run 20 along its axis unless a HEIGHT or WIDTH writes the length
	// [orig: CScrollWnd_Construct @ 0x64c450 sets +0xC10 = 20 @ 0x64c4f5; only the HEIGHT /
	// WIDTH arm of CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0 writes it, @ 0x64cb5f].
	if (w.type == mnu::WindowType::Scroll && !w.has_scroll_extent) {
		note_(Code::ScrollExtentDefault, "20", nullptr, -1, "scroll_extent");
	}
	// A list's and a combo's rows of TYPE IMAGE or COLOR are the text written for them, which the row
	// draws as its label [orig: CListWnd_ParseXMLDefinition @ 0x645b0f..0x645b26, the literal row
	// @ 0x645c1b..0x645c6f; CListWnd_DrawItems @ 0x643f30] (MenuFrameCompiler::configure, build_items).
	if (w.type == mnu::WindowType::List || w.type == mnu::WindowType::LanList) {
		for (size_t i = 0; i < w.items.items.size(); ++i) {
			if (is_image_or_color_item(w.items.items[i])) {
				note_(Code::ItemKindAsText, w.items.items[i].type, "items.item", static_cast<int>(i), "type");
			}
		}
	}
	if (w.type == mnu::WindowType::Combo && w.list_box) {
		for (const mnu::Item &item : w.list_box->items.items) {
			if (is_image_or_color_item(item)) {
				note_(Code::ItemKindAsText, item.type, "list_box", 0, "");
				break;
			}
		}
	}
	// A table's bitmap and bitmap-and-text cells and its SUBST images are drawn
	// (menu_frame_table.cpp, emit_table); an ITEMS IMAGEROW row stays deferred, noted where its
	// appearance row is read (note_appearance_row_). A CUSTOM_DRAW column is the menu's code's to
	// draw: its header and body cells raise the table's custom-draw event to the handler the code
	// binds, with no ITEMS pass behind them [orig: CUITable_Render @ 0x6411d0, the header event
	// @ 0x6413b3..0x641419, the body event @ 0x641730..0x6417bc; CUIScene_RegisterControlCallback
	// @ 0x63c060], so a compile with none bound (the editor's) draws nothing there. Noted at the
	// BODY that makes the column custom: the last one its running column index reaches, as
	// build_table_columns_ reads them.
	if (w.type == mnu::WindowType::Table) {
		const mnu::TableColumn &xml = w.table_data.column;
		const int count = xml.has_count && xml.count >= 1 ? xml.count : 1;
		std::vector<int> last(static_cast<size_t>(count), -1);
		int running = 0;
		for (size_t i = 0; i < xml.bodies.size(); ++i) {
			if (xml.bodies[i].has_column) running = xml.bodies[i].column;
			if (running >= 0 && running < count) last[static_cast<size_t>(running)] = static_cast<int>(i);
		}
		for (const int i : last) {
			if (i >= 0 && iequals(xml.bodies[static_cast<size_t>(i)].display, "CUSTOM_DRAW")) {
				note_(Code::TableCellsCustom, xml.bodies[static_cast<size_t>(i)].display, "column.body", i,
						"display");
			}
		}
	}
}

// A part node (a spin arrow, or a window inside one) is its list's SPINUP or SPINDOWN
// record: a field of the arrow's own window stays, a row inside it is the record.
void MenuFrameCompiler::settle_part_notes_(size_t from) const {
	for (size_t n = from; n < notes_.size(); ++n) {
		MenuFrameNote &note = notes_[n];
		if (note.widget < 0 || note.widget >= static_cast<int>(nodes_.size()) ||
				!nodes_[static_cast<size_t>(note.widget)].part) {
			continue;
		}
		int arrow = note.widget;
		while (nodes_[static_cast<size_t>(arrow)].parent >= 0 &&
				nodes_[static_cast<size_t>(nodes_[static_cast<size_t>(arrow)].parent)].part) {
			arrow = nodes_[static_cast<size_t>(arrow)].parent;
		}
		const WidgetNode &part = nodes_[static_cast<size_t>(arrow)];
		const bool own_field = note.widget == arrow && note.list.empty();
		note.widget = part.parent;
		note.list = part.part_kind == 2 ? "spindown" : "spinup";
		note.record = 0;
		if (!own_field) {
			note.field.clear();
		}
	}
}

// --- the loader's ------------------------------------------------------------

// Every window naming the file that did not load, at the field or row that names it (the
// screen when none does). A font or texture that does not load leaves a zero handle,
// which draws and measures nothing [orig: CFontCache_LoadOrGetFont @ 0x652f70;
// CTextureManager_LoadOrFindTexture @ 0x654980 writes the handle 0 first]; a table that
// does not load answers no lookup [orig: CUIStringTable_LookupString @ 0x6527c0].
void MenuFrameCompiler::add_load_note(MenuFrameNoteCode code, const std::string &name) {
	const size_t from = notes_.size();
	const auto push = [&](int widget, const char *list, int record, const char *field) {
		MenuFrameNote note;
		note.widget = widget;
		note.code = code;
		note.subject = name;
		note.list = list;
		note.record = record;
		note.field = field;
		for (size_t n = from; n < notes_.size(); ++n) {
			if (notes_[n] == note) {
				return;
			}
		}
		notes_.push_back(std::move(note));
	};
	const auto names = [&](const std::string &value) {
		return !value.empty() && strutil::iequals(resolve_var(value), name);
	};
	int32_t slot = kMenuTexNone;
	for (size_t s = 0; s < texture_names_.size(); ++s) {
		if (strutil::iequals(texture_names_[s], name)) {
			slot = static_cast<int32_t>(s);
		}
	}
	for (size_t i = 0; i < nodes_.size(); ++i) {
		const int index = static_cast<int>(i);
		const WidgetNode &node = nodes_[i];
		const mnu::Window &w = *node.window;
		switch (code) {
			case Code::FontMissing:
			case Code::FontUnreadable:
				if (names(w.font.name)) {
					push(index, "", -1, "font.name");
				}
				break;
			case Code::TextTableMissing:
			case Code::TextTableUnreadable:
				if (w.has_text_rsrc && strutil::iequals(w.text_rsrc, name)) {
					push(index, "", -1, "text_rsrc");
				}
				for (const auto &part : { std::make_pair(&w.list_box, "list_box"), std::make_pair(&w.scrollbar, "scrollbar") }) {
					if (part.first->present() && (*part.first)->has_text_rsrc &&
							strutil::iequals((*part.first)->text_rsrc, name)) {
						push(index, part.second, 0, "");
					}
				}
				break;
			case Code::TextureMissing:
			case Code::TextureUnreadable: {
				if (slot == kMenuTexNone) {
					break;
				}
				const auto rows = [&](const StatePass (&states)[4], const char *list, bool part_rows) {
					for (const StatePass &pass : states) {
						if (pass.has_image && pass.texture == slot) {
							push(index, list, part_rows ? 0 : pass.image_row, part_rows ? "" : "value");
						}
					}
				};
				rows(node.states, "appearance", false);
				rows(node.items_states, "items.appearance", false);
				rows(node.popup_states, "list_box", true);
				rows(node.popup_items_states, "list_box", true);
				for (const auto &part : { std::make_pair(&node.embedded_scrollbar, "scrollbar"),
							 std::make_pair(&node.popup_scrollbar, "list_box") }) {
					rows(part.first->track, part.second, true);
					rows(part.first->shuttle, part.second, true);
					rows(part.first->up, part.second, true);
					rows(part.first->down, part.second, true);
				}
				if (w.type == mnu::WindowType::Scroll) {
					rows(node.scrollbar.shuttle, "shuttle", false);
					rows(node.scrollbar.up, "scrollup", false);
					rows(node.scrollbar.down, "scrolldown", false);
				}
				if (node.frame_owner == index && node.frame_stencil == slot) {
					push(index, "", -1, "frame.stencil");
				}
				if (node.frame_owner == index && node.frame_brush == slot) {
					push(index, "", -1, "frame.brush");
				}
				if (node.cursor == slot) {
					push(index, "", -1, "cursor.file");
				}
				for (size_t r = 0; r < node.items.size(); ++r) {
					if (node.items[r].kind == WidgetNode::ItemVisual::kImage && node.items[r].texture == slot) {
						push(index, "items.item", static_cast<int>(r), "text");
					}
				}
				for (const WidgetNode::ItemVisual &item : node.popup_items) {
					if (item.kind == WidgetNode::ItemVisual::kImage && item.texture == slot) {
						push(index, "list_box", 0, "");
					}
				}
				break;
			}
			default:
				break;
		}
	}
	if (notes_.size() == from) {
		push(-1, "", -1, "");
	}
	settle_part_notes_(from);
}

// --- layout ------------------------------------------------------------------

std::vector<MenuFrameNote> MenuFrameCompiler::layout_notes(const MenuFrameState &state) const {
	std::vector<MenuFrameNote> out;
	const auto push = [&out](int widget, Code code, const std::string &subject, const char *list, int record,
							  const char *field) {
		MenuFrameNote note;
		note.widget = widget;
		note.code = code;
		note.subject = subject;
		note.list = list;
		note.record = record;
		note.field = field;
		out.push_back(std::move(note));
	};
	for (int index = 0; index < document_nodes_; ++index) {
		const WidgetNode &node = nodes_[static_cast<size_t>(index)];
		const mnu::Window &w = *node.window;
		const MenuWidgetState *ws = state_for(state, index);
		const mnu::RectEdges rect = node_rect_(node, ws);
		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;
		bool fills = false;    // COLOR and IMAGE: stretched into the rect
		bool outlines = false; // OUTLINE: drawn as edge lines, which a rect with no area keeps
		for (const StatePass &pass : node.states) {
			fills = fills || pass.has_color || pass.has_image;
			outlines = outlines || pass.has_outline;
		}
		const bool passes = fills || outlines;
		const std::string authored =
				w.string_data.present ? resolve_text_value(node.text_table, w.string_data.type, w.string_data.value).text
									  : std::string();
		int frame_tile = 0;
		const FrameGate frame_gate = w.draw_frame ? frame_gate_(node, &frame_tile) : FrameGate::NoFrame;

		// The rect [orig: the parse tail @ 0x649736 -> CStaticWnd_AdjustRectToTextSize @ 0x6575f0]:
		// what fills the rect shows nothing in one with no area (a COLOR or IMAGE pass
		// [orig: CUIElement_Draw @ 0x64a8a0]; a frame's tiled middle, all a frame with no
		// BRUSH draws [orig: CUIElement_DrawFrame @ 0x64a210, the fill quad @ 0x64a2b3; the
		// border pieces, with a BRUSH, hang outside the rect and still draw]), and the mouse
		// never reaches it [orig: CWnd_HitTestPoint @ 0x646700 is PtInRect]. Quiet where
		// nothing vanishes: an OUTLINE still draws its edge lines [orig:
		// CUIElement_DrawOutlineRect @ 0x647fc0], a window a HOTKEY presses needs no mouse, a
		// plain window or label only holds what it holds.
		const bool clickable = w.type != mnu::WindowType::Window && w.type != mnu::WindowType::Static &&
				w.type != mnu::WindowType::Marquee && w.hotkeys.empty();
		const bool frame_fill_only = frame_gate == FrameGate::Draws && node.frame_brush < 0;
		const bool vanishes = fills || frame_fill_only || clickable;
		if ((width <= 0 || height <= 0) && vanishes) {
			bool images_unloaded = !node.image_rows.empty();
			for (const WidgetNode::ImageRow &row : node.image_rows) {
				images_unloaded = images_unloaded && !texture_loaded_(row.texture);
			}
			const bool text_sized = mnu::window_type_is_text_sized(w.type);
			const char *cause = text_sized && !authored.empty() && node.measure_font == 0 ? "font"
					: images_unloaded                                                     ? "image"
																						  : "position";
			push(index, Code::RectEmpty, cause, "", -1, width <= 0 ? "position.right" : "position.bottom");
		}

		// The label [orig: CStaticWnd_DrawLabel @ 0x656fb0, through fit_label_ as the draw].
		const bool label = w.type == mnu::WindowType::Static || w.type == mnu::WindowType::Button ||
				w.type == mnu::WindowType::Radio;
		const std::string text = label ? widget_text(node, ws) : authored;
		bool has_text = !text.empty();
		if (!has_text && (w.type == mnu::WindowType::List || w.type == mnu::WindowType::LanList ||
								 w.type == mnu::WindowType::SpinList || w.type == mnu::WindowType::Combo)) {
			for (const auto *items : { &node.items, &node.popup_items }) {
				for (const WidgetNode::ItemVisual &item : *items) {
					has_text = has_text || (item.kind == WidgetNode::ItemVisual::kText && !item.text.empty());
				}
			}
		}
		if (has_text && reads_string(w.type) && font_for(node) == nullptr) {
			// [orig: font_cache_ensure_font_loaded @ 0x653de0 returns 0 for handle 0]
			push(index, Code::TextNoFont, text, "", -1, "font.name");
		} else if (label && !text.empty() && width > 0 && height > 0) {
			const int edge = w.string_data.has_edge ? w.string_data.edge : 0;
			std::string drawn;
			int drawn_w = 0;
			int line_h = 0;
			if (!fit_label_(node, text, width - 2 * edge, &drawn, &drawn_w, &line_h)) {
				push(index, Code::TextNoRoom, text, "", -1, "string.value");
			} else if (drawn.size() < text.size()) {
				push(index, Code::TextTruncated, drawn, "", -1, "string.value");
			}
		}
		// A string id the window's table does not define shows the id [orig:
		// CUIStringTable_LookupString @ 0x6527c0; the caller keeps the key @ 0x657f45].
		if (reads_string(w.type) && w.string_data.present && iequals(w.string_data.type, "id")) {
			const std::string id = resolve_var(w.string_data.value);
			if (!id.empty() && (text_tables_ == nullptr || text_tables_->lookup(node.text_table, id) == nullptr)) {
				push(index, Code::TextIdMissing, id, "", -1, "string.value");
			}
		}

		// The appearance: the state the pump would leave, its fallback [orig:
		// CWnd_ProcessMouseEvent @ 0x647c89..0x647cb5], and every IMAGE row's band
		// [orig: Render_DrawTiledTextureStrip @ 0x67aed0; the first load's HEIGHT,
		// CTextureManager_LoadOrFindTexture @ 0x654980].
		bool any_state = false;
		for (const StatePass &pass : node.states) {
			any_state = any_state || pass.present;
		}
		const int held = pump_visual_state(node, ws);
		const int drawn_state = appearance_state_with_fallback(node, held);
		if (any_state && held != kStateDefault && drawn_state != held) {
			push(index, Code::StateFallback, state_token(held), "", -1, "");
		}
		const bool checked = ws != nullptr && ws->has_checked ? ws->checked : w.checked;
		if (checked && !node.states[kStateSelected].present &&
				((w.type == mnu::WindowType::CheckBox && drawn_state >= 0) ||
						(w.type == mnu::WindowType::Radio && passes))) {
			// [orig: CCheckWnd_Render @ 0x64ae20 draws slot 3 directly; CRadioWnd_Render
			// @ 0x656e33 forces +236 = 3, no availability check]
			push(index, Code::CheckedNoArt, state_token(kStateSelected), "", -1, "");
		}
		for (int slot = 0; slot < 4; ++slot) {
			const StatePass &pass = node.states[slot];
			if (!pass.has_image || !texture_loaded_(pass.texture)) {
				continue;
			}
			const int texture_height = texture_sizes_[static_cast<size_t>(pass.texture)].second;
			const int64_t band = pass.band_height == -1 ? texture_height : pass.band_height;
			const int64_t start = static_cast<int64_t>(pass.map_state) * band;
			const int64_t end = static_cast<int64_t>(pass.map_state + 1) * band;
			if (start < 0 || static_cast<uint32_t>(end) > static_cast<uint32_t>(texture_height) || end < 0) {
				push(index, Code::ImageBandEmpty, state_token(slot), "appearance", pass.image_row, "map_state");
			}
			// The band this row asked for, its HEIGHT or else the texture's own: equal to the
			// first load's, it draws what the author wrote.
			const int64_t asked = pass.row_height == -1 ? texture_height : pass.row_height;
			if (asked != band) {
				push(index, Code::ImageHeightShared,
						pass.band_height == -1 ? std::string("texture") : std::to_string(pass.band_height),
						"appearance", pass.image_row, "height");
			}
		}

		// The frame [orig: the +0x134 gate, CStaticWnd_Render @ 0x657b10; CUIElement_DrawFrame
		// @ 0x64a210]. A FRAME that names no STENCIL (a BRUSH alone) has a stencil handle of
		// 0, so nothing is set up [orig: CUIElement_InitBorderMaterials @ 0x646f70 returns at 0x646f7c];
		// it has no name for the asset graph to find missing, so it is its own code.
		if (w.draw_frame) {
			switch (frame_gate) {
				case FrameGate::NoFrame:
					push(index, Code::FrameAbsent, std::string(), "", -1, "draw_frame");
					break;
				case FrameGate::NoStencil: {
					const mnu::Window &owner = *nodes_[static_cast<size_t>(node.frame_owner)].window;
					if (resolve_var(owner.frame.stencil).empty()) {
						push(index, Code::FrameNoStencil, owner.name, "", -1, "draw_frame");
					} else {
						push(index, Code::FrameStencilUnloaded, owner.frame.stencil, "", -1, "draw_frame");
					}
					break;
				}
				case FrameGate::TileZero:
					push(index, Code::FrameTileZero, nodes_[static_cast<size_t>(node.frame_owner)].window->name, "",
							-1, "draw_frame");
					break;
				case FrameGate::Draws:
					break;
			}
		}

		// The spin arrows: a rect with no area is neither drawn nor hit [orig:
		// CWnd_HitTestPoint @ 0x646700 is PtInRect on the rect alone;
		// CSpinListWnd_CreateUpDownChildren @ 0x64b8b0].
		for (const int arrow : { node.spin_up, node.spin_down }) {
			if (arrow < 0) {
				continue;
			}
			const WidgetNode &part = nodes_[static_cast<size_t>(arrow)];
			if (!node_shown(*part.window, nullptr)) {
				continue;
			}
			const mnu::RectEdges arrow_rect = solve_rect(part);
			if (arrow_rect.right <= arrow_rect.left || arrow_rect.bottom <= arrow_rect.top) {
				push(index, Code::SpinArrowEmpty, part.part_kind == 2 ? "spindown" : "spinup",
						part.part_kind == 2 ? "spindown" : "spinup", 0,
						arrow_rect.right <= arrow_rect.left ? "position.right" : "position.bottom");
			}
		}

		// A list's rows past its rect with no scrollbar to reach them [orig:
		// CListWnd_DrawItems @ 0x643f30 stops at the rect's bottom].
		if ((w.type == mnu::WindowType::List || w.type == mnu::WindowType::LanList) &&
				!node.embedded_scrollbar.present) {
			const int rows = ws != nullptr && ws->has_items ? static_cast<int>(ws->items.size())
															: static_cast<int>(node.items.size());
			const int row_h = row_height_(node);
			if (row_h > 0 && height > 0 && rows > height / row_h) {
				push(index, Code::ListRowsClipped, std::to_string(rows - height / row_h), "", -1, "");
			}
		}

		// A table's columns [orig: CUITable_Render @ 0x6411d0, the header loop @ 0x64137c..
		// 0x641390: width 0 skipped, past the right edge neither drawn nor advanced].
		if (w.type == mnu::WindowType::Table) {
			const std::vector<TableColumnSetup> columns = table_columns_(node, ws);
			bool any_width = false;
			for (const TableColumnSetup &column : columns) {
				any_width = any_width || column.width != 0;
			}
			// A HEADER that leaves its column at width 0 (its WIDTH, or the one it carries
			// from the HEADER before it [orig: CTableWnd_ParseXMLContentDefinition @
			// 0x6427d0]), and no later HEADER of that column replaces: the column is skipped.
			// Columns the menu's code sets up are the code's.
			bool zero_header = false;
			if (ws == nullptr || !ws->has_table_columns) {
				const std::vector<mnu::TableHeaderSetup> setup = mnu::table_header_setup(w.table_data.column);
				for (size_t h = 0; h < setup.size(); ++h) {
					if (!setup[h].set_up || setup[h].width != 0) {
						continue;
					}
					bool replaced = false;
					for (size_t later = h + 1; later < setup.size(); ++later) {
						replaced = replaced || (setup[later].set_up && setup[later].column == setup[h].column);
					}
					if (!replaced) {
						push(index, Code::TableHeaderWidthZero, columns[static_cast<size_t>(setup[h].column)].label,
								"column.header", static_cast<int>(h), "width");
						zero_header = true;
					}
				}
			}
			if (!any_width && !zero_header) {
				push(index, Code::TableNoColumns, std::string(), "", -1, "");
			} else if (any_width) {
				const int gap = w.table_data.column.has_spacing ? w.table_data.column.spacing : 0;
				int left = rect.left;
				for (const TableColumnSetup &column : columns) {
					if (column.width == 0) {
						continue;
					}
					const int right = left + column.width;
					if (right > rect.right) {
						push(index, Code::TableHeaderClipped, column.label, "", -1, "");
						continue;
					}
					left = right + gap;
				}
			}
		}

		// A marquee's credits load from its DATASOURCE at run time (the embedder's).
		if (w.type == mnu::WindowType::Marquee && !w.datasources.empty() &&
				(ws == nullptr || ws->marquee.nodes.empty())) {
			push(index, Code::MarqueeRuntimeContent, w.datasources.front(), "datasource", 0, "");
		}
	}
	return out;
}

bool MenuFrameCompiler::solve_local_rect(int index, const mnu::Position &candidate,
		mnu::RectEdges *out) const {
	if (out == nullptr || index < 0 || index >= document_nodes_) {
		return false;
	}
	*out = solve_rect_at(nodes_[static_cast<size_t>(index)], candidate);
	return true;
}

} // namespace opennova::menu
