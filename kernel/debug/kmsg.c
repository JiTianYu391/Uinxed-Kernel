/*
 *
 *      kmsg.c
 *      Kernel message ring buffer (Linux-compatible log store)
 *
 *      2026/8/5 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Stores every printk record (regardless of console_loglevel) in a
 *  fixed-size buffer and exposes it to userspace through the syslog(2)
 *  syscall, /dev/kmsg and /proc/sys/kernel/printk, following the Linux
 *  printk/devkmsg interfaces.
 *
 *  Layout: records are packed contiguously in [tail, head); when a new
 *  record would not fit before the buffer end, the used block is compacted
 *  down to offset 0 and, if still too small, whole records are dropped
 *  from the oldest side.  There is no wrap-around state to get wrong.
 *
 */

#include <drivers/timer/tsc.h>
#include <kernel/cmdline.h>
#include <kernel/errno.h>
#include <kernel/kmsg.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Ring buffer                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  level;
    uint8_t  flags; /* unused; reserved for continuation ('c') records */
    uint16_t len;   /* text length including the trailing '\n' */
    uint64_t seq;
    uint64_t ts_usec;
} kmsg_record_hdr_t;

#define KMSG_HDR_SIZE 16 /* must stay 8-byte aligned */

static spinlock_t kmsg_lock;
static uint8_t   *kmsg_buf;
static size_t     kmsg_head;       /* next write offset; == tail when empty */
static size_t     kmsg_tail;       /* oldest record offset */
static uint64_t   kmsg_seq;        /* seq of the next record */
static uint64_t   kmsg_syslog_seq; /* syslog(2) read cursor */
static bool       kmsg_ready;

/* Round n up to an 8-byte boundary. */
static size_t kmsg_align(size_t n)
{
    return (n + 7) & ~(size_t)7;
}

/* Move the used block [tail, head) down to offset 0. */
static void kmsg_compact_locked(void)
{
    size_t used = kmsg_head - kmsg_tail;
    if (used && kmsg_tail) memmove(kmsg_buf, kmsg_buf + kmsg_tail, used);
    kmsg_head = used;
    kmsg_tail = 0;
}

/* Drop the oldest record; returns false when the ring was corrupt. */
static bool kmsg_drop_oldest_locked(void)
{
    while (kmsg_tail < kmsg_head) {
        size_t used = kmsg_head - kmsg_tail;
        if (used < KMSG_HDR_SIZE) { kmsg_tail = kmsg_head; break; }
        kmsg_record_hdr_t *hdr = (kmsg_record_hdr_t *)(kmsg_buf + kmsg_tail);
        size_t next = kmsg_align(kmsg_tail + KMSG_HDR_SIZE + hdr->len);
        if (next > kmsg_head) return false;
        kmsg_tail = next;
        if (kmsg_tail >= kmsg_head) kmsg_tail = kmsg_head;
    }
    if (kmsg_tail == kmsg_head) { kmsg_tail = kmsg_head = 0; kmsg_syslog_seq = 0; }
    return true;
}

void kmsg_record(int level, const char *text, size_t len)
{
    if (!kmsg_ready || !text) return;
    if (level < 0) level = default_loglevel;
    if (level > 7) level = 7;
    if (!len || text[len - 1] != '\n') len = len ? len + 1 : 0;
    if (!len) return;

    size_t total = KMSG_HDR_SIZE + len;
    if (total > KMSG_BUFFER_SIZE) {
        len   = KMSG_BUFFER_SIZE - KMSG_HDR_SIZE;
        total = KMSG_BUFFER_SIZE;
    }

    spin_lock(&kmsg_lock);
    for (;;) {
        size_t free = KMSG_BUFFER_SIZE - kmsg_head;
        if (free >= total) break;
        if (kmsg_tail) { kmsg_compact_locked(); continue; } /* recheck after compact */
        if (!kmsg_drop_oldest_locked()) { kmsg_tail = kmsg_head = 0; kmsg_syslog_seq = 0; }
        if (kmsg_head == 0 && KMSG_BUFFER_SIZE >= total) break; /* ring emptied */
    }

    kmsg_record_hdr_t *hdr = (kmsg_record_hdr_t *)(kmsg_buf + kmsg_head);
    hdr->level   = (uint8_t)level;
    hdr->flags   = 0;
    hdr->len     = (uint16_t)len;
    hdr->seq     = kmsg_seq++;
    hdr->ts_usec = (uint64_t)(nano_time() / 1000);
    memcpy(kmsg_buf + kmsg_head + KMSG_HDR_SIZE, text, len);
    kmsg_head += total;
    spin_unlock(&kmsg_lock);
}

/* ------------------------------------------------------------------ */
/*  syslog(2) accessors                                                */
/* ------------------------------------------------------------------ */

/* Write one record in "<level>text" form; returns bytes written. */
static size_t kmsg_format_record(const kmsg_record_hdr_t *hdr, char *dst, size_t size)
{
    size_t need = 3 + hdr->len; /* "<N>" + text */
    if (!dst || !size) return need;
    size_t out = 0;
    if (size > out) dst[out++] = '<';
    if (size > out) dst[out++] = (char)('0' + hdr->level);
    if (size > out) dst[out++] = '>';
    size_t n = hdr->len < size - out ? hdr->len : size - out;
    if (n) memcpy(dst + out, (const uint8_t *)hdr + KMSG_HDR_SIZE, n);
    out += n;
    return out;
}

/* Walk the ring from `from` to `to`, invoking visit() for every intact
 * record (sanity-checked header). */
static void kmsg_walk(size_t from, size_t to, void (*visit)(const kmsg_record_hdr_t *, size_t off, void *opaque), void *opaque)
{
    while (from < to) {
        if (to - from < KMSG_HDR_SIZE) break;
        const kmsg_record_hdr_t *hdr = (const kmsg_record_hdr_t *)(kmsg_buf + from);
        size_t next = kmsg_align(from + KMSG_HDR_SIZE + hdr->len);
        if (next > to) break;
        if (visit) visit(hdr, from, opaque);
        from = next;
    }
}

/* Sum of "<level>" prefix + text lengths over the range. */
static void kmsg_count_bytes(const kmsg_record_hdr_t *hdr, size_t off, void *opaque)
{
    (void)off;
    size_t *total = opaque;
    *total += 3 + hdr->len;
}

/* Copy one record into a userspace-bound scratch buffer. */
struct kmsg_copy_ctx {
        char   *dst;
        size_t  size;
        size_t  written;
};

static void kmsg_copy_record(const kmsg_record_hdr_t *hdr, size_t off, void *opaque)
{
    (void)off;
    struct kmsg_copy_ctx *ctx = opaque;
    if (ctx->written >= ctx->size) return;
    ctx->written += kmsg_format_record(hdr, ctx->dst + ctx->written, ctx->size - ctx->written);
}

size_t kmsg_read_all(char *dst, size_t size, bool clear)
{
    if (!kmsg_ready) return 0;
    struct kmsg_copy_ctx ctx = { dst, size, 0 };

    spin_lock(&kmsg_lock);
    kmsg_walk(kmsg_tail, kmsg_head, kmsg_copy_record, &ctx);
    spin_unlock(&kmsg_lock);
    if (clear) kmsg_clear();
    return ctx.written;
}

ssize_t kmsg_syslog_read(char *dst, size_t size)
{
    if (!kmsg_ready || !dst || !size) return 0;
    ssize_t  written = 0;
    uint64_t cursor  = kmsg_syslog_seq;

    spin_lock(&kmsg_lock);
    size_t off = kmsg_tail;
    while (off < kmsg_head) {
        if (kmsg_head - off < KMSG_HDR_SIZE) break;
        const kmsg_record_hdr_t *hdr = (const kmsg_record_hdr_t *)(kmsg_buf + off);
        size_t next = kmsg_align(off + KMSG_HDR_SIZE + hdr->len);
        if (next > kmsg_head) break;
        if (hdr->seq >= cursor) {
            size_t n = kmsg_format_record(hdr, dst + written, size - (size_t)written);
            written += (ssize_t)n;
            kmsg_syslog_seq = hdr->seq + 1;
            if ((size_t)written >= size) break;
        }
        off = next;
    }
    spin_unlock(&kmsg_lock);
    return written;
}

size_t kmsg_total_bytes(void)
{
    size_t total = 0;
    if (!kmsg_ready) return 0;

    spin_lock(&kmsg_lock);
    kmsg_walk(kmsg_tail, kmsg_head, kmsg_count_bytes, &total);
    spin_unlock(&kmsg_lock);
    return total;
}

size_t kmsg_unread_bytes(void)
{
    size_t   total  = 0;
    size_t   from   = kmsg_head;
    uint64_t target = kmsg_syslog_seq;

    spin_lock(&kmsg_lock);
    size_t off = kmsg_tail;
    while (off < kmsg_head) {
        if (kmsg_head - off < KMSG_HDR_SIZE) break;
        const kmsg_record_hdr_t *hdr = (const kmsg_record_hdr_t *)(kmsg_buf + off);
        size_t next = kmsg_align(off + KMSG_HDR_SIZE + hdr->len);
        if (next > kmsg_head) break;
        if (hdr->seq >= target) { from = off; break; }
        off = next;
    }
    if (from < kmsg_head) kmsg_walk(from, kmsg_head, kmsg_count_bytes, &total);
    spin_unlock(&kmsg_lock);
    return total;
}

size_t kmsg_buffer_size(void)
{
    return KMSG_BUFFER_SIZE;
}

void kmsg_clear(void)
{
    spin_lock(&kmsg_lock);
    kmsg_head = kmsg_tail = 0;
    kmsg_syslog_seq       = 0;
    spin_unlock(&kmsg_lock);
}

/* Find the first stored record with seq >= `seq`; returns its offset, or
 * kmsg_head when none exists. */
static size_t kmsg_find_seq_locked(uint64_t seq)
{
    size_t off = kmsg_tail;
    while (off < kmsg_head) {
        if (kmsg_head - off < KMSG_HDR_SIZE) break;
        const kmsg_record_hdr_t *hdr = (const kmsg_record_hdr_t *)(kmsg_buf + off);
        size_t next = kmsg_align(off + KMSG_HDR_SIZE + hdr->len);
        if (next > kmsg_head) break;
        if (hdr->seq >= seq) return off;
        off = next;
    }
    return kmsg_head;
}

uint64_t kmsg_first_seq(void)
{
    uint64_t seq = kmsg_seq;

    spin_lock(&kmsg_lock);
    size_t off = kmsg_find_seq_locked(0);
    if (off < kmsg_head) seq = ((kmsg_record_hdr_t *)(kmsg_buf + off))->seq;
    spin_unlock(&kmsg_lock);
    return seq;
}

uint64_t kmsg_next_seq(void)
{
    return kmsg_seq;
}

ssize_t kmsg_dev_read(uint64_t cursor_seq, char *dst, size_t size, uint64_t *next_seq)
{
    if (!kmsg_ready) return 0;
    if (!dst || !size || !next_seq) return -EINVAL;

    spin_lock(&kmsg_lock);
    size_t off = kmsg_find_seq_locked(cursor_seq);
    if (off >= kmsg_head) {
        *next_seq = kmsg_seq;
        spin_unlock(&kmsg_lock);
        return 0; /* caught up: EOF for nonblocking readers */
    }

    kmsg_record_hdr_t *hdr = (kmsg_record_hdr_t *)(kmsg_buf + off);

    /* devkmsg header: "<level>,<seq>,<ts_usec>,<flags>;" */
    char hbuf[48];
    int  hlen = snprintf(hbuf, sizeof(hbuf), "%u,%llu,%llu,;", hdr->level,
                         (unsigned long long)hdr->seq, (unsigned long long)hdr->ts_usec);
    if (hlen < 0) hlen = 0;
    if ((size_t)hlen > sizeof(hbuf)) hlen = (int)sizeof(hbuf);

    size_t need = (size_t)hlen + hdr->len;
    size_t out  = need < size ? need : size;
    size_t pos  = 0;
    if (pos < out) {
        size_t n = (size_t)hlen < out - pos ? (size_t)hlen : out - pos;
        memcpy(dst + pos, hbuf, n);
        pos += n;
    }
    if (pos < out) {
        size_t n = hdr->len < out - pos ? hdr->len : out - pos;
        memcpy(dst + pos, (const uint8_t *)hdr + KMSG_HDR_SIZE, n);
        pos += n;
    }
    *next_seq = hdr->seq + 1;
    spin_unlock(&kmsg_lock);
    return (ssize_t)pos;
}

ssize_t kmsg_dev_write(const char *buf, size_t size)
{
    if (!buf && size) return -EINVAL;
    if (!size) return 0;

    char *copy = malloc(size + 1);
    if (!copy) return -ENOMEM;
    memcpy(copy, buf, size);
    copy[size] = '\0';

    int         level = 6; /* LOG_INFO default, as in Linux */
    const char *text  = copy;

    if (text[0] == '<') {
        /* "<level>message" */
        const char *end = strchr(text, '>');
        if (end && (size_t)(end - text) >= 2 && (size_t)(end - text) <= 4 && text[1] >= '0' && text[1] <= '7') {
            level = text[1] - '0';
            text  = end + 1;
        }
    } else {
        /* "level,seq,ts[,flags];message" devkmsg injection form */
        const char *semi = strchr(text, ';');
        if (semi && text[0] >= '0' && text[0] <= '7') {
            level = text[0] - '0';
            text  = semi + 1;
        }
    }

    size_t len = size - (size_t)(text - copy);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    if (len) {
        char *msg = malloc(len + 2);
        if (msg) {
            memcpy(msg, text, len);
            msg[len]     = '\n';
            msg[len + 1] = '\0';
            printk_emit(level, msg, len + 1);
            free(msg);
        }
    }
    free(copy);
    return (ssize_t)size;
}

/* ------------------------------------------------------------------ */
/*  Init                                                               */
/* ------------------------------------------------------------------ */

void kmsg_init(void)
{
    if (kmsg_ready) return;
    kmsg_buf = malloc(KMSG_BUFFER_SIZE);
    if (!kmsg_buf) return;
    memset(kmsg_buf, 0, KMSG_BUFFER_SIZE);
    kmsg_head = kmsg_tail = 0;
    kmsg_seq             = 0;
    kmsg_syslog_seq      = 0;
    kmsg_ready           = true;

    /* Parse the loglevel= kernel command line parameter, like Linux. */
    const char *cmdline = get_cmdline();
    if (cmdline) {
        const char *p = cmdline;
        while ((p = strstr(p, "loglevel="))) {
            p += 9;
            if (*p >= '0' && *p <= '7') {
                console_loglevel = *p - '0';
                break;
            }
        }
    }
}
