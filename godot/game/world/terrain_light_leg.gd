class_name TerrainLightLeg
extends RefCounted

## The terrain leg of the EffectWorld light pool: hand the terrain node the
## shared pool and the frame time once per light frame, so its next render
## frame re-draws every patch with the pool lights that patch overlaps.
##
## Retail re-draws each terrain batch once more per passing pool light,
## additively, through the two-stage projected-texture pass
## [orig: render_terrain_sector_batch @0x6092A0, the per-light else-arm
## @0x609870..0x6098BC -> Light_SetupTerrainProjectedPass @0x5AA830]. The
## collect (<= 16 per batch, both light groups cleared, the authored
## terrain-disable flag), the projection contract and the pixel constants are
## portable in renderer::LightScene::collect_terrain_pass_rows; the Terrain
## device packs the rows into its per-slot texture and the terrain shader sums
## the two MODULATE2X stages (docs/render/render-lighting-re.md).
##
## Device fold: the pipeline runs render_light_frame AFTER this frame's
## terrain draw (game_frame_pipeline.gd), so the pool state a patch re-draws
## with is one frame old at 62 Hz. The patch <-> slot match is exact — the
## terrain collects against its own draw list — and the pipeline order stays
## as witnessed (D-RORD).


## One typed call from GameWorld.render_light_frame. A null director (no
## mission) retires the leg; the terrain clears its rows texture.
static func render_frame(terrain: Terrain, director: EffectLightDirector) -> void:
	if terrain == null:
		return
	var scene: LightScene = director.scene() if director != null else null
	terrain.set_light_context(scene, Time.get_ticks_msec())
