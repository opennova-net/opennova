#include "animation_document.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include <base/io/strutil.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>
#include <formats/bad/bad_write.h>
#include <runtime/anim/anim_event_bits.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kClip = node_kind(AnimationKind::Clip);
constexpr NodeKind kBone = node_kind(AnimationKind::Bone);
constexpr NodeKind kEvent = node_kind(AnimationKind::Event);
constexpr uint32_t kFlagTranslation = bad::BAD_FLAG_TRANSLATION;

FieldSchema field(const char *id, FieldType type, const char *label, const char *section = "", bool read_only = false,
                  size_t width = 0) {
	FieldSchema f;
	f.id = id;
	f.type = type;
	f.width = width;
	f.label = label;
	f.section = section;
	f.read_only = read_only;
	return f;
}

const std::vector<FieldSchema> &clip_fields() {
	static const std::vector<FieldSchema> fields = [] {
		std::vector<FieldSchema> out;
		FieldSchema version = field("version", FieldType::Integer, "Version");
		version.choices = {{"0", 0, "0 (events carry no trigger word)"}, {"1", 1, "1"}};
		out.push_back(version);
		out.push_back(field("fps", FieldType::Integer, "Frames per second"));
		FieldSchema flags = field("flags", FieldType::Integer, "Flags");
		flags.flags = true;
		flags.choices = {{"loop", int64_t(bad::BAD_FLAG_LOOP), "Loops"},
		                 {"translation", int64_t(bad::BAD_FLAG_TRANSLATION), "Translation rows (the motion's)"},
		                 {"bit3", int64_t(bad::BAD_FLAG_BIT3), "Bit 3 (carried; the engine does not read it)"}};
		out.push_back(flags);
		out.push_back(field("frames", FieldType::Count, "Frames", "", true));
		out.push_back(field("bones", FieldType::Count, "Bones", "", true));
		return out;
	}();
	return fields;
}

const std::vector<FieldSchema> &bone_fields() {
	static const std::vector<FieldSchema> fields = {
		field("name", FieldType::Text, "Name", "", false, 32),
		field("parent", FieldType::Integer, "Parent bone", "", true),
		field("length", FieldType::Real, "Length (m)", "", true),
		field("keys", FieldType::Count, "Keys", "", true),
	};
	return fields;
}

const std::vector<FieldSchema> &event_fields() {
	static const std::vector<FieldSchema> fields = [] {
		std::vector<FieldSchema> out = {
			field("velocity.x", FieldType::Real, "Forward (m per frame)", "Hips' ground step"),
			field("velocity.y", FieldType::Real, "Left (m per frame)", "Hips' ground step"),
			field("velocity.z", FieldType::Real, "Up (m per frame)", "Hips' ground step"),
		};
		// The bits the engine fires on this frame [orig: AnimMap_UpdateEntity @0x40b5f0, the
		// out-transform @0x40b82f..0x40b8a3] (runtime/anim/anim_event_bits.h).
		FieldSchema trigger = field("trigger", FieldType::Unsigned, "Triggers");
		trigger.flags = true;
		for (const anim::AnimEventBit &bit : anim::kAnimEventBits)
			trigger.choices.push_back({bit.name, int64_t(bit.mask), bit.what});
		out.push_back(trigger);
		out.push_back(field("bottom", FieldType::Real, "Hips above ground (m)", "Heights"));
		out.push_back(field("top", FieldType::Real, "Head above ground (m)", "Heights"));
		return out;
	}();
	return fields;
}

ClipRow &clip_of(Node &node) { return static_cast<ClipRow &>(node); }
const ClipRow &clip_of(const Node &node) { return static_cast<const ClipRow &>(node); }

// The index of a nested record in its collection, or SIZE_MAX.
size_t index_of(const ClipRow &row, const NodeAddress &address) {
	const size_t c = address.kind == kBone ? 0 : address.kind == kEvent ? 1 : SIZE_MAX;
	if (c == SIZE_MAX || address.child == 0) return SIZE_MAX;
	const auto &ids = row.collections[c];
	const auto found = std::find(ids.begin(), ids.end(), address.child);
	return found == ids.end() ? SIZE_MAX : size_t(found - ids.begin());
}

bad::BadBuildVec3 mission_velocity(const bad::BadEvent &e) {
	return bad::bad_mission_from_clip(bad::BadBuildVec3{e.velocity[0], e.velocity[1], e.velocity[2]});
}

uint32_t known_trigger_bits() {
	uint32_t mask = 0;
	for (const anim::AnimEventBit &bit : anim::kAnimEventBits) mask |= bit.mask;
	return mask;
}

} // namespace

ClipRow::ClipRow() {
	kind = kClip;
	collections.resize(2);
}

bool is_animation_kind(AssetKind kind) { return kind == AssetKind::Animation; }

const char *AnimationDocument::kind_label(NodeKind kind) const {
	return kind == kClip ? "Clip" : kind == kBone ? "Bone" : kind == kEvent ? "Frame event" : "";
}

NodeKind AnimationDocument::kind_from_name(const std::string &name) const {
	if (name == "clip") return kClip;
	if (name == "bone") return kBone;
	if (name == "event") return kEvent;
	return -1;
}

std::vector<Document::Collection> AnimationDocument::collections(const Node &row, const NodeAddress &owner) const {
	if (row.kind != kClip || owner.child != 0) return {};
	CollectionSpec bones{kBone, "Bones", "name", "bone", true};
	CollectionSpec events{kEvent, "Frame events", "", "event", true};
	return {{bones, row.collections[0]}, {events, row.collections[1]}};
}

const std::vector<FieldSchema> &AnimationDocument::fields(NodeKind kind) const {
	static const std::vector<FieldSchema> none;
	return kind == kClip ? clip_fields() : kind == kBone ? bone_fields() : kind == kEvent ? event_fields() : none;
}

FieldSchema AnimationDocument::field_on(const NodeAddress &address, const FieldSchema &field) const {
	FieldSchema out = field;
	const ClipRow *row = clip();
	if (row && address.kind == kEvent && field.id == "trigger" && row->version == 0) out.applies = Applicability::Ignored;
	// A bone's parent by the parent bone's name (the file writes its place): the clip's own bones.
	if (row && address.kind == kBone && field.id == "parent") {
		out.choices.push_back({"-1", -1, "None (a root)"});
		for (size_t i = 0; i < row->bones.size(); ++i) out.choices.push_back({std::to_string(i), int64_t(i), row->bones[i].name});
	}
	return out;
}

bool AnimationDocument::read(const Node &node, const NodeAddress &address, const std::string &field, Value &out) const {
	if (node.kind != kClip) return false;
	const ClipRow &row = clip_of(node);
	if (address.child == 0) {
		if (field == "version") out = int64_t(row.version);
		else if (field == "fps") out = int64_t(row.fps);
		else if (field == "flags") out = int64_t(row.flags);
		else if (field == "frames") out = int64_t(row.base ? row.base->frame_count : 0);
		else if (field == "bones") out = int64_t(row.bones.size());
		else return false;
		return true;
	}
	const size_t i = index_of(row, address);
	if (i == SIZE_MAX) return false;
	if (address.kind == kBone) {
		const bad::BadBone &b = row.bones[i];
		if (field == "name") out = std::string(b.name);
		else if (field == "parent") out = int64_t(b.parent_index);
		else if (field == "length") out = double(b.length);
		else if (field == "keys") out = int64_t(row.base && i < row.base->num_channels ? row.base->channels[i].frame_count : 0);
		else return false;
		return true;
	}
	const bad::BadEvent &e = row.events[i];
	if (field == "velocity.x" || field == "velocity.y" || field == "velocity.z") {
		const bad::BadBuildVec3 m = mission_velocity(e);
		out = field.back() == 'x' ? m.x : field.back() == 'y' ? m.y : m.z;
	} else if (field == "trigger") out = int64_t(static_cast<uint32_t>(e.trigger));
	else if (field == "bottom") out = double(e.bottom);
	else if (field == "top") out = double(e.top);
	else return false;
	return true;
}

const ClipRow *AnimationDocument::clip() const {
	for (const auto &r : rows())
		if (r && r->kind == kClip) return static_cast<const ClipRow *>(r.get());
	return nullptr;
}

SerializeResult AnimationDocument::serialize() const {
	SerializeResult result;
	const ClipRow *row = clip();
	if (!row || !row->base) {
		result.issues.push_back({true, 0, "", "", "The clip could not be written."});
		return result;
	}
	bad::BadFile file = *row->base; // shallow: the channels and translations are the base's
	file.version = row->version;
	file.fps = row->fps;
	file.flags = row->flags;
	file.bones = const_cast<bad::BadBone *>(row->bones.data());
	file.num_bones = row->bones.size();
	file.events = const_cast<bad::BadEvent *>(row->events.data());
	file.num_events = row->events.size();
	std::vector<uint8_t> bytes;
	if (bad::bad_write_buffer(&file, bytes) != 0) {
		result.issues.push_back({true, 0, row->clip_name, "", "The clip could not be written (a bone name of 32 characters?)."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

bool AnimationDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                              std::shared_ptr<const FileState> &, std::vector<SourceIssue> &, Diagnostic &error) {
	if (!is_animation_kind(kind())) {
		error = make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This file is not a clip.", path());
		return false;
	}
	const assets::BoneAnimation base = assets::parse_bone_animation(bytes.data(), bytes.size());
	if (!base) {
		error = make_diagnostic(DiagnosticSeverity::Error, "document.parse", "The clip could not be read.", path());
		return false;
	}
	auto row = std::make_shared<ClipRow>();
	row->base = base;
	row->clip_name = std::filesystem::path(path()).stem().generic_string();
	row->version = base->version;
	row->fps = base->fps;
	row->flags = base->flags;
	row->bones.assign(base->bones, base->bones + base->num_bones);
	row->events.assign(base->events, base->events + base->num_events);
	row->collections[0].resize(row->bones.size());
	row->collections[1].resize(row->events.size());
	rows.push_back(row);
	return true;
}

std::shared_ptr<Node> AnimationDocument::make_node(NodeKind, NodeId, std::string &error) {
	error = "A clip is one record: import a clip set (.o3a) for another.";
	return nullptr;
}

bool AnimationDocument::set_field(Node &node, const NodeAddress &address, const std::string &field, const Value &value,
                                  std::string &error) {
	ClipRow &row = clip_of(node);
	// The value the row being changed holds (an earlier edit of the batch may have set it).
	Value current;
	if (!read(node, address, field, current)) {
		error = "Unknown field.";
		return false;
	}
	if (same_value(current, value)) return true;
	const int64_t *whole = std::get_if<int64_t>(&value);
	const double *number = std::get_if<double>(&value);
	const std::string *text = std::get_if<std::string>(&value);
	const auto real = [&](double &out) {
		if (number) out = *number;
		else if (whole) out = double(*whole);
		else {
			error = "This field takes a number.";
			return false;
		}
		if (!std::isfinite(out)) {
			error = "A finite number.";
			return false;
		}
		return true;
	};
	if (address.child == 0) {
		if (!whole) {
			error = "This field takes a whole number.";
			return false;
		}
		if (field == "version") {
			if (*whole != 0 && *whole != 1) {
				error = "A clip is version 0 or 1.";
				return false;
			}
			if (*whole == 0)
				for (const bad::BadEvent &e : row.events)
					if (e.trigger != 0) {
						error = "A version 0 clip carries no trigger word: clear the events' triggers first.";
						return false;
					}
			row.version = uint32_t(*whole);
		} else if (field == "fps") {
			if (*whole < 1 || *whole > 1000) {
				error = "A frame rate from 1 to 1000.";
				return false;
			}
			row.fps = uint32_t(*whole);
		} else if (field == "flags") {
			if (*whole < 0 || *whole > 0xFFFFFFFFll) {
				error = "The flags are a 32-bit word.";
				return false;
			}
			if ((uint32_t(*whole) ^ row.flags) & kFlagTranslation) {
				error = "The translation rows are the clip's motion: export the clip again from Blender to add or drop them.";
				return false;
			}
			row.flags = uint32_t(*whole);
		} else {
			error = "This field is read-only.";
			return false;
		}
		return true;
	}
	const size_t i = index_of(row, address);
	if (i == SIZE_MAX) {
		error = "The record no longer exists.";
		return false;
	}
	if (address.kind == kBone) {
		if (field != "name") {
			error = "This field is the motion's: export the clip again from Blender to change it.";
			return false;
		}
		if (!text || text->size() > 31 || text->find('\0') != std::string::npos) {
			error = "A bone name holds at most 31 characters.";
			return false;
		}
		std::memset(row.bones[i].name, 0, sizeof(row.bones[i].name));
		std::memcpy(row.bones[i].name, text->data(), text->size());
		return true;
	}
	bad::BadEvent &e = row.events[i];
	if (field == "trigger") {
		if (!whole || *whole < 0 || *whole > 0xFFFFFFFFll) {
			error = "The triggers are a 32-bit word.";
			return false;
		}
		if (row.version == 0) {
			error = "A version 0 clip carries no trigger word: make it version 1 first.";
			return false;
		}
		e.trigger = int32_t(uint32_t(*whole));
		return true;
	}
	double v = 0.0;
	if (!real(v)) return false;
	if (field == "bottom") e.bottom = float(v);
	else if (field == "top") e.top = float(v);
	else {
		bad::BadBuildVec3 m = mission_velocity(e);
		(field.back() == 'x' ? m.x : field.back() == 'y' ? m.y : m.z) = v;
		const bad::BadBuildVec3 c = bad::bad_clip_from_mission(m);
		e.velocity[0] = float(c.x);
		e.velocity[1] = float(c.y);
		e.velocity[2] = float(c.z);
	}
	return true;
}

bool AnimationDocument::edit_collection(Node &, const Edit &, const IdAllocator &, NodeId &, std::string &error) {
	error = "The bones and frames are the clip's motion: export the clip again from Blender to change them.";
	return false;
}

bool AnimationDocument::set_file_value(std::shared_ptr<const FileState> &, const Edit &, Diagnostic &error) {
	error = make_diagnostic(DiagnosticSeverity::Error, "document.value", "A clip has no file-wide values.", path());
	return false;
}

bool AnimationDocument::accept_change(const Change &change, std::string &error) const {
	if (!change.before || !change.after) {
		error = "A clip is one record.";
		return false;
	}
	return true;
}

std::vector<Diagnostic> validate_animations(const ValidationInput &input, const AssetGraph &) {
	std::vector<Diagnostic> findings;
	const uint32_t known = known_trigger_bits();
	for (const auto &asset : input.scan.entries) {
		if (!is_animation_kind(asset.kind)) continue;
		Diagnostic error;
		const std::shared_ptr<const Document> document = input.document(asset, error);
		if (!document) {
			findings.push_back(error);
			continue;
		}
		const auto *clip_document = dynamic_cast<const AnimationDocument *>(document.get());
		const ClipRow *row = clip_document ? clip_document->clip() : nullptr;
		if (!row) continue;
		if (row->fps != 30) {
			Diagnostic d = make_diagnostic(DiagnosticSeverity::Info, "animation.fps",
			                               "This clip plays at " + std::to_string(row->fps) +
			                                       " frames per second; every retail clip plays at 30.",
			                               document->path(), "fps");
			d.row_id = row->id;
			d.record_kind = kClip;
			findings.push_back(std::move(d));
		}
		// A bone whose parent does not come before it: the rig builds a bone's pose on its
		// parent's, in bone order (bad_parent_in_order).
		for (size_t i = 0; i < row->bones.size(); ++i) {
			const int32_t parent = row->bones[i].parent_index;
			if (bad::bad_parent_in_order(parent, i)) continue;
			const std::string name = row->bones[i].name;
			const std::string message =
			        parent >= 0 && size_t(parent) < row->bones.size()
			                ? "Bone " + name + "'s parent, " + row->bones[size_t(parent)].name +
			                          ", is numbered after it: the rig builds each bone's pose on its parent's, in bone "
			                          "order, so the parent's is not built yet when " + name + " reads it."
			                : "Bone " + name + "'s parent, " + std::to_string(parent) +
			                          ", is no bone of the clip: the rig builds each bone's pose on its parent's.";
			Diagnostic d = make_diagnostic(DiagnosticSeverity::Warning, "animation.parent_order", message, document->path(), "parent");
			d.row_id = row->id;
			d.record_kind = kBone;
			d.child_id = row->collections[0][i];
			d.record = document->record_path({row->id, kBone, d.child_id});
			findings.push_back(std::move(d));
		}
		for (size_t i = 0; i < row->events.size(); ++i) {
			if ((static_cast<uint32_t>(row->events[i].trigger) & ~known) == 0) continue;
			Diagnostic d = make_diagnostic(DiagnosticSeverity::Info, "animation.trigger_unknown",
			                               "Frame " + std::to_string(i) + " sets a trigger bit the engine does not read.",
			                               document->path(), "trigger");
			d.row_id = row->id;
			d.record_kind = kEvent;
			d.child_id = row->collections[1][i];
			d.record = document->record_path({row->id, kEvent, d.child_id});
			findings.push_back(std::move(d));
		}
	}
	return findings;
}

} // namespace opennova::editor
