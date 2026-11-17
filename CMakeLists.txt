# Per-LED overlay for ZMK's RGB underglow.
if (CONFIG_ZMK_RGB_LED_OVERLAY)
  zephyr_library()
  zephyr_library_sources(src/led_overlay.c)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
  # Public header for indicator modules: #include <zmk_rgb_led_overlay/led_overlay.h>
  zephyr_include_directories(include)
endif()
