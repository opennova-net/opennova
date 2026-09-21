extends GutTest

const PROBE_PATH := "res://probes/render/foliage_spawn_capture_probe.gd"
const ProbeScript := preload(PROBE_PATH)


func test_spawn_capture_requires_requested_runtime_expansion_and_archive_winners() -> void:
	assert_eq(ProbeScript.runtime_mount_validation_error(
		"revx02", "revx02", true), "")
	assert_ne(ProbeScript.runtime_mount_validation_error(
		"revx02", "", true), "",
		"A silent expansion-to-base fallback must make the capture fail.")
	assert_ne(ProbeScript.runtime_mount_validation_error(
		"revx02", "revx02", false), "",
		"A loose/editor root must not masquerade as the packed comparison mount.")

	var winning_entries := [
		{
			"logical_name": "00TRa.bms",
			"source_type": "pff",
			"archive_path": "C:/Game/JO/localres.pff",
		},
		{
			"logical_name": "00TRa.trn",
			"source_type": "pff",
			"archive_path": "C:/Game/JO/expansion/revx02/RevX02.pff",
		},
		{
			"logical_name": "00TRa.env",
			"source_type": "pff",
			"archive_path": "C:/Game/JO/expansion/revx02/RevX02.pff",
		},
	]
	assert_eq(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries), "")
	winning_entries[0]["archive_path"] = ""
	assert_ne(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries), "",
		"The compared mission itself must name its packed winning archive.")
	winning_entries[0]["archive_path"] = "C:/Game/JO/localres.pff"
	winning_entries[2]["source_type"] = "loose"
	assert_ne(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries), "",
		"Every reported comparison input must name its packed winning source.")
	winning_entries[2]["source_type"] = "pff"
	winning_entries[0] = {
		"logical_name": "00TRa.bms",
		"source_type": "loose",
		"source_path": "C:/Authoring/00TRa.bms",
		"archive_path": "",
	}
	assert_eq(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries, true), "",
		"A saved loose mission may drive a standalone packed-runtime capture.")


func test_spawn_capture_requires_runtime_foliage_for_00tre_only() -> void:
	var empty_stats := FoliageFrameStats.new()
	assert_false(ProbeScript.runtime_foliage_validation_error(
		"00TRe.bms", empty_stats, 0).is_empty(),
		"The foliage oracle must not PASS 00TRe when dispatch produced nothing.")
	var intent_only := FoliageFrameStats.new()
	intent_only.runtime_detail_intents = 1
	assert_false(ProbeScript.runtime_foliage_validation_error(
		"00TRe.bms", intent_only, 1).is_empty(),
		"Intent-only 00TRe output is not enough when the reimpl has no detail instances.")
	var one_instance := FoliageFrameStats.new()
	one_instance.runtime_detail_intents = 1
	one_instance.detail_high_instances = 1
	assert_eq(ProbeScript.runtime_foliage_validation_error("00TRe.bms", one_instance, 1), "")
	assert_eq(ProbeScript.runtime_foliage_validation_error(
		"00TRa.bms", empty_stats, 0), "",
		"00TRa's exact spawn is an intentional zero-visible-foliage control.")
