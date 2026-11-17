// Turns Dear ImGui's draw lists into plain textured triangles that need no scissor test: every triangle is clipped to its command's
// clip rectangle here, on the CPU. The tablet is drawn into places where a scissor box is awkward (the aircraft's 3-D panel texture,
// VR and popped-out windows), so the clipping is done before the triangles reach OpenGL. The same output feeds the off-line
// rasterizer (tests/ui_shot.cpp).
#pragma once

#include <vector>

#include "imgui.h"

namespace xpt {

struct Batch {
    ImTextureID tex = 0;
    std::vector<ImDrawVert> v;   // triangle list (3 per triangle), in ImGui coordinates (y down)
};

void clipDrawData(const ImDrawData* dd, std::vector<Batch>& out);
void scaleColors(std::vector<Batch>& batches, float k);   // RGB times k (0..1), alpha kept: the tablet's brightness

} // namespace xpt
