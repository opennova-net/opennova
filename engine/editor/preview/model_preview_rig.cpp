#include <editor/preview/model_preview_rig.h>

#include <algorithm>
#include <cstdio>

#include <base/io/strutil.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/animation_slots.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/viewport_follow.h>
#include <formats/adm/adm.h>
#include <runtime/anim/anim_event_bits.h>
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
		if (edge->kind != ReferenceKind::AnimationMap) continue;
		const std::vector<GraphEdge const *> record_edges = graph.references_of(edge->source);
		for (const char *field : preview_model_fields(edge->field))
			for (const GraphEdge *graphic : record_edges) {
				if (graphic->record != edge->record || graphic->kind != ReferenceKind::Model || graphic->field != field) continue;
				const std::string model = model_file(scan, graphic->target);
				if (model.empty()) continue;
				rig.model = model;
				rig.source = file_of(edge->source) + ": " + edge->record;
				rig.pairing = usage_target(scan, *edge);
				rig.record_file = edge->source;
				rig.record = edge->record;
				rig.record_field = edge->field;
				return rig;
			}
	}
	return rig;
}

std::vector<const char *> preview_model_fields(const std::string &map_field) {
	if (map_field == "anim_def") return {"graphic", "graphic_enemy"};
	if (map_field == "animadm") return {"gfx1"};
	return {};
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

PreviewClipChoice preview_clip_choice(const Document &document, const NodeAddress &selection, const PreviewRig &rig,
                                      const anim::SkeletalClips &clips) {
	PreviewClipChoice out;
	// A table's row and clip are the entry and token the rig's loader registered them
	// from (it reads this document's rows, unsaved ones included, as the table's file; a
	// table that cannot be written is not previewed at all): the selected clip, else the
	// row's first clip the loader registered.
	if (const auto *table = dynamic_cast<const AnimationMapDocument *>(&document)) {
		const auto &rows = table->rows();
		const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto &r) { return r && r->id == selection.row; });
		if (row == rows.end()) return out;
		const auto &map_row = static_cast<const AnimationMapRow &>(**row);
		const int slot = animation_key_slot(map_row.key);
		if (slot < 0) {
			out.note = "'" + map_row.key + "' names none of the game's slots: the game skips this row.";
			return out;
		}
		const std::string words = animation_slot_words(slot);
		const size_t entry = size_t(row - rows.begin());
		const std::vector<NodeId> &tokens = (*row)->collections[0];
		// What a token registered: its own file, failsafe.bad in its place, or nothing.
		const auto registered = [&](size_t token, std::string &key) {
			const int variant = clips.variant_of(entry, token, key);
			if (variant < 0) return variant;
			const anim::SkeletalClips::ClipSource *source = clips.find_clip_source(key, variant);
			if (source && token < map_row.clips.size() && !same_clip(source->file, map_row.clips[token]))
				out.note = map_row.clips[token] + " is not in the project: the game plays " + source->file + " in its place.";
			return variant;
		};
		const auto chosen = selection.child ? std::find(tokens.begin(), tokens.end(), selection.child) : tokens.end();
		if (chosen != tokens.end()) {
			const size_t token = size_t(chosen - tokens.begin());
			if ((out.variant = registered(token, out.key)) >= 0) return out;
			out.key.clear();
			if (token < map_row.clips.size())
				out.note = (map_row.clips[token].empty() ? std::string("This clip names no file")
				                                         : map_row.clips[token] + " is not in the project") +
				           ": the game leaves it out of " + words + "'s clips.";
		}
		for (size_t token = 0; token < tokens.size(); ++token) {
			std::string key;
			const int variant = registered(token, key);
			if (variant < 0) continue;
			out.key = key;
			out.variant = variant;
			if (chosen != tokens.end()) out.note += " " + words + " plays " + map_row.clips[token] + " here.";
			return out;
		}
		// No token of the row registered: the slot is unauthored (unless another row names it), and an
		// unauthored slot serves the reset row's first clip, which the body plays only where the slot's
		// selector does not test for its clip (animation_slot_absence).
		out.variant = 0;
		const std::string slot_key = adm::adm_slot_key(map_row.key);
		if (slot != 0 && clips.has_clip(slot_key)) {
			out.key = slot_key;
			out.note = words + " has no clip the game loads on this row: it plays the clips another row gives the slot.";
		} else if (slot != 0 && clips.has_clip("anim_reset")) {
			out.key = "anim_reset";
			const anim::SkeletalClips::ClipSource *reset = clips.find_clip_source(out.key, 0);
			const std::string clip = "the reset clip" + (reset ? ", " + reset->file + "," : std::string());
			const std::string why = words + " has no clip the game loads (" +
			                        (map_row.clips.empty() ? std::string("the row names none")
			                                               : "none of its files is in the project") + "): ";
			switch (animation_slot_absence(slot)) {
			case AnimSlotAbsence::PlaysReset:
				out.note = why + "the game plays " + clip + " in its place.";
				break;
			case AnimSlotAbsence::NotPicked: {
				const std::string instead = animation_slot_instead(slot);
				out.note = why + "the game never picks " + words + " then" +
				           (instead.empty() ? std::string() : " (" + instead + ")") + ". The slot serves " + clip +
				           " shown here; a mission's forced animation of it plays that.";
				break;
			}
			case AnimSlotAbsence::Untraced:
				out.note = why + "the slot serves " + clip + " shown here; whether the game picks the slot then is not traced.";
				break;
			}
		}
		return out;
	}
	// A clip plays as the first token that registered its file.
	if (!dynamic_cast<const AnimationDocument *>(&document)) return out;
	for (const anim::SkeletalClips::LoadedClip &loaded : clips.clips())
		if (same_clip(loaded.source.file, rig.clip)) {
			out.variant = clips.variant_of(loaded.source.entry, loaded.source.token, out.key);
			if (out.variant < 0) out.key.clear();
			return out;
		}
	return out;
}

std::string preview_joint_name(size_t bone, const std::string &clip_name) {
	char word[8];
	std::snprintf(word, sizeof(word), "BN%02u", unsigned(bone + 1));
	// SkeletalClips names a model-table bone past the bind clip's records MDL<i> (its own rule, no
	// name the clip gives): the part's name alone then.
	const bool made = clip_name.size() > 3 && clip_name.compare(0, 3, "MDL") == 0 &&
	                  clip_name.find_first_not_of("0123456789", 3) == std::string::npos;
	if (clip_name.empty() || made) return word;
	if (strutil::starts_with_icase(clip_name, word)) return clip_name;
	return std::string(word) + " " + clip_name;
}

std::vector<PreviewJoint> preview_posed_joints(const anim::SkeletalClips &rig, const std::string &key, int variant,
                                               int32_t ticks) {
	std::vector<PreviewJoint> out;
	if (!rig.loaded() || !rig.fk_valid() || key.empty() || !rig.find_clip_variant(key, variant)) return out;
	std::vector<anim::PoseBone> pose;
	rig.eval_pose(key, rig.clip_seconds_at_tick(key, ticks, variant), variant, pose);
	const std::vector<int> &parents = rig.parents();
	const auto &rest_inverse = rig.rest_global_inverse();
	const size_t count = std::min({pose.size(), parents.size(), rest_inverse.size()});
	// The parent-local FK the skin poses by (anim::pose_globals: each bone's global on its parent's, the
	// parents first, fk_valid; a free model collapses no bone), its deform the global over the rest
	// global's inverse (the matrices world::EntityPoseProvider::build_skeletal composes, before the
	// render-frame swizzle).
	std::vector<anim::SkeletalClips::RestTransform> global;
	anim::pose_globals(pose, parents, count, -1, global);
	out.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		const int parent = parents[i];
		PreviewJoint joint;
		joint.bone = int(i);
		joint.parent = parent >= 0 && size_t(parent) < i ? parent : -1;
		joint.name = preview_joint_name(i, i < rig.bones().size() ? rig.bones()[i].name : std::string());
		joint.at = PreviewVec3{global[i].origin.x, global[i].origin.y, global[i].origin.z};
		joint.deform = anim::rest_mul(global[i], rest_inverse[i]);
		out.push_back(std::move(joint));
	}
	return out;
}

PreviewVec3 preview_joint_carry(const PreviewJoint &joint, const PreviewVec3 &point, bool direction) {
	const anim::Vec3 in{point.x, point.y, point.z};
	const anim::Vec3 o = direction ? anim::rest_transform_direction(joint.deform, in)
	                               : anim::rest_transform_point(joint.deform, in);
	return PreviewVec3{o.x, o.y, o.z};
}

const char *preview_event_letter(uint32_t trigger) {
	if (trigger & (anim::kAnimEventFirePrimary | anim::kAnimEventFireSecondary | anim::kAnimEventFireMarker3)) return "F";
	if (trigger & anim::kAnimEventFootLeft) return "L";
	if (trigger & anim::kAnimEventFootRight) return "R";
	constexpr uint32_t kFoley = ((anim::kAnimEventFoley1 << anim::kAnimEventFoleyCount) - 1) & ~(anim::kAnimEventFoley1 - 1);
	return trigger & kFoley ? "S" : "";
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
