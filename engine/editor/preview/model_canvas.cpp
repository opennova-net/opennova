#include <editor/preview/model_canvas.h>

#include <algorithm>
#include <cmath>

#include <editor/documents/animation_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_viewport.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// What the canvas shows: the model's document (this instance of it), none for a model an
// animation plays on.
CanvasSubject subject_of(const ModelCanvasFrame &frame) {
	CanvasSubject subject;
	if (frame.document) {
		subject.path = frame.document->path();
		subject.identity = frame.document->identity();
	}
	return subject;
}

bool is_selected(const ModelCanvasFrame &frame, const ModelOverlay &overlay) {
	return frame.selected >= 0 && overlay.kind == frame.selected_kind &&
			overlay.index == frame.selected;
}

// The viewport's camera set to `camera` (a SetViewport of its path).
void set_camera(const ModelCanvasFrame &frame, const OrbitCamera &camera, CanvasRequests &out) {
	if (frame.model)
		out.request(request::set_viewport(frame.model->path(), model_camera_change(camera)));
}

} // namespace

int model_canvas_under(const ModelCanvasFrame &frame, const CanvasInput &in) {
	if (!in.hovered || !frame.model)
		return -1;
	return pick_model_overlay(frame.overlays, frame.model->camera(), in.width, in.height,
			in.mouse.x, in.mouse.y, kModelPickSlop);
}

int model_canvas_bone_under(const ModelCanvasFrame &frame, const CanvasInput &in) {
	if (!in.hovered || !frame.model || frame.joints.empty())
		return -1;
	const OrbitCamera &camera = frame.model->camera();
	int best = -1;
	float best_depth = 0.0f;
	for (size_t i = 0; i < frame.joints.size(); ++i) {
		float x = 0.0f, y = 0.0f, depth = 0.0f;
		if (!camera.project(frame.joints[i].at, in.width, in.height, x, y, &depth)) continue;
		if (std::fabs(x - in.mouse.x) > kModelPickSlop || std::fabs(y - in.mouse.y) > kModelPickSlop) continue;
		if (best < 0 || depth < best_depth) {
			best = int(i);
			best_depth = depth;
		}
	}
	return best;
}

const std::vector<ModelCollisionShape> &model_canvas_collision(const ModelCanvasFrame &frame) {
	static const std::vector<ModelCollisionShape> none;
	return frame.collision ? *frame.collision : none;
}

int model_canvas_collision_under(const ModelCanvasFrame &frame, const CanvasInput &in) {
	const std::vector<ModelCollisionShape> &shapes = model_canvas_collision(frame);
	if (!in.hovered || !frame.model || shapes.empty())
		return -1;
	return pick_model_collision(shapes, frame.model->camera(), in.width, in.height, in.mouse.x, in.mouse.y);
}

NodeAddress model_canvas_bone_record(const ModelCanvasFrame &frame, int joint) {
	const auto *clip = dynamic_cast<const AnimationDocument *>(frame.clip_document);
	const ClipRow *row = clip ? clip->clip() : nullptr;
	if (!row || joint < 0 || size_t(joint) >= frame.joints.size()) return NodeAddress();
	const size_t bone = size_t(frame.joints[size_t(joint)].bone);
	if (bone >= row->collections[0].size()) return NodeAddress();
	return NodeAddress{row->id, node_kind(AnimationKind::Bone), row->collections[0][bone]};
}

ModelGrab model_canvas_grab(const ModelCanvasFrame &frame, const CanvasInput &in, int under) {
	ModelGrab grab;
	grab.pan = in.middle || in.keys.shift;
	grab.pick = under;
	if (grab.pan || !frame.current || frame.selected < 0 || frame.document->blocked() ||
			!frame.editable)
		return grab;
	const OrbitCamera &camera = frame.model->camera();
	for (const ModelOverlay &overlay : frame.overlays) {
		if (!is_selected(frame, overlay))
			continue;
		float hx = 0.0f, hy = 0.0f;
		const auto near_pointer = [&](const PreviewVec3 &point) {
			return camera.project(point, in.width, in.height, hx, hy) &&
					std::fabs(hx - in.mouse.x) <= kModelPickSlop &&
					std::fabs(hy - in.mouse.y) <= kModelPickSlop;
		};
		if (overlay.has_direction && near_pointer(frame.model->axis_tip(overlay)))
			grab.which = ModelHandle::Axis;
		else if (overlay.kind != ModelOverlayKind::Pivot && near_pointer(overlay.at))
			grab.which = ModelHandle::Place;
		else
			break;
		grab.handle = true;
		grab.marker = overlay;
		if (grab.which == ModelHandle::Place)
			grab.others = frame.others;
		grab.offset = CanvasPoint{ hx - in.mouse.x, hy - in.mouse.y };
		break;
	}
	return grab;
}

// --- ModelCanvas -----------------------------------------------------------------------------

void ModelCanvas::follow(
		const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) {
	frame_ = static_cast<const ModelViewport &>(viewport).canvas_frame(context);
	picks_valid_ = false;
	follow(frame_, out);
}

const ModelCanvas::Picks &ModelCanvas::picks_(const CanvasInput &in) const {
	if (picks_valid_ && picks_mouse_.x == in.mouse.x && picks_mouse_.y == in.mouse.y && picks_width_ == in.width &&
	    picks_height_ == in.height && picks_hovered_ == in.hovered)
		return picks_cache_;
	++pick_count_;
	picks_cache_.under = model_canvas_under(frame_, in);
	picks_cache_.bone = picks_cache_.under < 0 ? model_canvas_bone_under(frame_, in) : -1;
	picks_cache_.collision =
			picks_cache_.under < 0 && picks_cache_.bone < 0 ? model_canvas_collision_under(frame_, in) : -1;
	picks_valid_ = true;
	picks_mouse_ = in.mouse;
	picks_width_ = in.width;
	picks_height_ = in.height;
	picks_hovered_ = in.hovered;
	return picks_cache_;
}

void ModelCanvas::input(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	input(frame_, in, picks_(in).under, out);
}

OverlayList ModelCanvas::shapes(const ViewportContext &, const CanvasInput &in) const {
	const Picks &at = picks_(in);
	return shapes(frame_, in, at.under, at.bone, at.collision);
}

std::string ModelCanvas::hover_tip(const ViewportContext &, const CanvasInput &in) const {
	const Picks &at = picks_(in);
	return hover_tip(frame_, at.under, at.bone, at.collision);
}

void ModelCanvas::follow(const ModelCanvasFrame &frame, CanvasRequests &out) {
	gesture_.frame(subject_of(frame), out);
	if (!gesture_.pressed())
		grab_ = ModelGrab();
}

void ModelCanvas::input(
		const ModelCanvasFrame &frame, const CanvasInput &in, int under, CanvasRequests &out) {
	if (!frame.model)
		return;
	const ModelViewport &model = *frame.model;
	// The camera this frame moves to, an orbit or a pan then the wheel's dolly on it: one SetViewport
	// (two would each start from the camera as the frame began, the second undoing the first).
	OrbitCamera camera = model.camera();
	bool camera_moved = false;
	if (in.pressed) {
		gesture_.press(subject_of(frame), in.screen, out);
		grab_ = model_canvas_grab(frame, in, under);
		if (under < 0) grab_.bone = model_canvas_bone_under(frame, in);
		if (under < 0 && grab_.bone < 0) grab_.collision = model_canvas_collision_under(frame, in);
	}
	if (gesture_.pressed() && !in.down) {
		// A click on a marker selects its record (while the picture is the document's).
		if (!gesture_.dragging() && grab_.pick >= 0 && size_t(grab_.pick) < frame.overlays.size() &&
				frame.current) {
			const NodeAddress record = model_overlay_record(
					*frame.document, frame.overlays[size_t(grab_.pick)], model.lod());
			if (record.row)
				out.request(request::select_record(frame.document->path(), record));
		}
		// A click on a joint selects its bone in the clip (S17).
		if (!gesture_.dragging() && grab_.pick < 0 && grab_.bone >= 0 && frame.clip_document) {
			const NodeAddress record = model_canvas_bone_record(frame, grab_.bone);
			if (record.row)
				out.request(request::select_record(frame.clip_document->path(), record));
		}
		// A click on a collision shape selects its record (S17), while the picture is the document's.
		if (!gesture_.dragging() && grab_.pick < 0 && grab_.bone < 0 && grab_.collision >= 0 && frame.current &&
				size_t(grab_.collision) < model_canvas_collision(frame).size()) {
			const NodeAddress record = model_collision_record(*frame.document, model_canvas_collision(frame)[size_t(grab_.collision)]);
			if (record.row)
				out.request(request::select_record(frame.document->path(), record));
		}
		gesture_.release(out);
		grab_ = ModelGrab();
	} else if (gesture_.pressed()) {
		gesture_.move(in.screen);
		if (gesture_.dragging() && grab_.handle && frame.clock) {
			// The handle follows the pointer (kept where the press took it) in the plane that faces
			// the eye; each step is planned from the markers as they were pressed.
			const float snap = in.keys.alt ? 0.0f : frame.snap;
			std::vector<Edit> edits;
			if (model.handle_edits(*frame.document, grab_.marker, grab_.which,
						in.mouse.x + grab_.offset.x, in.mouse.y + grab_.offset.y, in.width,
						in.height, snap, gesture_.token(), *frame.clock, edits, &grab_.others) &&
					!edits.empty()) {
				out.request(request::edit_record(gesture_.path(), std::move(edits)));
				gesture_.sent();
			}
		} else if (gesture_.dragging() && !grab_.handle && (in.delta.x != 0.0f || in.delta.y != 0.0f)) {
			if (grab_.pan)
				camera.pan(in.delta.x, in.delta.y, in.width);
			else
				camera.orbit(in.delta.x, in.delta.y);
			camera_moved = true;
		}
	}
	if (in.hovered && in.wheel != 0.0f) {
		camera.dolly(std::pow(kModelWheelDolly, in.wheel));
		camera_moved = true;
	}
	// The first-person eye (DI-13) stands where the game's camera does: no gesture moves it.
	if (camera_moved && !camera.posed)
		set_camera(frame, camera, out);
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame))
		frame_selected(frame, in.width, in.height, out);
}

void ModelCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	grab_ = ModelGrab();
}

void ModelCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
	if (!gesture_.pressed())
		grab_ = ModelGrab();
}

void ModelCanvas::frame_selected(
		const ModelCanvasFrame &frame, int width, int height, CanvasRequests &out) const {
	if (!frame.model || frame.model->camera().posed)
		return;
	const ModelViewport &model = *frame.model;
	for (const ModelOverlay &overlay : frame.overlays) {
		if (!is_selected(frame, overlay) || !model.model())
			continue;
		set_camera(frame, model.framed_on(overlay, width, height), out);
		return;
	}
	// The selected collision record's shape (S17).
	if (frame.selected_collision >= 0 && size_t(frame.selected_collision) < model_canvas_collision(frame).size()) {
		PreviewVec3 center;
		float radius = 0.0f;
		model_collision_bounds(model_canvas_collision(frame)[size_t(frame.selected_collision)], center, radius);
		OrbitCamera camera = model.camera();
		camera.frame(center, radius, width, height);
		set_camera(frame, camera, out);
		return;
	}
	if (model.model())
		set_camera(frame, model.framed(width, height), out);
}

OverlayList ModelCanvas::shapes(
		const ModelCanvasFrame &frame, const CanvasInput &in, int under, int bone_under, int collision_under) const {
	OverlayList list;
	if (!frame.model)
		return list;
	const ModelViewport &model = *frame.model;
	const OrbitCamera &camera = model.camera();
	const int width = in.width, height = in.height;
	// The collision under everything else (S17): each shape's edges and sphere in its colour; the one
	// under the pointer and the selected one drawn over the rest, filled faintly.
	const auto draw_collision = [&](const ModelCollisionShape &s, OverlayRole role) {
		const bool lit = role != OverlayRole::Normal;
		const float thickness = role == OverlayRole::Selected ? 2.0f : lit ? 1.5f : 1.0f;
		const uint8_t alpha = lit ? 255 : 170;
		if (lit)
			for (size_t t = 0; t + 2 < s.triangles.size(); t += 3) {
				CanvasPoint p[3];
				bool seen = true;
				for (int k = 0; k < 3 && seen; ++k) seen = camera.project(s.triangles[t + size_t(k)], width, height, p[k].x, p[k].y);
				if (seen) list.quad(p[0], p[1], p[2], p[2], s.rgb, 60);
			}
		for (size_t e = 0; e + 1 < s.edges.size(); e += 2) {
			CanvasPoint a, b;
			if (camera.project(s.edges[e], width, height, a.x, a.y) && camera.project(s.edges[e + 1], width, height, b.x, b.y))
				list.line(a, b, s.rgb, thickness, role, alpha);
		}
		float cx = 0.0f, cy = 0.0f, depth = 0.0f;
		if (s.sphere && s.radius > 0.0f && camera.project(s.center, width, height, cx, cy, &depth) && depth > 0.0f) {
			const float reach = s.radius * camera.focal(width) / depth;
			if (reach > 1.0f && reach < 4.0f * float(width)) list.ring(CanvasPoint{cx, cy}, reach, role, thickness, s.rgb, alpha);
		}
	};
	for (size_t i = 0; i < model_canvas_collision(frame).size(); ++i)
		if (int(i) != frame.selected_collision && int(i) != collision_under) draw_collision(model_canvas_collision(frame)[i], OverlayRole::Normal);
	if (collision_under >= 0 && size_t(collision_under) < model_canvas_collision(frame).size() && collision_under != frame.selected_collision)
		draw_collision(model_canvas_collision(frame)[size_t(collision_under)], OverlayRole::Hover);
	if (frame.selected_collision >= 0 && size_t(frame.selected_collision) < model_canvas_collision(frame).size())
		draw_collision(model_canvas_collision(frame)[size_t(frame.selected_collision)], OverlayRole::Selected);
	// The legend: a swatch and its words for each colour drawn, from the picture's lower left corner up.
	const std::vector<ModelCollisionLegendRow> legend = model_collision_legend(model_canvas_collision(frame));
	const float row_height = 16.0f;
	for (size_t i = 0; i < legend.size(); ++i) {
		const float y = float(height) - 10.0f - row_height * float(legend.size() - i);
		list.quad(CanvasPoint{8.0f, y + 2.0f}, CanvasPoint{18.0f, y + 2.0f}, CanvasPoint{18.0f, y + 12.0f},
				CanvasPoint{8.0f, y + 12.0f}, legend[i].rgb);
		list.text(CanvasPoint{22.0f, y}, legend[i].words, 0xE6E6E6);
	}
	// A clip's bones under the markers: a line from each joint to its parent's, a dot at each, the
	// one under the pointer and the selected one ringed.
	std::vector<CanvasPoint> joints(frame.joints.size());
	std::vector<bool> shown(frame.joints.size(), false);
	for (size_t i = 0; i < frame.joints.size(); ++i)
		shown[i] = camera.project(frame.joints[i].at, width, height, joints[i].x, joints[i].y);
	for (size_t i = 0; i < frame.joints.size(); ++i) {
		const int parent = frame.joints[i].parent;
		if (shown[i] && parent >= 0 && size_t(parent) < joints.size() && shown[size_t(parent)])
			list.line(joints[size_t(parent)], joints[i], kBoneRgb, 1.5f);
	}
	for (size_t i = 0; i < frame.joints.size(); ++i) {
		if (!shown[i]) continue;
		list.marker(joints[i], OverlayGlyph::Dot, 3.0f, OverlayRole::Normal, kBoneRgb);
		if (int(i) == bone_under) list.ring(joints[i], 7.0f, OverlayRole::Hover, 1.5f);
		if (frame.joints[i].bone == frame.selected_bone) list.ring(joints[i], 8.0f, OverlayRole::Selected, 2.0f);
	}
	for (size_t i = 0; i < frame.overlays.size(); ++i) {
		const ModelOverlay &overlay = frame.overlays[i];
		float x = 0.0f, y = 0.0f, depth = 0.0f;
		if (!camera.project(overlay.at, width, height, x, y, &depth))
			continue;
		const CanvasPoint at{ x, y };
		const auto line_to = [&](const PreviewVec3 &tip, uint32_t rgb) {
			float tx = 0.0f, ty = 0.0f;
			if (camera.project(tip, width, height, tx, ty))
				list.line(at, CanvasPoint{ tx, ty }, rgb, 1.5f);
		};
		switch (overlay.kind) {
			case ModelOverlayKind::UserPoint:
				if (overlay.has_direction)
					line_to(model.axis_tip(overlay), kUserPointRgb);
				list.quad(CanvasPoint{ at.x, at.y - 4.0f }, CanvasPoint{ at.x + 4.0f, at.y },
						CanvasPoint{ at.x, at.y + 4.0f }, CanvasPoint{ at.x - 4.0f, at.y },
						kUserPointRgb);
				break;
			case ModelOverlayKind::Light: {
				const uint32_t rgb = overlay.color & 0xFFFFFFu;
				if (overlay.radius > 0.0f && depth > 0.0f) {
					const float reach = overlay.radius * camera.focal(width) / depth;
					if (reach > 2.0f && reach < 4.0f * float(width))
						list.ring(at, reach, OverlayRole::Normal, 1.0f, rgb, 0x60);
				}
				if (overlay.has_direction)
					line_to(model.axis_tip(overlay), rgb);
				list.marker(at, OverlayGlyph::Dot, 4.5f, OverlayRole::Normal, rgb);
				break;
			}
			case ModelOverlayKind::Pivot:
				list.marker(at, OverlayGlyph::Cross, 5.0f, OverlayRole::Normal, kPivotRgb, 1.5f);
				break;
		}
		if (int(i) == under)
			list.ring(at, 8.0f, OverlayRole::Hover, 1.5f);
		if (is_selected(frame, overlay)) {
			list.ring(at, 9.0f, OverlayRole::Selected, 2.0f);
			// The selected marker's axis tip: the handle that turns it.
			float tx = 0.0f, ty = 0.0f;
			if (overlay.has_direction &&
					camera.project(model.axis_tip(overlay), width, height, tx, ty))
				list.disc(CanvasPoint{ tx, ty }, 4.0f, OverlayRole::Selected);
		} else {
			// Another selected record's marker, ringed thin: a drag of the primary's place moves it.
			for (const ModelOverlay &other : frame.others)
				if (other.kind == overlay.kind && other.index == overlay.index)
					list.ring(at, 9.0f, OverlayRole::Selected, 1.0f);
		}
	}
	return list;
}

std::string ModelCanvas::hover_tip(const ModelCanvasFrame &frame, int under, int bone_under, int collision_under) const {
	if (gesture_.dragging())
		return std::string();
	// A collision shape by its words, and what a click does where the picture is the document's (S17).
	if (under < 0 && bone_under < 0 && collision_under >= 0 && size_t(collision_under) < model_canvas_collision(frame).size()) {
		const ModelCollisionShape &shape = model_canvas_collision(frame)[size_t(collision_under)];
		return shape.name + (shape.pickable && frame.current ? " (click to select it)" : std::string());
	}
	if (under >= 0 && size_t(under) < frame.overlays.size()) {
		// A user point by its name and what the game reads it as (documents/model_labels.h).
		const ModelOverlay &overlay = frame.overlays[size_t(under)];
		const std::string role =
		    overlay.kind == ModelOverlayKind::UserPoint ? model_user_point_role(overlay.name) : std::string();
		return role.empty() ? overlay.name : overlay.name + ": " + role;
	}
	if (bone_under < 0 || size_t(bone_under) >= frame.joints.size())
		return std::string();
	// A joint by its bone's name, and what a click does where the clip is open.
	const PreviewJoint &joint = frame.joints[size_t(bone_under)];
	return joint.name + (model_canvas_bone_record(frame, bone_under).row ? " (click to select it)" : std::string());
}

} // namespace opennova::editor
