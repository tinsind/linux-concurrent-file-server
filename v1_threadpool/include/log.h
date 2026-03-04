#ifndef __LOG_H__
#define __LOG_H__

#include <netinet/in.h>


void log_client_event(const char *event, const struct sockaddr_in *addr);
void log_info(const char *format, ...);
void log_error(const char *format, ...);


#endif // __LOG_H__
