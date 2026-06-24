#include "pins_and_config.h"
#include "RP_MultiHop.h"   // untuk definisi NetRuntime::* dan StaticRoute::ID_*

using PinsAndConfig::NetMultiHop;

// 1) Daftar device (ID & alias & role)
const NetMultiHop::DeviceEntry NetMultiHop::DEVICES[] = {
  { StaticRoute::ID_S,  "S",  NetRuntime::DeviceRole::SINK  },
  { StaticRoute::ID_R1, "R1", NetRuntime::DeviceRole::RELAY },
  { StaticRoute::ID_R2, "R2", NetRuntime::DeviceRole::RELAY },
  { StaticRoute::ID_N1, "N1", NetRuntime::DeviceRole::NODE  },
  { StaticRoute::ID_N2, "N2", NetRuntime::DeviceRole::NODE  },
  { StaticRoute::ID_N3, "N3", NetRuntime::DeviceRole::NODE  },
};

// 2) Pohon static: child -> parent
const NetMultiHop::StaticRouteEntry NetMultiHop::ROUTES[] = {
  { StaticRoute::ID_S,   0                 },
  { StaticRoute::ID_R1,  StaticRoute::ID_S },
  { StaticRoute::ID_R2,  StaticRoute::ID_R1},
  { StaticRoute::ID_N1,  StaticRoute::ID_R2},
  { StaticRoute::ID_N2,  StaticRoute::ID_N1},
  { StaticRoute::ID_N3,  StaticRoute::ID_N2},
};

// 3) Data slot (index 1..5)
const NetMultiHop::DataSlot NetMultiHop::DATA_SLOTS[] = {
  // slotIndex, ownerId,          startSec, durationSec
  { 1, StaticRoute::ID_R1,  0,  4 },
  { 2, StaticRoute::ID_R2,  4, 10 },
  { 3, StaticRoute::ID_N1, 10, 20 },
  { 4, StaticRoute::ID_N2, 20, 30 },
  { 5, StaticRoute::ID_N3, 30, 40 },
};

// 4) HELLO slot (urutan sama dengan HELLO_IDS lama)
const NetMultiHop::HelloSlot NetMultiHop::HELLO_SLOTS[] = {
  // id,                     slotIndex (0..5)
  { StaticRoute::ID_S,  5 },
  { StaticRoute::ID_R1, 4 },
  { StaticRoute::ID_R2, 3 },
  { StaticRoute::ID_N1, 2 },
  { StaticRoute::ID_N2, 1 },
  { StaticRoute::ID_N3, 0 },
};

// 5) Frame & routing config
const NetMultiHop::FrameConfig NetMultiHop::FRAME = {
  40,  // dataLenSec
  2,   // legacy
  2,   // helloSlotSec
  20   // helloLenSec
};

const NetMultiHop::RoutingConfig NetMultiHop::ROUTING = {
  true,   // dynamicRoutingEnabled
  -101,   // rssiMinParent
  -110,   // rssiBadParent
  2       // rssiSwitchMarginDb
};

// 6) NetworkConfig global (dipakai langsung di NetRuntime::applyConfig)
const NetMultiHop::NetworkConfig NetMultiHop::NET = {
  NetMultiHop::DEVICES,
  sizeof(NetMultiHop::DEVICES) / sizeof(NetMultiHop::DEVICES[0]),
  NetMultiHop::ROUTES,
  sizeof(NetMultiHop::ROUTES) / sizeof(NetMultiHop::ROUTES[0]),
  NetMultiHop::DATA_SLOTS,
  sizeof(NetMultiHop::DATA_SLOTS) / sizeof(NetMultiHop::DATA_SLOTS[0]),
  NetMultiHop::HELLO_SLOTS,
  sizeof(NetMultiHop::HELLO_SLOTS) / sizeof(NetMultiHop::HELLO_SLOTS[0]),
  NetMultiHop::FRAME,
  NetMultiHop::ROUTING,
  5   // lastDataSlotId
};
