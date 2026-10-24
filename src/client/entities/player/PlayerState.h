#pragma once
// Serialisable snapshots of a player (shared by the sender and the receiver).
#include <cstdint>

#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"

namespace coop {

// Compressed pad state (CControllerState).
struct PadState {
    int8_t lx = 0, ly = 0, rx = 0, ry = 0;  // sticks, -128..127 (game uses -128..128)
    uint16_t buttons = 0;

    enum : uint16_t {
        B_SQUARE = 1 << 0,    // jump / brake
        B_CROSS = 1 << 1,     // sprint / accelerate
        B_CIRCLE = 1 << 2,    // fire
        B_TRIANGLE = 1 << 3,  // enter / exit vehicle
        B_RS1 = 1 << 4,       // aim / handbrake
        B_LS1 = 1 << 5,
        B_LS2 = 1 << 6,
        B_RS2 = 1 << 7,
        B_SHOCKL = 1 << 8,    // crouch / horn
        B_SHOCKR = 1 << 9,
        B_DUP = 1 << 10,
        B_DDOWN = 1 << 11,
        B_DLEFT = 1 << 12,
        B_DRIGHT = 1 << 13,
        B_WALK = 1 << 14,
        B_SELECT = 1 << 15,
    };

    static PadState From(const game::CControllerState& s);
    void To(game::CControllerState& s) const;
    void Write(Writer& w) const;
    void Read(Reader& r);
    bool operator==(const PadState& o) const {
        return lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry && buttons == o.buttons;
    }
};

// Active camera of the player (needed to replay movement direction and aiming).
struct CameraState {
    uint16_t mode = 0;
    uint16_t weaponMode = 0;
    float fov = 70.0f;
    game::CVector front{0, 1, 0};
    game::CVector sourceOffset{0, -3, 1};  // camera position relative to the ped
    game::CVector up{0, 0, 1};
    float orientation = 0;
    float alpha = 0, beta = 0;
    float lookPitch = 0;
    bool useMouse3rd = true;
    float chairX = 0.53f, chairY = 0.4f;

    void Write(Writer& w) const;
    void Read(Reader& r);
};

struct OnFootState {
    uint32_t time = 0;  // sender clock (ms)
    game::CVector pos, vel;
    float rotation = 0, aimRotation = 0;
    uint8_t health = 100, armour = 0;
    uint8_t weapon = 0;
    uint16_t ammo = 0;
    uint16_t flags = 0;  // F_*
    uint8_t flags2 = 0;  // F2_*
    int8_t area = 0;
    uint8_t moveState = 0;
    uint8_t wanted = 0;  // stars, shown on the name tag
    uint8_t shots = 0;   // the shots its gun has fired, counted round (the others' copies fire as many)
    PadState pad;
    CameraState cam;

    enum : uint16_t {
        F_DUCKING = 1, F_IN_AIR = 2, F_DEAD = 4, F_SWIMMING = 8, F_JETPACK = 16, F_IN_TRAIN = 32, F_PHONE = 64,
        F_POPULATION = 128,  // its game makes the people and traffic around it (not left to a nearby player's game)
        F_TRAINS = 256,      // its game makes random trains (not left to a nearby player's game)
        F_ATTACHED = 512,    // a script stood it on a vehicle (ATTACH_CHAR_TO_CAR): attach* below
        F_DOWNED = 1024,     // wounded, lying where it fell until someone revives it (downSeconds left)
        F_REVIVING = 2048,   // it is reviving a wounded player
        F_ON_VEHICLE = 4096, // it stands on a vehicle (a car's roof, a truck's back): onVeh, onLocal
        F_TASK_WALK = 8192,  // a task walks it, not its pad (a door's walk-in, a script's go-to)
    };
    enum : uint8_t { F2_DOOR = 1 };  // F2_DOOR: going through a door of an entrance (CEntryExitManager::ms_exitEnterState)
    // F_ON_VEHICLE: the vehicle's net id and the place in its own frame (the other games put it on their copy there)
    uint16_t onVeh = 0;
    game::CVector onLocal{};
    // F_ATTACHED: the vehicle's net id, the place on it and the turret's angles (CPed::AttachPedToEntity)
    uint16_t attachVeh = 0;
    game::CVector attachOffset{};
    uint16_t attachA = 0;
    float attachB = 0;
    uint8_t downSeconds = 0;  // F_DOWNED

    void Write(Writer& w) const;
    bool Read(Reader& r);
};

}  // namespace coop
