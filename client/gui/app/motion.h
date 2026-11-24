/*
 * OpenChime GUI -- motion, and the preference that reduces it (REQ-269).
 *
 * The client moves in three places: the highlight on a message you jump to
 * fades out, the file tray's bar pulses, eases and carries a moving light,
 * and the caret blinks. "Reduce motion" (Appearance) stills all three, and by
 * default follows the system's own setting: Windows' "Show animations", a
 * browser's prefers-reduced-motion.
 *
 * Each function here is the arithmetic the painter used to do inline, with
 * the reduced case beside it, so the two can be asserted where they RUN
 * (tests/test_motion.c) rather than by watching frames.
 */
#ifndef OC_GUI_MOTION_H
#define OC_GUI_MOTION_H

#include <stdint.h>

/* The preference's values, in the order its row shows them. */
enum { OC_MOTION_OFF = 0, OC_MOTION_ON = 1, OC_MOTION_SYSTEM = 2, OC_MOTION_PREF_COUNT };

#define OC_MOTION_FLASH_MS 1600u   /* the jump highlight's life */
#define OC_MOTION_CARET_MS 530u    /* one caret phase, lit or dark */
#define OC_MOTION_PULSE_REDUCED 0.35f   /* the waiting track's strength, held */

/* Whether motion is reduced: On reduces, Off does not, and Follow system
 * reduces only when the system says so (`sys` 1); 0 and -1 (unknown) do not.
 * A value outside the three reads as Follow system. */
int   oc_motion_reduced(int pref, int sys);

/* The jump highlight's strength, 0..1, at `now` for one ending at `until`:
 * falling with time, or held at full strength until its end when reduced.
 * 0 from `until` on. */
float oc_motion_flash_alpha(uint64_t now, uint64_t until, int reduced);

/* One frame's step of a bar from `shown` toward `target`: a fraction of the
 * way, snapping when close; the target at once when reduced. */
float oc_motion_ease(float shown, float target, int reduced);

/* The waiting track's strength at `now`: a slow pulse, or a constant when
 * reduced. */
float oc_motion_pulse(uint64_t now, int reduced);

/* Whether the caret is lit at `now`, its phase begun at `ref`: alternate
 * phases of OC_MOTION_CARET_MS, or always when reduced. */
int   oc_motion_caret_lit(uint64_t now, uint64_t ref, int reduced);

#endif /* OC_GUI_MOTION_H */
