/*
 * OpenChime client — platform capability switchboard (openblocks pattern).
 * Phase 1 targets Linux desktop; platform -D flags will select features
 * (touch, mobile safe-area, alternate gfx backend) in the cross-platform phase.
 */
#ifndef OC_PLATFORM_H
#define OC_PLATFORM_H

#if !defined(OC_PLATFORM_DESKTOP) && !defined(OC_PLATFORM_MOBILE)
#define OC_PLATFORM_DESKTOP 1
#endif

#endif /* OC_PLATFORM_H */
