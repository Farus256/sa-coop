#pragma once
// The partner's character: CJ or one of the Grove Street Families. A partner new to the host's world (the server has
// no profile of it) or joining the host's new game picks it in the lobby, on cards drawn there; it is put on once the
// world has loaded and kept in its profile from then on. Whoever joins after the game started is CJ. A character who
// is not CJ cannot change clothes: the clothes shops, barbers and tattoo parlours stay shut for it, the game never
// dresses it (CJ's clothes on another body crash the game), and CJ's voice lines are not its.
#include <cstdint>
#include <string>

namespace coop::skin {

struct Text {
    const char* ru;
    const char* en;
};
struct Choice {
    uint16_t model;              // 0: CJ in his clothes
    Text name;
    uint32_t shirt, pants, hat;  // the colours of its card (ARGB; hat 0: none)
};
const Choice* Choices(int& count);

// the lobby: whether this player picks a character now (new here), what it picked
void OnProfile(bool empty);   // the server's profile of this player came (empty: new to this world)
bool Newcomer();
void SetChoice(int index);
int ChosenIndex();
// Progress, once the world loaded and the player is new here: what was picked in the lobby goes on
void ApplyChoice();

void Wear(uint16_t model);   // this game's player becomes that character (0: CJ in his clothes)
uint16_t Worn();             // what this game's player is: 0 CJ, else the Families model

}  // namespace coop::skin
