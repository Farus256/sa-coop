#pragma once
// The HUD in the proportions it was drawn for. The game lays its HUD out on a 640-wide 4:3 screen and stretches it to the
// screen's width: on 16:9 the radar is an oval and every icon and letter a third wider than it is tall. While the HUD is
// drawn (Begin..End), what it draws is narrowed by 4:3 / the screen's aspect towards the side of the screen it belongs
// to: what lies in the left third towards the left edge (the radar, the help box), the right third towards the right
// edge (money, weapon, wanted level), the middle towards the centre (titles, subtitles). Each picture (an Im2D primitive)
// and each string goes by its own reference point, so nothing is torn apart; what covers most of the screen (a fade)
// stays as it is. Texts are narrowed where they are printed (CFont::PrintString), their buffered letters not again.
namespace coop::hud {

void Install();   // the hooks on the font (once)
void Begin();     // the HUD (or a message of it) is drawn now
void End();
void Suspend();   // what is drawn now belongs to the world or the camera (the crosshair, a sniper scope): as it is
void Resume();
bool Active();    // narrowing what is drawn now
float Factor();   // 4:3 / the screen's aspect, 1 on a 4:3 screen or with the setting off

}  // namespace coop::hud
