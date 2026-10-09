// The convex solid a collision volume (BVOL) bounds, as facet polygons: the
// geometry of a volume's planes, for anything that draws or measures a volume
// (the C++ twin of the Blender add-on's volume_facets,
// tools/blender/opennova_3di/importer.py).

#pragma once

#include <stddef.h>

#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>

namespace opennova::threedi {

// The convex solid of a volume's planes (n . p + d <= 0 inside, d the plane's stored radius: the game's
// test [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150, (n . p >> 14) + d - r < 0]), as polygons in
// mission axes, one per plane it has a face on: the add-on's rule (tools/blender/opennova_3di/importer.py
// volume_facets), corners kept within 1 mm of the solid. Empty when the planes bound no solid (a flat
// ladder keeps its facing's polygon).
std::vector<std::vector<ThreediBuildVec3>> threedi_volume_polygons(const ThreediBoundingPlane *planes,
                                                                   size_t count, bool ladder);

// The solid the game tests a volume as: its planes' solid within its stored box, the box being the quick
// test a point outside of is never inside (docs/world/world-wac-ai-re.md section 15.2). The box's planes
// join the volume's own only where those reach past it (two of the install's 14,811 volumes, I_LFP2's and
// I_LFPB's, by 8 cm), so a volume within its box keeps its own facets alone; a box stored inside out (four
// more, z above z) clips nothing: its planes' solid stands as the author made it.
std::vector<std::vector<ThreediBuildVec3>> threedi_volume_solid(const ThreediBoundingVolume &volume,
                                                                const ThreediBoundingPlane *planes);

} // namespace opennova::threedi
