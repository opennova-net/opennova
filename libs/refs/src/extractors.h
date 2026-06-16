// Internal: per-format extractor entry points + the shared edge sink.
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "refs/refs.h"

namespace opennova::refs::detail {

std::string lower_ascii(std::string s);

// Collects edges for one source file, skipping empty targets and duplicates on
// (target_kind, lowercased target_name); the first site wins.
class EdgeSink {
public:
    EdgeSink(std::string source_path, std::string source_kind, std::vector<Reference>& out)
        : source_path_(std::move(source_path)), source_kind_(std::move(source_kind)), out_(out) {}

    void add(const std::string& target_name, const std::string& target_kind, const std::string& site) {
        if (target_name.empty()) {
            return;
        }
        if (!seen_.insert(target_kind + "|" + lower_ascii(target_name)).second) {
            return;
        }
        out_.push_back(Reference{source_path_, source_kind_, target_name, target_kind, site});
    }

private:
    std::string source_path_;
    std::string source_kind_;
    std::vector<Reference>& out_;
    std::set<std::string> seen_;
};

bool extract_env(const std::string& source_path, const uint8_t* data, size_t size,
                 std::vector<Reference>& out, std::string& error);
bool extract_credits(const std::string& source_path, const uint8_t* data, size_t size,
                     std::vector<Reference>& out, std::string& error);
bool extract_items_def(const std::string& source_path, const uint8_t* data, size_t size,
                       std::vector<Reference>& out, std::string& error);
bool extract_avatars_def(const std::string& source_path, const uint8_t* data, size_t size,
                         std::vector<Reference>& out, std::string& error);
bool extract_threedi(const std::string& source_path, const uint8_t* data, size_t size,
                     std::vector<Reference>& out, std::string& error);
bool extract_mission(const std::string& source_path, const uint8_t* data, size_t size,
                     std::vector<Reference>& out, std::string& error);
bool extract_mnu(const std::string& source_path, const uint8_t* data, size_t size,
                 std::vector<Reference>& out, std::string& error);

}  // namespace opennova::refs::detail
