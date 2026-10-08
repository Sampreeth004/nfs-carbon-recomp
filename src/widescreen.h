#pragma once

#include <cstdint>

namespace nfscarbon {
// Called on the UI thread, before the guest starts. Render targets remain at
// retail sizes; the camera and HUD compensate for the display's wider aspect.
void ConfigureWidescreen(uint8_t* base, uint32_t width, uint32_t height);
}
