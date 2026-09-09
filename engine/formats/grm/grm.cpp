#include <formats/grm/grm.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>

namespace opennova::grm {
namespace {

// Token scratch is 1001 bytes and 30 pointers in the original. Quotes split
// tokens even without surrounding whitespace. [orig: tokenize_config_line @0x588AD0]
std::vector<std::string> tokens(std::string_view line) {
	const size_t nul = line.find('\0');
	std::string scratch(line.substr(0, std::min(size_t{1000}, nul == std::string_view::npos ? line.size() : nul)));
	std::vector<size_t> starts;
	bool quoted = false, start = true;
	for (size_t i = 0; i <= scratch.size(); ++i) {
		const char ch = i < scratch.size() ? scratch[i] : '\0';
		if ((ch == ' ' || ch == ',' || ch == '\t') && !quoted) {
			scratch[i] = '\0';
			start = true;
		} else if (ch == '"') {
			quoted = !quoted;
			start = true;
			scratch[i] = '\0';
		} else {
			if (start) starts.push_back(i);
			start = false;
		}
		if (starts.size() >= 30) break;
	}
	std::vector<std::string> result;
	for (size_t begin : starts) result.emplace_back(scratch.c_str() + begin);
	return result;
}

int32_t integer(const std::string &text) {
	// Win32 atol consumes a decimal prefix, including a sign.
	const long long n = std::strtoll(text.c_str(), nullptr, 10);
	return static_cast<int32_t>(std::clamp(n,
			static_cast<long long>(INT32_MIN), static_cast<long long>(INT32_MAX)));
}

float real(const std::string &text) {
	return static_cast<float>(std::strtod(text.c_str(), nullptr));
}

bool name_fits(const std::string &s, size_t limit) {
	return s.size() <= limit;
}

bool valid(const File &f, std::string &error) {
	auto fail = [&](const char *why) { error = why; return false; };
	if (!name_fits(f.base_texture, 259) || !name_fits(f.eye_textures[0], 259) ||
			!name_fits(f.eye_textures[1], 259) || !name_fits(f.saved.author, 255))
		return fail("GRM texture or author name exceeds its original field");
	for (const Vertex &v : f.vertices)
		if (!name_fits(v.group, 30) || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y))
			return fail("GRM invalid vertex name or coordinate");
	for (const auto &t : f.triangles)
		for (int32_t index : t)
			if (index < 0 || static_cast<size_t>(index) >= f.vertices.size())
				return fail("GRM triangle references a missing vertex");
	for (const Gesture &g : f.gestures) {
		if (!name_fits(g.name, 31) || g.parameters.size() > 32)
			return fail("GRM invalid gesture name or parameter count");
		for (const Parameter &p : g.parameters)
			if (!name_fits(p.group, 30) || !std::isfinite(p.offset.x) || !std::isfinite(p.offset.y))
				return fail("GRM invalid parameter name or offset");
	}
	for (Point p : {f.eye_size, f.eye_centers[0], f.eye_centers[1], f.eye_limits})
		if (!std::isfinite(p.x) || !std::isfinite(p.y))
			return fail("GRM invalid eye coordinate");
	return true;
}

} // namespace

bool parse(const uint8_t *data, size_t size, File &out, std::string &error) {
	error.clear();
	if (!data && size) { error = "GRM missing input"; return false; }
	File file;
	const std::string_view text(data ? reinterpret_cast<const char *>(data) : "", size);
	size_t position = 0, line_number = 0;
	int32_t gesture_index = 0;
	auto fail = [&](const char *why) {
		error = "GRM line " + std::to_string(line_number) + ": " + why;
		return false;
	};
	// [orig: FaceAnimConfig_LoadFile @0x588BE0]
	while (position < size) {
		++line_number;
		const size_t delimiter = text.find("\r\n", position);
		const size_t end = delimiter == std::string_view::npos ? size - 1 : delimiter;
		const std::string line(text.substr(position, end - position));
		position = end + 2;
		// These comments are ignored by the runtime loader; parse their named
		// fields so an unedited writer-produced document keeps its save stamp.
		auto &s = file.saved;
		if (std::sscanf(line.c_str(), "; Saved on %d/%d/%d, %d:%d:%d",
				&s.month, &s.day, &s.year, &s.hour, &s.minute, &s.second) == 6) continue;
		constexpr std::string_view author = "; Last edited by ";
		if (line.compare(0, author.size(), author) == 0) {
			s.author = line.substr(author.size());
			continue;
		}
		const auto t = tokens(line);
		if (t.empty() || t[0].empty() || t[0][0] == '/') continue;
		auto is = [&](const char *key) { return strutil::iequals(t[0], key); };
		auto count = [&](auto &rows, size_t maximum) {
			if (t.size() < 2) return false;
			const int32_t n = integer(t[1]);
			if (n < 0 || static_cast<size_t>(n) > maximum) return false;
			rows.resize(static_cast<size_t>(n));
			return true;
		};
		auto indexed = [&](size_t count, size_t argc, int32_t &i) {
			if (t.size() < argc) return false;
			i = integer(t[1]);
			return i >= 0 && static_cast<size_t>(i) < count;
		};
		// [orig: FaceAnimConfig_ParseProperty @0x5886A0]
		int32_t index = 0;
		if (is("basetexture")) {
			if (t.size() < 2) return fail("missing base texture");
			file.base_texture = t[1];
		} else if (is("eyetexture")) {
			if (t.size() < 2) return fail("missing eye texture");
			if (!t[1].empty()) {
				if (t.size() < 3) return fail("missing second eye texture");
				if (!t[2].empty()) file.eye_textures = {t[1], t[2]};
			}
		} else if (is("vertices")) {
			if (!count(file.vertices, size)) return fail("invalid vertex count");
		} else if (is("vertex")) {
			if (!indexed(file.vertices.size(), 5, index)) return fail("invalid vertex index");
			file.vertices[index] = {{real(t[2]), real(t[3])}, t[4]};
		} else if (is("triangles")) {
			if (!count(file.triangles, size)) return fail("invalid triangle count");
		} else if (is("tri")) {
			if (!indexed(file.triangles.size(), 5, index)) return fail("invalid triangle index");
			file.triangles[index] = {integer(t[2]), integer(t[3]), integer(t[4])};
		} else if (is("gestures")) {
			if (!count(file.gestures, size)) return fail("invalid gesture count");
		} else if (is("gesture")) {
			if (!indexed(file.gestures.size(), 3, gesture_index)) return fail("invalid gesture index");
			file.gestures[gesture_index].name = t[2];
		} else if (is("parameters") || is("parm")) {
			if (gesture_index < 0 || static_cast<size_t>(gesture_index) >= file.gestures.size())
				return fail("parameter without a gesture");
			auto &params = file.gestures[gesture_index].parameters;
			if (is("parameters")) {
				if (!count(params, 32)) return fail("invalid parameter count");
			} else {
				if (!indexed(params.size(), 5, index)) return fail("invalid parameter index");
				params[index] = {{real(t[2]), real(t[3])}, t[4]};
			}
		} else {
			Point *point = is("eyesize") ? &file.eye_size :
					is("eye1center") ? &file.eye_centers[0] :
					is("eye2center") ? &file.eye_centers[1] :
					is("eyelimits") ? &file.eye_limits : nullptr;
			if (point) {
				if (t.size() < 3) return fail("missing eye coordinate");
				*point = {real(t[1]), real(t[2])};
			}
		}
	}
	if (!valid(file, error)) return false;
	out = std::move(file);
	return true;
}

bool write(const File &file, std::vector<uint8_t> &out, std::string &error) {
	error.clear();
	if (!valid(file, error)) return false;
	// [orig: GRM writer @0x588320..0x58869A]. Win32 text-mode fprintf
	// expands each LF to CRLF; format it explicitly for portable output.
	std::ostringstream s;
	s.imbue(std::locale::classic());
	s << std::fixed << std::setprecision(4);
	const auto &t = file.saved;
	s << "; GRM File\r\n; Saved on " << t.month << '/' << t.day << '/' << t.year
	  << ", " << t.hour << ':' << std::setfill('0') << std::setw(2) << t.minute
	  << ':' << std::setw(2) << t.second << std::setfill(' ')
	  << "\r\n; Last edited by " << t.author << "\r\n\r\n"
	  << "basetexture    " << file.base_texture << "\r\n"
	  << "eyetexture     " << file.eye_textures[0] << "    " << file.eye_textures[1]
	  << "\r\n\r\n\r\nbasemesh\r\n  vertices    " << file.vertices.size() << "\r\n";
	for (size_t i = 0; i < file.vertices.size(); ++i) {
		const auto &v = file.vertices[i];
		s << "    vertex  " << std::setw(3) << i << "  " << std::setw(5) << v.uv.x
		  << "  " << std::setw(5) << v.uv.y << "  " << v.group << "\r\n";
	}
	s << "  endvertices\r\n  triangles    " << file.triangles.size() << "\r\n";
	for (size_t i = 0; i < file.triangles.size(); ++i) {
		const auto &v = file.triangles[i];
		s << "    tri     " << std::setw(3) << i << "   " << std::setw(2) << v[0]
		  << "  " << std::setw(2) << v[1] << "  " << std::setw(2) << v[2] << "\r\n";
	}
	s << "  endtriangles\r\nendbasemesh\r\n\r\neyes\r\n";
	auto point = [&](const char *name, Point p) {
		s << name << std::setw(5) << p.x << "  " << std::setw(5) << p.y << "\r\n";
	};
	point("  eyesize       ", file.eye_size);
	point("  eye1center    ", file.eye_centers[0]);
	point("  eye2center    ", file.eye_centers[1]);
	point("  eyelimits     ", file.eye_limits);
	s << "endeyes\r\n\r\ngestures  " << file.gestures.size() << "\r\n";
	for (size_t i = 0; i < file.gestures.size(); ++i) {
		const auto &g = file.gestures[i];
		if (i) s << "\r\n";
		s << "    gesture     " << std::setw(2) << i << "  " << g.name << "\r\n"
		  << "    parameters  " << std::setw(2) << g.parameters.size() << "\r\n";
		for (size_t j = 0; j < g.parameters.size(); ++j) {
			const auto &p = g.parameters[j];
			s << "    parm        " << j << "  " << std::setw(5) << p.offset.x
			  << "  " << std::setw(5) << p.offset.y << "  " << p.group << "\r\n";
		}
	}
	s << "endgestures\r\n";
	const std::string bytes = s.str();
	out.assign(bytes.begin(), bytes.end());
	return true;
}

} // namespace opennova::grm
