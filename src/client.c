/* omarchy-presence: reads the presence service from the command line.
 *
 *   omarchy-presence status   print the current state as one line of JSON
 *   omarchy-presence watch    print it now and again on every change
 *
 * `watch` is what the desktop shell runs: one JSON object per line, starting
 * at once and continuing for as long as it is left running, including while
 * the service is stopped or not installed (it then says "available": false).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>

#define BUS_NAME "org.omarchy.Presence1"
#define BUS_PATH "/org/omarchy/Presence1"
#define BUS_INTERFACE "org.omarchy.Presence1"

/* Print the state. Every value comes from a fixed set or is a number, so
 * nothing here needs JSON escaping. */
static void print_state(sd_bus *bus) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int available = 0, attentive = 0;
  uint32_t distance = 0;
  uint64_t since = 0;
  char *state = NULL, *privacy = NULL;
  bool ok =
    sd_bus_get_property_trivial(bus, BUS_NAME, BUS_PATH, BUS_INTERFACE, "Available", &error, 'b', &available) >= 0 &&
    sd_bus_get_property_string(bus, BUS_NAME, BUS_PATH, BUS_INTERFACE, "State", &error, &state) >= 0 &&
    sd_bus_get_property_trivial(bus, BUS_NAME, BUS_PATH, BUS_INTERFACE, "Attentive", &error, 'b', &attentive) >= 0 &&
    sd_bus_get_property_trivial(bus, BUS_NAME, BUS_PATH, BUS_INTERFACE, "DistanceMm", &error, 'u', &distance) >= 0 &&
    sd_bus_get_property_trivial(bus, BUS_NAME, BUS_PATH, BUS_INTERFACE, "Since", &error, 't', &since) >= 0 &&
    sd_bus_get_property_string(bus, BUS_NAME, BUS_PATH, BUS_INTERFACE, "CameraPrivacy", &error, &privacy) >= 0;

  bool known = ok && state && (strcmp(state, "present") == 0 || strcmp(state, "away") == 0);
  bool privacy_known = ok && privacy && (strcmp(privacy, "on") == 0 || strcmp(privacy, "off") == 0);

  printf("{\"available\":%s,\"state\":\"%s\",\"attentive\":%s,\"distance_mm\":%u,\"since\":%.3f,\"camera_privacy\":\"%s\"}\n",
         ok && available ? "true" : "false", known ? state : "unknown", ok && attentive ? "true" : "false",
         ok ? distance : 0, ok ? (double)since / 1e6 : 0.0, privacy_known ? privacy : "unknown");
  fflush(stdout);

  free(state);
  free(privacy);
  sd_bus_error_free(&error);
}

static int on_change(sd_bus_message *message, void *userdata, sd_bus_error *error) {
  (void)userdata, (void)error;
  print_state(sd_bus_message_get_bus(message));
  return 0;
}

int main(int argc, char **argv) {
  const char *command = argc > 1 ? argv[1] : "";
  bool watch = strcmp(command, "watch") == 0;
  sd_bus *bus = NULL;
  int r;

  if (!watch && strcmp(command, "status") != 0) {
    fprintf(stderr, "Usage: omarchy-presence status|watch\n");
    return 2;
  }

  if ((r = sd_bus_open_system(&bus)) < 0) {
    fprintf(stderr, "omarchy-presence: cannot reach the system bus: %s\n", strerror(-r));
    return EXIT_FAILURE;
  }

  if (watch) {
    /* Its own properties changing, and the service itself coming or going. */
    r = sd_bus_match_signal(bus, NULL, BUS_NAME, BUS_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            on_change, NULL);
    if (r >= 0)
      r = sd_bus_add_match(bus, NULL,
                           "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                           "member='NameOwnerChanged',arg0='" BUS_NAME "'",
                           on_change, NULL);
    if (r < 0) {
      fprintf(stderr, "omarchy-presence: %s\n", strerror(-r));
      return EXIT_FAILURE;
    }
  }

  print_state(bus);

  while (watch) {
    if ((r = sd_bus_process(bus, NULL)) < 0) break;
    if (r > 0) continue;
    if ((r = sd_bus_wait(bus, UINT64_MAX)) < 0) break;
  }

  sd_bus_unref(bus);
  return watch ? EXIT_FAILURE : EXIT_SUCCESS;
}
