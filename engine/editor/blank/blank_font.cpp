#include "blank_makers.h"

#include <editor/blank/blank_font_art.h>
#include <formats/fnt/fnt.h>

namespace opennova::editor {

using namespace opennova::fnt;

// One generated glyph set serves every hardcoded font name; the request's name only
// decides where the bytes land.
bool make_blank_font(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	fnt_font_t font{};
	if (blank_font::build_font(&font) != FNT_OK || fnt_validate(&font) != FNT_OK) {
		fnt_free(&font);
		error = make_finding(CoreFinding::BlankFont, DiagnosticSeverity::Error, "The built-in font could not be built.",
		                     request.logical_name);
		return false;
	}
	const size_t size = fnt_calculate_file_size(font.num_pages);
	out.assign(size, 0);
	size_t written = 0;
	const bool ok = fnt_write(&font, out.data(), out.size(), &written) == FNT_OK && written == size;
	fnt_free(&font);
	if (!ok) {
		out.clear();
		error = make_finding(CoreFinding::BlankFont, DiagnosticSeverity::Error, "The built-in font could not be written.",
		                     request.logical_name);
	}
	return ok;
}

} // namespace opennova::editor
