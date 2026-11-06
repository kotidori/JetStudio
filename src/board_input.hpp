#pragma once
// The board's buttons: GPIO pins and, when the board has one, an I2C port expander (PCF8574).
// generated_inputs.hpp gives one pin per action, in package order; a pin of ExpanderBase + n is
// bit n of the expander instead of a GPIO (see EXPANDER_PIN_BASE in tools/hardware.py).
#include "generated_board.hpp"
#include "generated_inputs.hpp"
#include <cstdint>

namespace input {
// 100 + n is expander bit n: a handheld with a D-pad on a PCF8574 runs out of GPIOs otherwise.
constexpr int ExpanderBase = 100;

void init();          // GPIO pull-ups, then the I2C bus and the expander
uint32_t read();      // bit i = action i held down; call once per frame
int action_bit(const char* name); // index of an action in the package order, or -1
bool expander_online();           // the expander answered on the last read
bool uses_expander();             // any action is wired to it
} // namespace input
