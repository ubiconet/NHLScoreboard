#include "common/data/time_util.h"

bool timeIsSynced() {
  return time(nullptr) >= 1704067200;  // 2024-01-01: NTP has set the clock.
}

bool parseCompileDate(const char* compileDate, tm& out) {
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int month = 0, day = 0, year = 0;
  char monStr[4] = {};
  if (sscanf(compileDate, "%3s %d %d", monStr, &day, &year) != 3) return false;
  const char* m = strstr(months, monStr);
  if (m == nullptr) return false;
  month = (m - months) / 3 + 1;
  memset(&out, 0, sizeof(out));
  out.tm_year = year - 1900;
  out.tm_mon  = month - 1;
  out.tm_mday = day;
  return true;
}

bool isoDateToEpoch(const char* isoDate, time_t& outEpoch) {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  if (isoDate == nullptr ||
      sscanf(isoDate, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5) {
    return false;
  }

  tm utcTime = {};
  utcTime.tm_year = year - 1900;
  utcTime.tm_mon = month - 1;
  utcTime.tm_mday = day;
  utcTime.tm_hour = hour;
  utcTime.tm_min = minute;
  utcTime.tm_isdst = -1;

  // mktime reads local time; compare it to the UTC fields it produced to
  // calculate the timezone offset at this specific instant, including DST.
  time_t asLocalTime = mktime(&utcTime);
  if (asLocalTime == static_cast<time_t>(-1)) return false;

  tm utcFields = {};
  gmtime_r(&asLocalTime, &utcFields);
  utcFields.tm_isdst = -1;
  time_t utcFieldsAsLocal = mktime(&utcFields);
  if (utcFieldsAsLocal == static_cast<time_t>(-1)) return false;

  outEpoch = asLocalTime + (asLocalTime - utcFieldsAsLocal);
  return true;
}
