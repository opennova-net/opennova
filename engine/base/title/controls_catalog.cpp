#include <base/title/controls_catalog.h>

#include <cstring>

namespace opennova::controls {

const ActionDef *Catalog::for_code(int code) const {
	// [orig: KeyBinding_SortBySequentialId @0x498260 — record i ends up as the
	//  row whose code is i; the codes are unique across the static rows]
	if (code < 0 || code >= 768) return nullptr;
	for (std::size_t i = 0; i < count; ++i)
		if (rows[i].code == code) return &rows[i];
	return nullptr;
}

const ActionDef *Catalog::for_token(const char *token) const {
	if (token == nullptr) return nullptr;
	for (std::size_t i = 0; i < count; ++i)
		if (std::strcmp(rows[i].token, token) == 0) return &rows[i];
	return nullptr;
}

} // namespace opennova::controls
