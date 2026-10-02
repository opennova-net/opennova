#include <editor/preview/model_preview_rig.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/viewport_follow.h>
#include <formats/adm/adm.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/entity_pose.h>

namespace opennova::editor {

namespace {

std::string file_of(const std::string &path) { return path.substr(path.find_last_of("/\\") + 1); }

bool same_clip(const std::string &a, const std::string &b) {
	return assets::asset_file_name(a, ".bad") == assets::asset_file_name(b, ".bad");
}

// A model's file name as the scan lists it, from a graph target (its extension optional).
std::string model_file(const AssetScan &scan, const std::string &target) {
	const AssetEntry *entry = scan.find(target);
	if (!entry && !strutil::ends_with_icase(target, ".3di")) entry = scan.find(target + ".3di");
	return entry && entry->kind == AssetKind::Model ? entry->logical_name : std::string();
}

} // namespace

std::shared_ptr<const adm::AdmFile> PreviewRigFiles::animation_map(const std::string &name) const {
	const std::string key = assets::asset_file_name(name, ".adm");
	std::vector<uint8_t> bytes;
	if (key.empty() || !files_ || !files_->read(key, bytes) || bytes.empty()) return {};
	return assets::parse_animation_map(bytes.data(), bytes.size());
}

std::shared_ptr<const bad::BadFile> PreviewRigFiles::bone_animation(const std::string &name) const {
	const std::string key = assets::asset_file_name(name, ".bad");
	std::vector<uint8_t> bytes;
	if (key.empty() || !files_ || !files_->read(key, bytes) || bytes.empty()) return {};
	return assets::parse_bone_animation(bytes.data(), bytes.size());
}

PreviewRig resolve_preview_rig(const AssetGraph &graph, const AssetScan &scan, const std::string &file, AssetKind kind,
                               const std::string &chosen) {
	PreviewRig rig;
	// A rig's files go by name, as the game finds them: the scan's first file of the name, which
	// the graph's queries take by its path.
	const auto path_of = [&scan](const std::string &name) {
		const AssetEntry *entry = scan.find(name);
		return entry ? entry->relative_path : name;
	};
	if (kind == AssetKind::AnimationMap) {
		rig.table = file;
	} else {
		rig.clip = file;
		for (const GraphEdge *edge : graph.referrers_of_file(path_of(file)))
			if (edge->kind == ReferenceKind::Animation) {
				rig.table = file_of(edge->source);
				break;
			}
	}
	if (!chosen.empty()) {
		rig.model = chosen;
		rig.source = "chosen";
		return rig;
	}
	if (rig.table.empty()) return rig;
	for (const GraphEdge *edge : graph.referrers_of_file(path_of(rig.table))) {
		if (edge->kind != ReferenceKind::AnimationMap || edge->field != "anim_def") continue;
		for (const GraphEdge *graphic : graph.references_of(edge->source)) {
			if (graphic->record != edge->record || graphic->kind != ReferenceKind::Model) continue;
			const std::string model = model_file(scan, graphic->target);
			if (model.empty()) continue;
			rig.model = model;
			rig.source = file_of(edge->source) + ": " + edge->record;
			return rig;
		}
	}
	return rig;
}

std::shared_ptr<const anim::SkeletalClips> load_preview_rig(const PreviewRig &rig, const threedi::Threedi3di3 &model,
                                                           const anim::RigFiles &files) {
	std::vector<anim::Vec3> origins;
	std::vector<int> parents;
	world::model_bone_table(model, origins, parents);
	auto clips = std::make_shared<anim::SkeletalClips>();
	const bool loaded = rig.table.empty()
	                            ? !rig.clip.empty() &&
	                                      clips->load_from_files(&files, rig.clip, {{kPreviewLoneClipKey, rig.clip}}, origins, parents)
	                            : clips->load_from_adm(&files, rig.table, origins, parents);
	return loaded ? clips : nullptr;
}

bool preview_clip_of(const Document &document, const NodeAddress &selection, const PreviewRig &rig,
                     const anim::SkeletalClips &clips, std::string &key, int &variant) {
	key.clear();
	variant = 0;
	// A table's row and clip are the entry and token the rig's loader registered them
	// from (it reads this document's rows, unsaved ones included, as the table's file; a
	// table that cannot be written is not previewed at all): the selected clip, else the
	// row's first clip the loader registered.
	if (const auto *table = dynamic_cast<const AnimationMapDocument *>(&document)) {
		const auto &rows = table->rows();
		const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto &r) { return r && r->id == selection.row; });
		if (row == rows.end()) return false;
		const size_t entry = size_t(row - rows.begin());
		const std::vector<NodeId> &tokens = (*row)->collections[0];
		const auto chosen = selection.child ? std::find(tokens.begin(), tokens.end(), selection.child) : tokens.end();
		if (chosen != tokens.end()) {
			variant = clips.variant_of(entry, size_t(chosen - tokens.begin()), key);
			return variant >= 0;
		}
		for (size_t token = 0; token < tokens.size(); ++token)
			if ((variant = clips.variant_of(entry, token, key)) >= 0) return true;
		variant = 0;
		return false;
	}
	// A clip plays as the first token that registered its file.
	if (!dynamic_cast<const AnimationDocument *>(&document)) return false;
	for (const anim::SkeletalClips::LoadedClip &loaded : clips.clips())
		if (same_clip(loaded.source.file, rig.clip)) {
			variant = clips.variant_of(loaded.source.entry, loaded.source.token, key);
			return true;
		}
	return false;
}

std::vector<PreviewClipEvent> preview_clip_events(const anim::SkeletalClips &rig, const std::string &key, int variant,
                                                  const bad::BadFile &clip_file) {
	std::vector<PreviewClipEvent> out;
	const anim::SkeletalClips::LoadedClip *clip = rig.find_clip_variant(key, variant);
	if (!clip || !clip_file.events) return out;
	const std::vector<int32_t> ticks = clip->clip.playback().first_ticks();
	for (size_t frame = 0; frame < ticks.size() && frame < clip_file.num_events; ++frame) {
		const uint32_t trigger = static_cast<uint32_t>(clip_file.events[frame].trigger);
		if (trigger && ticks[frame] >= 0) out.push_back({int(frame), ticks[frame], trigger});
	}
	return out;
}

} // namespace opennova::editor
