# supercamera-yuyv-linux

Experimental Linux V4L2 kernel driver for a Geek/Szitman **SuperCamera** USB endoscope variant using USB VID:PID `2ce3:3828`.

The supported variant exposes:

- USB interface `0`
- bulk IN endpoint `0x82`
- bulk OUT endpoint `0x02`
- native video stream `320x240 YUYV/YUY2`

Once loaded, the camera appears as a normal V4L2 device such as `/dev/video4` and can be used by `ffplay`, `v4l2-ctl`, OBS, browsers, and other V4L2-aware software.

> **Important:** other devices using the same `2ce3:3828` USB ID have been observed with a different interface-1/JPEG protocol. This driver deliberately checks for interface 0 and endpoints `0x82`/`0x02` before binding.

## Tested system

Developed and tested on Ubuntu 26.04 with Linux `7.0.0-31-generic`.

## Install as a DKMS package

Build the `.deb` from the repository:

```sh
./scripts/build-deb.sh
```

Then install it:

```sh
sudo apt install ./dist/supercamera-yuyv-dkms_0.1.0-1_all.deb
```

The package registers the source with DKMS, builds the module for the running kernel when matching headers are installed, and enables automatic rebuilds for future kernels.

Check the installation with:

```sh
dkms status
modinfo -n supercamera_yuyv
```

The USB module alias normally causes the module to load automatically when the camera is connected, so a manual `modprobe` should not usually be necessary.

## Install directly from source with DKMS

```sh
sudo apt install dkms build-essential linux-headers-$(uname -r)

sudo cp -a . /usr/src/supercamera-yuyv-0.1.0
sudo dkms add -m supercamera-yuyv -v 0.1.0
sudo dkms install -m supercamera-yuyv -v 0.1.0
sudo modprobe supercamera_yuyv
```

## Build without DKMS

```sh
sudo apt install build-essential linux-headers-$(uname -r)
make
sudo insmod ./supercamera_yuyv.ko
```

Unload a manually loaded module with:

```sh
sudo rmmod supercamera_yuyv
```

## Verify the camera

List V4L2 devices:

```sh
v4l2-ctl --list-devices
```

Inspect the camera format, replacing `/dev/videoX` with the assigned node:

```sh
v4l2-ctl -d /dev/videoX --list-formats-ext
```

Expected format:

```text
YUYV 4:2:2
320x240
```

Test video:

```sh
ffplay -f v4l2 \
  -video_size 320x240 \
  -input_format yuyv422 \
  /dev/videoX
```

The device number is not fixed; laptops with integrated cameras may assign the endoscope `/dev/video4`, `/dev/video5`, or another number.

## Remove the DKMS package

```sh
sudo apt remove supercamera-yuyv-dkms
```

or, for a complete package purge:

```sh
sudo apt purge supercamera-yuyv-dkms
```

## Protocol and attribution

The working protocol for this hardware variant was derived from and verified against the MIT-licensed [Bognabon/Endoscope_Viewer](https://github.com/Bognabon/Endoscope_Viewer) userspace implementation. The stream consists of 320x240 YUYV frames; the first successful video block includes a 512-byte preamble.

Development also referenced an earlier GPL-2.0-or-later Linux-media SuperCamera V4L2 patch by Amaury Barral and Hadrien Barral. That driver targets a different 640x480 JPEG/interface-1 variant of the same USB ID.

See [THIRD_PARTY.md](THIRD_PARTY.md) for attribution and licensing details.

## License

This project is licensed under **GPL-2.0-or-later**. See [LICENSE](LICENSE).
