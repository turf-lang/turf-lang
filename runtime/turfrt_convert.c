/*
 * runtime/turfrt_convert.c -- the string-to-number casts, `s to int` and
 * `s to double`.
 *
 * Codegen calls these and panics at the cast's own line when one returns 0.
 * They accept exactly what strings::parse_int and strings::parse_double
 * (lib/std/core/strings.tr) accept, so a cast and a parse never disagree
 * about what is a number: no surrounding space, no leading '+', no trailing
 * text, no hex, inf or nan, and an int must be in range. C's strtoll/strtod
 * accept all of those, which is why the text is checked here first.
 */

#include "turfrt.h"

#include <stdlib.h>

static int is_digit(char c) { return c >= '0' && c <= '9'; }

int turf_rt_str_to_int(const char *s, int64_t *out) {
  if (!s)
    return 0;
  const char *p = s;
  int neg = 0;
  if (*p == '-') {
    neg = 1;
    p++;
  }
  if (!is_digit(*p))
    return 0;
  /* Accumulated as a negative number: INT64_MIN has no positive twin. */
  int64_t r = 0;
  for (; *p; p++) {
    if (!is_digit(*p))
      return 0;
    int d = *p - '0';
    if (r < INT64_MIN / 10 || (r == INT64_MIN / 10 && d > -(INT64_MIN % 10)))
      return 0;
    r = r * 10 - d;
  }
  if (!neg) {
    if (r == INT64_MIN)
      return 0;
    r = -r;
  }
  *out = r;
  return 1;
}

int turf_rt_str_to_double(const char *s, double *out) {
  if (!s)
    return 0;
  const char *p = s;
  size_t digits = 0;
  if (*p == '-')
    p++;
  while (is_digit(*p)) {
    p++;
    digits++;
  }
  if (*p == '.') {
    p++;
    while (is_digit(*p)) {
      p++;
      digits++;
    }
  }
  if (digits == 0)
    return 0;
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-')
      p++;
    size_t exp_digits = 0;
    while (is_digit(*p)) {
      p++;
      exp_digits++;
    }
    if (exp_digits == 0)
      return 0;
  }
  if (*p)
    return 0;
  /* The text is plain decimal, which strtod rounds correctly; a magnitude
   * beyond the double range is +-inf, as IEEE says. A Turf program never
   * calls setlocale, so the decimal point is '.'. */
  *out = strtod(s, NULL);
  return 1;
}
