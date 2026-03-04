#include "log.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void format_now(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm_now;
    if(localtime_r(&now, &tm_now) == NULL) {
        if(len > 0) {
            buf[0] = '\0';
        }
        return;
    }

    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_now);
 }

 static void vlog_with_level(FILE *stream, const char *level, const char *fmt, va_list ap) {
    char ts[32];

    format_now(ts, sizeof(ts));
    fprintf(stream, "[%s] [%s] ", ts[0] ? ts : "unknown-time", level);
    vfprintf(stream, fmt, ap);
    fputc('\n', stream);
    fflush(stream);
 }

 void log_info(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vlog_with_level(stdout, "INFO", fmt, ap);
    va_end(ap);
 }

 void log_error(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vlog_with_level(stderr, "ERROR", fmt, ap);
    va_end(ap);
 }

 void log_client_event(const char *event, const struct sockaddr_in *addr) {
    char ip[INET_ADDRSTRLEN];
    unsigned short port = 0;

    if(event == NULL) {
        event = "(null)";
    }

    if(addr != NULL) {
        if(inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == NULL) {
            snprintf(ip, sizeof(ip), "invalid-ip");
        }
        port = ntohs(addr->sin_port);
    }

    log_info("%s: %s:%hu", event, ip, port);
 }
