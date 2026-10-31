#pragma once

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/inmatch/lan_multiplayer_menu.h>

#include <memory>

#include "mnu/menu_driver.h"
#include "mnu/mnu_document.h" // menu_driver.h holds its document by Ref

namespace godot {

class LanSession;
class LanServerRow;
class MissionCatalogRow;
class ResourceRoot;

// The LAN multiplayer screens' binding (mp.mnu, the shell's "lan" delegate): the engine's
// inmatch::LanMultiplayerMenu, which holds the screens' rules (the LAN list and its selection, the
// search state, the join's row rule, the host screen's populate, restriction lists, pool, rotation
// and START), over this binding's host: the LanSession device (the search, the enumerator, its
// records, the join's row and socket), the menu's resource root (the mission pool, the title it
// runs as, the expansion the game runs), the text tables and the cfg block through the owner's
// sources, and the session request and the join as signals the owner (MpMenuCompanion) turns
// into its typed records.
class LanMultiplayerMenu : public MenuScreenBinding {
	GDCLASS(LanMultiplayerMenu, MenuScreenBinding)

	class Host;
	friend class Host;

	std::unique_ptr<Host> host_;
	opennova::inmatch::LanMultiplayerMenu menu_;
	ObjectID session_id_;
	Ref<ResourceRoot> root_; // the menu's (set_resource_root)
	// The browse's rows as the session last listed them, aligned with the controller's servers: a
	// session-less owner (the menu seam's harness) joins a row as listed.
	TypedArray<LanServerRow> rows_;
	// (table name) -> RtxtStringFile, and () -> GameConfigBlock: the owner's string registry and
	// the process's cfg block.
	Callable text_source_;
	Callable config_source_;

	LanSession *session_() const;
	void on_servers_changed_(const Array &p_servers);
	void on_browse_error_(const String &p_message);
	void on_browse_finished_();

protected:
	static void _bind_methods();

public:
	LanMultiplayerMenu();
	~LanMultiplayerMenu() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &menu_; }

	// The LAN discovery device; its signals reach the controller. Null: none (the join takes a row
	// as listed, the search does nothing).
	void set_lan_session(LanSession *p_session);
	// The menu's resource root, as the shell hands the document over: the host pool's catalog, the
	// title a session over it runs as (the pool's), the expansion it runs.
	void set_resource_root(const Ref<ResourceRoot> &p_root);
	void set_sources(const Callable &p_text_tables, const Callable &p_config_block);
	// The host pool made anew from catalog rows (the open seeds it from the root's catalog).
	void seed_host_pool(const TypedArray<MissionCatalogRow> &p_rows);
	void set_novaworld_host(bool p_novaworld) { menu_.set_novaworld_host(p_novaworld); }
	bool is_novaworld_host() const { return menu_.novaworld_host(); }
	// The selected LAN row (-1 none) and the host START's gate (ADR 0018 read seams).
	int get_selected_server() const { return menu_.selected_server(); }
	bool can_start_host() const { return menu_.host_dialog().can_start(); }
};

} // namespace godot
