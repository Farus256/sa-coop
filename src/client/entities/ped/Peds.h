#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "common/api/protocol/Protocol.h"
#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

struct PedSyncState {
    uint32_t time = 0;
    game::CVector pos, vel;
    float heading = 0;
    uint8_t moveState = 1;
    uint8_t health = 100, armour = 0, weapon = 0;
    uint8_t flags = 0;  // F_*
    uint16_t vehNetId = 0;
    int8_t seat = -1;  // F_IN_VEHICLE: current seat, F_ENTERING: target seat (-1 driver)
    int8_t area = 0;
    game::CVector aim;  // F_AIMING: gun target relative to pos
    uint8_t aimKind = 0;  // F_AIMING: 0 the point above, 1 player aimId, 2 ped netId aimId
    uint16_t aimId = 0;
    // action animation (anything but locomotion): IFP block + index inside it, 0xFFFF = none
    uint16_t animBlock = 0xFFFF, animIdx = 0, animFlags = 0;
    float animTime = 0;

    enum : uint8_t {
        F_DEAD = 1,
        F_DUCKING = 2,
        F_IN_VEHICLE = 4,
        F_AIMING = 8,
        F_FIRING = 16,
        F_ENTERING = 32,  // walking to / opening vehicle vehNetId
        F_LEAVING = 64,
    };
};

// NPC known to the players. owner == my id: simulated here and announced; otherwise a mirror (a record without a ped
// while this game's player is far from it).
struct NetPed {
    uint16_t netId = 0;
    game::CPed* ped = nullptr;  // mirror: nullptr until it could be spawned (model streaming, vehicle missing)
    int handle = -1;
    uint8_t owner = 0;
    uint16_t model = 0;
    uint8_t pedType = 4;
    std::string special;  // model name for special character slots 290-299
    // owner side
    uint64_t lastSendMs = 0;
    int lastClip = -1;  // shots are detected by the clip emptying
    uint8_t lastWeapon = 0;
    uint64_t lastShotMs = 0;
    // mirror side
    bool hasState = false;
    PedSyncState state;
    uint64_t recvMs = 0;
    uint64_t lastTaskMs = 0;
    game::CVector lastTaskTarget;
    uint8_t lastTaskMove = 0;
    bool deadApplied = false;
    uint64_t lastHitMs = 0;  // our player hit it (sent to the owner): its loot falls here if it dies of it
    uint64_t lastRamMs = 0;  // our player's car hit it (sent once per contact)
    uint8_t gunMode = 0;  // 0 none, 1 aiming, 2 shooting
    int gunTargetHandle = -1;  // ped aimed at, -1: gunTarget point
    game::CVector gunTarget;
    uint64_t gunTaskMs = 0;
    uint64_t weaponMs = 0;     // the last weapon change: its own timer, the walk tasks' held a walking mirror's gun back
    uint16_t enterVeh = 0;
    uint64_t enterTaskMs = 0, leaveTaskMs = 0;
    uint16_t animBlock = 0xFFFF, animIdx = 0;  // action animation replayed on the mirror
    uint16_t baseBlock = 0xFFFF, baseIdx = 0;  // the full-body one under it (a fight stance under punches)
    float animTime = 0;                         // owner's time of it in the latest state (restarts go back)
    bool animFull = false;                      // it is not partial: locomotion tasks must wait
};

class Peds : public Module {
public:
    const char* Name() const override { return "Peds"; }
    void OnInstallHooks() override;
    void OnSessionEnd() override;
    void OnPeerLeave(uint8_t id) override;
    void OnGameStarted() override;
    void OnGameShutdown() override;
    void OnPreProcess() override;
    void OnPostProcess() override;

    static Peds& Instance();
    NetPed* Find(game::CPed* ped);
    game::CPed* FindOwned(uint16_t netId);  // our live ped announced under netId
    int HandleOf(uint16_t netId);            // script handle of the live ped (ours or a mirror), -1 if none
    // special character slots (models 290-299) are filled by name at run time
    void RememberSpecialModel(int model, const char* name);
    std::string SpecialName(int model) const;  // what the slot was filled with here, "" if nothing
    // the model here of the special character `name`: its slot, or a free one asked for it, from the top (scripts and
    // cutscenes fill them from 290); -1 while it loads or with no slot free
    int SpecialModelFor(const std::string& name);
    // a script here fills that slot with another character: the copies wearing it are made again elsewhere (the game
    // drops the model from under script people, and a copy is one)
    void RespawnCopiesOf(int model);
    // a player (re)started its game: announce our peds to it again
    void Reannounce(uint8_t to);
    // our ped goes to player `to` (it takes the mirror over as its own) and ours is deleted
    void GiveAway(uint16_t netId, uint8_t to);
    // this game would drop the ped, the other player is nearer: it goes to that player's game (false: not ours to give)
    bool QueueGiveAway(game::CPed* ped, uint8_t to);
    // a kind of ped the other game can take over (a mirror is a civilian there: no cops, medics or firemen)
    static bool CanGiveAwayType(const NetPed& np);

private:
    void ScanOwned();
    void Announce(NetPed& np, proto::PlayerMask to = proto::kAllPlayers);
    void SendState(NetPed& np);
    proto::PlayerMask StateTargets(NetPed& np);
    bool WantMirror(const NetPed& np) const;  // whether this game keeps a copy of another game's ped now
    void Unspawn(NetPed& np);
    void HandleCreate(uint8_t from, Reader& r);
    void HandleRemove(uint8_t from, Reader& r);
    void HandleSync(uint8_t from, Reader& r);
    void HandleOwner(uint8_t from, Reader& r);
    void ApplyMirror(NetPed& np);
    game::CPed* SpawnMirror(NetPed& np);
    void Forget(uint16_t netId, bool deletePed);
    void Adopt(NetPed& np);
    void DropMirrorsOf(uint8_t id, bool keep);  // keep: this game's own people now (it left), else deleted (it restarted)
    PedSyncState Capture(NetPed& np);

    std::map<uint16_t, NetPed> m_byId;
    std::map<game::CPed*, uint16_t> m_byPtr;
    uint16_t m_nextId = 1;
    std::map<int, std::string> m_specialNames;
    std::map<game::CPed*, uint8_t> m_giveAway;  // to hand over after the population code (ped -> player)
    uint64_t m_lastScan = 0, m_lastStatsMs = 0;
    bool m_wasReady = false;
    int m_mirrorShots = 0;  // diagnostics: shots fired by mirrors since the last stats line
};

// the interior a ped is in, by its area or, for the interiors' own people (they keep area 0), by a player near it
int8_t AreaOf(game::CPed* ped);

}  // namespace coop
