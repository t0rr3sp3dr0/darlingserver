/*
 * Copyright (c) 2000-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */

/*
 *	File:		i386/rtclock.c
 *	Purpose:	Routines for handling the machine dependent
 *			real-time clock. Historically, this clock is
 *			generated by the Intel 8254 Programmable Interval
 *			Timer, but local apic timers are now used for
 *			this purpose with the master time reference being
 *			the cpu clock counted by the timestamp MSR.
 */


#include <mach/mach_types.h>

#include <kern/cpu_data.h>
#include <kern/cpu_number.h>
#include <kern/clock.h>
#include <kern/host_notify.h>
#include <kern/macro_help.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <kern/assert.h>
#include <kern/timer_queue.h>
#include <mach/vm_prot.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>         /* for kernel_map */
#include <architecture/i386/pio.h>
#include <i386/machine_cpu.h>
#include <i386/cpuid.h>
#include <i386/cpu_threads.h>
#include <i386/mp.h>
#include <i386/machine_routines.h>
#include <i386/pal_routines.h>
#include <i386/proc_reg.h>
#include <i386/misc_protos.h>
#include <pexpert/pexpert.h>
#include <machine/limits.h>
#include <machine/commpage.h>
#include <sys/kdebug.h>
#include <i386/tsc.h>
#include <i386/rtclock_protos.h>
#define UI_CPUFREQ_ROUNDING_FACTOR      10000000

int             rtclock_init(void);

uint64_t        tsc_rebase_abs_time = 0;

volatile uint64_t gAcpiLastSleepTscBase = 0;
volatile uint64_t gAcpiLastSleepNanoBase = 0;
volatile uint64_t gAcpiLastWakeTscBase = 0;
volatile uint64_t gAcpiLastWakeNanoBase = 0;

static void     rtc_set_timescale(uint64_t cycles);
static uint64_t rtc_export_speed(uint64_t cycles);

void
rtc_timer_start(void)
{
#ifndef __DARLING__
	/*
	 * Force a complete re-evaluation of timer deadlines.
	 */
	x86_lcpu()->rtcDeadline = EndOfAllTime;
#endif // __DARLING__
	timer_resync_deadlines();
}

static inline uint32_t
_absolutetime_to_microtime(uint64_t abstime, clock_sec_t *secs, clock_usec_t *microsecs)
{
	uint32_t remain;
	*secs = abstime / (uint64_t)NSEC_PER_SEC;
	remain = (uint32_t)(abstime % (uint64_t)NSEC_PER_SEC);
	*microsecs = remain / NSEC_PER_USEC;
	return remain;
}

static inline void
_absolutetime_to_nanotime(uint64_t abstime, clock_sec_t *secs, clock_usec_t *nanosecs)
{
	*secs = abstime / (uint64_t)NSEC_PER_SEC;
	*nanosecs = (clock_usec_t)(abstime % (uint64_t)NSEC_PER_SEC);
}

/*
 * Nanotime/mach_absolutime_time
 * -----------------------------
 * The timestamp counter (TSC) - which counts cpu clock cycles and can be read
 * efficiently by the kernel and in userspace - is the reference for all timing.
 * The cpu clock rate is platform-dependent and may stop or be reset when the
 * processor is napped/slept.  As a result, nanotime is the software abstraction
 * used to maintain a monotonic clock, adjusted from an outside reference as needed.
 *
 * The kernel maintains nanotime information recording:
 *      - the ratio of tsc to nanoseconds
 *	  with this ratio expressed as a 32-bit scale and shift
 *	  (power of 2 divider);
 *	- { tsc_base, ns_base } pair of corresponding timestamps.
 *
 * The tuple {tsc_base, ns_base, scale, shift} is exported in the commpage
 * for the userspace nanotime routine to read.
 *
 * All of the routines which update the nanotime data are non-reentrant.  This must
 * be guaranteed by the caller.
 */
static inline void
rtc_nanotime_set_commpage(pal_rtc_nanotime_t *rntp)
{
	commpage_set_nanotime(rntp->tsc_base, rntp->ns_base, rntp->scale, rntp->shift);
}

/*
 * rtc_nanotime_init:
 *
 * Intialize the nanotime info from the base time.
 */
static inline void
_rtc_nanotime_init(pal_rtc_nanotime_t *rntp, uint64_t base)
{
	uint64_t        tsc = rdtsc64();

	_pal_rtc_nanotime_store(tsc, base, rntp->scale, rntp->shift, rntp);
}

void
rtc_nanotime_init(uint64_t base)
{
	gAcpiLastSleepTscBase = pal_rtc_nanotime_info.tsc_base;
	gAcpiLastSleepNanoBase = pal_rtc_nanotime_info.ns_base;

	_rtc_nanotime_init(&pal_rtc_nanotime_info, base);

	gAcpiLastWakeTscBase = pal_rtc_nanotime_info.tsc_base;
	gAcpiLastWakeNanoBase = pal_rtc_nanotime_info.ns_base;

	rtc_nanotime_set_commpage(&pal_rtc_nanotime_info);
}

/*
 * rtc_nanotime_init_commpage:
 *
 * Call back from the commpage initialization to
 * cause the commpage data to be filled in once the
 * commpages have been created.
 */
void
rtc_nanotime_init_commpage(void)
{
	spl_t                   s = splclock();

	rtc_nanotime_set_commpage(&pal_rtc_nanotime_info);
	splx(s);
}

/*
 * rtc_nanotime_read:
 *
 * Returns the current nanotime value, accessable from any
 * context.
 */
static inline uint64_t
rtc_nanotime_read(void)
{
	return _rtc_nanotime_read(&pal_rtc_nanotime_info);
}

/*
 * rtc_clock_napped:
 *
 * Invoked from power management when we exit from a low C-State (>= C4)
 * and the TSC has stopped counting.  The nanotime data is updated according
 * to the provided value which represents the new value for nanotime.
 */
void
rtc_clock_napped(uint64_t base, uint64_t tsc_base)
{
	pal_rtc_nanotime_t      *rntp = &pal_rtc_nanotime_info;
	uint64_t        oldnsecs;
	uint64_t        newnsecs;
	uint64_t        tsc;

	assert(!ml_get_interrupts_enabled());
	tsc = rdtsc64();
	oldnsecs = rntp->ns_base + _rtc_tsc_to_nanoseconds(tsc - rntp->tsc_base, rntp);
	newnsecs = base + _rtc_tsc_to_nanoseconds(tsc - tsc_base, rntp);

	/*
	 * Only update the base values if time using the new base values
	 * is later than the time using the old base values.
	 */
	if (oldnsecs < newnsecs) {
		_pal_rtc_nanotime_store(tsc_base, base, rntp->scale, rntp->shift, rntp);
		rtc_nanotime_set_commpage(rntp);
	}
}

/*
 * Invoked from power management to correct the SFLM TSC entry drift problem:
 * a small delta is added to the tsc_base.  This is equivalent to nudgin time
 * backwards.  We require this to be on the order of a TSC quantum which won't
 * cause callers of mach_absolute_time() to see time going backwards!
 */
void
rtc_clock_adjust(uint64_t tsc_base_delta)
{
	pal_rtc_nanotime_t  *rntp = &pal_rtc_nanotime_info;

	assert(!ml_get_interrupts_enabled());
	assert(tsc_base_delta < 100ULL); /* i.e. it's small */
	_rtc_nanotime_adjust(tsc_base_delta, rntp);
	rtc_nanotime_set_commpage(rntp);
}

/*
 * rtc_sleep_wakeup:
 *
 * Invoked from power management when we have awoken from a sleep (S3)
 * and the TSC has been reset, or from Deep Idle (S0) sleep when the TSC
 * has progressed.  The nanotime data is updated based on the passed-in value.
 *
 * The caller must guarantee non-reentrancy.
 */
void
rtc_sleep_wakeup(
	uint64_t                base)
{
	/* Set fixed configuration for lapic timers */
	rtc_timer->rtc_config();

	/*
	 * Reset nanotime.
	 * The timestamp counter will have been reset
	 * but nanotime (uptime) marches onward.
	 */
	rtc_nanotime_init(base);
}

void
rtc_decrementer_configure(void)
{
	rtc_timer->rtc_config();
}
/*
 * rtclock_early_init() is called very early at boot to
 * establish mach_absolute_time() and set it to zero.
 */
void
rtclock_early_init(void)
{
	assert(tscFreq);
	rtc_set_timescale(tscFreq);
}

/*
 * Initialize the real-time clock device.
 * In addition, various variables used to support the clock are initialized.
 */
int
rtclock_init(void)
{
#ifndef __DARLING__
	uint64_t        cycles;

	assert(!ml_get_interrupts_enabled());

	if (cpu_number() == master_cpu) {
		assert(tscFreq);

		/*
		 * Adjust and set the exported cpu speed.
		 */
		cycles = rtc_export_speed(tscFreq);

		/*
		 * Set min/max to actual.
		 * ACPI may update these later if speed-stepping is detected.
		 */
		gPEClockFrequencyInfo.cpu_frequency_min_hz = cycles;
		gPEClockFrequencyInfo.cpu_frequency_max_hz = cycles;

		rtc_timer_init();
		clock_timebase_init();
		ml_init_lock_timeout();
		ml_init_delay_spin_threshold(10);
	}

	/* Set fixed configuration for lapic timers */
	rtc_timer->rtc_config();
	rtc_timer_start();
#endif // __DARLING__

	return 1;
}

// utility routine
// Code to calculate how many processor cycles are in a second...

static void
rtc_set_timescale(uint64_t cycles)
{
	pal_rtc_nanotime_t      *rntp = &pal_rtc_nanotime_info;
	uint32_t    shift = 0;

	/* the "scale" factor will overflow unless cycles>SLOW_TSC_THRESHOLD */

	while (cycles <= SLOW_TSC_THRESHOLD) {
		shift++;
		cycles <<= 1;
	}

	rntp->scale = (uint32_t)(((uint64_t)NSEC_PER_SEC << 32) / cycles);

	rntp->shift = shift;

	/*
	 * On some platforms, the TSC is not reset at warm boot. But the
	 * rebase time must be relative to the current boot so we can't use
	 * mach_absolute_time(). Instead, we convert the TSC delta since boot
	 * to nanoseconds.
	 */
	if (tsc_rebase_abs_time == 0) {
		tsc_rebase_abs_time = _rtc_tsc_to_nanoseconds(
			rdtsc64() - tsc_at_boot, rntp);
	}

	rtc_nanotime_init(0);
}

static uint64_t
rtc_export_speed(uint64_t cyc_per_sec)
{
	pal_rtc_nanotime_t      *rntp = &pal_rtc_nanotime_info;
	uint64_t        cycles;

	if (rntp->shift != 0) {
		printf("Slow TSC, rtc_nanotime.shift == %d\n", rntp->shift);
	}

	/* Round: */
	cycles = ((cyc_per_sec + (UI_CPUFREQ_ROUNDING_FACTOR / 2))
	    / UI_CPUFREQ_ROUNDING_FACTOR)
	    * UI_CPUFREQ_ROUNDING_FACTOR;

	/*
	 * Set current measured speed.
	 */
	if (cycles >= 0x100000000ULL) {
		gPEClockFrequencyInfo.cpu_clock_rate_hz = 0xFFFFFFFFUL;
	} else {
		gPEClockFrequencyInfo.cpu_clock_rate_hz = (unsigned long)cycles;
	}
	gPEClockFrequencyInfo.cpu_frequency_hz = cycles;

	kprintf("[RTCLOCK] frequency %llu (%llu)\n", cycles, cyc_per_sec);
	return cycles;
}

void
clock_get_system_microtime(
	clock_sec_t                     *secs,
	clock_usec_t            *microsecs)
{
	uint64_t        now = rtc_nanotime_read();

	_absolutetime_to_microtime(now, secs, microsecs);
}

void
clock_get_system_nanotime(
	clock_sec_t                     *secs,
	clock_nsec_t            *nanosecs)
{
	uint64_t        now = rtc_nanotime_read();

	_absolutetime_to_nanotime(now, secs, nanosecs);
}

void
clock_gettimeofday_set_commpage(uint64_t abstime, uint64_t sec, uint64_t frac, uint64_t scale, uint64_t tick_per_sec)
{
	commpage_set_timestamp(abstime, sec, frac, scale, tick_per_sec);
}

void
clock_timebase_info(
	mach_timebase_info_t    info)
{
	info->numer = info->denom =  1;
}

/*
 * Real-time clock device interrupt.
 */
void
rtclock_intr(
	x86_saved_state_t       *tregs)
{
	uint64_t        rip;
	boolean_t       user_mode = FALSE;

	assert(get_preemption_level() > 0);
	assert(!ml_get_interrupts_enabled());

	if (is_saved_state64(tregs) == TRUE) {
		x86_saved_state64_t     *regs;

		regs = saved_state64(tregs);

		if (regs->isf.cs & 0x03) {
			user_mode = TRUE;
		}
		rip = regs->isf.rip;
	} else {
		x86_saved_state32_t     *regs;

		regs = saved_state32(tregs);

		if (regs->cs & 0x03) {
			user_mode = TRUE;
		}
		rip = regs->eip;
	}

	/* call the generic etimer */
	timer_intr(user_mode, rip);
}


#ifndef __DARLING__
/*
 *	Request timer pop from the hardware
 */

uint64_t
setPop(uint64_t time)
{
	uint64_t        now;
	uint64_t        pop;

	/* 0 and EndOfAllTime are special-cases for "clear the timer" */
	if (time == 0 || time == EndOfAllTime) {
		time = EndOfAllTime;
		now = 0;
		pop = rtc_timer->rtc_set(0, 0);
	} else {
		now = rtc_nanotime_read();      /* The time in nanoseconds */
		pop = rtc_timer->rtc_set(time, now);
	}

	/* Record requested and actual deadlines set */
	x86_lcpu()->rtcDeadline = time;
	x86_lcpu()->rtcPop      = pop;

	return pop - now;
}
#endif // __DARLING__

uint64_t
mach_absolute_time(void)
{
	return rtc_nanotime_read();
}

uint64_t
mach_approximate_time(void)
{
	return rtc_nanotime_read();
}

void
clock_interval_to_absolutetime_interval(
	uint32_t                interval,
	uint32_t                scale_factor,
	uint64_t                *result)
{
	*result = (uint64_t)interval * scale_factor;
}

void
absolutetime_to_microtime(
	uint64_t                        abstime,
	clock_sec_t                     *secs,
	clock_usec_t            *microsecs)
{
	_absolutetime_to_microtime(abstime, secs, microsecs);
}

void
nanotime_to_absolutetime(
	clock_sec_t                     secs,
	clock_nsec_t            nanosecs,
	uint64_t                        *result)
{
	*result = ((uint64_t)secs * NSEC_PER_SEC) + nanosecs;
}

void
absolutetime_to_nanoseconds(
	uint64_t                abstime,
	uint64_t                *result)
{
	*result = abstime;
}

void
nanoseconds_to_absolutetime(
	uint64_t                nanoseconds,
	uint64_t                *result)
{
	*result = nanoseconds;
}

void
machine_delay_until(
	uint64_t interval,
	uint64_t                deadline)
{
	(void)interval;
	while (mach_absolute_time() < deadline) {
		cpu_pause();
	}
}
