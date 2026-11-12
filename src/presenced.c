/* omarchy-presenced: says on the system bus whether someone is at the laptop.
 *
 * It reads a HID human-presence sensor through the kernel's IIO buffer,
 * filters out the sensor's short dropouts, follows the camera's privacy
 * switch, and publishes the result as org.omarchy.Presence1. The buffer can
 * only be enabled by root, which is the one reason this is a system service.
 * It reads the sensor and one camera control; it never reads a picture.
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>

#include "filter.h"
#include "privacy.h"
#include "sensor.h"

#define BUS_NAME "org.omarchy.Presence1"
#define BUS_PATH "/org/omarchy/Presence1"
#define BUS_INTERFACE "org.omarchy.Presence1"

#define IIO_ROOT "/sys/bus/iio/devices"
#define RETRY_MISSING_USEC (10 * USEC_PER_SEC)
#define RETRY_FAILED_USEC (2 * USEC_PER_SEC)
/* Distance is published in centimetre steps; the sensor's millimetres jitter. */
#define DISTANCE_STEP_MM 10
/* How often the privacy switch is read while the sensor sees nobody. */
#define PRIVACY_CHECK_USEC (2 * USEC_PER_SEC)

struct published {
  bool available;
  enum presence presence;
  bool attentive;
  uint32_t distance_mm;
  uint64_t since;
  enum privacy privacy;
};

struct daemon {
  sd_event *event;
  sd_bus *bus;
  struct sensor sensor;
  bool sensor_open;
  struct filter filter;
  int privacy_fd;
  enum privacy privacy;
  struct published published;
  sd_event_source *sensor_source;
  sd_event_source *privacy_source;
  sd_event_source *filter_timer;
  sd_event_source *retry_timer;
  sd_event_source *privacy_timer;
  bool privacy_checking;
};

static uint64_t clock_usec(clockid_t clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return (uint64_t)ts.tv_sec * USEC_PER_SEC + (uint64_t)ts.tv_nsec / 1000;
}

static const char *presence_name(enum presence presence) {
  switch (presence) {
  case PRESENCE_PRESENT: return "present";
  case PRESENCE_AWAY: return "away";
  default: return "unknown";
  }
}

static const char *privacy_name(enum privacy privacy) {
  switch (privacy) {
  case PRIVACY_ON: return "on";
  case PRIVACY_OFF: return "off";
  default: return "unknown";
  }
}

/* ---- publishing ------------------------------------------------------------ */

static struct published current_state(const struct daemon *d) {
  struct published p = { .available = d->sensor_open, .presence = PRESENCE_UNKNOWN, .privacy = d->privacy };

  /* The sensor looks through the camera, so with the privacy switch on it
   * sees an empty seat whoever is sitting there. That is "unknown", not
   * "away": nothing may lock the session or wake the screen on its word. */
  if (!d->sensor_open || d->privacy == PRIVACY_ON) return p;

  p.presence = d->filter.presence;
  p.attentive = d->filter.attentive;
  p.distance_mm = filter_distance(&d->filter) / DISTANCE_STEP_MM * DISTANCE_STEP_MM;
  p.since = d->filter.presence == PRESENCE_UNKNOWN ? 0 : d->filter.since_wall;
  return p;
}

static void publish(struct daemon *d) {
  struct published now = current_state(d), was = d->published;
  char *changed[7];
  int n = 0;

  if (now.available != was.available) changed[n++] = "Available";
  if (now.presence != was.presence) changed[n++] = "State";
  if (now.attentive != was.attentive) changed[n++] = "Attentive";
  if (now.distance_mm != was.distance_mm) changed[n++] = "DistanceMm";
  if (now.since != was.since) changed[n++] = "Since";
  if (now.privacy != was.privacy) changed[n++] = "CameraPrivacy";
  if (n == 0) return;

  changed[n] = NULL;
  d->published = now;
  sd_bus_emit_properties_changed_strv(d->bus, BUS_PATH, BUS_INTERFACE, changed);
}

static int get_property(sd_bus *bus, const char *path, const char *interface, const char *property,
                        sd_bus_message *reply, void *userdata, sd_bus_error *error) {
  const struct published *p = &((struct daemon *)userdata)->published;
  (void)bus, (void)path, (void)interface, (void)error;

  if (strcmp(property, "Available") == 0) return sd_bus_message_append(reply, "b", p->available);
  if (strcmp(property, "State") == 0) return sd_bus_message_append(reply, "s", presence_name(p->presence));
  if (strcmp(property, "Attentive") == 0) return sd_bus_message_append(reply, "b", p->attentive);
  if (strcmp(property, "DistanceMm") == 0) return sd_bus_message_append(reply, "u", p->distance_mm);
  if (strcmp(property, "Since") == 0) return sd_bus_message_append(reply, "t", p->since);
  if (strcmp(property, "CameraPrivacy") == 0) return sd_bus_message_append(reply, "s", privacy_name(p->privacy));
  return -ENOENT;
}

static const sd_bus_vtable PRESENCE_VTABLE[] = {
  SD_BUS_VTABLE_START(0),
  /* A working sensor is being read. */
  SD_BUS_PROPERTY("Available", "b", get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
  /* "present", "away", or "unknown" while there is nothing to go on. */
  SD_BUS_PROPERTY("State", "s", get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
  /* Someone is there and facing the screen. */
  SD_BUS_PROPERTY("Attentive", "b", get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
  /* How far away they are, 0 when not known. */
  SD_BUS_PROPERTY("DistanceMm", "u", get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
  /* When they arrived or left, microseconds since the epoch, 0 when unknown. */
  SD_BUS_PROPERTY("Since", "t", get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
  /* The camera's privacy switch: "on", "off", or "unknown". */
  SD_BUS_PROPERTY("CameraPrivacy", "s", get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
  SD_BUS_VTABLE_END,
};

/* ---- sensor ------------------------------------------------------------------ */

static void sensor_start(struct daemon *d);

static void arm(sd_event_source *timer, uint64_t usec_from_now) {
  sd_event_source_set_time_relative(timer, usec_from_now);
  sd_event_source_set_enabled(timer, SD_EVENT_ONESHOT);
}

static void arm_filter_timer(struct daemon *d) {
  uint64_t wait = d->sensor_open ? filter_deadline(&d->filter, clock_usec(CLOCK_MONOTONIC)) : NO_DEADLINE;

  if (wait == NO_DEADLINE)
    sd_event_source_set_enabled(d->filter_timer, SD_EVENT_OFF);
  else
    arm(d->filter_timer, wait);
}

static void set_privacy(struct daemon *d, enum privacy privacy);

/* The camera is asked to announce a change of its privacy switch, but many
 * cameras never do (the PX13's does not). The switch can still be read, and
 * there is only one time it is worth reading: while the sensor sees nobody,
 * because a blinded sensor always sees nobody. So it is read the moment the
 * seat looks empty and every two seconds for as long as it does, and not
 * while someone is being seen with the switch off. */
static void check_privacy_while_unseen(struct daemon *d) {
  /* And for as long as the switch is believed on, or the sensor seeing
   * someone again before the next read would leave that belief standing. */
  bool unseen = d->sensor_open && d->privacy_fd >= 0 &&
                (d->privacy == PRIVACY_ON || !(d->filter.have_raw && d->filter.raw_present));

  if (unseen && !d->privacy_checking) {
    d->privacy_checking = true;
    arm(d->privacy_timer, 0);
  } else if (!unseen && d->privacy_checking) {
    d->privacy_checking = false;
    sd_event_source_set_enabled(d->privacy_timer, SD_EVENT_OFF);
  }
}

static int on_privacy_timer(sd_event_source *source, uint64_t usec, void *userdata) {
  struct daemon *d = userdata;
  (void)source, (void)usec;

  set_privacy(d, privacy_read(d->privacy_fd));
  publish(d);
  arm_filter_timer(d);
  if (d->privacy_checking) arm(d->privacy_timer, PRIVACY_CHECK_USEC);
  return 0;
}

static void sensor_stop(struct daemon *d) {
  d->sensor_source = sd_event_source_disable_unref(d->sensor_source);
  d->privacy_source = sd_event_source_disable_unref(d->privacy_source);
  if (d->privacy_fd >= 0) close(d->privacy_fd);
  d->privacy_fd = -1;
  d->privacy = PRIVACY_UNKNOWN;
  if (d->sensor_open) sensor_close(&d->sensor);
  d->sensor_open = false;
  sd_event_source_set_enabled(d->filter_timer, SD_EVENT_OFF);
  check_privacy_while_unseen(d);
}

/* The last reading must not outlive the sensor: "someone is here" left
 * standing would stop the session ever locking. */
static void sensor_failed(struct daemon *d, int error, uint64_t retry_usec) {
  if (error) fprintf(stderr, "sensor error: %s\n", strerror(-error));
  sensor_stop(d);
  publish(d);
  arm(d->retry_timer, retry_usec);
}

static int on_sensor(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
  struct daemon *d = userdata;
  struct report reports[64];
  (void)source, (void)fd;

  if (revents & (EPOLLERR | EPOLLHUP)) {
    sensor_failed(d, -EIO, RETRY_FAILED_USEC);
    return 0;
  }

  int count = sensor_read(&d->sensor, reports, 64);
  if (count < 0) {
    sensor_failed(d, count, RETRY_FAILED_USEC);
    return 0;
  }

  uint64_t now = clock_usec(CLOCK_MONOTONIC), wall = clock_usec(CLOCK_REALTIME);
  for (int i = 0; i < count; i++)
    filter_feed(&d->filter, reports[i].present, reports[i].attentive, reports[i].distance_mm, now, wall);
  check_privacy_while_unseen(d);
  publish(d);
  arm_filter_timer(d);
  return 0;
}

static int on_filter_timer(sd_event_source *source, uint64_t usec, void *userdata) {
  struct daemon *d = userdata;
  (void)source, (void)usec;

  filter_tick(&d->filter, clock_usec(CLOCK_MONOTONIC), clock_usec(CLOCK_REALTIME));
  publish(d);
  arm_filter_timer(d);
  return 0;
}

static void set_privacy(struct daemon *d, enum privacy privacy) {
  if (privacy == PRIVACY_UNKNOWN || privacy == d->privacy) return;
  /* What the blinded sensor said while the switch was on is worthless:
   * decide afresh once it can see again. */
  if (d->privacy == PRIVACY_ON && privacy == PRIVACY_OFF) filter_init(&d->filter, NULL);
  d->privacy = privacy;
}

static int on_privacy(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
  struct daemon *d = userdata;
  (void)source;

  if (revents & (EPOLLERR | EPOLLHUP)) {
    /* The camera went away; the sensor on the same device follows. */
    sensor_failed(d, -ENODEV, RETRY_FAILED_USEC);
    return 0;
  }
  set_privacy(d, privacy_drain(fd));
  publish(d);
  arm_filter_timer(d);
  return 0;
}

static void sensor_start(struct daemon *d) {
  char device[PATH_MAX];
  int r;

  sensor_stop(d);
  if (sensor_find(IIO_ROOT, device, sizeof device) < 0) {
    publish(d);
    arm(d->retry_timer, RETRY_MISSING_USEC);
    return;
  }
  if ((r = sensor_open(&d->sensor, device)) < 0) {
    sensor_failed(d, r, RETRY_FAILED_USEC);
    return;
  }
  d->sensor_open = true;
  filter_init(&d->filter, NULL);

  r = sd_event_add_io(d->event, &d->sensor_source, d->sensor.fd, EPOLLIN, on_sensor, d);
  if (r < 0) {
    sensor_failed(d, r, RETRY_FAILED_USEC);
    return;
  }

  /* A camera without a privacy control is fine: the switch stays "unknown". */
  d->privacy_fd = privacy_open(device);
  if (d->privacy_fd >= 0) {
    d->privacy = privacy_read(d->privacy_fd);
    if (sd_event_add_io(d->event, &d->privacy_source, d->privacy_fd, EPOLLPRI, on_privacy, d) < 0) {
      close(d->privacy_fd);
      d->privacy_fd = -1;
      d->privacy = PRIVACY_UNKNOWN;
    }
  }

  fprintf(stderr, "watching %s\n", device);
  check_privacy_while_unseen(d);
  publish(d);
}

static int on_retry_timer(sd_event_source *source, uint64_t usec, void *userdata) {
  (void)source, (void)usec;
  sensor_start(userdata);
  return 0;
}

/* Whoever was here before a sleep says nothing about now, and the sensor may
 * have lost power meanwhile: start over on resume. */
static int on_prepare_for_sleep(sd_bus_message *message, void *userdata, sd_bus_error *error) {
  int going_to_sleep = 0;
  (void)error;

  if (sd_bus_message_read(message, "b", &going_to_sleep) >= 0 && !going_to_sleep) {
    fprintf(stderr, "resumed from sleep, restarting the sensor\n");
    sensor_start(userdata);
  }
  return 0;
}

static int on_signal(sd_event_source *source, const struct signalfd_siginfo *info, void *userdata) {
  (void)info, (void)userdata;
  return sd_event_exit(sd_event_source_get_event(source), 0);
}

int main(void) {
  struct daemon d = { .privacy_fd = -1 };
  sigset_t signals;
  int r;

  setvbuf(stderr, NULL, _IOLBF, 0);
  d.sensor.fd = -1;

  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  sigprocmask(SIG_BLOCK, &signals, NULL);

  if ((r = sd_event_default(&d.event)) < 0) goto fail;
  if ((r = sd_event_add_signal(d.event, NULL, SIGTERM, on_signal, NULL)) < 0) goto fail;
  if ((r = sd_event_add_signal(d.event, NULL, SIGINT, on_signal, NULL)) < 0) goto fail;
  if ((r = sd_event_add_time_relative(d.event, &d.filter_timer, CLOCK_MONOTONIC, 0, 0, on_filter_timer, &d)) < 0) goto fail;
  if ((r = sd_event_add_time_relative(d.event, &d.retry_timer, CLOCK_MONOTONIC, 0, 0, on_retry_timer, &d)) < 0) goto fail;
  if ((r = sd_event_add_time_relative(d.event, &d.privacy_timer, CLOCK_MONOTONIC, 0, 0, on_privacy_timer, &d)) < 0) goto fail;
  sd_event_source_set_enabled(d.filter_timer, SD_EVENT_OFF);
  sd_event_source_set_enabled(d.retry_timer, SD_EVENT_OFF);
  sd_event_source_set_enabled(d.privacy_timer, SD_EVENT_OFF);

  if ((r = sd_bus_open_system(&d.bus)) < 0) goto fail;
  if ((r = sd_bus_add_object_vtable(d.bus, NULL, BUS_PATH, BUS_INTERFACE, PRESENCE_VTABLE, &d)) < 0) goto fail;
  if ((r = sd_bus_match_signal(d.bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "PrepareForSleep", on_prepare_for_sleep, &d)) < 0)
    goto fail;
  if ((r = sd_bus_request_name(d.bus, BUS_NAME, 0)) < 0) goto fail;
  if ((r = sd_bus_attach_event(d.bus, d.event, SD_EVENT_PRIORITY_NORMAL)) < 0) goto fail;

  sensor_start(&d);
  sd_notify(0, "READY=1");
  r = sd_event_loop(d.event);

  sensor_stop(&d);
  sd_bus_flush_close_unref(d.bus);
  sd_event_unref(d.event);
  return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

fail:
  fprintf(stderr, "omarchy-presenced: %s\n", strerror(-r));
  return EXIT_FAILURE;
}
