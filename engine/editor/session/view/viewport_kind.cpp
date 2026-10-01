#include <editor/session/view/viewport_kind.h>

#include <iterator>

namespace opennova::editor {

namespace {

// One token per kind, in ViewportKind's order.
constexpr const char *kTokens[] = { "menu", "model" };

static_assert(std::size(kTokens) == kViewportKindCount, "every ViewportKind has exactly one token");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

constexpr bool tokens_well_formed() {
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		if (!kTokens[i][0]) return false;
		for (size_t j = i + 1; j < kViewportKindCount; ++j)
			if (same_text(kTokens[i], kTokens[j])) return false;
	}
	return true;
}
static_assert(tokens_well_formed(), "each ViewportKind has a token of its own");

} // namespace

const char *viewport_kind_token(ViewportKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < kViewportKindCount ? kTokens[index] : "";
}

bool viewport_kind_from_token(const std::string &token, ViewportKind &out) {
	for (size_t i = 0; i < kViewportKindCount; ++i)
		if (token == kTokens[i]) {
			out = static_cast<ViewportKind>(i);
			return true;
		}
	return false;
}

} // namespace opennova::editor
