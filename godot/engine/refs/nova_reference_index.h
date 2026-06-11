#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <refs/ref_graph.h>
#include <refs/refs.h>

#include "../resource_index/nova_resource_root.h"

namespace godot {

// The editor-facing reference index: libs/refs (extractors + RefGraph) bound
// over a mounted NovaResourceRoot. references_of extracts one file on demand;
// the first referrers_of (or stats) query triggers the lazy whole-root build,
// memoized inside RefGraph on (size, mtime) so later rebuilds only re-extract
// changed files. The index self-invalidates against NovaResourceRoot's global
// cache epoch like every other cache holder.
//
// Resolution policy lives HERE (the caller side): edges come back annotated
// with { status: "found"|"missing"|"unprobed", target_path } by probing
// kind-specific candidate names through the root — textures reuse
// texture_candidate_filenames (the cached resolver's fallback list), header
// refs get their engine extension appended (terrain/.trn, environment/.env,
// object_model/.3di, font/.fnt, anim_def/.adm, menu/.mnu, sound/.lwf,
// strings verbatim-then-.bin, datasource verbatim), and name-table kinds that
// are not files (sound_profile = a set inside a bank, string_id = a key
// inside a string table) stay "unprobed" rather than reporting a fake miss —
// per-key statusing belongs to callers holding the table context.
class NovaReferenceIndex : public RefCounted {
	GDCLASS(NovaReferenceIndex, RefCounted)

	Ref<NovaResourceRoot> root_;
	opennova::refs::RefGraph graph_;
	opennova::refs::BuildStats stats_;
	bool built_ = false;
	int64_t built_epoch_ = -1;

	void check_epoch();
	void ensure_graph();
	bool read_bytes(const String &path, std::vector<uint8_t> &out) const;
	Dictionary edge_to_dictionary(const opennova::refs::Reference &edge);

protected:
	static void _bind_methods();

public:
	void set_resource_root(const Ref<NovaResourceRoot> &root);

	// Outgoing references of one file (VFS logical name, or an absolute path the
	// editor opened from disk). On-demand single-file extraction — never triggers
	// the whole-root build. Array of edge dictionaries:
	// { source_path, source_kind, target_name, target_kind, site, status, target_path }.
	Array references_of(const String &path);

	// Incoming references: every edge in the mounted root naming `target_name`
	// (case-insensitive). Triggers the lazy whole-root build on first use.
	Array referrers_of(const String &target_name);

	// Probe `target_name` of `target_kind` through the root's resolution
	// semantics: { "status": "found"|"missing"|"unprobed", "path": String }.
	Dictionary resolve(const String &target_kind, const String &target_name);

	// True once the whole-root graph is built for the CURRENT cache epoch (an
	// epoch bump reads as not-built until the next query rebuilds).
	bool is_built();
	// Last build's counters: { files, recognized, extracted, memo_hits, failed }.
	// Triggers the build (it reports on the whole root by definition).
	Dictionary get_build_stats();
};

} // namespace godot
