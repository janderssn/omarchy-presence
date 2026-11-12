/* The HID human-presence sensor, read through the kernel's IIO buffer.
 *
 * The kernel driver is hid-sensor-prox; its IIO device is named "prox". The
 * sensor only reports real values while the buffer is enabled.
 */
#ifndef PRESENCE_SENSOR_H
#define PRESENCE_SENSOR_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One IIO scan element: where it sits in a record and how to decode it. */
struct channel {
  bool enabled;
  int index;
  bool big_endian;
  bool is_signed;
  unsigned bits;
  unsigned size; /* storage bytes */
  unsigned shift;
  unsigned offset;
};

enum { CH_PRESENCE, CH_DISTANCE, CH_ATTENTION, CH_COUNT };

struct sensor {
  char device[PATH_MAX]; /* /sys/bus/iio/devices/iio:deviceN */
  int fd;
  struct channel channels[CH_COUNT];
  size_t record_size;
};

struct report {
  bool present;
  bool attentive;
  uint32_t distance_mm;
};

/* Parse an IIO scan type such as "le:s16/32>>0". Returns 0 or -EINVAL. */
int channel_parse(struct channel *channel, const char *spec);

/* Place enabled channels as the kernel does: in index order, each aligned to
 * its own size. Returns the record size. */
size_t channels_layout(struct channel *channels, size_t count);

int64_t channel_decode(const struct channel *channel, const uint8_t *record);

/* Find the sensor under `iio_root`. Returns 0 and fills `device`, or -ENODEV. */
int sensor_find(const char *iio_root, char *device, size_t size);

/* Enable the buffer and open the device. Returns 0 or -errno. */
int sensor_open(struct sensor *sensor, const char *device);

/* Disable the buffer and close the device. Safe to call twice. */
void sensor_close(struct sensor *sensor);

/* Read waiting reports into `reports`. Returns how many, 0 when none are
 * waiting, or -errno; a device that has gone away is -ENODEV. */
int sensor_read(struct sensor *sensor, struct report *reports, size_t max);

#endif
