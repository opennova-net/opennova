#include <base/version/one_build.h>

// engine/base/CMakeLists.txt sets these on this TU alone (set_source_files_properties): the
// stamp's version, commit, staging flag (0 or 1) and commit time, checked there.
#if !defined(OPENNOVA_VERSION) || !defined(OPENNOVA_COMMIT) || !defined(OPENNOVA_STAGING) ||       \
		!defined(OPENNOVA_BUILD_TIME)
#error "one_build.cpp is compiled with the ONE stamp only: engine/base/CMakeLists.txt sets it"
#endif

namespace opennova {

Version OneBuild::wire_version() const {
	Version out;
	out.count = 4;
	for (size_t i = 0; i < 3; ++i)
		out.parts[i] = version.parts[i];
	return out;
}

std::string OneBuild::date_text() const {
	return build_date_text(time);
}

std::string OneBuild::label() const {
	std::string out = "ONE";
	const std::string number = version.text();
	if (!number.empty())
		out += " " + number;
	std::string notes = commit;
	if (staging)
		notes += notes.empty() ? "staging" : ", staging";
	return notes.empty() ? out : out + " (" + notes + ")";
}

const OneBuild &one_build() {
	static const OneBuild build = [] {
		OneBuild stamp;
		// CMake refuses a stamp that does not parse, so the fallback is never taken.
		stamp.version = Version::parse(OPENNOVA_VERSION).value_or(Version{});
		stamp.commit = OPENNOVA_COMMIT;
		stamp.staging = OPENNOVA_STAGING != 0;
		stamp.time = static_cast<int64_t>(OPENNOVA_BUILD_TIME);
		return stamp;
	}();
	return build;
}

} // namespace opennova
