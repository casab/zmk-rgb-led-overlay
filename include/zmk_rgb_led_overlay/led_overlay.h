/*
 * Copyright (c) 2026 Engin Al
 *
 * SPDX-License-Identifier: MIT
 *
 * Per-LED overlay on top of ZMK's RGB underglow. Indicator modules paint LEDs through this API.
 * Each caller uses its own slot; slots are blended over the underglow in order, so higher slots
 * end up on top.
 *
 * Static LEDs: zmk_led_overlay_set/clear describe what a slot should show; zmk_led_overlay_commit()
 * applies it with fades (new LEDs fade in, removed ones fade out, unchanged ones stay put).
 *
 * Animations: zmk_led_overlay_animate() plays a short one-shot effect in a slot, moving across the
 * LEDs by their real positions (`led-x` on the overlay node). A new one replaces the old one.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>

// Number of LEDs on the strip.
size_t zmk_led_overlay_length(void);

// Show `color` on one LED of a slot (fades in on commit).
int zmk_led_overlay_set(uint8_t slot, uint8_t led, struct led_rgb color);

// Like zmk_led_overlay_set, but the LED starts fading in `delay_ms` after the commit.
int zmk_led_overlay_set_delayed(uint8_t slot, uint8_t led, struct led_rgb color,
                                uint16_t delay_ms);

// Stop showing one LED / every LED of a slot (fades out on commit).
int zmk_led_overlay_clear_led(uint8_t slot, uint8_t led);
int zmk_led_overlay_clear(uint8_t slot);

// Apply the slots' set/clear changes.
void zmk_led_overlay_commit(void);

// While the keyboard is idle, a slot's static LEDs are hidden (so a persistent indicator doesn't
// keep the LED power on). Temporary indicators that clear themselves, such as a short flash on an
// event that can happen without a keypress, call this with true to show while idle too.
// Animations always show.
int zmk_led_overlay_set_show_when_idle(uint8_t slot, bool show);

enum zmk_led_overlay_anim_type {
    // A soft band of light crosses the board left to right / right to left.
    ZMK_LED_OVERLAY_ANIM_SWEEP_RIGHT,
    ZMK_LED_OVERLAY_ANIM_SWEEP_LEFT,
    // A soft band grows from the center outwards / shrinks from the edges inwards.
    ZMK_LED_OVERLAY_ANIM_RING_OUT,
    ZMK_LED_OVERLAY_ANIM_RING_IN,
    // The LEDs in `leds` fade in and out once.
    ZMK_LED_OVERLAY_ANIM_PULSE,
};

struct zmk_led_overlay_anim {
    enum zmk_led_overlay_anim_type type;
    struct led_rgb color;
    uint16_t duration_ms;
    // Bit per LED the animation may light. 0 means all LEDs (PULSE needs at least one bit).
    // Leaving LEDs out doesn't move the animation: sweeps and rings still use every LED's position.
    uint32_t leds;
};

// Play a one-shot animation in a slot.
int zmk_led_overlay_animate(uint8_t slot, const struct zmk_led_overlay_anim *anim);

// HSB (hue 0-360, saturation 0-100, brightness 0-100) to RGB, like ZMK's underglow does it.
struct led_rgb zmk_led_overlay_hsb(uint16_t h, uint8_t s, uint8_t b);
