# omarchy-presence

A small system service that says, on D-Bus, whether someone is sitting at the laptop.

It reads the human-presence sensor that many recent laptops have in their camera module (the one Windows uses for "wake on approach" and "lock on leave") and publishes a steady answer: someone is here, nobody is here, or it cannot tell. It also reports the camera's privacy switch, which laptops often give no other sign of.

It is the sensing half of face unlock and presence features for [Omarchy](https://omarchy.org/); the desktop side lives in Omarchy's shell. Nothing in it is Omarchy-specific, though: it is a plain D-Bus service.

## What it publishes

Bus name `org.omarchy.Presence1`, object `/org/omarchy/Presence1`, interface `org.omarchy.Presence1`, on the system bus. Properties only; each emits `PropertiesChanged`.

| Property | Type | Meaning |
|---|---|---|
| `Available` | `b` | A working sensor is being read. |
| `State` | `s` | `present`, `away`, or `unknown` while there is nothing to go on. |
| `Attentive` | `b` | Someone is there and facing the screen. |
| `DistanceMm` | `u` | How far away they are, in 10 mm steps; 0 when not known. |
| `Since` | `t` | When they arrived or left, microseconds since the epoch; 0 when unknown. |
| `CameraPrivacy` | `s` | The camera's privacy switch: `on`, `off`, or `unknown`. |

`State` is `unknown` when there is no sensor, just after start or resume, and **while the privacy switch is on**: the sensor looks through the camera, so a blocked camera makes it see an empty seat whoever is there. Consumers should do nothing on `unknown`.

## Command line

```bash
omarchy-presence status   # one line of JSON
omarchy-presence watch    # the same, again on every change
```

```json
{"available":true,"state":"present","attentive":true,"distance_mm":420,"since":1794511458.390,"camera_privacy":"off"}
```

`watch` keeps running while the service is stopped or not installed, reporting `"available": false`, so a program can start it once and read lines.

## How it decides

The sensor is jittery in two known ways, so its reports are filtered:

- **Arriving takes 1 second** of continuous presence. The sensor now and then reports a person for a moment when there is none.
- **Leaving takes 8 seconds** of continuous absence. The sensor drops to "nobody" whenever the camera it looks through is opened: about 3 seconds for an infrared face scan, up to 7 when a video call starts. `Since` is backdated to when the seat actually emptied.
- **Attention is held for 5 seconds** across the same dropouts.

The privacy switch is a V4L2 control on the camera. Cameras may announce a change, and the service listens for that, but many never do. So it also reads the control whenever the sensor sees nobody, and every 2 seconds for as long as that lasts: a blinded sensor always sees nobody, so that is the only time the switch can have turned on.

After a suspend it starts over, since whoever was there before says nothing about now.

## Hardware

Any sensor the kernel's `hid-sensor-prox` driver handles (HID usage "human presence", IIO device name `prox`). Developed on an ASUS ProArt PX13 (HN7306EA), whose sensor reports presence, distance and attention.

The sensor only gives real values through the IIO buffer, which only root can enable. That is the one reason this is a system service.

## Security

The service runs as root in a tight systemd sandbox: no network, read-only file system, no capabilities, and access to two device classes only (IIO, and video4linux for the one privacy control). It never reads a picture.

Whether someone is at the machine is readable by every local user.

## Build and install

```bash
make
make check
sudo make install        # or build the package: makepkg -si
sudo systemctl enable --now omarchy-presence
```

Needs `gcc`, `make`, `pkgconf` and systemd's development files (`systemd-libs` on Arch).

## License

MIT
