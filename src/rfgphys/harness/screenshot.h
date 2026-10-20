#pragma once
#include <string>

// Screenshots of the game's back buffer for unattended runs (Harness.ScreenshotFrames / ScreenshotEvery): the next
// presented frame after a request is copied, scaled down to at most 1280 pixels wide and written as a PNG.
namespace screenshot {

// Hooks keen::graphics::endFrame (captures right before it presents). Call before hooks::enable_all.
bool install();
// Any thread; the render thread writes the file at its next Present.
void request(const std::wstring& png_path);

}  // namespace screenshot
