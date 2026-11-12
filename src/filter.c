#include "filter.h"

#include <string.h>

/* Arriving takes a second: the sensor occasionally reports a person for a
 * moment when there is none.
 *
 * Leaving takes eight: the sensor drops to "nobody" whenever the camera it
 * looks through is opened, about three seconds for an infrared face scan and
 * up to seven when a video call starts. Anything shorter is noise.
 *
 * Attention is held across the same dropouts. */
const struct filter_config FILTER_DEFAULTS = {
  .arrive_usec = 1 * USEC_PER_SEC,
  .leave_usec = 8 * USEC_PER_SEC,
  .attention_hold_usec = 5 * USEC_PER_SEC,
};

void filter_init(struct filter *f, const struct filter_config *config) {
  memset(f, 0, sizeof *f);
  f->config = config ? *config : FILTER_DEFAULTS;
  f->presence = PRESENCE_UNKNOWN;
}

bool filter_feed(struct filter *f, bool present, bool attentive, uint32_t distance_mm,
                 uint64_t now, uint64_t wall) {
  bool changed = false;

  if (!f->have_raw || present != f->raw_present) {
    f->have_raw = true;
    f->raw_present = present;
    f->raw_present_at = now;
  }

  attentive = attentive && present;
  if (attentive) {
    f->have_inattentive_at = false;
  } else if (f->raw_attentive || !f->have_inattentive_at) {
    f->have_inattentive_at = true;
    f->inattentive_at = now;
  }
  f->raw_attentive = attentive;

  if (present && distance_mm > 0 && distance_mm != f->distance_mm) {
    f->distance_mm = distance_mm;
    changed = true;
  }

  return filter_tick(f, now, wall) || changed;
}

bool filter_tick(struct filter *f, uint64_t now, uint64_t wall) {
  bool changed = false;

  if (f->have_raw) {
    uint64_t held = now - f->raw_present_at;
    /* `since` is when it happened, not when the hold time ran out, so "away
     * for thirty seconds" counts from the moment the seat emptied. */
    if (f->raw_present && f->presence != PRESENCE_PRESENT && held >= f->config.arrive_usec) {
      f->presence = PRESENCE_PRESENT;
      f->since_wall = wall > held ? wall - held : 0;
      changed = true;
    } else if (!f->raw_present && f->presence != PRESENCE_AWAY && held >= f->config.leave_usec) {
      f->presence = PRESENCE_AWAY;
      f->since_wall = wall > held ? wall - held : 0;
      f->distance_mm = 0;
      changed = true;
    }
  }

  bool attentive = f->attentive;
  if (f->presence != PRESENCE_PRESENT)
    attentive = false;
  else if (f->raw_attentive)
    attentive = true;
  else if (f->have_inattentive_at && now - f->inattentive_at >= f->config.attention_hold_usec)
    attentive = false;

  if (attentive != f->attentive) {
    f->attentive = attentive;
    changed = true;
  }
  return changed;
}

uint64_t filter_deadline(const struct filter *f, uint64_t now) {
  uint64_t wait = NO_DEADLINE;

  if (f->have_raw) {
    uint64_t held = now - f->raw_present_at;
    uint64_t need = NO_DEADLINE;
    if (f->raw_present && f->presence != PRESENCE_PRESENT)
      need = f->config.arrive_usec;
    else if (!f->raw_present && f->presence != PRESENCE_AWAY)
      need = f->config.leave_usec;
    if (need != NO_DEADLINE) wait = held >= need ? 0 : need - held;
  }

  if (f->attentive && !f->raw_attentive && f->have_inattentive_at) {
    uint64_t held = now - f->inattentive_at;
    uint64_t left = held >= f->config.attention_hold_usec ? 0 : f->config.attention_hold_usec - held;
    if (left < wait) wait = left;
  }
  return wait;
}

uint32_t filter_distance(const struct filter *f) {
  return f->presence == PRESENCE_PRESENT ? f->distance_mm : 0;
}
