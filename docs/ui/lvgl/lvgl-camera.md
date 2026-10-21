# Camera image and streams (LVGL)

The picture tile, its full screen view (`picture_panel.cpp`) and MJPEG
streams, with the measurements per board. The tile itself is in
[lvgl-dashboard.md](lvgl-dashboard.md) section 3. The device's own camera (Settings, Camera) is in
[lvgl-settings.md](lvgl-settings.md) section 3, its web stream in
[web-app.md](../web/web-app.md) section 4.

## 1. Camera image

A picture tile asks for its image when the page opens and every `refresh` seconds (default 60)
while it is shown, but not while its last request is on its way (an image that came after a newer
request was thrown away, and a page of slow cameras only grew the queue); after 60 s without an
answer it asks again. A tap shows it full screen on black, refreshed the same way. Until the first
full screen frame arrives, the tile's frame is shown enlarged where it lies in the whole picture
(it is the picture's middle in the tile's shape; `core::Image::source_width` tells the picture's
shape), so the stream does not jump when it starts.

The full screen view (`picture_panel.cpp`) shows the whole picture. At 100 % it covers the screen
(the side that sticks out is cut off), so every camera looks the same whatever its resolution.
The smallest zoom shows the whole picture (its width or its height fills the screen), the largest
is 200 %. Two fingers zoom freely by their distance (pinch), and the spot of the picture between
them stays under them: LVGL zooms the frame on screen while they move, and when a finger lifts the
frames in that size are asked for. The buttons go from the zoom to the next step up or down (50,
75, 100, 150 and 200 %; below the smallest step the smallest zoom). A swipe moves the picture, as
far as its edges (a side smaller than the screen stays centered); the position is kept as a part
of the picture, so it stays on the same spot when the zoom changes. The finger left on the screen
after a pinch does not move the picture until it lifts too. On the WT32-SC01 the pinch ends when
the fingers come closer than about 3 cm (its controller makes one point of them). A touch shows
round buttons at the top center, "X" (back), "+" and "-" (grayed at the ends), with "swipe to move,
pinch to zoom" below them ("swipe to move" in the Windows version, which has no second finger);
they hide 5 s after the last touch, and the view opens with them shown. They sit below the pull-down zone of Settings
(`overlay::kZoneHeight`), which lies above the panel and takes the taps there. A new zoom zooms the
frame on screen with LVGL at once and asks for the frames in the new size: still images anew, a
stream goes on in it without a new connection. Zoom and position are stored per camera (entity and
stream, NVS `camview/*`, 10 s after the view closed, architecture.md section 9.3): the view opens as
it was left, also after a restart. The tile's frame shown until the first frame of the view does
not cut the stored position. Each change of size, zoom or place
redraws the whole screen (LVGL's own invalidation of the zoomed image left parts of the larger
picture on the black after a zoom out).

The image is `GET /api/camera_proxy/<entity>?width=W&height=H` with the token, W x H the size of
the tile inside its border or of the screen at the zoom. Home Assistant scales it when it can; the device
decodes the JPEG at the smallest scale that still covers the box (1:1 to 1:8), then cuts the middle
to the box's aspect ratio for a tile; the full screen view asks for the whole picture
(`whole`: the decoder's output as it is, the view scales it to cover). The frame is RGB565 in the display's byte order on the large heap,
shared between the tile and its full screen view (480x320: 300 KB).

The images are fetched by the media tasks of `hass` (`media.cpp`, `media_http.cpp`, `media_decode.cpp`; ESP-IDF's `esp_http_client`
and `esp_jpeg`), so a slow camera never holds up the values of the page. On the P4 up to three
tasks fetch at the same time, as the browser does (Home Assistant takes 2 to 5 s for some cameras);
the internal RAM of the ESP32 and the S3 has room for one (on the S3 three left 19 bytes and a
socket could not be created). A task is created when a request finds every task busy (6 KB of
stack, 8 KB with https); the images are decoded one at a time. One request per camera, stream and
size waits, the newest; one that falls out of the full queue (8) is answered with an error. A JPEG
of more than 512 KB is refused.

On the P4 the hardware decoder's frame is scaled to the box by the PPA. Its first scale after
most boots never finished: the ESP32-P4's silicon bug DIG-734, worked around by
`components/esp_driver_ppa` (its README). The scale keeps a 500 ms timeout as a guard: after that
the decoder and the PPA stay off until the restart and the images are decoded in software (25 to
30 ms for a tile). The buffers of that scale are never
freed, the DMA may still use them.

## 2. MJPEG stream

A picture tile with `stream: http://...` (an MJPEG stream, for example of go2rtc) plays the stream
full screen. The tile keeps the camera proxy's images: the stream is only opened while the full
screen view is open (a connection per tile image started and stopped the hardware encoder of a
stream server each time until its driver crashed).

A stream with a login has it in its address, `http://user:password@host/...`: the
media task sends it as Basic auth (`CONFIG_ESP_HTTP_CLIENT_ENABLE_BASIC_AUTH` in `sdkconfig.main`).
The device keeps the whole address; the backup export leaves the password out (`http://user@host/`),
so after an import the stream asks for it. A 401 is `core::StreamProblem::Login`, its reason
"login needed" (no login in the address), "password missing" (the user alone) or "login
rejected". A tile without a picture whose image failed (a login, a camera not reached) shows the
camera struck through (`camera-off`) until an image comes.

What the full screen view shows when the stream stops (no frame rate in the corner; the media log
has it every 5 s). The last picture stays under the alert
strip ([lvgl-design.md](lvgl-design.md) section 8):

| State | Alert strip | Ends when |
| --- | --- | --- |
| opening (the view was opened) | none, the spinner turns | the first picture (playing), or an error (failed) |
| playing | none | the stream stops: it ended, the connection broke off, no data for 30 s |
| the stream stopped: the 2 s before the first try, and every try while it connects | "⚠ Stream stopped, retrying" (orange) | the connection is up (loading), or the try fails (failed) |
| connected, the request sent, waiting for the first picture (go2rtc answers only with it) | "Stream loading ..." (grey-blue, video symbol) | the first picture (playing, the strip slides away), or an error (failed) |
| the try failed | "⟳ Stream failed: <reason>" (red, the reload icon) | the next try, 10 s after the start of the last one (retrying), a tap: a try at once, or the view is closed |

A try starts every 10 s while the stream is down, so it comes back by itself within about 10 s of
its server. A connection that is not taken within 5 s fails (a
restarting server answered nothing, and the connect waited 18 s); the answer and the data still
get 30 s each (`kStreamConnectMs` in `media.cpp`, `kStreamTimeoutMs` in `media.hpp`). The media task tells the
UI when the connection is up and the request sent (`core::Image::opened`). The
reason is a word or two ("unreachable", "no answer", "HTTP 404", "login needed", "ended", "broke
off", "not MJPEG", "no memory", `core::StreamProblem`), the whole text is in the log. A zoom change asks the
stream for the new size and changes nothing in the strip. The Windows version plays this with
`--do "stream down"` / `"stream up"` (a down server fails a try after 5 s, an up one sends the
first frame 4 s after the connection). Full screen waits 30 s for the stream's answer (go2rtc
answers once its first frame is there, which took longer than 10 s when it first had to start its
source); a missing answer says "no answer within ... s" instead of
"HTTP -1". `stream: hass` plays Home Assistant's own MJPEG stream of the
camera (`/api/camera_proxy_stream/<entity>` with the token) full screen, and the tile keeps the
scaled snapshots of the camera proxy (that stream has the camera's full size). One stream plays
at a time; the still images of the page go on in the other tasks (on the ESP32 and the S3 they
wait meanwhile). A frame is decoded once the UI took the
one before (it takes it within 10 to 15 ms); the frames in between are read and dropped. A
stream that sends more than the board reads falls behind: the sender queues the rest (go2rtc up to
64 frames per client, 10 to 40 s of Doorspy's 370 KB/s, 2026-10-11). When the next frame is
already waiting in the socket after every frame for 10 s, the stream is opened again (the new
connection before the old one is closed, so go2rtc keeps its source running): the picture never
drifts more than about 10 s behind. The stream carries no timestamp (part headers and JPEG
markers checked), so the delay cannot be measured exactly. This is a fallback: Wi-Fi is what it
is, and a stream that comes faster than the board reads should get a lower bit rate at the source
(go2rtc: a higher `-q:v`, a smaller size or fewer fps; not every source has that option). A
stream the board can take plays without delay. A frame that cannot be decoded is skipped; three in
a row end the stream. A stream frame is the whole picture as the decoder made it, without a
copy (the copy of 480x320 through PSRAM cost 166 ms), and the view scales it to cover the screen
at the zoom; the software decoders' scale is the largest that still shows 3/4 of the frame
(`core::stream_shift`: 704x576 on 800x480 loses a strip, 1280x960 is decoded at 1:2). A spinner
turns until the first frame (its track at 50 % opacity, so the image shows through); turning
the display builds the view again in the new size and restarts the stream in it (with the kept
zoom and position). `hass
streamscale auto|0..3` on the console forces the decoder's scale. The decoder gets a work area of
8 KB per image (esp_jpeg's 3100 bytes were too small for the snapshots of Home Assistant's
`camera.balconycam`).

Measured on the WT32-SC01 (ESP32, PSRAM at 40 MHz) with go2rtc, after the first 10 to 15 s (ffmpeg
starts with large frames, 60 to 130 KB, which decode slower):

| Stream | read | shown | decode per frame |
| --- | --- | --- | --- |
| 800x480, 8 fps | 1.3 to 1.4 fps | 1.3 to 1.4 fps | 575 ms (1:2: 450 ms) |
| 640x320, 4 fps | 2.3 to 2.4 fps | 2.3 to 2.4 fps | 322 ms |
| 640x320, 2 fps | 2 fps | 2 fps (all) | 330 ms |
| 640x320, `-q:v 7` (35 KB frames) | 1.5 fps | 1.5 fps | 365 ms |
| 800x480, `-q:v 7` (67 KB frames) | 0.9 fps | 0.9 fps | 645 ms |

The decode time follows the number of pixels (about 1.5 us each) and the size of the frame; 1:2
saves little, the Huffman decoding is most of it. Reading and decoding run one after the other
in the media task (20 to 30 % of its time waits for data), so the two could overlap on the second
core. Memory while playing: internal RAM 32 to 33 KB free (unchanged), 3.8 KB of the 6 KB stack
unused, about 1 MB of PSRAM for the 800x480 frames.

On the JC4880P443C (ESP32-P4) the hardware does the work (`hass/src/image_hw.cpp`): the JPEG
decoder decodes at full size (800x480 in 17 ms, 140 ms in software; pictures whose sides are not
divisible by 8, often Home Assistant's snapshots in a tile's size, and progressive JPEGs go to the
software decoder), and the PPA scales the frame to cover the request's size (`core::cover_block`,
scale in sixteenths; the whole picture for the full screen view, `core::whole_block`), so the UI
shows a stream's frame 1:1: LVGL's zoom of a smaller frame took 100 ms. The view shows a frame
up to 7 % larger than its box 1:1 (the scale rounds up to sixteenths). A whole picture is
enlarged to at most 2 M pixels (4 MB): 200 % of a 16:9 stream on the portrait screen would be
2880x1620, 9 MB a frame with two or three on the way, and every other frame failed for memory
(2026-10-11); LVGL zooms the rest (150 and 200 % in portrait: still about 14 fps for go2rtc's
800x480). A frame that already has the screen's size is not scaled. The stream reads the next frame
while the UI draws the last one (50 ms for 800x480, software rotation) and, with the hardware
decoder, decodes it then too; the software decoder waits for the UI first, so a frame that is
dropped is not decoded for nothing. The UI's take wakes the stream (a semaphore; the tick of 10 ms
was too coarse to poll). Further settings of the board: a TCP window of 64 KB (with the default
5.7 KB go2rtc waited while a frame decoded: 3.3 fps), LVGL's rotation buffer of 48 KB (a whole
band; 10 KB flushed it in 5 pieces), the copy of LVGL's bands into the frame buffer by the CPU
(with the DMA2D memory got corrupted while a stream played, `init_panel` of the board). The
PPA's first scale after most boots never finished (silicon bug DIG-734, esp-idf issue 19023: the
driver's workaround flag stayed clear for our geometry); `components/esp_driver_ppa` overrides
the IDF's driver and always sets the flag (its README has the numbers). A scale that still does
not answer within 500 ms switches the decoder and the PPA off until the restart, every image is
then decoded in software (`image_hw.cpp`).

Measured full screen 800x480 (2026-10-08):

| Stream | source | shown | decode + scale per frame |
| --- | --- | --- | --- |
| go2rtc 800x480, `-r 15 -q:v 18` (`_xl`, 23 KB frames) | 15 fps | 14.1 to 15.8 fps | 17 ms |
| go2rtc 800x480, `-r 12 -q:v 17` (24 KB frames) | 12.4 fps | 11 to 13 fps | 17 ms |
| go2rtc 640x320, `-r 5` / `-r 3` (`_m`, `_s`, 15 KB frames) | 5 / 3 fps | 5 / 3 fps | 7 + 15 ms |
| HA `camera.doorspy_cam_axxon0` (1280x960, 34 KB frames) | 9.4 fps | 9.4 fps | 55 + 21 ms |
| HA `camera.balconycam` (704x576, 52 KB frames) | 2 fps | 2 fps | 17 + 17 ms |

Before the hardware (software decoder, LVGL's zoom): 800x480 5.5 fps, doorspy 2.5 fps. The user
measured 18 to 22 fps with go2rtc's `-r 25` and saw no tearing with the CPU copy (2026-10-08).

On the JC3248W535 (ESP32-S3, 480x320) Espressif's `esp_new_jpeg` decodes (`hass/src/new_jpeg.cpp`,
a dependency of `hass` for the S3 only; SIMD, a 640x320 frame in 41 ms instead of TJpgDec's
148 ms). Its own buffers come from `hot_alloc` (internal RAM only while 32 KB stay free; it took
them from internal RAM first, whatever was left). It scales while it decodes, to sides in multiples of 8 (`core::cover_size8`): a frame
larger than the screen is decoded at the smallest size that covers it (800x480 at 536x320), so
LVGL needs no zoom; the scaling costs time (800x480 107 ms), a stream in the screen's size is
cheaper. The media task runs on the second core of every chip that has one: on the S3's first
core with Wi-Fi and LVGL the decoding starved the idle task (task watchdog) and took 60 ms
instead of 41. TCP window 23 KB (16 segments, the Wi-Fi buffers are in PSRAM; internal RAM
stayed at 40 KB free). Measured full screen (2026-10-08):

| Stream | source | shown | decode per frame |
| --- | --- | --- | --- |
| go2rtc 640x320, `-r 10 -q:v 17` | 10.2 fps | 9.2 to 10.9 fps | 41 ms |
| go2rtc 640x320, `-r 8 -q:v 16` (`_m`) | 8.2 fps | 7.7 to 8.4 fps | 40 ms |
| go2rtc 800x480, `-r 15 -q:v 18` (`_xl`) | 15 fps | 6.4 to 6.9 fps | 107 ms (scaled) |
| HA doorspy 1280x960 | 9.4 fps | about 3 fps | 210 to 240 ms (scaled) |

Before (TJpgDec, window 5.7 KB): 640x320 4.6 fps, 800x480 2.6 fps; the window alone gave 5.9 and
3.1 fps.

With the media task on the second core the P4 is unchanged (`_xl` 13.6 to 16.6 fps, 640x320 all
10 fps, doorspy 9.4 fps) and the WT32 (TJpgDec, default window) shows 640x320 at 2.3 to 2.6 fps,
800x480 at 1.8 fps and Home Assistant's doorspy (1280x960, 1:4, about 1 s per frame) at 0.8 fps;
internal RAM 32 KB free as before (2026-10-08).

## 3. Open points

* On the ESP32 (WT32-SC01, T4) camera images are decoded by the TJpgDec of the ROM, which does not
  read progressive JPEGs; such a camera keeps showing its icon (the log says why). A failed image keeps the last one.
