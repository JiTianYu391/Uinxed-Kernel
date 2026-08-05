/*
 *
 *      printk.h
 *      Kernel string print header file
 *
 *      2024/6/27 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PRINTK_H_
#define INCLUDE_PRINTK_H_

#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>

#ifndef KERNEL_LOG
#    define KERNEL_LOG 1
#endif

/* Log levels, Linux-style \001<digit> prefixes parsed by printk().
 * Messages with a level above console_loglevel are dropped. */
#define KERN_EMERG   "\001" "0"
#define KERN_ALERT   "\001" "1"
#define KERN_CRIT    "\001" "2"
#define KERN_ERR     "\001" "3"
#define KERN_WARNING "\001" "4"
#define KERN_NOTICE  "\001" "5"
#define KERN_INFO    "\001" "6"
#define KERN_DEBUG   "\001" "7"

/* Console log levels, mirroring Linux: records with a level above
 * console_loglevel are kept in the kmsg ring but not printed to the
 * console.  Managed via /proc/sys/kernel/printk and the loglevel= boot
 * argument. */
extern int console_loglevel;
extern int default_loglevel;
extern int minimum_loglevel;
extern int boot_loglevel;

/* Emit a fully formatted record (must end with '\n'): ring + console. */
void printk_emit(int level, const char *text, size_t len);

#define pr_emerg(...)   printk(KERN_EMERG __VA_ARGS__)
#define pr_alert(...)   printk(KERN_ALERT __VA_ARGS__)
#define pr_crit(...)    printk(KERN_CRIT __VA_ARGS__)
#define pr_err(...)     printk(KERN_ERR __VA_ARGS__)
#define pr_warn(...)    printk(KERN_WARNING __VA_ARGS__)
#define pr_notice(...)  printk(KERN_NOTICE __VA_ARGS__)
#define pr_info(...)    printk(KERN_INFO __VA_ARGS__)
#define pr_debug(...)   printk(KERN_DEBUG __VA_ARGS__)

/* Emit a message at most once per site. */
#define printk_once(fmt, ...)                   \
    do {                                        \
        static bool _printed_once_ = false;     \
        if (!_printed_once_) {                  \
            _printed_once_ = true;              \
            printk(fmt, ##__VA_ARGS__);         \
        }                                       \
    } while (0)

typedef enum {
    OFLOW_AT_FMTARG,
    OFLOW_AT_FMTSTR,
} overflow_kind_t;

typedef struct {
        uint64_t size;       // The size of the buff to write
        char    *buff;       // The buff to write
        char    *last_write; // The last write position
} fmt_arg_t;

typedef struct {
        overflow_kind_t kind; // The kind of overflow
        fmt_arg_t      *arg;  // The argument that overflow
} overflow_signal_t;

typedef struct {
        char  *buf;
        size_t idx;
        size_t size; /* Buffer size for safe writing, 0 means unlimited */
} unsafe_buf_data;

typedef struct {
        const char **fmt_ptr;       // a pointer to `fmt`
        size_t      *write_counter; // for `%n`
} args_fmter;

/* Kernel print string */
void printk(const char *format, ...);

/* Kernel print log */
void plogk(const char *format, ...);

/* Handler of unsafe buf writing */
uint8_t unsafe_buf_write(writer *writer, char c);

/* Handler of safe buf writing with size limit */
uint8_t unsafe_buf_write_safe(writer *writer, char c);

/* Store the formatted output in a character array */
int sprintf(char *str, const char *fmt, ...);

/* Store the formatted output in a character array with size limit */
int snprintf(char *str, size_t size, const char *fmt, ...);

/* Format with va_list, then store the formatted output in a character array */
int vsprintf(char *str, const char *fmt, va_list args);

/* Format with va_list, then store the formatted output in a character array with size limit */
int vsnprintf(char *str, size_t size, const char *fmt, va_list args);

/* Formatted output processing */
void wfmt_arg(writer *writer, args_fmter *fmter, va_list args);

/* Use a `writer` to write formatted string */
size_t vwprintf(writer *writer, const char *fmt, va_list args);

#endif // INCLUDE_PRINTK_H_
