#include "types.h"
#include "mmu.h"
#include "kernel.hh"
#include "amd64.h"
#include "cpu.hh"
#include "traps.h"
#include "spinlock.hh"
#include "condvar.hh"
#include "proc.hh"
#include "bits.hh"
#include "kalloc.hh"
#include "apic.hh"
#include "irq.hh"
#include "kstream.hh"
#include "hwvm.hh"
#include "refcache.hh"
#include "cpuid.hh"
#include "linearhash.hh"
#include "vm.hh"
#include "cmdline.hh"

extern "C" void __uaccess_end(void);

struct intdesc idt[256] __attribute__((section (".qdata"), aligned(4096)));

// Only used if xsave is not supported.
char fpu_initial_state[512]  __attribute__((section (".qdata")));

struct segdesc  __attribute__((aligned(16))) bootgdt[NSEGS] = {
  // null
  SEGDESC(0, 0, 0),
  // 32-bit kernel code
  SEGDESC(0, 0xfffff, SEG_R|SEG_CODE|SEG_S|SEG_DPL(0)|SEG_P|SEG_D|SEG_G),
  // 64-bit kernel code
  SEGDESC(0, 0, SEG_R|SEG_CODE|SEG_S|SEG_DPL(0)|SEG_P|SEG_L|SEG_G),
  // kernel data
  SEGDESC(0, 0xfffff, SEG_W|SEG_S|SEG_DPL(0)|SEG_P|SEG_D|SEG_G),
  // unused
  {0},
  // unused
  {0},
  // The order of the user data and user code segments is
  // important for syscall instructions.  See initseg.
  // 64-bit user data
  SEGDESC(0, 0xfffff, SEG_W|SEG_S|SEG_DPL(3)|SEG_P|SEG_D|SEG_G),
  // 64-bit user code
  SEGDESC(0, 0, SEG_R|SEG_CODE|SEG_S|SEG_DPL(3)|SEG_P|SEG_L|SEG_G),
};

DEFINE_PERCPU(char*, nmistacktop);

// boot.S
extern u64 trapentry[];

unsigned int(*registered_trap_handlers[256])(void*) = { 0 };
void* registered_trap_handler_contexts[256] = { 0 };

static struct irq_info
{
  irq_handler *handlers;
  // True if this IRQ has been allocated to a device
  bool in_use;
} irq_info[256 - T_IRQ0];

// Instruction pointers that cause transparent world barriers.
linearhash<u64, u64> transparent_wb_rips(10240);
linearhash<u64, u64> intentional_wb_rips(10240);

static void trap(struct trapframe *tf, bool had_secrets);

void
ensure_secrets()
{
  pushcli();
  bool had_secrets = secrets_mapped;
  switch_to_kstack();
  popcli();

  if (cmdline_params.track_wbs && !had_secrets) {
    intentional_wb_rips.increment((u64)__builtin_return_address(0) - 1);
  }
  if (!had_secrets && myproc()) {
    myproc()->intentional_barriers++;
  }
}


u64
sysentry_c(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 num)
{
  if(myproc()->killed) {
    procexit(-1);
  }

  trapframe *tf = (trapframe*) (myproc()->kstack + KSTACKSIZE - sizeof(*tf));
  myproc()->tf = tf;
  u64 r = syscall(a0, a1, a2, a3, a4, a5, num);

  if(myproc()->killed) {
    procexit(-1);
  }

  return r;
}

int
do_pagefault(struct trapframe *tf, bool had_secrets)
{
  uptr addr = rcr2();

  if (addr == 123) {
    extern u64* ENTRY_TIMES;
    extern u64 ENTRY_COUNT;
    u64 start = (tf->rax & 0xffffffff) | ((tf->rdx & 0xffffffff) << 32);
    u64 end = (tf->padding3[1] & 0xffffffff) | ((tf->padding3[0] & 0xffffffff) << 32);
    ENTRY_TIMES[ENTRY_COUNT++] = end - start;
    tf->rip += 8;
    return 0;
  }

  if (((tf->cs&3) == 0 || myproc() == 0) &&
      !had_secrets && addr >= KGLOBAL) {
    // Page fault was probably caused by trying to access secret
    // data so map all secrets in now and record where this happened.
    switch_to_kstack();

    if (cmdline_params.track_wbs) {
      uptr pc[2];
      getcallerpcs((void *) tf->rbp, pc, NELEM(pc));
      u64 bt = (tf->rip & 0x1fffff) | ((pc[0] & 0x1fffff) << 21) | ((pc[1] & 0x1fffff) << 42);
      transparent_wb_rips.increment(bt);
    }
    if (!had_secrets && myproc()) {
      myproc()->transparent_barriers++;
    }
    return 0;
  } else if (addr < USERTOP && tf->err & FEC_U) {
    sti();
    int r = pagefault(myproc()->vmap.get(), addr, tf->err);
    cli();

    // XXX distinguish between SIGSEGV and SIGBUS?
    if(r >= 0 || myproc()->deliver_signal(SIGSEGV)){
      return 0;
    }
  } else if (myproc() && myproc()->uaccess_) {
    // Normally __uaccess_* functions must be called with interrupts disabled so
    // that we can process page faults caused by unmapped pages. However, futex
    // critical sections need to hold a lock while checking user memory, so we
    // offer an escape hatch.
    if (myproc()->uaccess_ && mycpu()->ncli == 0) {
      sti();
      int r = pagefault(myproc()->vmap.get(), addr, tf->err);
      cli();
      if(r >= 0) {
        return 0;
      }
    }

    tf->rax = -1;
    tf->rip = (u64)__uaccess_end;
    return 0;
  }
  return -1;
}

static inline void
lapiceoi()
{
  lapic->eoi();
}

namespace {
  DEFINE_PERCPU(uintptr_t, nmi_lastpc);
  DEFINE_PERCPU(int, nmi_swallow);
}

extern "C" void
nmientry_c(struct nmiframe *tf)
{
  u64 saved_gsbase = readmsr(MSR_GS_BASE);
  writemsr(MSR_GS_BASE, tf->gsbase);

  // An NMI can come in after popcli() drops ncli to zero and intena
  // is 1, but before popcli() checks intena and calls sti.  If the
  // NMI handler acquires any lock, acquire() will call pushcli(),
  // which will set intena to 0, and upon return from the NMI, the
  // preempted popcli will observe intena=0 and fail to sti.
  int intena_save = mycpu()->intena;

  // The only locks that we can acquire during NMI are ones
  // we acquire only during NMI.

  // NMIs are tricky.  On the one hand, they're edge triggered,
  // which means we're not guaranteed to get an NMI interrupt for
  // every NMI event, so we have to proactively handle all of the
  // NMI sources we can.  On the other hand, they're also racy,
  // since an NMI source may successfully queue an NMI behind an
  // existing NMI, but we may detect that source when handling the
  // first NMI.  Our solution is to detect back-to-back NMIs and
  // keep track of how many NMI sources we've handled: as long as
  // the number of back-to-back NMIs in a row never exceeds the
  // number of NMI sources we've handled across these back-to-back
  // NMIs, we're not concerned, even if an individual NMI doesn't
  // detect any active sources.

  // Is this a back-to-back NMI?  If so, we might have handled all
  // of the NMI sources already.
  bool repeat = (*nmi_lastpc == tf->rip);
  *nmi_lastpc = tf->rip;
  if (!repeat)
    *nmi_swallow = 0;

  // Handle NMIs
  int handled = 0;
  handled += sampintr(tf);

  // No lapiceoi because only fixed delivery mode interrupts need to
  // be EOI'd (and fixed mode interrupts can't be programmed to
  // produce an NMI vector).

  if (handled == 0 && !*nmi_swallow)
    panic("NMI");

  // This NMI accounts for one handled event, so we can swallow up
  // to handled - 1 more back-to-back NMIs after this one.
  *nmi_swallow += handled - 1;

  mycpu()->intena = intena_save;

  writemsr(MSR_GS_BASE, saved_gsbase);
}

extern "C" void
dblfltentry_c(struct trapframe *tf)
{
  kerneltrap(tf);
}

// C/C++ entry point for traps; called by assembly trap stub
extern "C" void
trap_c(struct trapframe *tf, bool had_secrets)
{
  trap(tf, had_secrets);
}

static void
trap(struct trapframe *tf, bool had_secrets)
{
  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    kstats::inc(&kstats::sched_tick_count);
    // for now, just care about timer interrupts
#if CODEX
    codex_magic_action_run_async_event(T_IRQ0 + IRQ_TIMER);
#endif

    if (mycpu()->timer_printpc) {
      cprintf("cpu%d: proc %s rip %lx rsp %lx cs %x\n",
              mycpu()->id,
              myproc() ? myproc()->name : "(none)",
              tf->rip, tf->rsp, tf->cs);
      if (mycpu()->timer_printpc == 2 && tf->rbp > KBASE) {
        uptr pc[10];
        getcallerpcs((void *) tf->rbp, pc, NELEM(pc));
        for (int i = 0; i < 10 && pc[i]; i++)
          cprintf("cpu%d:   %lx\n", mycpu()->id, pc[i]);
      }
      mycpu()->timer_printpc = 0;
    }
    if (mycpu()->id == 0) {
      timerintr();

      extern u64* ENTRY_TIMES;
      extern u64 ENTRY_COUNT;
      if (ENTRY_COUNT != 0xffffffff) {
        u64 start = (tf->rax & 0xffffffff) | ((tf->rdx & 0xffffffff) << 32);
        u64 end = (tf->padding3[1] & 0xffffffff) | ((tf->padding3[0] & 0xffffffff) << 32);
        ENTRY_TIMES[ENTRY_COUNT++] = end - start;
        
        if (ENTRY_COUNT == 100)
          tf->rip = 0x100f;
      }
    }
    refcache::mycache->tick();
    lapiceoi();
    if (mycpu()->no_sched_count) {
      kstats::inc(&kstats::sched_blocked_tick_count);
      // Request a yield when no_sched_count is released.  We can
      // modify this without protection because interrupts are
      // disabled.
      mycpu()->no_sched_count |= NO_SCHED_COUNT_YIELD_REQUESTED;
      return;
    }
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + IRQ_MOUSE:
    mouseintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + IRQ_COM2:
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%lx\n",
            mycpu()->id, tf->cs, tf->rip);
    // [Intel SDM 10.9 Spurious Interrupt] The spurious interrupt
    // vector handler should return without an EOI.
    //lapiceoi();
    break;
  case T_IRQ0 + IRQ_ERROR:
    cprintf("cpu%d: lapic error?\n", mycpu()->id);
    lapiceoi();
    break;
  case T_TLBFLUSH: {
    lapiceoi();
    tlb_shootdown::on_ipi();
    break;
  }
  case T_SAMPCONF:
    lapiceoi();
    sampconf();
    break;
  case T_PAUSE:
    extern void pause_cpu(); // ipi.cc
    lapiceoi();
    pause_cpu();
    break;
  case T_IPICALL: {
    extern void on_ipicall();
    lapiceoi();
    on_ipicall();
    break;
  }
  case T_WAKE_CORE:
    lapiceoi();
    break;
  default:
    if (tf->trapno == T_ILLOP && (tf->cs&3) == 0 && tf->rip >= KTEXT && tf->rip < KTEXTEND) {
      u64 instr = *(u64*)tf->rip;
      u64* regs[16] = {
        &tf->rax, &tf->rcx, &tf->rdx, &tf->rbx, &tf->rsp, &tf->rbp, &tf->rsi, &tf->rdi,
        &tf->r8, &tf->r9, &tf->r10, &tf->r11, &tf->r12, &tf->r13, &tf->r14, &tf->r15,
      };

      // popcntq: This instruction is supported by all current processors, but
      // not QEMU's default 'qemu64'. This small emulation routine is only
      // needed so that Ward can run with qemu-system-x86_64 without passing any
      // extra arguments (ie '-cpu qemu64,+popcnt'). If QEMU bumps its default
      // provided features or compatibility is ever no-longer wanted, this code
      // can be removed.
      if ((instr & 0xc0fffff0ff) == 0xc0b80f40f3) {
        u64 reg = ((instr>>32) & 0x7) | (((instr>>8) & 0x1) << 3);
        u64 rm = ((instr>>35) & 0x7) | (((instr>>9) & 0x1) << 3);

        *regs[rm] = 0;
        for(int i = 0; i < 64; i++)
          if(*regs[reg] & (1ul << i))
            regs[rm]++;

        tf->rip += 5;
        return;
      }
    } else if (tf->trapno == T_ILLOP && (tf->cs&3) == 3) {
        u64 instr = *(u64*)tf->rip;
        if ((instr & 0xffff) == 0x0b0f) {
          extern u64* ENTRY_TIMES;
          extern u64 ENTRY_COUNT;

          u64 start = (tf->rax & 0xffffffff) | ((tf->rdx & 0xffffffff) << 32);
          u64 end = (tf->padding3[1] & 0xffffffff) | ((tf->padding3[0] & 0xffffffff) << 32);

          ENTRY_TIMES[ENTRY_COUNT++] = end - start;

          tf->rip += 2;
          return;
        }
    } else if (tf->trapno >= T_IRQ0 && irq_info[tf->trapno - T_IRQ0].handlers) {
      for (auto h = irq_info[tf->trapno - T_IRQ0].handlers; h; h = h->next)
        h->handle_irq();
      lapiceoi();
      piceoi();
      return;
    } else if (tf->trapno == T_PGFLT && do_pagefault(tf, had_secrets) == 0) {
      if(myproc()->killed)
        procexit(-1);
      return;
    } else if (tf->trapno < 256 && registered_trap_handlers[tf->trapno]) {
      ensure_secrets();
      registered_trap_handlers[tf->trapno](
        registered_trap_handler_contexts[tf->trapno]);
      return;
    }

    if (myproc() == 0 || (tf->cs&3) == 0) {
      kerneltrap(tf);
    } else {
      // In user space, assume process misbehaved.
      uerr.println("tid ", myproc()->tid, ' ', myproc()->name,
                   ": trap ", (u64)tf->trapno, " err ", (u32)tf->err,
                   " on cpu ", myid(), " rip ", shex(tf->rip),
                   " rsp ", shex(tf->rsp), " addr ", shex(rcr2()),
                   "--kill proc");
      myproc()->killed = 1;
    }
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == 0x3)
    procexit(-1);

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->get_state() == RUNNING &&
     (tf->trapno == T_IRQ0+IRQ_TIMER || myproc()->yield_)) {
    yield();
  }

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == 0x3)
    procexit(-1);
}

void
inittrap(void)
{
  for(int i = 0; i < 256; i++) {
    idt[i] = INTDESC(KCSEG, trapentry[i], INT_P | SEG_INTR64);
  }

  // Conservatively reserve all legacy IRQs.  This might cause us to
  // not be able to configure a device.
  for (int i = 0; i < 16; ++i)
    irq_info[i].in_use = true;
  // Also reserve the spurious vector
  irq_info[IRQ_SPURIOUS].in_use = true;
  // And reserve interrupt 255 (Intel SDM Vol. 3 suggests this can't
  // be used for MSI).
  irq_info[255 - T_IRQ0].in_use = true;
}

void
initvectoredtrap(void)
{
  // Configure double fault handling. Any double fault results in a kernel
  // panic, so it is harmless to just share double fault stacks globally.
  for (int c = 0; c < ncpu; c++) {
    cpus[c].ts.ist[2] = (u64)palloc("dblfltstack", KSTACKSIZE) + KSTACKSIZE;
  }
  idt[T_DBLFLT].ist = 2;

  // Configure NMI handling
  for (int c = 0; c < ncpu; c++) {
    nmistacktop[c] = kalloc("nmistack", KSTACKSIZE) + KSTACKSIZE;
    cpus[c].ts.ist[1] = (u64)nmistacktop[c] - 16;
    nmiframe* tf = (nmiframe*)(nmistacktop[c] - sizeof(nmiframe));
    tf->gsbase = (u64)&cpus[c].cpu;
    tf->stack = (u64)tf;
  }
  idt[T_NMI].ist = 1;
}

void
initfpu(void)
{
  // Allow usage of FPU instructions.
  lcr0(rcr0() & ~(CR0_TS | CR0_EM));
  if (cpuid::features().xsave)
    lcr4(rcr4() | CR4_OSXSAVE);
  // Initialize FPU, ignoring pending FP exceptions
  fninit();
  // Don't generate interrupts for any SSE exceptions
  ldmxcsr(0x1f80);

  if (!cpuid::features().xsave)
    fxsave(fpu_initial_state);
}

void
initmsr(void)
{
  // XXX Where should this code live?

#if defined(DISABLE_PREFETCH_STREAM)
#define CONTROL_PREFETCH_STREAM 1
#else
#define CONTROL_PREFETCH_STREAM 0
#define DISABLE_PREFETCH_STREAM 0
#endif
#if defined(DISABLE_PREFETCH_ADJ)
#define CONTROL_PREFETCH_ADJ 1
#else
#define CONTROL_PREFETCH_ADJ 0
#define DISABLE_PREFETCH_ADJ 0
#endif

  if (CONTROL_PREFETCH_STREAM || CONTROL_PREFETCH_ADJ) {
    // Is the MISC_FEATURE_CONTROL MSR valid?
    auto m = cpuid::model();
    if (!(cpuid::vendor_is_intel() && m.family == 6 &&
          (m.model == 0x1a || m.model == 0x1e || m.model == 0x1f || // Nehalem
           m.model == 0x25 || m.model == 0x2c || // Westmere
           m.model == 0x2e || // Nehalem-EX
           m.model == 0x2f)))  // Westmere-EX
      panic("Cannot control hardware prefetcher for this CPU model");

    uint64_t mfc = readmsr(MSR_INTEL_MISC_FEATURE_CONTROL);

    if (DISABLE_PREFETCH_STREAM)
      mfc |= MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_STREAMER;
    else if (CONTROL_PREFETCH_STREAM)
      mfc &= ~MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_STREAMER;

    if (DISABLE_PREFETCH_ADJ)
      mfc |= MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_SPATIAL;
    else if (CONTROL_PREFETCH_ADJ)
      mfc &= ~MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_SPATIAL;

    writemsr(MSR_INTEL_MISC_FEATURE_CONTROL, mfc);

    if (myid() == 0) {
      if (CONTROL_PREFETCH_STREAM)
        cprintf("msr: MLC stream prefetcher %s\n",
                DISABLE_PREFETCH_STREAM ? "disabled" : "enabled");
      if (CONTROL_PREFETCH_ADJ)
        cprintf("msr: Adjacent cache line prefetcher %s\n",
                DISABLE_PREFETCH_ADJ ? "disabled" : "enabled");
    }

    // XXX There are also the DCU prefetchers.  ben's BIOS doesn't
    // disable these when I set "Hardware prefetcher" to disable, so
    // I'm not convinced the bits are right.
  }
}

void
initseg(struct cpu *c)
{
  volatile struct desctr dtr;

  dtr.limit = sizeof(idt) - 1;
  dtr.base = (u64)idt;
  lidt((void *)&dtr.limit);

  // Load per-CPU GDT
  memmove(c->gdt, bootgdt, sizeof(bootgdt));
  dtr.limit = sizeof(c->gdt) - 1;
  dtr.base = (u64)c->gdt;
  lgdt((void *)&dtr.limit);

  __asm volatile("movl $0, %%eax\n"
                 "movw %%ax, %%es\n"
                 "movw %%ax, %%ss\n"
                 "movw %%ax, %%ds\n"
                 "mov %%rsp, %%rax\n"
                 "push $(0)\n"
                 "push %%rax\n"
                 "pushf\n"
                 "push %0\n"
                 "push $1f\n"
                 "iretq; 1:" : : "r"((u64)KCSEG) : "rax");

  u64 base = (u64) &c->ts;
  c->gdt[TSSSEG>>3] = (struct segdesc)
    SEGDESC(base, (sizeof(c->ts)-1), SEG_P|SEG_TSS64A);
  c->gdt[(TSSSEG>>3)+1] = (struct segdesc) SEGDESCHI(base);
  // "If the I/O bit map base address is greater than or equal to the TSS
  // segment limit, there is no I/O permission map, and all I/O instructions
  // generate exceptions when the CPL is greater than the current IOPL."
  c->ts.iomba = 0xffff;
  ltr(TSSSEG);

  // When executing a syscall instruction the CPU sets the SS selector
  // to (star >> 32) + 8 and the CS selector to (star >> 32).
  // When executing a sysret instruction the CPU sets the SS selector
  // to (star >> 48) + 8 and the CS selector to (star >> 48) + 16.
  u64 star = ((((u64)UCSEG|0x3) - 16)<<48)|((u64)KCSEG<<32);
  writemsr(MSR_STAR, star);
  writemsr(MSR_LSTAR, (u64)&sysentry);
  writemsr(MSR_SFMASK, FL_TF | FL_IF);

  extern u64 text;
  writefs(UDSEG);
  writemsr(MSR_FS_BASE, (u64)&text);
}

// Pushcli/popcli are like cli/sti except that they are matched:
// it takes two popcli to undo two pushcli.  Also, if interrupts
// are off, then pushcli, popcli leaves them off.
void
pushcli(void)
{
  u64 rflags;

  rflags = readrflags();
  cli();
  if(mycpu()->ncli++ == 0)
    mycpu()->intena = rflags & FL_IF;
}

void
popcli(void)
{
  if(readrflags()&FL_IF)
    panic("popcli - interruptible");
  if(--mycpu()->ncli < 0)
    panic("popcli");
  if(mycpu()->ncli == 0 && mycpu()->intena)
    sti();
}

// Record the current call stack in pcs[] by following the %rbp chain.
void
getcallerpcs(void *v, uptr pcs[], int n)
{
  uintptr_t rbp;
  int i;

  rbp = (uintptr_t)v;
  for(i = 0; i < n; i++){
    // Read saved %rip
    uintptr_t saved_rip;
    if (safe_read_vm(&saved_rip, rbp + sizeof(uintptr_t), sizeof(saved_rip)) !=
        sizeof(saved_rip))
      break;
    // Subtract 1 so it points to the call instruction
    pcs[i] = saved_rip - 1;
    // Read saved %rbp
    if (safe_read_vm(&rbp, rbp, sizeof(rbp)) != sizeof(rbp))
      break;
  }
  for(; i < n; i++)
    pcs[i] = 0;
}

bool
irq::reserve(const int *accept_gsi, size_t num_accept)
{
  assert(!valid());
  int gsi = -1;
  if (accept_gsi) {
    for (size_t i = 0; i < num_accept; ++i) {
      if (!irq_info[accept_gsi[i]].in_use) {
        gsi = accept_gsi[i];
        break;
      }
    }
  } else {
    // Find a free GSI.  Start from the top because system-assigned
    // GSI's tend to be low.
    for (int try_gsi = sizeof(irq_info) / sizeof(irq_info[0]) - 1; try_gsi >= 0;
         --try_gsi) {
      if (!irq_info[try_gsi].in_use) {
        gsi = try_gsi;
        break;
      }
    }
  }
  if (gsi == -1)
    // XXX Level-triggered, active-low interrupts can share an IRQ line
    return false;
  irq_info[gsi].in_use = true;
  this->gsi = gsi;
  vector = T_IRQ0 + gsi;
  return true;
}

void
irq::register_handler(irq_handler *handler)
{
  assert(valid());
  assert(vector == gsi + T_IRQ0);
  handler->next = irq_info[gsi].handlers;
  irq_info[gsi].handlers = handler;
}

void
to_stream(class print_stream *s, const struct irq &irq)
{
  if (irq.valid()) {
    s->print("IRQ ", irq.gsi);
    if (irq.level_triggered)
      s->print(irq.active_low ? " (level low)" : " (level high)");
    else
      s->print(irq.active_low ? " (falling edge)" : " (rising edge)");
  } else {
    s->print("invalid IRQ");
  }
}

void
scoped_critical::release_yield()
{
  kstats::inc(&kstats::sched_delayed_tick_count);
  // Clear the yield request and yield
  modify_no_sched_count(-NO_SCHED_COUNT_YIELD_REQUESTED);
  // Below here is racy, strictly speaking, but that's okay.
  yield();
}

bool
check_critical(critical_mask mask)
{
  if (mask == NO_CRITICAL)
    return true;
  bool safe = !(readrflags() & FL_IF);
  if (mask & NO_INT)
    return safe;
  safe = safe || mycpu()->no_sched_count;
  if (mask & NO_SCHED)
    return safe;
  safe = safe || myproc()->cpu_pin;
  if (mask & NO_MIGRATE)
    return safe;
  return false;
}
