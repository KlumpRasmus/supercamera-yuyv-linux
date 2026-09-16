# Third-party attribution

## Bognabon/Endoscope_Viewer

Protocol research for the supported interface-0/YUYV variant is based on the public project:

- https://github.com/Bognabon/Endoscope_Viewer
- Copyright (c) 2026 Bognabon
- License: MIT

The upstream MIT license notice is reproduced below:

```text
MIT License

Copyright (c) 2026 Bognabon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Earlier Linux-media SuperCamera driver

Development also referenced the earlier Linux-media V4L2 driver patch by Amaury Barral and Hadrien Barral:

- https://www.spinics.net/lists/linux-media/msg284049.html
- License in the submitted driver: GPL-2.0-or-later

That patch targets a different SuperCamera hardware/firmware variant using a 640x480 JPEG stream and a different USB interface layout. This repository implements the interface-0, 320x240 YUYV variant described above.
