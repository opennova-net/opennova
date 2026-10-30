#include "mnu/lan_multiplayer_menu.h"

#include "mission/mission_catalog.h"
#include "network/join_target.h"
#include "network/lan_server_row.h"
#include "network/lan_session.h"
#include "player/game_config_block.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using opennova::gd_to_retail_text;
using opennova::to_gd;
using opennova::to_std;

namespace godot {

namespace {

std::vector<opennova::menu::MissionChoice> choices_of(const TypedArray<MissionCatalogRow> &p_rows) {
	std::vector<opennova::menu::MissionChoice> choices;
	for (int64_t i = 0; i < p_rows.size(); ++i) {
		const Ref<MissionCatalogRow> row = p_rows[i];
		if (row.is_valid())
			choices.push_back({to_std(row->get_file()), to_std(row->display_text()),
					to_std(row->get_briefing()), static_cast<uint32_t>(row->get_game_type())});
	}
	return choices;
}

} // namespace

// The controller's host over the device legs.
class LanMultiplayerMenu::Host : public opennova::inmatch::LanMultiplayerHost {
	LanMultiplayerMenu *owner_;

	Ref<RtxtStringFile> table_(const char *p_name) const {
		if (!owner_->text_source_.is_valid()) return Ref<RtxtStringFile>();
		return owner_->text_source_.call(String(p_name));
	}
	Ref<ResourceRoot> root_() const {
		LanSession *session = owner_->session_();
		return session != nullptr ? session->get_resource_root() : Ref<ResourceRoot>();
	}

public:
	explicit Host(LanMultiplayerMenu *p_owner) : owner_(p_owner) {}

	std::string menu_text(const std::string &p_key) override {
		const String key = to_gd(p_key);
		const String text = RtxtStringFile::text_or(table_("menutxt"), "Menu", key,
				RtxtStringFile::text_or(table_("gameui"), "Menu", key, String()));
		return gd_to_retail_text(text);
	}
	std::string error_text(const std::string &p_section, const std::string &p_key) override {
		return gd_to_retail_text(
				RtxtStringFile::lookup_with_override(table_("gameerr"), to_gd(p_section), to_gd(p_key)));
	}
	opennova::hud::GameTextLookup gametext() override { return game_text_lookup(table_("gametext")); }
	opennova::inmatch::GameConfigBlock *config_block() override {
		if (!owner_->config_source_.is_valid()) return nullptr;
		const Ref<GameConfigBlock> block = owner_->config_source_.call();
		return block.is_valid() ? &block->native() : nullptr;
	}
	std::vector<opennova::menu::MissionChoice> host_pool() override {
		const Ref<ResourceRoot> root = root_();
		return root.is_valid() ? choices_of(MissionCatalog::rows(root))
		                       : std::vector<opennova::menu::MissionChoice>();
	}
	std::string expansion() override {
		const Ref<ResourceRoot> root = root_();
		return root.is_valid() ? to_std(root->get_expansion()) : std::string();
	}
	bool has_session() override { return owner_->session_() != nullptr; }
	bool start_search() override {
		LanSession *session = owner_->session_();
		// The LAN screen's browse: the broadcast over the retail game-server range, as LAN.
		return session != nullptr && int(session->call("start_browsing")) == int(OK);
	}
	void stop_enumerator() override {
		if (LanSession *session = owner_->session_()) session->stop_enumerator();
	}
	void drain_records() override {
		if (LanSession *session = owner_->session_()) session->drain_records();
	}
	bool join(int p_row) override {
		LanSession *session = owner_->session_();
		Ref<LanServerRow> row;
		if (session != nullptr) row = session->get_join_row(p_row);
		else if (p_row >= 0 && p_row < owner_->rows_.size()) row = owner_->rows_[p_row];
		if (row.is_null()) return false;
		const Ref<JoinTarget> target = JoinTarget::from_lan_row(row);
		// The join goes on with the search's socket (D-NET-1019), which stays open past the
		// window's end.
		if (session != nullptr) target->set_transport(session->take_transport());
		owner_->emit_signal("join_requested", target);
		return true;
	}
	void join_refused(const std::string &p_text) override {
		owner_->emit_signal("join_refused", opennova::cp1252_to_gd(p_text));
	}
	void save_config() override { owner_->emit_signal("config_save_requested"); }
	void host_start(const opennova::inmatch::LanHostRequest &p_request) override {
		PackedStringArray missions;
		for (const std::string &mission : p_request.missions) missions.push_back(to_gd(mission));
		PackedInt32Array options;
		for (const int32_t option : p_request.launch_options) options.push_back(option);
		owner_->emit_signal("host_start_requested", missions, options, to_gd(p_request.mission),
				opennova::cp1252_to_gd(p_request.game_type_attr), to_gd(p_request.expansion),
				p_request.novaworld);
	}
	void novaworld_host_cancelled() override { owner_->emit_signal("novaworld_host_cancelled"); }
};

LanMultiplayerMenu::LanMultiplayerMenu() : host_(std::make_unique<Host>(this)) {
	menu_.set_host(host_.get());
}

LanMultiplayerMenu::~LanMultiplayerMenu() {
	menu_.set_host(nullptr);
}

LanSession *LanMultiplayerMenu::session_() const {
	return session_id_.is_null() ? nullptr
	                             : Object::cast_to<LanSession>(ObjectDB::get_instance(session_id_));
}

void LanMultiplayerMenu::set_lan_session(LanSession *p_session) {
	const Callable servers(this, "_on_servers_changed");
	const Callable error(this, "_on_browse_error");
	const Callable finished(this, "_on_browse_finished");
	if (LanSession *previous = session_()) {
		if (previous->is_connected("servers_changed", servers)) previous->disconnect("servers_changed", servers);
		if (previous->is_connected("error_occurred", error)) previous->disconnect("error_occurred", error);
		if (previous->is_connected("browse_finished", finished))
			previous->disconnect("browse_finished", finished);
	}
	session_id_ = p_session != nullptr ? ObjectID(p_session->get_instance_id()) : ObjectID();
	if (p_session == nullptr) return;
	if (!p_session->is_connected("servers_changed", servers)) p_session->connect("servers_changed", servers);
	if (!p_session->is_connected("error_occurred", error)) p_session->connect("error_occurred", error);
	if (!p_session->is_connected("browse_finished", finished))
		p_session->connect("browse_finished", finished);
}

void LanMultiplayerMenu::set_sources(const Callable &p_text_tables, const Callable &p_config_block) {
	text_source_ = p_text_tables;
	config_source_ = p_config_block;
}

// The rows' text is the engine's (net/npwire/lan_discovery.h lan_discovery_row_label, through
// LanServerRow.list_label), in the page's bytes.
void LanMultiplayerMenu::on_servers_changed_(const Array &p_servers) {
	rows_.clear();
	std::vector<opennova::inmatch::LanMenuServer> servers;
	for (int64_t i = 0; i < p_servers.size(); ++i) {
		const Ref<LanServerRow> row = p_servers[i];
		if (row.is_null()) continue;
		rows_.push_back(row);
		servers.push_back({to_std(row->get_host_ip()), row->get_port(), gd_to_retail_text(row->list_label())});
	}
	menu_.servers_changed(std::move(servers));
}

void LanMultiplayerMenu::on_browse_error_(const String &p_message) {
	menu_.browse_error(gd_to_retail_text(p_message));
}

void LanMultiplayerMenu::on_browse_finished_() { menu_.browse_finished(); }

void LanMultiplayerMenu::seed_host_pool(const TypedArray<MissionCatalogRow> &p_rows) {
	menu_.seed_host_pool(choices_of(p_rows));
}

void LanMultiplayerMenu::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_lan_session", "session"), &LanMultiplayerMenu::set_lan_session);
	ClassDB::bind_method(D_METHOD("set_sources", "text_tables", "config_block"),
			&LanMultiplayerMenu::set_sources);
	ClassDB::bind_method(D_METHOD("seed_host_pool", "rows"), &LanMultiplayerMenu::seed_host_pool);
	ClassDB::bind_method(D_METHOD("set_novaworld_host", "novaworld"),
			&LanMultiplayerMenu::set_novaworld_host);
	ClassDB::bind_method(D_METHOD("is_novaworld_host"), &LanMultiplayerMenu::is_novaworld_host);
	ClassDB::bind_method(D_METHOD("get_selected_server"), &LanMultiplayerMenu::get_selected_server);
	ClassDB::bind_method(D_METHOD("can_start_host"), &LanMultiplayerMenu::can_start_host);
	ClassDB::bind_method(D_METHOD("_on_servers_changed", "servers"), &LanMultiplayerMenu::on_servers_changed_);
	ClassDB::bind_method(D_METHOD("_on_browse_error", "message"), &LanMultiplayerMenu::on_browse_error_);
	ClassDB::bind_method(D_METHOD("_on_browse_finished"), &LanMultiplayerMenu::on_browse_finished_);

	// The join from the selected row, its typed dial target (the session's socket on it).
	ADD_SIGNAL(MethodInfo("join_requested",
			PropertyInfo(Variant::OBJECT, "target", PROPERTY_HINT_RESOURCE_TYPE, "JoinTarget")));
	// A row whose index names no listed session: gameerr's CVUNABLETOCREATE.
	ADD_SIGNAL(MethodInfo("join_refused", PropertyInfo(Variant::STRING, "text")));
	// The LAN START's save point: the cfg block (which the START read the screen into) to game.cfg.
	ADD_SIGNAL(MethodInfo("config_save_requested"));
	// START: the rotation, its launch options, its head, the GAME_TYPE spin's value, the expansion
	// running and the channel; the session's other words are the cfg block's.
	ADD_SIGNAL(MethodInfo("host_start_requested", PropertyInfo(Variant::PACKED_STRING_ARRAY, "missions"),
			PropertyInfo(Variant::PACKED_INT32_ARRAY, "launch_options"), PropertyInfo(Variant::STRING, "mission"),
			PropertyInfo(Variant::STRING, "game_type_attr"), PropertyInfo(Variant::STRING, "expansion"),
			PropertyInfo(Variant::BOOL, "novaworld")));
	ADD_SIGNAL(MethodInfo("novaworld_host_cancelled"));
}

} // namespace godot
