#ifndef SM_TIMEUTIL_H
#define SM_TIMEUTIL_H

/* Current time in seconds (with sub-second fraction). */
double sm_now_realtime(void);   /* CLOCK_REALTIME  — wall clock, user-facing timestamps */
double sm_now_monotonic(void);  /* CLOCK_MONOTONIC — durations/deadlines, NTP-immune */

#endif /* SM_TIMEUTIL_H */
