// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

// Unit checks for the re-pacer v2 steady free-running output clock and the PLL
// warm-start state file (tools/reac_repacer.c, design §3/§3b). Two properties are
// pinned here, both standalone (no daemon symbols -- links the core like the other
// tests):
//
//   1. STEADY CLOCK. The output cadence is the audio (the stagebox recovers its word
//      clock from the emit inter-frame interval). The fix is to pace the output from a
//      FREE-RUNNING timer at one period and absorb input jitter as ring occupancy, never
//      letting it reach the emit interval. We mirror both the v1 per-tick drain servo
//      and the v2 steady emitter, drive them with the SAME jittery arrival series, and
//      assert the steady output's inter-frame-interval sd is tiny and independent of the
//      input jitter, while the servo's is not. (Occupancy stays bounded -> no overflow.)
//
//   2. WARM-START FILE. The per-port lock file round-trips: a saved period is restored
//      only for a matching out_iface AND matching rate label, and a missing/foreign/stale
//      record yields a cold start (0). The parser is mirrored from lock_load/lock_save.

#define _POSIX_C_SOURCE 200809L  /* mkstemp / fdopen / unlink under -std=c11 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_util.h"

#define P96K   125000.0   // nominal emit period (ns), ~8000 fps (96 kHz REAC)
#define NTICKS 200000     // ~25 s @ 8000 fps

// deterministic LCG so the test is reproducible without <random>
static unsigned long s_rng = 0x9e3779b97f4a7c15UL;
static double urand(void) { s_rng = s_rng * 6364136223846793005UL + 1442695040888963407UL; return (double)(s_rng >> 33) / (double)(1UL << 31); }

// build a jittery arrival series: frames are PRODUCED at the true rate but DELIVERED
// in bursts (WDS): a frame's arrival time is the ideal time plus a large random
// jitter (and occasional clumping), modelling the bursty Wi-Fi the ring must absorb.
static void make_arrivals(double *arr, int n, double period, double jitter_ns)
{
	double ideal = 0;
	for (int i = 0; i < n; i++) {
		ideal += period;
		double j = (urand() - 0.5) * 2.0 * jitter_ns;        // +/- jitter_ns
		arr[i] = ideal + j;
		if (i && arr[i] < arr[i - 1]) arr[i] = arr[i - 1];   // arrivals are monotone (a burst clumps)
	}
}

static double stdev(const double *x, int n)
{
	if (n < 2) return 0;
	double m = 0; for (int i = 0; i < n; i++) m += x[i]; m /= n;
	double v = 0; for (int i = 0; i < n; i++) { double d = x[i] - m; v += d * d; }
	return sqrt(v / (n - 1));
}

// -------- model A: v1 per-tick drain servo (occupancy chases target every tick) --------
// emit period is nudged each tick toward holding occupancy at a setpoint; this is exactly
// the behaviour that leaks input jitter into the output cadence (the "rubbery" artifact).
static double servo_ifi_sd(const double *arr, int n, double period, int prefill)
{
	double *ifi = malloc(sizeof(double) * n);
	int produced = 0, emitted = 0; double last_emit = -1; double emit_period = period;
	double dl = arr[0] + prefill * period;                  // first emit one buffer-depth after first arrival
	for (int k = 0; k < n - prefill - 1; k++) {
		while (produced < n && arr[produced] <= dl) produced++;
		int occ = produced - emitted;
		// fast servo: pull occ toward prefill by nudging the NEXT emit period (per tick).
		// This per-tick rate nudge is exactly what leaks input jitter into the cadence.
		double err = (double)(occ - prefill);
		double bias_ppm = 12.0 * err;                       // proportional, like SERVO_KP
		if (bias_ppm >  6000.0) bias_ppm =  6000.0;
		if (bias_ppm < -6000.0) bias_ppm = -6000.0;
		emit_period = period * (1.0 - bias_ppm * 1e-6);     // +bias = faster = drain
		if (last_emit >= 0) ifi[emitted] = dl - last_emit;  // the OUTPUT interval
		last_emit = dl;
		emitted++;
		dl += emit_period;
	}
	double sd = stdev(ifi + 1, emitted - 2);
	free(ifi);
	return sd;
}

// -------- model B: v2 steady free-running emitter (period fixed; occ absorbs jitter) ----
// the emit deadline advances by a CONSTANT period every tick; input jitter shows only as
// occupancy swing, never in the output interval. (A glacial trim would adjust `period`
// over seconds, sub-ppm -- omitted here since over this window it is ~0 and the point is
// that nothing per-tick touches the cadence.) Returns the output IFI sd and, via *max_occ,
// the worst occupancy so we can confirm the ring never has to be large.
static double steady_ifi_sd(const double *arr, int n, double period, int prefill, int *max_occ)
{
	double *ifi = malloc(sizeof(double) * n);
	int produced = 0, emitted = 0, mo = 0;
	double dl = arr[0] + prefill * period;                  // first emit one buffer-depth after first arrival
	double last_emit = -1;
	int k;
	for (k = 0; k < n - prefill - 1; k++) {
		while (produced < n && arr[produced] <= dl) produced++;
		int occ = produced - emitted;
		if (occ > mo) mo = occ;
		if (last_emit >= 0) ifi[emitted] = dl - last_emit;  // the OUTPUT interval
		last_emit = dl;
		emitted++;
		dl += period;                                        // FREE-RUN: constant period, no nudge
	}
	if (max_occ) *max_occ = mo;
	double sd = stdev(ifi + 1, emitted - 2);
	free(ifi);
	return sd;
}

static int test_steady_ifi_independent_of_input_jitter(void)
{
	int n = NTICKS;
	double *arr = malloc(sizeof(double) * n);

	// low jitter
	s_rng = 1; make_arrivals(arr, n, P96K, 0.2 * P96K);
	int mo_lo = 0; double sd_lo = steady_ifi_sd(arr, n, P96K, 64, &mo_lo);
	// 5x more input jitter
	s_rng = 1; make_arrivals(arr, n, P96K, 1.0 * P96K);
	int mo_hi = 0; double sd_hi = steady_ifi_sd(arr, n, P96K, 256, &mo_hi);

	// the free-running output interval is the constant period regardless of input jitter:
	// sd is ~0 in BOTH cases (and does not grow with input jitter).
	ASSERT(sd_lo < 1.0);                       // < 1 ns sd on a 125000 ns period
	ASSERT(sd_hi < 1.0);
	// 5x the input jitter must NOT 5x the output jitter (independence)
	ASSERT(sd_hi < sd_lo + 1.0);
	// the buffer that absorbed it stayed bounded (small ring is enough)
	ASSERT(mo_hi < 1024);
	free(arr);
	return 0;
}

static int test_steady_beats_per_tick_servo(void)
{
	int n = NTICKS;
	double *arr = malloc(sizeof(double) * n);

	// run BOTH emitters over the same arrivals at two input-jitter levels.
	int mo = 0;
	s_rng = 7; make_arrivals(arr, n, P96K, 0.4 * P96K);
	double steady_lo = steady_ifi_sd(arr, n, P96K, 128, &mo);
	double servo_lo  = servo_ifi_sd(arr, n, P96K, 128);
	s_rng = 7; make_arrivals(arr, n, P96K, 1.2 * P96K);   // 3x the input jitter
	double steady_hi = steady_ifi_sd(arr, n, P96K, 384, &mo);
	double servo_hi  = servo_ifi_sd(arr, n, P96K, 384);

	// the steady clock's output cadence is glass-smooth at BOTH levels -- the free-running
	// period is perfectly constant, so essentially no input jitter reaches the output IFI.
	ASSERT(steady_lo < 1.0);
	ASSERT(steady_hi < 1.0);
	// the per-tick servo, by contrast, LEAKS input jitter into the output interval: its IFI
	// sd is strictly worse than the steady clock AND grows with the input jitter (the leak),
	// while the steady clock's stays flat. This is exactly the "rubbery on silences" defect
	// the steady clock removes.
	ASSERT(servo_lo > steady_lo + 0.1);
	ASSERT(servo_hi > servo_lo * 1.5);            // more input jitter -> more output jitter (servo)
	ASSERT(steady_hi < steady_lo + 0.1);          // ... but the steady output does NOT grow
	free(arr);
	return 0;
}

// ---- warm-start state file: mirror of lock_load()'s accept/reject rules -----------------

static long parse_lock(const char *path, const char *out_name, long rate_hz)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[160]; long found = 0;
	while (fgets(line, sizeof line, f)) {
		char *p = line; while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0') continue;
		char nm[32]; long per = 0, lab = 0; long long ts = 0;
		if (sscanf(p, "%31s %ld %ld %lld", nm, &per, &lab, &ts) < 3) continue;
		if (strcmp(nm, out_name)) continue;
		if (lab != rate_hz) continue;
		found = per;
	}
	fclose(f);
	return found;
}

static int test_warm_start_roundtrip(void)
{
	char path[] = "/tmp/reac-repacer-test-XXXXXX";
	int fd = mkstemp(path);
	ASSERT(fd >= 0);
	FILE *f = fdopen(fd, "w");
	ASSERT(f != NULL);
	// realistic file: per-port records at 96 kHz (reac1 vs reac2 crystal offsets) plus a
	// comment line and a foreign port owned by another router/instance.
	fprintf(f, "# reac-repacer PLL warm-start lock\n");
	fprintf(f, "lan1 124867 96000 1749150000\n");
	fprintf(f, "lan2 124929 96000 1749150000\n");
	fprintf(f, "lan9 90703 48000 1749140000\n");   // foreign port, different rate
	fclose(f);

	// per-port restore: each port gets ITS OWN converged period (the crystal-offset point)
	ASSERT_EQ_INT(parse_lock(path, "lan1", 96000), 124867);
	ASSERT_EQ_INT(parse_lock(path, "lan2", 96000), 124929);
	// rate mismatch -> stale -> cold start (0)
	ASSERT_EQ_INT(parse_lock(path, "lan1", 48000), 0);
	// unknown port -> cold start
	ASSERT_EQ_INT(parse_lock(path, "lan3", 96000), 0);
	// the 48 kHz foreign record restores only at its own rate
	ASSERT_EQ_INT(parse_lock(path, "lan9", 48000), 90703);
	ASSERT_EQ_INT(parse_lock(path, "lan9", 96000), 0);

	unlink(path);
	return 0;
}

static int test_warm_start_missing_file_is_cold(void)
{
	ASSERT_EQ_INT(parse_lock("/tmp/reac-repacer-does-not-exist-zzz", "lan1", 96000), 0);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_steady_ifi_independent_of_input_jitter);
	rc |= RUN(test_steady_beats_per_tick_servo);
	rc |= RUN(test_warm_start_roundtrip);
	rc |= RUN(test_warm_start_missing_file_is_cold);
	return rc;
}
