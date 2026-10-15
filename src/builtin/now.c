/* now - the current date/time tool (builtin-mcp).

   answers with the wall-clock time of the machine: iso 8601 to the
   second, the utc offset of the broken-down time, and the timezone
   name ("2025-01-01T12:00:00+01:00 at Europe/Rome timezone"). no
   arguments: it is always about right now.

   the offset: tm_gmtoff of localtime_r/localtime_s - posix always
   fills it, msvc since 2005. zoneinfo windows do not honor TZ, so a
   TZ like "Europe/Rome" shifts the clock but tm_gmtoff stays at zero:
   a hand-rolled days_from_civil/ll_timegm normalization recovers the
   offset in that case (the same trick platform.c uses to keep
   timegm iso).

   the name: the TZ variable's leading segment when it names a zone
   (":Europe/Rome" or "Europe/Rome"), else what it says for a fixed
   offset ("UTC", "CET"), else the /etc/localtime symlink target when
   it ends in zoneinfo/, else utc (the fallback on every other
   windows). */

#include "builtin/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
/* days since 1970-01-01 from a civil date (howard hinnant's algorithm),
   see src/platform.c: tm fields to seconds since the epoch */
static long long now_days_from_civil(long long y, long long m, long long d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;                 /* [0, 399] */
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

/* same civil time read once as utc: the utc instant the wall clock
   appears to show, used to recover an offset the libc did not set */
static time_t now_timegm(const struct tm *tm) {
    return (time_t)(now_days_from_civil((long long)tm->tm_year + 1900,
                                        tm->tm_mon + 1, tm->tm_mday) *
                        86400 +
                    tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}
#endif

/* portable localtime: localtime_r/localtime_s, the errno locale
   guard glibc needs, gmtfill when the libc does not set tm_gmtoff */
static struct tm *now_localtime(time_t t, struct tm *out, long *gmtoff) {
    struct tm *r = NULL;
#ifdef _WIN32
    errno_t e = localtime_s(out, &t);
    if (e == 0) r = out;
#else
    int save = errno;
    r = localtime_r(&t, out);
    if (!r) errno = save;
#endif
    if (!r) return NULL;
    long off = 0;
#ifdef _WIN32
    /* tm_gmtoff: msvc 2005+ and mingw-w64 fill it, zoneinfo windows
       (no /usr/share/zoneinfo) ignore TZ instead - the fallback below
       measures the real offset in that case */
    off = out->tm_gmtoff;
    if (off == 0) {
        time_t utc = now_timegm(out);
        if (utc != (time_t)-1) off = (long)(t - utc);
    }
#else
    off = out->tm_gmtoff;
#endif
    *gmtoff = off;
    return out;
}

/* the timezone name: TZ when it names a zone, else the etc/localtime
   symlink, else utc (null-terminated dst, returns dst) */
static char *now_tz_name(char *dst, size_t sz) {
    const char *tz = getenv("TZ");
    if (tz && *tz) {
        if (*tz == ':') tz++;
        /* tz database names are plain posix path segments */
        bool ok = tz[0] != '\0';
        for (const char *p = tz; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '/' || c == '_' ||
                  c == '-' || c == '+' || c == '.')) {
                ok = false;
                break;
            }
        }
        if (ok && tz[0] != '\0') {
            snprintf(dst, sz, "%s", tz);
            return dst;
        }
    }
#ifndef _WIN32
    /* the distro way: /etc/localtime symlinked into the zoneinfo tree;
       readlink keeps it a peek, not a walk */
    char lnk[BM_PATH_MAX];
    ssize_t n = readlink("/etc/localtime", lnk, sizeof lnk - 1);
    if (n > 0) {
        lnk[n] = '\0';
        const char *zi = strstr(lnk, "zoneinfo/");
        if (zi && zi[9]) {
            snprintf(dst, sz, "%s", zi + 9);
            return dst;
        }
    }
#endif
    snprintf(dst, sz, "UTC");
    return dst;
}

void tool_now(const cJSON *args, buf_t *out, bool *is_error) {
    (void)args;
    /* one broken-down time, taken at call time */
    time_t t = time(NULL);
    struct tm tm;
    long gmtoff = 0;
    if (!now_localtime(t, &tm, &gmtoff)) {
        buf_append_str(out, "now: localtime failed");
        *is_error = true;
        return;
    }
    char iso[32];
    if (strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", &tm) == 0) {
        buf_append_str(out, "now: strftime failed");
        *is_error = true;
        return;
    }
    char tz[BM_PATH_MAX];
    buf_appendf(out, "%s%+03ld:%02ld at %s timezone", iso, gmtoff / 3600,
               labs(gmtoff) / 60 % 60, now_tz_name(tz, sizeof tz));
}
