class_name ParticleFixture
extends RefCounted

# Renderer fixtures enter through the real PTL loader. These helpers only
# compose authored sections; native parser/runtime tests own their semantics.
static func parse(test: GutTest, text: String, source: String = "") -> ParticleFile:
	var file := ParticleFile.new()
	test.assert_eq(file.load_from_buffer(text.to_utf8_buffer(), source), OK,
			"the fixture PTL text loads")
	return file


static func definition(id: String, properties: String) -> String:
	return "[particledef]\n{\nid = %s;\n%s\n}\n" % [id, properties]


static func effect(id: String, particles: PackedStringArray) -> String:
	return "[effectdef]\n{\nid = %s;\npdefs = %s;\n}\n" % [id, ", ".join(particles)]


static func catalog(test: GutTest, id: String, properties: String,
		effects: PackedStringArray) -> ParticleFile:
	var text := definition(id, properties)
	for name in effects:
		text += effect(name, [id])
	return parse(test, text)
