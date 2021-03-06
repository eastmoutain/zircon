// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <hypervisor/guest.h>

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <fbl/alloc_checker.h>
#include <fbl/type_support.h>
#include <zircon/device/sysinfo.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/hypervisor.h>
#include <zircon/syscalls/port.h>
#include <zircon/threads.h>

static const char kResourcePath[] = "/dev/misc/sysinfo";

// Number of threads reading from the async device port.
static const size_t kNumAsyncWorkers = 1;

static const size_t kMaxSize = 512ull << 30;
static const size_t kMinSize = 4 * (4 << 10);

static zx_status_t guest_get_resource(zx_handle_t* resource) {
    int fd = open(kResourcePath, O_RDWR);
    if (fd < 0)
        return ZX_ERR_IO;
    ssize_t n = ioctl_sysinfo_get_hypervisor_resource(fd, resource);
    close(fd);
    return n < 0 ? ZX_ERR_IO : ZX_OK;
}

zx_status_t Guest::Init(size_t mem_size) {
    zx_status_t status = phys_mem_.Init(mem_size);
    if (status != ZX_OK) {
        fprintf(stderr, "Failed to create guest physical memory.\n");
        return status;
    }

    zx_handle_t resource;
    status = guest_get_resource(&resource);
    if (status != ZX_OK) {
        fprintf(stderr, "Failed to get hypervisor resource.\n");
        return status;
    }

    status = zx_guest_create(resource, 0, phys_mem_.vmo(), &guest_);
    if (status != ZX_OK) {
        fprintf(stderr, "Failed to create guest.\n");
        return status;
    }
    zx_handle_close(resource);

    status = zx::port::create(0, &port_);
    if (status != ZX_OK) {
        fprintf(stderr, "Failed to create port.\n");
        return status;
    }

    for (size_t i = 0; i < kNumAsyncWorkers; ++i) {
        thrd_t thread;
        auto thread_func = +[](void* arg) { return static_cast<Guest*>(arg)->IoThread(); };
        int ret = thrd_create_with_name(&thread, thread_func, this, "io-handler");
        if (ret != thrd_success) {
            fprintf(stderr, "Failed to create io handler thread: %d\n", ret);
            return ZX_ERR_INTERNAL;
        }

        ret = thrd_detach(thread);
        if (ret != thrd_success) {
            fprintf(stderr, "Failed to detach io handler thread: %d\n", ret);
            return ZX_ERR_INTERNAL;
        }
    }

    return ZX_OK;
}

Guest::~Guest() {
    zx_handle_close(guest_);
}

zx_status_t Guest::IoThread() {
    while (true) {
        zx_port_packet_t packet;
        zx_status_t status = port_.wait(ZX_TIME_INFINITE, &packet, 0);
        if (status != ZX_OK) {
            fprintf(stderr, "Failed to wait for device port %d\n", status);
            break;
        }

        uint64_t addr;
        IoValue value;
        switch (packet.type) {
        case ZX_PKT_TYPE_GUEST_IO:
            addr = packet.guest_io.port;
            value.access_size = packet.guest_io.access_size;
            static_assert(sizeof(value.data) >= sizeof(packet.guest_io.data),
                          "IoValue too small to contain zx_packet_guest_io_t.");
            memcpy(value.data, packet.guest_io.data, sizeof(packet.guest_io.data));
            break;
        case ZX_PKT_TYPE_GUEST_BELL:
            addr = packet.guest_bell.addr;
            value.access_size = 0;
            value.u32 = 0;
            break;
        default:
            return ZX_ERR_NOT_SUPPORTED;
        }

        status = trap_key_to_mapping(packet.key)->Write(addr, value);
        if (status != ZX_OK) {
            fprintf(stderr, "Unable to handle packet for device %d\n", status);
            break;
        }
    }

    return ZX_ERR_INTERNAL;
}

static constexpr uint32_t trap_kind(TrapType type) {
    switch (type) {
    case TrapType::MMIO_SYNC:
        return ZX_GUEST_TRAP_MEM;
    case TrapType::MMIO_BELL:
        return ZX_GUEST_TRAP_BELL;
    case TrapType::PIO_SYNC:
    case TrapType::PIO_ASYNC:
        return ZX_GUEST_TRAP_IO;
    default:
        ZX_PANIC("Unhandled TrapType %d.\n",
                 static_cast<fbl::underlying_type<TrapType>::type>(type));
        return 0;
    }
}

static constexpr zx_handle_t get_trap_port(TrapType type, zx_handle_t port) {
    switch (type) {
    case TrapType::PIO_ASYNC:
    case TrapType::MMIO_BELL:
        return port;
    case TrapType::PIO_SYNC:
    case TrapType::MMIO_SYNC:
        return ZX_HANDLE_INVALID;
    default:
        ZX_PANIC("Unhandled TrapType %d.\n",
                 static_cast<fbl::underlying_type<TrapType>::type>(type));
        return ZX_HANDLE_INVALID;
    }
}

zx_status_t Guest::CreateMapping(TrapType type, uint64_t addr, size_t size, uint64_t offset,
                                 IoHandler* handler) {
    fbl::AllocChecker ac;
    auto mapping = fbl::make_unique_checked<IoMapping>(&ac, addr, size, offset, handler);
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    // Set a trap for the IO region. We set the 'key' to be the address of the
    // mapping so that we get the pointer to the mapping provided to us in port
    // packets.
    zx_handle_t port = get_trap_port(type, port_.get());
    uint32_t kind = trap_kind(type);
    uint64_t key = reinterpret_cast<uintptr_t>(mapping.get());
    zx_status_t status = zx_guest_set_trap(guest_, kind, addr, size, port, key);
    if (status != ZX_OK)
        return status;

    mappings_.push_front(fbl::move(mapping));
    return ZX_OK;
}

#if __x86_64__
enum {
    X86_PTE_P = 0x01,  /* P    Valid           */
    X86_PTE_RW = 0x02, /* R/W  Read/Write      */
    X86_PTE_PS = 0x80, /* PS   Page size       */
};

static const size_t kPml4PageSize = 512ull << 30;
static const size_t kPdpPageSize = 1 << 30;
static const size_t kPdPageSize = 2 << 20;
static const size_t kPtPageSize = 4 << 10;
static const size_t kPtesPerPage = PAGE_SIZE / sizeof(uint64_t);

/**
 * Create all page tables for a given page size.
 *
 * @param addr The mapped address of where to write the page table. Must be page-aligned.
 * @param size The size of memory to map.
 * @param l1_page_size The size of pages at this level.
 * @param l1_pte_off The offset of this page table, relative to the start of memory.
 * @param aspace_off The address space offset, used to keep track of mapped address space.
 * @param has_page Whether this level of the page table has associated pages.
 * @param map_flags Flags added to any descriptors directly mapping pages.
 */
static uintptr_t page_table(uintptr_t addr, size_t size, size_t l1_page_size, uintptr_t l1_pte_off,
                            uint64_t* aspace_off, bool has_page, uint64_t map_flags) {
    size_t l1_ptes = (size + l1_page_size - 1) / l1_page_size;
    bool has_l0_aspace = size % l1_page_size != 0;
    size_t l1_pages = (l1_ptes + kPtesPerPage - 1) / kPtesPerPage;
    uintptr_t l0_pte_off = l1_pte_off + l1_pages * PAGE_SIZE;

    uint64_t* pt = (uint64_t*)(addr + l1_pte_off);
    for (size_t i = 0; i < l1_ptes; i++) {
        if (has_page && (!has_l0_aspace || i < l1_ptes - 1)) {
            pt[i] = *aspace_off | X86_PTE_P | X86_PTE_RW | map_flags;
            *aspace_off += l1_page_size;
        } else {
            if (i > 0 && (i % kPtesPerPage == 0))
                l0_pte_off += PAGE_SIZE;
            pt[i] = l0_pte_off | X86_PTE_P | X86_PTE_RW;
        }
    }

    return l0_pte_off;
}
#endif // __x86_64__

zx_status_t guest_create_page_table(uintptr_t addr, size_t size, uintptr_t* end_off) {
    if (size % PAGE_SIZE != 0)
        return ZX_ERR_INVALID_ARGS;
    if (size > kMaxSize || size < kMinSize)
        return ZX_ERR_OUT_OF_RANGE;

#if __x86_64__
    uint64_t aspace_off = 0;
    *end_off = 0;
    *end_off = page_table(addr, size - aspace_off, kPml4PageSize, *end_off, &aspace_off, false, 0);
    *end_off = page_table(addr, size - aspace_off, kPdpPageSize, *end_off, &aspace_off, true, X86_PTE_PS);
    *end_off = page_table(addr, size - aspace_off, kPdPageSize, *end_off, &aspace_off, true, X86_PTE_PS);
    *end_off = page_table(addr, size - aspace_off, kPtPageSize, *end_off, &aspace_off, true, 0);
    return ZX_OK;
#else  // __x86_64__
    return ZX_ERR_NOT_SUPPORTED;
#endif // __x86_64__
}
