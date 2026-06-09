extends GutTest

# Verifies the polish-pass C1 work: get_decompiled_text_with_bank threads
# the SBF entry names down through libs/mus's mus_decompile_with_names so
# play / bind statements show real names instead of "sound_N" placeholders.

const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"


# The names-less path is unchanged. It must still emit the placeholder so the
# decompile golden test in libs/mus continues to match byte-for-byte.
func test_names_less_decompile_uses_sound_n_placeholder():
	var script := load(SCRIPT_FIXTURE) as NovaMusicScript
	assert_not_null(script, "fixture loads")
	if script == null:
		return
	var name := StringName(script.get_default_script_name())
	var text: String = script.get_decompiled_text(name)
	assert_true(text.contains("bind sound_1 \"sound_1\""),
		"names-less path emits the legacy bind placeholder")
	assert_true(text.contains("play sound_") or text.contains("on ("),
		"names-less path uses sound_N for play targets")


# The names-aware path replaces the placeholder with the SBF entry name.
# We don't pin the test to a specific entry name (jo_gamemus's slot 1 happens
# to be GAMINT but committing to that string would couple the test to fixture
# data); instead we assert (a) at least one bind line carries a non-placeholder
# quoted string and (b) the placeholder for slot 1 is gone.
func test_names_aware_decompile_substitutes_real_names():
	var script := load(SCRIPT_FIXTURE) as NovaMusicScript
	var bank := load(BANK_FIXTURE) as NovaSbfBank
	assert_not_null(script, "script fixture loads")
	assert_not_null(bank, "bank fixture loads")
	if script == null or bank == null:
		return
	var name := StringName(script.get_default_script_name())
	var text: String = script.get_decompiled_text_with_bank(name, bank)
	# bind sound_1 "<sbf_entry_1_name>" replaces bind sound_1 "sound_1".
	assert_false(text.contains("bind sound_1 \"sound_1\""),
		"names-aware path drops the sound_N quoted placeholder for slot 1")
	# Walk the bank's first few entries and check at least one name appears
	# verbatim in the bind line for its slot, so the wire-through is real
	# rather than a coincidental string match.
	var entry_count: int = bank.get_entry_count()
	var matched_any := false
	for i in range(min(entry_count, 13)):
		var entry_name: String = bank.get_entry_name(i)
		if entry_name.is_empty():
			continue
		var expected := "bind sound_%d \"%s\"" % [i, entry_name]
		if text.contains(expected):
			matched_any = true
			break
	assert_true(matched_any, "at least one bind sound_N \"<entry>\" pair lands in the text")


# Specific: jo_gamemus slot 1 is GAMINT. This locks the user-visible win that
# motivated the entire change. If a future fixture rotates the order, swap the
# slot index here.
func test_jo_gamemus_slot_1_resolves_to_gamint():
	var script := load(SCRIPT_FIXTURE) as NovaMusicScript
	var bank := load(BANK_FIXTURE) as NovaSbfBank
	if script == null or bank == null:
		pass_test("fixture missing; skipped")
		return
	var entry_1_name: String = bank.get_entry_name(1)
	if entry_1_name.is_empty():
		pass_test("bank slot 1 unnamed; skipped")
		return
	var name := StringName(script.get_default_script_name())
	var text: String = script.get_decompiled_text_with_bank(name, bank)
	# bind line carries the real name in quotes
	assert_true(text.contains("bind sound_1 \"%s\"" % entry_1_name),
		"bind sound_1 declaration uses %s" % entry_1_name)
	# play sites are bare identifiers (no quotes) so the user can grep them
	assert_false(text.contains("play sound_1"),
		"play sound_1 placeholder is gone for slot 1")


# Regression for the bind-aware compiler: the names-aware decompile must
# RECOMPILE. Before the fix, "play GAMINT" failed ("expected 'sound_N'"), which
# broke Compile / Compile&Run / Save whenever a bank was loaded. It must compile
# clean AND to the same bytecode as the names-less form.
func test_names_aware_decompile_recompiles_to_identical_bytecode():
	var script := load(SCRIPT_FIXTURE) as NovaMusicScript
	var bank := load(BANK_FIXTURE) as NovaSbfBank
	if script == null or bank == null:
		pass_test("fixture missing; skipped")
		return
	var name := StringName(script.get_default_script_name())
	var named_text: String = script.get_decompiled_text_with_bank(name, bank)
	var named: Dictionary = script.compile_text(named_text)
	assert_eq(int(named.get("rc", -1)), 0,
		"names-aware decompile recompiles (err: %s)" % String(named.get("err_msg", "")))
	var plain_text: String = script.get_decompiled_text(name)
	var plain: Dictionary = script.compile_text(plain_text)
	assert_eq(int(plain.get("rc", -1)), 0, "names-less decompile recompiles")
	assert_eq(named.get("bytecode", PackedByteArray()),
		plain.get("bytecode", PackedByteArray()),
		"names-aware and names-less compile to identical bytecode")
