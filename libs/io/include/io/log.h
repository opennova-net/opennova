#pragma once
// The one diagnostic channel for every libs/ library. Libraries never write to
// stdout/stderr themselves (the `libs_stdout_prints` ratchet holds that at
// zero): they call io::logf and stay SILENT unless the embedder — an app binary,
// the GDExtension, a test — installs a sink with io::set_log_sink. The
// formatted message is assembled only when a sink is installed, so a silent
// library pays one branch per call (this is what makes per-tick diagnostics
// affordable in the 62 Hz main loop). Messages arrive WITHOUT a trailing
// newline; the sink owns presentation.
#include <cstdarg>
#include <cstdio>

// C++14-compatible namespacing: several consumer libs predate the repo's
// cxx_std_17 default.
namespace opennova {
namespace io {

enum class LogLevel {
	kDebug = 0, // high-volume tracing (per-tick, per-record); embedders usually filter it out
	kInfo = 1,  // lifecycle narration (a service started, a stage completed)
	kWarn = 2,  // tolerated anomaly (parse oddity, clamped value, ignored row)
	kError = 3, // an operation failed; the caller is also reporting it via its return path
};

using LogSink = void (*)(LogLevel level, const char *message);

// One sink slot per process (function-local static: a single instance across
// every TU that inlines this header). Null = silent.
inline LogSink &log_sink_slot() {
	static LogSink sink = nullptr;
	return sink;
}

inline void set_log_sink(LogSink sink) { log_sink_slot() = sink; }

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
inline void logf(LogLevel level, const char *fmt, ...) {
	const LogSink sink = log_sink_slot();
	if (sink == nullptr) return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sink(level, buf);
}

} // namespace io
} // namespace opennova
