#ifndef _SYS_TIME_H_WIN32_COMPAT
#define _SYS_TIME_H_WIN32_COMPAT

#include <winsock2.h>

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#ifdef __cplusplus
extern "C" {
#endif

int gettimeofday(struct timeval *tv, struct timezone *tz);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIME_H_WIN32_COMPAT */
