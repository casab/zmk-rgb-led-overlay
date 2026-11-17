/*
 * Copyright (c) 2026 Engin Al
 *
 * SPDX-License-Identifier: MIT
 *
 * A virtual LED strip that sits between ZMK's RGB underglow and the real strip. Point
 * `zmk,underglow` at it: ZMK keeps doing effects, the RGB keys, saving and idle-off, and every
 * frame it draws passes through here, where
 *   - disabled LEDs are left out of the underglow effect (indicators may still use them),
 *   - the slots painted by indicator modules are blended on top, with fades and animations,
 *   - `identify` shows each LED in its own color, to find out which index is which LED.
 *
 * When the underglow is off (RGB off or idle) but an indicator wants to show, this turns the LED
 * power on for it and off again afterwards. While the keyboard is idle, indicators are hidden.
 *
 * Threads: all pixel state and drawing live on ZMK's low-priority work queue, the same queue the
 * underglow draws from. The public API only edits the slot targets under a spinlock and queues
 * work.
 */

#define DT_DRV_COMPAT zmk_led_strip_overlay

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <math.h>
#include <string.h>

#include <drivers/ext_power.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/rgb_underglow.h>
#include <zmk/workqueue.h>

#include <zmk_rgb_led_overlay/led_overlay.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Exactly one zmk,led-strip-overlay node is supported");

#define OVERLAY_NODE DT_DRV_INST(0)
#define NUM_LEDS DT_PROP(OVERLAY_NODE, chain_length)
#define NUM_SLOTS CONFIG_ZMK_RGB_LED_OVERLAY_SLOTS
#define IDENTIFY DT_PROP(OVERLAY_NODE, identify)
#define FADE_MS CONFIG_ZMK_RGB_LED_OVERLAY_FADE_MS
#define FRAME_MS CONFIG_ZMK_RGB_LED_OVERLAY_FRAME_MS

BUILD_ASSERT(NUM_LEDS <= 32, "At most 32 LEDs are supported");

static const struct device *const real_strip = DEVICE_DT_GET(DT_PHANDLE(OVERLAY_NODE, led_strip));

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#else
static const struct device *const ext_power = NULL;
#endif

#if DT_NODE_HAS_PROP(OVERLAY_NODE, disabled_leds)
static const uint8_t disabled_leds[] = DT_PROP(OVERLAY_NODE, disabled_leds);
#endif

#if DT_NODE_HAS_PROP(OVERLAY_NODE, led_x)
BUILD_ASSERT(DT_PROP_LEN(OVERLAY_NODE, led_x) == NUM_LEDS, "led-x needs one value per LED");
static const int32_t led_x_prop[] = DT_PROP(OVERLAY_NODE, led_x);
#endif

// LED positions along the board (left to right), and the board's span.
static float led_x[NUM_LEDS];
static float x_min, x_max, x_center;
// Half-width of the soft band used by animations, in the same unit as led-x.
static float band;

/* ---- Slot targets: written by the API, read by the work queue ---- */

struct slot_target {
    uint32_t mask;
    struct led_rgb color[NUM_LEDS];
    uint16_t delay_ms[NUM_LEDS];
};

static struct k_spinlock lock;
static struct slot_target targets[NUM_SLOTS];
static struct zmk_led_overlay_anim pending_anim[NUM_SLOTS];
static uint32_t pending_anim_mask;
static uint32_t disabled_mask;

/* ---- Work-queue state ---- */

enum pixel_state { PIXEL_OFF, PIXEL_IN, PIXEL_ON, PIXEL_OUT };

struct pixel {
    struct led_rgb color;
    int64_t t0; // IN: when the fade-in starts; OUT: when the fade-out started
    uint8_t state;
};

struct running_anim {
    struct zmk_led_overlay_anim anim;
    int64_t start;
    bool active;
};

static struct pixel pixels[NUM_SLOTS][NUM_LEDS];
static struct running_anim anims[NUM_SLOTS];

// Last frame from the underglow, and the frame sent to the strip.
static struct led_rgb base[NUM_LEDS];
static struct led_rgb out[NUM_LEDS];

// The LED power was turned on here (not by the underglow) for an indicator.
static bool powered_for_overlay;

static void frame_work_handler(struct k_work *work);
static void apply_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_overlay_frame_work, frame_work_handler);
static K_WORK_DEFINE(led_overlay_apply_work, apply_work_handler);

/* ---- Color helpers ---- */

struct led_rgb zmk_led_overlay_hsb(uint16_t h, uint8_t s, uint8_t b) {
    float r = 0, g = 0, bl = 0;

    h %= 360;
    uint8_t i = h / 60;
    float v = MIN(b, 100) / 100.0f;
    float sat = MIN(s, 100) / 100.0f;
    float f = h / 360.0f * 6 - i;
    float p = v * (1 - sat);
    float q = v * (1 - f * sat);
    float t = v * (1 - (1 - f) * sat);

    switch (i % 6) {
    case 0:
        r = v, g = t, bl = p;
        break;
    case 1:
        r = q, g = v, bl = p;
        break;
    case 2:
        r = p, g = v, bl = t;
        break;
    case 3:
        r = p, g = q, bl = v;
        break;
    case 4:
        r = t, g = p, bl = v;
        break;
    case 5:
        r = v, g = p, bl = q;
        break;
    }

    return (struct led_rgb){.r = r * 255, .g = g * 255, .b = bl * 255};
}

static struct led_rgb blend(struct led_rgb under, struct led_rgb over, float alpha) {
    return (struct led_rgb){
        .r = under.r + (over.r - under.r) * alpha,
        .g = under.g + (over.g - under.g) * alpha,
        .b = under.b + (over.b - under.b) * alpha,
    };
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static float smoothstep(float t) {
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}

/* ---- Public API (any thread) ---- */

size_t zmk_led_overlay_length(void) { return NUM_LEDS; }

int zmk_led_overlay_set_delayed(uint8_t slot, uint8_t led, struct led_rgb color,
                                uint16_t delay_ms) {
    if (slot >= NUM_SLOTS || led >= NUM_LEDS) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    targets[slot].mask |= BIT(led);
    targets[slot].color[led] = color;
    targets[slot].delay_ms[led] = delay_ms;
    k_spin_unlock(&lock, key);

    return 0;
}

int zmk_led_overlay_set(uint8_t slot, uint8_t led, struct led_rgb color) {
    return zmk_led_overlay_set_delayed(slot, led, color, 0);
}

int zmk_led_overlay_clear_led(uint8_t slot, uint8_t led) {
    if (slot >= NUM_SLOTS || led >= NUM_LEDS) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    targets[slot].mask &= ~BIT(led);
    k_spin_unlock(&lock, key);

    return 0;
}

int zmk_led_overlay_clear(uint8_t slot) {
    if (slot >= NUM_SLOTS) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    targets[slot].mask = 0;
    k_spin_unlock(&lock, key);

    return 0;
}

void zmk_led_overlay_commit(void) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &led_overlay_apply_work);
}

int zmk_led_overlay_animate(uint8_t slot, const struct zmk_led_overlay_anim *anim) {
    if (slot >= NUM_SLOTS || anim == NULL || anim->duration_ms == 0) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    pending_anim[slot] = *anim;
    pending_anim_mask |= BIT(slot);
    k_spin_unlock(&lock, key);

    zmk_led_overlay_commit();
    return 0;
}

/* ---- Work queue: fades, animations, drawing ---- */

// Current opacity of a static pixel; moves finished fades on.
static float pixel_alpha(struct pixel *p, int64_t now) {
    switch (p->state) {
    case PIXEL_IN: {
        if (now < p->t0) {
            return 0;
        }
        float a = (float)(now - p->t0) / FADE_MS;
        if (a >= 1) {
            p->state = PIXEL_ON;
            return 1;
        }
        return smoothstep(a);
    }
    case PIXEL_ON:
        return 1;
    case PIXEL_OUT: {
        float a = 1 - (float)(now - p->t0) / FADE_MS;
        if (a <= 0) {
            p->state = PIXEL_OFF;
            return 0;
        }
        return smoothstep(a);
    }
    default:
        return 0;
    }
}

// Linear opacity, used to continue a fade in the other direction without a jump.
static float pixel_linear_alpha(const struct pixel *p, int64_t now) {
    switch (p->state) {
    case PIXEL_IN:
        return now < p->t0 ? 0 : clamp01((float)(now - p->t0) / FADE_MS);
    case PIXEL_ON:
        return 1;
    case PIXEL_OUT:
        return clamp01(1 - (float)(now - p->t0) / FADE_MS);
    default:
        return 0;
    }
}

// Brightness of an animation at an LED, 0..1.
static float anim_intensity(const struct running_anim *ra, int led, int64_t now) {
    float t = (float)(now - ra->start) / ra->anim.duration_ms;
    if (t >= 1) {
        return 0;
    }

    float x = led_x[led];
    float half_span = (x_max - x_min) / 2;

    switch (ra->anim.type) {
    case ZMK_LED_OVERLAY_ANIM_SWEEP_RIGHT:
    case ZMK_LED_OVERLAY_ANIM_SWEEP_LEFT: {
        float travel = (x_max - x_min) + 2 * band;
        float pos = smoothstep(t) * travel;
        float center = ra->anim.type == ZMK_LED_OVERLAY_ANIM_SWEEP_RIGHT ? x_min - band + pos
                                                                          : x_max + band - pos;
        return smoothstep(1 - fabsf(x - center) / band);
    }
    case ZMK_LED_OVERLAY_ANIM_RING_OUT:
    case ZMK_LED_OVERLAY_ANIM_RING_IN: {
        float reach = half_span + band;
        float r = ra->anim.type == ZMK_LED_OVERLAY_ANIM_RING_OUT ? smoothstep(t) * reach
                                                                 : (1 - smoothstep(t)) * reach;
        float d = fabsf(x - x_center);
        return smoothstep(1 - fabsf(d - r) / band);
    }
    case ZMK_LED_OVERLAY_ANIM_PULSE:
        if (!(ra->anim.leds & BIT(led))) {
            return 0;
        }
        // Up and down once: 0 -> 1 -> 0.
        return smoothstep(1 - fabsf(2 * t - 1));
    }

    return 0;
}

// Builds `out` from `base` and the slots, and sends it. Returns whether anything is still moving.
static bool render(void) {
    int64_t now = k_uptime_get();
    bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    bool painted = false;
    bool moving = false;

    for (int i = 0; i < NUM_LEDS; i++) {
        out[i] = (disabled_mask & BIT(i)) ? (struct led_rgb){0} : base[i];

        if (IDENTIFY) {
            out[i] = zmk_led_overlay_hsb(i * 360 / NUM_LEDS, 100, 30);
            painted = true;
            continue;
        }

        for (int s = 0; s < NUM_SLOTS; s++) {
            struct pixel *p = &pixels[s][i];
            float a = pixel_alpha(p, now);
            if (p->state == PIXEL_IN || p->state == PIXEL_OUT) {
                moving = true;
            }
            if (active && a > 0) {
                out[i] = blend(out[i], p->color, a);
                painted = true;
            }

            struct running_anim *ra = &anims[s];
            if (ra->active) {
                float ai = anim_intensity(ra, i, now);
                if (active && ai > 0) {
                    out[i] = blend(out[i], ra->anim.color, ai);
                    painted = true;
                }
            }
        }
    }

    for (int s = 0; s < NUM_SLOTS; s++) {
        if (anims[s].active) {
            if (now - anims[s].start >= anims[s].anim.duration_ms) {
                anims[s].active = false;
            } else {
                moving = true;
            }
        }
    }

    if (ext_power != NULL && device_is_ready(ext_power)) {
        if (painted && ext_power_get(ext_power) <= 0) {
            ext_power_enable(ext_power);
            powered_for_overlay = true;
            // WS2812 LEDs need a moment after power-up before they take data.
            k_msleep(5);
        } else if (!painted && powered_for_overlay) {
            powered_for_overlay = false;
            bool underglow_on = false;
            zmk_rgb_underglow_get_state(&underglow_on);
            // The underglow manages the power itself while it's on; leave it alone then.
            if (!underglow_on) {
                led_strip_update_rgb(real_strip, out, NUM_LEDS);
                ext_power_disable(ext_power);
                return moving;
            }
        }
    }

    led_strip_update_rgb(real_strip, out, NUM_LEDS);
    return moving;
}

static void render_and_schedule(void) {
    if (render()) {
        // Keep drawing frames while something fades or moves. Doesn't push back a pending frame.
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_overlay_frame_work,
                                  K_MSEC(FRAME_MS));
    }
}

static void frame_work_handler(struct k_work *work) { render_and_schedule(); }

// Applies the slot targets (with fades) and new animations, then draws.
static void apply_work_handler(struct k_work *work) {
    static struct slot_target snapshot[NUM_SLOTS];
    static struct zmk_led_overlay_anim new_anims[NUM_SLOTS];
    int64_t now = k_uptime_get();

    k_spinlock_key_t key = k_spin_lock(&lock);
    memcpy(snapshot, targets, sizeof(snapshot));
    uint32_t anim_mask = pending_anim_mask;
    memcpy(new_anims, pending_anim, sizeof(new_anims));
    pending_anim_mask = 0;
    k_spin_unlock(&lock, key);

    for (int s = 0; s < NUM_SLOTS; s++) {
        for (int i = 0; i < NUM_LEDS; i++) {
            struct pixel *p = &pixels[s][i];
            bool wanted = snapshot[s].mask & BIT(i);
            float a = pixel_linear_alpha(p, now);

            if (wanted) {
                p->color = snapshot[s].color[i];
                if (p->state == PIXEL_OFF || p->state == PIXEL_OUT) {
                    uint16_t delay = snapshot[s].delay_ms[i];
                    p->state = PIXEL_IN;
                    // Continue from the current opacity, or wait for the delay from dark.
                    p->t0 = (delay > 0 && a == 0) ? now + delay : now - (int64_t)(a * FADE_MS);
                }
            } else if (p->state == PIXEL_IN || p->state == PIXEL_ON) {
                p->state = PIXEL_OUT;
                p->t0 = now - (int64_t)((1 - a) * FADE_MS);
            }
        }

        if (anim_mask & BIT(s)) {
            anims[s] = (struct running_anim){.anim = new_anims[s], .start = now, .active = true};
        }
    }

    render_and_schedule();
}

/* ---- led_strip API: the underglow draws through here ---- */

static int overlay_update_rgb(const struct device *dev, struct led_rgb *frame,
                              size_t num_pixels) {
    size_t n = MIN(num_pixels, NUM_LEDS);

    memcpy(base, frame, n * sizeof(struct led_rgb));
    render_and_schedule();

    return 0;
}

static size_t overlay_length(const struct device *dev) { return NUM_LEDS; }

static const struct led_strip_driver_api overlay_api = {
    .update_rgb = overlay_update_rgb,
    .length = overlay_length,
};

static int overlay_activity_listener(const zmk_event_t *eh) {
    // Hide indicators while idle, show them again on activity.
    zmk_led_overlay_commit();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_rgb_led_overlay, overlay_activity_listener);
ZMK_SUBSCRIPTION(zmk_rgb_led_overlay, zmk_activity_state_changed);

static int overlay_init(const struct device *dev) {
#if DT_NODE_HAS_PROP(OVERLAY_NODE, disabled_leds)
    for (size_t i = 0; i < ARRAY_SIZE(disabled_leds); i++) {
        if (disabled_leds[i] < NUM_LEDS) {
            disabled_mask |= BIT(disabled_leds[i]);
        }
    }
#endif

    for (int i = 0; i < NUM_LEDS; i++) {
#if DT_NODE_HAS_PROP(OVERLAY_NODE, led_x)
        led_x[i] = led_x_prop[i];
#else
        led_x[i] = i; // no positions: chain order, evenly spaced
#endif
        x_min = (i == 0 || led_x[i] < x_min) ? led_x[i] : x_min;
        x_max = (i == 0 || led_x[i] > x_max) ? led_x[i] : x_max;
    }
    x_center = (x_min + x_max) / 2;
    // About one and a half LED spacings wide on each side.
    band = MAX((x_max - x_min) / NUM_LEDS * 3, 1.0f);

    if (!device_is_ready(real_strip)) {
        LOG_ERR("LED overlay: the real LED strip isn't ready");
        return -ENODEV;
    }

    if (IDENTIFY) {
        LOG_WRN("LED overlay: identify mode, LED i shows hue i * 360 / %d", NUM_LEDS);
        zmk_led_overlay_commit();
    }

    return 0;
}

DEVICE_DT_INST_DEFINE(0, overlay_init, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_ZMK_RGB_LED_OVERLAY_INIT_PRIORITY, &overlay_api);
