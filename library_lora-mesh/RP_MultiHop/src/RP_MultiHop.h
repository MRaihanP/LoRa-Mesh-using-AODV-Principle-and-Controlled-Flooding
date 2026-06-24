#pragma once
#include <Arduino.h>

// Core base & hardware mapping
#include "Pins.h"
#include "Base.h"

// ID hash 24-bit
#include "ID.h"

// Time abstraction (RTC diimplementasikan di luar)
#include "NetTime.h"

// Network config & runtime config (topologi, slot, dll)
#include "NetConfig.h"
#include "NetRuntimeConfig.h"

// Static route & slot fallback
#include "StaticRoute.h"
#include "SlotPlan.h"
#include "FramePlan.h"

// Packet layout & builder
#include "Packet.h"

// Per-role device
#include "Node.h"
#include "Relay.h"
#include "Sink.h"
