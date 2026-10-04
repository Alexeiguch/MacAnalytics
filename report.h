#ifndef REPORT_H
#define REPORT_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLE_MS        400
#define MENU_WIDTH       60
#define TEMP_MIN_VALID   (-20.0)
#define TEMP_MAX_VALID   130.0

void print_banner(const char *title);
void sleep_ms(int ms);
void format_bytes(uint64_t bytes, char *buf, size_t n, bool decimal);
void format_bar(double percent, char *buf, size_t n);

bool sysctl_string(const char *name, char *buf, size_t buflen);
bool sysctl_int(const char *name, int *out);
bool sysctl_u64(const char *name, uint64_t *out);

bool cfstring_to_buf(CFStringRef string, char *buf, size_t n);
bool registry_int64(io_registry_entry_t entry, CFStringRef key, int64_t *out);
bool registry_string(io_registry_entry_t entry, CFStringRef key, char *buf, size_t n);

void copy_core_layout(char *buf, size_t n);

void print_cpu(void);
void print_memory(void);
void print_storage(void);
void print_thermal(void);

#endif
