# Wiring

Wire by the **label printed on each part**, never by position in a table or
photo. Unplug the USB cable before adding or moving any wire.

Pin numbers below are the numbers printed beside the pins on the Freenove
ESP32-WROVER board. Two pins are labelled with names instead of numbers:
**VP is GPIO 36** and **VN is GPIO 39**.

## Already wired

| Part | Pin label on the part | ESP32 pin |
|---|---|---|
| DAC | SCK | GND |
| DAC | BCK | 26 |
| DAC | DIN | 22 |
| DAC | LCK | 25 |
| DAC | VCC | 3.3V |
| DAC | GND | GND |

The SD card slot is built into the board (pins 14, 15 and 2 internally).

## LCD (LCD1602 with I2C backpack)

| Pin label on the backpack | ESP32 pin |
|---|---|
| GND | GND |
| VCC | 5V |
| SDA | 21 |
| SCL | 27 |

SCL goes to **27**, not 22 as in Freenove's tutorial: 22 is the DAC's data
line. 5V goes to the LCD's VCC and nowhere else.

If the backlight is on but no text shows, turn the small blue screw on the
back of the LCD (the contrast adjuster) until characters appear.

## Keypad (4x4 membrane)

Hold the keypad with the keys facing you and the ribbon at the bottom. The
eight pins are numbered 1 to 8 from left to right.

| Keypad pin | What it is | ESP32 pin | Also needs |
|---|---|---|---|
| 1 | Row 1 (keys 1 2 3 A) | 32 | |
| 2 | Row 2 (keys 4 5 6 B) | 33 | |
| 3 | Row 3 (keys 7 8 9 C) | 4 | |
| 4 | Row 4 (keys * 0 # D) | 13 | |
| 5 | Column 1 (keys 1 4 7 *) | 34 | 10k resistor to 3.3V |
| 6 | Column 2 (keys 2 5 8 0) | 35 | 10k resistor to 3.3V |
| 7 | Column 3 (keys 3 6 9 #) | VP (36) | 10k resistor to 3.3V |
| 8 | Column 4 (keys A B C D) | VN (39) | 10k resistor to 3.3V |

### The four resistors

Pins 34, 35, VP and VN can only read, and have no built-in pull-up. Each of
the four column wires therefore needs a 10k resistor (brown, black, orange)
from that wire to **3.3V**. On a breadboard: put each column wire in its own
row, and bridge each of those rows to the 3.3V rail with one resistor.

**The resistors go to 3.3V, never 5V.** 5V on these pins can damage the chip.

Without the resistors the keys read as random presses. The firmware checks
for this at start-up and logs which column is affected.

### If the keys come out wrong

Getting the keypad's eight wires in the wrong order cannot damage anything.
The keypad test screen shows which key the player thinks was pressed. If
they do not match, note what each key shows; it is fixed by reordering the
pin list in `main/pins.h`, not by rewiring.

## Pins to leave alone

- **0 and 12**: used by the chip while starting up. 12 held high at reset
  stops the board booting.
- **5, 18, 19, 23**: free, reserved for future hardware.
