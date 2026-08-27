#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "devtools/imgui_pass_node.h"

#include <runtime/devtools/oned_ui.h>

#include <memory>

namespace godot {

// One edge the ONED surface reported: the action and, for SELECT_RECENT, the
// picked index (ADR 0017 typed record).
class OnedUiRequest : public RefCounted {
	GDCLASS(OnedUiRequest, RefCounted)

public:
	int get_action() const { return action_; }
	int get_index() const { return index_; }
	void assign(const opennova::devtools::OnedRequest &p_request);

protected:
	static void _bind_methods();

private:
	int action_ = 0;
	int index_ = -1;
};

// ONED's run surface (ADR 0037 scope, ADR 0039 toolkit): the engine draws the
// one window (engine/runtime/devtools/oned_ui.h); this node is its seam. The
// app seeds and reads the text fields, pushes the state only it has (recents,
// readiness, running, status) and drains the typed requests every frame —
// process spawning, native directory dialogs and settings persistence stay in
// GDScript. push_request() is the automation/test entry: the same edges the
// mouse produces.
class OnedUi : public ImGuiPassNode {
	GDCLASS(OnedUi, ImGuiPassNode)

public:
	enum Action {
		NONE = static_cast<int>(opennova::devtools::OnedAction::NONE),
		EDIT_RESOURCE_DIR = static_cast<int>(opennova::devtools::OnedAction::EDIT_RESOURCE_DIR),
		APPLY_RESOURCE_DIR = static_cast<int>(opennova::devtools::OnedAction::APPLY_RESOURCE_DIR),
		APPLY_RETAIL_DIR = static_cast<int>(opennova::devtools::OnedAction::APPLY_RETAIL_DIR),
		COMMIT_PROFILE = static_cast<int>(opennova::devtools::OnedAction::COMMIT_PROFILE),
		BROWSE_RESOURCE_DIR = static_cast<int>(opennova::devtools::OnedAction::BROWSE_RESOURCE_DIR),
		BROWSE_RETAIL_DIR = static_cast<int>(opennova::devtools::OnedAction::BROWSE_RETAIL_DIR),
		SELECT_RECENT = static_cast<int>(opennova::devtools::OnedAction::SELECT_RECENT),
		CLEAR_RECENTS = static_cast<int>(opennova::devtools::OnedAction::CLEAR_RECENTS),
		RUN_OPENNOVA = static_cast<int>(opennova::devtools::OnedAction::RUN_OPENNOVA),
		RUN_RETAIL = static_cast<int>(opennova::devtools::OnedAction::RUN_RETAIL),
		STOP = static_cast<int>(opennova::devtools::OnedAction::STOP),
	};

	OnedUi();
	~OnedUi() override;

	void set_resource_dir(const String &p_value);
	String get_resource_dir() const;
	void set_game_code(const String &p_value);
	String get_game_code() const;
	void set_expansion(const String &p_value);
	String get_expansion() const;
	void set_retail_dir(const String &p_value);
	String get_retail_dir() const;

	void set_recent_dirs(const PackedStringArray &p_dirs);
	PackedStringArray get_recent_dirs() const;
	// Empty block = enabled; a non-empty block disables the action and is its tooltip.
	void set_readiness(const String &p_opennova_block, const String &p_retail_block, bool p_running);
	String get_opennova_block() const;
	String get_retail_block() const;
	bool is_running() const;
	// kind: &"info", &"warn" or &"error".
	void set_status(const String &p_text, const StringName &p_kind);
	String get_status_text() const;
	StringName get_status_kind() const;

	// The request queue: null when empty.
	Ref<OnedUiRequest> take_request();
	bool has_requests() const;
	void push_request(int p_action, int p_index = -1);

protected:
	static void _bind_methods();
	opennova::devtools::ImGuiPass *engine_pass() override;

private:
	std::unique_ptr<opennova::devtools::OnedUi> ui_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::OnedUi::Action);
