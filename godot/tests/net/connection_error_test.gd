extends GutTest
# A failed join or a lost session reads retail's reason text over the joiner
# connection's error record, through the shell's gameerr table
# [orig: CNapiNetwork_GetDisconnectReasonString @0x4c7000].

var _saved_gameerr: RtxtStringFile
var _saved_override: RtxtStringFile


func before_each() -> void:
	_saved_gameerr = Strings.get_table(Strings.TABLE_GAMEERR)
	_saved_override = Strings.get_override_table()
	Strings.set_override_table(null)


func after_each() -> void:
	Strings.register_table(Strings.TABLE_GAMEERR, _saved_gameerr)
	Strings.set_override_table(_saved_override)


func _gameerr() -> RtxtStringFile:
	var table := RtxtStringFile.new()
	var net_connect := table.add_section("MPNetConnectCodes")
	table.add_entry("NCC007", "Your game is incompatible with this server. (NCC007)",
			net_connect, Vector2i.ZERO)
	var game_disconnect := table.add_section("MPGameDisconnectCodes")
	table.add_entry("GDC032", "[[$]] (GDC032)", game_disconnect, Vector2i.ZERO)
	table.add_entry("GDC047", "An expansion pack/mod type mismatch has occurred. (GDC047)",
			game_disconnect, Vector2i.ZERO)
	return table


func test_a_join_refusal_reads_its_gameerr_entry() -> void:
	var error := ConnectionError.from_fields(7, 0, "", 0, 0, "")
	assert_true(error.is_set())
	assert_eq(error.reason_key(), "NCC007")
	assert_eq(error.reason_text(null, _gameerr()),
			"Your game is incompatible with this server. (NCC007)")
	assert_true(error.refuses_this_install(),
			"a game-version refusal cannot be retried on the same server")


func test_a_disconnect_substitutes_its_text() -> void:
	var error := ConnectionError.from_fields(0, 0, "", 2, 32, "Kicked by admin")
	assert_eq(error.reason_text(null, _gameerr()), "Kicked by admin (GDC032)")
	assert_false(error.refuses_this_install(), "a kick is not a refusal of this install")


func test_an_empty_record_has_no_reason() -> void:
	var error := ConnectionError.from_fields(0, 0, "", 0, 0, "")
	assert_false(error.is_set())
	assert_eq(error.reason_key(), "")
	assert_eq(error.reason_text(null, _gameerr()), "")


func test_the_post_mission_route_shows_the_records_reason() -> void:
	Strings.register_table(Strings.TABLE_GAMEERR, _gameerr())
	var route := PostMissionRoute.new()
	route.error = true
	route.error_text = "the host closed the session (GDC047; reason 47, class 2)"
	route.connection_error = ConnectionError.from_fields(0, 0, "", 2, 47, "")
	assert_eq(MainGame.post_mission_error_text(route),
			"An expansion pack/mod type mismatch has occurred. (GDC047)",
			"the menu shows retail's GDC text, not the diagnostic")
	route.connection_error = null
	assert_eq(MainGame.post_mission_error_text(route), route.error_text,
			"without a record the diagnostic stands")
