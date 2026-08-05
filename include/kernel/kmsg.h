/*
 *
 *      kmsg.h
 *      Kernel message ring buffer (Linux-compatible log store)
 *
 *      2026/8/5 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KERNEL_KMSG_H_
#define INCLUDE_KERNEL_KMSG_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Ring buffer capacity in bytes (Linux default log_buf is 1/4 of RAM, capped;
 * a fixed 256 KiB keeps the boot-time footprint predictable; log_buf_len=
 * overrides it). */
#define KMSG_BUFFER_SIZE (256 * 1024)

/* printk.devkmsg= boot argument, mirroring Linux: "on" accepts every write,
 * "off" rejects them (-EPERM), "ratelimit" (default) accepts them. */
enum kmsg_devkmsg_mode {
    KMSG_DEVKMSG_RATELIMIT,
    KMSG_DEVKMSG_ON,
    KMSG_DEVKMSG_OFF,
};

extern enum kmsg_devkmsg_mode kmsg_devkmsg;

/* syslog(2) / klogctl(2) action numbers, identical to Linux. */
#define SYSLOG_ACTION_CLOSE        0
#define SYSLOG_ACTION_OPEN         1
#define SYSLOG_ACTION_READ         2
#define SYSLOG_ACTION_READ_ALL     3
#define SYSLOG_ACTION_READ_CLEAR   4
#define SYSLOG_ACTION_CLEAR        5
#define SYSLOG_ACTION_CONSOLE_OFF  6
#define SYSLOG_ACTION_CONSOLE_ON   7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD  9
#define SYSLOG_ACTION_SIZE_BUFFER  10

/* Append a record to the ring buffer.  text must end with '\n'. */
void kmsg_record(int level, const char *text, size_t len);

/* Copy all records as "<level>text" lines into dst; return bytes written.
 * If clear is true the ring is emptied afterwards (READ_CLEAR semantics). */
size_t kmsg_read_all(char *dst, size_t size, bool clear);

/* syslog READ: copy records not yet consumed through the syslog interface
 * (as "<level>text" lines, oldest first, up to size bytes) and advance the
 * syslog read cursor past them; returns bytes written. */
ssize_t kmsg_syslog_read(char *dst, size_t size);

/* Empty the ring buffer (and reset the syslog read cursor). */
void kmsg_clear(void);

/* Bytes of all records not yet consumed through syslog(2) (SIZE_UNREAD). */
size_t kmsg_unread_bytes(void);

/* Total bytes of all currently stored records including "<level>" prefixes. */
size_t kmsg_total_bytes(void);

/* Total ring buffer capacity in bytes. */
size_t kmsg_buffer_size(void);

/* Sequence number of the oldest stored record, or of the next record. */
uint64_t kmsg_first_seq(void);
uint64_t kmsg_next_seq(void);

/* /dev/kmsg record reader.  Emits one record in the Linux devkmsg format
 * "<level>,<seq>,<ts_usec>,<flags>;text\n" starting at cursor_seq; returns
 * the number of bytes copied (0 when caught up) and stores the next
 * cursor value in *next_seq. */
ssize_t kmsg_dev_read(uint64_t cursor_seq, char *dst, size_t size, uint64_t *next_seq);

/* /dev/kmsg writer: accepts "<level>msg\n", "msg\n" or full
 * "level,seq,ts,flags;msg" records; injects into the ring and console. */
ssize_t kmsg_dev_write(const char *buf, size_t size);

/* Early boot init: parses the loglevel= kernel command line parameter. */
void kmsg_init(void);

#endif // INCLUDE_KERNEL_KMSG_H_
