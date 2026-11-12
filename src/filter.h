/* Turns raw human-presence sensor reports into a steady state.
 *
 * Pure logic with the clock passed in, so it is tested without a sensor.
 * Times are microseconds; `now` is monotonic, `wall` is the real-time clock.
 */
#ifndef PRESENCE_FILTER_H
#define PRESENCE_FILTER_H

#include <stdbool.h>
#include <stdint.h>

#define USEC_PER_SEC 1000000ULL
#define NO_DEADLINE UINT64_MAX

enum presence { PRESENCE_UNKNOWN, PRESENCE_PRESENT, PRESENCE_AWAY };

struct filter_config {
  uint64_t arrive_usec;         /* presence must hold this long to count as arrived */
  uint64_t leave_usec;          /* absence must hold this long to count as gone */
  uint64_t attention_hold_usec; /* attention is held across dropouts this long */
};

struct filter {
  struct filter_config config;
  enum presence presence;
  bool attentive;
  uint32_t distance_mm; /* last distance seen while someone was there, 0 if none */
  uint64_t since_wall;  /* when they arrived or left, 0 while unknown */

  bool have_raw;
  bool raw_present;
  uint64_t raw_present_at;
  bool raw_attentive;
  bool have_inattentive_at;
  uint64_t inattentive_at;
};

extern const struct filter_config FILTER_DEFAULTS;

void filter_init(struct filter *f, const struct filter_config *config);

/* Take one sensor report. Returns true when the published state changed. */
bool filter_feed(struct filter *f, bool present, bool attentive, uint32_t distance_mm,
                 uint64_t now, uint64_t wall);

/* Apply the hold times. Returns true when the published state changed. */
bool filter_tick(struct filter *f, uint64_t now, uint64_t wall);

/* Microseconds until filter_tick() could next change something, or NO_DEADLINE. */
uint64_t filter_deadline(const struct filter *f, uint64_t now);

/* The distance to publish: only meaningful while someone is there. */
uint32_t filter_distance(const struct filter *f);

#endif
