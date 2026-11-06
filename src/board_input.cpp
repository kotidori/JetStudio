#include "board_input.hpp"
#include <driver/gpio.h>
#include <esp_log.h>
#include <cstring>
#if JET_I2C_EXPANDER
#include <driver/i2c_master.h>
#endif

// Buttons are read once per frame. A GPIO button pulls its pin to GND (internal pull-up), unless the
// board lists it in "active_high" (the Nucleo's blue USER button has its own pull-down).
// The PCF8574 is a quasi-bidirectional expander: writing 1 to a bit turns on its weak pull-up, so a
// button to GND reads back as 0. One byte per frame at 400 kHz costs about 60 us.
namespace input {
namespace {
constexpr int ActionCount = int(sizeof(input_pins) / sizeof(input_pins[0]));
uint8_t bits = 0;     // expander byte, already turned into "pressed" bits
bool online = false;
#if JET_I2C_EXPANDER
constexpr const char* Tag = "input";
constexpr int ExpanderTimeoutMs = 20;
constexpr int FailuresBeforeSlowPolling = 8;
constexpr int SlowPollFrames = 60; // while the expander is missing: try again once a second
i2c_master_bus_handle_t bus = nullptr;
i2c_master_dev_handle_t expander = nullptr;
int failures = 0, frames = 0;
#endif

bool is_expander_pin(int pin) { return pin >= ExpanderBase; }

void read_expander() {
#if JET_I2C_EXPANDER
    if (!expander) return;
    if (!online && failures >= FailuresBeforeSlowPolling && ++frames % SlowPollFrames) return;
    uint8_t value = 0;
    const esp_err_t error = i2c_master_receive(expander, &value, 1, ExpanderTimeoutMs);
    if (error == ESP_OK) {
        bits = Board::expanderActiveHigh ? value : uint8_t(~value);
        online = true;
        failures = 0;
    } else {
        bits = 0;
        if (online) ESP_LOGW(Tag, "The expander at 0x%02x stopped answering (%s)", Board::expanderAddress, esp_err_to_name(error));
        online = false;
        ++failures;
    }
#endif
}
} // namespace

bool uses_expander() {
    for (int pin : input_pins)
        if (is_expander_pin(pin)) return true;
    return false;
}

bool expander_online() { return online; }

int action_bit(const char* name) {
    for (int i = 0; i < ActionCount; ++i)
        if (std::strcmp(input_names[i], name) == 0) return i;
    return -1;
}

void init() {
    for (int i = 0; i < ActionCount; ++i) {
        const int pin = input_pins[i];
        if (pin < 0 || is_expander_pin(pin)) continue;
        gpio_config_t config{};
        config.pin_bit_mask = 1ULL << pin;
        config.mode = GPIO_MODE_INPUT;
        // An active-high button (the board's own, with a pull-down) must not get a pull-up.
        config.pull_up_en = (input_active_high >> i) & 1 ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
        config.pull_down_en = (input_active_high >> i) & 1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        gpio_config(&config);
    }
#if JET_I2C_EXPANDER
    if (!uses_expander()) return;
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = gpio_num_t(Board::expanderSda);
    bus_config.scl_io_num = gpio_num_t(Board::expanderScl);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.intr_priority = 0;
    bus_config.trans_queue_depth = 0; // synchronous reads, one per frame
    bus_config.flags.enable_internal_pullup = 1;
    esp_err_t error = i2c_new_master_bus(&bus_config, &bus);
    if (error != ESP_OK) {
        ESP_LOGE(Tag, "I2C bus on SDA %d / SCL %d failed: %s", Board::expanderSda, Board::expanderScl, esp_err_to_name(error));
        return;
    }
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = uint16_t(Board::expanderAddress);
    device.scl_speed_hz = 400000;
    device.scl_wait_us = 0;
    device.flags.disable_ack_check = 0;
    error = i2c_master_bus_add_device(bus, &device, &expander);
    if (error != ESP_OK) {
        ESP_LOGE(Tag, "The expander at 0x%02x could not be added: %s", Board::expanderAddress, esp_err_to_name(error));
        expander = nullptr;
        return;
    }
    const uint8_t all_high = 0xff; // every pin an input with its weak pull-up on
    i2c_master_transmit(expander, &all_high, 1, 50);
    read_expander();
    if (online) ESP_LOGI(Tag, "Expander at 0x%02x: %d buttons", Board::expanderAddress, Board::expanderBits);
    else ESP_LOGW(Tag, "No expander at 0x%02x: its buttons stay released", Board::expanderAddress);
#endif
}

uint32_t read() {
    read_expander();
    uint32_t mask = 0;
    for (int i = 0; i < ActionCount; ++i) {
        const int pin = input_pins[i];
        if (pin < 0) continue;
        bool pressed;
        if (is_expander_pin(pin)) {
            const int bit = pin - ExpanderBase;
            pressed = bit < Board::expanderBits && ((bits >> bit) & 1) != 0;
        } else {
            const int level = gpio_get_level(gpio_num_t(pin));
            pressed = ((input_active_high >> i) & 1) ? level == 1 : level == 0;
        }
        if (pressed) mask |= 1u << i;
    }
    return mask;
}
} // namespace input
