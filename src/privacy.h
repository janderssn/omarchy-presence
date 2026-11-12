/* The privacy control of the camera the presence sensor is built into.
 *
 * A laptop's camera privacy key blanks the camera in hardware, usually with
 * no light to show for it, and blinds the presence sensor with it. The camera
 * reports the switch as a V4L2 control. Cameras that announce a change send
 * an event; many do not, so the caller also reads the control when it has
 * reason to. No picture is ever read.
 */
#ifndef PRESENCE_PRIVACY_H
#define PRESENCE_PRIVACY_H

enum privacy { PRIVACY_UNKNOWN, PRIVACY_OFF, PRIVACY_ON };

/* Open the first camera on the same USB device as `sensor_device` that has a
 * privacy control, and subscribe to its changes. Returns an fd to watch for
 * EPOLLPRI, or -errno when there is no such camera. */
int privacy_open(const char *sensor_device);

/* The switch as the camera reports it right now. */
enum privacy privacy_read(int fd);

/* Take the pending change events off the fd. Returns the latest state, or
 * PRIVACY_UNKNOWN when there was none. */
enum privacy privacy_drain(int fd);

#endif
