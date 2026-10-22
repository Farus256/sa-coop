#pragma once
#include <enet/enet.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "common/api/nat-traversal/NatTraversal.h"

namespace coop::server {

// What the host tells its friends to connect to, and whether that can work.
struct AddressInfo {
    std::string external;   // "1.2.3.4:7788", the address the internet sees; empty when unknown
    std::string lan;        // "192.168.1.5:7788", for players in the same network
    bool done = false;      // the checks are over
    bool upnp = false;      // the router forwards the port (UPnP)
    std::string upnpError;  // why it does not
    bool cgnat = false;     // the router itself sits behind the provider's NAT: not reachable from the internet
};

// Internet play by IP: UPnP port mapping and the public address learned with STUN through the ENet socket itself.
// It also answers the clients' pings (kPingMagic) on the game port. One instance per process (ENet's intercept callback
// has no user data).
class Internet {
public:
    ~Internet() { Stop(); }
    // returns at once; the UPnP and STUN work runs on a thread when `internet` is set
    void Start(ENetHost* host, uint16_t port, bool internet);
    void Stop();
    bool TakeNewInfo(AddressInfo& out);  // the info changed since the last call
    AddressInfo Info();

private:
    void Worker();
    void Publish(const AddressInfo& info);
    static int ENET_CALLBACK Intercept(ENetHost* host, ENetEvent* event);

    ENetHost* m_host = nullptr;
    uint16_t m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    nat::UpnpResult m_upnp;

    std::mutex m_mutex;
    AddressInfo m_info;
    bool m_infoNew = false;
    uint8_t m_txid[12] = {};
    bool m_stunWaiting = false;
    nat::Endpoint m_mapped;
};

}  // namespace coop::server
