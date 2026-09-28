// ui.h — what the overlay shows. Everything here is text on ImGui's foreground draw list, not ImGui windows:
// windows would need input, and no platform backend is linked yet.
#pragma once

namespace crsrt::ui {

// The text size for a back buffer this tall: 26 px at 1440, never under 16.
float font_px(float height);

// One frame's contents, between ImGui::NewFrame and ImGui::Render. `frame` is the overlay's frame count, for the log.
void draw(float width, float height, float font_px, long frame);

}  // namespace crsrt::ui
