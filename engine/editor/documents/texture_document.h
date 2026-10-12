#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/texture_image.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/edit_history.h>
#include <editor/model/finding_code_row.h>
#include <editor/documents/texture_raster.h>
#include <editor/model/raster_document.h>
#include <editor/model/value.h>

namespace opennova::editor {

// A whole image a texture document takes as one edit (ADR 0046 S18, an Apply edit's payload,
// "texture.image"): the file's new bytes, made by a texture operation through the editor's writers
// (texture_operations.h), and the step's words.
struct TextureImageEdit final : EditPayload {
	std::vector<uint8_t> bytes;
	std::string words;
	const char *token() const override { return "texture.image"; }
};

// A texture as a document (ADR 0046 S18; CONTEXT.md "Texture document"): a .tga, .mdt, .pcx, .dds or
// .png file of the project, read as the game reads it (texture_image.h: the texels the reader its name picks
// decodes, the palette of an indexed one, the levels a DDS holds, and what it is in a modder's words). Since S25
// (B3) it is a raster document (documents/texture_raster.h): one plane of its texels where a brush paints its form
// (an 8-bit PCX's indices, an uncompressed TGA's colours, a true-colour PNG's), beside the form its file is written
// in again; the file whole where none does. It takes whole images (an Apply edit of a TextureImageEdit, one undo
// step each: a resize, an alpha, a stored form, its rows, its palette's indices: a whole change of the raster) and
// texels painted (a TexturePaintEdit, S25 B4: tile regions, a stroke's batches one step), its history under the
// history's byte budget, and Save writes the state it is at: the file as it was read while its texels are those it
// read, else its form written again from them. Its picture is the texture viewport (preview/texture_viewport.h),
// the Document tab's main view, which the Preview window shows too for a texture Files selects.
class TextureDocument final : public RasterDocument {
public:
	explicit TextureDocument(HistoryBudget budget = {});
	// The copy a snapshot is (DocumentBase::snapshot).
	TextureDocument(const TextureDocument &other) = default;

	// The texture as the game reads it (null before a load): decoded as it is first asked for, so a
	// validation that reads only its header (validate_texture_file) never decodes its texels.
	const std::shared_ptr<const TextureImage> &image() const;
	// The file's bytes as the document holds them now: as read (decoded as a document's are: the base's) while
	// its texels and form are those it read, else its form written from its texels.
	const std::vector<uint8_t> &bytes() const;
	// Whether a brush paints it (one plane of its texels: texture_raster.h's paintable forms).
	bool paintable() const { return texture_paintable(state()); }

	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override;
	bool holds_bytes() const override { return true; }

protected:
	bool apply_edits(const std::vector<Edit> &edits, Diagnostic &error) override;
	bool read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
	                 Diagnostic &error) override;

private:
	// What the document read (the bytes and the state they made), and its bytes and image as they stand.
	std::shared_ptr<const std::vector<uint8_t>> read_bytes_;
	RasterState read_state_;
	bool loaded_ = false;
	mutable uint64_t bytes_revision_ = 0, bytes_generation_ = 0;
	mutable std::shared_ptr<const std::vector<uint8_t>> bytes_;
	mutable uint64_t image_revision_ = 0, image_generation_ = 0;
	mutable std::shared_ptr<const TextureImage> image_;
	// The states whole steps made (texture operations) and the bytes each was read from: a save of that state writes
	// those bytes, never a re-encode of them (the last kWholeKept).
	std::vector<std::pair<RasterState, std::shared_ptr<const std::vector<uint8_t>>>> whole_bytes_;
	// The image of the bytes read, decoded once a load; a painted state's image is made from it and the plane.
	mutable uint64_t read_image_generation_ = UINT64_MAX;
	mutable std::shared_ptr<const TextureImage> read_image_;
	// The image of a paintable state built from its plane and side over the image read (no codec round trip: a paint
	// shows per dab); null where it cannot be (not paintable, the read image not one decoded level of the plane's
	// sides): image() decodes the bytes then.
	std::shared_ptr<const TextureImage> painted_image_() const;
};

// The texture type's findings (ADR 0046 S18), each on a texture file: its own, what the reader its name
// picks makes of it whatever uses it (validate_texture_file, from its header: texture_header), and a file
// of a name its loader passes over (graph/texture_checks). What a use's role asks of the file its loader
// opens is a finding on the use, the referring file's, whatever its type: the core's texture codes
// (CoreFinding::Texture*). An error gates (the game writes past the image, or reads past it into its own
// memory); the others are warnings, an image the game loads and goes on from among them. Each finding says what the game does, its witness cited where
// it is made.
enum class TextureFinding {
	Unloadable,          // texture.unloadable: the reader its name picks refuses it
	TgaUnfilled,         // texture.tga_unfilled: a TGA form the reader leaves unset, the game going on
	TgaZeroed,           // texture.tga_zeroed: a TGA form the reader zeroes
	TgaUpsideDown,       // texture.tga_upside_down: rows top first, the reader takes them bottom up
	TgaColourMapSkipped, // texture.tga_colour_map_skipped: a true-colour TGA's colour map read as texels
	PcxOverrun,          // texture.pcx_overrun (an error): an 8-bit PCX whose rows are longer than its width
	NotRead,             // texture.not_read: a file its loader passes over for another of the name
	TgaTruncated,        // texture.tga_truncated (an error): the file ends before the texels the reader copies
	PcxShortRows,        // texture.pcx_short_rows: an 8-bit PCX whose rows are shorter than its width
	DdsNotPowerOfTwo,    // texture.dds_not_pow2: a DDS-reader image whose sides are not powers of two
	kCount,
};
const FindingCodeRow &finding_code(TextureFinding code);

// The texture type's row (documents/document_types.cpp): its documents; its file's own findings (the
// reader faults its header shows, whatever reads it: TextureFinding's first rows); its fields (none: a
// texture holds no records); its finding codes (TextureFinding's); and its content on the wire, the
// document query's `texture`: the reader, whether the game loads it and why not, whether its texels
// are decoded, its sides, levels, palette size, alpha and facts.
std::unique_ptr<DocumentBase> make_texture_document();
std::vector<Diagnostic> validate_texture_file(const DocumentBase &document);
const std::vector<FieldSchema> &texture_fields(NodeKind kind);
FindingTable texture_finding_codes();
io::JsonValue texture_content_json(const DocumentBase &document);
// A texture's facts on the wire (the document query's and the viewport's body).
io::JsonValue texture_image_json(const TextureImage &image);

} // namespace opennova::editor
