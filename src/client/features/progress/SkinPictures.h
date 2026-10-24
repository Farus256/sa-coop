#pragma once
// Pictures of the Families characters for the lobby (ARGB pixels, straight alpha, rows top-down)
#include <cstdint>

namespace coop::skin {

const uint32_t* Picture(uint16_t model, int& w, int& h);  // nullptr: none (CJ)

}  // namespace coop::skin
