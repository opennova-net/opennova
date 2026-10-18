#pragma once

// What each machine should run: the supervisor's answer to a machine agent's
// sync (agent_route.h). The agent routes authenticate the agent, store its
// report (fleet_repository.h record_sync) and ask this seam what the machine
// should run; the supervisor implements it from the rows it placed
// (placement.h), and the route tests drive a fake. Crow-free, so the
// supervisor's side needs no HTTP layer. Service policy, not a port.
//
// The answer, per desired container (agent_protocol.h DesiredContainer): its
// name (hosted_container_name: srv-<server id>-g<generation>), its slot on
// this machine (fleet_repository.h assign_slot), the serve image the server
// is pinned to, the trees it is pinned to (its base data set's and its
// expansion's, data_set_repository.h, ADR 0064), opennova-serve's arguments,
// its environment, its limits (fleet_config.h: the slot's CPUs and memory) and
// its working directory's files (the host file, game.cfg, admin.cfg, the
// NovaWorld credentials); and, beside the containers, every tree they use
// (`trees`: where the agent fetches it from the object store and its
// manifest, each file's name and size). The files go only until the machine has reported the
// container once, in any state: `reported` is every container the agent runs,
// so the implementation marks each desired container it names as acknowledged
// and leaves its files out from then on (the route leaves out the files of any
// container this very report names, whatever the answer says). A container
// the answer leaves out is stopped and removed by the agent (its slot's files
// with it); one that exited stays reported until the answer drops it, and a
// restart is a new name (a new generation), never the same container again.
// The route sets each container's uid to its slot's and drops one whose slot
// is past the machine's.

#include "hosted/agent_protocol.h"
#include "hosted/fleet_repository.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <map>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct FleetDecision {
	std::vector<DesiredContainer> desired; // one per slot, one per name
	// Every tree the desired containers name, and only those: the agent fetches
	// what it lacks before a start and may drop what no reply names.
	std::map<int64_t, TreeSpec> trees;
	// Whether the service is moving the machine's servers off it. The route
	// says so as well whenever the machine's row is draining.
	bool drain = false;
};

class FleetDesiredState {
public:
	virtual ~FleetDesiredState() = default;

	// Machine `machine`'s answer, after its agent reported `reported` (already
	// stored: `machine` is its row after record_sync). Called on the internal
	// listener's handler threads, several machines at once, so an
	// implementation locks what it shares; `db` is the connection the route
	// leased for this request, with no transaction open. A throw is logged and
	// answered with the routes' one refusal (the agent changes nothing).
	virtual FleetDecision sync(db::Database &db, const FleetMachineRow &machine,
			const std::vector<ContainerReport> &reported) = 0;
};

} // namespace opennova::novaworld_server::hosted
