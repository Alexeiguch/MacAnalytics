#include <mach/mach.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#include "report.h"

typedef struct {
    uint64_t total;
    uint64_t used;
    uint64_t app;
    uint64_t wired;
    uint64_t compressed;
    uint64_t cached;
    uint64_t free_pages;
    uint64_t swap_used;
    uint64_t swap_total;
    uint32_t pressure;
    bool have_pressure;
    bool have_swap;
} MemoryInfo;


// -----------------------------------------------------------------------------
// Memory
// -----------------------------------------------------------------------------

static bool read_memory(MemoryInfo *info)
{
    memset(info, 0, sizeof(*info));

    if (!sysctl_u64("hw.memsize", &info->total) || info->total == 0)
        return false;

    vm_size_t page_size = 0;

    if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS ||
        page_size == 0)
        return false;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    memset(&vm, 0, sizeof(vm));

    kern_return_t result = host_statistics64(
        mach_host_self(),
        HOST_VM_INFO64,
        (host_info64_t)&vm,
        &count
    );

    if (result != KERN_SUCCESS)
        return false;

    uint64_t internal = vm.internal_page_count;
    uint64_t purgeable = vm.purgeable_count;
    uint64_t app_pages = internal > purgeable ? internal - purgeable : 0;

    info->app = app_pages * (uint64_t)page_size;
    info->wired = (uint64_t)vm.wire_count * (uint64_t)page_size;
    info->compressed = (uint64_t)vm.compressor_page_count * (uint64_t)page_size;
    info->cached = ((uint64_t)vm.external_page_count + purgeable) *
                   (uint64_t)page_size;
    info->free_pages = (uint64_t)vm.free_count * (uint64_t)page_size;
    info->used = info->app + info->wired + info->compressed;

    struct xsw_usage swap;
    size_t swap_len = sizeof(swap);
    memset(&swap, 0, sizeof(swap));

    if (sysctlbyname("vm.swapusage", &swap, &swap_len, NULL, 0) == 0) {
        info->swap_total = swap.xsu_total;
        info->swap_used = swap.xsu_used;
        info->have_swap = true;
    }

    uint32_t pressure = 0;
    size_t pressure_len = sizeof(pressure);

    if (sysctlbyname(
            "kern.memorystatus_vm_pressure_level",
            &pressure,
            &pressure_len,
            NULL,
            0) == 0) {
        info->pressure = pressure;
        info->have_pressure = true;
    }

    return true;
}


static const char *pressure_name(uint32_t level)
{
    switch (level) {
        case 0x1:
            return "Normal";
        case 0x2:
            return "Warning";
        case 0x4:
            return "Urgent";
        case 0x8:
            return "Critical";
        default:
            return "Unknown";
    }
}


static void print_memory_line(const char *label, uint64_t bytes, uint64_t total)
{
    char size[32];
    format_bytes(bytes, size, sizeof(size), false);

    if (total > 0) {
        printf(
            "  %-14s %10s    %5.1f%%\n",
            label,
            size,
            (double)bytes / (double)total * 100.0
        );
    } else {
        printf("  %-14s %10s\n", label, size);
    }
}


void print_memory(void)
{
    MemoryInfo info;

    print_banner("Memory");

    if (!read_memory(&info)) {
        fprintf(stderr, "Could not read memory statistics.\n\n");
        return;
    }

    double used_pct = (double)info.used / (double)info.total * 100.0;
    char used_text[32];
    char total_text[32];
    char bar[32];

    format_bytes(info.used, used_text, sizeof(used_text), false);
    format_bytes(info.total, total_text, sizeof(total_text), false);
    format_bar(used_pct, bar, sizeof(bar));

    printf("  %s used of %s\n", used_text, total_text);
    printf("  %s  %5.1f%%\n\n", bar, used_pct);
    printf("  These match Activity Monitor. 1 GB = 1024 MB.\n\n");

    print_memory_line("App memory", info.app, info.total);
    print_memory_line("Wired", info.wired, info.total);
    print_memory_line("Compressed", info.compressed, info.total);
    print_memory_line("Cached files", info.cached, info.total);
    print_memory_line("Free", info.free_pages, info.total);

    if (info.have_swap) {
        char swap_used[32];
        char swap_total[32];

        format_bytes(info.swap_used, swap_used, sizeof(swap_used), false);
        format_bytes(info.swap_total, swap_total, sizeof(swap_total), false);

        printf("\n  %-14s %s used", "Swap", swap_used);

        if (info.swap_total > 0)
            printf(" of %s", swap_total);

        printf("\n");
    }

    if (info.have_pressure) {
        printf(
            "  %-14s %s\n",
            "Pressure",
            pressure_name(info.pressure)
        );
    }

    printf("\n");
}
