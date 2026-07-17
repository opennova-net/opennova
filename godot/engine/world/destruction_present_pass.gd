extends RefCounted

# THE host destruction-presentation pass: presents the sim's item destruction on
# the viewing host — the husk model swap on destroyed items, the death-piece
# debris (trail effects riding the sim's piece pool), the section-debris bursts,
# the death/fire/other wreck effect families with the random fire crackle, and
# the destruction sounds. Drains NovaSimulation.drain_destruction_events() +
# get_death_pieces() once per present, the fire_present_pass precedent.
#
# [orig map — docs/world/world-wac-ai-re.md §24:
#  husk swap: Flags & 4 switches render + collision to the def husk model
#    (Entity_RaycastCollisionModel @ 0x413086 pick; no husk -> the graphic keeps
#    standing, the witnessed fallback);
#  pieces: Entity_SpawnDeathPieces @ 0x493400 -> the 256-slot pool ticked by
#    DeathPiece_TickAll @ 0x57b900 (each piece renders ONE husk section with a
#    per-type trail effect from g_death_piece_types @ 0x8404f0);
#  section debris: Entity_SpawnSectionDebris @ 0x43f580 — collision-face
#    centroid sampling at stride (scale<<8)/150, Effect_TreeWoodExp per tri
#    (material 17 -> Effect_TreeFoliageExp), directions away from the blast;
#  wreck effects: Entity_InitDeathSounds @ 0x4939b0 (the particledeath family
#    at the Dead bones) + Entity_UpdateDeadWreckEffects @ 0x493140 (the fire
#    family's random crackle Effect_BoatExpSec + EXPLO_SHIP_SM, underwater
#    steam-out Effect_Boat01Steam).]
#
# Stand-ins (tracked in the §24 record + D-ITEM ledger rows): pieces draw as
# their type's trail effect following the sim piece (the single-section husk
# MESH chunk needs per-part render instancing); the section-debris burst spawns
# a small radial set at the entity (the per-triangle collision-face sampling
# needs the CFAC face plumb); effect anchors ride the entity origin, not the
# husk Dead/Fire/Other user points; the glass user-point shatter is counted but
# not yet drawn.

const BURST_EFFECT := "Effect_TreeWoodExp"       # [orig: g_fx_TreeWoodExp @ 0x2C25BF4]
const BURST_COUNT := 6                            # sampling stand-in (see header)
const FIRE_CRACKLE_EFFECT := "Effect_BoatExpSec"  # [orig: g_fx_BoatExpSec @ 0x2C25CB8]
const FIRE_CRACKLE_SOUND := "EXPLO_SHIP_SM"       # [orig: g_snd_EXPLO_SHIP_SM_b @ 0x24E08F4]
const FIRE_CRACKLE_CHANCE := 16.0 / 65536.0       # [orig: PRNG_Next16_C() < 16 @ 0x4932bf]

# The debris-type trail effects by table index [orig: g_death_piece_types
# @ 0x8404f0 +0x2C column; "" = the type authors no trail (NP rows)].
const PIECE_TRAIL_BY_TYPE: Array[String] = [
	"",                # 0 HULL
	"Effect_VexpM",    # 1 WHEEL
	"Effect_VexpS",    # 2 CHUNK_S
	"Effect_VexpM",    # 3 CHUNK_M
	"Effect_VexpL",    # 4 CHUNK_L
	"Effect_PDust_S",  # 5 ROCK_S
	"Effect_PDust_M",  # 6 ROCK_M
	"",                # 7 ROCK_L
	"",                # 8 CHUNKNP_S
	"",                # 9 CHUNKNP_M
	"",                # 10 CHUNKNP_L
	"",                # 11 CACTUS_
	"Effect_VexpSL",   # 12 CHUNKSF_M
]

var _sim                          # NovaSimulation
var _index                        # MissionEntityRegistry
var _placer                       # MissionObjectPlacer (husk model builds)
var _item_db                      # NovaItemDatabase (husk graphic names)
var _game_world                   # GameWorld (effect anchors) or null
var _audio_provider := Callable() # -> NovaMissionAudio (or null)
var _fx_provider := Callable()    # -> NovaEffectWorld (or null)
var _husked: Dictionary = {}      # bms_id -> husk model Node3D (or null for no-husk)
var _burning: Dictionary = {}     # bms_id -> {node, fire} — the crackle roll set
var _piece_pos: Dictionary = {}   # piece slot -> Vector3 (anchor resolver source)
var _piece_live: Dictionary = {}  # piece slot -> true (trail spawned)
var _rng := RandomNumberGenerator.new()
var _stats := {
	"husk_swaps": 0, "no_husk": 0, "pieces_peak": 0, "bursts": 0,
	"effects": 0, "sounds": 0, "glass": 0, "crackles": 0,
}


func get_stats() -> Dictionary:
	return _stats.duplicate()


func setup(sim, index, placer, item_db, game_world, audio_provider: Callable,
		fx_provider: Callable) -> void:
	_sim = sim
	_index = index
	_placer = placer
	_item_db = item_db
	_game_world = game_world
	_audio_provider = audio_provider
	_fx_provider = fx_provider
	_rng.randomize()


func teardown() -> void:
	for slot in _piece_live.keys():
		_unregister_piece_anchor(int(slot))
	_piece_live.clear()
	_piece_pos.clear()
	for bms_id in _husked.keys():
		var husk: Variant = _husked[bms_id]
		if husk is Node3D and is_instance_valid(husk):
			(husk as Node3D).queue_free()
	_husked.clear()
	_burning.clear()


## Once per present, after the sim advanced (beside the fire pass).
func present() -> void:
	if _sim == null or not _sim.has_method("drain_destruction_events"):
		return  # stale native DLL — presentation degrades silently, sim unaffected
	var events: Dictionary = _sim.drain_destruction_events()
	if not events.is_empty():
		for husk_v in events.get("husk_swaps", []):
			_apply_husk_swap(husk_v as Dictionary)
		for burst_v in events.get("debris_bursts", []):
			_apply_debris_burst(burst_v as Dictionary)
		for eff_v in events.get("effects", []):
			_apply_effect(eff_v as Dictionary)
		for snd_v in events.get("sounds", []):
			_apply_sound(snd_v as Dictionary)
		_stats.glass += int((events.get("glass_breaks", []) as Array).size())
	_present_pieces()
	_tick_wreck_fires()


# The husk model swap: hide the intact node's visual children and graft the husk
# model as a child (it inherits the node transform, so settling wrecks keep
# moving with the present pass). No husk authored -> the intact graphic keeps
# standing, dead — the witnessed render-pick fallback.
func _apply_husk_swap(husk: Dictionary) -> void:
	var bms_id := int(husk.get("bms_id", 0))
	if _husked.has(bms_id):
		return
	_stats.husk_swaps += 1
	var node: Node3D = null
	if _index != null:
		node = _index.resolve_single(bms_id)
	if node == null or not is_instance_valid(node):
		_husked[bms_id] = null
		return
	var item_id := int(husk.get("item_id", 0))
	var def_id := item_id + 100000  # mission::kItemIdOffset (item DB keys)
	var husk_graphic := ""
	if _item_db != null:
		husk_graphic = String(_item_db.get_husk(def_id))
		if husk_graphic.is_empty():
			husk_graphic = String(_item_db.get_huskfinal(def_id))
	if husk_graphic.is_empty() or _placer == null \
			or not _placer.has_method("build_model_from_graphic"):
		_husked[bms_id] = null
		_stats.no_husk += 1
		return
	for child in node.get_children():
		if child is Node3D:
			(child as Node3D).visible = false
	var model: Node3D = _placer.build_model_from_graphic(husk_graphic, "", node)
	if model != null:
		model.name = "HuskModel"
	_husked[bms_id] = model


# The section-debris burst [orig: Entity_SpawnSectionDebris @ 0x43f580].
# Sampling stand-in: a small radial set around the entity, directed away from
# the recorded blast center (or radially when none — the witnessed 63.3° pitch
# fallback lives in the effect's own emission).
func _apply_debris_burst(burst: Dictionary) -> void:
	var fx = _fx_provider.call() if _fx_provider.is_valid() else null
	if fx == null:
		return
	var node: Node3D = null
	if _index != null:
		node = _index.resolve_single(int(burst.get("bms_id", 0)))
	if node == null or not is_instance_valid(node):
		return
	_stats.bursts += 1
	var origin := node.global_position
	var blast: Vector3 = burst.get("blast_center", Vector3.ZERO)
	var away := Vector3.UP
	if bool(burst.get("has_blast_center", false)):
		away = (origin - blast)
		away.y = absf(away.y)
		if away.length_squared() > 0.0001:
			away = away.normalized()
		else:
			away = Vector3.UP
	for i in range(BURST_COUNT):
		var jitter := Vector3(_rng.randf_range(-1.5, 1.5), _rng.randf_range(0.0, 1.5),
				_rng.randf_range(-1.5, 1.5))
		fx.spawn_effect(BURST_EFFECT, origin + jitter, away)
		_stats.effects += 1


func _apply_effect(eff: Dictionary) -> void:
	var fx = _fx_provider.call() if _fx_provider.is_valid() else null
	if fx == null:
		return
	var effect := String(eff.get("effect", ""))
	if effect.is_empty():
		return
	var pos: Vector3 = eff.get("pos", Vector3.ZERO)
	var family := int(eff.get("family", 0))
	var net_id := int(eff.get("attach_net_id", 0))
	if family == 0 or net_id == 0:
		fx.spawn_effect(effect, pos, eff.get("dir", Vector3.ZERO))
		_stats.effects += 1
		return
	# Attached families (death smoke / fire / other): one owned group per
	# (entity, family), anchored at the wreck (the husk Dead/Fire/Other
	# user-point anchors are the tracked refinement).
	var key := "wreck:%d:%d" % [net_id, family]
	fx.spawn_effect_owned(key, effect, pos, Vector3.UP)
	_stats.effects += 1
	if _game_world != null and _game_world.has_method("register_effect_anchor"):
		var bms_id := int(eff.get("attach_bms_id", 0))
		var node: Node3D = null
		if _index != null and bms_id != 0:
			node = _index.resolve_single(bms_id)
		if node != null and is_instance_valid(node):
			_game_world.register_effect_anchor(key, func() -> Variant:
				return node.global_transform if is_instance_valid(node) else null)
			if family == 2:
				_burning[net_id] = {"node": node}


func _apply_sound(snd: Dictionary) -> void:
	var audio = _audio_provider.call() if _audio_provider.is_valid() else null
	if audio == null:
		return
	var name := String(snd.get("sound", ""))
	if name.is_empty():
		return
	audio.fire_soundset(name, snd.get("pos", Vector3.ZERO), 0)
	_stats.sounds += 1


# Death pieces: the sim owns positions/physics; each live piece carries its
# type's trail effect as an owned follow group. The single-section husk mesh
# chunk is the tracked residual (§24).
func _present_pieces() -> void:
	if not _sim.has_method("get_death_pieces"):
		return
	var fx = _fx_provider.call() if _fx_provider.is_valid() else null
	var pieces: Array = _sim.get_death_pieces()
	_stats.pieces_peak = maxi(int(_stats.pieces_peak), pieces.size())
	var seen: Dictionary = {}
	for piece_v in pieces:
		var piece: Dictionary = piece_v
		var slot := int(piece.get("slot", -1))
		if slot < 0:
			continue
		seen[slot] = true
		var pos: Vector3 = piece.get("pos", Vector3.ZERO)
		_piece_pos[slot] = pos
		if bool(piece.get("settled", false)):
			continue
		if not _piece_live.has(slot):
			_piece_live[slot] = true
			var trail := _piece_trail(int(piece.get("type_index", 0)))
			if fx != null and not trail.is_empty():
				var key := "piece:%d" % slot
				fx.spawn_effect_owned(key, trail, pos, Vector3.UP)
				if _game_world != null and _game_world.has_method("register_effect_anchor"):
					_game_world.register_effect_anchor(key, func() -> Variant:
						return _piece_pos.get(slot) if _piece_live.has(slot) else null)
	for slot in _piece_live.keys():
		if not seen.has(int(slot)):
			_unregister_piece_anchor(int(slot))
			_piece_live.erase(slot)
			_piece_pos.erase(slot)


func _piece_trail(type_index: int) -> String:
	if type_index < 0 or type_index >= PIECE_TRAIL_BY_TYPE.size():
		return ""
	return PIECE_TRAIL_BY_TYPE[type_index]


func _unregister_piece_anchor(slot: int) -> void:
	if _game_world != null and _game_world.has_method("unregister_effect_anchor"):
		_game_world.unregister_effect_anchor("piece:%d" % slot)


# The wreck-fire random crackle [orig: Entity_UpdateDeadWreckEffects @ 0x493140
# — per tick, per fire bone: PRNG < 16/65536 -> Effect_BoatExpSec + the crackle
# sound; the underwater steam-out rides the effect world's kill plane].
func _tick_wreck_fires() -> void:
	if _burning.is_empty():
		return
	var fx = _fx_provider.call() if _fx_provider.is_valid() else null
	var audio = _audio_provider.call() if _audio_provider.is_valid() else null
	for net_id in _burning.keys():
		var entry: Dictionary = _burning[net_id]
		var node: Variant = entry.get("node")
		if not (node is Node3D) or not is_instance_valid(node):
			_burning.erase(net_id)
			continue
		if _rng.randf() < FIRE_CRACKLE_CHANCE:
			_stats.crackles += 1
			var pos := (node as Node3D).global_position
			if fx != null:
				fx.spawn_effect(FIRE_CRACKLE_EFFECT, pos, Vector3.UP)
			if audio != null:
				audio.fire_soundset(FIRE_CRACKLE_SOUND, pos, 0)
