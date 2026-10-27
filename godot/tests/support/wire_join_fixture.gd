class_name WireJoinFixture
extends RefCounted

## A joiner's world is the host's 616-byte S2C 0x0B mission header and the wire
## stream behind it (D-NET-194): a joiner never opens a mission body, as retail's
## client never opens the .bms, so its pools 1..3 hold only the rows its
## materializer spawns from the stream. load_wire_header drives a joiner's
## connection until that header lands and loads it as the joiner's header-only
## document, the production GameWorld path's shape.


## Loads the host's mission header into `joiner` as its header-only world; false
## when the header never landed or does not open.
static func load_wire_header(host: Simulation, joiner: Simulation, ticks: int = 800) -> bool:
	var header := PackedByteArray()
	for _tick in range(ticks):
		joiner.poll_join_preload()
		host.step()
		header = joiner.get_join_mission_header()
		if header.size() == 616:
			break
		OS.delay_msec(2)
	if header.size() != 616:
		return false
	var wire_mission := MissionData.new()
	if wire_mission.open_wire_header(header) != OK:
		return false
	return joiner.load_from_mission_data(wire_mission)
