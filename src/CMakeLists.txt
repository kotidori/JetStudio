# Only the JetScript VM: the editor ships scripts as bytecode inside the package.
file(GLOB JET_SCRIPT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../../runtime/script/*.cpp")
list(FILTER JET_SCRIPT_SOURCES EXCLUDE REGEX "js_(lexer|parser|compiler)\\.cpp$")
# board_input.cpp reads the buttons (GPIO and the I2C expander), sd_card.cpp the games on the card,
# launcher.cpp draws the menu that chooses between them. All three are switched off by the macros in
# generated_board.hpp when the board doesn't have the hardware.
idf_component_register(SRCS "main.cpp" "board_input.cpp" "sd_card.cpp" "launcher.cpp"
  "../../runtime/runtime.cpp" "../../runtime/physics.cpp" "../../runtime/particles.cpp" "../../runtime/tween.cpp"
  "../../runtime/animation.cpp" "../../runtime/script_host.cpp" "../../runtime/pack.cpp"
  ${JET_SCRIPT_SOURCES}
  # JetConfig.hpp comes from the Jet component (firmware/config), not runtime/config.
  # sd_card.cpp needs fatfs (esp_vfs_fat_sdspi_mount) and, through it, sdmmc/esp_driver_sdspi
  # (sdspi_host.h, sdmmc_cmd.h); board_input.cpp needs esp_driver_i2c for the PCF8574 and
  # esp_driver_gpio for the direct buttons.
  INCLUDE_DIRS "." "../../runtime" "../../runtime/script"
  REQUIRES Jet LovyanGFX esp_timer fatfs vfs esp_driver_spi esp_driver_gpio esp_driver_i2c)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PUBLIC JETSCRIPT_NO_COMPILER)
