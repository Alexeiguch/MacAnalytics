#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/param.h>

#include "report.h"

#define MAX_VOLUMES 128
#define MAX_DRIVES  32

typedef struct {
    char name[128];
    char mount[MAXPATHLEN];
    char role[24];
    int disk_unit;
    uint64_t capacity;
    uint64_t available;
    uint64_t used;
    bool startup;
    bool network;
} VolumeInfo;


typedef struct {
    char name[128];
    char bsd[32];
    char where[32];
    uint64_t size;
} DriveInfo;


// -----------------------------------------------------------------------------
// Storage
// -----------------------------------------------------------------------------

static bool parse_disk_unit(const char *device, int *unit)
{
    const char *cursor = device;

    if (strncmp(cursor, "/dev/", 5) == 0)
        cursor += 5;

    if (strncmp(cursor, "disk", 4) != 0)
        return false;

    cursor += 4;

    if (!isdigit((unsigned char)*cursor))
        return false;

    int value = 0;

    while (isdigit((unsigned char)*cursor)) {
        value = value * 10 + (*cursor - '0');
        cursor++;
    }

    *unit = value;

    return true;
}


static const char *volume_role(const char *mount)
{
    if (strcmp(mount, "/") == 0)
        return "System";

    if (strcmp(mount, "/System/Volumes/Data") == 0)
        return "Data";

    if (strcmp(mount, "/System/Volumes/VM") == 0)
        return "VM";

    if (strcmp(mount, "/System/Volumes/Preboot") == 0)
        return "Preboot";

    if (strcmp(mount, "/System/Volumes/Update") == 0)
        return "Update";

    if (strcmp(mount, "/System/Volumes/xarts") == 0)
        return "Secure";

    if (strcmp(mount, "/System/Volumes/iSCPreboot") == 0)
        return "iSC";

    if (strcmp(mount, "/System/Volumes/Hardware") == 0)
        return "Hardware";

    if (strstr(mount, "CoreSimulator") != NULL)
        return "Simulator";

    if (strncmp(mount, "/Volumes/", 9) == 0)
        return "External";

    return "Volume";
}


static bool read_volume_attrs(
    const char *mount,
    uint64_t *capacity,
    uint64_t *available,
    uint64_t *used,
    char *name,
    size_t name_len
)
{
    unsigned char buf[1280];
    struct attrlist list;

    memset(buf, 0, sizeof(buf));
    memset(&list, 0, sizeof(list));
    name[0] = '\0';

    list.bitmapcount = ATTR_BIT_MAP_COUNT;
    list.volattr =
        ATTR_VOL_INFO |
        ATTR_VOL_SIZE |
        ATTR_VOL_SPACEAVAIL |
        ATTR_VOL_SPACEUSED |
        ATTR_VOL_NAME;

    if (getattrlist(mount, &list, buf, sizeof(buf), 0) != 0)
        return false;

    uint32_t length = 0;
    memcpy(&length, buf, sizeof(length));

    // getattrlist packs 64-bit values on 4-byte boundaries, in man-page
    // order: size, available, used, then the name reference.
    if (length < 36)
        return false;

    memcpy(capacity, buf + 4, sizeof(*capacity));
    memcpy(available, buf + 12, sizeof(*available));
    memcpy(used, buf + 20, sizeof(*used));

    int32_t name_offset = 0;
    uint32_t name_size = 0;
    memcpy(&name_offset, buf + 28, sizeof(name_offset));
    memcpy(&name_size, buf + 32, sizeof(name_size));

    if (name_offset < 0 || name_size == 0)
        return true;

    size_t start = 28u + (size_t)name_offset;

    if (start >= length || start + name_size > length)
        return true;

    if (buf[start + name_size - 1] == '\0' && name_size > 0)
        name_size--;

    if (name_size >= name_len)
        name_size = (uint32_t)name_len - 1;

    memcpy(name, buf + start, name_size);
    name[name_size] = '\0';

    return true;
}


static bool interesting_volume(const struct statfs *fs, bool *network)
{
    *network = false;

    if (strcmp(fs->f_fstypename, "devfs") == 0 ||
        strcmp(fs->f_fstypename, "autofs") == 0)
        return false;

    if (fs->f_flags & MNT_LOCAL)
        return true;

    if (strcmp(fs->f_fstypename, "smbfs") == 0 ||
        strcmp(fs->f_fstypename, "nfs") == 0 ||
        strcmp(fs->f_fstypename, "afpfs") == 0 ||
        strcmp(fs->f_fstypename, "webdav") == 0) {
        *network = true;
        return true;
    }

    return false;
}


static bool volume_before(const VolumeInfo *a, const VolumeInfo *b)
{
    if (a->startup != b->startup)
        return a->startup;

    bool a_data = strcmp(a->mount, "/System/Volumes/Data") == 0;
    bool b_data = strcmp(b->mount, "/System/Volumes/Data") == 0;

    if (a_data != b_data)
        return a_data;

    if (a->used != b->used)
        return a->used > b->used;

    return strcmp(a->mount, b->mount) < 0;
}


static void sort_volume_indexes(
    const VolumeInfo *volumes,
    int *indexes,
    int count
)
{
    for (int i = 1; i < count; i++) {
        int saved = indexes[i];
        int j = i;

        while (j > 0 &&
               volume_before(&volumes[saved], &volumes[indexes[j - 1]])) {
            indexes[j] = indexes[j - 1];
            j--;
        }

        indexes[j] = saved;
    }
}


static bool drive_product(io_registry_entry_t media, char *buf, size_t n)
{
    io_registry_entry_t current = media;
    bool found = false;

    IOObjectRetain(current);

    for (int depth = 0; depth < 8 && !found; depth++) {
        io_registry_entry_t parent = IO_OBJECT_NULL;

        if (IORegistryEntryGetParentEntry(
                current,
                kIOServicePlane,
                &parent) != KERN_SUCCESS)
            break;

        CFTypeRef chars = IORegistryEntryCreateCFProperty(
            parent,
            CFSTR(kIOPropertyDeviceCharacteristicsKey),
            kCFAllocatorDefault,
            0
        );

        if (chars && CFGetTypeID(chars) == CFDictionaryGetTypeID()) {
            CFTypeRef product = CFDictionaryGetValue(
                (CFDictionaryRef)chars,
                CFSTR(kIOPropertyProductNameKey)
            );

            if (product)
                found = cfstring_to_buf((CFStringRef)product, buf, n);
        }

        if (chars)
            CFRelease(chars);

        IOObjectRelease(current);
        current = parent;
    }

    if (current)
        IOObjectRelease(current);

    return found;
}


static void print_volume_row(const VolumeInfo *volume)
{
    char used[32];
    format_bytes(volume->used, used, sizeof(used), true);

    printf(
        "    %-36.36s %-26.26s %12s   %-10s\n",
        volume->name[0] ? volume->name : volume->mount,
        volume->mount,
        used,
        volume->role
    );
}


void print_storage(void)
{
    struct statfs *mounts = NULL;
    int mount_count = getmntinfo(&mounts, MNT_NOWAIT);

    print_banner("Storage");

    if (mount_count <= 0 || !mounts) {
        fprintf(stderr, "Could not list mounted volumes.\n\n");
        return;
    }

    VolumeInfo *volumes = calloc((size_t)MAX_VOLUMES, sizeof(VolumeInfo));

    if (!volumes) {
        fprintf(stderr, "Out of memory.\n\n");
        return;
    }

    int volume_count = 0;

    for (int i = 0; i < mount_count && volume_count < MAX_VOLUMES; i++) {
        bool network = false;

        if (!interesting_volume(&mounts[i], &network))
            continue;

        VolumeInfo *volume = &volumes[volume_count];

        snprintf(volume->mount, sizeof(volume->mount), "%s", mounts[i].f_mntonname);
        snprintf(volume->role, sizeof(volume->role), "%s", volume_role(volume->mount));
        volume->network = network;
        volume->startup = strcmp(volume->mount, "/") == 0;
        volume->disk_unit = -1;
        parse_disk_unit(mounts[i].f_mntfromname, &volume->disk_unit);

        bool got_attrs = read_volume_attrs(
            volume->mount,
            &volume->capacity,
            &volume->available,
            &volume->used,
            volume->name,
            sizeof(volume->name)
        );

        if (!got_attrs) {
            uint64_t bsize = mounts[i].f_bsize ? mounts[i].f_bsize : 1;
            volume->capacity = mounts[i].f_blocks * bsize;
            volume->available = mounts[i].f_bavail * bsize;

            if (mounts[i].f_blocks >= mounts[i].f_bfree)
                volume->used = (mounts[i].f_blocks - mounts[i].f_bfree) * bsize;

            const char *slash = strrchr(volume->mount, '/');
            snprintf(
                volume->name,
                sizeof(volume->name),
                "%s",
                (slash && slash[1]) ? slash + 1 : volume->mount
            );
        }

        if (volume->capacity == 0 && volume->used == 0)
            continue;

        volume_count++;
    }

    VolumeInfo *startup = NULL;

    for (int i = 0; i < volume_count; i++) {
        if (volumes[i].startup) {
            startup = &volumes[i];
            break;
        }
    }

    if (startup && startup->capacity > 0) {
        uint64_t container_used = startup->capacity > startup->available
            ? startup->capacity - startup->available
            : 0;
        double pct = (double)container_used / (double)startup->capacity * 100.0;
        char cap[32];
        char avail[32];
        char used[32];
        char bar[32];

        format_bytes(startup->capacity, cap, sizeof(cap), true);
        format_bytes(startup->available, avail, sizeof(avail), true);
        format_bytes(container_used, used, sizeof(used), true);
        format_bar(pct, bar, sizeof(bar));

        printf("  Startup disk\n");
        printf("  %-14s %s\n", "Capacity", cap);
        printf("  %-14s %s\n", "Available", avail);
        printf("  %-14s %s\n", "Used", used);
        printf("  %s  %5.1f%%\n\n", bar, pct);
    }

    printf("  Sizes are decimal, same as Finder (1 GB = 1000 MB).\n");
    printf("  APFS volumes in one container share its free space.\n");
    printf("  Per-volume used does not add up to container used,\n");
    printf("  because snapshots and purgeable space sit outside it.\n\n");

    printf(
        "  %-4s %-14s %-14s %s\n",
        "Disk",
        "Capacity",
        "Free",
        "Volumes"
    );
    printf("  ----------------------------------------------------------------\n");

    bool container_seen[MAX_VOLUMES] = { false };

    for (int i = 0; i < volume_count; i++) {
        if (container_seen[i] || volumes[i].network)
            continue;

        int indexes[MAX_VOLUMES];
        int members = 0;
        int unit = volumes[i].disk_unit;
        uint64_t capacity = volumes[i].capacity;
        uint64_t available = volumes[i].available;
        bool is_startup = false;

        for (int j = i; j < volume_count && members < MAX_VOLUMES; j++) {
            if (volumes[j].network)
                continue;

            bool same = (unit >= 0 && volumes[j].disk_unit == unit) ||
                        (unit < 0 && j == i);

            if (!same)
                continue;

            container_seen[j] = true;
            indexes[members++] = j;

            if (volumes[j].startup)
                is_startup = true;

            if (volumes[j].capacity > capacity)
                capacity = volumes[j].capacity;
        }

        sort_volume_indexes(volumes, indexes, members);

        char cap[32];
        char avail[32];
        format_bytes(capacity, cap, sizeof(cap), true);
        format_bytes(available, avail, sizeof(avail), true);

        if (unit >= 0) {
            printf(
                "  %-4d %-14s %-14s %s\n",
                unit,
                cap,
                avail,
                is_startup ? "(startup)" : ""
            );
        } else {
            printf(
                "  %-4s %-14s %-14s\n",
                "-",
                cap,
                avail
            );
        }

        printf(
            "    %-36s %-26s %12s   %s\n",
            "Volume",
            "Mount",
            "Used",
            "Role"
        );

        for (int m = 0; m < members; m++)
            print_volume_row(&volumes[indexes[m]]);

        printf("\n");
    }

    bool printed_network = false;

    for (int i = 0; i < volume_count; i++) {
        if (!volumes[i].network)
            continue;

        if (!printed_network) {
            printf("  Network volumes\n");
            printed_network = true;
        }

        print_volume_row(&volumes[i]);
    }

    if (printed_network)
        printf("\n");

    free(volumes);

    CFMutableDictionaryRef match = IOServiceMatching("IOMedia");

    if (!match)
        return;

    CFDictionarySetValue(match, CFSTR(kIOMediaWholeKey), kCFBooleanTrue);

    io_iterator_t drives = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(
        kIOMainPortDefault,
        match,
        &drives
    );

    if (result != KERN_SUCCESS)
        return;

    DriveInfo found[MAX_DRIVES];
    int drive_count = 0;
    int skipped_images = 0;
    io_object_t media;

    while ((media = IOIteratorNext(drives)) != IO_OBJECT_NULL) {
        char content[64] = "";
        char product[128] = "";
        char bsd[32] = "";

        registry_string(media, CFSTR(kIOMediaContentKey), content, sizeof(content));

        bool partition =
            strcmp(content, "GUID_partition_scheme") == 0 ||
            strcmp(content, "FDisk_partition_scheme") == 0 ||
            strcmp(content, "Apple_partition_scheme") == 0;

        if (!partition) {
            IOObjectRelease(media);
            continue;
        }

        int64_t size = 0;

        if (!registry_int64(media, CFSTR(kIOMediaSizeKey), &size) || size <= 0) {
            IOObjectRelease(media);
            continue;
        }

        if (!drive_product(media, product, sizeof(product)))
            snprintf(product, sizeof(product), "Disk");

        if (strcasecmp(product, "Disk Image") == 0) {
            skipped_images++;
            IOObjectRelease(media);
            continue;
        }

        registry_string(media, CFSTR(kIOBSDNameKey), bsd, sizeof(bsd));

        CFTypeRef removable_ref = IORegistryEntryCreateCFProperty(
            media,
            CFSTR(kIOMediaRemovableKey),
            kCFAllocatorDefault,
            0
        );

        bool removable = removable_ref &&
                         CFGetTypeID(removable_ref) == CFBooleanGetTypeID() &&
                         CFBooleanGetValue((CFBooleanRef)removable_ref);

        if (removable_ref)
            CFRelease(removable_ref);

        if (drive_count < MAX_DRIVES) {
            DriveInfo *drive = &found[drive_count++];

            snprintf(drive->name, sizeof(drive->name), "%s", product);
            snprintf(drive->bsd, sizeof(drive->bsd), "%s", bsd[0] ? bsd : "-");
            snprintf(drive->where, sizeof(drive->where), "%s",
                     removable ? "External" : "Internal");
            drive->size = (uint64_t)size;
        }

        IOObjectRelease(media);
    }

    IOObjectRelease(drives);

    if (drive_count > 0) {
        for (int i = 1; i < drive_count; i++) {
            DriveInfo saved = found[i];
            int j = i;

            while (j > 0 && found[j - 1].size < saved.size) {
                found[j] = found[j - 1];
                j--;
            }

            found[j] = saved;
        }

        printf("  Physical drives\n");
        printf(
            "  %-24s %-8s %-10s %s\n",
            "Name",
            "Device",
            "Location",
            "Size"
        );

        for (int i = 0; i < drive_count; i++) {
            char size[32];
            format_bytes(found[i].size, size, sizeof(size), true);

            printf(
                "  %-24.24s %-8s %-10s %s\n",
                found[i].name,
                found[i].bsd,
                found[i].where,
                size
            );
        }

        printf("\n");
    }

    if (skipped_images > 0) {
        printf(
            "  Skipped %d disk image%s.\n\n",
            skipped_images,
            skipped_images == 1 ? "" : "s"
        );
    }
}
