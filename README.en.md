<p align="center">
  <img src="docs/images/logo.png" alt="SA:COOP" width="440">
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT license"></a>
  <img src="https://img.shields.io/badge/GTA_San_Andreas-1.0_US-orange" alt="GTA San Andreas 1.0 US">
  <img src="https://img.shields.io/badge/players-2–16-green" alt="2–16 players">
</p>

<p align="center"><a href="README.md">Русский</a> · <b>English</b></p>

SA:COOP is a mod for GTA San Andreas that lets two to sixteen players go through the story together over the
network. One player is the host: the game runs on their save, and they start the missions. The others connect to
them and play in the same world as characters of their own, with their own money, skills and look.

> This is a fork of [SA:COOP](https://github.com/Karimov-Akbar/sa-coop) by Karimov-Akbar (MIT license). The original
> is made for two players, the fork for 2–16. See [What's new in the fork](#whats-new-in-the-fork-030).

## What's new in the fork (0.3.0)

- **Up to 16 players.** The host picks the number of places (2–16, 4 by default) on the "Host a game" tab of the F9
  window, or with `max_players` in `coop\server.ini`.
- **Traffic by distance.** Players near each other (within 350 m) get each other's state 30 times a second, distant
  ones twice a second. Another game's cars and pedestrians appear for a player only near them (cars within 190 m,
  pedestrians within 135 m); distant ones stay records and do not fill the game's pools.
- **The invite code is a ticket.** The 16-character code `XXXX-XXXX-XXXX-XXXX` holds a random server key besides the
  address. You join with the code or with the password. After 8 wrong codes a minute the server blocks that address
  for a minute. Old 12-character codes still parse.
- **Missions.** A marker waits only for the players within 200 m of it; distant ones catch up later. The host can
  start without the missing ones with `/go`. The marker shows whom it waits for.
- **Cutscenes.** Up to three partners stand next to CJ, each in a spot of their own; the others watch on their screen.
- **Player colours** for the name over the head, in the chat, in the F2 list and on the radar.
- **Commands:** `/tp <nick>`, `/tphere <nick>`, `/tphere all` (host), `/go` (host), `/mute <nick>`, `/unmute <nick>`.
- **The microphone** opens only while B is held and closes after 30 s of silence (unless voice is always on).
- **A shared world per group.** Where players are together, one game makes the pedestrians, traffic and trains: the
  host's, or the lowest-numbered player's. Groups in different parts of the map each get their own.
- **Linux and macOS builds** (`LLVM_MINGW`) and server tests (`python tools/build.py servertest`).

Version 0.3.0 does not connect to the original 0.2.x: every player needs to update.

**Status:** the server and the protocol pass automated tests with 16 bots (274 checks). Behaviour in the game with
three or more players has not been tried in practice yet — please report bugs with `coop\coop.log`.

![The host and the partner on Grove Street](docs/images/en/street-host.jpg)

## What the mod does

### The story together

The host starts a mission, but it only begins once every partner nearby stands in the marker (or the host types
`/go`). The partner sees the same
texts, subtitles, radar blips, markers, race checkpoints and timers, and the texts come in the language of the
partner's own game. The partner also hears the lines characters say during missions outside cutscenes. Weapons
the mission gives CJ go to the partner as well. Both players get the mission reward. Enemies attack both of them,
and either player can hurt them.

| The host stands in the marker | The partner sees that the host is waiting |
|---|---|
| ![The host waits for the partner in the marker](docs/images/en/marker-host.jpg) | ![The partner is told that the host waits at the marker](docs/images/en/marker-partner.jpg) |

### Cutscenes

Cutscenes play for everyone at the same time, and the partners' characters (up to three) stand next to CJ in them. The
character listens to the conversation, shifts their weight, nods and looks at whoever is talking, and when CJ
walks, keeps a little behind him and to the side. These movements come from the animations of the cutscenes
themselves. A cutscene can only be skipped by everyone: one presses Enter, and the others agree with Enter or refuse
with Esc.

| | |
|---|---|
| ![Cutscene in the garage: CJ on the right, the partner's character behind him](docs/images/cutscene-garage.jpg) | ![Cutscene in the yard: the partner's character behind CJ and Ryder](docs/images/cutscene-yard.jpg) |
| ![Cutscene at the party: the partner's character by the wall on the left](docs/images/cutscene-party.jpg) | ![Cutscene with Emmet: the partner's character next to CJ](docs/images/cutscene-gun.jpg) |

### A shared world

Both players have the same pedestrians, people inside buildings, cars and police, the same time of day and the
same weather. Pedestrians chat and swear the same way for both, and police roadblocks with their barriers are seen
by both. When one player drives away, the pedestrians and cars near the other one move over to the other
player's game instead of vanishing. Knocked-down lamp posts and signs, broken glass and crates, fire, gang
territories, the building doors a mission opens and the doors of Pay 'n' Spray and mod shops look the same for
both. Horseshoes, oysters, snapshots and tags count toward the shared world.

| The host's screen | The partner's screen |
|---|---|
| ![A Ballas member fights the partner, on the host's screen](docs/images/world-host.jpg) | ![The same fight on the partner's screen](docs/images/world-partner.jpg) |

### Cars

Each player drives on their own or rides as a passenger with the other. A passenger can shoot from the window: the
right mouse button leans out with the gun, the left one fires. Tuning and damage are visible to the other player.
Plane landing gear and nozzles, tank and fire truck turrets, hydraulics, the horn, the police helicopter's
searchlight, trailers, the tow truck and the airport tug all work. Explosions, rockets, grenades, tank shells,
fire truck water, NPC drive-bys, trains and RC toys are seen by both players. Either player can leave a car in the
garage at a safe house, and the host's world keeps it.

![The partner rides as a passenger in the host's car](docs/images/car.jpg)

### Your own progress

Food and the gym, weapon and driving skills, clothes, haircuts, tattoos, money and side job rewards are separate
for each player. What a player does at a diner, the gym, a barber's or a tattoo parlour, the other player sees as
well, along with a tray of food, a cigarette, a phone at the ear and an open parachute. If a player changes their character model, for example with a skin selector, the other player sees the new
model too. The friend comes into the host's world as their own CJ, in the clothes and with the tattoos their CJ
has in their own game. If the friend joined from the main menu, the look comes from their latest save. When the
host saves the game, the friend's progress is saved on the host's side as well.

| The host sees the partner | The partner sees the host |
|---|---|
| ![The partner in their own clothes on the host's screen](docs/images/look-host.jpg) | ![The host on the partner's screen](docs/images/look-partner.jpg) |

### Friendly fire

Before creating the game, the host can allow friendly fire with a checkbox in the F9 window. Then the players can
hurt and kill each other with bullets, fists, explosions, fire and cars. Without the checkbox they cannot harm
each other.

| | |
|---|---|
| ![The host swings a baseball bat at the partner](docs/images/friendly-fire-1.jpg) | ![The partner is knocked down, and the host stands over them with the bat](docs/images/friendly-fire-2.jpg) |

### Russian and English

The SA:COOP window, the chat and the hints come in Russian and English. The language switch is at the top of the
F9 window, and each player picks their own: the host can play in Russian while the partner plays in English.

| The host's screen | The partner's screen |
|---|---|
| ![The player list and the server's reply in Russian on the host's screen](docs/images/lang-host.jpg) | ![The same moment on the partner's screen, all in English](docs/images/lang-partner.jpg) |

### Everything else

Side jobs (taxi, paramedic, firefighter, vigilante, pimp), races, schools, the shooting range, trucking and the
quarry are played by each player in their own game. There is text chat and voice chat, and the farther away the
partner is, the quieter they sound. A waypoint one player sets on the map shows up on the other player's radar,
and a house one player buys appears for both. Above the partner's head you see their health, armour, weapon and
wanted stars. Only the host starts turf wars, but both players' kills of rival gang members provoke them. Cheats
for the weather, traffic, pedestrians and game speed work for both players, while weapon, money and health cheats
only work for the player who typed them.

![Chat, voice and the player list on the partner's screen](docs/images/en/chat-voice.jpg)

## Installation

You need GTA San Andreas version 1.0 US, either clean or a modded build. If your game comes from Steam or the
Rockstar Games Launcher, downgrade it to 1.0 US first, for example with GTA SA Downgrader.

1. Download `SA-COOP.zip` from the [Releases](../../releases) page and unpack it into the game folder so that
   `coop.asi`, `install.bat` and the `coop` folder end up next to `gta_sa.exe`.
2. Run `install.bat`. In a clean game it installs an ASI plugin loader, and in a build that already has one it
   changes nothing.
3. Start the game. The SA:COOP window opens in the main menu, and later you can bring it up with F9. Enter your
   nickname. To switch the window to English, click EN at the top of it.

The archive also contains a detailed player guide in Russian, `Инструкция.txt`.

## How to play

The host presses F9, opens the "Host a game" tab, picks a save and clicks "Start the server". If the host is
already in the game, the server is created from the current game. The "Friendly fire" checkbox above the button
lets the players hurt each other, and "Players: up to N" sets the places (2–16). A couple of seconds later the
window shows a code like `XXXX-XXXX-XXXX-XXXX`, which the host sends to the friends. The code contains the host's
external IP address and the server's key, so only send it to the people you play with: it is the pass to the server.

The friends open the "Join a game" tab, paste the code and click "Connect". After that, the host's world loads on
their side.

![The SA:COOP window in the main menu](docs/images/en/menu.jpg)

You can also start the story from the very beginning: the last item in the save list is "New game". The host
plays the prologue alone, while the friend connects right away and waits. When CJ gets on the bike, the friend
appears next to him, and there is a bike for the friend too.

You don't need Hamachi or Radmin. The server opens its port through UPnP or punches through NAT. If neither works
with your internet providers, the host can turn on a relay with the `turn` line in `coop\server.ini`, as the
guide describes.

## Keys

| Key | What it does |
|---|---|
| F9 | the SA:COOP window |
| T | chat: Enter sends a message, Esc closes the chat |
| B | voice chat while the key is held |
| G | get into the nearest car as a passenger |
| Right mouse button | as a passenger: lean out of the window with the gun, fire with the left button |
| F2 | the player list |
| Enter / Esc | in a cutscene: offer to skip it, agree / refuse |

Chat commands:

| Command | What it does |
|---|---|
| `/help`, `/players` | the commands and the players |
| `/tp [nick]` | go to a player (no nick: when there is only one partner in the game) |
| `/tphere [nick]` | bring a player over; `/tphere all` brings everyone (host only) |
| `/go` | host: start the mission without waiting for the missing ones |
| `/mute <nick>`, `/unmute <nick>` | stop / start hearing a player's voice (for you only) |
| `/pm <nick> <text>` | private message |
| `/kick <nick> [reason]` | host: kick a player |

A unique beginning of a nickname is enough.

## Limitations

- The more players are together, the more the host uploads (the server runs on the host's PC): about 8–10 Mbit/s
  for 16 players in one place, about 0.5 Mbit/s for 4. Players spread over the map load the network much less.
- A cutscene shows at most three partners next to CJ.
- Only the host can start story missions, Cesar's races and turf wars. When a mission has CJ carry something, like
  the crates in the house robbery with Ryder, the host does it.
- You see the other player's train but cannot get into it: it is driven by the player whose game made it.
- The other player's parachute canopy is not shown, only their animations. Each player hears their own
  pedestrians' chatter outside missions.
- The partner gets story progress (new areas, gyms, mission markers) by loading the host's world after every
  mission the host passes. Loading takes a few seconds, and the partner keeps their position, weapons and
  progress.
- The host keeps the partner's profile under the partner's nickname. With a different nickname, money and skills
  start over.
- Whatever gets knocked down or broken on the streets stays that way for a few minutes, then the map puts
  everything back, as in the normal game.
- If both players are behind a "symmetric" NAT (common with mobile internet), the invite code cannot connect them
  without a relay. Then you need `turn` or port 7788/UDP forwarded to the host's computer.
- Only `gta_sa.exe` version 1.0 US is supported. The players' builds may differ, but mods that change the story or
  the saves are needed on both sides. If the partner's `data\script\main.scm` differs from the host's, the host's
  world does not load for them, and the mod says so in the chat.
- Mods installed through modloader work. Each player sees their own models, textures and effects. A car that a mod
  adds under a new ID is only visible to the player who has that mod.

## Bugs and ideas

Report bugs, crashes and ideas in [Issues](../../issues/new/choose). A short form opens there: what happened, how
to make it happen again, the mod version and which game you have. The form is in Russian, but you can answer in
English. A [Telegram group](https://t.me/+GNKZb83LqBQyMGEy) is also available if you encounter difficulties sending to GitHub.

With a bug report, attach `coop\coop.log` from the game folder of the player who had the problem. If the game
crashed, also attach `coop\crash.log`, and if you cannot connect, attach the host's `coop\server-data\server.log`.
The log of the previous run is kept as `coop\coop.old.log`: send it if the game was started again after a crash.

## Building from source

You need Windows, Python 3.10 or newer and PowerShell. A script downloads the compiler (llvm-mingw) into the
`toolchain` folder:

```
powershell -ExecutionPolicy Bypass -File tools/get-toolchain.ps1
python tools/build.py          # coop.dll, coop-server.exe and vorbisFile.dll in build/release/bin
python tools/build.py dist     # the same plus the ready archive dist/SA-COOP.zip
python tools/fsd_check.py      # checks the dependencies between the layers
python tools/i18n_check.py     # checks that every text of the game has its English one
python tools/build.py servertest  # server tests: build/release/bin/coop-server-test.exe
```

On Linux and macOS set `LLVM_MINGW` to the llvm-mingw folder; the server tests run under Wine:
`LLVM_MINGW=~/llvm-mingw python3 tools/build.py && wine build/release/bin/coop-server-test.exe`.

GitHub Actions builds every push and keeps the archive in the build artifacts. A tag like `v0.2.0` publishes a
release with `SA-COOP.zip` attached.

## Project layout

The sources follow Feature-Sliced Design. Each of the three programs is a separate application with the layers
`app`, `pages`, `widgets`, `features`, `entities` and `shared`, and the code they share lives in `src/common`.

```
src/
├── client/        coop.asi, runs inside the game
│   ├── app/       entry point, main loop, game patches
│   ├── pages/     the F9 window
│   ├── widgets/   the overlay on top of the game
│   ├── features/  story and cutscenes, progress, garages, voice, weather and more
│   ├── entities/  session, player, vehicle, ped, mission, profile
│   └── shared/    engine wrappers, network, drawing and input, config
├── server/        coop-server.exe, the server for 2–16 players
├── loader/        vorbisFile.dll, the ASI loader for a clean game
└── common/        protocol, network, log and utilities for all three
```

A layer only includes the layers below it and its own slice, which `tools/fsd_check.py` checks. How the mod works
inside and why it is built this way is described in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (in Russian).

## Third-party code

- [ENet](http://enet.bespin.org) (MIT license): network transport, `third_party/enet`.
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause license): function hooking, `third_party/minhook`.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw): the compiler, downloaded during the build.
- Engine addresses and structures were checked against [gta-reversed](https://github.com/gta-reversed/gta-reversed)
  and [plugin-sdk](https://github.com/DK22Pac/plugin-sdk).

## License

The SA:COOP code is released under the MIT license (see [LICENSE](LICENSE)): the original project © Karimov-Akbar,
the fork's changes © their authors. ENet and MinHook have their own
licenses, and their texts are in `third_party` and in the `coop\licenses` folder of the archive.

SA:COOP is not affiliated with Rockstar Games or Take-Two Interactive. You need your own copy of GTA San Andreas,
and the repository contains no game files.
