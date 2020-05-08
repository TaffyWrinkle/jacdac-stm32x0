#include "jdsimple.h"

#define LIGHT_TYPE_WS2812B_GRB 0
#define LIGHT_TYPE_SK9822 1

#define DEFAULT_INTENSITY 15
#define DEFAULT_NUMPIXELS 15
#define DEFAULT_MAXPOWER 200

#define FRAME_TIME 50000

#define LOG DMESG

#define LIGHT_REG_LIGHTTYPE 0x80
#define LIGHT_REG_NUMPIXELS 0x81
#define LIGHT_REG_DURATION 0x82
#define LIGHT_REG_COLOR 0x83

#define LIGHT_CMD_START_ANIMATION 0x80

REG_DEFINITION(                   //
    light_regs,                   //
    REG_SRV_BASE,                 //
    REG_U8(JD_REG_INTENSITY),     //
    REG_U8(LIGHT_REG_LIGHTTYPE),  //
    REG_U16(LIGHT_REG_NUMPIXELS), //
    REG_U16(JD_REG_MAX_POWER),    //
    REG_U16(LIGHT_REG_DURATION),  //
    REG_U32(LIGHT_REG_COLOR),     //
)

struct srv_state {
    SRV_COMMON;

    uint8_t intensity;
    uint8_t lighttype;
    uint16_t numpixels;
    uint16_t maxpower;
    uint16_t duration;
    uint32_t color;

    uint32_t *pxbuffer;
    uint16_t pxbuffer_allocated;
    uint32_t nextFrame;
    volatile uint8_t in_tx;
    volatile uint8_t dirty;
    uint8_t inited;

    uint8_t anim_flag;
    uint32_t anim_step, anim_value;
    srv_cb_t anim_fn;
    uint32_t anim_end;
};

static srv_t *state_;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (r << 16) | (g << 8) | b;
}

static uint32_t hsv(uint8_t hue, uint8_t sat, uint8_t val) {
    // scale down to 0..192
    hue = (hue * 192) >> 8;

    // reference: based on FastLED's hsv2rgb rainbow algorithm
    // [https://github.com/FastLED/FastLED](MIT)
    int invsat = 255 - sat;
    int brightness_floor = (val * invsat) >> 8;
    int color_amplitude = val - brightness_floor;
    int section = (hue / 0x40) >> 0; // [0..2]
    int offset = (hue % 0x40) >> 0;  // [0..63]

    int rampup = offset;
    int rampdown = (0x40 - 1) - offset;

    int rampup_amp_adj = ((rampup * color_amplitude) / (256 / 4));
    int rampdown_amp_adj = ((rampdown * color_amplitude) / (256 / 4));

    int rampup_adj_with_floor = (rampup_amp_adj + brightness_floor);
    int rampdown_adj_with_floor = (rampdown_amp_adj + brightness_floor);

    int r, g, b;
    if (section) {
        if (section == 1) {
            // section 1: 0x40..0x7F
            r = brightness_floor;
            g = rampdown_adj_with_floor;
            b = rampup_adj_with_floor;
        } else {
            // section 2; 0x80..0xBF
            r = rampup_adj_with_floor;
            g = brightness_floor;
            b = rampdown_adj_with_floor;
        }
    } else {
        // section 0: 0x00..0x3F
        r = rampdown_adj_with_floor;
        g = rampup_adj_with_floor;
        b = brightness_floor;
    }
    return rgb(r, g, b);
}

static const uint8_t b_m16[] = {0, 49, 49, 41, 90, 27, 117, 10};
static int isin(uint8_t theta) {
    // reference: based on FASTLed's sin approximation method:
    // [https://github.com/FastLED/FastLED](MIT)
    int offset = theta;
    if (theta & 0x40) {
        offset = 255 - offset;
    }
    offset &= 0x3F; // 0..63

    int secoffset = offset & 0x0F; // 0..15
    if (theta & 0x40)
        secoffset++;

    int section = offset >> 4; // 0..3
    int s2 = section * 2;

    int b = b_m16[s2];
    int m16 = b_m16[s2 + 1];
    int mx = (m16 * secoffset) >> 4;

    int y = mx + b;
    if (theta & 0x80)
        y = -y;

    y += 128;

    return y;
}

static bool is_empty(const uint8_t *data, uint32_t size) {
    for (unsigned i = 0; i < size; ++i) {
        if (data[i])
            return false;
    }
    return true;
}

static bool is_enabled(srv_t *state) {
    // TODO check if all pixels are zero
    return state->numpixels > 0 && state->intensity > 0;
}

static void set(srv_t *state, uint32_t index, uint32_t color) {
    px_set(state->pxbuffer, index, state->intensity, color);
}

static void show(srv_t *state) {
    state->dirty = 1;
}

static void set_all(srv_t *state, uint32_t color) {
    for (int i = 0; i < state->numpixels; ++i)
        set(state, i, color);
}

static void anim_set_all(srv_t *state) {
    state->anim_fn = NULL;
    set_all(state, state->color);
    show(state);
}

static void anim_start(srv_t *state, srv_cb_t fn, uint32_t duration) {
    state->anim_fn = fn;
    if (duration == 0)
        state->anim_end = 0;
    else
        state->anim_end = now + duration * 1000;
}
static void anim_finished(srv_t *state) {
    if (state->anim_end == 0)
        state->anim_fn = NULL;
}

static void anim_frame(srv_t *state) {
    if (state->anim_fn) {
        state->anim_fn(state);
        show(state);
        if (state->anim_end && in_past(state->anim_end)) {
            state->anim_fn = NULL;
        }
    }
}

// ---------------------------------------------------------------------------------------
// rainbow
// ---------------------------------------------------------------------------------------

static void rainbow_step(srv_t *state) {
    for (int i = 0; i < state->numpixels; ++i)
        set(state, i,
            hsv(((unsigned)(i * 256) / (state->numpixels - 1) + state->anim_value) & 0xff, 0xff,
                0xff));
    state->anim_value += state->anim_step;
    if (state->anim_value >= 0xff) {
        state->anim_value = 0;
        anim_finished(state);
    }
}
static void anim_rainbow(srv_t *state) {
    state->anim_value = 0;
    state->anim_step = 128U / state->numpixels + 1;
    anim_start(state, rainbow_step, state->duration);
}

// ---------------------------------------------------------------------------------------
// running lights
// ---------------------------------------------------------------------------------------

static void running_lights_step(srv_t *state) {
    if (state->anim_value >= state->numpixels * 2) {
        state->anim_value = 0;
        anim_finished(state);
        return;
    }

    state->anim_value++;
    for (int i = 0; i < state->numpixels; ++i) {
        int level = (state->intensity * ((isin(i + state->anim_value) * 127) + 128)) >> 8;
        px_set(state->pxbuffer, i, level, state->color);
    }
}

static void anim_running_lights(srv_t *state) {
    state->anim_value = 0;
    if (!state->color)
        state->color = 0xff0000;
    anim_start(state, running_lights_step, state->duration);
}

// ---------------------------------------------------------------------------------------
// sparkle
// ---------------------------------------------------------------------------------------

static void sparkle_step(srv_t *state) {
    if (state->anim_value == 0)
        set_all(state, 0);
    state->anim_value++;

    if ((int)state->anim_step < 0) {
        state->anim_step = random_int(state->numpixels - 1);
        set(state, state->anim_step, state->color);
    } else {
        set(state, state->anim_step, 0);
        state->anim_step = -1;
    }

    if (state->anim_value > 50) {
        anim_finished(state);
        state->anim_value = 0;
    }
}

static void anim_sparkle(srv_t *state) {
    state->anim_value = 0;
    state->anim_step = -1;
    if (!state->color)
        state->color = 0xffffff;
    anim_start(state, sparkle_step, state->duration);
}

// ---------------------------------------------------------------------------------------
// color wipe
// ---------------------------------------------------------------------------------------

static void color_wipe_step(srv_t *state) {
    if (state->anim_value < state->numpixels) {
        set(state, state->anim_value, state->anim_flag ? state->color : 0);
        state->anim_value++;
    } else {
        state->anim_flag = !state->anim_flag;
        state->anim_value = 0;
        if (state->anim_flag)
            anim_finished(state);
    }
}

static void anim_color_wipe(srv_t *state) {
    state->anim_value = 0;
    state->anim_flag = 1;
    if (!state->color)
        state->color = 0x0000ff;
    anim_start(state, color_wipe_step, state->duration);
}

// ---------------------------------------------------------------------------------------
// theatre chase
// ---------------------------------------------------------------------------------------
static void theatre_chase_step(srv_t *state) {
    if (state->anim_value < 10) {
        if (state->anim_step < 3) {
            for (int i = 0; i < state->numpixels; i += 3)
                set(state, i + state->anim_step, state->anim_flag ? state->color : 0);
            state->anim_flag = !state->anim_flag;
            state->anim_step++;
        } else {
            state->anim_step = 0;
        }
        state->anim_value++;
    } else {
        state->anim_value = 0;
        anim_finished(state);
    }
}

static void anim_theatre_chase(srv_t *state) {
    state->anim_value = 0;
    state->anim_step = 0;
    state->anim_flag = 0;
    if (!state->color)
        state->color = 0xff0000;
    anim_start(state, theatre_chase_step, state->duration);
}

// ---------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------

static srv_cb_t animations[] = {
    NULL,
    anim_set_all,
    anim_rainbow,
    anim_running_lights,
    anim_color_wipe,
    NULL, // comet
    anim_theatre_chase,
    anim_sparkle,
};

static void tx_done(void) {
    pwr_leave_pll();
    state_->in_tx = 0;
}

void light_process(srv_t *state) {
    // we always check timer to avoid problem with timer overflows
    bool should = should_sample(&state->nextFrame, FRAME_TIME);

    if (!is_enabled(state))
        return;

    if (should)
        anim_frame(state);

    if (state->dirty && !state->in_tx) {
        state->dirty = 0;
        if (is_empty((uint8_t *)state->pxbuffer, PX_WORDS(state->numpixels) * 4)) {
            pwr_pin_enable(0);
            return;
        } else {
            pwr_pin_enable(1);
        }
        state->in_tx = 1;
        pwr_enter_pll();
        px_tx(state->pxbuffer, state->numpixels * 3, tx_done);
    }
}

static void sync_config(srv_t *state) {
    if (!is_enabled(state)) {
        pwr_pin_enable(0);
        return;
    }

    if (!state->inited) {
        state->inited = true;
        px_init();
    }

    int needed = PX_WORDS(state->numpixels);
    if (needed > state->pxbuffer_allocated) {
        state->pxbuffer_allocated = needed;
        state->pxbuffer = alloc(needed * 4);
    }

    pwr_pin_enable(1);
}

static void start_animation(srv_t *state, unsigned anim) {
    if (anim < sizeof(animations) / sizeof(animations[0])) {
        srv_cb_t f = animations[anim];
        if (f) {
            state->anim_step = 0;
            state->anim_flag = 0;
            state->anim_value = 0;
            LOG("start anim %d", anim);
            f(state);
        }
    }
}

static unsigned light_power(srv_t *state) {
    uint8_t *d = (uint8_t *)state->pxbuffer;
    unsigned n = state->numpixels * 3;

    unsigned r = 14000 + 930 * state->numpixels;
    while (n--)
        r += *d++ * 46;

    return r;
}

void light_handle_packet(srv_t *state, jd_packet_t *pkt) {
    switch (pkt->service_command) {
    case LIGHT_CMD_START_ANIMATION:
        sync_config(state);
        if (pkt->service_size > 0)
            start_animation(state, pkt->data[0]);
        break;
    default:
        srv_handle_reg(state, pkt, light_regs);
        break;
    }
}

SRV_DEF(light, JD_SERVICE_CLASS_LIGHT);
void light_init() {
    SRV_ALLOC(light);
    state_ = state; // there is global singleton state
    state->intensity = DEFAULT_INTENSITY;
    state->numpixels = DEFAULT_NUMPIXELS;
    state->maxpower = DEFAULT_MAXPOWER;

#if 0
    state->numpixels = 190;
    state->intensity = 0x0f;
    state->color = 0x00ff00;

    sync_config(state);
    start_animation(state, 2);

    state->intensity = 0x01;
    //    state->numpixels = 3;
    sync_config(state);
    state->intensity = 0x02;
    set_all(state, 0xff0000);
    show(state);
    // pwr_pin_enable(0);
#endif
}
