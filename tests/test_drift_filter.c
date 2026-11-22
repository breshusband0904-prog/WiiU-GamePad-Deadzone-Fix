/* Host unit tests for src/drift_filter.c (no Wii U hardware involved). */
#include "drift_filter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFailures = 0;
static int gChecks   = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        gChecks++;                                                                   \
        if (!(cond)) {                                                               \
            gFailures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabsf((float) (a) - (float) (b)) <= (eps))

#define BTN_A                 0x00008000u
#define BTN_ZL                0x00000080u
#define BTN_HOME              0x00000002u

/* Same stride as the real VPADStatus (0xAC); the padding is filled with a
 * pattern so tests can prove nothing outside the header is touched. */
#define FAKE_STATUS_SIZE      0xAC
typedef struct FakeStatus {
    DriftSampleHeader h;
    uint8_t rest[FAKE_STATUS_SIZE - sizeof(DriftSampleHeader)];
} FakeStatus;

static void fake_init(FakeStatus *s, uint32_t n) {
    memset(s, 0, sizeof(FakeStatus) * n);
    for (uint32_t i = 0; i < n; i++) memset(s[i].rest, 0xA5, sizeof(s[i].rest));
}

static int padding_intact(const FakeStatus *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        for (size_t b = 0; b < sizeof(s[i].rest); b++)
            if (s[i].rest[b] != 0xA5) return 0;
    return 1;
}

static void set_sample(FakeStatus *s, float x, float y, uint32_t hold, uint32_t trig, uint32_t rel) {
    s->h.x       = x;
    s->h.y       = y;
    s->h.hold    = hold;
    s->h.trigger = trig;
    s->h.release = rel;
}

static DriftParams params(float dz, float outer, float sens) {
    DriftParams p;
    drift_params_default(&p);
    p.deadzone    = dz;
    p.outer       = outer;
    p.sensitivity = sens;
    drift_params_sanitize(&p);
    return p;
}

static float mag(float x, float y) { return sqrtf(x * x + y * y); }

/* -------------------------------------------------------------------------- */

static void test_deadzone_boundaries(void) {
    DriftParams p = params(0.10f, 1.0f, 1.0f);
    float x, y;

    x = 0.0f, y = 0.0f;
    CHECK(drift_filter_stick(&p, &x, &y) == 1 && x == 0.0f && y == 0.0f);

    x = 0.06f, y = 0.08f; /* m = 0.10 exactly at the deadzone -> zero */
    CHECK(drift_filter_stick(&p, &x, &y) == 1 && x == 0.0f && y == 0.0f);

    x = 0.0999f, y = 0.0f;
    CHECK(drift_filter_stick(&p, &x, &y) == 1 && x == 0.0f);

    x = -0.1001f, y = 0.0f; /* just outside: tiny but non-zero, direction kept */
    CHECK(drift_filter_stick(&p, &x, &y) == 0);
    CHECK(x < 0.0f && x > -0.001f && y == 0.0f);

    /* continuity: output magnitude grows linearly from 0 at the edge */
    x = 0.55f, y = 0.0f;
    drift_filter_stick(&p, &x, &y);
    CHECK_NEAR(x, 0.5f, 1e-5f);

    /* outer edge and beyond map to full deflection */
    x = 0.0f, y = 1.0f;
    drift_filter_stick(&p, &x, &y);
    CHECK_NEAR(y, 1.0f, 1e-6f);
    x = 0.0f, y = -1.05f; /* raw sticks can exceed 1.0 */
    drift_filter_stick(&p, &x, &y);
    CHECK_NEAR(y, -1.0f, 1e-6f);

    /* deadzone 0, outer 1: identity inside the unit circle */
    DriftParams id = params(0.0f, 1.0f, 1.0f);
    x = 0.3f, y = -0.4f;
    drift_filter_stick(&id, &x, &y);
    CHECK_NEAR(x, 0.3f, 1e-6f);
    CHECK_NEAR(y, -0.4f, 1e-6f);

    /* deadzone 0.5 */
    DriftParams big = params(0.5f, 1.0f, 1.0f);
    x = 0.5f, y = 0.0f;
    CHECK(drift_filter_stick(&big, &x, &y) == 1);
    x = 0.75f, y = 0.0f;
    drift_filter_stick(&big, &x, &y);
    CHECK_NEAR(x, 0.5f, 1e-5f);
}

static void test_outer_and_sensitivity(void) {
    DriftParams p = params(0.10f, 0.80f, 1.0f);
    float x = 0.80f, y = 0.0f;
    drift_filter_stick(&p, &x, &y);
    CHECK_NEAR(x, 1.0f, 1e-6f);

    /* outer <= deadzone is sanitized to deadzone + 0.05 */
    DriftParams bad = params(0.40f, 0.30f, 1.0f);
    CHECK_NEAR(bad.outer, 0.45f, 1e-6f);
    x = 0.45f, y = 0.0f;
    drift_filter_stick(&bad, &x, &y);
    CHECK_NEAR(x, 1.0f, 1e-5f);

    /* sensitivity never pushes the output outside the unit circle */
    const float sens[] = {0.5f, 1.0f, 2.0f, 4.0f};
    for (int s = 0; s < 4; s++) {
        DriftParams q = params(0.10f, 1.0f, sens[s]);
        for (int a = 0; a < 360; a += 7) {
            for (float r = 0.0f; r <= 1.1f; r += 0.05f) {
                float fx = r * cosf((float) a * 3.14159265f / 180.0f);
                float fy = r * sinf((float) a * 3.14159265f / 180.0f);
                drift_filter_stick(&q, &fx, &fy);
                CHECK(mag(fx, fy) <= 1.0f + 1e-5f);
            }
        }
    }
    DriftParams half = params(0.0f, 1.0f, 0.5f);
    x = 0.8f, y = 0.0f;
    drift_filter_stick(&half, &x, &y);
    CHECK_NEAR(x, 0.4f, 1e-6f);
}

static void test_direction_preserved(void) {
    DriftParams p = params(0.15f, 0.9f, 1.3f);
    for (int a = 0; a < 360; a += 5) {
        const float rx = 0.6f * cosf((float) a * 3.14159265f / 180.0f);
        const float ry = 0.6f * sinf((float) a * 3.14159265f / 180.0f);
        float x = rx, y = ry;
        drift_filter_stick(&p, &x, &y);
        /* cross product ~0 and same half-plane => same direction */
        CHECK(fabsf(rx * y - ry * x) < 1e-5f);
        CHECK(rx * x + ry * y > 0.0f);
    }
}

static void test_offset(void) {
    DriftParams p   = params(0.05f, 1.0f, 1.0f);
    p.offsetEnabled = 1;
    p.offsetX       = 0.20f;
    p.offsetY       = -0.10f;
    drift_params_sanitize(&p);

    float x = 0.20f, y = -0.10f; /* exactly the measured rest position */
    CHECK(drift_filter_stick(&p, &x, &y) == 1 && x == 0.0f && y == 0.0f);

    x = 0.22f, y = -0.11f; /* near rest */
    CHECK(drift_filter_stick(&p, &x, &y) == 1);

    p.offsetEnabled = 0; /* offset ignored when disabled */
    x = 0.20f, y = -0.10f;
    CHECK(drift_filter_stick(&p, &x, &y) == 0 && x > 0.0f);

    DriftParams c   = params(0.1f, 1.0f, 1.0f);
    c.offsetEnabled = 1;
    c.offsetX       = 0.9f;
    c.offsetY       = -0.9f;
    drift_params_sanitize(&c);
    CHECK_NEAR(c.offsetX, 0.5f, 1e-6f);
    CHECK_NEAR(c.offsetY, -0.5f, 1e-6f);

    DriftParams e   = params(0.1f, 1.0f, 1.0f);
    e.offsetEnabled = 1;
    e.offsetX       = 0.25f;
    e.offsetY       = -0.25f;
    drift_params_sanitize(&e);
    x = 0.25f, y = -0.25f;
    CHECK(drift_filter_stick(&e, &x, &y) == 1);
}

static void test_non_finite(void) {
    DriftParams p = params(0.1f, 1.0f, 1.0f);
    float x = NAN, y = 0.5f;
    CHECK(drift_filter_stick(&p, &x, &y) == 0);
    CHECK(isnan(x) && y == 0.5f);
    x = INFINITY, y = 0.0f;
    drift_filter_stick(&p, &x, &y);
    CHECK(isinf(x));
    CHECK(drift_emu_keep_mask(NAN, 0.0f) == DRIFT_EMU_MASK);

    DriftParams bad;
    bad.deadzone      = NAN;
    bad.outer         = INFINITY;
    bad.sensitivity   = -INFINITY;
    bad.offsetX       = NAN;
    bad.offsetY       = NAN;
    bad.offsetEnabled = 7;
    drift_params_sanitize(&bad);
    CHECK_NEAR(bad.deadzone, 0.10f, 1e-6f);
    CHECK_NEAR(bad.outer, 1.0f, 1e-6f);
    CHECK_NEAR(bad.sensitivity, 1.0f, 1e-6f);
    CHECK(bad.offsetX == 0.0f && bad.offsetY == 0.0f && bad.offsetEnabled == 1);
}

static uint32_t keep_at(float deg, float r) {
    return drift_emu_keep_mask(r * cosf(deg * 3.14159265f / 180.0f), r * sinf(deg * 3.14159265f / 180.0f));
}

static void test_keep_mask(void) {
    CHECK(drift_emu_keep_mask(0.0f, 0.0f) == 0);
    CHECK(drift_emu_keep_mask(-0.2499f, 0.0f) == 0);
    CHECK(drift_emu_keep_mask(-0.25f, 0.0f) == DRIFT_EMU_LEFT);
    CHECK(drift_emu_keep_mask(1.0f, 0.0f) == DRIFT_EMU_RIGHT);
    CHECK(drift_emu_keep_mask(0.0f, 1.0f) == DRIFT_EMU_UP);
    CHECK(drift_emu_keep_mask(0.0f, -1.0f) == DRIFT_EMU_DOWN);
    CHECK(keep_at(45.0f, 1.0f) == (DRIFT_EMU_RIGHT | DRIFT_EMU_UP));
    CHECK(keep_at(135.0f, 1.0f) == (DRIFT_EMU_LEFT | DRIFT_EMU_UP));
    CHECK(keep_at(225.0f, 1.0f) == (DRIFT_EMU_LEFT | DRIFT_EMU_DOWN));
    CHECK(keep_at(315.0f, 1.0f) == (DRIFT_EMU_RIGHT | DRIFT_EMU_DOWN));
    CHECK(keep_at(67.0f, 1.0f) == (DRIFT_EMU_RIGHT | DRIFT_EMU_UP));
    CHECK(keep_at(68.0f, 1.0f) == DRIFT_EMU_UP);
    CHECK(keep_at(22.0f, 1.0f) == DRIFT_EMU_RIGHT);
    CHECK(keep_at(23.0f, 1.0f) == (DRIFT_EMU_RIGHT | DRIFT_EMU_UP));
    CHECK(keep_at(90.0f, 0.3f) == DRIFT_EMU_UP);
    CHECK(keep_at(180.0f, 0.24f) == 0);
}

static void test_should_process(void) {
    int buf[4];
    CHECK(drift_should_process(1, 0, 0, buf, 4, 0) == 1);
    CHECK(drift_should_process(3, 1, 0, buf, 4, 1) == 3);
    CHECK(drift_should_process(9, 1, 0, buf, 4, 0) == 4); /* bounded by buffer */
    CHECK(drift_should_process(0, 1, 0, buf, 4, 0) == 0);
    CHECK(drift_should_process(-1, 1, -1, buf, 4, 0) == 0);
    CHECK(drift_should_process(1, 1, -2, buf, 4, 0) == 0); /* error reported */
    CHECK(drift_should_process(1, 0, -2, buf, 4, 0) == 1); /* no error pointer */
    CHECK(drift_should_process(1, 1, 0, NULL, 4, 0) == 0);
    CHECK(drift_should_process(1, 1, 0, buf, 0, 0) == 0);
    CHECK(drift_should_process(1, 1, 0, buf, 4, 2) == 0);
    CHECK(drift_should_process(1, 1, 0, buf, 4, -1) == 0);
}

/* One LOOSE call with a single sample; returns delivered header. */
static DriftSampleHeader loose1(DriftChannelState *st, const DriftParams *p, int enabled,
                                float x, float y, uint32_t hold, uint32_t trig, uint32_t rel) {
    FakeStatus s;
    fake_init(&s, 1);
    set_sample(&s, x, y, hold, trig, rel);
    drift_process(st, &s, sizeof(FakeStatus), 1, 0, enabled, p);
    CHECK(padding_intact(&s, 1));
    return s.h;
}

/* User-requested case: the system still reports an emulated direction while
 * the filtered analog movement is extremely small. */
static void test_emu_bit_with_tiny_filtered_motion(void) {
    DriftParams p = params(0.45f, 1.0f, 1.0f);
    DriftChannelState st;
    drift_state_reset(&st);

    /* raw 0.46 just outside a 0.45 deadzone -> filtered ~0.018, system says LEFT */
    float fx = -0.46f, fy = 0.0f;
    CHECK(drift_filter_stick(&p, &fx, &fy) == 0);
    CHECK(fx < 0.0f && fx > -0.02f);

    DriftSampleHeader d = loose1(&st, &p, 1, -0.46f, 0.0f,
                                 DRIFT_EMU_LEFT | BTN_A, DRIFT_EMU_LEFT, 0);
    CHECK(d.x < 0.0f && d.x > -0.02f); /* analog movement is kept (tiny) */
    CHECK((d.hold & DRIFT_EMU_MASK) == 0);
    CHECK((d.trigger & DRIFT_EMU_MASK) == 0);
    CHECK((d.hold & BTN_A) == BTN_A); /* other buttons untouched */

    /* Same in TIGHT mode, with the tiny sample sandwiched between real pushes */
    DriftChannelState t;
    drift_state_reset(&t);
    FakeStatus s[3];
    fake_init(s, 3);
    set_sample(&s[2], -1.0f, 0.0f, DRIFT_EMU_LEFT, DRIFT_EMU_LEFT, 0); /* oldest: real push */
    set_sample(&s[1], -0.46f, 0.0f, DRIFT_EMU_LEFT, 0, 0);             /* tiny filtered */
    set_sample(&s[0], -1.0f, 0.0f, DRIFT_EMU_LEFT, 0, 0);              /* newest: real push */
    drift_process(&t, s, sizeof(FakeStatus), 3, 1, 1, &p);
    CHECK((s[2].h.hold & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK((s[1].h.hold & DRIFT_EMU_MASK) == 0);
    CHECK((s[1].h.release & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK((s[0].h.hold & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK((s[0].h.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);

    /* At exactly the minimum magnitude the bit is kept. */
    DriftParams id = params(0.0f, 1.0f, 1.0f);
    DriftChannelState u;
    drift_state_reset(&u);
    d = loose1(&u, &id, 1, -0.25f, 0.0f, DRIFT_EMU_LEFT, DRIFT_EMU_LEFT, 0);
    CHECK((d.hold & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK((d.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
}

static void test_drift_then_real_push(void) {
    DriftParams p = params(0.20f, 1.0f, 1.0f);
    DriftChannelState st;
    drift_state_reset(&st);
    DriftSampleHeader d;

    /* drift at -0.18 (inside the deadzone); system already holds LEFT */
    d = loose1(&st, &p, 1, -0.18f, 0.02f, DRIFT_EMU_LEFT, DRIFT_EMU_LEFT, 0);
    CHECK(d.x == 0.0f && d.y == 0.0f);
    CHECK((d.hold & DRIFT_EMU_MASK) == 0 && (d.trigger & DRIFT_EMU_MASK) == 0);
    for (int i = 0; i < 5; i++) {
        d = loose1(&st, &p, 1, -0.18f, 0.02f, DRIFT_EMU_LEFT, 0, 0);
        CHECK((d.hold | d.trigger | d.release) == 0);
    }
    /* real push left: system never re-triggers (bit already held) but the
     * game must see a press edge */
    d = loose1(&st, &p, 1, -0.9f, 0.0f, DRIFT_EMU_LEFT, 0, 0);
    CHECK((d.hold & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK((d.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    d = loose1(&st, &p, 1, -0.9f, 0.0f, DRIFT_EMU_LEFT, 0, 0);
    CHECK((d.trigger & DRIFT_EMU_MASK) == 0 && (d.release & DRIFT_EMU_MASK) == 0);
    /* back to drift: the system still holds LEFT, but the game gets a release */
    d = loose1(&st, &p, 1, -0.18f, 0.0f, DRIFT_EMU_LEFT, 0, 0);
    CHECK((d.hold & DRIFT_EMU_MASK) == 0);
    CHECK((d.release & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    /* full release: system releases LEFT, game already got its release */
    d = loose1(&st, &p, 1, 0.0f, 0.0f, 0, 0, DRIFT_EMU_LEFT);
    CHECK((d.release & DRIFT_EMU_MASK) == 0);
}

static void test_enable_disable_transitions(void) {
    DriftParams p = params(0.20f, 1.0f, 1.0f);
    DriftChannelState st;
    drift_state_reset(&st);
    DriftSampleHeader d;

    /* fresh + disabled = bit-identical pass-through */
    FakeStatus a, b;
    fake_init(&a, 1);
    set_sample(&a, -0.18f, 0.03f, DRIFT_EMU_LEFT | BTN_ZL, DRIFT_EMU_LEFT | BTN_ZL, BTN_A);
    b = a;
    drift_process(&st, &a, sizeof(FakeStatus), 1, 0, 0, &p);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);

    /* enable while the system holds a drift bit that the game has seen */
    d = loose1(&st, &p, 1, -0.18f, 0.03f, DRIFT_EMU_LEFT, 0, 0);
    CHECK((d.hold & DRIFT_EMU_MASK) == 0);
    CHECK((d.release & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT); /* no orphaned held bit */

    /* disable again: system still holds LEFT -> first disabled call must
     * deliver a trigger, since the game currently believes it is released */
    d = loose1(&st, &p, 0, -0.18f, 0.03f, DRIFT_EMU_LEFT, 0, 0);
    CHECK((d.hold & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK((d.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK_NEAR(d.x, -0.18f, 1e-7f); /* analog untouched while disabled */

    /* subsequent disabled calls are bit-identical again */
    fake_init(&a, 1);
    set_sample(&a, -0.18f, 0.03f, DRIFT_EMU_LEFT, 0, 0);
    b = a;
    drift_process(&st, &a, sizeof(FakeStatus), 1, 0, 0, &p);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}

static void test_unsynced_trusts_system_edges(void) {
    DriftParams p = params(0.20f, 1.0f, 1.0f);
    DriftChannelState st;
    drift_state_reset(&st);
    DriftSampleHeader d;

    /* strong push with system trigger -> trigger kept */
    d = loose1(&st, &p, 1, 0.9f, 0.0f, DRIFT_EMU_RIGHT, DRIFT_EMU_RIGHT, 0);
    CHECK((d.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_RIGHT);

    /* held without trigger right after reset -> no phantom trigger */
    drift_state_reset(&st);
    d = loose1(&st, &p, 1, 0.9f, 0.0f, DRIFT_EMU_RIGHT, 0, 0);
    CHECK((d.hold & DRIFT_EMU_MASK) == DRIFT_EMU_RIGHT);
    CHECK((d.trigger & DRIFT_EMU_MASK) == 0);

    /* system release of a bit that is not held is passed on */
    drift_state_reset(&st);
    d = loose1(&st, &p, 1, 0.0f, 0.0f, 0, 0, DRIFT_EMU_UP);
    CHECK((d.release & DRIFT_EMU_MASK) == DRIFT_EMU_UP);
}

static void test_other_bits_untouched(void) {
    DriftParams p   = params(0.30f, 0.9f, 1.5f);
    p.offsetEnabled = 1;
    p.offsetX       = 0.1f;
    drift_params_sanitize(&p);
    DriftChannelState st;
    drift_state_reset(&st);
    srand(1234);
    for (int i = 0; i < 2000; i++) {
        const uint32_t other = ((uint32_t) rand() << 16 ^ (uint32_t) rand()) & ~DRIFT_EMU_MASK;
        const uint32_t oth2  = ((uint32_t) rand() << 16 ^ (uint32_t) rand()) & ~DRIFT_EMU_MASK;
        const uint32_t oth3  = ((uint32_t) rand() << 16 ^ (uint32_t) rand()) & ~DRIFT_EMU_MASK;
        const uint32_t emu   = ((uint32_t) rand()) & DRIFT_EMU_MASK;
        const float x        = ((float) rand() / (float) RAND_MAX) * 2.2f - 1.1f;
        const float y        = ((float) rand() / (float) RAND_MAX) * 2.2f - 1.1f;
        const int tight      = rand() & 1;
        const int enabled    = (rand() % 4) != 0;
        FakeStatus s;
        fake_init(&s, 1);
        set_sample(&s, x, y, other | emu, oth2 | emu, oth3);
        drift_process(&st, &s, sizeof(FakeStatus), 1, tight, enabled, &p);
        CHECK((s.h.hold & ~DRIFT_EMU_MASK) == other);
        CHECK((s.h.trigger & ~DRIFT_EMU_MASK) == oth2);
        CHECK((s.h.release & ~DRIFT_EMU_MASK) == oth3);
        CHECK(padding_intact(&s, 1));
    }
}

/* Randomized invariant: delivered digital directions never contradict the
 * delivered analog value and are never added beyond what the system set. */
static void test_consistency_invariant(void) {
    srand(42);
    for (int i = 0; i < 20000; i++) {
        DriftParams p   = params(((float) (rand() % 51)) / 100.0f,
                                 ((float) (50 + rand() % 51)) / 100.0f,
                                 ((float) (50 + rand() % 151)) / 100.0f);
        p.offsetEnabled = rand() & 1;
        p.offsetX       = ((float) (rand() % 51) - 25.0f) / 100.0f;
        p.offsetY       = ((float) (rand() % 51) - 25.0f) / 100.0f;
        drift_params_sanitize(&p);

        DriftChannelState st;
        drift_state_reset(&st);
        st.synced      = rand() & 1;
        st.prevEmuHold = (uint32_t) rand() & DRIFT_EMU_MASK;
        st.lastEnabled = rand() & 1;

        const uint32_t n = 1 + (uint32_t) (rand() % 8);
        const int tight  = rand() & 1;
        FakeStatus s[8];
        uint32_t sysEmu[8];
        fake_init(s, n);
        for (uint32_t k = 0; k < n; k++) {
            sysEmu[k] = (uint32_t) rand() & DRIFT_EMU_MASK;
            set_sample(&s[k], ((float) rand() / (float) RAND_MAX) * 2.2f - 1.1f,
                       ((float) rand() / (float) RAND_MAX) * 2.2f - 1.1f,
                       sysEmu[k], (uint32_t) rand() & DRIFT_EMU_MASK, (uint32_t) rand() & DRIFT_EMU_MASK);
        }
        drift_process(&st, s, sizeof(FakeStatus), n, tight, 1, &p);

        for (uint32_t k = 0; k < n; k++) {
            const uint32_t emu = s[k].h.hold & DRIFT_EMU_MASK;
            /* trigger/release are only ever reported for consistent bits */
            CHECK((s[k].h.trigger & DRIFT_EMU_MASK & ~emu) == 0);
            CHECK((s[k].h.release & DRIFT_EMU_MASK & emu) == 0);
            CHECK(mag(s[k].h.x, s[k].h.y) <= 1.0f + 1e-5f);
            if (tight) {
                CHECK((emu & ~sysEmu[k]) == 0);
                CHECK((emu & ~drift_emu_keep_mask(s[k].h.x, s[k].h.y)) == 0);
            } else {
                CHECK((emu & ~sysEmu[0]) == 0);
                CHECK((emu & ~drift_emu_keep_mask(s[0].h.x, s[0].h.y)) == 0);
                CHECK(s[k].h.hold == s[0].h.hold);
            }
        }
        if (s[0].h.x == 0.0f && s[0].h.y == 0.0f) CHECK((s[0].h.hold & DRIFT_EMU_MASK) == 0);
    }
}

static void test_loose_multi_sample(void) {
    const uint32_t counts[] = {1, 2, 16, 64};
    DriftParams p           = params(0.20f, 1.0f, 1.0f);
    for (int c = 0; c < 4; c++) {
        const uint32_t n = counts[c];
        FakeStatus *s    = (FakeStatus *) malloc(sizeof(FakeStatus) * n);
        DriftChannelState st;
        drift_state_reset(&st);
        st.synced = 1; /* previous call delivered nothing */

        fake_init(s, n);
        for (uint32_t k = 0; k < n; k++) {
            /* LOOSE: same buttons for all samples; analog differs per sample */
            set_sample(&s[k], -0.05f - 0.9f * (float) k / (float) (n > 1 ? n - 1 : 1), 0.0f,
                       DRIFT_EMU_LEFT | BTN_A, DRIFT_EMU_LEFT | BTN_A, 0);
        }
        drift_process(&st, s, sizeof(FakeStatus), n, 0, 1, &p);
        /* newest sample (-0.05) is inside the deadzone: LEFT suppressed everywhere */
        for (uint32_t k = 0; k < n; k++) {
            CHECK(s[k].h.hold == s[0].h.hold);
            CHECK(s[k].h.trigger == s[0].h.trigger);
            CHECK(s[k].h.release == s[0].h.release);
        }
        CHECK((s[0].h.hold & DRIFT_EMU_MASK) == 0);
        CHECK((s[0].h.hold & BTN_A) == BTN_A && (s[0].h.trigger & BTN_A) == BTN_A);
        CHECK(s[0].h.x == 0.0f);
        if (n > 1) CHECK_NEAR(s[n - 1].h.x, -(0.95f - 0.2f) / 0.8f, 1e-5f); /* per-sample analog */
        CHECK(padding_intact(s, n));

        /* next call: newest is a real push -> one trigger on every sample */
        fake_init(s, n);
        for (uint32_t k = 0; k < n; k++) set_sample(&s[k], -1.0f, 0.0f, DRIFT_EMU_LEFT, 0, 0);
        drift_process(&st, s, sizeof(FakeStatus), n, 0, 1, &p);
        for (uint32_t k = 0; k < n; k++) {
            CHECK((s[k].h.hold & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
            CHECK((s[k].h.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
        }
        free(s);
    }
}

static void test_tight_multi_sample(void) {
    DriftParams p = params(0.20f, 1.0f, 1.0f);
    DriftChannelState st;
    drift_state_reset(&st);
    st.synced = 1;

    /* index 0 = newest. Press in the middle sample. */
    FakeStatus s[3];
    fake_init(s, 3);
    set_sample(&s[2], 0.0f, 0.0f, 0, 0, 0);
    set_sample(&s[1], 0.0f, 0.95f, DRIFT_EMU_UP, DRIFT_EMU_UP, 0);
    set_sample(&s[0], 0.0f, 0.95f, DRIFT_EMU_UP, 0, 0);
    drift_process(&st, s, sizeof(FakeStatus), 3, 1, 1, &p);
    CHECK((s[2].h.trigger & DRIFT_EMU_MASK) == 0);
    CHECK((s[1].h.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_UP);
    CHECK((s[0].h.trigger & DRIFT_EMU_MASK) == 0);
    CHECK((s[0].h.hold & DRIFT_EMU_MASK) == DRIFT_EMU_UP);
    CHECK(st.prevEmuHold == DRIFT_EMU_UP);

    /* Next call: release happens in the oldest sample; chain carried over */
    fake_init(s, 3);
    set_sample(&s[2], 0.0f, 0.0f, 0, 0, DRIFT_EMU_UP);
    set_sample(&s[1], 0.0f, 0.0f, 0, 0, 0);
    set_sample(&s[0], 0.0f, 0.0f, 0, 0, 0);
    drift_process(&st, s, sizeof(FakeStatus), 3, 1, 1, &p);
    CHECK((s[2].h.release & DRIFT_EMU_MASK) == DRIFT_EMU_UP);
    CHECK((s[1].h.release & DRIFT_EMU_MASK) == 0);
    CHECK((s[0].h.release & DRIFT_EMU_MASK) == 0);

    /* Drift sample inside the deadzone in the middle of a held direction */
    fake_init(s, 3);
    set_sample(&s[2], 0.9f, 0.0f, DRIFT_EMU_RIGHT, DRIFT_EMU_RIGHT, 0);
    set_sample(&s[1], 0.15f, 0.0f, DRIFT_EMU_RIGHT, 0, 0);
    set_sample(&s[0], 0.9f, 0.0f, DRIFT_EMU_RIGHT, 0, 0);
    drift_process(&st, s, sizeof(FakeStatus), 3, 1, 1, &p);
    CHECK((s[2].h.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_RIGHT);
    CHECK((s[1].h.hold & DRIFT_EMU_MASK) == 0 && (s[1].h.release & DRIFT_EMU_MASK) == DRIFT_EMU_RIGHT);
    CHECK((s[0].h.trigger & DRIFT_EMU_MASK) == DRIFT_EMU_RIGHT);
}

static void test_stateless_fallback(void) {
    DriftParams p = params(0.20f, 1.0f, 1.0f);
    FakeStatus s[2], ref[2];
    fake_init(s, 2);
    set_sample(&s[0], -0.1f, 0.0f, DRIFT_EMU_LEFT | BTN_A, DRIFT_EMU_LEFT, DRIFT_EMU_UP | BTN_HOME);
    set_sample(&s[1], -0.9f, 0.0f, DRIFT_EMU_LEFT, DRIFT_EMU_LEFT, 0);
    memcpy(ref, s, sizeof(s));

    drift_process_stateless(s, sizeof(FakeStatus), 2, 0, &p); /* disabled: no-op */
    CHECK(memcmp(s, ref, sizeof(s)) == 0);

    drift_process_stateless(s, sizeof(FakeStatus), 2, 1, &p);
    CHECK(s[0].h.x == 0.0f);
    CHECK(s[0].h.hold == BTN_A && (s[0].h.trigger & DRIFT_EMU_MASK) == 0);
    CHECK(s[0].h.release == (DRIFT_EMU_UP | BTN_HOME));
    CHECK((s[1].h.hold & DRIFT_EMU_MASK) == DRIFT_EMU_LEFT);
    CHECK(padding_intact(s, 2));
}

int main(void) {
    test_deadzone_boundaries();
    test_outer_and_sensitivity();
    test_direction_preserved();
    test_offset();
    test_non_finite();
    test_keep_mask();
    test_should_process();
    test_emu_bit_with_tiny_filtered_motion();
    test_drift_then_real_push();
    test_enable_disable_transitions();
    test_unsynced_trusts_system_edges();
    test_other_bits_untouched();
    test_consistency_invariant();
    test_loose_multi_sample();
    test_tight_multi_sample();
    test_stateless_fallback();

    printf("test_drift_filter: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
