// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

// A note on the distribution of code between us and the userspace driver:
// The default location for code is the userspace driver. Reasons for
// putting code here are: implementation requirement (need ring zero to write
// MSRs), stability, and performance. The device driver should do as much
// error checking as possible before calling us.
// Note that we do a lot of verification of the input configuration:
// We don't want to be compromised if the userspace driver gets compromised.

// TODO(dje): wip
// The thought is to use resources (as in ResourceDispatcher), at which point
// this will all get rewritten. Until such time, the goal here is KISS.
// This file contains the lower part of Intel Performance Monitor support that
// must be done in the kernel (so that we can read/write msrs).
// The userspace driver is in system/dev/misc/cpu-trace/intel-pm.c.

// TODO(dje): See Intel Vol 3 18.2.3.1 for hypervisor recommendations.
// TODO(dje): LBR, BTS, et.al. See Intel Vol 3 Chapter 17.
// TODO(dje): PMI mitigations
// TODO(dje): Eventually may wish to virtualize some/all of the MSRs,
//            some have multiple disparate uses.
// TODO(dje): vmo management
// TODO(dje): check hyperthread handling
// TODO(dje): See about reducing two loops (programmable+fixed) into one.
// TODO(dje): If we're using one counter as the trigger, we could skip
// resetting the other counters and instead record the last value (so that we
// can continue to emit the delta into the trace buffer) - assuming the write
// to memory is faster than the wrmsr which is apparently true.
// TODO(dje): rdpmc

#include <arch/arch_ops.h>
#include <arch/mmu.h>
#include <arch/x86.h>
#include <arch/x86/apic.h>
#include <arch/x86/feature.h>
#include <arch/x86/mmu.h>
#include <arch/x86/perf_mon.h>
#include <assert.h>
#include <err.h>
#include <fbl/alloc_checker.h>
#include <fbl/macros.h>
#include <fbl/ref_ptr.h>
#include <fbl/unique_ptr.h>
#include <kernel/mp.h>
#include <kernel/mutex.h>
#include <kernel/stats.h>
#include <kernel/thread.h>
#include <platform.h>
#include <vm/vm.h>
#include <vm/vm_address_region.h>
#include <vm/vm_aspace.h>
#include <lib/ktrace.h>
#include <zircon/device/cpu-trace/cpu-perf.h>
#include <zircon/device/cpu-trace/intel-pm.h>
#include <zircon/ktrace.h>
#include <zircon/mtrace.h>
#include <zircon/thread_annotations.h>
#include <zxcpp/new.h>
#include <pow2.h>
#include <string.h>
#include <trace.h>

#define LOCAL_TRACE 0

// TODO(dje): Freeze-on-PMI doesn't work in skylake.
// This is here for experimentation purposes.
#define TRY_FREEZE_ON_PMI 0

// At a minimum we require Performance Monitoring version 4.
// KISS: Skylake supports version 4.
#define MINIMUM_PERFMON_VERSION 4

// MSRs

#define IA32_PLATFORM_INFO 0xce

#define IA32_PERF_CAPABILITIES 0x345

// The counter MSR addresses are contiguous from here.
#define IA32_PMC_FIRST 0x0c1
// The event selection MSR addresses are contiguous from here.
#define IA32_PERFEVTSEL_FIRST 0x186

#define IA32_FIXED_CTR_CTRL 0x38d

// The fixed counter MSR addresses are contiguous from here.
#define IA32_FIXED_CTR0 0x309

#define IA32_PERF_GLOBAL_CTRL 0x38f
#define IA32_PERF_GLOBAL_STATUS 0x38e
#define IA32_PERF_GLOBAL_OVF_CTRL 0x390
#define IA32_PERF_GLOBAL_STATUS_RESET 0x390 // Yes, same as OVF_CTRL.
#define IA32_PERF_GLOBAL_STATUS_SET 0x391
#define IA32_PERF_GLOBAL_INUSE 0x392

#define IA32_DEBUGCTL 0x1d9

// These aren't constexpr as we iterate to fill in values for each counter.
static uint64_t kGlobalCtrlWritableBits;
static uint64_t kFixedCounterCtrlWritableBits;

static constexpr size_t kMaxRecordSize = sizeof(cpuperf_pc_record_t);

// Commented out values represent currently unsupported features.
// They remain present for documentation purposes.
static constexpr uint64_t kDebugCtrlWritableBits =
    (/*IA32_DEBUGCTL_LBR_MASK |*/
     /*IA32_DEBUGCTL_BTF_MASK |*/
     /*IA32_DEBUGCTL_TR_MASK |*/
     /*IA32_DEBUGCTL_BTS_MASK |*/
     /*IA32_DEBUGCTL_BTINT_MASK |*/
     /*IA32_DEBUGCTL_BTS_OFF_OS_MASK |*/
     /*IA32_DEBUGCTL_BTS_OFF_USR_MASK |*/
     /*IA32_DEBUGCTL_FREEZE_LBRS_ON_PMI_MASK |*/
#if TRY_FREEZE_ON_PMI
     IA32_DEBUGCTL_FREEZE_PERFMON_ON_PMI_MASK |
#endif
     /*IA32_DEBUGCTL_FREEZE_WHILE_SMM_EN_MASK |*/
     /*IA32_DEBUGCTL_RTM_MASK |*/
     0);
static constexpr uint64_t kEventSelectWritableBits =
    (IA32_PERFEVTSEL_EVENT_SELECT_MASK |
     IA32_PERFEVTSEL_UMASK_MASK |
     IA32_PERFEVTSEL_USR_MASK |
     IA32_PERFEVTSEL_OS_MASK |
     IA32_PERFEVTSEL_E_MASK |
     IA32_PERFEVTSEL_PC_MASK |
     IA32_PERFEVTSEL_INT_MASK |
     IA32_PERFEVTSEL_ANY_MASK |
     IA32_PERFEVTSEL_EN_MASK |
     IA32_PERFEVTSEL_INV_MASK |
     IA32_PERFEVTSEL_CMASK_MASK);

static bool supports_perfmon = false;

static uint32_t perfmon_version = 0;
static uint32_t perfmon_num_programmable_counters = 0;
static uint32_t perfmon_programmable_counter_width = 0;
static uint32_t perfmon_num_fixed_counters = 0;
static uint32_t perfmon_fixed_counter_width = 0;
static uint32_t perfmon_unsupported_events = 0;
static uint32_t perfmon_capabilities = 0;

// Maximum counter values, derived from their width.
static uint64_t perfmon_max_fixed_counter_value = 0;
static uint64_t perfmon_max_programmable_counter_value = 0;

// Counter bits in GLOBAL_STATUS to check on each interrupt.
static uint64_t perfmon_counter_status_bits = 0;

struct PerfmonCpuData {
    // The trace buffer, passed in from userspace.
    fbl::RefPtr<VmObject> buffer_vmo;
    size_t buffer_size = 0;

    // The trace buffer when mapped into kernel space.
    // This is only done while the trace is running.
    fbl::RefPtr<VmMapping> buffer_mapping;
    cpuperf_buffer_header_t* buffer_start = 0;
    void* buffer_end = 0;

    // The next record to fill.
    cpuperf_record_header_t* buffer_next = nullptr;
} __CPU_ALIGN;

struct PerfmonState {
    static zx_status_t Create(unsigned n_cpus, fbl::unique_ptr<PerfmonState>* out_state);
    explicit PerfmonState(unsigned n_cpus);
    ~PerfmonState();

    // IA32_PERF_GLOBAL_CTRL
    uint64_t global_ctrl = 0;

    // IA32_FIXED_CTR_CTRL
    uint64_t fixed_ctrl = 0;

    // IA32_DEBUGCTL
    uint64_t debug_ctrl = 0;

    // See intel-pm.h:zx_x86_ipm_config_t.
    cpuperf_event_id_t timebase_id = CPUPERF_EVENT_ID_NONE;

    // The number of each kind of counter in use, so we don't have to iterate
    // over the entire arrays.
    unsigned num_used_fixed = 0;
    unsigned num_used_programmable = 0;

    // Number of entries in |cpu_data|.
    const unsigned num_cpus;

    // An array with one entry for each cpu.
    // TODO(dje): Ideally this would be something like
    // fbl::unique_ptr<PerfmonCpuData[]> cpu_data;
    // but that will need to wait for a "new" that handles aligned allocs.
    PerfmonCpuData* cpu_data = nullptr;

    // |fixed_hw_map[i]| is the h/w fixed counter number.
    // This is used to only look at fixed counters that are used.
    unsigned fixed_hw_map[IPM_MAX_FIXED_COUNTERS] = {};

    // The counters are reset to this at the start.
    // And again for those that are reset on overflow.
    uint64_t fixed_initial_value[IPM_MAX_FIXED_COUNTERS] = {};
    uint64_t programmable_initial_value[IPM_MAX_PROGRAMMABLE_COUNTERS] = {};

    // Flags for each counter, IPM_CONFIG_FLAG_*.
    uint32_t fixed_flags[IPM_MAX_FIXED_COUNTERS] = {};
    uint32_t programmable_flags[IPM_MAX_PROGRAMMABLE_COUNTERS] = {};

    // The ids for each of the in-use counters, or zero if not used.
    // These are passed in from the driver and then written to the buffer,
    // but otherwise have no meaning to us.
    // All in-use entries appear consecutively.
    cpuperf_event_id_t fixed_ids[IPM_MAX_FIXED_COUNTERS] = {};
    cpuperf_event_id_t programmable_ids[IPM_MAX_PROGRAMMABLE_COUNTERS] = {};

    // IA32_PERFEVTSEL_*
    uint64_t events[IPM_MAX_PROGRAMMABLE_COUNTERS] = {};
};

static fbl::Mutex perfmon_lock;

static fbl::unique_ptr<PerfmonState> perfmon_state TA_GUARDED(perfmon_lock);

// This is accessed atomically as it is also accessed by the PMI handler.
static int perfmon_active = false;

zx_status_t PerfmonState::Create(unsigned n_cpus, fbl::unique_ptr<PerfmonState>* out_state) {
    fbl::AllocChecker ac;
    auto state = fbl::unique_ptr<PerfmonState>(new (&ac) PerfmonState(n_cpus));
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    size_t space_needed = sizeof(PerfmonCpuData) * n_cpus;
    auto cpu_data = reinterpret_cast<PerfmonCpuData*>(
        memalign(alignof(PerfmonCpuData), space_needed));
    if (!cpu_data)
        return ZX_ERR_NO_MEMORY;

    for (unsigned cpu = 0; cpu < n_cpus; ++cpu) {
        new (&cpu_data[cpu]) PerfmonCpuData();
    }

    state->cpu_data = cpu_data;
    *out_state = fbl::move(state);
    return ZX_OK;
}

PerfmonState::PerfmonState(unsigned n_cpus)
        : num_cpus(n_cpus) { }

PerfmonState::~PerfmonState() {
    DEBUG_ASSERT(!atomic_load(&perfmon_active));
    if (cpu_data) {
        for (unsigned cpu = 0; cpu < num_cpus; ++cpu) {
            auto data = &cpu_data[cpu];
            data->~PerfmonCpuData();
        }
        free(cpu_data);
    }
}

void x86_perfmon_init(void)
{
    struct cpuid_leaf leaf;
    if (!x86_get_cpuid_subleaf(X86_CPUID_PERFORMANCE_MONITORING, 0, &leaf)) {
        return;
    }

    perfmon_version = leaf.a & 0xff;

    perfmon_num_programmable_counters = (leaf.a >> 8) & 0xff;
    if (perfmon_num_programmable_counters > IPM_MAX_PROGRAMMABLE_COUNTERS) {
        TRACEF("perfmon: unexpected num programmable counters %u in cpuid.0AH\n",
               perfmon_num_programmable_counters);
        return;
    }
    perfmon_programmable_counter_width = (leaf.a >> 16) & 0xff;
    // The <16 test is just something simple to ensure it's usable.
    if (perfmon_programmable_counter_width < 16 ||
        perfmon_programmable_counter_width > 64) {
        TRACEF("perfmon: unexpected programmable counter width %u in cpuid.0AH\n",
               perfmon_programmable_counter_width);
        return;
    }
    perfmon_max_programmable_counter_value = ~0ul;
    if (perfmon_programmable_counter_width < 64) {
        perfmon_max_programmable_counter_value =
            (1ul << perfmon_programmable_counter_width) - 1;
    }

    unsigned ebx_length = (leaf.a >> 24) & 0xff;
    if (ebx_length > 7) {
        TRACEF("perfmon: unexpected value %u in cpuid.0AH.EAH[31..24]\n",
               ebx_length);
        return;
    }
    perfmon_unsupported_events = leaf.b & ((1u << ebx_length) - 1);

    perfmon_num_fixed_counters = leaf.d & 0x1f;
    if (perfmon_num_fixed_counters > IPM_MAX_FIXED_COUNTERS) {
        TRACEF("perfmon: unexpected num fixed counters %u in cpuid.0AH\n",
               perfmon_num_fixed_counters);
        return;
    }
    perfmon_fixed_counter_width = (leaf.d >> 5) & 0xff;
    // The <16 test is just something simple to ensure it's usable.
    if (perfmon_fixed_counter_width < 16 ||
        perfmon_fixed_counter_width > 64) {
        TRACEF("perfmon: unexpected fixed counter width %u in cpuid.0AH\n",
               perfmon_fixed_counter_width);
        return;
    }
    perfmon_max_fixed_counter_value = ~0ul;
    if (perfmon_fixed_counter_width < 64) {
        perfmon_max_fixed_counter_value =
            (1ul << perfmon_fixed_counter_width) - 1;
    }

    supports_perfmon = perfmon_version >= MINIMUM_PERFMON_VERSION;

    if (x86_feature_test(X86_FEATURE_PDCM)) {
        perfmon_capabilities = static_cast<uint32_t>(read_msr(IA32_PERF_CAPABILITIES));
    }

    perfmon_counter_status_bits = 0;
    for (unsigned i = 0; i < perfmon_num_programmable_counters; ++i)
        perfmon_counter_status_bits |= IA32_PERF_GLOBAL_STATUS_PMC_OVF_MASK(i);
    for (unsigned i = 0; i < perfmon_num_fixed_counters; ++i)
        perfmon_counter_status_bits |= IA32_PERF_GLOBAL_STATUS_FIXED_OVF_MASK(i);

    kGlobalCtrlWritableBits = 0;
    for (unsigned i = 0; i < perfmon_num_programmable_counters; ++i)
        kGlobalCtrlWritableBits |= IA32_PERF_GLOBAL_CTRL_PMC_EN_MASK(i);
    for (unsigned i = 0; i < perfmon_num_fixed_counters; ++i)
        kGlobalCtrlWritableBits |= IA32_PERF_GLOBAL_CTRL_FIXED_EN_MASK(i);
    kFixedCounterCtrlWritableBits = 0;
    for (unsigned i = 0; i < perfmon_num_fixed_counters; ++i) {
        kFixedCounterCtrlWritableBits |= IA32_FIXED_CTR_CTRL_EN_MASK(i);
        kFixedCounterCtrlWritableBits |= IA32_FIXED_CTR_CTRL_ANY_MASK(i);
        kFixedCounterCtrlWritableBits |= IA32_FIXED_CTR_CTRL_PMI_MASK(i);
    }
}

static void x86_perfmon_clear_overflow_indicators() {
    uint64_t value = (IA32_PERF_GLOBAL_OVF_CTRL_CLR_COND_CHGD_MASK |
                      IA32_PERF_GLOBAL_OVF_CTRL_DS_BUFFER_CLR_OVF_MASK |
                      IA32_PERF_GLOBAL_OVF_CTRL_UNCORE_CLR_OVF_MASK);

    // This function isn't performance critical enough to precompute this.
    for (unsigned i = 0; i < perfmon_num_programmable_counters; ++i) {
        value |= IA32_PERF_GLOBAL_OVF_CTRL_PMC_CLR_OVF_MASK(i);
    }

    for (unsigned i = 0; i < perfmon_num_fixed_counters; ++i) {
        value |= IA32_PERF_GLOBAL_OVF_CTRL_FIXED_CTR_CLR_OVF_MASK(i);
    }

    write_msr(IA32_PERF_GLOBAL_OVF_CTRL, value);
}

// Return the h/w register number for fixed event id |id|
// or IPM_MAX_FIXED_COUNTERS if not found.
static unsigned x86_perfmon_lookup_fixed_counter(cpuperf_event_id_t id) {
    if (CPUPERF_EVENT_ID_UNIT(id) != CPUPERF_UNIT_FIXED)
        return IPM_MAX_FIXED_COUNTERS;
    switch (CPUPERF_EVENT_ID_EVENT(id)) {
#define DEF_FIXED_EVENT(symbol, id, regnum, flags, name, description) \
    case id: return regnum;
#include <zircon/device/cpu-trace/intel-pm-events.inc>
    default: return IPM_MAX_FIXED_COUNTERS;
    }
}

static void x86_perfmon_write_header(cpuperf_record_header_t* hdr,
                                     cpuperf_record_type_t type,
                                     cpuperf_event_id_t event,
                                     zx_time_t time) {
    hdr->type = type;
    hdr->reserved_flags = 0;
    hdr->event = event;
    hdr->reserved = 0;
    hdr->time = time;
}

static cpuperf_record_header_t* x86_perfmon_write_tick_record(
        cpuperf_record_header_t* hdr,
        cpuperf_event_id_t counter, zx_time_t time) {
    auto rec = reinterpret_cast<cpuperf_tick_record_t*>(hdr);
    x86_perfmon_write_header(&rec->header, CPUPERF_RECORD_TICK,
                             counter, time);
    ++rec;
    return reinterpret_cast<cpuperf_record_header_t*>(rec);
}

static cpuperf_record_header_t* x86_perfmon_write_value_record(
        cpuperf_record_header_t* hdr,
        cpuperf_event_id_t counter, zx_time_t time,
        uint64_t value) {
    auto rec = reinterpret_cast<cpuperf_value_record_t*>(hdr);
    x86_perfmon_write_header(&rec->header, CPUPERF_RECORD_VALUE,
                             counter, time);
    rec->value = value;
    ++rec;
    return reinterpret_cast<cpuperf_record_header_t*>(rec);
}

static cpuperf_record_header_t* x86_perfmon_write_pc_record(
        cpuperf_record_header_t* hdr,
        cpuperf_event_id_t counter, zx_time_t time,
        uint64_t cr3, uint64_t pc) {
    auto rec = reinterpret_cast<cpuperf_pc_record_t*>(hdr);
    x86_perfmon_write_header(&rec->header, CPUPERF_RECORD_PC, counter, time);
    rec->aspace = cr3;
    rec->pc = pc;
    ++rec;
    return reinterpret_cast<cpuperf_record_header_t*>(rec);
}

zx_status_t x86_ipm_get_properties(zx_x86_ipm_properties_t* props) {
    fbl::AutoLock al(&perfmon_lock);

    if (!supports_perfmon)
        return ZX_ERR_NOT_SUPPORTED;
    props->pm_version = perfmon_version;
    props->num_fixed_counters = perfmon_num_fixed_counters;
    props->num_programmable_counters = perfmon_num_programmable_counters;
    props->fixed_counter_width = perfmon_fixed_counter_width;
    props->programmable_counter_width = perfmon_programmable_counter_width;
    props->perf_capabilities = perfmon_capabilities;
    return ZX_OK;
}

zx_status_t x86_ipm_init() {
    fbl::AutoLock al(&perfmon_lock);

    if (!supports_perfmon)
        return ZX_ERR_NOT_SUPPORTED;
    if (atomic_load(&perfmon_active))
        return ZX_ERR_BAD_STATE;
    if (perfmon_state)
        return ZX_ERR_BAD_STATE;

    fbl::unique_ptr<PerfmonState> state;
    auto status = PerfmonState::Create(arch_max_num_cpus(), &state);
    if (status != ZX_OK)
        return status;

    perfmon_state = fbl::move(state);
    return ZX_OK;
}

zx_status_t x86_ipm_assign_buffer(uint32_t cpu, fbl::RefPtr<VmObject> vmo) {
    fbl::AutoLock al(&perfmon_lock);

    if (!supports_perfmon)
        return ZX_ERR_NOT_SUPPORTED;
    if (atomic_load(&perfmon_active))
        return ZX_ERR_BAD_STATE;
    if (!perfmon_state)
        return ZX_ERR_BAD_STATE;
    if (cpu >= perfmon_state->num_cpus)
        return ZX_ERR_INVALID_ARGS;

    // A simple safe approximation of the minimum size needed.
    size_t min_size_needed = sizeof(cpuperf_buffer_header_t);
    min_size_needed += CPUPERF_MAX_COUNTERS * kMaxRecordSize;
    if (vmo->size() < min_size_needed)
        return ZX_ERR_INVALID_ARGS;

    auto data = &perfmon_state->cpu_data[cpu];
    data->buffer_vmo = vmo;
    data->buffer_size = vmo->size();
    // The buffer is mapped into kernelspace later.

    return ZX_OK;
}

static zx_status_t x86_ipm_verify_control_config(
        const zx_x86_ipm_config_t* config) {
#if TRY_FREEZE_ON_PMI
    if (!(config->debug_ctrl & IA32_DEBUGCTL_FREEZE_PERFMON_ON_PMI_MASK)) {
        // IWBN to pass back a hint, instead of either nothing or
        // a log message.
        TRACEF("IA32_DEBUGCTL_FREEZE_PERFMON_ON_PMI not set\n");
        return ZX_ERR_INVALID_ARGS;
    }
#else
    if (config->debug_ctrl & IA32_DEBUGCTL_FREEZE_PERFMON_ON_PMI_MASK) {
        TRACEF("IA32_DEBUGCTL_FREEZE_PERFMON_ON_PMI is set\n");
        return ZX_ERR_INVALID_ARGS;
    }
#endif

    if (config->global_ctrl & ~kGlobalCtrlWritableBits) {
        TRACEF("Non writable bits set in |global_ctrl|\n");
        return ZX_ERR_INVALID_ARGS;
    }
    if (config->fixed_ctrl & ~kFixedCounterCtrlWritableBits) {
        TRACEF("Non writable bits set in |fixed_ctrl|\n");
        return ZX_ERR_INVALID_ARGS;
    }
    if (config->debug_ctrl & ~kDebugCtrlWritableBits) {
        TRACEF("Non writable bits set in |debug_ctrl|\n");
        return ZX_ERR_INVALID_ARGS;
    }

    return ZX_OK;
}

static zx_status_t x86_ipm_verify_fixed_config(
        const zx_x86_ipm_config_t* config, unsigned* out_num_used) {
    bool seen_last = false;
    unsigned num_used = perfmon_num_fixed_counters;
    for (unsigned i = 0; i < perfmon_num_fixed_counters; ++i) {
        cpuperf_event_id_t id = config->fixed_ids[i];
        if (id != 0 && seen_last) {
            TRACEF("Active fixed events not front-filled\n");
            return ZX_ERR_INVALID_ARGS;
        }
        if (id == 0) {
            if (!seen_last)
                num_used = i;
            seen_last = true;
        }
        if (seen_last) {
            if (config->fixed_initial_value[i] != 0) {
                TRACEF("Unused |fixed_initial_value[%u]| not zero\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
            if (config->fixed_flags[i] != 0) {
                TRACEF("Unused |fixed_flags[%u]| not zero\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
        } else {
            if (config->fixed_initial_value[i] > perfmon_max_fixed_counter_value) {
                TRACEF("Initial value too large for |fixed_initial_value[%u]|\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
            if (config->fixed_flags[i] & ~IPM_CONFIG_FLAG_MASK) {
                TRACEF("Unused bits set in |fixed_flags[%u]|\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
            unsigned hw_regnum = x86_perfmon_lookup_fixed_counter(id);
            if (hw_regnum == IPM_MAX_FIXED_COUNTERS) {
                TRACEF("Invalid fixed counter id |fixed_ids[%u]|\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
        }
    }

    *out_num_used = num_used;
    return ZX_OK;
}

static zx_status_t x86_ipm_verify_programmable_config(
        const zx_x86_ipm_config_t* config, unsigned* out_num_used) {
    bool seen_last = false;
    unsigned num_used = perfmon_num_programmable_counters;
    for (unsigned i = 0; i < perfmon_num_programmable_counters; ++i) {
        cpuperf_event_id_t id = config->programmable_ids[i];
        if (id != 0 && seen_last) {
            TRACEF("Active programmable events not front-filled\n");
            return ZX_ERR_INVALID_ARGS;
        }
        if (id == 0) {
            if (!seen_last)
                num_used = i;
            seen_last = true;
        }
        if (seen_last) {
            if (config->programmable_events[i] != 0) {
                TRACEF("Unused |programmable_events[%u]| not zero\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
            if (config->programmable_initial_value[i] != 0) {
                TRACEF("Unused |programmable_initial_value[%u]| not zero\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
            if (config->programmable_flags[i] != 0) {
                TRACEF("Unused |programmable_flags[%u]| not zero\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
        } else {
            if (config->programmable_events[i] & ~kEventSelectWritableBits) {
                TRACEF("Non writable bits set in |programmable_events[%u]|\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
            if (config->programmable_initial_value[i] > perfmon_max_programmable_counter_value) {
                TRACEF("Initial value too large for |programmable_initial_value[%u]|\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
            if (config->programmable_flags[i] & ~IPM_CONFIG_FLAG_MASK) {
                TRACEF("Unused bits set in |programmable_flags[%u]|\n", i);
                return ZX_ERR_INVALID_ARGS;
            }
        }
    }

    *out_num_used = num_used;
    return ZX_OK;
}

// Stage the configuration for later activation by START.
// One of the main goals of this function is to verify the provided config
// is ok, e.g., it won't cause us to crash.
zx_status_t x86_ipm_stage_config(const zx_x86_ipm_config_t* config) {
    fbl::AutoLock al(&perfmon_lock);

    if (!supports_perfmon)
        return ZX_ERR_NOT_SUPPORTED;
    if (atomic_load(&perfmon_active))
        return ZX_ERR_BAD_STATE;
    if (!perfmon_state)
        return ZX_ERR_BAD_STATE;

    auto state = perfmon_state.get();

    auto status = x86_ipm_verify_control_config(config);
    if (status != ZX_OK)
        return status;

    unsigned num_used_fixed;
    status = x86_ipm_verify_fixed_config(config, &num_used_fixed);
    if (status != ZX_OK)
        return status;
    state->num_used_fixed = num_used_fixed;

    unsigned num_used_programmable;
    status = x86_ipm_verify_programmable_config(config, &num_used_programmable);
    if (status != ZX_OK)
        return status;
    state->num_used_programmable = num_used_programmable;

    state->global_ctrl = config->global_ctrl;
    static_assert(sizeof(state->events) == sizeof(config->programmable_events), "");
    memcpy(state->events, config->programmable_events, sizeof(state->events));
    state->fixed_ctrl = config->fixed_ctrl;
    state->debug_ctrl = config->debug_ctrl;

    static_assert(sizeof(state->programmable_initial_value) ==
                  sizeof(config->programmable_initial_value), "");
    memcpy(state->programmable_initial_value, config->programmable_initial_value,
           sizeof(state->programmable_initial_value));
    static_assert(sizeof(state->fixed_initial_value) ==
                  sizeof(config->fixed_initial_value), "");
    memcpy(state->fixed_initial_value, config->fixed_initial_value,
           sizeof(state->fixed_initial_value));

    static_assert(sizeof(state->programmable_flags) ==
                  sizeof(config->programmable_flags), "");
    memcpy(state->programmable_flags, config->programmable_flags,
           sizeof(state->programmable_flags));
    static_assert(sizeof(state->fixed_flags) ==
                  sizeof(config->fixed_flags), "");
    memcpy(state->fixed_flags, config->fixed_flags,
           sizeof(state->fixed_flags));

    static_assert(sizeof(state->programmable_ids) ==
                  sizeof(config->programmable_ids), "");
    memcpy(state->programmable_ids, config->programmable_ids,
           sizeof(state->programmable_ids));
    static_assert(sizeof(state->fixed_ids) ==
                  sizeof(config->fixed_ids), "");
    memcpy(state->fixed_ids, config->fixed_ids,
           sizeof(state->fixed_ids));

    for (unsigned i = 0; i < countof(state->fixed_hw_map); ++i) {
        state->fixed_hw_map[i] = x86_perfmon_lookup_fixed_counter(config->fixed_ids[i]);
    }

    return ZX_OK;
}

static void x86_ipm_unmap_buffers_locked(PerfmonState* state) {
    unsigned num_cpus = state->num_cpus;
    for (unsigned cpu = 0; cpu < num_cpus; ++cpu) {
        auto data = &state->cpu_data[cpu];
        if (data->buffer_start) {
            data->buffer_mapping->Destroy();
        }
        data->buffer_mapping.reset();
        data->buffer_start = nullptr;
        data->buffer_end = nullptr;
        data->buffer_next = nullptr;
    }
}

static zx_status_t x86_ipm_map_buffers_locked(PerfmonState* state) {
    unsigned num_cpus = state->num_cpus;
    zx_status_t status = ZX_OK;
    for (unsigned cpu = 0; cpu < num_cpus; ++cpu) {
        auto data = &state->cpu_data[cpu];
        // Heads up: The logic is off if |vmo_offset| is non-zero.
        const uint64_t vmo_offset = 0;
        const size_t size = data->buffer_size;
        const uint arch_mmu_flags = ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;
        const char* name = "ipm-buffer";
        status = VmAspace::kernel_aspace()->RootVmar()->CreateVmMapping(
            0 /* ignored */, size, 0 /* align pow2 */, 0 /* vmar flags */,
            data->buffer_vmo, vmo_offset, arch_mmu_flags, name,
            &data->buffer_mapping);
        if (status != ZX_OK) {
            TRACEF("error %d mapping buffer: cpu %u, size 0x%zx\n",
                   status, cpu, size);
            break;
        }
        // Pass true for |commit| so that we get our pages mapped up front.
        // Otherwise we'll need to allow for a page fault to happen in the
        // PMI handler.
        status = data->buffer_mapping->MapRange(vmo_offset, size, true);
        if (status != ZX_OK) {
            TRACEF("error %d mapping range: cpu %u, size 0x%zx\n",
                   status, cpu, size);
            data->buffer_mapping->Destroy();
            data->buffer_mapping.reset();
            break;
        }
        data->buffer_start = reinterpret_cast<cpuperf_buffer_header_t*>(
            data->buffer_mapping->base() + vmo_offset);
        data->buffer_end = reinterpret_cast<char*>(data->buffer_start) + size;
        TRACEF("buffer mapped: cpu %u, start %p, end %p\n",
               cpu, data->buffer_start, data->buffer_end);

        auto hdr = data->buffer_start;
        hdr->version = CPUPERF_BUFFER_VERSION;
        hdr->arch = CPUPERF_BUFFER_ARCH_X86_64;
        hdr->flags = 0;
        hdr->ticks_per_second = ticks_per_second();
        hdr->capture_end = sizeof(*hdr);
        data->buffer_next = reinterpret_cast<cpuperf_record_header_t*>(
            reinterpret_cast<char*>(data->buffer_start) + hdr->capture_end);
    }
    if (status != ZX_OK) {
        x86_ipm_unmap_buffers_locked(state);
    }
    return status;
}

// This is invoked via mp_sync_exec which thread safety analysis cannot follow.
static void x86_ipm_start_cpu_task(void* raw_context) TA_NO_THREAD_SAFETY_ANALYSIS {
    DEBUG_ASSERT(arch_ints_disabled());
    DEBUG_ASSERT(!atomic_load(&perfmon_active) && raw_context);

    auto state = reinterpret_cast<PerfmonState*>(raw_context);

    for (unsigned i = 0; i < state->num_used_fixed; ++i) {
        unsigned hw_num = state->fixed_hw_map[i];
        DEBUG_ASSERT(hw_num < perfmon_num_fixed_counters);
        write_msr(IA32_FIXED_CTR0 + hw_num, state->fixed_initial_value[i]);
    }
    write_msr(IA32_FIXED_CTR_CTRL, state->fixed_ctrl);

    for (unsigned i = 0; i < state->num_used_programmable; ++i) {
        // Ensure PERFEVTSEL.EN is zero before resetting the counter value,
        // h/w requires it (apparently even if global ctrl is off).
        write_msr(IA32_PERFEVTSEL_FIRST + i, 0);
        // The counter must be written before PERFEVTSEL.EN is set to 1.
        write_msr(IA32_PMC_FIRST + i, state->programmable_initial_value[i]);
        write_msr(IA32_PERFEVTSEL_FIRST + i, state->events[i]);
    }

    write_msr(IA32_DEBUGCTL, state->debug_ctrl);

    apic_pmi_unmask();

    // Enable counters as late as possible so that our setup doesn't contribute
    // to the data.
    write_msr(IA32_PERF_GLOBAL_CTRL, state->global_ctrl);
}

// Begin collecting data.

zx_status_t x86_ipm_start() {
    fbl::AutoLock al(&perfmon_lock);

    if (!supports_perfmon)
        return ZX_ERR_NOT_SUPPORTED;
    if (atomic_load(&perfmon_active))
        return ZX_ERR_BAD_STATE;
    if (!perfmon_state)
        return ZX_ERR_BAD_STATE;

    // Sanity check the buffers and map them in.
    // This is deferred until now so that they are mapped in as minimally as
    // necessary.
    // TODO(dje): OTOH one might want to start/stop/start/stop/... and
    // continually mapping/unmapping will be painful. Revisit when things
    // settle down.
    auto state = perfmon_state.get();
    auto status = x86_ipm_map_buffers_locked(state);
    if (status != ZX_OK)
        return status;

    TRACEF("Enabling perfmon, %u fixed, %u programmable\n",
           state->num_used_fixed, state->num_used_programmable);
    if (LOCAL_TRACE) {
        LTRACEF("global ctrl: 0x%" PRIx64 ", fixed ctrl: 0x%" PRIx64 "\n",
                state->global_ctrl, state->fixed_ctrl);
        for (unsigned i = 0; i < state->num_used_fixed; ++i) {
            LTRACEF("fixed[%u]: num %u, initial 0x%" PRIx64 "\n",
                    i, state->fixed_hw_map[i], state->fixed_initial_value[i]);
        }
        for (unsigned i = 0; i < state->num_used_programmable; ++i) {
            LTRACEF("programmable[%u]: id 0x%x, initial 0x%" PRIx64 "\n",
                    i, state->programmable_ids[i],
                    state->programmable_initial_value[i]);
        }
    }

    ktrace(TAG_IPM_START, 0, 0, 0, 0);
    mp_sync_exec(MP_IPI_TARGET_ALL, 0, x86_ipm_start_cpu_task, state);
    atomic_store(&perfmon_active, true);
    return ZX_OK;
}

// This is invoked via mp_sync_exec which thread safety analysis cannot follow.
static void x86_ipm_stop_cpu_task(void* raw_context) TA_NO_THREAD_SAFETY_ANALYSIS {
    // Disable all counters ASAP.
    write_msr(IA32_PERF_GLOBAL_CTRL, 0);
    apic_pmi_mask();

    DEBUG_ASSERT(arch_ints_disabled());
    DEBUG_ASSERT(!atomic_load(&perfmon_active));
    DEBUG_ASSERT(raw_context);

    auto state = reinterpret_cast<PerfmonState*>(raw_context);
    auto cpu = arch_curr_cpu_num();
    auto data = &state->cpu_data[cpu];
    auto now = rdtsc();

    // Retrieve final counter values and write into the trace buffer.

    if (data->buffer_start) {
        LTRACEF("Collecting last data for cpu %u\n", cpu);
        auto hdr = data->buffer_start;
        auto next = data->buffer_next;
        auto last =
            reinterpret_cast<cpuperf_record_header_t*>(data->buffer_end) - 1;

        // If the counter triggers interrupts then the PMI handler will
        // continually reset it to its initial value. To keep things simple
        // just always subtract out the initial value from the current value
        // and write the difference out. For non-interrupt triggering events
        // the user should normally initialize the counter to zero to get
        // correct results.
        // Counters that don't trigger interrupts could overflow and we won't
        // necessarily catch it, but there's nothing we can do about it.
        // We can handle the overflowed-once case, which should catch the
        // vast majority of cases.
        // TODO(dje): Counters that trigger interrupts should never have
        // an overflowed value here, but that's what I'm seeing.

        for (unsigned i = 0; i < state->num_used_programmable; ++i) {
            if (next > last) {
                hdr->flags |= CPUPERF_BUFFER_FLAG_FULL;
                break;
            }
            cpuperf_event_id_t id = state->programmable_ids[i];
            DEBUG_ASSERT(id != 0);
            uint64_t value = read_msr(IA32_PMC_FIRST + i);
            if (value >= state->programmable_initial_value[i]) {
                value -= state->programmable_initial_value[i];
            } else {
                // The max counter value is generally not 64 bits.
                value += (perfmon_max_programmable_counter_value -
                          state->programmable_initial_value[i] + 1);
            }
            next = x86_perfmon_write_value_record(next, id, now, value);
        }
        for (unsigned i = 0; i < state->num_used_fixed; ++i) {
            if (next > last) {
                hdr->flags |= CPUPERF_BUFFER_FLAG_FULL;
                break;
            }
            cpuperf_event_id_t id = state->fixed_ids[i];
            DEBUG_ASSERT(id != 0);
            unsigned hw_num = state->fixed_hw_map[i];
            DEBUG_ASSERT(hw_num < perfmon_num_fixed_counters);
            uint64_t value = read_msr(IA32_FIXED_CTR0 + hw_num);
            if (value >= state->fixed_initial_value[i]) {
                value -= state->fixed_initial_value[i];
            } else {
                // The max counter value is generally not 64 bits.
                value += (perfmon_max_fixed_counter_value -
                          state->fixed_initial_value[i] + 1);
            }
            next = x86_perfmon_write_value_record(next, id, now, value);
        }

        data->buffer_next = next;
        hdr->capture_end =
            reinterpret_cast<char*>(data->buffer_next) -
            reinterpret_cast<char*>(data->buffer_start);

        if (hdr->flags & CPUPERF_BUFFER_FLAG_FULL) {
            LTRACEF("Buffer overflow on cpu %u\n", cpu);
        }
    }

    x86_perfmon_clear_overflow_indicators();
}

// Stop collecting data.
// It's ok to call this multiple times.
// Returns an error if called before ALLOC or after FREE.
zx_status_t x86_ipm_stop() {
    fbl::AutoLock al(&perfmon_lock);

    if (!supports_perfmon)
        return ZX_ERR_NOT_SUPPORTED;
    if (!perfmon_state)
        return ZX_ERR_BAD_STATE;

    TRACEF("Disabling perfmon\n");

    // Do this before anything else so that any PMI interrupts from this point
    // on won't try to access potentially unmapped memory.
    atomic_store(&perfmon_active, false);

    // TODO(dje): Check clobbering of values - user should be able to do
    // multiple stops and still read register values.

    auto state = perfmon_state.get();
    mp_sync_exec(MP_IPI_TARGET_ALL, 0, x86_ipm_stop_cpu_task, state);
    ktrace(TAG_IPM_STOP, 0, 0, 0, 0);

    // x86_ipm_start currently maps the buffers in, so we unmap them here.
    // Make sure to do this after we've turned everything off so that we
    // don't get another PMI after this.
    x86_ipm_unmap_buffers_locked(state);

    return ZX_OK;
}

// Worker for x86_ipm_fini to be executed on all cpus.
// This is invoked via mp_sync_exec which thread safety analysis cannot follow.
static void x86_ipm_reset_task(void* raw_context) TA_NO_THREAD_SAFETY_ANALYSIS {
    DEBUG_ASSERT(arch_ints_disabled());
    DEBUG_ASSERT(!atomic_load(&perfmon_active));
    DEBUG_ASSERT(!raw_context);

    write_msr(IA32_PERF_GLOBAL_CTRL, 0);
    apic_pmi_mask();
    x86_perfmon_clear_overflow_indicators();

    write_msr(IA32_DEBUGCTL, 0);

    for (unsigned i = 0; i < perfmon_num_programmable_counters; ++i) {
        write_msr(IA32_PERFEVTSEL_FIRST + i, 0);
        write_msr(IA32_PMC_FIRST + i, 0);
    }

    write_msr(IA32_FIXED_CTR_CTRL, 0);
    for (unsigned i = 0; i < perfmon_num_fixed_counters; ++i) {
        write_msr(IA32_FIXED_CTR0 + i, 0);
    }
}

// Finish data collection, reset h/w back to initial state and undo
// everything x86_ipm_init did.
// Must be called while tracing is stopped.
// It's ok to call this multiple times.
zx_status_t x86_ipm_fini() {
    fbl::AutoLock al(&perfmon_lock);

    if (!supports_perfmon)
        return ZX_ERR_NOT_SUPPORTED;
    if (atomic_load(&perfmon_active))
        return ZX_ERR_BAD_STATE;

    mp_sync_exec(MP_IPI_TARGET_ALL, 0, x86_ipm_reset_task, nullptr);

    perfmon_state.reset();

    return ZX_OK;
}

// Interrupt handling.

// Helper function so that there is only one place where we enable/disable
// interrupts (our caller).
// Returns true if success, false if buffer is full.

static bool pmi_interrupt_handler(x86_iframe_t *frame, PerfmonState* state) {
    // This is done here instead of in the caller so that it is done *after*
    // we disable the counters.
    CPU_STATS_INC(perf_ints);

    uint cpu = arch_curr_cpu_num();
    auto data = &state->cpu_data[cpu];

    // On x86 zx_ticks_get uses rdtsc.
    zx_time_t now = rdtsc();
    LTRACEF("cpu %u: now %" PRIu64 ", sp %p\n", cpu, now, __GET_FRAME());

    // Rather than continually checking if we have enough space, just check
    // for the maximum amount we'll need.
    size_t space_needed =
        (state->num_used_programmable + state->num_used_fixed) *
        kMaxRecordSize;
    if (reinterpret_cast<char*>(data->buffer_next) + space_needed > data->buffer_end) {
        TRACEF("cpu %u: @%" PRIu64 " pmi buffer full\n", cpu, now);
        data->buffer_start->flags |= CPUPERF_BUFFER_FLAG_FULL;
        return false;
    }

    const uint64_t status = read_msr(IA32_PERF_GLOBAL_STATUS);
    uint64_t bits_to_clear = 0;
    uint64_t cr3 = x86_get_cr3();

    LTRACEF("cpu %u: status 0x%" PRIx64 "\n", cpu, status);

    if (status & perfmon_counter_status_bits) {
#if TRY_FREEZE_ON_PMI
        if (!(status & IA32_PERF_GLOBAL_STATUS_CTR_FRZ_MASK))
            LTRACEF("Eh? status.CTR_FRZ not set\n");
#else
        if (status & IA32_PERF_GLOBAL_STATUS_CTR_FRZ_MASK)
            LTRACEF("Eh? status.CTR_FRZ is set\n");
#endif

        auto next = data->buffer_next;
        bool saw_timebase = false;

        // Note: We don't write "value" records here instead prefering the
        // smaller "tick" record. If the user is tallying the counts the user
        // is required to recognize this and apply the tick rate.

        for (unsigned i = 0; i < state->num_used_programmable; ++i) {
            if (!(status & IA32_PERF_GLOBAL_STATUS_PMC_OVF_MASK(i)))
                continue;
            cpuperf_event_id_t id = state->programmable_ids[i];
            // Counters using a separate timebase are handled below.
            // We shouldn't get an interrupt on a counter using a timebase.
            // TODO(dje): The counter could still overflow. Later.
            if (id == state->timebase_id) {
                saw_timebase = true;
            } else if (state->programmable_flags[i] & IPM_CONFIG_FLAG_TIMEBASE) {
                continue;
            }
            if (state->programmable_flags[i] & IPM_CONFIG_FLAG_PC) {
                next = x86_perfmon_write_pc_record(next, id, now,
                                                   cr3, frame->ip);
            } else {
                next = x86_perfmon_write_tick_record(next, id, now);
            }
            LTRACEF("cpu %u: resetting PMC %u to 0x%" PRIx64 "\n",
                    cpu, i, state->programmable_initial_value[i]);
            write_msr(IA32_PMC_FIRST + i, state->programmable_initial_value[i]);
        }

        for (unsigned i = 0; i < state->num_used_fixed; ++i) {
            unsigned hw_num = state->fixed_hw_map[i];
            DEBUG_ASSERT(hw_num < perfmon_num_fixed_counters);
            if (!(status & IA32_PERF_GLOBAL_STATUS_FIXED_OVF_MASK(hw_num)))
                continue;
            cpuperf_event_id_t id = state->fixed_ids[i];
            // Counters using a separate timebase are handled below.
            // We shouldn't get an interrupt on a counter using a timebase.
            // TODO(dje): The counter could still overflow. Later.
            if (id == state->timebase_id) {
                saw_timebase = true;
            } else if (state->fixed_flags[i] & IPM_CONFIG_FLAG_TIMEBASE) {
                continue;
            }
            if (state->fixed_flags[i] & IPM_CONFIG_FLAG_PC) {
                next = x86_perfmon_write_pc_record(next, id, now,
                                                   cr3, frame->ip);
            } else {
                next = x86_perfmon_write_tick_record(next, id, now);
            }
            LTRACEF("cpu %u: resetting FIXED %u to 0x%" PRIx64 "\n",
                    cpu, hw_num, state->fixed_initial_value[i]);
            write_msr(IA32_FIXED_CTR0 + hw_num, state->fixed_initial_value[i]);
        }

        bits_to_clear |= perfmon_counter_status_bits;

        // Now handle counters that have IPM_CONFIG_FLAG_TIMEBASE set.
        if (saw_timebase) {
            for (unsigned i = 0; i < state->num_used_programmable; ++i) {
                if (!(state->programmable_flags[i] & IPM_CONFIG_FLAG_TIMEBASE))
                    continue;
                cpuperf_event_id_t id = state->programmable_ids[i];
                uint64_t value = read_msr(IA32_PMC_FIRST + i);
                next = x86_perfmon_write_value_record(next, id, now, value);
                // We could leave the counter alone, but it could overflow.
                // Instead reduce the risk and just always reset to zero.
                LTRACEF("cpu %u: resetting PMC %u to 0x%" PRIx64 "\n",
                        cpu, i, state->programmable_initial_value[i]);
                write_msr(IA32_PMC_FIRST + i, state->programmable_initial_value[i]);
            }
            for (unsigned i = 0; i < state->num_used_fixed; ++i) {
                if (!(state->fixed_flags[i] & IPM_CONFIG_FLAG_TIMEBASE))
                    continue;
                cpuperf_event_id_t id = state->fixed_ids[i];
                unsigned hw_num = state->fixed_hw_map[i];
                DEBUG_ASSERT(hw_num < perfmon_num_fixed_counters);
                uint64_t value = read_msr(IA32_FIXED_CTR0 + hw_num);
                next = x86_perfmon_write_value_record(next, id, now, value);
                // We could leave the counter alone, but it could overflow.
                // Instead reduce the risk and just always reset to zero.
                LTRACEF("cpu %u: resetting FIXED %u to 0x%" PRIx64 "\n",
                        cpu, hw_num, state->fixed_initial_value[i]);
                write_msr(IA32_FIXED_CTR0 + hw_num, state->fixed_initial_value[i]);
            }
        }

        data->buffer_next = next;
    }

    // We shouldn't be seeing these set (at least not yet).
    if (status & IA32_PERF_GLOBAL_STATUS_TRACE_TOPA_PMI_MASK)
        LTRACEF("WARNING: GLOBAL_STATUS_TRACE_TOPA_PMI set\n");
    if (status & IA32_PERF_GLOBAL_STATUS_LBR_FRZ_MASK)
        LTRACEF("WARNING: GLOBAL_STATUS_LBR_FRZ set\n");
    if (status & IA32_PERF_GLOBAL_STATUS_DS_BUFFER_OVF_MASK)
        LTRACEF("WARNING: GLOBAL_STATUS_DS_BUFFER_OVF set\n");
    // TODO(dje): IA32_PERF_GLOBAL_STATUS_ASCI_MASK ???

    // Note IA32_PERF_GLOBAL_STATUS_CTR_FRZ_MASK is readonly.
    bits_to_clear |= (IA32_PERF_GLOBAL_STATUS_UNCORE_OVF_MASK |
                      IA32_PERF_GLOBAL_STATUS_COND_CHGD_MASK);

    // TODO(dje): No need to accumulate bits to clear if we're going to clear
    // everything that's set anyway. Kept as is during development.
    bits_to_clear |= status;

    LTRACEF("cpu %u: clearing status bits 0x%" PRIx64 "\n",
            cpu, bits_to_clear);
    write_msr(IA32_PERF_GLOBAL_STATUS_RESET, bits_to_clear);

    // TODO(dje): Always do this test for now. Later conditionally include
    // via some debugging macro.
    uint64_t end_status = read_msr(IA32_PERF_GLOBAL_STATUS);
    if (end_status != 0)
        TRACEF("WARNING: cpu %u: end status 0x%" PRIx64 "\n", cpu, end_status);

    return true;
}

enum handler_return apic_pmi_interrupt_handler(x86_iframe_t *frame) TA_NO_THREAD_SAFETY_ANALYSIS {
    if (!atomic_load(&perfmon_active)) {
        apic_issue_eoi();
        return INT_NO_RESCHEDULE;
    }

#if TRY_FREEZE_ON_PMI
    // Note: We're using perfmon v4 "streamlined" processing here.
    // See Intel vol3 table 17-3 "Legacy and Streamlined Operation with
    // Freeze_Perfmon_On_PMI = 1, Counter Overflowed".
#else
    // Turn all counters off as soon as possible so that the counters that
    // haven't overflowed yet stop counting while we're working.
    // TODO(dje): Is this necessary with CTR_FRZ?
    // Otherwise once we reset the counter that overflowed the other counters
    // will resume counting, and if we don't reset them too then CTR_FRZ
    // remains set and we'll get no more PMIs.
    write_msr(IA32_PERF_GLOBAL_CTRL, 0);
#endif

    DEBUG_ASSERT(arch_ints_disabled());

    auto state = perfmon_state.get();

#if 0
    // TODO(dje): We may want this anyway. If we want to be able to handle
    // page faults inside this handler we'll need to turn interrupts back
    // on. At the moment we can't do this as we don't handle recursive PMIs.
    arch_set_in_int_handler(false);
    arch_enable_ints();
#endif

    bool success = pmi_interrupt_handler(frame, state);

#if 0
    arch_disable_ints();
    arch_set_in_int_handler(true);
#endif

    // This is done here instead of in the caller so that we have full control
    // of when counting is restored.
    apic_issue_eoi();

    // If buffer is full leave everything turned off.
    if (!success) {
#if TRY_FREEZE_ON_PMI
        write_msr(IA32_PERF_GLOBAL_CTRL, 0);
#else
        // Don't restore GLOBAL_CTRL, leave everything turned off.
#endif
    } else {
        // The docs suggest this is only necessary for earlier chips
        // (e.g., not Skylake). Intel vol3 section 10.5.1 "Local Vector Table".
        // However, this is needed for at least Skylake too (at least when
        // Freeze-On-PMI is off).
        apic_pmi_unmask();

#if !TRY_FREEZE_ON_PMI
        // This is the last thing we do: Once we do this the counters
        // will start counting again.
        write_msr(IA32_PERF_GLOBAL_CTRL, state->global_ctrl);
#endif
    }

    return INT_NO_RESCHEDULE;
}
