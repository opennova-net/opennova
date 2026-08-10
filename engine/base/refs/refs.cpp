#include "refs/refs.h"

#include <cctype>

#include "extractors.h"

#include <io/strutil.h>

namespace opennova::refs {
namespace detail {

std::string lower_ascii(std::string s) { return opennova::strutil::to_lower(s); }

namespace {

std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string ext_of(const std::string& basename) {
    const size_t dot = basename.find_last_of('.');
    return dot == std::string::npos ? std::string() : basename.substr(dot + 1);
}

}  // namespace
}  // namespace detail

bool can_extract(const std::string& name) {
    const std::string base = detail::lower_ascii(detail::basename_of(name));
    const std::string ext = detail::ext_of(base);
    return ext == "env" || ext == "kda" || ext == "3di" || ext == "bms" || ext == "mis" || ext == "mnu" ||
           base == "items.def" || base == "avatars.def";
}

bool extract(const std::string& source_path, const uint8_t* data, size_t size,
             std::vector<Reference>& out, std::string& error) {
    error.clear();
    if (data == nullptr && size > 0) {
        error = "null data";
        return false;
    }
    const std::string base = detail::lower_ascii(detail::basename_of(source_path));
    const std::string ext = detail::ext_of(base);
    if (ext == "env") {
        return detail::extract_env(source_path, data, size, out, error);
    }
    if (ext == "kda") {
        return detail::extract_credits(source_path, data, size, out, error);
    }
    if (ext == "3di") {
        return detail::extract_threedi(source_path, data, size, out, error);
    }
    if (ext == "bms" || ext == "mis") {
        return detail::extract_mission(source_path, data, size, out, error);
    }
    if (ext == "mnu") {
        return detail::extract_mnu(source_path, data, size, out, error);
    }
    if (base == "items.def") {
        return detail::extract_items_def(source_path, data, size, out, error);
    }
    if (base == "avatars.def") {
        return detail::extract_avatars_def(source_path, data, size, out, error);
    }
    // Unrecognized formats are not an error: callers sweep whole file lists.
    return true;
}

}  // namespace opennova::refs
