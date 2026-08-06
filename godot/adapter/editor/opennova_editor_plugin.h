#pragma once

#include <godot_cpp/classes/editor_import_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

namespace godot {

class OpenNovaEditorPlugin : public EditorPlugin {
	GDCLASS(OpenNovaEditorPlugin, EditorPlugin)

	Ref<EditorImportPlugin> fnt_importer_;

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
};

} // namespace godot
