#include "refs/ref_graph.h"

#include <utility>

#include "extractors.h"

namespace opennova::refs {

BuildStats RefGraph::build(const std::vector<GraphFileInfo>& files, const ReadFileFn& read_file) {
    BuildStats stats;
    stats.files = files.size();

    std::map<std::string, Entry> next;
    std::map<std::string, std::string> next_errors;
    for (const GraphFileInfo& info : files) {
        if (!can_extract(info.path)) {
            continue;
        }
        stats.recognized++;
        const std::string key = detail::lower_ascii(info.path);

        // Memo: an unchanged (size, mtime) stamp keeps the prior edges. A zero
        // mtime (PFF member) never matches, so archive entries re-extract.
        auto prior = by_source_.find(key);
        if (prior != by_source_.end() && info.modified_time != 0 &&
            prior->second.modified_time == info.modified_time &&
            prior->second.size_bytes == info.size_bytes) {
            stats.memo_hits++;
            next.emplace(key, std::move(prior->second));
            by_source_.erase(prior);
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!read_file(info.path, bytes)) {
            stats.failed++;
            next_errors[key] = "unreadable";
            continue;
        }
        Entry entry;
        entry.size_bytes = info.size_bytes;
        entry.modified_time = info.modified_time;
        std::string error;
        if (!extract(info.path, bytes.data(), bytes.size(), entry.edges, error)) {
            stats.failed++;
            next_errors[key] = error;
            continue;
        }
        stats.extracted++;
        next.emplace(key, std::move(entry));
    }

    by_source_ = std::move(next);
    errors_ = std::move(next_errors);
    rebuild_target_index();
    return stats;
}

std::vector<Reference> RefGraph::references_of(const std::string& path) const {
    auto it = by_source_.find(detail::lower_ascii(path));
    return it == by_source_.end() ? std::vector<Reference>() : it->second.edges;
}

std::vector<Reference> RefGraph::referrers_of(const std::string& target_name) const {
    auto it = by_target_.find(detail::lower_ascii(target_name));
    return it == by_target_.end() ? std::vector<Reference>() : it->second;
}

std::vector<Reference> RefGraph::broken_references(const ExistsFn& exists) const {
    std::vector<Reference> broken;
    for (const auto& [key, entry] : by_source_) {
        for (const Reference& edge : entry.edges) {
            if (!exists(edge.target_name, edge.target_kind)) {
                broken.push_back(edge);
            }
        }
    }
    return broken;
}

void RefGraph::rebuild_target_index() {
    by_target_.clear();
    for (const auto& [key, entry] : by_source_) {
        for (const Reference& edge : entry.edges) {
            by_target_[detail::lower_ascii(edge.target_name)].push_back(edge);
        }
    }
}

}  // namespace opennova::refs
