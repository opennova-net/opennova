#pragma once

#include <memory>

#include <editor/preview/mission_scene.h>

namespace opennova::editor {

class Document;

// What a mission viewport's scene reads from (ADR 0046 S14): the mission document's typed reads
// (documents/mission_reads.h: its entities pool by pool, its areas, its paths, a row alone, the
// header), never a field by its name per value (3,400 entities open at once). Null for a document
// that is no mission.
std::unique_ptr<MissionSceneSource> mission_scene_source(const Document &document);

} // namespace opennova::editor
