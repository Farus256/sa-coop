#pragma once

#include <string>

namespace coop::firewall {

// Windows Firewall towards a program that takes connections, on the network profiles in use now. Blocked: a rule blocks
// it (a "Windows Security Alert" cancelled, or never seen behind the full-screen game, leaves block rules). NoRule:
// nothing allows it there (only Private networks were ticked while the network is Public). Unknown: not readable.
enum class State { Allowed, Blocked, NoRule, Unknown };
State Check(const std::wstring& program);

// Asks Windows for administrator rights (its UAC prompt) to replace the program's rules with one that allows it on
// every network. False when the player refused or it could not be asked; the rules change a moment after true.
bool AllowElevated(const std::wstring& program);

}  // namespace coop::firewall
