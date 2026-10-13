// One folder's files as a texture lookup reads them: see texture_folder.h. Tooling (opennova-3di's
// scene), not a port: the game lists no folder, so there is no original function to cite.
#include "texture_folder.h"

#include <base/io/strutil.h>

namespace opennova {

TextureFolder list_texture_folder(const std::filesystem::path &dir) {
	TextureFolder listing;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		try {
			if (!it->is_regular_file(ec))
				continue;
			listing.emplace(
					strutil::to_lower(it->path().filename().u8string()), it->path().u8string());
		} catch (const std::exception &) {
		}
	}
	return listing;
}

} // namespace opennova
