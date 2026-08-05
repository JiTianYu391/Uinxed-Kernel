/*
 *
 *      printk.c
 *      Kernel string printing
 *
 *      2024/6/27 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/char/tty.h>
#include <drivers/ports/serial.h>
#include <kernel/kmsg.h>
#include <kernel/printk.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <sync/spin_lock.h>

#ifdef KERNEL_LOG
#    include <drivers/acpi/acpi.h>
#endif

#define BUF_SIZE 2048 // least 2 bytes (1 byte is for '\0')

/* Console log levels, mirroring Linux: records with a level at or above
 * console_loglevel are kept in the kmsg ring but not printed to the
 * console.  Controlled via /proc/sys/kernel/printk, syslog(2) and the
 * loglevel=/quiet/debug/ignore_loglevel boot parameters. */
int console_loglevel = 7; /* CONSOLE_LOGLEVEL_DEFAULT: everything but KERN_DEBUG */
int default_loglevel = 4; /* MESSAGE_LOGLEVEL_DEFAULT */
int minimum_loglevel = 1; /* CONSOLE_LOGLEVEL_MIN */
int boot_loglevel    = 7; /* default console loglevel */

/* ignore_loglevel boot argument (Linux: prints all messages to console). */
bool ignore_loglevel = false;

/* printk.time= boot argument (Linux PRINTK_TIME, default on). */
bool printk_time = true;

/* Lock for printk */
spinlock_t printk_lock = {
    .lock   = 0,
    .rflags = 0,
};

/* Lock for plogk */
spinlock_t plogk_lock = {
    .lock   = 0,
    .rflags = 0,
};

/* Mirrored writer: forwards every character to the active console and, once
 * the serial ports are up, to COM1 so headless debugging always works.
 * When the boot console *is* a serial port, that port already receives the
 * output through the line-buffered tty path, so no mirror is needed. */
static uint8_t klog_writer_handler(writer *writer, char c)
{
    (void)writer;
    tty_writer_handler(&tty_writer, c);
    tty_device_t *boot = get_boot_tty();
    if (!boot || boot->type == TTY_DEVICE_VGA || boot->type == TTY_DEVICE_DRM) write_serial(SERIAL_PORT_1, (uint8_t)c);
    return 1;
}

static writer klog_writer = {
    .data    = 0,
    .handler = klog_writer_handler,
};

/* Emit a fully formatted record (text must end with '\n'): store it in the
 * kmsg ring unconditionally, then print to the console unless the level is
 * suppressed (level >= console_loglevel, Linux's suppress_message_printing).
 * Used by printk() and /dev/kmsg. */
void printk_emit(int level, const char *text, size_t len)
{
    if (!text || !len) return;
    if (level < 0) level = default_loglevel;

    kmsg_record(level, text, len);

    if (!ignore_loglevel && level >= console_loglevel) return;
    spin_lock(&printk_lock); // Lock
    if (printk_time) {
        /* "[   12.345678] " prefix, as Linux prints when PRINTK_TIME is on. */
        char ts[24];
        int  n = snprintf(ts, sizeof(ts), "[%5llu.%06llu] ",
                          (unsigned long long)(nano_time() / 1000000000),
                          (unsigned long long)((nano_time() / 1000) % 1000000));
        if (n < 0) n = 0;
        if ((size_t)n > sizeof(ts)) n = (int)sizeof(ts);
        for (int i = 0; i < n; i++) klog_writer_handler(&klog_writer, ts[i]);
    }
    for (size_t i = 0; i < len; i++) klog_writer_handler(&klog_writer, text[i]);
    spin_unlock(&printk_lock); // Unlock
}

/* Kernel print string */
void printk(const char *format, ...)
{
    if (!format) return;

    /* Parse an optional \001<digit> log-level prefix. */
    int level = default_loglevel;
    if (format[0] == '\001' && format[1] >= '0' && format[1] <= '7') {
        level  = format[1] - '0';
        format += 2;
    }

    char buf[BUF_SIZE];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(buf)) len = (int)sizeof(buf) - 1;
    if (len && buf[len - 1] != '\n' && (size_t)len < sizeof(buf) - 1) buf[len++] = '\n';

    printk_emit(level, buf, (size_t)len);
}

/* Kernel print log (KERN_INFO level; timestamp added by printk_emit
 * when printk_time is enabled, as Linux does) */
void plogk(const char *format, ...)
{
#if KERNEL_LOG
    if (!format || (!ignore_loglevel && 6 >= console_loglevel)) return;
    spin_lock(&plogk_lock); // Lock
    char buf[BUF_SIZE];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    if (n && buf[n - 1] != '\n' && (size_t)n < sizeof(buf) - 1) buf[n++] = '\n';
    printk_emit(6, buf, (size_t)n);
    spin_unlock(&plogk_lock); // Unlock
#else
    (void)format;
#endif
}

/* Handler of unsafe buf writing */
uint8_t unsafe_buf_write(writer *writer, char c)
{
    unsafe_buf_data *data = (unsafe_buf_data *)writer->data;
    data->buf[data->idx]  = c;
    ++data->idx;
    return 1; // Always success? :)
}

/* Handler of safe buf writing with size limit */
uint8_t unsafe_buf_write_safe(writer *writer, char c)
{
    unsafe_buf_data *data = (unsafe_buf_data *)writer->data;
    if (data->size == 0 || data->idx < data->size - 1) {
        data->buf[data->idx] = c;
        ++data->idx;
        return 1;
    }
    return 0; // Buffer full
}

/* Store the formatted output in a character array */
int sprintf(char *str, const char *fmt, ...)
{
    int             c                 = 0;
    unsafe_buf_data unsafe_buf_data   = {.buf = str, .idx = 0};
    writer          unsafe_buf_writer = {
                 .data    = &unsafe_buf_data,
                 .handler = unsafe_buf_write,
    };
    va_list arg;
    va_start(arg, fmt);

    c = (int)vwprintf(&unsafe_buf_writer, fmt, arg);
    unsafe_buf_writer.handler(&unsafe_buf_writer, '\0');

    va_end(arg);
    return c;
}

/* Store the formatted output in a character array with size limit */
int snprintf(char *str, size_t size, const char *fmt, ...)
{
    int             c                 = 0;
    unsafe_buf_data unsafe_buf_data   = {.buf = str, .idx = 0, .size = size};
    writer          unsafe_buf_writer = {
                 .data    = &unsafe_buf_data,
                 .handler = unsafe_buf_write_safe,
    };
    va_list arg;
    va_start(arg, fmt);

    c = (int)vwprintf(&unsafe_buf_writer, fmt, arg);
    if (size > 0) {
        size_t idx = unsafe_buf_data.idx < size ? unsafe_buf_data.idx : size - 1;
        str[idx]   = '\0';
    }

    va_end(arg);
    return c;
}

/* Format with va_list, then store the formatted output in a character array */
int vsprintf(char *str, const char *fmt, va_list args)
{
    int             c                 = 0;
    unsafe_buf_data unsafe_buf_data   = {.buf = str, .idx = 0};
    writer          unsafe_buf_writer = {
                 .data    = &unsafe_buf_data,
                 .handler = unsafe_buf_write,
    };
    c = (int)vwprintf(&unsafe_buf_writer, fmt, args);
    unsafe_buf_writer.handler(&unsafe_buf_writer, '\0');
    return c;
}

/* Format with va_list, then store the formatted output in a character array with size limit */
int vsnprintf(char *str, size_t size, const char *fmt, va_list args)
{
    int             c                 = 0;
    unsafe_buf_data unsafe_buf_data   = {.buf = str, .idx = 0, .size = size};
    writer          unsafe_buf_writer = {
                 .data    = &unsafe_buf_data,
                 .handler = unsafe_buf_write_safe,
    };
    c = (int)vwprintf(&unsafe_buf_writer, fmt, args);
    if (size > 0) {
        size_t idx = unsafe_buf_data.idx < size ? unsafe_buf_data.idx : size - 1;
        str[idx]   = '\0';
    }
    return c;
}

typedef enum num_size {
    HALF_2 = 0, // char
    HALF_1 = 1, // short
    INT    = 2, // int
    LONG_1 = 3, // long
    LONG_2 = 4, // long long
    SIZE_T = 5, // size_t
} num_size_t;

/* Formatted output processing */
void wfmt_arg(writer *writer, args_fmter *fmter, va_list args)
{
    char         *str           = 0; // for `%s`
    size_t        write_counter = 0;
    size_t        str_len       = 0; // for align `%s`
    const char  **fmt_ptr       = fmter->fmt_ptr;
    write_handler write         = writer->handler;

    num_formatter_t num_fmter = {};
    num_fmt_type    num_flag  = {};
    int8_t          size_cnt  = INT;

    /* Error args */
    if (!writer || !write || !fmt_ptr || !(*fmt_ptr) || **fmt_ptr != '%') return;

    while (1) {
        ++(*fmt_ptr); // Skip '%' or any flags
        switch (**fmt_ptr) {
            case '-' :
                num_flag.left = 1;
                break;
            case '+' :
                num_flag.plus = 1;
                break;
            case ' ' :
                num_flag.space = 1;
                break;
            case '#' :
                num_flag.special = 1;
                break;
            case '0' :
                num_flag.zeropad = 1;
                break;
            default :
                break;
        }

        /* Calc num_fmter.size */
        if (IS_DIGIT(**fmt_ptr)) {
            num_fmter.size = skip_atoi(fmt_ptr);
        } else if (**fmt_ptr == '*') {
            /* by the following argument */
            ++(*fmt_ptr); // Skip '*'
            num_fmter.size = (size_t)va_arg(args, int);
        }

        /* Calc num_fmter.precision */
        if (**fmt_ptr == '.') {
            ++(*fmt_ptr); // Skip '.'
            if (IS_DIGIT(**fmt_ptr)) {
                num_fmter.precision = skip_atoi(fmt_ptr);
            } else if (**fmt_ptr == '*') {
                /* by the following argument */
                ++(*fmt_ptr); // Skip '*'
                num_fmter.precision = (size_t)va_arg(args, int);
            }
        }

        /* Calc size_cnt */
        switch (**fmt_ptr) {
            case 'h' :
                size_cnt--;
                if (size_cnt < HALF_2) size_cnt = HALF_2; // hh
                continue;
            case 'L' :      // += 2
                size_cnt++; // fallthrough
            case 'l' :
                size_cnt++;
                if (size_cnt > LONG_2) size_cnt = LONG_2; // ll
                continue;
            case 'z' :
                size_cnt = SIZE_T; // z
                continue;
            default :
                break;
        }

        /* Read argument */
        switch (**fmt_ptr) {
            case 'c' :
                num_fmter.num = va_arg(args, int);
                break;
            case 's' :
                str                    = va_arg(args, char *);
                static char null_str[] = "(null)";
                if (str == 0) str = null_str;
                break;
            case 'd' :
            case 'i' :
                switch (size_cnt) {
                    case HALF_2 :
                        num_fmter.num = (size_t)(char)va_arg(args, int);
                        break;
                    case HALF_1 :
                        num_fmter.num = (size_t)(short)va_arg(args, int);
                        break;
                    case INT :
                        num_fmter.num = (size_t)(int)va_arg(args, int);
                        break;
                    case LONG_1 :
                        num_fmter.num = (size_t)(long)va_arg(args, long);
                        break;
                    case LONG_2 :
                        num_fmter.num = (size_t)(long long)va_arg(args, long long);
                        break;
                    case SIZE_T : // fallthrough
                    default :
                        num_fmter.num = va_arg(args, size_t);
                        break;
                }
                break;
            case 'o' :
            case 'x' :
            case 'X' :
            case 'b' :
            case 'u' :
                switch (size_cnt) {
                    case HALF_2 :
                        num_fmter.num = (size_t)(unsigned char)va_arg(args, int);
                        break;
                    case HALF_1 :
                        num_fmter.num = (size_t)(unsigned short)va_arg(args, int);
                        break;
                    case INT :
                        num_fmter.num = (size_t)(unsigned int)va_arg(args, int);
                        break;
                    case LONG_1 :
                        num_fmter.num = (size_t)(unsigned long)va_arg(args, long);
                        break;
                    case LONG_2 :
                        num_fmter.num = (size_t)(unsigned long long)va_arg(args, long long);
                        break;
                    case SIZE_T : // fallthrough
                    default :
                        num_fmter.num = va_arg(args, size_t);
                        break;
                }
                break;
            case 'p' :
                num_fmter.num = (size_t)va_arg(args, void *);
                break;
            default : // may no data
                break;
        }

        /* Calc length of `%s` and set num_flag */
        switch (**fmt_ptr) {
            case 'c' :
                /* The alignment loops subtract one for the character itself. */
                if (num_fmter.size == 0) num_fmter.size = 1;
                break;
            case 's' :
                str_len = strlen(str);
                if (num_fmter.size < str_len) num_fmter.size = str_len;
                break;
            case 'o' :
                num_fmter.base = 8;
                break;
            case 'p' :
                num_flag.small   = 1;
                num_flag.special = 1;
                num_flag.zeropad = 1;
                if (num_fmter.size < 16) num_fmter.size = 16;
                num_fmter.base = 16;
                break;
            case 'x' :
                num_flag.small = 1; // fallthrough
            case 'X' :
                num_fmter.base = 16;
                break;
            case 'd' :
            case 'i' :
                num_flag.sign = 1; // fallthrough
            case 'u' :
                num_fmter.base = 10;
                break;
            case 'b' :
                num_fmter.base = 2;
                break;
            case 'n' :
                *(fmter->write_counter) += write_counter;
                *(int *)va_arg(args, void *) = (int)*fmter->write_counter;
                break;
            case '%' :
                break;
            default :
                /* Unexpected */
                return;
        }

        /* Write to arg space */
        switch (**fmt_ptr) {
            case 'c' :
                /* Right align */
                if (!(num_flag.left)) {
                    while (write_counter < num_fmter.size - 1) {
                        write(writer, ' ');
                        ++write_counter;
                    }
                }

                /* Write char */
                write(writer, (char)num_fmter.num);

                /* Left align */
                if (num_flag.left) {
                    while (write_counter < num_fmter.size - 1) {
                        write(writer, ' ');
                        ++write_counter;
                    }
                }
                break;
            case 's' :

                /* Right align */
                if (!(num_flag.left)) {
                    while (write_counter < num_fmter.size - str_len) {
                        write(writer, ' ');
                        ++write_counter;
                    }
                    str_len = num_fmter.size;
                }

                /* Write string */
                while (write_counter < str_len) {
                    write(writer, *str);
                    ++str;
                    ++write_counter;
                }

                /* Left align */
                if (num_flag.left) {
                    while (write_counter < num_fmter.size - str_len) {
                        write(writer, ' ');
                        ++write_counter;
                    }
                }
                break;
            case 'o' : // fallthrough
            case 'p' : // fallthrough
            case 'x' : // fallthrough
            case 'X' : // fallthrough
            case 'd' : // fallthrough
            case 'i' : // fallthrough
            case 'u' : // fallthrough
            case 'b' :
                write_counter += wnumber(writer, num_fmter, num_flag);
                break; // Format number with `writer`
            case '%' :
                write(writer, '%');
                break;
            default :
                break;
        }
        break;
    }
    *(fmter->write_counter) += write_counter;
    /* Unnecessary to update `fmt_ptr` */
}

/* Use a `writer` to write formatted string */
size_t vwprintf(writer *writer, const char *fmt, va_list args)
{
    const char   *fmt_ptr = fmt;
    size_t        result  = 0;
    write_handler write   = writer->handler;

    args_fmter fmter = {
        .fmt_ptr       = &fmt_ptr,
        .write_counter = &result,
    };

    while (*fmt_ptr != '\0') {
        if (*fmt_ptr != '%') {
            if (!write(writer, *fmt_ptr)) {
                /* Write failed, return current result */
                return result;
            }
            fmt_ptr++;
            result++;
            continue;
        }

        /* *fmt_ptr == '%' */
        wfmt_arg(writer, &fmter, args);
        fmt_ptr++;
    }
    return result;
}
