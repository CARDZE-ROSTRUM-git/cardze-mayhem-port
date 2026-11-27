/*
 * Host FatFs replacement backed by a plain host directory.
 *
 * The firmware talks to the SD card exclusively through the FatFs API (f_*),
 * so the cleanest way to satisfy the user's "SD card = one folder" requirement
 * is to implement that same API over POSIX instead of over a FAT block device.
 * Every call is mapped to ~/mayhem-sd/ (override with the MAYHEM_SD environment
 * variable), keeping the firmware's directory layout (CAPTURES, FREQMAN, ...)
 * but without a real FAT volume.
 *
 * Only the entry points the portable code actually calls are provided. Paths
 * are FatFs-style ("/CAPTURES/foo.TXT", or "" for the volume root); the leading
 * drive designator and separator are mapped onto the host root directory.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "ff.h"
#include "diskio.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

/* Directory scanning uses std::filesystem rather than <dirent.h>: FatFs
 * declares a global type named DIR, which would collide with POSIX's DIR. */

namespace {

/* ---------------------------------------------------------------------- */
/* Root directory and path conversion.                                    */
/* ---------------------------------------------------------------------- */

std::string root_dir() {
    if (const char* env = std::getenv("MAYHEM_SD")) {
        if (*env) return env;
    }
    const char* home = std::getenv("HOME");
    std::string base = home ? home : ".";
    return base + "/mayhem-sd";
}

/* UTF-16 (TCHAR) -> UTF-8. */
std::string to_utf8(const char16_t* s) {
    std::string out;
    if (!s) return out;
    while (*s) {
        char32_t cp = *s++;
        if (cp >= 0xD800 && cp <= 0xDBFF && *s >= 0xDC00 && *s <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (*s++ - 0xDC00);
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

/* UTF-8 -> UTF-16, terminated. Writes at most `max` units (incl. terminator). */
void from_utf8(const std::string& in, char16_t* out, size_t max) {
    if (max == 0) return;
    size_t o = 0;
    for (size_t i = 0; i < in.size() && o + 1 < max;) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        char32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            i++;
            continue;
        }
        if (i + extra >= in.size()) break;
        for (size_t k = 0; k < extra; k++) {
            cp = (cp << 6) | (static_cast<unsigned char>(in[i + 1 + k]) & 0x3F);
        }
        i += extra + 1;
        if (cp >= 0x10000) {
            if (o + 2 >= max) break;
            cp -= 0x10000;
            out[o++] = static_cast<char16_t>(0xD800 + (cp >> 10));
            out[o++] = static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out[o++] = static_cast<char16_t>(cp);
        }
    }
    out[o] = 0;
}

/* Strip a FatFs drive designator ("0:") and map the remaining path under root. */
std::string host_path(const char16_t* fspath) {
    std::string p = to_utf8(fspath);
    if (p.size() >= 2 && p[1] == ':') {
        p.erase(0, 2);
    }
    std::string root = root_dir();
    if (p.empty() || p == "/") {
        return root;
    }
    if (p.front() == '/') {
        return root + p;
    }
    return root + "/" + p;
}

/* ---------------------------------------------------------------------- */
/* Open-file and open-directory side tables.                              */
/*                                                                        */
/* FIL and DIR are the firmware's own structs (we cannot extend them), so  */
/* the POSIX handles live here, keyed by the caller's object address.      */
/* ---------------------------------------------------------------------- */

struct OpenFile {
    FILE* fp{nullptr};
};

struct DirEntry {
    std::string name;  // UTF-8
    bool is_dir{false};
    uint64_t size{0};
    std::time_t mtime{0};
};

struct OpenDir {
    std::vector<DirEntry> entries;
    size_t index{0};
    std::string pattern;  // UTF-8 wildcard, matched against `name`
};

std::mutex g_mutex;
std::map<FIL*, OpenFile> g_files;
std::map<DIR*, OpenDir> g_dirs;

/* Update the size field the firmware reads through the FatFs macros (f_size,
 * f_tell, f_eof, f_error). */
void set_size(FIL* fp, uint64_t size) {
    fp->obj.objsize = static_cast<FSIZE_t>(size);
    fp->err = 0;
}

/* Wildcard match with '*' and '?', case-insensitive (FAT semantics). */
bool wildcard_match(const char* pat, const char* name) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            for (const char* n = name; ; n++) {
                if (wildcard_match(pat, n)) return true;
                if (!*n) return false;
            }
        } else if (*pat == '?') {
            if (!*name) return false;
            pat++;
            name++;
        } else {
            if (std::toupper(static_cast<unsigned char>(*pat)) !=
                std::toupper(static_cast<unsigned char>(*name))) {
                return false;
            }
            pat++;
            name++;
        }
    }
    return *name == 0;
}

bool is_hidden_name(const char* n) {
    return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

void fill_filinfo(const DirEntry& e, FILINFO* fno) {
    std::memset(fno, 0, sizeof(*fno));
    fno->fsize = static_cast<FSIZE_t>(e.size);
    fno->fattrib = e.is_dir ? AM_DIR : AM_ARC;
    from_utf8(e.name, reinterpret_cast<char16_t*>(fno->fname),
              sizeof(fno->fname) / sizeof(TCHAR));
    /* FAT timestamps: date = ((year-1980)<<9)|(month<<5)|day,
     * time = (hour<<11)|(minute<<5)|(second/2). */
    struct tm t{};
    gmtime_r(&e.mtime, &t);
    if (t.tm_year >= 80) {
        fno->fdate = static_cast<WORD>(((t.tm_year - 80) << 9) |
                                       ((t.tm_mon + 1) << 5) | t.tm_mday);
        fno->ftime = static_cast<WORD>((t.tm_hour << 11) |
                                       (t.tm_min << 5) | (t.tm_sec / 2));
    }
}

void collect_entries(const std::string& dir, OpenDir& od) {
    od.entries.clear();
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return;
    for (const auto& entry : it) {
        const std::string name = entry.path().filename().string();
        if (is_hidden_name(name.c_str())) continue;
        DirEntry e;
        e.name = name;
        std::error_code sec;
        const auto st = entry.symlink_status(sec);
        e.is_dir = !sec && std::filesystem::is_directory(st);
        if (!e.is_dir) {
            std::error_code fec;
            const auto sz = entry.file_size(fec);
            if (!fec) e.size = static_cast<uint64_t>(sz);
        }
        std::error_code tec;
        const auto ftime = entry.last_write_time(tec);
        if (!tec) {
            /* file_time_type -> time_t via the C++20 clock conversion. */
            const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            e.mtime = std::chrono::system_clock::to_time_t(sctp);
        }
        od.entries.push_back(std::move(e));
    }
    std::sort(od.entries.begin(), od.entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
}

}  // namespace

/* ---------------------------------------------------------------------- */
/* FatFs API                                                              */
/* ---------------------------------------------------------------------- */

extern "C" {

FRESULT f_mount(FATFS* fs, const TCHAR* path, BYTE opt) {
    (void)fs;
    (void)path;
    (void)opt;
    /* Make sure the backing folder exists; a missing SD is not an error here,
     * the firmware creates its directory tree on demand. */
    const std::string root = root_dir();
    ::mkdir(root.c_str(), 0775);
    return FR_OK;
}

FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode) {
    if (!fp) return FR_INVALID_OBJECT;
    const std::string host = host_path(path);

    /* FatFs opens files, never directories: f_open("") or a directory path
     * fails with FR_NO_FILE. On the host fopen() happily opens a directory (it
     * only fails on the first read), so an empty/"directory" path would hand the
     * firmware an open handle whose reads return zero bytes without ever
     * reaching EOF. FileLineReader then advances by the (empty) line length and
     * spins forever - the remote app's load hangs the UI at 100% CPU. Reject it
     * here to match the device. */
    {
        struct stat st{};
        if (::stat(host.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return FR_NO_FILE;
        }
    }

    const char* fopen_mode = nullptr;
    bool exclusive = false;

    if (mode & FA_CREATE_NEW) {
        exclusive = true;
        fopen_mode = (mode & FA_READ) ? "w+b" : "wb";
    } else if (mode & FA_CREATE_ALWAYS) {
        fopen_mode = "w+b";
    } else if ((mode & 0x30) == 0x30) { /* FA_OPEN_APPEND */
        fopen_mode = "a+b";
    } else if (mode & FA_OPEN_ALWAYS) {
        fopen_mode = "r+b";
    } else if (mode & FA_WRITE) {
        fopen_mode = "r+b";
    } else {
        fopen_mode = "rb";
    }

    if (exclusive) {
        struct stat st{};
        if (::stat(host.c_str(), &st) == 0) return FR_EXIST;
    }

    FILE* f = std::fopen(host.c_str(), fopen_mode);
    if (!f && (mode & FA_OPEN_ALWAYS) && !(mode & FA_CREATE_NEW)) {
        /* Open-or-create: the file was absent, so create it. */
        f = std::fopen(host.c_str(), (mode & FA_READ) ? "w+b" : "wb");
    }
    if (!f) {
        struct stat st{};
        return (::stat(host.c_str(), &st) != 0) ? FR_NO_FILE : FR_DENIED;
    }

    std::memset(&fp->obj, 0, sizeof(fp->obj));
    fp->fptr = 0;
    fp->err = 0;

    struct stat st{};
    if (::fstat(fileno(f), &st) == 0) {
        set_size(fp, static_cast<uint64_t>(st.st_size));
    } else {
        set_size(fp, 0);
    }

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_files[fp] = OpenFile{f};
    }
    return FR_OK;
}

FRESULT f_close(FIL* fp) {
    if (!fp) return FR_INVALID_OBJECT;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_files.find(fp);
    if (it == g_files.end()) return FR_INVALID_OBJECT;
    if (it->second.fp) std::fclose(it->second.fp);
    g_files.erase(it);
    return FR_OK;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br) {
    if (br) *br = 0;
    if (!fp || !buff) return FR_INVALID_OBJECT;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_files.find(fp);
    if (it == g_files.end()) return FR_INVALID_OBJECT;
    const size_t n = std::fread(buff, 1, btr, it->second.fp);
    if (br) *br = static_cast<UINT>(n);
    fp->fptr += n;
    return FR_OK;
}

FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw) {
    if (bw) *bw = 0;
    if (!fp || !buff) return FR_INVALID_OBJECT;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_files.find(fp);
    if (it == g_files.end()) return FR_INVALID_OBJECT;
    const size_t n = std::fwrite(buff, 1, btw, it->second.fp);
    if (bw) *bw = static_cast<UINT>(n);
    fp->fptr += n;
    if (fp->fptr > fp->obj.objsize) fp->obj.objsize = fp->fptr;
    return (n == btw) ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs) {
    if (!fp) return FR_INVALID_OBJECT;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_files.find(fp);
    if (it == g_files.end()) return FR_INVALID_OBJECT;
    if (::fseeko(it->second.fp, static_cast<off_t>(ofs), SEEK_SET) != 0) {
        return FR_DISK_ERR;
    }
    fp->fptr = ofs;
    return FR_OK;
}

FRESULT f_truncate(FIL* fp) {
    if (!fp) return FR_INVALID_OBJECT;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_files.find(fp);
    if (it == g_files.end()) return FR_INVALID_OBJECT;
    if (ftruncate(fileno(it->second.fp), static_cast<off_t>(fp->fptr)) != 0) {
        return FR_DISK_ERR;
    }
    fp->obj.objsize = fp->fptr;
    return FR_OK;
}

FRESULT f_sync(FIL* fp) {
    if (!fp) return FR_INVALID_OBJECT;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_files.find(fp);
    if (it == g_files.end()) return FR_INVALID_OBJECT;
    return (std::fflush(it->second.fp) == 0) ? FR_OK : FR_DISK_ERR;
}

FRESULT f_opendir(DIR* dp, const TCHAR* path) {
    if (!dp) return FR_INVALID_OBJECT;
    const std::string host = host_path(path);
    struct stat st{};
    if (::stat(host.c_str(), &st) != 0) return FR_NO_PATH;
    if (!S_ISDIR(st.st_mode)) return FR_NO_PATH;

    OpenDir od;
    collect_entries(host, od);
    od.pattern = "*";
    std::memset(&dp->obj, 0, sizeof(dp->obj));
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_dirs[dp] = std::move(od);
    }
    return FR_OK;
}

FRESULT f_closedir(DIR* dp) {
    if (!dp) return FR_INVALID_OBJECT;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_dirs.erase(dp);
    return FR_OK;
}

FRESULT f_readdir(DIR* dp, FILINFO* fno) {
    if (!dp) return FR_INVALID_OBJECT;
    if (!fno) return f_closedir(dp);
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_dirs.find(dp);
    if (it == g_dirs.end()) return FR_INVALID_OBJECT;
    OpenDir& od = it->second;

    while (od.index < od.entries.size()) {
        const DirEntry& e = od.entries[od.index++];
        if (od.pattern.empty() || od.pattern == "*" ||
            wildcard_match(od.pattern.c_str(), e.name.c_str())) {
            fill_filinfo(e, fno);
            return FR_OK;
        }
    }
    std::memset(fno, 0, sizeof(*fno));
    return FR_OK;
}

FRESULT f_findfirst(DIR* dp, FILINFO* fno, const TCHAR* path, const TCHAR* pattern) {
    const FRESULT r = f_opendir(dp, path);
    if (r != FR_OK) return r;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_dirs.find(dp);
        if (it != g_dirs.end()) it->second.pattern = to_utf8(pattern);
    }
    return f_readdir(dp, fno);
}

FRESULT f_findnext(DIR* dp, FILINFO* fno) {
    return f_readdir(dp, fno);
}

FRESULT f_stat(const TCHAR* path, FILINFO* fno) {
    const std::string host = host_path(path);
    struct stat st{};
    if (::stat(host.c_str(), &st) != 0) return FR_NO_FILE;

    DirEntry e;
    const size_t slash = host.find_last_of('/');
    e.name = (slash == std::string::npos) ? host : host.substr(slash + 1);
    e.is_dir = S_ISDIR(st.st_mode);
    e.size = static_cast<uint64_t>(st.st_size);
    e.mtime = st.st_mtime;
    if (fno) fill_filinfo(e, fno);
    return FR_OK;
}

FRESULT f_mkdir(const TCHAR* path) {
    const std::string host = host_path(path);
    if (::mkdir(host.c_str(), 0775) != 0) {
        struct stat st{};
        if (::stat(host.c_str(), &st) == 0) return FR_EXIST;
        return FR_DENIED;
    }
    return FR_OK;
}

FRESULT f_unlink(const TCHAR* path) {
    const std::string host = host_path(path);
    struct stat st{};
    if (::stat(host.c_str(), &st) != 0) return FR_NO_FILE;
    if (S_ISDIR(st.st_mode)) {
        if (::rmdir(host.c_str()) != 0) return FR_DENIED;
    } else {
        if (::unlink(host.c_str()) != 0) return FR_DENIED;
    }
    return FR_OK;
}

FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new) {
    const std::string from = host_path(path_old);
    const std::string to = host_path(path_new);
    if (::rename(from.c_str(), to.c_str()) != 0) return FR_DENIED;
    return FR_OK;
}

FRESULT f_getfree(const TCHAR* path, DWORD* nclst, FATFS** fatfs) {
    (void)path;

    /* The firmware's std::filesystem::space() shim dereferences the FATFS it
     * gets back here (fs->csize, fs->n_fatent), so a valid object must always be
     * returned -- a null pointer segfaults when an app opens its file view.
     * Report the host volume's free/total space in 512-byte clusters so the
     * "time available" readouts are plausible. */
    static FATFS host_fatfs{};
    host_fatfs.csize = 1;  /* 1 * _MIN_SS (512) bytes per cluster */

    struct statvfs vfs{};
    if (::statvfs(root_dir().c_str(), &vfs) == 0) {
        const uint64_t total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
        const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        host_fatfs.n_fatent = static_cast<DWORD>(total_bytes / 512) + 2;
        if (nclst) *nclst = static_cast<DWORD>(free_bytes / 512);
    } else {
        host_fatfs.n_fatent = 2;
        if (nclst) *nclst = 0;
    }

    if (fatfs) *fatfs = &host_fatfs;
    return FR_OK;
}

FRESULT f_utime(const TCHAR* path, const FILINFO* fno) {
    const std::string host = host_path(path);
    struct stat st{};
    if (::stat(host.c_str(), &st) != 0) return FR_NO_FILE;
    if (fno) {
        struct tm t{};
        t.tm_year = ((fno->fdate >> 9) & 0x7F) + 80;
        t.tm_mon = ((fno->fdate >> 5) & 0x0F) - 1;
        t.tm_mday = fno->fdate & 0x1F;
        t.tm_hour = (fno->ftime >> 11) & 0x1F;
        t.tm_min = (fno->ftime >> 5) & 0x3F;
        t.tm_sec = (fno->ftime & 0x1F) * 2;
        struct utimbuf ut{};
        const std::time_t when = timegm(&t);
        ut.actime = when;
        ut.modtime = when;
        if (::utime(host.c_str(), &ut) != 0) return FR_DENIED;
    }
    return FR_OK;
}

/* ---- Character I/O (TCHAR == UTF-16) ---- */

int f_putc(TCHAR c, FIL* fp) {
    const uint16_t v = static_cast<uint16_t>(c);
    UINT bw = 0;
    const uint8_t bytes[2] = {static_cast<uint8_t>(v & 0xFF),
                              static_cast<uint8_t>((v >> 8) & 0xFF)};
    if (f_write(fp, bytes, 2, &bw) != FR_OK || bw != 2) return EOF;
    return static_cast<int>(c);
}

int f_puts(const TCHAR* str, FIL* fp) {
    int n = 0;
    for (const char16_t* p = reinterpret_cast<const char16_t*>(str); *p; p++) {
        if (f_putc(static_cast<TCHAR>(*p), fp) == EOF) return EOF;
        n++;
    }
    return n;
}

int f_printf(FIL* fp, const TCHAR* str, ...) {
    /* Format in UTF-8, then write the result as UTF-16. */
    const std::string fmt = to_utf8(reinterpret_cast<const char16_t*>(str));
    char buf[512];
    va_list ap;
    va_start(ap, str);
    const int len = std::vsnprintf(buf, sizeof(buf), fmt.c_str(), ap);
    va_end(ap);
    if (len < 0) return -1;

    std::string text(buf, static_cast<size_t>(std::min<int>(len, sizeof(buf) - 1)));
    char16_t u16[512];
    from_utf8(text, u16, sizeof(u16) / sizeof(u16[0]));
    return f_puts(reinterpret_cast<const TCHAR*>(u16), fp);
}

TCHAR* f_gets(TCHAR* buff, int len, FIL* fp) {
    if (!buff || len <= 0) return nullptr;
    char16_t* out = reinterpret_cast<char16_t*>(buff);
    int n = 0;
    while (n < len - 1) {
        uint16_t v = 0;
        UINT br = 0;
        if (f_read(fp, &v, 2, &br) != FR_OK || br != 2) break;
        out[n++] = static_cast<char16_t>(v);
        if (v == '\n') break;
    }
    out[n] = 0;
    return (n > 0) ? buff : nullptr;
}

/* ---- Low-level disk I/O (referenced by the SD debug screen) ---- */

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count) {
    (void)pdrv;
    (void)buff;
    (void)sector;
    (void)count;
    return RES_NOTRDY;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count) {
    (void)pdrv;
    (void)buff;
    (void)sector;
    (void)count;
    return RES_NOTRDY;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            if (buff) *static_cast<DWORD*>(buff) = 0;
            return RES_OK;
        case GET_SECTOR_SIZE:
            if (buff) *static_cast<WORD*>(buff) = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            if (buff) *static_cast<DWORD*>(buff) = 1;
            return RES_OK;
        case MMC_GET_TYPE:
            if (buff) *static_cast<BYTE*>(buff) = 0;
            return RES_OK;
        case MMC_GET_CSD:
        case MMC_GET_CID:
        case MMC_GET_OCR:
        case MMC_GET_SDSTAT:
            if (buff) std::memset(buff, 0, 16);
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    const std::time_t now = std::time(nullptr);
    struct tm t{};
    gmtime_r(&now, &t);
    return static_cast<DWORD>(((t.tm_year + 1900 - 1980) << 25) |
                              ((t.tm_mon + 1) << 21) | (t.tm_mday << 16) |
                              (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
}

}  // extern "C"
