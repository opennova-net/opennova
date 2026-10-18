#pragma once

// A fleet roll's plan (POST /api/admin/fleet/roll drains the machines it
// names; GET previews it, apps/novaworld_web/hosted/fleet_routes.h): a roll is
// for a change of what a machine launches from (the image the launch templates
// pin, the templates, the user-data), not of what it runs (hostd and the
// serve images reach the machines in place: app deploy rolls hostd, a
// server's next start takes the release's serve image). Of the live machines
// that take servers (launching, booting, ready), one whose image is the one
// the templates pin is kept, and every other one replaced: its servers move,
// no restart counted, to machines launched from the templates as they are. A
// machine whose image is not known (the cloud not read here, or it does not
// list the machine yet) is replaced too, as is every machine when no image is
// pinned. Pure: the routes read the rows and the images. Service policy, not a
// port.

#include "fleet_repository.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct RollStep {
	int64_t machine_id = 0;
	MachineKind kind = MachineKind::Shared;
	FleetMachineState state = FleetMachineState::Ready;
	std::string image_id; // its image ("" when not known)
	int servers = 0; // the live servers on it
	bool replace = false;
	std::string why;
};

// The plan for `machines` (the live ones; the others are left out), each
// one's image from `image_of` (by machine id) and its servers from
// `servers_of`, against `pinned_image` (ONNET_FLEET_IMAGE_ID; "" for none).
std::vector<RollStep> plan_fleet_roll(const std::vector<FleetMachineRow> &machines,
		const std::map<int64_t, std::string> &image_of, const std::map<int64_t, int> &servers_of,
		const std::string &pinned_image);

// Whether a machine in `state` is one a roll may drain (launching, booting, ready).
bool machine_rollable(FleetMachineState state);

} // namespace opennova::novaworld_server::hosted
