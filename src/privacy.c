#include "privacy.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum privacy privacy_read(int fd) {
  struct v4l2_control control = { .id = V4L2_CID_PRIVACY };

  if (fd < 0 || ioctl(fd, VIDIOC_G_CTRL, &control) < 0) return PRIVACY_UNKNOWN;
  return control.value ? PRIVACY_ON : PRIVACY_OFF;
}

enum privacy privacy_drain(int fd) {
  enum privacy latest = PRIVACY_UNKNOWN;
  struct v4l2_event event;

  for (;;) {
    memset(&event, 0, sizeof event);
    if (ioctl(fd, VIDIOC_DQEVENT, &event) < 0) break;
    if (event.type == V4L2_EVENT_CTRL && event.id == V4L2_CID_PRIVACY &&
        event.u.ctrl.changes & V4L2_EVENT_CTRL_CH_VALUE)
      latest = event.u.ctrl.value ? PRIVACY_ON : PRIVACY_OFF;
  }
  return latest;
}

/* The directory of the USB device a sysfs path belongs to: the nearest
 * parent that has an idVendor file. */
static int usb_device_dir(const char *sysfs_path, char *out, size_t size) {
  char path[PATH_MAX], probe[PATH_MAX];

  if (!realpath(sysfs_path, path)) return -errno;
  while (strlen(path) > 1) {
    if (snprintf(probe, sizeof probe, "%s/idVendor", path) < (int)sizeof probe && access(probe, F_OK) == 0) {
      if (strlen(path) >= size) return -ENAMETOOLONG;
      strcpy(out, path);
      return 0;
    }
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) break;
    *slash = '\0';
  }
  return -ENODEV;
}

int privacy_open(const char *sensor_device) {
  char usb[PATH_MAX], pattern[PATH_MAX];
  glob_t nodes;
  int result = -ENODEV;

  int r = usb_device_dir(sensor_device, usb, sizeof usb);
  if (r < 0) return r;
  if (snprintf(pattern, sizeof pattern, "%s/*/video4linux/video*", usb) >= (int)sizeof pattern) return -ENAMETOOLONG;
  if (glob(pattern, 0, NULL, &nodes) != 0) return -ENODEV;

  for (size_t i = 0; i < nodes.gl_pathc && result < 0; i++) {
    char dev[PATH_MAX];
    const char *name = strrchr(nodes.gl_pathv[i], '/');

    if (!name || snprintf(dev, sizeof dev, "/dev%s", name) >= (int)sizeof dev) continue;
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;

    struct v4l2_event_subscription subscription = { .type = V4L2_EVENT_CTRL, .id = V4L2_CID_PRIVACY };
    if (privacy_read(fd) != PRIVACY_UNKNOWN && ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) == 0)
      result = fd;
    else
      close(fd);
  }
  globfree(&nodes);
  return result;
}
