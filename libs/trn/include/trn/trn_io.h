#pragma once

#include "trn.h"

#include <istream>
#include <ostream>
#include <string>

namespace opennova {

bool load_trn(std::istream &input, TrnConfig &out, std::string &error);
bool save_trn(std::ostream &output, const TrnConfig &cfg, std::string &error);

} // namespace opennova
