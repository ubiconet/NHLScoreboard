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

namespace {
// Days since 1970-01-01 for a civil (Gregorian) date — the classic
// era-based formula; TZ/mktime are deliberately not involved so the
// result is pure UTC.
long daysFromCivil(long y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const long yoe = y - era * 400;                                  // [0, 399]
  const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
  return era * 146097 + doe - 719468;
}
}  // namespace

void applyCoarseHttpDate(const char* httpDate) {
  if (timeIsSynced()) return;  // a real clock exists — never fight NTP
  if (httpDate == nullptr) return;
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int day = 0, year = 0, hour = 0, min = 0, sec = 0;
  char monStr[4] = {};
  // RFC 1123 form: "Mon, 19 Oct 2026 15:04:05 GMT" — the leading weekday
  // and anything before the comma are skipped.
  const char* comma = strchr(httpDate, ',');
  if (comma == nullptr ||
      sscanf(comma + 1, " %d %3s %d %d:%d:%d", &day, monStr, &year, &hour,
             &min, &sec) != 6) {
    return;
  }
  const char* m = strstr(months, monStr);
  if (m == nullptr || day < 1 || day > 31 || hour > 23 || min > 59 ||
      sec > 60) {
    return;
  }
  int month = (int)(m - months) / 3 + 1;
  time_t epoch = daysFromCivil(year, month, day) * 86400L + hour * 3600L +
                 min * 60L + sec;
  if (epoch < 1704067200) return;  // reject pre-2024 garbage
  timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
}
