#pragma once

#include <cstdio>

namespace opennova::project {

// A native export utility, separate from the editable-project session verbs.
int run_asset_catalog_command(int argc, const char *const *argv, std::FILE *out, std::FILE *err);

} // namespace opennova::project
