#pragma once
#include <enet/enet.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/api/nat-traversal/NatTraversal.h"
#include "shared/api/turn-relay/TurnRelay.h"

namespace coop::server {

// Internet play without VPN: UPnP port mapping, the public endpoint learned with STUN through the ENet
// socket itself, an invite code, and UDP hole punching towards joiners who announce their endpoint on
// the signalling channel. One instance per process (ENet's intercept callback has no user data).
class Internet {
public:
    ~Internet() { Stop(); }
    // returns at once, the work runs on a thread; turnSpec: optional relay (see TurnRelay); key: the invite code's
    void Start(ENetHost* host, uint16_t port, const std::string& turnSpec, uint32_t key);
    void Stop();
    void Service();                              // network thread: punch packets
    std::string Code();                          // empty until known
    bool TakeNewCode(std::string& code);         // the code became known since the last call

private:
    void Worker();
    void QueuePunch(const std::string& message);
    static int ENET_CALLBACK Intercept(ENetHost* host, ENetEvent* event);

    ENetHost* m_host = nullptr;
    uint16_t m_port = 0;
    uint32_t m_key = 0;
    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    nat::UpnpResult m_upnp;
    nat::SignalSubscriber m_signal;
    TurnRelay m_turn;
    bool m_turnEnabled = false;

    std::mutex m_mutex;
    std::string m_code;
    bool m_codeNew = false;
    uint8_t m_txid[12] = {};
    bool m_stunWaiting = false;
    nat::Endpoint m_mapped;
    struct Punch {
        ENetAddress addr;
        int left;
        uint64_t nextMs;
    };
    std::vector<Punch> m_punches;
};

}  // namespace coop::server
