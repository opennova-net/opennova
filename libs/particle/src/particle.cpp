#include "particle/particle.h"

namespace opennova::particle {

const ParticleDef *ParticleFile::find_particle(std::string_view id) const noexcept {
	for (const ParticleDef &particle : particles) {
		if (particle.id == id) {
			return &particle;
		}
	}
	return nullptr;
}

const TableDef *ParticleFile::find_table(std::string_view id) const noexcept {
	for (const TableDef &table : tables) {
		if (table.id == id) {
			return &table;
		}
	}
	return nullptr;
}

} // namespace opennova::particle
