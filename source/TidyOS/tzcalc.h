// Local time in another time zone from a POSIX TZ string ("EST5EDT,M3.2.0,M11.1.0"),
// without touching the TZ environment variable. (newlib's setenv() never frees the old
// value, so switching TZ back and forth every second slowly ate all the free memory.)
#pragma once
#include <time.h>
#include <ctype.h>
#include <stdlib.h>

// days since 1970-01-01 for a calendar date
static inline long tzDaysFromCivil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// "[+-]hh[:mm[:ss]]" -> seconds
static inline long tzParseOff(const char *& p)
{
  int sign = 1;
  if (*p == '+') ++p; else if (*p == '-') { sign = -1; ++p; }
  long h = strtol(p, (char **)&p, 10), m = 0, s = 0;
  if (*p == ':') { ++p; m = strtol(p, (char **)&p, 10); if (*p == ':') { ++p; s = strtol(p, (char **)&p, 10); } }
  return sign * (h * 3600 + m * 60 + s);
}

static inline void tzSkipName(const char *& p)
{
  if (*p == '<') { while (*p && *p != '>') ++p; if (*p) ++p; return; }
  while (isalpha((unsigned char)*p)) ++p;
}

// "Mm.w.d[/time]": when in the given year (as seconds of local time since the epoch)
static inline bool tzRule(const char *& p, int year, long & at)
{
  if (*p != 'M') return false;
  ++p;
  int m = strtol(p, (char **)&p, 10); if (*p++ != '.') return false;
  int w = strtol(p, (char **)&p, 10); if (*p++ != '.') return false;
  int d = strtol(p, (char **)&p, 10);
  long tod = 7200;
  if (*p == '/') { ++p; tod = tzParseOff(p); }
  long first = tzDaysFromCivil(year, m, 1);
  int wdFirst = (int)((first + 4) % 7);                 // 1970-01-01 was a Thursday
  if (wdFirst < 0) wdFirst += 7;
  long day = first + ((d - wdFirst + 7) % 7) + (long)(w - 1) * 7;
  static const int ML[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  int len = ML[m - 1] + (m == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0));
  while (day >= first + len) day -= 7;                  // week 5 = the last one
  at = day * 86400 + tod;
  return true;
}

// like localtime_r() with TZ set to tz
static inline void tzLocal(const char * tz, time_t now, struct tm & out)
{
  const char * p = tz;
  tzSkipName(p);
  long stdOff = -tzParseOff(p);                         // POSIX offsets are west-positive
  long dstOff = stdOff;
  bool hasDst = false;
  if (*p && *p != ',') {
    tzSkipName(p);
    hasDst = true;
    dstOff = stdOff + 3600;
    if (*p && *p != ',') dstOff = -tzParseOff(p);
  }
  long off = stdOff;
  if (hasDst && *p == ',') {
    ++p;
    time_t ls = now + stdOff;                           // standard local time
    struct tm y; gmtime_r(&ls, &y);
    long start, end;
    const char * q = p;
    if (tzRule(q, y.tm_year + 1900, start) && *q == ',' && (++q, tzRule(q, y.tm_year + 1900, end))) {
      long t = (long)ls;
      long endStd = end - (dstOff - stdOff);            // the end rule is given in daylight time
      bool inDst = start < endStd ? (t >= start && t < endStd) : !(t >= endStd && t < start);
      if (inDst) off = dstOff;
    }
  }
  time_t lt = now + off;
  gmtime_r(&lt, &out);
  out.tm_isdst = off != stdOff;
}
