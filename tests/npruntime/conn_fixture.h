// The NapiNPConnection factory the npruntime tests share: one connection in
// the phase a test wants (New, or InMatch with its owned entity announced),
// bound to a transport. Construction contract in one place, so a change to
// NapiNPConnection's link/burst/phase fields is edited once.
#pragma once

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_tick.h>       // inmatch::Server_RerollPlayerTickSeed
#include <runtime/replication/connection.h>        // replication::TransportMode
#include <runtime/inmatch/session_transport.h> // replication::ISessionTransport
#include <runtime/world/entity.h>         // world::EntityHandle

#include <cstdint>

namespace conn_fixture {

inline opennova::inmatch::NapiNPConnection make_conn(uint32_t id, int type,
                                                opennova::replication::ISessionTransport *t,
                                                opennova::replication::TransportMode mode,
                                                opennova::world::EntityHandle owned, bool spawned) {
	opennova::inmatch::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = spawned;
	c.spawned_announced = spawned;
	c.phase = spawned ? opennova::inmatch::ConnectionPhase::InMatch : opennova::inmatch::ConnectionPhase::New;
	return c;
}

// make_conn, with a spawned connection's tick seed re-rolled the way join and
// deployment arm it (Server_RerollPlayerTickSeed).
inline opennova::inmatch::NapiNPConnection make_seeded_conn(uint32_t id, int type,
                                                opennova::replication::ISessionTransport *t,
                                                opennova::replication::TransportMode mode,
                                                opennova::world::EntityHandle owned, bool spawned) {
	opennova::inmatch::NapiNPConnection c = make_conn(id, type, t, mode, owned, spawned);
	if (spawned) (void)opennova::inmatch::Server_RerollPlayerTickSeed(c);
	return c;
}

} // namespace conn_fixture
