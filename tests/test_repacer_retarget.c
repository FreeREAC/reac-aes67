// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

// Unit check for the re-pacer's glitch-free occupancy retarget math (the core of
// live reconfiguration in tools/reac_repacer.c apply_hot_params). The scheduling
// formula is mirrored here so the relation between a depth change and the
// clock-bias walk stays pinned: grow holds the output back (negative bias), shrink
// runs it ahead (positive bias), the bias never exceeds the inaudible cap, and
// simulating the biased walk lands the occupancy on the new target. Standalone --
// no daemon symbols, so it links against the core like the other tests.

#include <math.h>
#include "test_util.h"

// keep in sync with tools/reac_repacer.c
#define RETARGET_MS     400.0
#define RETARGET_MAXPPM 2000.0

// mirror of the ramp scheduling in apply_hot_params(): ppm bias + tick count to
// walk occupancy from old_pf to new_pf slots without flushing the ring.
static void schedule(long period_ns, int old_pf, int new_pf, double *ppm_out, int *ticks_out)
{
	int delta = new_pf - old_pf;
	if (delta == 0) { *ppm_out = 0; *ticks_out = 0; return; }
	double fps = 1.0e9 / (double)period_ns;
	double ticks = RETARGET_MS * 1.0e-3 * fps; if (ticks < 1.0) ticks = 1.0;
	double ppm = -(double)delta * 1.0e6 / ticks;
	if (ppm > RETARGET_MAXPPM || ppm < -RETARGET_MAXPPM) {
		double cap = RETARGET_MAXPPM;
		ticks = (double)(delta < 0 ? -delta : delta) * 1.0e6 / cap;
		ppm = (delta > 0) ? -cap : cap;
	}
	*ppm_out = ppm; *ticks_out = (int)(ticks + 0.5);
}

// each biased tick moves occupancy by -ppm*1e-6 slots (output ahead at +ppm = drain)
static double simulate(int start_occ, double ppm, int ticks)
{
	double occ = start_occ;
	for (int t = 0; t < ticks; t++) occ += -ppm * 1e-6;
	return occ;
}

#define P96K 125000L   // ~8000 fps (96 kHz REAC)

static int test_grow_holds_back_and_reaches_target(void)
{
	double ppm; int ticks;
	schedule(P96K, 64, 1200, &ppm, &ticks);
	ASSERT(ppm < 0.0);                                  // grow = hold output back
	ASSERT(fabs(ppm) <= RETARGET_MAXPPM + 1e-6);        // bias within the inaudible cap
	ASSERT(fabs(simulate(64, ppm, ticks) - 1200.0) < 2.0);
	return 0;
}

static int test_shrink_runs_ahead_and_reaches_target(void)
{
	double ppm; int ticks;
	schedule(P96K, 1200, 64, &ppm, &ticks);
	ASSERT(ppm > 0.0);                                  // shrink = run output ahead
	ASSERT(fabs(ppm) <= RETARGET_MAXPPM + 1e-6);
	ASSERT(fabs(simulate(1200, ppm, ticks) - 64.0) < 2.0);
	return 0;
}

static int test_small_nudge_is_inaudible(void)
{
	double ppm; int ticks;
	schedule(P96K, 100, 102, &ppm, &ticks);            // +2 slots = a by-ear nudge
	ASSERT(fabs(ppm) < RETARGET_MAXPPM);
	ASSERT(fabs(ppm) < 700.0);                          // within the ~1-cent inaudible band
	ASSERT(fabs(simulate(100, ppm, ticks) - 102.0) < 1.0);
	return 0;
}

static int test_no_change_arms_no_walk(void)
{
	double ppm; int ticks;
	schedule(P96K, 200, 200, &ppm, &ticks);
	ASSERT(ppm == 0.0);
	ASSERT_EQ_INT(ticks, 0);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_grow_holds_back_and_reaches_target);
	rc |= RUN(test_shrink_runs_ahead_and_reaches_target);
	rc |= RUN(test_small_nudge_is_inaudible);
	rc |= RUN(test_no_change_arms_no_walk);
	return rc;
}
