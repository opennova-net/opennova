#include "netsim/serializing_sink.h"

namespace opennova::netsim {

void SerializingSink::send_command(world::EntityHandle /*owner*/,
                                   uint16_t /*command_id*/,
                                   const int32_t * /*args*/, int /*argc*/) {
	// Phase 1 seam: the SP listen server is authoritative for every entity, so no
	// entity-targeted command is forwarded to a remote owner yet. Phase 2+ serializes
	// the command onto channel_ [orig: NapiNPServer_SendFiltered(..., 0x23, ...)].
	(void)channel_;
}

} // namespace opennova::netsim
