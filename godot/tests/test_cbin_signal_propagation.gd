extends GutTest

# Probe object that counts signal emissions.
class SignalCounter:
	extends RefCounted
	var changed := 0
	var structure := 0
	func on_changed() -> void:
		changed += 1
	func on_structure() -> void:
		structure += 1

func test_entry_mutation_fires_parent_changed() -> void:
	var res := CbinCreditsResource.new()
	var entry := CbinTextEntry.new()
	entry.set_text("initial")
	res.add_entry(entry)

	var counter := SignalCounter.new()
	res.changed.connect(counter.on_changed)
	counter.changed = 0  # zero out any add_entry emission

	entry.set_text("mutated")
	assert_eq(counter.changed, 1, "parent must re-emit changed when an entry mutates")

func test_image_texture_name_mutation_fires_parent_changed() -> void:
	var res := CbinCreditsResource.new()
	var entry := CbinImageEntry.new()
	entry.set_texture_name("initial.png")
	res.add_entry(entry)

	var counter := SignalCounter.new()
	res.changed.connect(counter.on_changed)
	counter.changed = 0

	entry.set_texture_name("missing.png")
	assert_eq(counter.changed, 1, "parent must re-emit changed when a missing image filename changes")

func test_entry_mutation_does_not_fire_structure_changed() -> void:
	var res := CbinCreditsResource.new()
	var entry := CbinTextEntry.new()
	res.add_entry(entry)

	var counter := SignalCounter.new()
	res.entries_structure_changed.connect(counter.on_structure)
	counter.structure = 0

	entry.set_text("mutated")
	assert_eq(counter.structure, 0, "entry mutation must NOT fire entries_structure_changed")

func test_add_entry_fires_both_signals() -> void:
	var res := CbinCreditsResource.new()
	var counter := SignalCounter.new()
	res.changed.connect(counter.on_changed)
	res.entries_structure_changed.connect(counter.on_structure)

	res.add_entry(CbinTextEntry.new())
	assert_eq(counter.changed, 1, "add fires changed once")
	assert_eq(counter.structure, 1, "add fires entries_structure_changed once")

func test_remove_disconnects_entry() -> void:
	var res := CbinCreditsResource.new()
	var entry := CbinTextEntry.new()
	res.add_entry(entry)

	var counter := SignalCounter.new()
	res.changed.connect(counter.on_changed)

	res.remove_entry(0)
	counter.changed = 0  # ignore remove_entry's own emission

	entry.set_text("after remove")
	assert_eq(counter.changed, 0, "removed entry must not propagate")

func test_clear_disconnects_all_entries() -> void:
	var res := CbinCreditsResource.new()
	var a := CbinTextEntry.new()
	var b := CbinTextEntry.new()
	res.add_entry(a)
	res.add_entry(b)

	var counter := SignalCounter.new()
	res.changed.connect(counter.on_changed)

	res.clear_entries()
	counter.changed = 0

	a.set_text("after clear a")
	b.set_text("after clear b")
	assert_eq(counter.changed, 0, "cleared entries must not propagate")

func test_from_text_connects_new_entries() -> void:
	var res := CbinCreditsResource.new()
	var ok := res.from_text("[ENV]\nscroll_rate=0.5\n[TEXT]\nHello\n")
	assert_true(ok, "from_text succeeds on valid input")
	assert_eq(res.get_entry_count(), 1, "one entry after from_text")

	var counter := SignalCounter.new()
	res.changed.connect(counter.on_changed)
	counter.changed = 0

	var entry := res.get_entry(0) as CbinTextEntry
	entry.set_text("Mutated after from_text")
	assert_eq(counter.changed, 1, "entries created via from_text must propagate")
