#pragma once

#include "tpj.h"

#include <istream>
#include <ostream>
#include <string>

namespace opennova {

bool load_tpj(std::istream &input, TpjProject &out, std::string &error);
bool save_tpj(std::ostream &output, const TpjProject &project, std::string &error);

TpjProject load_tpj_file(const std::string &filepath);
void save_tpj_file(const std::string &filepath, const TpjProject &project);

} // namespace opennova
