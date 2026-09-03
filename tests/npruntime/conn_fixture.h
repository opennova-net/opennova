// The NapiNPConnection factory the npruntime tests share: one connection in
// the phase a test wants (New, or InMatch with its owned entity announced),
// bound to a transport. Construction contract in one place, so a change to
// NapiNPConnection's link/burst/phase fields is edited once.
#ifndef OPENNOVA_TESTS_NPRUNTIME_CONN_FIXTURE_H
#define OPENNOVA_TESTS_NPRUNTIME_CONN_FIXTURE_H

#include <runtime/session/napi_np_connection.h>
#include <runtime/replication/connection.h>        // netsim::TransportMode
#include <runtime/session/session_transport.h> // netsim::ISessionTransport
#include <runtime/world/entity.h>         // world::EntityHandle

#include <cstdint>

namespace conn_fixture {

inline opennova::np::NapiNPConnection make_conn(uint32_t id, int type,
                                                opennova::netsim::ISessionTransport *t,
                                                opennova::netsim::TransportMode mode,
                                                opennova::world::EntityHandle owned, bool spawned) {
	opennova::np::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = spawned;
	c.spawned_announced = spawned;
	c.phase = spawned ? opennova::np::ConnectionPhase::InMatch : opennova::np::ConnectionPhase::New;
	return c;
}

} // namespace conn_fixture

#endif // OPENNOVA_TESTS_NPRUNTIME_CONN_FIXTURE_H
