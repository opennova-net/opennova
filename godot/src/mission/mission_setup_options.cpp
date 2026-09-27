#include "mission/mission_setup_options.h"
#include "audio/music_director.h"
#include <godot_cpp/core/object.hpp>

namespace godot {

MusicDirector *MissionSetupOptions::get_music_director() const {
    return Object::cast_to<MusicDirector>(ObjectDB::get_instance(music_director_id_));
}

void MissionSetupOptions::set_music_director(MusicDirector *director) {
    music_director_id_ = director ? ObjectID(director->get_instance_id()) : ObjectID();
}

void MissionSetupOptions::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_music_director", "director"), &MissionSetupOptions::set_music_director);
#define SETUP_OPTION(m_variant, m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionSetupOptions::get_##m_name);         \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MissionSetupOptions::set_##m_name); \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
#define SETUP_OBJECT(m_class, m_name)                                                            \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionSetupOptions::get_##m_name);         \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MissionSetupOptions::set_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, #m_name, PROPERTY_HINT_RESOURCE_TYPE, #m_class), \
			"set_" #m_name, "get_" #m_name);
	SETUP_OBJECT(Simulation, simulation)
	SETUP_OPTION(Variant::STRING, mission_file)
	SETUP_OPTION(Variant::STRING, mission_name)
	SETUP_OPTION(Variant::BOOL, playable)
	SETUP_OBJECT(HostSessionOptions, host_session)
	SETUP_OBJECT(JoinTarget, join_target)
	SETUP_OBJECT(CharacterJoinProfile, local_character_profile)
	SETUP_OBJECT(CharacterJoinProfile, join_character_profile)
	SETUP_OPTION(Variant::PACKED_STRING_ARRAY, spawn_names)
	SETUP_OBJECT(ResourceRoot, resource_root)
	SETUP_OBJECT(ItemDatabase, item_db)
	SETUP_OBJECT(TerrainData, terrain)
	SETUP_OPTION(Variant::PACKED_BYTE_ARRAY, terrain_til)
	SETUP_OPTION(Variant::STRING, wac_basename)
	SETUP_OBJECT(MissionObjectPlacer, placer)
	SETUP_OPTION(Variant::STRING, net_transport)
	SETUP_OPTION(Variant::INT, bind_port)
	SETUP_OPTION(Variant::STRING, server_name)
	SETUP_OPTION(Variant::STRING, player_name)
	SETUP_OPTION(Variant::INT, max_players)
	SETUP_OPTION(Variant::STRING, channel)
	SETUP_OPTION(Variant::STRING, nw_gate_host)
	SETUP_OPTION(Variant::INT, nw_gate_port)
	SETUP_OPTION(Variant::INT, region_index)
	SETUP_OPTION(Variant::STRING, advertise)
#undef SETUP_OBJECT
#undef SETUP_OPTION
}

} // namespace godot
