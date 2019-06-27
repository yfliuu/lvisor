// The local APIC manages internal (non-I/O) interrupts.
// See Chapter 8 & Appendix C of Intel processor manual volume 3.

#include "param.h"
#include "types.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "traps.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"  // ncpu

// Local APIC registers, divided by 4 for use as uint[] indices.
#define ID      0x0020   // ID
#define VER     0x0030   // Version
#define TPR     0x0080   // Task Priority
#define EOI     0x00B0   // EOI
#define SVR     0x00F0   // Spurious Interrupt Vector
  #define ENABLE     0x00000100   // Unit Enable
#define ESR     0x0280   // Error Status
#define ICRLO   0x0300   // Interrupt Command
  #define INIT       0x00000500   // INIT/RESET
  #define STARTUP    0x00000600   // Startup IPI
  #define DELIVS     0x00001000   // Delivery status
  #define ASSERT     0x00004000   // Assert interrupt (vs deassert)
  #define DEASSERT   0x00000000
  #define LEVEL      0x00008000   // Level triggered
  #define BCAST      0x00080000   // Send to all APICs, including self.
  #define BUSY       0x00001000
  #define FIXED      0x00000000
#define ICRHI   0x0310   // Interrupt Command [63:32]
#define TIMER   0x0320   // Local Vector Table 0 (TIMER)
  #define X1         0x0000000B   // divide counts by 1
  #define PERIODIC   0x00020000   // Periodic
#define PCINT   0x0340   // Performance Counter LVT
#define LINT0   0x0350   // Local Vector Table 1 (LINT0)
#define LINT1   0x0360   // Local Vector Table 2 (LINT1)
#define ERROR   0x0370   // Local Vector Table 3 (ERROR)
  #define MASKED     0x00010000   // Interrupt masked
#define TICR    0x0380   // Timer Initial Count
#define TCCR    0x0390   // Timer Current Count
#define TDCR    0x03E0   // Timer Divide Configuration

#define MSR_APIC_000   0x800
#define MSR_APIC_BASE  0x01b
#define APIC_BASE_EXTD (1<<10)

volatile uint *lapic;  // Initialized in mp.c

static int x2apic;

static int
lapicr(uint index)
{
  if(x2apic){
    uint low, high;
    rdmsr(MSR_APIC_000+(index>>4), &low, &high);
    return low;
  }
  return lapic[index/4];
}

static void
lapicw(uint index, uint value)
{
  if(x2apic){
    wrmsr(MSR_APIC_000+(index>>4), value, 0);
    return;
  }
  lapic[index/4] = value;
  lapic[ID/4];  // wait for write to finish, by reading
}

static int
lapicr_id(void)
{
  int id = lapicr(ID);
  return x2apic ? id : (id>>24)&0xff;
}

static void
lapicw_icr(uint value, uint id)
{
  if(x2apic)
    return wrmsr(MSR_APIC_000+(ICRLO>>4), value, id);
  lapicw(ICRLO+0x10, id<<24);
  lapicw(ICRLO, value);
}
//PAGEBREAK!

static void
try_enable_x2apic(void)
{
  uint regs[4], low, high;
  cpuid(1, regs);
  if(!(regs[2]&(1<<21)))
    return;
  rdmsr(MSR_APIC_BASE, &low, &high);
  if(!(low&APIC_BASE_EXTD))
    wrmsr(MSR_APIC_BASE, low|APIC_BASE_EXTD, high);
  x2apic = 1;
}

void
lapicinit(void)
{
  if(!lapic)
    return;

  try_enable_x2apic();

  // Enable local APIC; set spurious interrupt vector.
  lapicw(SVR, ENABLE | (T_IRQ0 + IRQ_SPURIOUS));

  // The timer repeatedly counts down at bus frequency
  // from lapic[TICR] and then issues an interrupt.
  // If xv6 cared more about precise timekeeping,
  // TICR would be calibrated using an external time source.
  lapicw(TDCR, X1);
  lapicw(TIMER, PERIODIC | (T_IRQ0 + IRQ_TIMER));
  lapicw(TICR, 10000000);

  // Disable logical interrupt lines.
  lapicw(LINT0, MASKED);
  lapicw(LINT1, MASKED);

  // Disable performance counter overflow interrupts
  // on machines that provide that interrupt entry.
  if(((lapicr(VER)>>16) & 0xFF) >= 4)
    lapicw(PCINT, MASKED);

  // Map error interrupt to IRQ_ERROR.
  lapicw(ERROR, T_IRQ0 + IRQ_ERROR);

  // Clear error status register (requires back-to-back writes).
  lapicw(ESR, 0);
  lapicw(ESR, 0);

  // Ack any outstanding interrupts.
  lapicw(EOI, 0);

  // Send an Init Level De-Assert to synchronise arbitration ID's.
  lapicw_icr(BCAST | INIT | LEVEL, 0);
  while(lapicr(ICRLO) & DELIVS)
    ;

  // Enable interrupts on the APIC (but not on the processor).
  lapicw(TPR, 0);
}

int
cpunum(void)
{
  int apicid, i;
  
  // Cannot call cpu when interrupts are enabled:
  // result not guaranteed to last long enough to be used!
  // Would prefer to panic but even printing is chancy here:
  // almost everything, including cprintf and panic, calls cpu,
  // often indirectly through acquire and release.
  if(readeflags()&FL_IF){
    static int n;
    if(n++ == 0)
      cprintf("cpu called from %x with interrupts enabled\n",
        __builtin_return_address(0));
  }

  if (!lapic)
    return 0;

  apicid = lapicr_id();
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return i;
  }
  panic("unknown apicid\n");
}

// Acknowledge interrupt.
void
lapiceoi(void)
{
  if(lapic)
    lapicw(EOI, 0);
}

// Spin for a given number of microseconds.
// On real hardware would want to tune this dynamically.
void
microdelay(int us)
{
}

#define CMOS_PORT    0x70
#define CMOS_RETURN  0x71

// Start additional processor running entry code at addr.
// See Appendix B of MultiProcessor Specification.
void
lapicstartap(uchar apicid, uint addr)
{
  int i;
  ushort *wrv;

  // "The BSP must initialize CMOS shutdown code to 0AH
  // and the warm reset vector (DWORD based at 40:67) to point at
  // the AP startup code prior to the [universal startup algorithm]."
  outb(CMOS_PORT, 0xF);  // offset 0xF is shutdown code
  outb(CMOS_PORT+1, 0x0A);
  wrv = (ushort*)P2V((0x40<<4 | 0x67));  // Warm reset vector
  wrv[0] = 0;
  wrv[1] = addr >> 4;

  // "Universal startup algorithm."
  // Send INIT (level-triggered) interrupt to reset other CPU.
  lapicw_icr(INIT | LEVEL | ASSERT, apicid);
  microdelay(200);
  lapicw_icr(INIT | LEVEL, apicid);
  microdelay(100);    // should be 10ms, but too slow in Bochs!

  // Send startup IPI (twice!) to enter code.
  // Regular hardware is supposed to only accept a STARTUP
  // when it is in the halted state due to an INIT.  So the second
  // should be ignored, but it is part of the official Intel algorithm.
  // Bochs complains about the second one.  Too bad for Bochs.
  for(i = 0; i < 2; i++){
    lapicw_icr(STARTUP | (addr>>12), apicid);
    microdelay(200);
  }
}

#define CMOS_STATA   0x0a
#define CMOS_STATB   0x0b
#define CMOS_UIP    (1 << 7)        // RTC update in progress

#define SECS    0x00
#define MINS    0x02
#define HOURS   0x04
#define DAY     0x07
#define MONTH   0x08
#define YEAR    0x09

static uint cmos_read(uint reg)
{
  outb(CMOS_PORT,  reg);
  microdelay(200);

  return inb(CMOS_RETURN);
}

static void fill_rtcdate(struct rtcdate *r)
{
  r->second = cmos_read(SECS);
  r->minute = cmos_read(MINS);
  r->hour   = cmos_read(HOURS);
  r->day    = cmos_read(DAY);
  r->month  = cmos_read(MONTH);
  r->year   = cmos_read(YEAR);
}

// qemu seems to use 24-hour GWT and the values are BCD encoded
void cmostime(struct rtcdate *r)
{
  struct rtcdate t1, t2;
  int sb, bcd;

  sb = cmos_read(CMOS_STATB);

  bcd = (sb & (1 << 2)) == 0;

  // make sure CMOS doesn't modify time while we read it
  for(;;) {
    fill_rtcdate(&t1);
    if(cmos_read(CMOS_STATA) & CMOS_UIP)
        continue;
    fill_rtcdate(&t2);
    if(memcmp(&t1, &t2, sizeof(t1)) == 0)
      break;
  }

  // convert
  if(bcd) {
#define    CONV(x)     (t1.x = ((t1.x >> 4) * 10) + (t1.x & 0xf))
    CONV(second);
    CONV(minute);
    CONV(hour  );
    CONV(day   );
    CONV(month );
    CONV(year  );
#undef     CONV
  }

  *r = t1;
  r->year += 2000;
}
