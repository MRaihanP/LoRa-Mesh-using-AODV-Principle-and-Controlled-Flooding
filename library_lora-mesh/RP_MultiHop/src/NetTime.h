#pragma once
#include <Arduino.h>

// Satu-satunya API waktu yang boleh dipakai layer jaringan.
// Implementasinya disediakan di luar (RtcTime.cpp atau sumber lain).
uint32_t nowUnixLike();
