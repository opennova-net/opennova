extends GutTest

# net_killfeed.gd: the spectator kill feed that replaces the HTML viewer's event
# log. Covers name resolution, the $A/$B canned-message substitution
# ([orig: Chat_FormatMessage @ 0x422C60]), and the fallback line used when no
# canned template is loaded into NovaStrings.

const NetKillFeed := preload("res://game/net_killfeed.gd")

const KIND_KILL := 2
const KIND_GAMEEVENT := 3


func _feed() -> CanvasLayer:
	var f := NetKillFeed.new()
	add_child_autofree(f)
	return f


func test_name_of_resolves_known_and_unknown() -> void:
	var f := _feed()
	var names := {5: "FooPlayer", 4: ""}
	assert_eq(f._name_of(5, names), "FooPlayer", "known handle resolves to its name")
	assert_eq(f._name_of(4, names), "s4", "blank name falls back to slot label")
	assert_eq(f._name_of(0x1002, {}), "s2·p1", "non-organic handle shows pool")
	assert_eq(f._name_of(65535, names), "?", "the 0xFFFF sentinel reads as none")


func test_apply_tokens_substitutes_a_and_b() -> void:
	var f := _feed()
	assert_eq(f._apply_tokens("$A killed $B", "Foo", "Bar"), "Foo killed Bar")


func test_kill_falls_back_without_template() -> void:
	# No gametext loaded into NovaStrings -> _canned() returns "" -> fallback line.
	var f := _feed()
	var ev := {
		"kind": KIND_KILL, "source": 5, "target": 4, "label": "STRCND04",
		"event_type": 4,
	}
	assert_eq(f._format(ev, {5: "Foo", 4: "Bar"}), "Foo ✖ Bar",
		"a kill with no resolvable template reads as attacker x victim")


func test_gameevent_without_template_shows_label() -> void:
	var f := _feed()
	var ev := {"kind": KIND_GAMEEVENT, "source": 65535, "target": 65535,
		"label": "STRCND42", "event_type": 42}
	assert_eq(f._format(ev, {}), "STRCND42",
		"an unresolved objective event shows its STRCND key")


func test_env_readout_blank_until_seen() -> void:
	var f := _feed()
	assert_eq(f._format_env({"found": false}), "", "no env snapshot -> blank readout")


func test_env_readout_formats_time_and_fog() -> void:
	var f := _feed()
	# tod_fixed = 0x8000 -> half a day -> 12:00.
	var env := {"found": true, "tod_fixed": 0x8000, "fog_dist": 1500,
		"cloud_scroll": 3, "overcast": 0, "quake_ticks": 0}
	var line: String = f._format_env(env)
	assert_string_contains(line, "TOD 12:00")
	assert_string_contains(line, "fog 1500")
