#include <cstdlib>
#include <iostream>
#include <string>

#include "ase/ase_parser.h"

int main() {
  ase::Document doc{};
  std::string error;
  const bool ok = ase::parse_file("does_not_exist.ase", doc, error);
  if (ok) {
    std::cerr << "Unexpected success for missing file\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
