/*
 * OpenChime GUI -- motion, and the preference that reduces it (motion.h).
 * Pure arithmetic: no platform, no window, no clock of its own.
 */
#include "motion.h"

#include <math.h>

int oc_motion_reduced(int pref, int sys) {
    if (pref == OC_MOTION_ON)  return 1;
    if (pref == OC_MOTION_OFF) return 0;
    return sys == 1;
}

float oc_motion_flash_alpha(uint64_t now, uint64_t until, int reduced) {
    if (now >= until) return 0.0f;
    if (reduced) return 1.0f;
    float a = (float)(until - now) / (float)OC_MOTION_FLASH_MS;
    return a > 1.0f ? 1.0f : a;
}

float oc_motion_ease(float shown, float target, int reduced) {
    if (reduced) return target;
    float d = target - shown;
    return (d > -0.002f && d < 0.002f) ? target : shown + d * 0.22f;
}

float oc_motion_pulse(uint64_t now, int reduced) {
    if (reduced) return OC_MOTION_PULSE_REDUCED;
    return 0.35f + 0.25f * (float)sin((double)now / 300.0);
}

int oc_motion_caret_lit(uint64_t now, uint64_t ref, int reduced) {
    if (reduced) return 1;
    return ((now - ref) / OC_MOTION_CARET_MS) % 2 == 0;
}
