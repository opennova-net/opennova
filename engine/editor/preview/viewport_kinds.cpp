#include <editor/preview/viewport_kinds.h>

#include <cctype>
#include <cstring>
#include <iterator>
#include <vector>

#include <editor/model/document.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/script_viewport.h>
#include <editor/session/view/documents_view.h>

namespace opennova::editor {

namespace {

using T = DocumentTypeId;

// The menu's: a menu's screen, which its stylesheets and string tables feed. The model's: a model,
// or a clip or an animation table played on its rig's model.
constexpr ViewportFeed kMenuFeeds[] = {
	{ T::Menu, true },
	{ T::Styles, false },
	{ T::Strings, false },
};
constexpr ViewportFeed kModelFeeds[] = {
	{ T::Model, true },
	{ T::Animation, true },
	{ T::AnimationMap, true },
};
// The script device's (S13 V10): every text type (S13 D9), its text as it stands, the Document tab's
// main view.
constexpr ViewportFeed kScriptFeeds[] = {
	{ T::Script, true },
	{ T::MusicScript, true },
	{ T::Credits, true },
	{ T::Shader, true },
	{ T::Text, true },
};

constexpr ViewportKindRow kRows[] = {
	{ ViewportKind::Menu, ViewportRole::Preview, true, true, kMenuFeeds, std::size(kMenuFeeds), MenuViewport::make },
	{ ViewportKind::Model, ViewportRole::Preview, true, false, kModelFeeds, std::size(kModelFeeds),
			ModelViewport::make },
	{ ViewportKind::Script, ViewportRole::Main, false, false, kScriptFeeds, std::size(kScriptFeeds),
			ScriptViewport::make },
};

static_assert(std::size(kRows) == kViewportKindCount, "every ViewportKind has exactly one row");

// One row per kind in the enum's order, each with a make and a type it shows.
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const ViewportKindRow &row = kRows[i];
		if (row.kind != static_cast<ViewportKind>(i) || !row.make || !row.feed_count) return false;
		bool shows = false;
		for (size_t f = 0; f < row.feed_count; ++f) {
			if (row.feeds[f].type == T::None) return false;
			shows = shows || row.feeds[f].shows;
		}
		if (!shows) return false;
	}
	return true;
}
static_assert(rows_well_formed(),
		"the viewport kinds follow ViewportKind's order, each with a make and a type it shows");

// Two kinds of `role` that a document type reaches both (`shows_only`: as what they show; else as
// what they show or what feeds them): none, for the static_asserts below.
constexpr bool kinds_apart(ViewportRole role, bool shows_only) {
	for (size_t i = 0; i < kViewportKindCount; ++i)
		for (size_t j = i + 1; j < kViewportKindCount; ++j) {
			if (kRows[i].role != role || kRows[j].role != role) continue;
			for (size_t a = 0; a < kRows[i].feed_count; ++a)
				for (size_t b = 0; b < kRows[j].feed_count; ++b) {
					if (shows_only && (!kRows[i].feeds[a].shows || !kRows[j].feeds[b].shows)) continue;
					if (kRows[i].feeds[a].type == kRows[j].feeds[b].type) return false;
				}
		}
	return true;
}
// The Preview window shows one kind for a type; a type's Document tab has one main view.
static_assert(kinds_apart(ViewportRole::Preview, false), "a document type feeds one Preview-role viewport kind at most");
static_assert(kinds_apart(ViewportRole::Main, true), "a document type is shown by one Main-role viewport kind at most");

const DocumentBase *open_at(const DocumentsView &documents, const std::string &path) {
	for (const auto &document : documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

} // namespace

const ViewportKindRow &viewport_kind_row(ViewportKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return kRows[index < kViewportKindCount ? index : 0];
}

bool viewport_kind_shows(ViewportKind kind, DocumentTypeId type) {
	if (kind == ViewportKind::kCount) return false;
	const ViewportKindRow &row = viewport_kind_row(kind);
	for (size_t i = 0; i < row.feed_count; ++i)
		if (row.feeds[i].type == type && row.feeds[i].shows) return true;
	return false;
}

ViewportKind main_viewport_kind(DocumentTypeId type) {
	for (const ViewportKindRow &row : kRows)
		if (row.role == ViewportRole::Main && viewport_kind_shows(row.kind, type)) return row.kind;
	return ViewportKind::kCount;
}

ViewportKind default_viewport_kind(DocumentTypeId type) {
	const ViewportKind preview = preview_kind_of(type);
	return viewport_kind_shows(preview, type) ? preview : main_viewport_kind(type);
}

ViewportKind preview_kind_of(DocumentTypeId type) {
	for (const ViewportKindRow &row : kRows) {
		if (row.role != ViewportRole::Preview) continue;
		for (size_t i = 0; i < row.feed_count; ++i)
			if (row.feeds[i].type == type) return row.kind;
	}
	return ViewportKind::kCount;
}

std::string viewport_shown_types() {
	std::vector<std::string> named;
	for (const ViewportKindRow &row : kRows)
		for (size_t f = 0; f < row.feed_count; ++f) {
			if (!row.feeds[f].shows) continue;
			// The type in the words of the first asset kind it opens ("Animation map").
			for (size_t k = 0; k < kAssetKindCount; ++k) {
				const AssetKindRow &kind = asset_kind_row(static_cast<AssetKind>(k));
				if (kind.document != row.feeds[f].type) continue;
				std::string label = kind.label;
				for (char &c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				const bool vowel = !label.empty() && std::strchr("aeiou", label[0]) != nullptr;
				named.push_back((vowel ? "an " : "a ") + label);
				break;
			}
		}
	std::string out;
	for (size_t i = 0; i < named.size(); ++i)
		out += (i == 0 ? "" : i + 1 == named.size() ? " or " : ", ") + named[i];
	return out;
}

ViewportKind preview_kind(const DocumentsView &documents, ViewportKind last) {
	ViewportKind kind = last;
	if (const DocumentBase *active = open_at(documents, documents.active)) {
		const ViewportKind fed = preview_kind_of(asset_kind_row(active->kind()).document);
		if (fed != ViewportKind::kCount) kind = fed;
	}
	// What each has to show: the view keeps a kind's target until its document closes.
	if (kind != ViewportKind::kCount && !documents.previews[kind].path.empty()) return kind;
	for (const ViewportKindRow &row : kRows)
		if (row.role == ViewportRole::Preview && !documents.previews[row.kind].path.empty()) return row.kind;
	return ViewportKind::kCount;
}

void update_preview_targets(DocumentsView &documents) {
	const DocumentBase *shown = open_at(documents, documents.active);
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const auto kind = static_cast<ViewportKind>(i);
		const ViewportKindRow &row = viewport_kind_row(kind);
		PreviewTarget &target = documents.previews[kind];
		if (row.role != ViewportRole::Preview) {
			target = PreviewTarget();
			continue;
		}
		// The active document, when the kind shows its type: a kind that shows a row of it follows
		// the row a selection lands in, and keeps the one it had while none is selected.
		if (shown && viewport_kind_shows(kind, asset_kind_row(shown->kind()).document)) {
			if (!row.part) target = {shown->path(), 0};
			else if (documents.selection.primary.row) target = {shown->path(), documents.selection.primary.row};
		}
		// A target whose document closed, or whose row is gone, clears.
		const DocumentBase *document = open_at(documents, target.path);
		const Document *records = document ? records_of(*document) : nullptr;
		if (!document || (row.part && (!records || !records->row(target.part)))) target = PreviewTarget();
	}
	documents.preview_shown = preview_kind(documents, documents.preview_shown);
}

} // namespace opennova::editor
