// imgui_user_config.h — included by imgui.h everywhere (IMGUI_USER_CONFIG, CMakeLists.txt), so ImGui's own files and
// cr-srt's agree. An ImGui assert logs (overlay/renderer.cpp) instead of calling abort() inside the game.
#pragma once

void crsrt_imgui_assert(const char *expr, const char *file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : crsrt_imgui_assert(#_EXPR, __FILE__, __LINE__))
