#pragma once

#include <cstddef>
#include <string>

namespace opennova::nw_lister {

// The process's io::log sink: timestamped lines on stderr and, with a path, appended to a file.
// Debug lines show only when `verbose`. Every registered secret is masked first, so an account
// name, a password or a session tag never reaches a terminal or a file.
void install_console_log(const std::string &file_path, bool verbose);
// Register a value the log must never show (ignored when shorter than two characters).
void add_log_secret(const std::string &secret);
// "abcd...(48)": enough of a value to correlate log lines, never the whole of it.
std::string mask_value(const std::string &value, size_t keep = 4);

} // namespace opennova::nw_lister
