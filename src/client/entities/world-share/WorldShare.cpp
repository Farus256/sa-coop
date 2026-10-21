#include "entities/world-share/WorldShare.h"

#include "shared/api/net-session/Session.h"

namespace coop {

WorldShare& Share() {
    static WorldShare s;
    return s;
}

bool MakesBefore(uint8_t other) {
    uint8_t host = Net().HostId(), me = Net().MyId();
    return other == host || (me != host && other < me);
}

}  // namespace coop
