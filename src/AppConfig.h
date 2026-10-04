#pragma once

// Shared constants for the Arcade Test app.
//
// The arcade display is portrait. The sample draws directly in portrait
// coordinates instead of rotating a landscape UI at runtime.
namespace AppConfig {

constexpr int kLogicalWidth = 720;
constexpr int kLogicalHeight = 1280;

// The firmware framebuffer is landscape while the arcade display is mounted in
// portrait. Render into a portrait canvas, then rotate that canvas when
// presenting it to the framebuffer.
constexpr int kFramebufferWidth = kLogicalHeight;
constexpr int kFramebufferHeight = kLogicalWidth;
constexpr double kFirmwareRotationDegrees = 90.0;

} // namespace AppConfig
