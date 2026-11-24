/*
 * SCLP event type 0x07: file access
 *
 * The service element serves files by name to the running system: the files
 * of the medium or server location a system was loaded from ("load from
 * removable media or server"), which installers read after the list-directed
 * load, and which Linux knows as the HMC drive (drivers/s390/char/sclp_ftp.c).
 * This device serves the files below one host directory.
 *
 * A request is a WRITE EVENT DATA event buffer. It is accepted at once, runs
 * in a worker thread, and its result is presented as an event of the same
 * type, announced with a service interrupt and fetched with READ EVENT DATA.
 *
 * Two request forms exist, told apart by the routing code:
 *   3  load a file with an 8-character EBCDIC name at a real address
 *   4  file commands (get, put, append, list, delete) with an ASCII path,
 *      an offset and a length. The data buffer is either a real address
 *      (real-space ASCE) or a virtual address in the address space that the
 *      ASCE designates; the tables are walked here, the CPU's DAT state is
 *      not involved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <dirent.h>
#include <sys/syscall.h>
#include <linux/openat2.h>
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "hw/s390x/sclp.h"
#include "hw/s390x/event-facility.h"
#include "hw/s390x/ebcdic.h"
#include "trace.h"

#define SCLP_FILE_ROUTE_LOAD    3
#define SCLP_FILE_ROUTE_FTP     4
#define SCLP_FILE_PCX           0x80

/* completion codes (ldflg) */
#define SCLP_FILE_OK            0x80
#define SCLP_FILE_FAILED        0x01
#define SCLP_FILE_NOT_PERMITTED 0x02
#define SCLP_FILE_RUNS          0x03

enum {
    SCLP_FILE_CMD_NOOP = 0,
    SCLP_FILE_CMD_GET = 1,
    SCLP_FILE_CMD_PUT = 2,
    SCLP_FILE_CMD_APPEND = 3,
    SCLP_FILE_CMD_DIR = 4,
    SCLP_FILE_CMD_NLIST = 5,
    SCLP_FILE_CMD_DELETE = 6,
    SCLP_FILE_CMD_CANCEL = 7,
};

#define SCLP_FILE_NAME_LEN      256
#define SCLP_FILE_IMAGE_LEN     8
#define SCLP_FILE_CHUNK_PAGES   256
#define SCLP_FILE_PAGE_SIZE     4096

#define ASCE_ORIGIN             (~0xfffULL)
#define ASCE_REAL_SPACE         0x20
#define ASCE_TYPE_MASK          0x0c
#define REGION_ENTRY_INVALID    0x20
#define SEGMENT_ENTRY_INVALID   0x20
#define SEGMENT_ENTRY_FC        0x400
#define PAGE_ENTRY_INVALID      0x400

typedef struct SclpFileHeader {
    EventBufferHeader ebh;
    uint16_t route;
    uint8_t pcx;
    uint8_t ldflg;
} QEMU_PACKED SclpFileHeader;

typedef struct SclpFileFtp {
    SclpFileHeader h;
    uint8_t cmd;
    uint8_t pgsize;
    uint8_t srcflg;
    uint8_t spare;
    uint64_t offset;
    uint64_t fsize;
    uint64_t length;
    uint64_t failaddr;
    uint64_t bufaddr;
    uint64_t asce;
    uint8_t fident[SCLP_FILE_NAME_LEN];
} QEMU_PACKED SclpFileFtp;

typedef struct SclpFileLoad {
    SclpFileHeader h;
    uint8_t type;
    uint8_t flags[3];
    uint32_t origin;
    uint8_t image_out[SCLP_FILE_IMAGE_LEN];
    uint8_t image_in[SCLP_FILE_IMAGE_LEN];
} QEMU_PACKED SclpFileLoad;

typedef union SclpFileRequest {
    SclpFileHeader h;
    SclpFileFtp ftp;
    SclpFileLoad load;
} SclpFileRequest;

#define TYPE_SCLP_FILE_ACCESS "sclpfileaccess"
OBJECT_DECLARE_SIMPLE_TYPE(SCLPFileAccess, SCLP_FILE_ACCESS)

struct SCLPFileAccess {
    SCLPEvent event;

    char *path;
    bool readonly;

    QemuMutex lock;         /* protects root_fd */
    int root_fd;

    /* the request in flight and then its result, in guest byte order */
    SclpFileRequest req;
    uint16_t req_len;
    bool busy;              /* worker thread running (BQL) */
    bool cancel;
    QemuThread thread;
};

static bool can_handle_event(uint8_t type)
{
    return type == SCLP_EVENT_FILE_ACCESS;
}

static sccb_mask_t send_mask(void)
{
    return SCLP_EVENT_MASK_FILE_ACCESS;
}

static sccb_mask_t receive_mask(void)
{
    return SCLP_EVENT_MASK_FILE_ACCESS;
}

/* Host side: files below the root directory only */

static int sclp_file_root(SCLPFileAccess *fa)
{
    int fd;

    qemu_mutex_lock(&fa->lock);
    fd = fa->root_fd < 0 ? -1 : dup(fa->root_fd);
    qemu_mutex_unlock(&fa->lock);
    return fd;
}

static int sclp_file_openat(int root, const char *name, int flags, mode_t mode)
{
    struct open_how how = {
        .flags = flags | O_CLOEXEC,
        .mode = (flags & O_CREAT) ? mode : 0,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS,
    };

    while (*name == '/') {
        name++;
    }
    if (!*name) {
        name = ".";
    }
    return syscall(SYS_openat2, root, name, &how, sizeof(how));
}

/* Guest side: the data buffer of a request */

typedef struct SclpFileBuffer {
    uint64_t asce;
    uint64_t addr;
} SclpFileBuffer;

static bool sclp_file_read_entry(uint64_t origin, uint64_t index,
                                 uint64_t *entry)
{
    uint64_t value;

    if (address_space_read(&address_space_memory, origin + index * 8,
                           MEMTXATTRS_UNSPECIFIED, &value, 8) != MEMTX_OK) {
        return false;
    }
    *entry = be64_to_cpu(value);
    return true;
}

/* The absolute address of the page that holds @vaddr, false if not mapped */
static bool sclp_file_translate(const SclpFileBuffer *buf, uint64_t vaddr,
                                hwaddr *page)
{
    uint64_t origin = buf->asce & ASCE_ORIGIN;
    uint64_t entry;
    int level;

    if (buf->asce & ASCE_REAL_SPACE) {
        *page = vaddr & ~0xfffULL;
        return true;
    }

    /* 3: region first ... 1: region third, 0: segment table */
    level = (buf->asce & ASCE_TYPE_MASK) >> 2;
    if (level < 3 && (vaddr >> (31 + 11 * level))) {
        return false;
    }
    for (; level > 0; level--) {
        if (!sclp_file_read_entry(origin, (vaddr >> (20 + 11 * level)) & 0x7ff,
                                  &entry) ||
            (entry & REGION_ENTRY_INVALID)) {
            return false;
        }
        origin = entry & ASCE_ORIGIN;
    }
    if (!sclp_file_read_entry(origin, (vaddr >> 20) & 0x7ff, &entry) ||
        (entry & SEGMENT_ENTRY_INVALID)) {
        return false;
    }
    if (entry & SEGMENT_ENTRY_FC) {
        *page = (entry & ~0xfffffULL) | (vaddr & 0xff000);
        return true;
    }
    if (!sclp_file_read_entry(entry & ~0x7ffULL, (vaddr >> 12) & 0xff,
                              &entry) ||
        (entry & PAGE_ENTRY_INVALID)) {
        return false;
    }
    *page = entry & ~0xfffULL;
    return true;
}

/*
 * Copy between host memory and the guest buffer at @pos. Returns the number
 * of bytes copied, which is short when the buffer is not mapped any further.
 */
static size_t sclp_file_copy(const SclpFileBuffer *buf, uint64_t pos,
                             uint8_t *data, size_t len, bool to_guest)
{
    size_t done = 0;

    bql_lock();
    while (done < len) {
        uint64_t vaddr = buf->addr + pos + done;
        uint64_t in_page = vaddr & (SCLP_FILE_PAGE_SIZE - 1);
        size_t n = MIN(len - done, SCLP_FILE_PAGE_SIZE - in_page);
        hwaddr page;

        if (!sclp_file_translate(buf, vaddr, &page) ||
            address_space_rw(&address_space_memory, page | in_page,
                             MEMTXATTRS_UNSPECIFIED, data + done, n,
                             to_guest) != MEMTX_OK) {
            break;
        }
        done += n;
    }
    bql_unlock();
    return done;
}

/* File commands */

static int sclp_file_errno_code(int err)
{
    switch (err) {
    case EACCES:
    case EPERM:
    case EROFS:
    case EXDEV:     /* RESOLVE_BENEATH: the path leaves the root */
    case ELOOP:
        return SCLP_FILE_NOT_PERMITTED;
    default:
        return SCLP_FILE_FAILED;
    }
}

/* Read from @fd at @offset into the guest buffer */
static int sclp_file_get(SCLPFileAccess *fa, int fd, const SclpFileBuffer *buf,
                         uint64_t offset, uint64_t length, uint64_t *done)
{
    g_autofree uint8_t *data = g_malloc(SCLP_FILE_CHUNK_PAGES *
                                        SCLP_FILE_PAGE_SIZE);

    *done = 0;
    while (*done < length && !qatomic_read(&fa->cancel)) {
        size_t want = MIN(length - *done,
                          SCLP_FILE_CHUNK_PAGES * SCLP_FILE_PAGE_SIZE);
        ssize_t n = pread(fd, data, want, offset + *done);
        size_t copied;

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SCLP_FILE_FAILED;
        }
        if (n == 0) {
            break;
        }
        copied = sclp_file_copy(buf, *done, data, n, true);
        *done += copied;
        if (copied < n) {
            break;
        }
    }
    return SCLP_FILE_OK;
}

/* Write the guest buffer to @fd */
static int sclp_file_put(SCLPFileAccess *fa, int fd, const SclpFileBuffer *buf,
                         uint64_t length, uint64_t *done)
{
    g_autofree uint8_t *data = g_malloc(SCLP_FILE_CHUNK_PAGES *
                                        SCLP_FILE_PAGE_SIZE);

    *done = 0;
    while (*done < length && !qatomic_read(&fa->cancel)) {
        size_t want = MIN(length - *done,
                          SCLP_FILE_CHUNK_PAGES * SCLP_FILE_PAGE_SIZE);
        size_t copied = sclp_file_copy(buf, *done, data, want, false);
        size_t written = 0;

        while (written < copied) {
            ssize_t n = write(fd, data + written, copied - written);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return SCLP_FILE_FAILED;
            }
            written += n;
        }
        *done += copied;
        if (copied < want) {
            break;
        }
    }
    return SCLP_FILE_OK;
}

/*
 * Directory listing as text. Long form: one "ls -l" style line per entry,
 * as an FTP server sends for LIST. Name list: the names of the regular
 * files, one per line.
 */
static int sclp_file_name_cmp(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static GString *sclp_file_list(int dirfd, bool names_only)
{
    static const char months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    GString *out = g_string_new(NULL);
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    DIR *dir = fdopendir(dirfd);
    struct dirent *de;
    time_t now = time(NULL);

    if (!dir) {
        close(dirfd);
        return out;
    }
    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
            g_ptr_array_add(names, g_strdup(de->d_name));
        }
    }
    g_ptr_array_sort(names, sclp_file_name_cmp);

    for (unsigned int i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        struct stat st;
        struct tm tm;
        char when[16];

        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) ||
            !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
            continue;
        }
        if (names_only) {
            if (S_ISREG(st.st_mode)) {
                g_string_append_printf(out, "%s\n", name);
            }
            continue;
        }
        gmtime_r(&st.st_mtime, &tm);
        if (st.st_mtime > now || now - st.st_mtime > 180 * 24 * 3600) {
            snprintf(when, sizeof(when), "%5d", tm.tm_year + 1900);
        } else {
            snprintf(when, sizeof(when), "%02d:%02d", tm.tm_hour, tm.tm_min);
        }
        g_string_append_printf(out, "%c%s 1 0 0 %12" PRIu64 " %s %2d %s %s\n",
                               S_ISDIR(st.st_mode) ? 'd' : '-',
                               S_ISDIR(st.st_mode) ? "r-xr-xr-x" : "r--r--r--",
                               (uint64_t)st.st_size, months[tm.tm_mon],
                               tm.tm_mday, when, name);
    }
    closedir(dir);
    return out;
}

static void sclp_file_do_ftp(SCLPFileAccess *fa, SclpFileFtp *ftp)
{
    g_autofree char *name = g_strndup((const char *)ftp->fident,
                                      SCLP_FILE_NAME_LEN);
    SclpFileBuffer buf = {
        .asce = be64_to_cpu(ftp->asce),
        .addr = be64_to_cpu(ftp->bufaddr),
    };
    uint64_t offset = be64_to_cpu(ftp->offset);
    uint64_t length = be64_to_cpu(ftp->length);
    uint64_t done = 0, fsize = 0;
    int code = SCLP_FILE_FAILED;
    int root = -1, fd = -1;
    struct stat st;

    trace_sclp_file_ftp(ftp->pgsize, ftp->srcflg, ftp->spare, ftp->h.pcx,
                        ftp->h.ldflg, be64_to_cpu(ftp->fsize),
                        be64_to_cpu(ftp->failaddr));
    trace_sclp_file_request(ftp->cmd, name, offset, length, buf.addr,
                            buf.asce);

    if (ftp->cmd == SCLP_FILE_CMD_NOOP || ftp->cmd == SCLP_FILE_CMD_CANCEL) {
        code = SCLP_FILE_OK;
        goto out;
    }
    root = sclp_file_root(fa);
    if (root < 0) {
        code = SCLP_FILE_NOT_PERMITTED;
        goto out;
    }

    switch (ftp->cmd) {
    case SCLP_FILE_CMD_GET:
        fd = sclp_file_openat(root, name, O_RDONLY, 0);
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
            code = fd < 0 ? sclp_file_errno_code(errno) : SCLP_FILE_FAILED;
            break;
        }
        fsize = st.st_size;
        code = sclp_file_get(fa, fd, &buf, offset, length, &done);
        break;
    case SCLP_FILE_CMD_PUT:
    case SCLP_FILE_CMD_APPEND:
        if (fa->readonly) {
            code = SCLP_FILE_NOT_PERMITTED;
            break;
        }
        fd = sclp_file_openat(root, name, O_WRONLY | O_CREAT |
                              (ftp->cmd == SCLP_FILE_CMD_PUT ? O_TRUNC
                                                             : O_APPEND),
                              0644);
        if (fd < 0) {
            code = sclp_file_errno_code(errno);
            break;
        }
        code = sclp_file_put(fa, fd, &buf, length, &done);
        if (!fstat(fd, &st)) {
            fsize = st.st_size;
        }
        break;
    case SCLP_FILE_CMD_DIR:
    case SCLP_FILE_CMD_NLIST: {
        g_autoptr(GString) list = NULL;

        fd = sclp_file_openat(root, name, O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0) {
            code = sclp_file_errno_code(errno);
            break;
        }
        list = sclp_file_list(fd, ftp->cmd == SCLP_FILE_CMD_NLIST);
        fd = -1;    /* closed by the listing */
        fsize = list->len;
        if (offset < list->len) {
            done = sclp_file_copy(&buf, 0, (uint8_t *)list->str + offset,
                                  MIN(length, list->len - offset), true);
        }
        code = SCLP_FILE_OK;
        break;
    }
    case SCLP_FILE_CMD_DELETE: {
        g_autofree char *dirname = g_path_get_dirname(name);
        g_autofree char *basename = g_path_get_basename(name);

        if (fa->readonly) {
            code = SCLP_FILE_NOT_PERMITTED;
            break;
        }
        fd = sclp_file_openat(root, dirname, O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0 || unlinkat(fd, basename, 0)) {
            code = sclp_file_errno_code(errno);
            break;
        }
        code = SCLP_FILE_OK;
        break;
    }
    default:
        break;
    }

out:
    if (fd >= 0) {
        close(fd);
    }
    if (root >= 0) {
        close(root);
    }
    ftp->h.ldflg = code;
    ftp->length = cpu_to_be64(done);
    ftp->fsize = cpu_to_be64(fsize);
    trace_sclp_file_result(ftp->cmd, code, done, fsize);
}

/* Load a file with an EBCDIC name at a real address */
static void sclp_file_do_load(SCLPFileAccess *fa, SclpFileLoad *load)
{
    char name[SCLP_FILE_IMAGE_LEN + 1];
    SclpFileBuffer buf = {
        .asce = ASCE_REAL_SPACE,
        .addr = be32_to_cpu(load->origin),
    };
    uint64_t done = 0;
    int code = SCLP_FILE_FAILED;
    int root, fd, i;
    struct stat st;

    for (i = 0; i < SCLP_FILE_IMAGE_LEN && load->image_in[i] != 0x40 &&
                load->image_in[i]; i++) {
        name[i] = ebcdic2ascii[load->image_in[i]];
    }
    name[i] = '\0';
    trace_sclp_file_load(load->type, load->flags[0], load->flags[1],
                         load->flags[2], load->h.pcx, load->h.ldflg);
    trace_sclp_file_request(0x100 | load->type, name, 0, 0, buf.addr,
                            buf.asce);

    root = sclp_file_root(fa);
    if (root < 0) {
        code = SCLP_FILE_NOT_PERMITTED;
        goto out;
    }
    fd = sclp_file_openat(root, name, O_RDONLY, 0);
    if (fd < 0) {
        code = sclp_file_errno_code(errno);
    } else {
        if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
            code = sclp_file_get(fa, fd, &buf, 0, st.st_size, &done);
            if (code == SCLP_FILE_OK && done < st.st_size) {
                code = SCLP_FILE_FAILED;
            }
        }
        close(fd);
    }
    close(root);
out:
    if (code == SCLP_FILE_OK) {
        memcpy(load->image_out, load->image_in, SCLP_FILE_IMAGE_LEN);
    }
    load->h.ldflg = code;
    trace_sclp_file_result(0x100 | load->type, code, done, done);
}

static void *sclp_file_worker(void *opaque)
{
    SCLPFileAccess *fa = opaque;

    if (be16_to_cpu(fa->req.h.route) == SCLP_FILE_ROUTE_FTP) {
        sclp_file_do_ftp(fa, &fa->req.ftp);
    } else {
        sclp_file_do_load(fa, &fa->req.load);
    }

    bql_lock();
    fa->event.event_pending = true;
    sclp_service_interrupt(0);
    bql_unlock();
    return NULL;
}

/* Wait for the worker thread. Called with the BQL held. */
static void sclp_file_finish(SCLPFileAccess *fa, bool cancel)
{
    if (!fa->busy) {
        return;
    }
    qatomic_set(&fa->cancel, cancel);
    bql_unlock();
    qemu_thread_join(&fa->thread);
    bql_lock();
    fa->busy = false;
    qatomic_set(&fa->cancel, false);
}

static int write_event_data(SCLPEvent *event, EventBufferHeader *ebh)
{
    SCLPFileAccess *fa = SCLP_FILE_ACCESS(event);
    SclpFileHeader *h = (SclpFileHeader *)ebh;
    uint16_t len = be16_to_cpu(ebh->length);
    uint16_t need;

    if (len < sizeof(SclpFileHeader)) {
        return SCLP_RC_EVENT_BUFFER_SYNTAX_ERROR;
    }
    switch (be16_to_cpu(h->route)) {
    case SCLP_FILE_ROUTE_FTP:
        need = sizeof(SclpFileFtp);
        break;
    case SCLP_FILE_ROUTE_LOAD:
        need = sizeof(SclpFileLoad);
        break;
    default:
        return SCLP_RC_INVALID_FUNCTION;
    }
    if (len < need) {
        return SCLP_RC_EVENT_BUFFER_SYNTAX_ERROR;
    }

    if (fa->busy && !event->event_pending) {
        /* one request at a time */
        return SCLP_RC_INVALID_FUNCTION;
    }
    /* a result that was never fetched is dropped */
    sclp_file_finish(fa, false);
    event->event_pending = false;

    memcpy(&fa->req, ebh, need);
    fa->req_len = need;
    fa->busy = true;
    qemu_thread_create(&fa->thread, "sclp-file", sclp_file_worker, fa,
                       QEMU_THREAD_JOINABLE);

    ebh->flags |= SCLP_EVENT_BUFFER_ACCEPTED;
    return SCLP_RC_NORMAL_COMPLETION;
}

static int read_event_data(SCLPEvent *event, EventBufferHeader *evt_buf_hdr,
                           int *slen)
{
    SCLPFileAccess *fa = SCLP_FILE_ACCESS(event);

    if (!event->event_pending || *slen < fa->req_len) {
        return 0;
    }
    sclp_file_finish(fa, false);
    event->event_pending = false;

    memcpy(evt_buf_hdr, &fa->req, fa->req_len);
    evt_buf_hdr->length = cpu_to_be16(fa->req_len);
    evt_buf_hdr->type = SCLP_EVENT_FILE_ACCESS;
    evt_buf_hdr->flags = SCLP_EVENT_BUFFER_ACCEPTED;
    *slen -= fa->req_len;
    return 1;
}

/* The directory: settable while the machine runs, empty for none */

static char *sclp_file_get_path(Object *obj, Error **errp)
{
    return g_strdup(SCLP_FILE_ACCESS(obj)->path ?: "");
}

static void sclp_file_set_path(Object *obj, const char *value, Error **errp)
{
    SCLPFileAccess *fa = SCLP_FILE_ACCESS(obj);
    int fd = -1, old;

    if (value && *value) {
        fd = open(value, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            error_setg_errno(errp, errno, "cannot open directory '%s'", value);
            return;
        }
    }
    qemu_mutex_lock(&fa->lock);
    old = fa->root_fd;
    fa->root_fd = fd;
    qemu_mutex_unlock(&fa->lock);
    if (old >= 0) {
        close(old);
    }
    g_free(fa->path);
    fa->path = g_strdup(value && *value ? value : NULL);
}

static int sclp_file_pre_save(void *opaque)
{
    sclp_file_finish(opaque, false);
    return 0;
}

static const VMStateDescription vmstate_sclp_file = {
    .name = TYPE_SCLP_FILE_ACCESS,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = sclp_file_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(event.event_pending, SCLPFileAccess),
        VMSTATE_UINT16(req_len, SCLPFileAccess),
        VMSTATE_BUFFER_UNSAFE(req, SCLPFileAccess, 0, sizeof(SclpFileRequest)),
        VMSTATE_END_OF_LIST()
    }
};

static void sclp_file_reset(DeviceState *dev)
{
    SCLPFileAccess *fa = SCLP_FILE_ACCESS(dev);

    sclp_file_finish(fa, true);
    fa->event.event_pending = false;
    fa->req_len = 0;
}

static void sclp_file_unrealize(DeviceState *dev)
{
    sclp_file_finish(SCLP_FILE_ACCESS(dev), true);
}

static void sclp_file_instance_init(Object *obj)
{
    SCLPFileAccess *fa = SCLP_FILE_ACCESS(obj);

    qemu_mutex_init(&fa->lock);
    fa->root_fd = -1;
}

static void sclp_file_instance_finalize(Object *obj)
{
    SCLPFileAccess *fa = SCLP_FILE_ACCESS(obj);

    if (fa->root_fd >= 0) {
        close(fa->root_fd);
    }
    g_free(fa->path);
    qemu_mutex_destroy(&fa->lock);
}

static const Property sclp_file_properties[] = {
    DEFINE_PROP_BOOL("readonly", SCLPFileAccess, readonly, true),
};

static void sclp_file_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCLPEventClass *k = SCLP_EVENT_CLASS(klass);

    device_class_set_props(dc, sclp_file_properties);
    device_class_set_legacy_reset(dc, sclp_file_reset);
    dc->unrealize = sclp_file_unrealize;
    dc->vmsd = &vmstate_sclp_file;
    dc->desc = "SCLP file access (files of a host directory by name)";
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    object_class_property_add_str(klass, "path", sclp_file_get_path,
                                  sclp_file_set_path);
    object_class_property_set_description(klass, "path",
        "Host directory the guest can access; empty for none. "
        "Can be changed while the machine runs");

    k->get_send_mask = send_mask;
    k->get_receive_mask = receive_mask;
    k->can_handle_event = can_handle_event;
    k->read_event_data = read_event_data;
    k->write_event_data = write_event_data;
}

static const TypeInfo sclp_file_info = {
    .name          = TYPE_SCLP_FILE_ACCESS,
    .parent        = TYPE_SCLP_EVENT,
    .instance_size = sizeof(SCLPFileAccess),
    .instance_init = sclp_file_instance_init,
    .instance_finalize = sclp_file_instance_finalize,
    .class_init    = sclp_file_class_init,
    .class_size    = sizeof(SCLPEventClass),
};

static void register_types(void)
{
    type_register_static(&sclp_file_info);
}

type_init(register_types)
