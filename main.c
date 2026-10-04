#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "report.h"

// Build: make
// Run with no arguments for the menu. Thermal sensors sometimes need sudo.


#define TOP_PROCESS_COUNT 8
#define MAX_IFACES        64

// -----------------------------------------------------------------------------
// Reports the user can choose
// -----------------------------------------------------------------------------

typedef enum {
    SECTION_SYSTEM = 0,
    SECTION_CPU,
    SECTION_MEMORY,
    SECTION_STORAGE,
    SECTION_BATTERY,
    SECTION_THERMAL,
    SECTION_NETWORK,
    SECTION_PROCESSES,
    SECTION_COUNT
} Section;


typedef struct {
    const char *name;
    const char *title;
    const char *blurb;
} SectionInfo;


static const SectionInfo kSections[SECTION_COUNT] = {
    { "system",    "System",    "Model, chip, macOS version, uptime" },
    { "cpu",       "CPU",       "Overall usage, per-core load, load average" },
    { "memory",    "Memory",    "RAM, wired, compressed, cache, swap" },
    { "storage",   "Storage",   "Disks, APFS containers, and volumes" },
    { "battery",   "Battery",   "Charge, health, cycles, power adapter" },
    { "thermal",   "Thermal",   "SoC, power, battery, and SSD temperatures" },
    { "network",   "Network",   "Interfaces, addresses, and traffic" },
    { "processes", "Processes", "Busiest and largest processes" }
};




typedef struct {
    char name[IFNAMSIZ];
    unsigned index;
    int flags;
    uint64_t ibytes;
    uint64_t obytes;
    char addrs[6][INET6_ADDRSTRLEN];
    int addr_count;
    bool seen;
} NetIface;


typedef struct {
    pid_t pid;
    char name[64];
    uint64_t cpu_time;
    uint64_t rss;
    int32_t threads;
    bool alive;
} ProcSnap;


typedef struct {
    pid_t pid;
    char name[64];
    double cpu;
    uint64_t rss;
} ProcRow;


// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

void print_banner(const char *title)
{
    int len = (int)strlen(title);
    int pad = MENU_WIDTH - len;

    if (pad < 0)
        pad = 0;

    int left = pad / 2;
    int right = pad - left;

    printf("\n");
    printf("============================================================\n");
    printf("%*s%s%*s\n", left, "", title, right, "");
    printf("============================================================\n\n");
}


static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    char *end = s + strlen(s) - 1;

    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}


void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    nanosleep(&ts, NULL);
}


static uint64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


// proc_taskinfo CPU times are mach absolute ticks, not nanoseconds.
static double ticks_to_ns(uint64_t ticks)
{
    static mach_timebase_info_data_t info;
    static bool ready = false;

    if (!ready) {
        if (mach_timebase_info(&info) != KERN_SUCCESS) {
            info.numer = 1;
            info.denom = 1;
        }

        if (info.denom == 0)
            info.denom = 1;

        ready = true;
    }

    return (double)ticks * (double)info.numer / (double)info.denom;
}


void format_bytes(uint64_t bytes, char *buf, size_t n, bool decimal)
{
    double unit = decimal ? 1000.0 : 1024.0;
    const char *names[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double value = (double)bytes;
    int index = 0;

    while (value >= unit && index < 5) {
        value /= unit;
        index++;
    }

    if (index == 0)
        snprintf(buf, n, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, n, "%.2f %s", value, names[index]);
}


void format_bar(double percent, char *buf, size_t n)
{
    const int width = 20;

    if (n < (size_t)width + 3)
        return;

    if (percent < 0)
        percent = 0;

    if (percent > 100)
        percent = 100;

    int filled = (int)(percent / 100.0 * width + 0.5);

    buf[0] = '[';

    for (int i = 0; i < width; i++)
        buf[1 + i] = (i < filled) ? '#' : '.';

    buf[width + 1] = ']';
    buf[width + 2] = '\0';
}


static void format_uptime(time_t seconds, char *buf, size_t n)
{
    if (seconds < 0)
        seconds = 0;

    long days = (long)(seconds / 86400);
    seconds %= 86400;
    long hours = (long)(seconds / 3600);
    seconds %= 3600;
    long mins = (long)(seconds / 60);

    if (days > 0)
        snprintf(buf, n, "%ld days %ld hours %ld min", days, hours, mins);
    else if (hours > 0)
        snprintf(buf, n, "%ld hours %ld min", hours, mins);
    else
        snprintf(buf, n, "%ld min", mins);
}


bool sysctl_string(const char *name, char *buf, size_t buflen)
{
    if (!buf || buflen == 0)
        return false;

    memset(buf, 0, buflen);

    size_t len = buflen - 1;

    if (sysctlbyname(name, buf, &len, NULL, 0) != 0)
        return false;

    buf[buflen - 1] = '\0';

    return buf[0] != '\0';
}


bool sysctl_int(const char *name, int *out)
{
    int value = 0;
    size_t len = sizeof(value);

    if (sysctlbyname(name, &value, &len, NULL, 0) != 0)
        return false;

    *out = value;

    return true;
}


bool sysctl_u64(const char *name, uint64_t *out)
{
    uint64_t value = 0;
    size_t len = sizeof(value);

    if (sysctlbyname(name, &value, &len, NULL, 0) != 0)
        return false;

    *out = value;

    return true;
}


bool cfstring_to_buf(CFStringRef string, char *buf, size_t n)
{
    if (!string || !buf || n == 0)
        return false;

    if (CFGetTypeID(string) != CFStringGetTypeID())
        return false;

    return CFStringGetCString(string, buf, n, kCFStringEncodingUTF8);
}


static bool dict_bool(CFDictionaryRef dict, CFStringRef key, bool *out)
{
    if (!dict)
        return false;

    CFTypeRef value = CFDictionaryGetValue(dict, key);

    if (!value || CFGetTypeID(value) != CFBooleanGetTypeID())
        return false;

    *out = CFBooleanGetValue((CFBooleanRef)value);

    return true;
}


static bool dict_int64(CFDictionaryRef dict, CFStringRef key, int64_t *out)
{
    if (!dict)
        return false;

    CFTypeRef value = CFDictionaryGetValue(dict, key);

    if (!value || CFGetTypeID(value) != CFNumberGetTypeID())
        return false;

    return CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, out);
}


static bool dict_string(
    CFDictionaryRef dict,
    CFStringRef key,
    char *buf,
    size_t n
)
{
    if (!dict)
        return false;

    CFTypeRef value = CFDictionaryGetValue(dict, key);

    if (!value || CFGetTypeID(value) != CFStringGetTypeID())
        return false;

    return CFStringGetCString((CFStringRef)value, buf, n, kCFStringEncodingUTF8);
}


bool registry_int64(
    io_registry_entry_t entry,
    CFStringRef key,
    int64_t *out
)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        entry,
        key,
        kCFAllocatorDefault,
        0
    );

    bool ok = false;

    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        ok = CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, out);

    if (value)
        CFRelease(value);

    return ok;
}


bool registry_string(
    io_registry_entry_t entry,
    CFStringRef key,
    char *buf,
    size_t n
)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        entry,
        key,
        kCFAllocatorDefault,
        0
    );

    bool ok = cfstring_to_buf((CFStringRef)value, buf, n);

    if (value)
        CFRelease(value);

    return ok;
}


void copy_core_layout(char *buf, size_t n)
{
    int logical = 0;
    int physical = 0;
    int clusters[4] = { 0, 0, 0, 0 };
    const char *names[] = {
        "performance",
        "efficiency",
        "cluster 3",
        "cluster 4"
    };
    const char *keys[] = {
        "hw.perflevel0.logicalcpu",
        "hw.perflevel1.logicalcpu",
        "hw.perflevel2.logicalcpu",
        "hw.perflevel3.logicalcpu"
    };

    sysctl_int("hw.logicalcpu", &logical);
    sysctl_int("hw.physicalcpu", &physical);

    int described = 0;
    int found = 0;

    for (int i = 0; i < 4; i++) {
        if (sysctl_int(keys[i], &clusters[i]) && clusters[i] > 0) {
            described += clusters[i];
            found++;
        }
    }

    if (logical <= 0) {
        snprintf(buf, n, "unavailable");
        return;
    }

    if (found >= 2 && described == logical) {
        char detail[160] = "";
        size_t used = 0;

        for (int i = 0; i < 4; i++) {
            if (clusters[i] <= 0)
                continue;

            int wrote = snprintf(
                detail + used,
                sizeof(detail) - used,
                "%s%d %s",
                used ? " + " : "",
                clusters[i],
                names[i]
            );

            if (wrote < 0 || (size_t)wrote >= sizeof(detail) - used)
                break;

            used += (size_t)wrote;
        }

        snprintf(buf, n, "%d logical (%s)", logical, detail);
        return;
    }

    if (physical > 0 && physical != logical)
        snprintf(buf, n, "%d logical, %d physical", logical, physical);
    else
        snprintf(buf, n, "%d logical", logical);
}



// -----------------------------------------------------------------------------
// Menu
// -----------------------------------------------------------------------------

static void print_usage(const char *argv0)
{
    fprintf(stderr, "\nUsage: %s [selection]\n\n", argv0);
    fprintf(stderr, "With no arguments, an interactive menu is shown.\n\n");
    fprintf(stderr, "Sections:\n");

    for (int i = 0; i < SECTION_COUNT; i++) {
        fprintf(
            stderr,
            "  %d  %-12s %s\n",
            i + 1,
            kSections[i].name,
            kSections[i].blurb
        );
    }

    fprintf(stderr, "\n  a  everything    Print every report\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s all\n", argv0);
    fprintf(stderr, "  %s cpu memory storage\n", argv0);
    fprintf(stderr, "  %s 2 3 6\n\n", argv0);
}


static void print_menu(void)
{
    print_banner("Mac System Report");

    printf("Choose what to print. Separate choices with spaces or commas.\n\n");

    for (int i = 0; i < SECTION_COUNT; i++) {
        printf(
            "  %d  %-12s %s\n",
            i + 1,
            kSections[i].title,
            kSections[i].blurb
        );
    }

    printf("\n");
    printf("  a  %-12s %s\n", "Everything", "Print every report above");
    printf("  q  %-12s %s\n", "Quit", "Leave the program");
    printf("\n");
    printf("Examples: 1 3 4    cpu memory    2-5    a\n\n");
    printf("Choice: ");
    fflush(stdout);
}


static bool token_is_quit(const char *token)
{
    return strcasecmp(token, "q") == 0 ||
           strcasecmp(token, "quit") == 0 ||
           strcasecmp(token, "exit") == 0 ||
           strcmp(token, "0") == 0;
}


static bool token_is_all(const char *token)
{
    return strcasecmp(token, "a") == 0 ||
           strcasecmp(token, "all") == 0 ||
           strcasecmp(token, "everything") == 0 ||
           strcmp(token, "*") == 0;
}


static int section_from_name(const char *token)
{
    if (strcasecmp(token, "ram") == 0 || strcasecmp(token, "mem") == 0)
        return SECTION_MEMORY;

    if (strcasecmp(token, "disk") == 0 ||
        strcasecmp(token, "disks") == 0 ||
        strcasecmp(token, "volume") == 0 ||
        strcasecmp(token, "volumes") == 0)
        return SECTION_STORAGE;

    if (strcasecmp(token, "temp") == 0 ||
        strcasecmp(token, "temps") == 0 ||
        strcasecmp(token, "temperature") == 0 ||
        strcasecmp(token, "temperatures") == 0 ||
        strcasecmp(token, "sensors") == 0)
        return SECTION_THERMAL;

    if (strcasecmp(token, "power") == 0 || strcasecmp(token, "charge") == 0)
        return SECTION_BATTERY;

    if (strcasecmp(token, "net") == 0 || strcasecmp(token, "interfaces") == 0)
        return SECTION_NETWORK;

    if (strcasecmp(token, "proc") == 0 ||
        strcasecmp(token, "ps") == 0 ||
        strcasecmp(token, "top") == 0)
        return SECTION_PROCESSES;

    if (strcasecmp(token, "info") == 0 ||
        strcasecmp(token, "os") == 0 ||
        strcasecmp(token, "overview") == 0)
        return SECTION_SYSTEM;

    for (int i = 0; i < SECTION_COUNT; i++) {
        if (strcasecmp(token, kSections[i].name) == 0 ||
            strcasecmp(token, kSections[i].title) == 0)
            return i;
    }

    return -1;
}


static bool parse_number_token(const char *token, int *begin, int *end)
{
    const char *dash = strchr(token, '-');

    if (!dash) {
        char *stop = NULL;
        long value = strtol(token, &stop, 10);

        if (stop == token || *stop != '\0')
            return false;

        *begin = (int)value;
        *end = (int)value;

        return true;
    }

    if (dash == token || *(dash + 1) == '\0' || strchr(dash + 1, '-'))
        return false;

    char left[32];
    char right[32];
    size_t left_len = (size_t)(dash - token);

    if (left_len == 0 || left_len >= sizeof(left))
        return false;

    memcpy(left, token, left_len);
    left[left_len] = '\0';
    snprintf(right, sizeof(right), "%s", dash + 1);

    char *stop = NULL;
    long a = strtol(left, &stop, 10);

    if (stop == left || *stop != '\0')
        return false;

    stop = NULL;
    long b = strtol(right, &stop, 10);

    if (stop == right || *stop != '\0')
        return false;

    *begin = (int)a;
    *end = (int)b;

    return true;
}


// Returns 1 when reports were selected, 0 when the line was empty,
// -1 to quit, and -2 when a token was not recognized.
static int parse_selection(char *line, bool selected[SECTION_COUNT])
{
    memset(selected, 0, sizeof(bool) * SECTION_COUNT);

    char *cursor = trim(line);

    if (*cursor == '\0')
        return 0;

    bool any = false;
    char *save = NULL;

    for (char *token = strtok_r(cursor, " ,\t", &save);
         token != NULL;
         token = strtok_r(NULL, " ,\t", &save)) {

        if (token_is_quit(token))
            return -1;

        if (token_is_all(token)) {
            for (int i = 0; i < SECTION_COUNT; i++)
                selected[i] = true;

            return 1;
        }

        int begin = 0;
        int end = 0;

        if (parse_number_token(token, &begin, &end)) {
            if (begin < 1 || end > SECTION_COUNT || begin > end) {
                fprintf(stderr, "Unknown choice: %s\n", token);
                return -2;
            }

            for (int number = begin; number <= end; number++)
                selected[number - 1] = true;

            any = true;
            continue;
        }

        int section = section_from_name(token);

        if (section < 0) {
            fprintf(stderr, "Unknown choice: %s\n", token);
            fprintf(stderr, "Use a number from 1 to %d, a section name, or a for everything.\n",
                    SECTION_COUNT);
            return -2;
        }

        selected[section] = true;
        any = true;
    }

    return any ? 1 : 0;
}


// -----------------------------------------------------------------------------
// System
// -----------------------------------------------------------------------------

static bool read_root_space(uint64_t *capacity, uint64_t *available)
{
    unsigned char buf[256];
    struct attrlist list;

    memset(buf, 0, sizeof(buf));
    memset(&list, 0, sizeof(list));

    list.bitmapcount = ATTR_BIT_MAP_COUNT;
    list.volattr = ATTR_VOL_INFO | ATTR_VOL_SIZE | ATTR_VOL_SPACEAVAIL;

    if (getattrlist("/", &list, buf, sizeof(buf), 0) != 0)
        return false;

    uint32_t length = 0;
    memcpy(&length, buf, sizeof(length));

    if (length < 20)
        return false;

    memcpy(capacity, buf + 4, sizeof(*capacity));
    memcpy(available, buf + 12, sizeof(*available));

    return true;
}


static void print_system(void)
{
    char hostname[256] = "unknown";
    char model[128] = "unknown";
    char chip[128] = "unknown";
    char product[64] = "unknown";
    char build[64] = "";
    char kernel[64] = "";
    char cores[160] = "";
    char uptime_text[64] = "";
    char boot_text[64] = "";
    char memory_text[32] = "";
    char disk_total_text[32] = "";
    char disk_free_text[32] = "";
    char computer[256] = "";

    gethostname(hostname, sizeof(hostname));
    sysctl_string("hw.model", model, sizeof(model));
    sysctl_string("machdep.cpu.brand_string", chip, sizeof(chip));
    sysctl_string("kern.osproductversion", product, sizeof(product));
    sysctl_string("kern.osversion", build, sizeof(build));
    sysctl_string("kern.osrelease", kernel, sizeof(kernel));
    copy_core_layout(cores, sizeof(cores));

    CFStringRef computer_ref = SCDynamicStoreCopyComputerName(NULL, NULL);

    if (!cfstring_to_buf(computer_ref, computer, sizeof(computer)))
        snprintf(computer, sizeof(computer), "%s", hostname);

    if (computer_ref)
        CFRelease(computer_ref);

    struct passwd *user = getpwuid(getuid());
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    struct timeval boot;
    size_t boot_len = sizeof(boot);
    memset(&boot, 0, sizeof(boot));

    if (sysctlbyname("kern.boottime", &boot, &boot_len, NULL, 0) == 0 &&
        boot.tv_sec > 0) {

        time_t now = time(NULL);
        format_uptime(now - boot.tv_sec, uptime_text, sizeof(uptime_text));

        struct tm local;
        localtime_r(&boot.tv_sec, &local);
        strftime(boot_text, sizeof(boot_text), "%a %b %e %Y %H:%M", &local);
    }

    uint64_t mem_total = 0;

    if (sysctl_u64("hw.memsize", &mem_total))
        format_bytes(mem_total, memory_text, sizeof(memory_text), false);

    uint64_t disk_total = 0;
    uint64_t disk_free = 0;

    if (read_root_space(&disk_total, &disk_free)) {
        format_bytes(disk_total, disk_total_text, sizeof(disk_total_text), true);
        format_bytes(disk_free, disk_free_text, sizeof(disk_free_text), true);
    }

    print_banner("System");

    printf("  %-14s %s\n", "Computer", computer);
    printf("  %-14s %s\n", "Host name", hostname);

    if (user && user->pw_name)
        printf("  %-14s %s\n", "User", user->pw_name);

    printf("  %-14s %s\n", "Model", model);
    printf("  %-14s %s\n", "Chip", chip);
    printf("  %-14s %s\n", "Cores", cores);

    if (memory_text[0])
        printf("  %-14s %s\n", "Memory", memory_text);

    if (disk_total_text[0]) {
        printf(
            "  %-14s %s available of %s\n",
            "Startup disk",
            disk_free_text,
            disk_total_text
        );
    }

    if (build[0])
        printf("  %-14s %s  (build %s)\n", "macOS", product, build);
    else
        printf("  %-14s %s\n", "macOS", product);

    printf(
        "  %-14s %s %s %s\n",
        "Kernel",
        uts.sysname[0] ? uts.sysname : "Darwin",
        kernel,
        uts.machine
    );

    if (uptime_text[0])
        printf("  %-14s %s\n", "Uptime", uptime_text);

    if (boot_text[0])
        printf("  %-14s %s\n", "Booted", boot_text);

    printf("\n");
}



// -----------------------------------------------------------------------------
// Battery and power
// -----------------------------------------------------------------------------

static bool valid_minutes(int64_t minutes)
{
    return minutes > 0 && minutes < 24 * 60 * 7;
}


static void format_minutes(int64_t minutes, char *buf, size_t n)
{
    long hours = (long)(minutes / 60);
    long mins = (long)(minutes % 60);

    if (hours > 0)
        snprintf(buf, n, "%ld h %ld min", hours, mins);
    else
        snprintf(buf, n, "%ld min", mins);
}


static void print_power_adapter(void)
{
    CFDictionaryRef adapter = IOPSCopyExternalPowerAdapterDetails();

    if (!adapter)
        return;

    int64_t watts = 0;
    bool have_watts = dict_int64(
        adapter,
        CFSTR(kIOPSPowerAdapterWattsKey),
        &watts
    );

    if (have_watts && watts > 0)
        printf("  %-16s %lld W adapter connected\n", "Power adapter", (long long)watts);
    else
        printf("  %-16s connected\n", "Power adapter");

    CFRelease(adapter);
}


static void print_battery_registry(void)
{
    io_service_t battery = IOServiceGetMatchingService(
        kIOMainPortDefault,
        IOServiceMatching("IOPMPowerSource")
    );

    if (!battery)
        return;

    int64_t cycles = 0;
    int64_t design = 0;
    int64_t full = 0;
    int64_t now = 0;
    int64_t temperature = 0;

    bool have_cycles = registry_int64(battery, CFSTR("CycleCount"), &cycles);
    bool have_design = registry_int64(battery, CFSTR("DesignCapacity"), &design);
    bool have_full = registry_int64(battery, CFSTR("AppleRawMaxCapacity"), &full);
    bool have_now = registry_int64(battery, CFSTR("AppleRawCurrentCapacity"), &now);
    bool have_temp = registry_int64(battery, CFSTR("Temperature"), &temperature);

    if (have_cycles)
        printf("  %-16s %lld\n", "Cycle count", (long long)cycles);

    if (have_design && have_full && design > 0) {
        double health = (double)full / (double)design * 100.0;

        printf(
            "  %-16s %.1f%%   (%lld / %lld mAh)\n",
            "Max capacity",
            health,
            (long long)full,
            (long long)design
        );
    }

    if (have_now)
        printf("  %-16s %lld mAh\n", "Current charge", (long long)now);

    int64_t voltage = 0;
    int64_t amperage = 0;
    bool have_voltage = registry_int64(battery, CFSTR("Voltage"), &voltage);
    bool have_amperage = registry_int64(battery, CFSTR("Amperage"), &amperage);

    if (have_voltage && voltage > 0)
        printf("  %-16s %.2f V\n", "Voltage", voltage / 1000.0);

    if (have_voltage && have_amperage && voltage > 0) {
        double watts = fabs((double)amperage * (double)voltage / 1000000.0);
        printf("  %-16s %.1f W\n", "Power", watts);
    }

    if (have_temp) {
        double celsius = (double)temperature;

        if (celsius > 200.0)
            celsius /= 100.0;

        if (celsius >= TEMP_MIN_VALID && celsius <= TEMP_MAX_VALID)
            printf("  %-16s %.1f °C\n", "Pack temperature", celsius);
    }

    IOObjectRelease(battery);
}


static void print_one_power_source(CFDictionaryRef source)
{
    bool present = true;

    if (dict_bool(source, CFSTR(kIOPSIsPresentKey), &present) && !present)
        return;

    char name[128] = "Battery";
    char state[64] = "";
    char health[64] = "";

    dict_string(source, CFSTR(kIOPSNameKey), name, sizeof(name));
    dict_string(source, CFSTR(kIOPSPowerSourceStateKey), state, sizeof(state));
    dict_string(source, CFSTR(kIOPSBatteryHealthKey), health, sizeof(health));

    int64_t current = 0;
    int64_t maximum = 0;
    bool have_current = dict_int64(source, CFSTR(kIOPSCurrentCapacityKey), &current);
    bool have_max = dict_int64(source, CFSTR(kIOPSMaxCapacityKey), &maximum);

    bool charging = false;
    bool charged = false;
    bool finishing = false;

    dict_bool(source, CFSTR(kIOPSIsChargingKey), &charging);
    dict_bool(source, CFSTR(kIOPSIsChargedKey), &charged);
    dict_bool(source, CFSTR(kIOPSIsFinishingChargeKey), &finishing);

    const char *status = "Not charging";

    if (finishing)
        status = "Finishing charge";
    else if (charging)
        status = "Charging";
    else if (charged)
        status = "Charged";
    else if (strcmp(state, kIOPSBatteryPowerValue) == 0)
        status = "On battery";

    printf("  %s\n", name);

    if (have_current && have_max && maximum > 0) {
        double percent = (double)current / (double)maximum * 100.0;
        char bar[32];

        if (percent < 0)
            percent = 0;

        if (percent > 100)
            percent = 100;

        format_bar(percent, bar, sizeof(bar));
        printf("  %-16s %5.1f%%  %s\n", "Charge", percent, bar);
    }

    printf("  %-16s %s\n", "State", status[0] ? status : state);

    if (state[0])
        printf("  %-16s %s\n", "Source", state);

    if (health[0])
        printf("  %-16s %s\n", "Health", health);

    int64_t minutes = 0;

    if (charging &&
        dict_int64(source, CFSTR(kIOPSTimeToFullChargeKey), &minutes) &&
        valid_minutes(minutes)) {

        char text[32];
        format_minutes(minutes, text, sizeof(text));
        printf("  %-16s %s\n", "Time to full", text);
    }

    if (!charging &&
        strcmp(state, kIOPSBatteryPowerValue) == 0 &&
        dict_int64(source, CFSTR(kIOPSTimeToEmptyKey), &minutes) &&
        valid_minutes(minutes)) {

        char text[32];
        format_minutes(minutes, text, sizeof(text));
        printf("  %-16s %s\n", "Time remaining", text);
    }

    int64_t voltage = 0;
    int64_t amps = 0;
    bool have_voltage = dict_int64(source, CFSTR(kIOPSVoltageKey), &voltage);
    bool have_amps = dict_int64(source, CFSTR(kIOPSCurrentKey), &amps);

    if (have_voltage && voltage > 0)
        printf("  %-16s %.2f V\n", "Voltage", voltage / 1000.0);

    if (have_amps)
        printf("  %-16s %lld mA\n", "Current", (long long)amps);

    if (have_voltage && have_amps && voltage > 0) {
        double watts = fabs((double)amps * (double)voltage / 1000000.0);
        printf("  %-16s %.1f W\n", "Power", watts);
    }

    printf("\n");
}


static void print_battery(void)
{
    print_banner("Battery and Power");

    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    CFArrayRef list = blob ? IOPSCopyPowerSourcesList(blob) : NULL;
    CFIndex count = list ? CFArrayGetCount(list) : 0;
    bool printed_source = false;

    for (CFIndex i = 0; i < count; i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(list, i);
        CFDictionaryRef source = IOPSGetPowerSourceDescription(blob, item);

        if (!source)
            continue;

        print_one_power_source(source);
        printed_source = true;
    }

    if (!printed_source)
        printf("  No internal battery. This Mac is on external power.\n");

    print_battery_registry();
    print_power_adapter();
    printf("\n");

    if (list)
        CFRelease(list);

    if (blob)
        CFRelease(blob);
}


// -----------------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------------

static size_t sa_round(size_t length)
{
    if (length == 0)
        return sizeof(uint32_t);

    return (length + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
}


static NetIface *iface_by_index(NetIface *ifaces, int *count, unsigned index)
{
    for (int i = 0; i < *count; i++) {
        if (ifaces[i].index == index)
            return &ifaces[i];
    }

    if (*count >= MAX_IFACES)
        return NULL;

    NetIface *iface = &ifaces[*count];
    memset(iface, 0, sizeof(*iface));
    iface->index = index;
    (*count)++;

    return iface;
}


static void iface_add_addr(NetIface *iface, const char *text)
{
    if (!iface || !text || text[0] == '\0')
        return;

    if (iface->addr_count >= 6)
        return;

    for (int i = 0; i < iface->addr_count; i++) {
        if (strcmp(iface->addrs[i], text) == 0)
            return;
    }

    snprintf(
        iface->addrs[iface->addr_count],
        sizeof(iface->addrs[0]),
        "%s",
        text
    );
    iface->addr_count++;
}


static bool skip_interface(const NetIface *iface)
{
    if (!(iface->flags & IFF_UP) || !(iface->flags & IFF_RUNNING))
        return true;

    if (iface->flags & IFF_LOOPBACK)
        return true;

    if (strncmp(iface->name, "awdl", 4) == 0 ||
        strncmp(iface->name, "llw", 3) == 0 ||
        strncmp(iface->name, "gif", 3) == 0 ||
        strncmp(iface->name, "stf", 3) == 0 ||
        strncmp(iface->name, "pktap", 5) == 0 ||
        strncmp(iface->name, "ap", 2) == 0)
        return true;

    // Idle tunnels show up with no address and a few kilobytes of traffic.
    if (iface->addr_count == 0) {
        const uint64_t quiet = 1024ULL * 1024ULL;

        if (iface->ibytes < quiet && iface->obytes < quiet)
            return true;
    }

    return false;
}


static void print_network(void)
{
    print_banner("Network");

    int mib[6] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0 };
    size_t length = 0;

    if (sysctl(mib, 6, NULL, &length, NULL, 0) != 0 || length == 0) {
        fprintf(stderr, "Could not list network interfaces.\n\n");
        return;
    }

    char *buffer = malloc(length);

    if (!buffer) {
        fprintf(stderr, "Out of memory.\n\n");
        return;
    }

    if (sysctl(mib, 6, buffer, &length, NULL, 0) != 0) {
        free(buffer);
        fprintf(stderr, "Could not list network interfaces.\n\n");
        return;
    }

    NetIface ifaces[MAX_IFACES];
    int iface_count = 0;
    char *cursor = buffer;
    char *end = buffer + length;

    while (cursor + sizeof(struct if_msghdr) <= end) {
        struct if_msghdr *header = (struct if_msghdr *)cursor;

        if (header->ifm_msglen == 0 || cursor + header->ifm_msglen > end)
            break;

        if (header->ifm_type == RTM_IFINFO2) {
            struct if_msghdr2 *info = (struct if_msghdr2 *)cursor;
            struct sockaddr_dl *link = (struct sockaddr_dl *)(info + 1);
            NetIface *iface = iface_by_index(ifaces, &iface_count, info->ifm_index);

            if (iface && (char *)link + sizeof(struct sockaddr_dl) <= cursor + info->ifm_msglen) {
                size_t name_len = link->sdl_nlen;

                if (name_len >= sizeof(iface->name))
                    name_len = sizeof(iface->name) - 1;

                memcpy(iface->name, link->sdl_data, name_len);
                iface->name[name_len] = '\0';
                iface->flags = info->ifm_flags;
                iface->ibytes = info->ifm_data.ifi_ibytes;
                iface->obytes = info->ifm_data.ifi_obytes;
                iface->seen = true;
            }
        } else if (header->ifm_type == RTM_NEWADDR) {
            struct ifa_msghdr *addr_header = (struct ifa_msghdr *)cursor;
            NetIface *iface = iface_by_index(
                ifaces,
                &iface_count,
                addr_header->ifam_index
            );
            char *sa_cursor = (char *)(addr_header + 1);
            char *sa_end = cursor + addr_header->ifam_msglen;

            for (int bit = 0; bit < RTAX_MAX && iface; bit++) {
                if (!(addr_header->ifam_addrs & (1 << bit)))
                    continue;

                if (sa_cursor + sizeof(struct sockaddr) > sa_end)
                    break;

                struct sockaddr *sa = (struct sockaddr *)sa_cursor;
                size_t step = sa->sa_len ? sa_round(sa->sa_len) : sizeof(uint32_t);

                if (sa_cursor + step > sa_end)
                    break;

                if (bit == RTAX_IFA && sa->sa_family == AF_INET) {
                    char text[INET_ADDRSTRLEN];
                    struct sockaddr_in *ipv4 = (struct sockaddr_in *)sa;

                    if (inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)))
                        iface_add_addr(iface, text);
                } else if (bit == RTAX_IFA && sa->sa_family == AF_INET6) {
                    struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)sa;

                    if (!IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr) &&
                        !IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr)) {
                        char text[INET6_ADDRSTRLEN];

                        if (inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text)))
                            iface_add_addr(iface, text);
                    }
                }

                sa_cursor += step;
            }
        }

        cursor += header->ifm_msglen;
    }

    free(buffer);

    printf(
        "  %-10s %-16s %14s %14s\n",
        "Interface",
        "Address",
        "Received",
        "Sent"
    );
    printf("  ----------------------------------------------------------------\n");

    int shown = 0;

    for (int i = 0; i < iface_count; i++) {
        if (!ifaces[i].seen || skip_interface(&ifaces[i]))
            continue;

        char received[32];
        char sent[32];

        format_bytes(ifaces[i].ibytes, received, sizeof(received), false);
        format_bytes(ifaces[i].obytes, sent, sizeof(sent), false);

        if (ifaces[i].addr_count == 0) {
            printf(
                "  %-10s %-16s %14s %14s\n",
                ifaces[i].name,
                "-",
                received,
                sent
            );
        } else {
            printf(
                "  %-10s %-16.16s %14s %14s\n",
                ifaces[i].name,
                ifaces[i].addrs[0],
                received,
                sent
            );

            for (int a = 1; a < ifaces[i].addr_count; a++) {
                printf(
                    "  %-10s %-16.16s\n",
                    "",
                    ifaces[i].addrs[a]
                );
            }
        }

        shown++;
    }

    if (shown == 0)
        printf("  No active interfaces.\n");

    printf("\n");
    printf("  Traffic totals are since boot. Link-local addresses are hidden.\n\n");
}


// -----------------------------------------------------------------------------
// Processes
// -----------------------------------------------------------------------------

static int collect_processes(ProcSnap **out, int *out_count, int *threads)
{
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);

    if (bytes <= 0)
        return -1;

    bytes += (int)sizeof(pid_t) * 128;

    pid_t *pids = malloc((size_t)bytes);

    if (!pids)
        return -1;

    int written = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes);

    if (written <= 0) {
        free(pids);
        return -1;
    }

    int pid_count = written / (int)sizeof(pid_t);
    ProcSnap *snaps = calloc((size_t)pid_count, sizeof(ProcSnap));

    if (!snaps) {
        free(pids);
        return -1;
    }

    int used = 0;
    int thread_total = 0;

    for (int i = 0; i < pid_count; i++) {
        if (pids[i] <= 0)
            continue;

        struct proc_taskinfo task;
        int got = proc_pidinfo(
            pids[i],
            PROC_PIDTASKINFO,
            0,
            &task,
            sizeof(task)
        );

        if (got <= 0)
            continue;

        ProcSnap *snap = &snaps[used];
        char path[PROC_PIDPATHINFO_MAXSIZE];

        snap->pid = pids[i];
        snap->cpu_time = task.pti_total_user + task.pti_total_system;
        snap->rss = task.pti_resident_size;
        snap->threads = task.pti_threadnum;
        snap->alive = true;
        thread_total += task.pti_threadnum;

        if (proc_pidpath(pids[i], path, sizeof(path)) > 0) {
            const char *base = strrchr(path, '/');
            base = (base && base[1]) ? base + 1 : path;
            snprintf(snap->name, sizeof(snap->name), "%s", base);
        } else if (proc_name(pids[i], snap->name, sizeof(snap->name)) <= 0) {
            snprintf(snap->name, sizeof(snap->name), "(%d)", pids[i]);
        }

        used++;
    }

    free(pids);

    *out = snaps;
    *out_count = used;

    if (threads)
        *threads = thread_total;

    return 0;
}


static const ProcSnap *find_snap(const ProcSnap *snaps, int count, pid_t pid)
{
    for (int i = 0; i < count; i++) {
        if (snaps[i].pid == pid)
            return &snaps[i];
    }

    return NULL;
}


static int cmp_cpu_desc(const void *a, const void *b)
{
    const ProcRow *left = a;
    const ProcRow *right = b;

    if (left->cpu > right->cpu)
        return -1;

    if (left->cpu < right->cpu)
        return 1;

    return 0;
}


static int cmp_rss_desc(const void *a, const void *b)
{
    const ProcRow *left = a;
    const ProcRow *right = b;

    if (left->rss > right->rss)
        return -1;

    if (left->rss < right->rss)
        return 1;

    return 0;
}


static void print_proc_table(const ProcRow *rows, int count, bool cpu_first)
{
    int shown = count < TOP_PROCESS_COUNT ? count : TOP_PROCESS_COUNT;

    if (cpu_first) {
        printf(
            "  %-8s %8s %12s  %s\n",
            "PID",
            "CPU",
            "Memory",
            "Name"
        );
    } else {
        printf(
            "  %-8s %12s %8s  %s\n",
            "PID",
            "Memory",
            "CPU",
            "Name"
        );
    }

    for (int i = 0; i < shown; i++) {
        char memory[32];
        format_bytes(rows[i].rss, memory, sizeof(memory), false);

        if (cpu_first) {
            printf(
                "  %-8d %7.1f%% %12s  %.42s\n",
                (int)rows[i].pid,
                rows[i].cpu,
                memory,
                rows[i].name
            );
        } else {
            printf(
                "  %-8d %12s %7.1f%%  %.42s\n",
                (int)rows[i].pid,
                memory,
                rows[i].cpu,
                rows[i].name
            );
        }
    }
}


static void print_processes(void)
{
    print_banner("Processes");

    ProcSnap *first = NULL;
    ProcSnap *second = NULL;
    int first_count = 0;
    int second_count = 0;
    int threads = 0;

    if (collect_processes(&first, &first_count, NULL) != 0) {
        fprintf(stderr, "Could not list processes.\n\n");
        return;
    }

    uint64_t started = mono_ns();
    sleep_ms(SAMPLE_MS);

    if (collect_processes(&second, &second_count, &threads) != 0) {
        free(first);
        fprintf(stderr, "Could not sample processes.\n\n");
        return;
    }

    uint64_t elapsed = mono_ns() - started;

    if (elapsed == 0)
        elapsed = 1;

    ProcRow *rows = calloc((size_t)second_count, sizeof(ProcRow));

    if (!rows) {
        free(first);
        free(second);
        fprintf(stderr, "Out of memory.\n\n");
        return;
    }

    int row_count = 0;

    for (int i = 0; i < second_count; i++) {
        const ProcSnap *before = find_snap(first, first_count, second[i].pid);
        uint64_t delta = 0;

        if (before && second[i].cpu_time >= before->cpu_time)
            delta = second[i].cpu_time - before->cpu_time;

        rows[row_count].pid = second[i].pid;
        rows[row_count].rss = second[i].rss;
        rows[row_count].cpu = ticks_to_ns(delta) / (double)elapsed * 100.0;
        snprintf(rows[row_count].name, sizeof(rows[row_count].name), "%s", second[i].name);
        row_count++;
    }

    printf("  Running processes : %d\n", second_count);
    printf("  Threads           : %d\n", threads);
    printf(
        "  CPU was sampled over %.2f s. 100%% is one full core.\n",
        (double)elapsed / 1000000000.0
    );
    printf("  Processes that hide their task info are not listed.\n\n");

    qsort(rows, (size_t)row_count, sizeof(ProcRow), cmp_cpu_desc);
    printf("  Highest CPU\n");
    print_proc_table(rows, row_count, true);

    qsort(rows, (size_t)row_count, sizeof(ProcRow), cmp_rss_desc);
    printf("\n  Largest memory\n");
    print_proc_table(rows, row_count, false);
    printf("\n");

    free(rows);
    free(first);
    free(second);
}


// -----------------------------------------------------------------------------
// Run the chosen reports
// -----------------------------------------------------------------------------

static void run_sections(const bool selected[SECTION_COUNT])
{
    for (int i = 0; i < SECTION_COUNT; i++) {
        if (!selected[i])
            continue;

        switch ((Section)i) {
            case SECTION_SYSTEM:
                print_system();
                break;
            case SECTION_CPU:
                print_cpu();
                break;
            case SECTION_MEMORY:
                print_memory();
                break;
            case SECTION_STORAGE:
                print_storage();
                break;
            case SECTION_BATTERY:
                print_battery();
                break;
            case SECTION_THERMAL:
                print_thermal();
                break;
            case SECTION_NETWORK:
                print_network();
                break;
            case SECTION_PROCESSES:
                print_processes();
                break;
            case SECTION_COUNT:
                break;
        }
    }
}


int main(int argc, char **argv)
{
    bool selected[SECTION_COUNT];

    if (argc > 1) {
        char line[1024];

        line[0] = '\0';

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0 ||
                strcmp(argv[i], "help") == 0) {
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            }

            if (line[0] != '\0')
                strlcat(line, " ", sizeof(line));

            strlcat(line, argv[i], sizeof(line));
        }

        int parsed = parse_selection(line, selected);

        if (parsed != 1) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        run_sections(selected);
        return EXIT_SUCCESS;
    }

    if (!isatty(STDIN_FILENO)) {
        char line[512];

        if (!fgets(line, sizeof(line), stdin)) {
            fprintf(stderr, "No selection given and stdin is not a terminal.\n");
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        int parsed = parse_selection(line, selected);

        if (parsed == -1)
            return EXIT_SUCCESS;

        if (parsed != 1) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        run_sections(selected);
        return EXIT_SUCCESS;
    }

    print_menu();

    for (;;) {
        char line[512];

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        int parsed = parse_selection(line, selected);

        if (parsed == -1)
            break;

        if (parsed != 1) {
            printf("Choice: ");
            fflush(stdout);
            continue;
        }

        run_sections(selected);
        break;
    }

    return EXIT_SUCCESS;
}
