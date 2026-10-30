#pragma once

#include <godot_cpp/variant/string.hpp>

#include <base/io/cp1252.h>

#include <string>

#include "mnu/menu_driver.h"
#include "mnu/mnu_document.h"
#include "util/string_convert.h"

namespace godot {

// A menu runtime's text for a script, as MenuDriver.get_widget_text reads it: a code-page
// document's bytes each its character (D-MNU-46, D-MNU-49), a Unicode document's valid UTF-8 as it
// is and anything else as cp1252 (retail_text_to_utf8). The screen controllers' bindings hand
// their controllers' texts out through it.
inline String menu_text_to_gd(const MenuDriver *p_driver, const std::string &p_text) {
	const Ref<MnuDocument> doc = p_driver != nullptr ? p_driver->document() : Ref<MnuDocument>();
	const bool code_page = doc.is_null() ||
			doc->get_native().source_encoding == opennova::mnu::SourceEncoding::CodePage;
	return code_page ? opennova::cp1252_to_gd(p_text)
					 : opennova::to_gd(opennova::retail_text_to_utf8(p_text));
}

} // namespace godot
