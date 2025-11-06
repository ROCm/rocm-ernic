#include <stdio.h>
#include <stdarg.h>

void error_vreport(const char *fmt, va_list ap)
{
    fprintf(stderr, "ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void warn_vreport(const char *fmt, va_list ap)
{
    fprintf(stderr, "WARN: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void info_vreport(const char *fmt, va_list ap)
{
    printf("INFO: ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
}

void error_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    error_vreport(fmt, ap);
    va_end(ap);
}

void warn_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    warn_vreport(fmt, ap);
    va_end(ap);
}

void info_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    info_vreport(fmt, ap);
    va_end(ap);
}
