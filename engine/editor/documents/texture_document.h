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
#include <editor/model/finding_code_row.h>
#include <editor/model/value.h>

namespace opennova::editor {

// A texture as a document (ADR 0046 S18; CONTEXT.md "Texture document"): a .tga, .mdt, .pcx, .dds or
// .png file of the project, held as the file stores it and read as the game reads it (texture_image.h:
// the texels the reader its name picks decodes, the palette of an indexed one, the levels a DDS holds,
// and what it is in a modder's words). Read only for now: it takes no edit (document.payload), has no
// history, and Save writes nothing new (it serializes to the bytes it was read from). Its picture is the
// texture viewport (preview/texture_viewport.h), the Document tab's main view, which the Preview window
// shows too for a texture Files selects.
class TextureDocument final : public DocumentBase {
public:
	TextureDocument() = default;
	// The copy a snapshot is (DocumentBase::snapshot).
	TextureDocument(const TextureDocument &other) = default;

	// The texture as the game reads it (null before a load): decoded as it is first asked for, so a
	// validation that reads only its header (validate_texture_file) never decodes its texels.
	const std::shared_ptr<const TextureImage> &image() const;
	// The file's bytes as the document read them (decoded as a document's are: the base's).
	const std::vector<uint8_t> &bytes() const { return bytes_; }

	void end_edit_group() override {}
	bool dirty() const override { return false; }
	bool can_undo() const override { return false; }
	bool can_redo() const override { return false; }
	uint64_t revision() const override { return 0; }
	size_t history_bytes() const override { return 0; }
	// Nothing changes: the state a caller read is the one it holds (its load the same).
	bool changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override;
	bool holds_image() const override { return true; }

protected:
	bool apply_edits(const std::vector<Edit> &edits, Diagnostic &error) override;
	void undo_step() override {}
	void redo_step() override {}
	bool read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
	                 Diagnostic &error) override;
	void on_saved() override {}

private:
	std::vector<uint8_t> bytes_;
	bool loaded_ = false;
	mutable std::shared_ptr<const TextureImage> image_;
};

// The texture type's findings (ADR 0046 S18), each on a texture file: its own, what the reader its name
// picks makes of it whatever uses it (validate_texture_file, from its header: texture_header), and a file
// of a name its loader passes over (graph/texture_checks). What a use's role asks of the file its loader
// opens is a finding on the use, the referring file's, whatever its type: the core's texture codes
// (CoreFinding::Texture*). An error gates (the game draws texels the editor cannot vouch for, or writes
// past the image); the others are warnings. Each finding says what the game does, its witness cited where
// it is made.
enum class TextureFinding {
	Unloadable,          // texture.unloadable: the reader its name picks refuses it
	TgaUnfilled,         // texture.tga_unfilled (an error): a TGA form the reader leaves unset
	TgaZeroed,           // texture.tga_zeroed: a TGA form the reader zeroes
	TgaUpsideDown,       // texture.tga_upside_down: rows top first, the reader takes them bottom up
	TgaColourMapSkipped, // texture.tga_colour_map_skipped: a true-colour TGA's colour map read as texels
	PcxOverrun,          // texture.pcx_overrun (an error): an odd-width 8-bit PCX's rows overrun
	NotRead,             // texture.not_read: a file its loader passes over for another of the name
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
