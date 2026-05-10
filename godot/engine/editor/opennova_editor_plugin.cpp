#include "editor/opennova_editor_plugin.h"

#include "fnt/fnt_import_plugin.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void OpenNovaEditorPlugin::_bind_methods() {}

void OpenNovaEditorPlugin::_enter_tree() {
	if (!fnt_importer_.is_valid()) {
		Ref<NovaFntImportPlugin> importer;
		importer.instantiate();
		fnt_importer_ = importer;
	}
	add_import_plugin(fnt_importer_, true);
}

void OpenNovaEditorPlugin::_exit_tree() {
	if (fnt_importer_.is_valid()) {
		remove_import_plugin(fnt_importer_);
		fnt_importer_.unref();
	}
}
