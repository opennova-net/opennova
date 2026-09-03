class_name MissionSetupOptions
extends RefCounted

## Typed record for MissionPresentation.setup() (ADR 0017/0042): every input the
## composition hands the runtime driver, replacing the old options Dictionary.
## GameWorld._start_runtime builds one per load and NetSessionDrive.
## stage_runtime_options stamps the staged net-session request onto the same
## record; isolated tests fill only the fields they exercise. An unset reference
## reads null/empty and means "not provided", exactly as the old absent keys did.

## The already-connected simulation to adopt: a remote join owns the live socket +
## NP session before the wire-header world load (NetSessionDrive surrenders it
## here). Null = setup creates a fresh Simulation. MissionPresentation is the one
## adopter either way (ADR 0011/0012).
var simulation: Simulation = null
## Mission identity for diagnostics and the host session advertisement. setup()
## normalizes mission_file to a portable basename for get_mission_file(); the
## host leg advertises the raw value.
var mission_file := ""
var mission_name := ""
## Serve-and-play/SP hosts spawn their own player at bring-up; false for
## diagnostic previews and a DEDICATED serve (ADR 0015 serve mode).
var playable := false

# --- The typed net-session request (ADR 0017): at most one is non-null per load. ---
## Co-op LAN HOST request — the listen server binds a real UDP socket.
var host_session: HostSessionConfig = null
## Co-op LAN JOINER dial target — the non-authority client.
var join_target: JoinTarget = null
## The host's two per-side character selections projected to the wire
## (Simulation.set_local_character_profile). Null = keep the sim's shipped defaults.
var local_character_profile: CharacterJoinProfile = null
## The joiner's profile-to-wire projection (Simulation.set_join_character_profile).
## Null = none staged.
var join_character_profile: CharacterJoinProfile = null
## Host-session spawn-name list; empty falls back to [mission_name].
var spawn_names := PackedStringArray()

# --- Boot feeds (Simulation.boot_mission, S9 / ADR 0028). ---
var resource_root: ResourceRoot = null
var item_db: ItemDatabase = null
var terrain: TerrainData = null
## Terrain-tile (.til) bytes for the S2C 0x45 stream a listen host serves.
var terrain_til := PackedByteArray()
var wac_basename := ""
var infantry_adm := ""

# --- Presentation composition. ---
var placer: MissionObjectPlacer = null
## The owner-anchor registry the destruction/throwable passes anchor their
## wreck/piece/move effect groups through.
var effect_anchors: ItemEffectDirector = null

# --- Device providers. Genuinely device-Callable seams stay Callables: the
#     named typed field is the contract (ADR 0034 d3 retired the Dictionary
#     bundle, not the Callable). An unset Callable reads "unavailable". ---
## func() -> MissionAudio. Presence also gates the fire + destruction
## presentation passes (a dedicated serve presents neither).
var fire_audio := Callable()
## func() -> EffectWorld
var fire_fx := Callable()
## func() -> Vector3: the camera listener position (also the scar pass's camera).
var fire_listener := Callable()
## The dynamic light-pool routes (renderer/light_scene.h witness map); unset
## without a light director.
var muzzle_light := Callable()
var death_light := Callable()
## func() -> Node: the live environment node the scar pass reads fog distance +
## combined terrain light off each present frame.
var environment_node := Callable()

# --- Net staging read DOWNSTREAM of setup(): GameWorld's LAN bind-failure
#     report and NetSessionDrive's NovaWorld gate registration. ---
## "lan" for a staged host, "lan-join" for a staged joiner, "" for a local start.
var net_transport := ""
var bind_port := HostSessionConfig.DEFAULT_LAN_PORT
var server_name := ""
## The host's own callsign advertised on the gate row.
var player_name := ""
var max_players := HostSessionConfig.DEFAULT_MAX_PLAYERS
var channel := HostSessionConfig.CHANNEL_LAN
## Arms the drive's NovaWorld gate registration for a LAN listen host. No
## producer stages it today (the old opts["listen_server"] flag lost its
## producer in the #426 restructure), so gate registration stays latent; this is
## the typed translation of that guard, not a silent revival.
var listen_server := false
# NovaWorld gate registration (CHANNEL_NOVAWORLD): an empty gate host means
# pure LAN — nothing is registered.
var nw_gate_host := ""
var nw_gate_port := HostSessionConfig.DEFAULT_GATE_PORT
var region := "us"
## Explicit advertised-IP override for the gate row; empty keeps the socket's.
var advertise := ""
