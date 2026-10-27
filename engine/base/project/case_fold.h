#pragma once

#include <cstdint>

namespace opennova::project {

// Unicode's simple case folding of one code point (CaseFolding.txt's C and S rows: one code point to one, the form
// a case-insensitive file system's name compare reads two names the same by), the code point itself where it folds
// to none other. Generated: case_fold.cpp, by scripts/project/case_fold_table.py.
uint32_t simple_case_fold(uint32_t code_point);

} // namespace opennova::project
