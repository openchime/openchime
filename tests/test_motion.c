/* Tests for "Reduce motion" (client/gui/app/motion.c, REQ-269): which
 * preference and system value reduce, and that each moving thing -- the jump
 * highlight, the file tray's bar and pulse, the caret -- holds still when
 * reduced and moves as before when not. */
#include "check.h"
#include "motion.h"

int run_motion_tests(void) {
    printf("test_motion: the preference by the system, the flash, the bar, the pulse, the caret\n");

    /* The truth table: three preferences by the system's 1, 0 and -1. */
    CHECK(oc_motion_reduced(OC_MOTION_ON, 1) == 1);
    CHECK(oc_motion_reduced(OC_MOTION_ON, 0) == 1);
    CHECK(oc_motion_reduced(OC_MOTION_ON, -1) == 1);
    CHECK(oc_motion_reduced(OC_MOTION_OFF, 1) == 0);
    CHECK(oc_motion_reduced(OC_MOTION_OFF, 0) == 0);
    CHECK(oc_motion_reduced(OC_MOTION_OFF, -1) == 0);
    CHECK(oc_motion_reduced(OC_MOTION_SYSTEM, 1) == 1);
    CHECK(oc_motion_reduced(OC_MOTION_SYSTEM, 0) == 0);
    CHECK(oc_motion_reduced(OC_MOTION_SYSTEM, -1) == 0);   /* unknown: not reduced */

    /* The jump highlight: constant to its end when reduced, then gone; falling
     * with time when not, from full at its start. */
    const uint64_t start = 100000, until = start + OC_MOTION_FLASH_MS;
    for (uint64_t t = start; t < until; t += 50)
        CHECK(oc_motion_flash_alpha(t, until, 1) == 1.0f);
    CHECK(oc_motion_flash_alpha(until - 1, until, 1) == 1.0f);
    CHECK(oc_motion_flash_alpha(until, until, 1) == 0.0f);
    CHECK(oc_motion_flash_alpha(until + 500, until, 1) == 0.0f);
    CHECK(oc_motion_flash_alpha(start, until, 0) == 1.0f);
    {
        float prev = 2.0f;
        int falls = 1;
        for (uint64_t t = start; t <= until; t += 50) {
            float a = oc_motion_flash_alpha(t, until, 0);
            if (!(a < prev) || a < 0.0f) falls = 0;
            prev = a;
        }
        CHECK(falls);
        CHECK(prev == 0.0f);
    }

    /* The bar: the target in one step when reduced; a part of the way when
     * not, closer each step, and there once close enough. */
    CHECK(oc_motion_ease(0.0f, 0.8f, 1) == 0.8f);
    CHECK(oc_motion_ease(0.9f, 0.1f, 1) == 0.1f);
    {
        float s = 0.0f, d0 = 0.8f;
        s = oc_motion_ease(s, 0.8f, 0);
        CHECK(s > 0.0f && s < 0.8f);
        CHECK(0.8f - s < d0);
        int steps = 0;
        while (s != 0.8f && steps < 200) { float d = 0.8f - s; s = oc_motion_ease(s, 0.8f, 0); CHECK(0.8f - s < d); steps++; }
        CHECK(s == 0.8f);
        CHECK(steps > 1);
    }

    /* The waiting track: a constant when reduced, a pulse when not. */
    {
        int same = 1, varies = 0;
        float r0 = oc_motion_pulse(0, 1), p0 = oc_motion_pulse(0, 0);
        for (uint64_t t = 0; t < 4000; t += 37) {
            if (oc_motion_pulse(t, 1) != r0) same = 0;
            if (oc_motion_pulse(t, 0) != p0) varies = 1;
        }
        CHECK(same);
        CHECK(r0 > 0.0f && r0 <= 1.0f);
        CHECK(varies);
    }

    /* The caret: lit at every time when reduced; when not, lit for the first
     * 530 ms phase, dark for the next, and so on. */
    {
        const uint64_t ref = 5000;
        int always = 1;
        for (uint64_t t = ref; t < ref + 5000; t += 13)
            if (!oc_motion_caret_lit(t, ref, 1)) always = 0;
        CHECK(always);
        CHECK(oc_motion_caret_lit(ref, ref, 0) == 1);
        CHECK(oc_motion_caret_lit(ref + OC_MOTION_CARET_MS - 1, ref, 0) == 1);
        CHECK(oc_motion_caret_lit(ref + OC_MOTION_CARET_MS, ref, 0) == 0);
        CHECK(oc_motion_caret_lit(ref + 2 * OC_MOTION_CARET_MS - 1, ref, 0) == 0);
        CHECK(oc_motion_caret_lit(ref + 2 * OC_MOTION_CARET_MS, ref, 0) == 1);
        CHECK(oc_motion_caret_lit(ref + 3 * OC_MOTION_CARET_MS + 10, ref, 0) == 0);
    }

    return failures;
}
