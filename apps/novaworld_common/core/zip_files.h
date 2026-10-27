#pragma once

// Zip archives over the vendored miniz (third_party/miniz): an app's uploaded build unpacked for
// its release build (products/release_build.h), and a folder packed whole. An entry whose name
// would leave the folder (empty, absolute, a drive, a backslash, a control byte, an empty, "." or
// ".." part) refuses the whole archive before a byte is written, as does an encrypted or
// unsupported one. Platform plumbing, not a port.

#include <string>

namespace opennova::novaworld_server {

// Every file of the zip `zip` written under `dir` (made, with each file's folders, when missing),
// a directory entry made empty; false with `error` (the archive does not read, an entry it
// refuses, a file that does not write), the files written before it left.
bool unzip_files(const std::string &zip, const std::string &dir, std::string &error);

// Every regular file under `dir`, by its path from it ('/'-separated, in byte order), packed into
// the new zip `zip`; false with `error`, no zip left.
bool zip_directory(const std::string &dir, const std::string &zip, std::string &error);

} // namespace opennova::novaworld_server
