// ASE file writer — serializes Document to 3ds Max ASCII Scene Export format.
#pragma once

#include <string>

#include "ase/types.h"

namespace ase {

struct WriteOptions {
  int precision = 8;  // float decimal places (8 needed for int16/256 roundtrip)
};

// Write a Document to an ASE file. Returns true on success.
bool write_file(const std::string& path, const Document& doc,
                const WriteOptions& opts = WriteOptions{},
                std::string* error = nullptr);

// Write a Document to a string. Returns true on success.
bool write_string(std::string& out, const Document& doc,
                  const WriteOptions& opts = WriteOptions{},
                  std::string* error = nullptr);

}  // namespace ase
