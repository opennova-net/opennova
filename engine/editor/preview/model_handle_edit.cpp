#include <editor/preview/model_handle_edit.h>

#include <cmath>
#include <cstring>
#include <variant>

#include <editor/documents/model_document.h>
#include <formats/threedi/threedi_build.h>

namespace opennova::editor {

namespace {

using threedi::ThreediBuildVec3;
using threedi::ThreediMatrix4x4;

// The inverse of a row-vector affine matrix's 3x3 part (false when it is singular).
bool invert3(const ThreediMatrix4x4 &m, double out[9]) {
	const double a = m.m[0], b = m.m[1], c = m.m[2];
	const double d = m.m[4], e = m.m[5], f = m.m[6];
	const double g = m.m[8], h = m.m[9], i = m.m[10];
	const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::fabs(det) < 1e-12) return false;
	const double k = 1.0 / det;
	out[0] = (e * i - f * h) * k;
	out[1] = (c * h - b * i) * k;
	out[2] = (b * f - c * e) * k;
	out[3] = (f * g - d * i) * k;
	out[4] = (a * i - c * g) * k;
	out[5] = (c * d - a * f) * k;
	out[6] = (d * h - e * g) * k;
	out[7] = (b * g - a * h) * k;
	out[8] = (a * e - b * d) * k;
	return true;
}

// v * R^-1 (row vectors).
ThreediBuildVec3 apply(const double r[9], const double v[3]) {
	return ThreediBuildVec3{v[0] * r[0] + v[1] * r[3] + v[2] * r[6], v[0] * r[1] + v[1] * r[4] + v[2] * r[7],
	                        v[0] * r[2] + v[1] * r[5] + v[2] * r[8]};
}

double snapped(double value, float snap) {
	return snap > 0.0f ? std::round(value / double(snap)) * double(snap) : value;
}

Edit set(const NodeAddress &record, const char *field, double value, uint64_t gesture) {
	Edit edit;
	edit.address = record;
	edit.field = field;
	edit.value = value;
	edit.gesture = gesture;
	return edit;
}

// Whether `value` leaves the record's `field` as the record holds it: a user point's 16.16 word the
// same to within half its step (a point of the picture taken back to the model loses a little of
// it, which a write would turn into a step of the word), a light's float the same.
bool held(const ModelDocument &document, const NodeAddress &record, const char *field, ModelOverlayKind kind,
		double value) {
	Value current;
	if (!document.get(record, field, current) || !std::holds_alternative<double>(current)) return false;
	const double now = std::get<double>(current);
	if (kind == ModelOverlayKind::UserPoint) return std::llround(value * 65536.0) == std::llround(now * 65536.0);
	return float(value) == float(now);
}

// A Set of `field` to `value` where it changes what the record holds.
void set_changed(const ModelDocument &document, const NodeAddress &record, const char *field,
		ModelOverlayKind kind, double value, uint64_t gesture, std::vector<Edit> &out) {
	if (!held(document, record, field, kind, value)) out.push_back(set(record, field, value, gesture));
}

} // namespace

bool model_handle_from_token(const char *token, ModelHandle &out) {
	if (std::strcmp(token, "place") == 0) out = ModelHandle::Place;
	else if (std::strcmp(token, "axis") == 0) out = ModelHandle::Axis;
	else return false;
	return true;
}

bool model_handle_edits(const ModelDocument &document, const threedi::Threedi3di3 &model, const ModelOverlay &overlay,
                        int lod, uint32_t time_ms, const int32_t bus[96], ModelHandle handle, const PreviewVec3 &to,
                        float snap, uint64_t gesture, std::vector<Edit> &out) {
	out.clear();
	if (overlay.kind == ModelOverlayKind::Pivot) return false;
	if (handle == ModelHandle::Axis && !overlay.has_direction) return false;
	const NodeAddress record = model_overlay_record(document, overlay, lod);
	if (!record.row) return false;
	const ThreediMatrix4x4 pose = model_overlay_pose(model, overlay, lod, time_ms, bus);
	double inverse[9];
	if (!invert3(pose, inverse)) return false;
	if (handle == ModelHandle::Place) {
		// The preview's point in the model's axes (x mirrored back), the part's pose undone.
		const double posed[3] = {-double(to.x) - pose.m[12], double(to.y) - pose.m[13], double(to.z) - pose.m[14]};
		const ThreediBuildVec3 file = threedi::threedi_build_to_mission(apply(inverse, posed));
		set_changed(document, record, "position.x", overlay.kind, snapped(file.x, snap), gesture, out);
		set_changed(document, record, "position.y", overlay.kind, snapped(file.y, snap), gesture, out);
		set_changed(document, record, "position.z", overlay.kind, snapped(file.z, snap), gesture, out);
		return true;
	}
	const double dx = double(to.x) - overlay.at.x, dy = double(to.y) - overlay.at.y, dz = double(to.z) - overlay.at.z;
	const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (!(length > 1e-6)) return false;
	const double turned[3] = {-dx / length, dy / length, dz / length};
	const ThreediBuildVec3 axis = threedi::threedi_build_to_mission(apply(inverse, turned));
	const double norm = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
	if (!(norm > 1e-6)) return false;
	set_changed(document, record, "direction.x", overlay.kind, axis.x / norm, gesture, out);
	set_changed(document, record, "direction.y", overlay.kind, axis.y / norm, gesture, out);
	set_changed(document, record, "direction.z", overlay.kind, axis.z / norm, gesture, out);
	return true;
}

} // namespace opennova::editor
