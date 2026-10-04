#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "report.h"

// -----------------------------------------------------------------------------
// Private Apple HID APIs
// -----------------------------------------------------------------------------
// These APIs are private and may change between macOS versions. Fine for a
// local diagnostic tool, not for Mac App Store software.
// -----------------------------------------------------------------------------

typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef struct __IOHIDEvent *IOHIDEventRef;

IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef allocator);

CFArrayRef IOHIDEventSystemClientCopyServices(
    IOHIDEventSystemClientRef client
);

void IOHIDEventSystemClientSetMatching(
    IOHIDEventSystemClientRef client,
    CFDictionaryRef match
);

CFTypeRef IOHIDServiceClientCopyProperty(
    IOHIDServiceClientRef service,
    CFStringRef property
);

IOHIDEventRef IOHIDServiceClientCopyEvent(
    IOHIDServiceClientRef service,
    int64_t eventType,
    uint64_t options,
    uint64_t reserved
);

double IOHIDEventGetFloatValue(
    IOHIDEventRef event,
    int32_t field
);


#define kIOHIDEventTypeTemperature         15
#define kIOHIDEventFieldTemperatureValue   983040

#define PRODUCT_BUFFER_SIZE 128

// -----------------------------------------------------------------------------
// Sensor categories
// -----------------------------------------------------------------------------

typedef enum {
    SENSOR_SOC,
    SENSOR_POWER,
    SENSOR_BATTERY,
    SENSOR_STORAGE,
    SENSOR_CALIBRATION,
    SENSOR_UNKNOWN
} SensorCategory;


typedef struct {
    int count;
    double sum;
    double min;
    double max;
} TemperatureStats;

// -----------------------------------------------------------------------------
// Thermal sensors
// -----------------------------------------------------------------------------

static void stats_init(TemperatureStats *stats)
{
    stats->count = 0;
    stats->sum = 0.0;
    stats->min = INFINITY;
    stats->max = -INFINITY;
}


static void stats_add(TemperatureStats *stats, double value)
{
    stats->count++;
    stats->sum += value;

    if (value < stats->min)
        stats->min = value;

    if (value > stats->max)
        stats->max = value;
}


static double stats_average(const TemperatureStats *stats)
{
    if (stats->count == 0)
        return NAN;

    return stats->sum / stats->count;
}


static bool starts_with(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}


static bool contains_ignore_case(const char *str, const char *needle)
{
    if (!str || !needle)
        return false;

    size_t str_len = strlen(str);
    size_t needle_len = strlen(needle);

    if (needle_len == 0 || needle_len > str_len)
        return false;

    for (size_t i = 0; i <= str_len - needle_len; i++) {
        size_t j = 0;

        while (
            j < needle_len &&
            tolower((unsigned char)str[i + j]) ==
            tolower((unsigned char)needle[j])
        ) {
            j++;
        }

        if (j == needle_len)
            return true;
    }

    return false;
}


static bool valid_temperature(double temp)
{
    return isfinite(temp) &&
           temp >= TEMP_MIN_VALID &&
           temp <= TEMP_MAX_VALID;
}


static SensorCategory classify_sensor(const char *product)
{
    if (!product)
        return SENSOR_UNKNOWN;

    // Calibration sensors must be checked before generic PMU sensors.
    if (contains_ignore_case(product, "tcal"))
        return SENSOR_CALIBRATION;

    if (contains_ignore_case(product, "gas gauge") ||
        contains_ignore_case(product, "battery"))
        return SENSOR_BATTERY;

    if (contains_ignore_case(product, "NAND") ||
        contains_ignore_case(product, "SSD"))
        return SENSOR_STORAGE;

    if ((starts_with(product, "PMU ") ||
         starts_with(product, "PMU2 ")) &&
        contains_ignore_case(product, "tdie"))
        return SENSOR_SOC;

    if ((starts_with(product, "PMU ") ||
         starts_with(product, "PMU2 ")) &&
        contains_ignore_case(product, "tdev"))
        return SENSOR_POWER;

    return SENSOR_UNKNOWN;
}


static const char *category_name(SensorCategory category)
{
    switch (category) {
        case SENSOR_SOC:
            return "SoC / Chip";
        case SENSOR_POWER:
            return "Power System";
        case SENSOR_BATTERY:
            return "Battery";
        case SENSOR_STORAGE:
            return "SSD / Storage";
        case SENSOR_CALIBRATION:
            return "Calibration";
        default:
            return "Other";
    }
}


static void friendly_sensor_name(
    const char *product,
    SensorCategory category,
    char *buffer,
    size_t buffer_size
)
{
    switch (category) {
        case SENSOR_SOC:
            snprintf(buffer, buffer_size, "SoC Die (%s)", product);
            break;
        case SENSOR_POWER:
            snprintf(buffer, buffer_size, "Power (%s)", product);
            break;
        case SENSOR_BATTERY:
            snprintf(buffer, buffer_size, "Battery");
            break;
        case SENSOR_STORAGE:
            snprintf(buffer, buffer_size, "SSD / NAND");
            break;
        case SENSOR_CALIBRATION:
            snprintf(buffer, buffer_size, "Calibration (%s)", product);
            break;
        default:
            snprintf(buffer, buffer_size, "%s", product);
            break;
    }
}


static bool get_product_name(
    IOHIDServiceClientRef service,
    char *buffer,
    size_t buffer_size
)
{
    if (!service || !buffer || buffer_size == 0)
        return false;

    snprintf(buffer, buffer_size, "Unknown");

    CFTypeRef property =
        IOHIDServiceClientCopyProperty(service, CFSTR("Product"));

    if (!property)
        return false;

    bool success = false;

    if (CFGetTypeID(property) == CFStringGetTypeID()) {
        success = CFStringGetCString(
            (CFStringRef)property,
            buffer,
            buffer_size,
            kCFStringEncodingUTF8
        );
    }

    CFRelease(property);

    return success;
}


static void print_temp_summary(
    const char *title,
    const TemperatureStats *stats,
    bool show_lowest
)
{
    if (stats->count <= 0)
        return;

    printf("%s\n", title);
    printf("  Average : %6.2f °C\n", stats_average(stats));
    printf("  Hottest : %6.2f °C\n", stats->max);

    if (show_lowest)
        printf("  Lowest  : %6.2f °C\n", stats->min);

    if (stats->count > 1)
        printf("  Sensors : %d\n", stats->count);

    printf("\n");
}


void print_thermal(void)
{
    print_banner("Thermal");

    IOHIDEventSystemClientRef system_client =
        IOHIDEventSystemClientCreate(kCFAllocatorDefault);

    if (!system_client) {
        fprintf(stderr, "Could not open the HID sensor service.\n\n");
        return;
    }

    int page = 0xFF00;
    int usage = 5;

    CFNumberRef num_page = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberIntType,
        &page
    );
    CFNumberRef num_usage = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberIntType,
        &usage
    );

    if (!num_page || !num_usage) {
        fprintf(stderr, "Could not build the thermal sensor filter.\n\n");

        if (num_page)
            CFRelease(num_page);

        if (num_usage)
            CFRelease(num_usage);

        CFRelease(system_client);
        return;
    }

    const void *keys[] = {
        CFSTR("PrimaryUsagePage"),
        CFSTR("PrimaryUsage")
    };
    const void *values[] = {
        num_page,
        num_usage
    };

    CFDictionaryRef matching = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );

    CFRelease(num_page);
    CFRelease(num_usage);

    if (!matching) {
        fprintf(stderr, "Could not build the thermal sensor filter.\n\n");
        CFRelease(system_client);
        return;
    }

    IOHIDEventSystemClientSetMatching(system_client, matching);
    CFRelease(matching);

    CFArrayRef services = IOHIDEventSystemClientCopyServices(system_client);

    if (!services) {
        fprintf(
            stderr,
            "No thermal sensors were found. Try running with sudo.\n\n"
        );
        CFRelease(system_client);
        return;
    }

    CFIndex count = CFArrayGetCount(services);

    printf("Found %ld thermal sensors.\n\n", (long)count);
    printf("%-4s %-32s %-16s %10s\n", "#", "Sensor", "Category", "Temp");
    printf("------------------------------------------------------------------\n");

    TemperatureStats soc_stats;
    TemperatureStats power_stats;
    TemperatureStats battery_stats;
    TemperatureStats storage_stats;

    stats_init(&soc_stats);
    stats_init(&power_stats);
    stats_init(&battery_stats);
    stats_init(&storage_stats);

    for (CFIndex i = 0; i < count; i++) {
        IOHIDServiceClientRef service =
            (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);

        if (!service)
            continue;

        char product[PRODUCT_BUFFER_SIZE];

        get_product_name(service, product, sizeof(product));

        SensorCategory category = classify_sensor(product);
        char friendly[160];

        friendly_sensor_name(product, category, friendly, sizeof(friendly));

        IOHIDEventRef event = IOHIDServiceClientCopyEvent(
            service,
            kIOHIDEventTypeTemperature,
            0,
            0
        );

        if (!event) {
            printf(
                "%-4ld %-32.32s %-16s %10s\n",
                (long)(i + 1),
                friendly,
                category_name(category),
                "N/A"
            );
            continue;
        }

        double temp = IOHIDEventGetFloatValue(
            event,
            kIOHIDEventFieldTemperatureValue
        );

        CFRelease(event);

        if (!valid_temperature(temp)) {
            printf(
                "%-4ld %-32.32s %-16s %10s\n",
                (long)(i + 1),
                friendly,
                category_name(category),
                "Invalid"
            );
            continue;
        }

        printf(
            "%-4ld %-32.32s %-16s %8.2f °C\n",
            (long)(i + 1),
            friendly,
            category_name(category),
            temp
        );

        // Unconnected probes sit a little below zero and would skew the average.
        if (temp < 0)
            continue;

        switch (category) {
            case SENSOR_SOC:
                stats_add(&soc_stats, temp);
                break;
            case SENSOR_POWER:
                stats_add(&power_stats, temp);
                break;
            case SENSOR_BATTERY:
                stats_add(&battery_stats, temp);
                break;
            case SENSOR_STORAGE:
                stats_add(&storage_stats, temp);
                break;
            default:
                break;
        }
    }

    printf("\n");
    printf("Temperature summary\n\n");

    print_temp_summary("Chip / SoC", &soc_stats, true);
    print_temp_summary("Power system", &power_stats, false);
    print_temp_summary("Battery", &battery_stats, false);
    print_temp_summary("SSD / storage", &storage_stats, false);

    printf("Calibration sensors, and readings below 0 °C, are left out of the summary.\n\n");

    CFRelease(services);
    CFRelease(system_client);
}

