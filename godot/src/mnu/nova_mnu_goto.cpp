#include "nova_mnu_goto.h"

#include "nova_mnu_menu.h"

using namespace godot;

void MnuGoto::trigger() {
	if (edit_mode_ || menu_ == nullptr) {
		return;
	}
	for (const MnuActionData &action : actions_) {
		menu_->dispatch_widget_action(action);
	}
}

void MnuGoto::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_hotkey"), &MnuGoto::get_hotkey);
	ClassDB::bind_method(D_METHOD("get_fire_on_show"), &MnuGoto::get_fire_on_show);
	ClassDB::bind_method(D_METHOD("get_action_count"), &MnuGoto::get_action_count);
	ClassDB::bind_method(D_METHOD("trigger"), &MnuGoto::trigger);
}
