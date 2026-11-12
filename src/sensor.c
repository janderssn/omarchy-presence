#include "sensor.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const CHANNEL_NAMES[CH_COUNT] = {
  [CH_PRESENCE] = "in_proximity0",
  [CH_DISTANCE] = "in_proximity1",
  [CH_ATTENTION] = "in_attention",
};

int channel_parse(struct channel *channel, const char *spec) {
  char endian[3], sign;
  unsigned bits, storage, shift;

  if (sscanf(spec, "%2[a-z]:%c%u/%u>>%u", endian, &sign, &bits, &storage, &shift) != 5) return -EINVAL;
  if (strcmp(endian, "le") != 0 && strcmp(endian, "be") != 0) return -EINVAL;
  if (sign != 's' && sign != 'u') return -EINVAL;
  if (storage == 0 || storage > 64 || storage % 8 != 0 || bits == 0 || bits + shift > storage) return -EINVAL;

  channel->big_endian = endian[0] == 'b';
  channel->is_signed = sign == 's';
  channel->bits = bits;
  channel->size = storage / 8;
  channel->shift = shift;
  return 0;
}

size_t channels_layout(struct channel *channels, size_t count) {
  size_t offset = 0, largest = 1;

  /* Few channels: walk the indexes in order rather than sorting. */
  for (int index = 0; index < 64; index++) {
    for (size_t i = 0; i < count; i++) {
      struct channel *c = &channels[i];
      if (!c->enabled || c->index != index) continue;
      if (offset % c->size) offset += c->size - offset % c->size;
      c->offset = offset;
      offset += c->size;
      if (c->size > largest) largest = c->size;
    }
  }
  if (offset % largest) offset += largest - offset % largest;
  return offset;
}

int64_t channel_decode(const struct channel *channel, const uint8_t *record) {
  uint64_t raw = 0;
  const uint8_t *bytes = record + channel->offset;

  for (unsigned i = 0; i < channel->size; i++) {
    unsigned at = channel->big_endian ? i : channel->size - 1 - i;
    raw = raw << 8 | bytes[at];
  }
  raw >>= channel->shift;
  if (channel->bits < 64) raw &= (1ULL << channel->bits) - 1;
  if (channel->is_signed && channel->bits < 64 && raw & (1ULL << (channel->bits - 1)))
    return (int64_t)raw - (int64_t)(1ULL << channel->bits);
  return (int64_t)raw;
}

static int read_text(const char *path, char *buffer, size_t size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  ssize_t n = read(fd, buffer, size - 1);
  int error = n < 0 ? -errno : 0;
  close(fd);
  if (error) return error;
  buffer[n] = '\0';
  buffer[strcspn(buffer, "\n")] = '\0';
  return 0;
}

static int write_text(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  ssize_t n = write(fd, value, strlen(value));
  int error = n < 0 ? -errno : 0;
  close(fd);
  return error;
}

static int join(char *out, size_t size, const char *a, const char *b) {
  int n = snprintf(out, size, "%s/%s", a, b);
  return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

int sensor_find(const char *iio_root, char *device, size_t size) {
  DIR *dir = opendir(iio_root);
  if (!dir) return -ENODEV;

  int found = -ENODEV;
  struct dirent *entry;
  while (found < 0 && (entry = readdir(dir))) {
    char base[PATH_MAX], path[PATH_MAX], name[64];

    if (strncmp(entry->d_name, "iio:device", 10) != 0) continue;
    if (join(base, sizeof base, iio_root, entry->d_name) < 0) continue;
    if (join(path, sizeof path, base, "name") < 0 || read_text(path, name, sizeof name) < 0) continue;
    if (strcmp(name, "prox") != 0) continue;
    if (join(path, sizeof path, base, "scan_elements/in_proximity0_en") < 0 || access(path, F_OK) != 0) continue;
    if (strlen(base) >= size) continue;
    strcpy(device, base);
    found = 0;
  }
  closedir(dir);
  return found;
}

static int scan_path(char *out, size_t size, const char *device, const char *channel, const char *suffix) {
  int n = snprintf(out, size, "%s/scan_elements/%s_%s", device, channel, suffix);
  return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

static int sensor_enable(struct sensor *sensor) {
  const char *device = sensor->device;
  char path[PATH_MAX], text[64];
  int r;

  /* A buffer left enabled by a crashed run refuses new settings. */
  if ((r = join(path, sizeof path, device, "buffer0/enable")) < 0) return r;
  if ((r = write_text(path, "0")) < 0) return r;

  for (int i = 0; i < CH_COUNT; i++) {
    struct channel *c = &sensor->channels[i];

    if (scan_path(path, sizeof path, device, CHANNEL_NAMES[i], "en") < 0 || access(path, F_OK) != 0) continue;
    if ((r = write_text(path, "1")) < 0) return r;
    if ((r = scan_path(path, sizeof path, device, CHANNEL_NAMES[i], "index")) < 0) return r;
    if ((r = read_text(path, text, sizeof text)) < 0) return r;
    c->index = atoi(text);
    if ((r = scan_path(path, sizeof path, device, CHANNEL_NAMES[i], "type")) < 0) return r;
    if ((r = read_text(path, text, sizeof text)) < 0) return r;
    if ((r = channel_parse(c, text)) < 0) return r;
    c->enabled = true;
  }
  if (!sensor->channels[CH_PRESENCE].enabled) return -ENODEV;
  sensor->record_size = channels_layout(sensor->channels, CH_COUNT);

  if ((r = join(path, sizeof path, device, "buffer0/length")) < 0) return r;
  if ((r = write_text(path, "64")) < 0) return r;
  if ((r = join(path, sizeof path, device, "buffer0/enable")) < 0) return r;
  if ((r = write_text(path, "1")) < 0) return r;

  const char *node = strrchr(device, '/');
  if (snprintf(path, sizeof path, "/dev%s", node ? node : "") >= (int)sizeof path) return -ENAMETOOLONG;
  sensor->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  return sensor->fd < 0 ? -errno : 0;
}

int sensor_open(struct sensor *sensor, const char *device) {
  memset(sensor, 0, sizeof *sensor);
  sensor->fd = -1;
  if (strlen(device) >= sizeof sensor->device) return -ENAMETOOLONG;
  strcpy(sensor->device, device);

  int r = sensor_enable(sensor);
  /* Leave nothing half switched on. */
  if (r < 0) sensor_close(sensor);
  return r;
}

void sensor_close(struct sensor *sensor) {
  char path[PATH_MAX];

  if (sensor->fd >= 0) close(sensor->fd);
  sensor->fd = -1;
  if (!sensor->device[0]) return;

  /* The device may already be gone; there is then nothing to switch off. */
  if (join(path, sizeof path, sensor->device, "buffer0/enable") == 0) write_text(path, "0");
  for (int i = 0; i < CH_COUNT; i++) {
    if (!sensor->channels[i].enabled) continue;
    if (scan_path(path, sizeof path, sensor->device, CHANNEL_NAMES[i], "en") == 0) write_text(path, "0");
  }
  sensor->device[0] = '\0';
}

int sensor_read(struct sensor *sensor, struct report *reports, size_t max) {
  uint8_t data[64 * 64];
  size_t want = sensor->record_size * max;

  if (sensor->record_size == 0 || sensor->record_size > 64) return -EINVAL;
  if (want > sizeof data) want = sizeof data - sizeof data % sensor->record_size;

  ssize_t n = read(sensor->fd, data, want);
  if (n < 0) return errno == EAGAIN ? 0 : -errno;
  /* Readable with nothing to read: the device is gone. */
  if (n == 0) return -ENODEV;

  int count = 0;
  for (size_t at = 0; at + sensor->record_size <= (size_t)n; at += sensor->record_size) {
    const uint8_t *record = data + at;
    struct report *report = &reports[count++];
    const struct channel *distance = &sensor->channels[CH_DISTANCE];
    const struct channel *attention = &sensor->channels[CH_ATTENTION];

    /* With two proximity channels the first is the presence flag and the
     * second the distance; a sensor with only one reports presence alone. */
    report->present = channel_decode(&sensor->channels[CH_PRESENCE], record) > 0;
    int64_t mm = distance->enabled ? channel_decode(distance, record) : 0;
    report->distance_mm = mm > 0 && mm < 100000 ? (uint32_t)mm : 0;
    /* Without an attention channel, being there is all that can be asked. */
    report->attentive = attention->enabled ? channel_decode(attention, record) > 0 : report->present;
  }
  return count;
}
