#include <editor/session/view/navigation_view.h>

namespace opennova::editor {

bool same_place(const NavigationPlace &a, const NavigationPlace &b) {
	if (a.pane != b.pane || a.path != b.path) return false;
	if (a.pane != NavigationPlace::Pane::Document) return true;
	// The same record of the same instance; else the same locator, which names it in any read of the file.
	if (a.document != 0 && a.document == b.document && (a.record.row || b.record.row)) return a.record == b.record;
	return a.locator == b.locator;
}

const char *navigation_pane_token(NavigationPlace::Pane pane) {
	switch (pane) {
		case NavigationPlace::Pane::Document:
			return "document";
		case NavigationPlace::Pane::Page:
			return "page";
		case NavigationPlace::Pane::Files:
			return "files";
	}
	return "document";
}

} // namespace opennova::editor
