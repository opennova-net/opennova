class_name ParticleFixture
extends RefCounted

# Renderer fixtures enter through the real PTL loader. These helpers only
# compose authored sections; native parser/runtime tests own their semantics.
# The game's reader cuts lines at CR LF alone and ends a word at a blank
# (formats/particle parser.cpp), so the text goes in CR LF and every name is
# quoted, a quoted run being one word.
static func parse(test: GutTest, text: String, source: String = "") -> ParticleFile:
	var file := ParticleFile.new()
	var crlf := text.replace("\r\n", "\n").replace("\n", "\r\n")
	test.assert_eq(file.load_from_buffer(crlf.to_utf8_buffer(), source), OK,
			"fixture PTL parses through ParticleFile")
	return file


static func definition(id: String, properties: String) -> String:
	return "[particledef]\n{\nid = \"%s\";\n%s\n}\n" % [id, properties]


static func effect(id: String, particles: PackedStringArray) -> String:
	var quoted := PackedStringArray()
	for name in particles:
		quoted.append("\"%s\"" % name)
	return "[effectdef]\n{\nid = \"%s\";\npdefs = %s;\n}\n" % [id, ", ".join(quoted)]


static func catalog(test: GutTest, id: String, properties: String,
		effects: PackedStringArray) -> ParticleFile:
	var text := definition(id, properties)
	for name in effects:
		text += effect(name, [id])
	return parse(test, text)
