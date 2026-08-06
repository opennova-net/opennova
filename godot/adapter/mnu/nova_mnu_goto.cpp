#include "nova_mnu_goto.h"

#include "nova_mnu_menu.h"

using namespace godot;

void NovaMnuGoto::trigger() {
	if (edit_mode_ || menu_ == nullptr) {
		return;
	}
	for (const MnuActionData &action : actions_) {
		menu_->dispatch_widget_action(action);
	}
}

void NovaMnuGoto::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_hotkey"), &NovaMnuGoto::get_hotkey);
	ClassDB::bind_method(D_METHOD("get_fire_on_show"), &NovaMnuGoto::get_fire_on_show);
	ClassDB::bind_method(D_METHOD("get_action_count"), &NovaMnuGoto::get_action_count);
	ClassDB::bind_method(D_METHOD("trigger"), &NovaMnuGoto::trigger);
}
