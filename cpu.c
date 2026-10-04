#include <mach/mach.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "report.h"

static const char *core_label(int index, int cpu_count, char *buf, size_t n)
{
    int performance = 0;
    int efficiency = 0;
    bool have_p = sysctl_int("hw.perflevel0.logicalcpu", &performance);
    bool have_e = sysctl_int("hw.perflevel1.logicalcpu", &efficiency);

    if (have_p && have_e &&
        performance > 0 && efficiency > 0 &&
        performance + efficiency == cpu_count) {

        if (index < performance)
            snprintf(buf, n, "P%d", index);
        else
            snprintf(buf, n, "E%d", index - performance);

        return buf;
    }

    snprintf(buf, n, "C%d", index);

    return buf;
}

// -----------------------------------------------------------------------------
// CPU
// -----------------------------------------------------------------------------

static bool copy_cpu_ticks(
    processor_cpu_load_info_data_t **out,
    natural_t *cpu_count
)
{
    processor_info_array_t info = NULL;
    mach_msg_type_number_t count = 0;
    natural_t ncpu = 0;

    kern_return_t result = host_processor_info(
        mach_host_self(),
        PROCESSOR_CPU_LOAD_INFO,
        &ncpu,
        &info,
        &count
    );

    if (result != KERN_SUCCESS || ncpu == 0 || !info)
        return false;

    size_t bytes = (size_t)ncpu * sizeof(processor_cpu_load_info_data_t);
    processor_cpu_load_info_data_t *copy = malloc(bytes);

    if (!copy) {
        vm_deallocate(
            mach_task_self(),
            (vm_address_t)info,
            (vm_size_t)count * sizeof(integer_t)
        );
        return false;
    }

    memcpy(copy, info, bytes);

    vm_deallocate(
        mach_task_self(),
        (vm_address_t)info,
        (vm_size_t)count * sizeof(integer_t)
    );

    *out = copy;
    *cpu_count = ncpu;

    return true;
}


void print_cpu(void)
{
    processor_cpu_load_info_data_t *first = NULL;
    processor_cpu_load_info_data_t *second = NULL;
    natural_t first_count = 0;
    natural_t second_count = 0;

    print_banner("CPU");

    if (!copy_cpu_ticks(&first, &first_count)) {
        fprintf(stderr, "Could not read CPU load.\n\n");
        return;
    }

    sleep_ms(SAMPLE_MS);

    if (!copy_cpu_ticks(&second, &second_count)) {
        free(first);
        fprintf(stderr, "Could not read a second CPU sample.\n\n");
        return;
    }

    natural_t cpu_count = first_count < second_count ? first_count : second_count;

    uint64_t user = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t nice = 0;

    printf(
        "  Sampled over %.2f s. Per-core 100%% means that core was fully busy.\n",
        SAMPLE_MS / 1000.0
    );
    printf("  Overall 100%% means every core was fully busy.\n\n");

    printf("  %-4s %8s  %s\n", "Core", "Busy", "");
    printf("  ----------------------------------------------\n");

    for (natural_t i = 0; i < cpu_count; i++) {
        uint32_t d_user = second[i].cpu_ticks[CPU_STATE_USER] -
                          first[i].cpu_ticks[CPU_STATE_USER];
        uint32_t d_system = second[i].cpu_ticks[CPU_STATE_SYSTEM] -
                            first[i].cpu_ticks[CPU_STATE_SYSTEM];
        uint32_t d_idle = second[i].cpu_ticks[CPU_STATE_IDLE] -
                          first[i].cpu_ticks[CPU_STATE_IDLE];
        uint32_t d_nice = second[i].cpu_ticks[CPU_STATE_NICE] -
                          first[i].cpu_ticks[CPU_STATE_NICE];

        user += d_user;
        system += d_system;
        idle += d_idle;
        nice += d_nice;

        uint32_t total = d_user + d_system + d_idle + d_nice;
        double busy = 0;

        if (total > 0)
            busy = (double)(d_user + d_system + d_nice) / (double)total * 100.0;

        char label[8];
        char bar[32];

        core_label((int)i, (int)cpu_count, label, sizeof(label));
        format_bar(busy, bar, sizeof(bar));

        printf("  %-4s %7.1f%%  %s\n", label, busy, bar);
    }

    uint64_t all = user + system + idle + nice;
    double all_busy = 0;
    double all_user = 0;
    double all_system = 0;
    double all_idle = 0;

    if (all > 0) {
        all_busy = (double)(user + system + nice) / (double)all * 100.0;
        all_user = (double)(user + nice) / (double)all * 100.0;
        all_system = (double)system / (double)all * 100.0;
        all_idle = (double)idle / (double)all * 100.0;
    }

    char bar[32];
    format_bar(all_busy, bar, sizeof(bar));

    printf("\n");
    printf("  Overall    %5.1f%%  %s\n", all_busy, bar);
    printf("  User       %5.1f%%\n", all_user);
    printf("  System     %5.1f%%\n", all_system);
    printf("  Idle       %5.1f%%\n", all_idle);

    double load[3] = { 0, 0, 0 };

    if (getloadavg(load, 3) == 3) {
        printf(
            "\n  Load average (1, 5, 15 min):  %.2f   %.2f   %.2f\n",
            load[0],
            load[1],
            load[2]
        );
    }

    char layout[160];
    copy_core_layout(layout, sizeof(layout));
    printf("  Cores: %s\n\n", layout);

    free(first);
    free(second);
}
