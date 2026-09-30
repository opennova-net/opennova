#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/validation_cache.h>
#include <editor/model/document.h>
#include <formats/bad/bad.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class AssetGraph;

// A clip (ADR 0046 S10): a `.bad`, whose engine data the editor changes: the header's
// version, frame rate and flags, each bone's name, and each frame's event (the hips'
// ground step, the trigger bits the game fires on that frame, the hips' and the head's
// heights). The keys and translation rows are the clip's motion, authored in Blender
// (the `.o3a` import brings a new one): they stay in the immutable parsed base
// (`assets::parse_bone_animation`) and are written as read. One row, the clip, holds its
// bones and its events (both fixed: their count is the motion's). serialize() writes
// from scratch through the engine's writer (bad_write.h); an untouched clip reads back
// field-equal (ctest anim_retail_rewrite).

enum class AnimationKind : NodeKind { Clip = 0, Bone = 1, Event = 2 };
constexpr NodeKind node_kind(AnimationKind kind) { return static_cast<NodeKind>(kind); }

struct ClipRow : Node {
	assets::BoneAnimation base;
	std::string clip_name;         // the file's stem (the clip has no name of its own)
	uint32_t version = 1, fps = 30, flags = 0;
	std::vector<bad::BadBone> bones;
	std::vector<bad::BadEvent> events;
	// collections: 0 bones, 1 events.

	ClipRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<ClipRow>(*this); }
	std::string name() const override { return clip_name; }
};

class AnimationDocument : public Document {
public:
	// The clip, its row (the file's one, never added), then its bones and frame events.
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const override;
	const std::vector<FieldSchema> &fields(NodeKind kind) const override;
	// A bone's parent: none (a root), or one of the clip's bones by name.
	bool record_choices(const NodeAddress &address, const FieldUse &use,
			std::vector<FieldChoice> &out) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<AnimationDocument>(*this);
	}

	const ClipRow *clip() const;

protected:
	// An event's trigger word only on a version 1 clip (version 0 events carry none); a
	// bone's parent chosen among the clip's bones, shown by name (record_choices).
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, std::string &error) override;
	// The translation flag is refused (the rows it promises are the motion's); version 0
	// is refused while an event fires a trigger, and a trigger on a version 0 clip.
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
	bool accept_change(const Change &change, std::string &error) const override;
};

bool is_animation_kind(AssetKind kind);

// The clip document type's validator (document_types): every clip loads, open documents
// standing in for their files; a frame rate other than the 30 every retail clip plays at
// and an event bit the engine does not read are notes; a bone whose parent does not come
// before it (bad::bad_parent_in_order, the rule the runtime's rig is FK-safe by) is a
// warning on its parent.
std::vector<Diagnostic> validate_animations(const ValidationInput &input, const AssetGraph &graph);

} // namespace opennova::editor
