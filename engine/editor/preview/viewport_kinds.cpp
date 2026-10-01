#include <editor/preview/viewport_kinds.h>

#include <iterator>

#include <editor/preview/menu_viewport.h>
#include <editor/preview/model_viewport.h>

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

constexpr ViewportKindRow kRows[] = {
	{ ViewportKind::Menu, "menu", ViewportRole::Preview, true, true, kMenuFeeds, std::size(kMenuFeeds),
			MenuViewport::make },
	{ ViewportKind::Model, "model", ViewportRole::Preview, true, false, kModelFeeds,
			std::size(kModelFeeds), ModelViewport::make },
};

static_assert(std::size(kRows) == kViewportKindCount, "every ViewportKind has exactly one row");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// One row per kind in the enum's order, each with a token of its own, a make, and a type it shows.
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const ViewportKindRow &row = kRows[i];
		if (row.kind != static_cast<ViewportKind>(i) || !row.token[0] || !row.make || !row.feed_count)
			return false;
		bool shows = false;
		for (size_t f = 0; f < row.feed_count; ++f) {
			if (row.feeds[f].type == T::None) return false;
			shows = shows || row.feeds[f].shows;
		}
		if (!shows) return false;
		for (size_t j = i + 1; j < kViewportKindCount; ++j)
			if (same_text(row.token, kRows[j].token)) return false;
	}
	return true;
}
static_assert(rows_well_formed(),
		"the viewport kinds follow ViewportKind's order, each a token of its own, a make and a type it shows");

// The Preview window shows one kind for a type: a type feeds at most one Preview-role kind.
constexpr bool preview_kinds_apart() {
	for (size_t i = 0; i < kViewportKindCount; ++i)
		for (size_t j = i + 1; j < kViewportKindCount; ++j) {
			if (kRows[i].role != ViewportRole::Preview || kRows[j].role != ViewportRole::Preview) continue;
			for (size_t a = 0; a < kRows[i].feed_count; ++a)
				for (size_t b = 0; b < kRows[j].feed_count; ++b)
					if (kRows[i].feeds[a].type == kRows[j].feeds[b].type) return false;
		}
	return true;
}
static_assert(preview_kinds_apart(), "a document type feeds one Preview-role viewport kind at most");

} // namespace

const ViewportKindRow &viewport_kind_row(ViewportKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return kRows[index < kViewportKindCount ? index : 0];
}

bool viewport_kind_from_token(const std::string &token, ViewportKind &out) {
	for (const ViewportKindRow &row : kRows)
		if (token == row.token) {
			out = row.kind;
			return true;
		}
	return false;
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

ViewportKind preview_kind_of(DocumentTypeId type) {
	for (const ViewportKindRow &row : kRows) {
		if (row.role != ViewportRole::Preview) continue;
		for (size_t i = 0; i < row.feed_count; ++i)
			if (row.feeds[i].type == type) return row.kind;
	}
	return ViewportKind::kCount;
}

} // namespace opennova::editor
