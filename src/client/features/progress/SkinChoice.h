#pragma once
// The partner's character: CJ or one of the Grove Street Families. Every partner in the lobby picks it there (a window
// beside the lobby shows it, arrows under it turn to the next one); it is put on once the world has loaded and kept in
// its profile from then on. A partner who leaves the arrows alone keeps the one its profile has (CJ when new); one who
// joins after the game started is CJ when new to the world. A character who
// is not CJ cannot change clothes: the clothes shops, barbers and tattoo parlours stay shut for it, the game never
// dresses it (CJ's clothes on another body crash the game), and CJ's voice lines are not its.
#include <cstdint>
#include <string>

namespace coop::skin {

struct Text {
    const char* ru;
    const char* en;
};
enum Top : uint8_t { TOP_TANK, TOP_TEE, TOP_LONG };
enum Head : uint8_t { HEAD_HAIR, HEAD_CAP, HEAD_BANDANA };
struct Choice {
    uint16_t model;              // 0: CJ in his clothes
    Text name;
    uint32_t shirt, pants, hat;  // the colours of its picture (ARGB)
    uint8_t top, head;           // what it wears up top and on its head
    uint32_t shoes;
};
const Choice* Choices(int& count);

// the lobby: whether this player picks a character now (new here), what it picked
// the server's profile of this player came (empty: new to this world), with the character it keeps
void OnProfile(bool empty, uint16_t worn);
bool Newcomer();
void SetChoice(int index);   // the player turned to this one in the lobby
int ChosenIndex();
bool Picked();               // ... it did (what it picked goes on over its profile's)
// Progress, once the world loaded and the player is new here or picked one: what was picked in the lobby goes on
void ApplyChoice();

void Wear(uint16_t model);   // this game's player becomes that character (0: CJ in his clothes)
uint16_t Worn();             // what this game's player is: 0 CJ, else the Families model

}  // namespace coop::skin
