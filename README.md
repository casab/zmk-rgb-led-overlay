# zmk-rgb-led-overlay

Per-LED control, fades and animations on top of ZMK's RGB underglow, for [ZMK](https://zmk.dev).

ZMK's underglow lights the whole strip with one effect. This module adds a virtual LED strip
between the underglow and the real strip. ZMK keeps doing everything it does today (effects, the
`&rgb_ug` keys, saving, turning off when idle), and every frame passes through here, where:

- **disabled LEDs** are left out of the underglow effect (indicators can still light them
  briefly);
- **indicator LEDs** painted by other modules are blended on top of the effect, fading in and out;
- **animations** (sweeps, rings, pulses) move across the board by the LEDs' real positions;
- an **identify** mode shows each LED in its own color, so you can find out which index is which.

When the underglow is off (RGB off, or idle) and an indicator wants to show, the module turns the
LED power on just for it and off again afterwards. While the keyboard is idle, indicators are
hidden. Animation frames only run while something is fading or moving.

Indicator modules that use it:
[zmk-rgb-layer-indicator](https://github.com/casab/zmk-rgb-layer-indicator),
[zmk-rgb-ble-indicator](https://github.com/casab/zmk-rgb-ble-indicator),
[zmk-rgb-battery-indicator](https://github.com/casab/zmk-rgb-battery-indicator),
[zmk-rgb-media-indicator](https://github.com/casab/zmk-rgb-media-indicator).

## Add it to your zmk-config

1. `config/west.yml`: add the project.

   ```yaml
       - name: zmk-rgb-led-overlay
         remote: casab          # url-base: https://github.com/casab
         revision: main
   ```

2. RGB underglow must be enabled (`CONFIG_ZMK_RGB_UNDERGLOW=y`).

3. In your keymap (or an overlay), add the node and point the underglow at it:

   ```dts
   / {
       led_overlay: led_overlay {
           compatible = "zmk,led-strip-overlay";
           led-strip = <&led_strip>;   // the real strip from your board/shield
           chain-length = <11>;        // same as the real strip
           disabled-leds = <10>;       // optional: LEDs left out of the underglow effect
           led-x = <105 66 43 79 123 166 211 246 223 185 145>;  // optional: x of each LED, mm
       };
       chosen {
           zmk,underglow = &led_overlay;
       };
   };
   ```

## Finding out which LED is which

LEDs are numbered along the strip's wiring (0 is the first after the controller), not by
position. Build once with `identify;` in the node: LED *i* then shows hue *i* × 360 / chain-length,
so the order is red, orange, yellow, green, ..., blue, purple, pink. Note which color is where,
then remove `identify;`.

If the board's KiCad files are published, they give both the order and the positions: follow the
DOUT → DIN connections from the controller for the order, and take each LED footprint's x
coordinate for `led-x`.

## Properties

| Property | Type | Required | Meaning |
| --- | --- | --- | --- |
| `led-strip` | phandle | yes | The real LED strip. |
| `chain-length` | int | yes | Number of LEDs (ZMK's underglow reads it from this node). |
| `disabled-leds` | array | no | LED indexes left out of the underglow effect. |
| `led-x` | array | no | x position of each LED in chain order (any unit), for animations. Without it, LEDs count as evenly spaced in chain order. |
| `identify` | bool | no | Show each LED in its own color instead of the underglow. |

## Kconfig

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_ZMK_RGB_LED_OVERLAY_SLOTS` | 6 | Overlay slots; each indicator uses one, and higher slots are drawn on top. |
| `CONFIG_ZMK_RGB_LED_OVERLAY_FADE_MS` | 150 | Fade-in / fade-out time of indicator LEDs. |
| `CONFIG_ZMK_RGB_LED_OVERLAY_FRAME_MS` | 16 | Time between frames while something moves. |

## API for indicator modules

`#include <zmk_rgb_led_overlay/led_overlay.h>` (available when `CONFIG_ZMK_RGB_LED_OVERLAY=y`).

Static LEDs: describe what a slot shows, then commit. New LEDs fade in, removed ones fade out,
unchanged ones stay put, so repainting a slot with the same LEDs doesn't flicker.

```c
zmk_led_overlay_clear(slot);
zmk_led_overlay_set(slot, led, zmk_led_overlay_hsb(270, 100, 50));
zmk_led_overlay_set_delayed(slot, led, color, 40);  // starts fading in 40 ms after the commit
zmk_led_overlay_commit();
```

Animations: one-shot effects; a new one in the same slot replaces the old one.

```c
struct zmk_led_overlay_anim anim = {
    .type = ZMK_LED_OVERLAY_ANIM_SWEEP_RIGHT,  // also SWEEP_LEFT, RING_OUT, RING_IN, PULSE
    .color = zmk_led_overlay_hsb(175, 100, 50),
    .duration_ms = 350,
    .leds = 0,                                 // PULSE: bit per LED
};
zmk_led_overlay_animate(slot, &anim);
```

## License

MIT
