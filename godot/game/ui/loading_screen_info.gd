class_name LoadingScreenInfo
extends RefCounted
## What a mission load shows on the loading screen: the .bms name driving the
## sidecar background lookup, whether the load carries an MP session (the
## text-overlay gate) and the session variables retail resolves before its
## load (the witnesses sit on LoadingScreen.setup / update_session_info). A
## JOINER's post-auth 0x7B record refreshes the same fields mid-load; empty
## strings and a negative game type leave the current values alone.

var mission_file := ""
var in_session := false
var server_name := ""
var mission_name := ""
var custom_text := ""
## The numeric session game type; -1 = not carried.
var game_type := -1


static func make(mission_file: String, in_session: bool, server_name: String,
		mission_name: String, game_type: int, custom_text: String) -> LoadingScreenInfo:
	var info := LoadingScreenInfo.new()
	info.mission_file = mission_file
	info.in_session = in_session
	info.server_name = server_name
	info.mission_name = mission_name
	info.game_type = game_type
	info.custom_text = custom_text
	return info


## A single-player load: only the mission file matters (no session overlay).
static func for_mission(mission_file: String) -> LoadingScreenInfo:
	return make(mission_file, false, "", "", -1, "")
