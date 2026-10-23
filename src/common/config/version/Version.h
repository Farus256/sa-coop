#pragma once
#include <cstdint>

#define COOP_NAME "SA:COOP"
#define COOP_VERSION_STR "0.3.5"

namespace coop {
// Bumped on any incompatible change of the network protocol. The multi-player fork counts from 100: a game or a server
// of the original two-player mod (protocol 17 and earlier) is told that the versions differ instead of joining.
constexpr uint32_t kProtocolVersion = 105;
constexpr uint16_t kDefaultPort = 7788;
// Size of gta_sa.exe 1.0 US (HOODLUM) — the only supported executable.
constexpr uint32_t kSupportedExeSize = 14383616;
}  // namespace coop
