#pragma once

#include <string>
#include <vector>

namespace opennova::lister {

// Timestamped logging to stderr and (optionally) a log file. Every line passes
// through redact(): any registered secret (the account name/password, session
// tags) is replaced by a mask before it can reach a terminal or file.
void log_open_file(const std::string &path);
void log_add_secret(const std::string &secret);
void log_set_verbose(bool verbose);
bool log_verbose();
void logf(const char *fmt, ...);
void vlogf(const char *fmt, ...); // only when verbose
std::string redact(std::string text);
// "abcd…(48)" — enough to correlate, never the whole value.
std::string mask_value(const std::string &value, size_t keep = 4);

} // namespace opennova::lister
