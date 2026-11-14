/*
 * Host-side storage for the LPC43xx register blocks and for the firmware
 * memory regions that Mayhem accesses through fixed addresses:
 *   - backup RAM (persistent settings: theme, calibration, ...)
 *   - the M0/M4 shared memory block (message queues)
 *
 * On the real hardware the backup RAM is battery-backed, so the firmware
 * expects whatever it wrote there to still be present after a power cycle.
 * A plain anonymous mapping would zero it on every start, which is exactly why
 * the UI theme (and every other setting) used to reset on each launch. To match
 * the hardware, the backup RAM region is mapped onto a file inside the emulated
 * SD card (MAP_SHARED), so writes are kept across restarts. The shared-memory
 * region only lives for the duration of a run and stays anonymous.
 *
 * Both regions are mmap'ed at the exact addresses used by the upstream memory
 * map so that the unmodified upstream code keeps working.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#include "hal.h"
#include "memory_map.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace host_regs {
LPC_CREG_Type creg{};
LPC_RTC_Type rtc{};
LPC_CGU_Type cgu{};
LPC_CCU1_Type ccu1{};
LPC_CCU2_Type ccu2{};
LPC_RGU_Type rgu{};
LPC_SCU_Type scu{};
LPC_GPIO_Type gpio{};
LPC_GPIO_INT_Type gpio_int{};
LPC_ADCx_Type adc0{};
LPC_ADCx_Type adc1{};
LPC_SSPx_Type ssp1{};
LPC_SGPIO_Type sgpio{};
LPC_EVENTROUTER_Type eventrouter{};
}  // namespace host_regs

namespace {

void* map_at(const char* name, uint32_t base, size_t size, int flags, int fd) {
    void* wanted = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
    void* got = ::mmap(wanted, size, PROT_READ | PROT_WRITE, flags | MAP_FIXED, fd, 0);
    if (got == MAP_FAILED || got != wanted) {
        std::fprintf(stderr,
                     "mayhem-desktop: cannot map %s at 0x%08x (%zu bytes): %s\n",
                     name, base, size, std::strerror(errno));
        std::abort();
    }
    return got;
}

/* Zeroed, process-private region: used for the shared memory block. */
void map_anonymous(const char* name, uint32_t base, size_t size) {
    void* got = map_at(name, base, size, MAP_PRIVATE | MAP_ANONYMOUS, -1);
    std::memset(got, 0, size);
}

/* Resolve the emulated SD root the same way the FatFs shim does, so the
 * persistent store travels with the "Mayhem Desktop" folder. */
std::string sd_root() {
    if (const char* env = std::getenv("MAYHEM_SD")) {
        if (*env) return env;
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/mayhem-sd";
}

/* Folder the backup-RAM region is currently mapped against. Kept separately
 * from sd_root() (which reads MAYHEM_SD) because the launcher sets MAYHEM_SD
 * before asking for the remap, so the two would otherwise look equal and the
 * remap would be skipped while the region still pointed at the old folder. */
std::string g_backup_ram_dir;

/* File-backed region: stands in for the battery-backed backup RAM, so the
 * firmware's persistent memory survives a restart. If the backing file cannot
 * be created the region silently falls back to an anonymous mapping, which
 * still lets the program run (only persistence is lost). */
void map_backup_ram(uint32_t base, size_t size) {
    const std::string dir = sd_root();
    const std::string path = dir + "/.pmem_backup_ram";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);  /* ignore: open() reports the real error */

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        std::fprintf(stderr,
                     "mayhem-desktop: cannot open persistent store %s: %s "
                     "(settings will not be saved)\n",
                     path.c_str(), std::strerror(errno));
        map_anonymous("backup_ram", base, size);
        return;
    }

    /* Make sure the file is at least the size of the region; a freshly created
     * file is sparse and reads back as zeros, i.e. an empty/uncalibrated pmem,
     * which the firmware detects via its checksum and fills with defaults. */
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        std::fprintf(stderr,
                     "mayhem-desktop: cannot size persistent store %s: %s\n",
                     path.c_str(), std::strerror(errno));
    }

    map_at("backup_ram", base, size, MAP_SHARED, fd);
    ::close(fd);
    g_backup_ram_dir = dir;
}

__attribute__((constructor(101))) void map_firmware_regions() {
    map_anonymous("shared_memory",
                  portapack::memory::map::shared_memory.base(),
                  portapack::memory::map::shared_memory.size());
    map_backup_ram(portapack::memory::map::backup_ram.base(),
                   portapack::memory::map::backup_ram.size());

    /* NOTE: the backup RAM region above is mapped at load time from the default
     * SD folder, before the launcher has run. The launcher may pick a different
     * folder; host_regs::remap_backup_ram() replaces this mapping once the choice
     * is known, and it is called before portapack::init() ever reads the region
     * (nothing dereferences it between here and there), so the remap is safe. */

    /* Peripherals addressed through raw pointers rather than the LPC_* macros:
     * common/i2s.hpp does reinterpret_cast<LPC_I2S_Type*>(BaseAddress) and
     * common/gpdma.cpp dereferences LPC_GPDMA (0x40002000). Those addresses must
     * be writable, so the register blocks are mapped like the rest. One page
     * each is more than enough for the MMR window. */
    map_anonymous("gpdma", 0x40002000U, 0x1000U);
    map_anonymous("i2s0", 0x400a2000U, 0x1000U);
    map_anonymous("i2s1", 0x400a3000U, 0x1000U);
}

}  // namespace

namespace host_regs {

void remap_backup_ram(const char* sd_dir) {
    if (sd_dir == nullptr || *sd_dir == '\0') return;

    /* The constructor already mapped the region against the default folder. Only
     * re-map when the launcher asked for a different one, so the common case (no
     * launcher, or the default folder) keeps the original mapping untouched. */
    if (g_backup_ram_dir == sd_dir) return;

    const uint32_t base = portapack::memory::map::backup_ram.base();
    const size_t size = portapack::memory::map::backup_ram.size();

    const std::string dir = sd_dir;
    const std::string path = dir + "/.pmem_backup_ram";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec); /* ignore: open() reports errors */

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        std::fprintf(stderr,
                     "mayhem-desktop: cannot open persistent store %s: %s "
                     "(settings will not be saved)\n",
                     path.c_str(), std::strerror(errno));
        return; /* keep the constructor's mapping */
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        std::fprintf(stderr, "mayhem-desktop: cannot size persistent store %s: %s\n",
                     path.c_str(), std::strerror(errno));
    }

    /* MAP_FIXED replaces the existing mapping atomically. This runs before any
     * firmware code reads the region (the launcher runs before portapack::init),
     * so no stale pointer is observed mid-swap. */
    map_at("backup_ram", base, size, MAP_SHARED, fd);
    ::close(fd);
}

}  // namespace host_regs
