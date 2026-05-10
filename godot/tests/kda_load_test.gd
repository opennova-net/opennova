extends GutTest

const KDA_PATH := "res://assets/credits/nlist.kda"


func test_kda_loads_as_cbin_credits_resource() -> void:
	var res = ResourceLoader.load(KDA_PATH)

	assert_not_null(res, "nlist.kda should load through the registered KdaResourceFormatLoader.")
	if res == null:
		return

	assert_true(res.get_class() == "CbinCreditsResource",
		"Loaded resource should be a CbinCreditsResource, got: %s" % res.get_class())

	assert_true(res.get_entry_count() > 0,
		"nlist.kda should contain at least one credits entry.")

	assert_true(res.get_scroll_rate() > 0.0,
		"scroll_rate should be positive, got: %.4f" % res.get_scroll_rate())

	print("PASS: loaded %s with %d entries, scroll_rate=%.2f" % [
		KDA_PATH, res.get_entry_count(), res.get_scroll_rate()
	])
