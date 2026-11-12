/* Tests for the filter and the IIO record decoding. Run: make check */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../src/filter.h"
#include "../src/sensor.h"

#define S(seconds) ((uint64_t)((seconds) * 1000000.0))
#define WALL 1000

struct event { double at; bool present, attentive; uint32_t distance; };

/* Feed events and tick every 0.1 s up to `until`, as the daemon's timer would. */
static void run(struct filter *f, const struct event *events, int count, double from, double until) {
  int next = 0;
  while (next < count && events[next].at < from - 1e-9) next++;
  for (int step = (int)(from * 10 + 0.5); step <= (int)(until * 10 + 0.5); step++) {
    double now = step / 10.0;
    while (next < count && events[next].at <= now + 1e-9) {
      filter_feed(f, events[next].present, events[next].attentive, events[next].distance, S(now), S(WALL + now));
      next++;
    }
    filter_tick(f, S(now), S(WALL + now));
  }
}

static struct filter fresh(void) {
  struct filter f;
  filter_init(&f, NULL);
  return f;
}

static void unknown_until_the_sensor_has_decided(void) {
  struct filter f = fresh();
  struct event e[] = { { 0, false, false, 0 } };
  assert(f.presence == PRESENCE_UNKNOWN);
  run(&f, e, 1, 0, 7.9);
  assert(f.presence == PRESENCE_UNKNOWN);
  run(&f, e, 1, 8.0, 8.0);
  assert(f.presence == PRESENCE_AWAY);
}

static void arrival_needs_a_second_of_presence(void) {
  struct filter f = fresh();
  struct event e[] = { { 0, true, true, 400 } };
  run(&f, e, 1, 0, 0.9);
  assert(f.presence == PRESENCE_UNKNOWN && !f.attentive);
  run(&f, e, 1, 1.0, 1.0);
  assert(f.presence == PRESENCE_PRESENT && f.attentive && filter_distance(&f) == 400);
}

static void camera_dropouts_are_ignored(void) {
  /* Seen on the ASUS PX13: an infrared scan blanks the sensor for about 3 s,
   * the colour camera starting for about 7 s. */
  struct filter f = fresh();
  struct event e[] = { { 0, true, true, 400 }, { 12, false, false, 0 }, { 19, true, true, 424 } };
  run(&f, e, 3, 0, 1.0);
  for (double t = 1.1; t <= 25; t += 0.1) {
    run(&f, e, 3, t, t);
    assert(f.presence == PRESENCE_PRESENT);
  }
}

static void leaving_takes_eight_seconds_and_is_backdated(void) {
  struct filter f = fresh();
  struct event e[] = { { 0, true, true, 400 }, { 10, false, false, 0 } };
  run(&f, e, 2, 0, 17.9);
  assert(f.presence == PRESENCE_PRESENT);
  run(&f, e, 2, 18.0, 18.0);
  assert(f.presence == PRESENCE_AWAY && !f.attentive);
  /* Counted from when the seat emptied, not from when that was believed. */
  assert(f.since_wall == S(WALL + 10));
  assert(filter_distance(&f) == 0);
}

static void a_brief_phantom_is_not_an_arrival(void) {
  struct filter f = fresh();
  struct event e[] = { { 0, false, false, 0 }, { 9, true, false, 857 }, { 9.6, false, false, 0 } };
  run(&f, e, 3, 0, 8.0);
  for (double t = 8.1; t <= 15; t += 0.1) {
    run(&f, e, 3, t, t);
    assert(f.presence == PRESENCE_AWAY && filter_distance(&f) == 0);
  }
}

static void attention_is_held_then_dropped_and_returns_at_once(void) {
  struct filter f = fresh();
  struct event e[] = { { 0, true, true, 400 }, { 3, true, false, 400 }, { 12, true, true, 400 } };
  run(&f, e, 3, 0, 7.9);
  assert(f.attentive);
  run(&f, e, 3, 8.0, 8.0);
  assert(f.presence == PRESENCE_PRESENT && !f.attentive);
  run(&f, e, 3, 8.1, 12.0);
  assert(f.attentive);
}

static void deadline_tracks_the_next_change(void) {
  struct filter f = fresh();
  assert(filter_deadline(&f, 0) == NO_DEADLINE);
  filter_feed(&f, true, true, 400, S(0), S(WALL));
  assert(filter_deadline(&f, S(0.25)) == S(0.75));
  filter_tick(&f, S(1), S(WALL + 1));
  assert(filter_deadline(&f, S(1)) == NO_DEADLINE);
  filter_feed(&f, false, false, 0, S(2), S(WALL + 2));
  /* Attention runs out first (5 s), then presence (8 s). */
  assert(filter_deadline(&f, S(3)) == S(4));
  assert(filter_deadline(&f, S(20)) == 0);
}

static void px13_record_layout(void) {
  struct channel c[CH_COUNT];
  memset(c, 0, sizeof c);
  const char *types[CH_COUNT] = { "le:s8/32>>0", "le:s16/32>>0", "le:s8/32>>0" };
  for (int i = 0; i < CH_COUNT; i++) {
    assert(channel_parse(&c[i], types[i]) == 0);
    c[i].enabled = true;
    c[i].index = i;
  }
  assert(channels_layout(c, CH_COUNT) == 12);
  uint8_t record[12] = { 1, 0, 0, 0, 0x9c, 0x01, 0, 0, 100, 0, 0, 0 };
  assert(channel_decode(&c[CH_PRESENCE], record) == 1);
  assert(channel_decode(&c[CH_DISTANCE], record) == 412);
  assert(channel_decode(&c[CH_ATTENTION], record) == 100);
}

static void mixed_sizes_are_aligned_and_signed(void) {
  struct channel c[2];
  memset(c, 0, sizeof c);
  assert(channel_parse(&c[0], "le:u8/8>>0") == 0);
  assert(channel_parse(&c[1], "be:s16/16>>0") == 0);
  c[0].enabled = c[1].enabled = true;
  c[1].index = 1;
  assert(channels_layout(c, 2) == 4);
  assert(c[1].offset == 2);
  uint8_t record[4] = { 7, 0, 0xFF, 0xFE };
  assert(channel_decode(&c[0], record) == 7);
  assert(channel_decode(&c[1], record) == -2);
}

static void a_disabled_channel_takes_no_room(void) {
  struct channel c[CH_COUNT];
  memset(c, 0, sizeof c);
  assert(channel_parse(&c[CH_PRESENCE], "le:s8/32>>0") == 0);
  c[CH_PRESENCE].enabled = true;
  assert(channels_layout(c, CH_COUNT) == 4);
}

static void bad_scan_types_are_refused(void) {
  struct channel c;
  const char *bad[] = { "weird", "le:x8/32>>0", "me:s8/32>>0", "le:s8/0>>0", "le:s40/32>>0", "le:s8/12>>0", "le:s8/128>>0" };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) assert(channel_parse(&c, bad[i]) == -EINVAL);
}

int main(void) {
  unknown_until_the_sensor_has_decided();
  arrival_needs_a_second_of_presence();
  camera_dropouts_are_ignored();
  leaving_takes_eight_seconds_and_is_backdated();
  a_brief_phantom_is_not_an_arrival();
  attention_is_held_then_dropped_and_returns_at_once();
  deadline_tracks_the_next_change();
  px13_record_layout();
  mixed_sizes_are_aligned_and_signed();
  a_disabled_channel_takes_no_room();
  bad_scan_types_are_refused();
  puts("all checks passed");
  return 0;
}
