#pragma once

#include <godot_cpp/classes/ref.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>

#include "resource_index/resource_root.h"

namespace godot {

// The game's mounted resource root as the engine's file source (engine/base/vfs/
// file_source.h), what the menu frame reads its string tables, fonts and textures
// through: a name reads the root's file by its flat name, exactly as the frame read it
// before; its stamp names the mount the root holds now (the global cache epoch, which
// every mount or clear on any root moves, and this root), 0 when the root does not have
// the name. A plain C++ seam, not a Godot class.
class ResourceRootFileSource : public opennova::FileSource {
public:
	ResourceRootFileSource() = default;
	explicit ResourceRootFileSource(const Ref<ResourceRoot> &p_root) :
			root_(p_root) {}
	void set_root(const Ref<ResourceRoot> &p_root) { root_ = p_root; }
	const Ref<ResourceRoot> &root() const { return root_; }

	bool read(const std::string &p_name, std::vector<uint8_t> &r_out) const override;
	uint64_t stamp(const std::string &p_name) const override;

private:
	Ref<ResourceRoot> root_;
};

} // namespace godot
