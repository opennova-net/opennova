class_name HudFixture
extends RefCounted

## A REAL HudOverlay's minimum for the GUT presenter pins: a staged root with a
## synthetic hudpos.def and the synthetic one-page font (the
## hud_presenter_lanes_test recipe), a GameWorld exposing it, and a
## GameHudPresenter in the tree with its overlay built through the public
## ensure_game_hud() (ADR 0043 rule 11: real fixtures, no presenter doubles).
## Tests remove the staged directory in after_each (TestFs.remove_dir_recursive).

const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"  # staged as Gunpl22b.fnt

## Every HUDDECLUT row the game reads, each shown at all four detail levels. The
## game's mask table is zero until a row writes it, so a layout without these
## rows hides every gated element (the ammo count, the health bar, the
## crosshair, the corner map, the chat rings: docs/interface/hud-re.md D-HUD-54);
## a synthetic layout that wants them drawn authors the rows, as every shipped
## hudpos.def does. A later row for a slot replaces an earlier one.
const DECLUTTER_ROWS := [
	"HUDDECLUT_MSNTITLE 1 1 1 1", "HUDDECLUT_FARPINFO 1 1 1 1", "HUDDECLUT_BREATHTIME 1 1 1 1",
	"HUDDECLUT_WAYPOINT 1 1 1 1", "HUDDECLUT_ALTGRP 1 1 1 1", "HUDDECLUT_SPEED 1 1 1 1",
	"HUDDECLUT_CKPTCOND 1 1 1 1", "HUDDECLUT_DMGBAR 1 1 1 1", "HUDDECLUT_WPNGRP 1 1 1 1",
	"HUDDECLUT_EXPPOINTS 1 1 1 1", "HUDDECLUT_NWSTAT 1 1 1 1", "HUDDECLUT_CFGDISP 1 1 1 1",
	"HUDDECLUT_LOLLIS 1 1 1 1", "HUDDECLUT_XHAIRS 1 1 1 1", "HUDDECLUT_VELVECT 1 1 1 1",
	"HUDDECLUT_FARPIND 1 1 1 1", "HUDDECLUT_TARGDMGDISP 1 1 1 1", "HUDDECLUT_SPINMAP 1 1 1 1",
	"HUDDECLUT_TRGTCNT 1 1 1 1", "HUDDECLUT_TEAMID 1 1 1 1", "HUDDECLUT_PWRBAR 1 1 1 1",
	"HUDDECLUT_CLOCK 1 1 1 1", "HUDDECLUT_HUDLS 1 1 1 1", "HUDDECLUT_CHAT 1 1 1 1",
]


## DECLUTTER_ROWS as CR LF lines, for a fixture that writes a whole file.
static func declutter_text() -> String:
	return "\r\n".join(PackedStringArray(DECLUTTER_ROWS)) + "\r\n"


## With `with_minimal_pack` the root also carries the minimal asset pack (over
## the Tmap terrain), so WorldFixture.boot_minimal() can load mnml.bms from it.
static func stage_root(with_minimal_pack := false) -> String:
	var dir_path := WorldFixture.stage_minimal_root("hud", true) if with_minimal_pack \
			else OS.get_temp_dir().path_join("hud_fixture_%d" % Time.get_ticks_usec())
	assert(DirAccess.make_dir_recursive_absolute(dir_path) == OK)
	var def_file := FileAccess.open(dir_path.path_join("hudpos.def"), FileAccess.WRITE)
	def_file.store_string("fonthud1_hi Gunpl22b.fnt\r\nHUDCHATTEXT 142 , 711\r\n" + declutter_text())
	def_file.close()
	var fnt := FileAccess.open(dir_path.path_join("Gunpl22b.fnt"), FileAccess.WRITE)
	fnt.store_buffer(FileAccess.get_file_as_bytes(FONT_FIXTURE))
	fnt.close()
	return dir_path


## A presenter in the tree over a LOADED `world` with its real HudOverlay built
## and configured from the world's root (a world exposes its resource root only
## once a mission is loaded).
static func presenter_over(test: GutTest, world: GameWorld) -> GameHudPresenter:
	var presenter: GameHudPresenter = test.add_child_autofree(GameHudPresenter.new())
	presenter.setup(world, null, null)
	presenter.ensure_game_hud()
	test.assert_true(presenter.get_game_hud().is_configured(),
			"the real overlay configured from the staged hudpos.def")
	return presenter


## The minimal mission booted from a `stage_root(true)` directory, with the
## presenter and its real overlay over it.
static func booted_presenter(test: GutTest, staged_dir: String) -> GameHudPresenter:
	return presenter_over(test, WorldFixture.boot_minimal(test, staged_dir))
