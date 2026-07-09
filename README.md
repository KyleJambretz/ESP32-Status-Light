# ESP32 Website Status Light

An ESP32 checks whether [kylejambretz.dev](https://kylejambretz.dev) is online and shows the answer on an RGB LED.

Green means the site is up. But green and red are not enough on their own. If my WiFi drops, a simple monitor turns red and blames the website, which is wrong. This one checks whether the problem is on my end.

## How it decides

When the site check fails, the board tries a plain TCP connection to `1.1.1.1`.

- Site fails, `1.1.1.1` works, so the site is really down.
- Site fails, `1.1.1.1` also fails, so my network is down and the site is not to blame.

That one extra check is the difference between a light you trust and a light you ignore.

## Colors

| Color  | Pattern   | Meaning                                      |
|--------|-----------|----------------------------------------------|
| Purple | Solid     | Just booted, nothing checked yet             |
| Blue   | Breathing | Not connected to WiFi (my fault)             |
| Amber  | Breathing | WiFi is up but the internet is not (my fault)|
| Red    | Breathing | Internet is fine, the site will not respond  |
| Red    | Solid     | The site answered with an error (4xx or 5xx) |
| Green  | Dim solid | The site answered normally (2xx)             |

## Hardware

- ESP32-WROOM dev board
- One RGB LED with R, G, B, and a common ground pin
- Three resistors, around 220 ohms each (Good practice, but I did not)

| LED pin | ESP32 pin |
|---------|-----------|
| R       | GPIO 25   |
| G       | GPIO 26   |
| B       | GPIO 27   |
| Common  | GND       |

If the common leg goes to 3V3 instead of GND, you have a common anode LED. Set `COMMON_ANODE` to `true` in the sketch.
