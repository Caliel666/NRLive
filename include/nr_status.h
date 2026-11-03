#pragma once
#include <string>

// The DLSSNR runtime's current status line (network build stage, size and GPU
// time, or why NR is not running), for the overlay. Render thread only.
const std::wstring& nrStatusLine();
void setNrStatusLine(const std::wstring& status);
