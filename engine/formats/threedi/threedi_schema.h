// The .3di property table (ADR 0046 d9, A16): the engine features of a model an
// editor changes, named once by a dotted path per record, over the engine's own
// records (Threedi3di3's ThreediMaterial, ThreediLight, ThreediUserPoint,
// ThreediPartAnimation, ...; ADR 0027: the parsed struct is what an editor
// changes). The units are the `.o3d` scene text's (docs/threedi/o3d-scene-format.md):
// mission axes, byte colours, raw PANM track words, 16.16 truncated as the
// exporter stores a point; a get is `scene`'s reading and a set is `build`'s, so
// the editor and the Blender add-on agree. Each field names its unit where it has one
// and the row it shares (a position's axes, a colour's channels, a frame's row), each
// member named by its component. Geometry (vertices, strips, collision planes and
// faces' corners) is not here: it is authored in Blender.
//
// A Set of the value a field already reads changes nothing, so a retail word that
// sits off the text's grid survives a Set of every field to its own value. Only
// the words a field derives are derived again when it changes (a light's axis and
// view_proj follow its position, direction, cone and reach, never its colour).
// Nothing here names an editor type (formats/mnu/mnu_schema.h is the precedent); the
// editor's model document (engine/editor/documents/model_document) projects it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::threedi {

// The records the table describes, and the native struct behind each:
// Model ThreediHeader, Lod ThreediLod, PartAnimation ThreediPartAnimation,
// Material ThreediMaterial, Texture ThreediMaterialTexture, Light ThreediLight,
// UserPoint ThreediUserPoint, Register ThreediControlRegister, Frame
// ThreediMatrix4x4 (an MTRX row), Section ThreediCollisionObject, Volume
// ThreediBoundingVolume, Face ThreediCollisionFace, Occlusion
// ThreediOcclusionObject.
enum class ThreediSchemaShape {
	Model, Lod, PartAnimation, Material, Texture, Light, UserPoint, Register, Frame, Section, Volume, Face, Occlusion,
};

enum class ThreediSchemaType { Integer, Real, Text };

// What a field names outside its record: a texture file, a CTRL register (by its
// index in the model's table), a part (by its index in LOD 0), an MTRX frame (by
// its row).
enum class ThreediSchemaReference { None, Texture, Register, Part, Frame };

using ThreediSchemaValue = std::variant<int64_t, double, std::string>;

struct ThreediSchemaChoice {
	const char *name = "";
	int64_t value = 0;
	const char *label = ""; // "" = the name
};

struct ThreediSchemaField {
	const char *path = "";        // "position.x", "rgbgen.start.r", "rotx.style"
	ThreediSchemaType type = ThreediSchemaType::Integer;
	size_t width = 0;             // a text's capacity in bytes, the terminator included
	int64_t min = 0, max = 0;     // an integer's range
	bool read_only = false;       // shown, never set (geometry, derived words)
	bool flags = false;           // the choices are bits of one integer
	ThreediSchemaReference reference = ThreediSchemaReference::None;
	std::vector<ThreediSchemaChoice> choices;
	const char *label = "";       // "" = the path; a component's names it ("Position X")
	const char *unit = "";        // what the value is in ("m", "deg"; "" = none)
	const char *group = "";       // the row it shares with its neighbours of the group ("" = none)
	bool channel = false;         // a colour's red, green or blue byte in a group of the three
	// What the game makes of the value is not witnessed: shown as the file holds it, never
	// set (the second-channel material words, a light's byte 34).
	bool unverified = false;
	const char *note = "";        // what the value is, where the label cannot say it
};

struct ThreediSchemaRecord {
	ThreediSchemaShape shape = ThreediSchemaShape::Model;
	void *data = nullptr;
	explicit operator bool() const { return data != nullptr; }
};

const std::vector<ThreediSchemaField> &threedi_schema_fields(ThreediSchemaShape shape);
const ThreediSchemaField *threedi_schema_field(ThreediSchemaShape shape, const std::string &path);

bool threedi_schema_get(const ThreediSchemaRecord &record, const std::string &path, ThreediSchemaValue &out);
// A value within the field's type, width and range; a Set of the value the field
// already reads changes nothing. False, with `error`, when the value does not fit
// or the field is read-only.
bool threedi_schema_set(const ThreediSchemaRecord &record, const std::string &path, const ThreediSchemaValue &value,
		std::string &error);
// Whether the game reads the field on this record: a generator's register only
// above style 0x70 and its phase only at or below [orig: ThreediGp_LoadCtrlRegisters
// @ 0x5B4640]; a PANM track only when its flags make it present [orig:
// PANM_SampleTrack @ 0x5B2270]; a PANM row's rotation frame only on a spinner or
// Euler row (threedi_panm_frame_row); a light's axis and cone only on a spot light.
bool threedi_schema_reads(const ThreediSchemaRecord &record, const std::string &path);
// What the field names on this record: a generator's or a track's parameter is a
// register only above style 0x70, a flipbook's time only when the flipbook reads a
// register, a PANM row's frame byte an MTRX row only as threedi_panm_frame_row reads
// it (0 and 0x80..0xFF name none).
ThreediSchemaReference threedi_schema_reference(const ThreediSchemaRecord &record, const std::string &path);

} // namespace opennova::threedi
