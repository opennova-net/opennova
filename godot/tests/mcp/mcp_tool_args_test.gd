extends GutTest

# McpToolArgs: the typed reads every endpoint's handlers validate a call's
# JSON arguments with (a JSON number arrives as an int or a float; anything
# else, or a non-finite or fractional value where a whole one is asked, reads
# null so the handler refuses the call instead of erroring mid-call).


func test_finite_number_takes_ints_and_finite_floats_only() -> void:
	assert_eq(McpToolArgs.finite_number(3), 3.0)
	assert_eq(McpToolArgs.finite_number(0.25), 0.25)
	for value in [INF, -INF, NAN, "1", [1], {"v": 1}, null, true]:
		assert_null(McpToolArgs.finite_number(value), "%s is no finite number" % [value])


func test_integer_number_takes_whole_values_only() -> void:
	assert_eq(McpToolArgs.integer_number(7), 7)
	assert_eq(McpToolArgs.integer_number(7.0), 7, "a JSON 7.0 is the integer 7")
	assert_eq(typeof(McpToolArgs.integer_number(7.0)), TYPE_INT)
	for value in [7.5, INF, NAN, "7", [7], null]:
		assert_null(McpToolArgs.integer_number(value), "%s is no whole number" % [value])
