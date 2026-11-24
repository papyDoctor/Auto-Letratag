# Auto-Letratag

This project is a reverse engineering of the LetraTag LT‑100H, a 12 mm label printer. I built it for my own needs, to automate label making. I have thousands of 12 mm labels to print with increasing numbers, and I no longer wanted to do it by hand on the LT‑100H. As far as I know, there is no device that can print series of 12 mm labels with automatic cutting.

The system connects to a PC through its USB‑C port, which both powers the printer and controls it.

In practice, this project needs an LT‑100H and:

| Function | Part |
|---|---|
| Microcontroller | Waveshare RP2350‑Zero (RP2350A, USB‑C) |
| Motor | NEMA 17 17HS19‑1684D, directly on the original roller |
| Motor driver | Watterott TMC2209 SilentStepStick, 1/32 microstepping, stealthChop, about 0.4 A |
| Head and motor supply | TPS61089 boost converter, 5 V → 7.5 V, enabled by the firmware |
| Head driver | 4 × TLC5916 in a daisy chain (constant-current outputs, about 69 mA per dot) |
| Cutter | HS-5085MG servo |
| User interface | 2 buttons, VBUS and +7.5 V sensing, input for an optional thermistor |

All of the original electronics are replaced by a custom board. The thermal print head, the drive roller, the blade and the tape cassettes come from the original machine. The board drives a stepper motor coupled directly to the roller and cuts the tape with a servo. A terminal program lays out the text and sends it to the printer.

Terminal features:

- **Numbered series.** The template `BOX-{n:03}` from 1 to 20 gives BOX-001, BOX-002, … BOX-020.
- **Chained labels.** Labels are printed back to back. Each one is cut when its boundary reaches the blade, so no tape is wasted between labels.
- **Continuous printing** at about 9 mm/s: the motor does not stop between lines.
- **Full-height text** using all 30 dots of the head, on one or more lines, in any TrueType font.
- **Saved templates** to quickly reuse frequent texts.

The 3D model of the assembly is in [`mechanical/ASSY.glb`](mechanical/ASSY.glb).

> This is a personal project, I'm not affiliated with Dymo. Dymo and LetraTag are trademarks of their respective owners.

## Specifications

| | |
|---|---|
| Print head | 30 dots, 0.15 mm pitch (about 170 dpi), 4.5 mm print height |
| Speed | about 9 mm/s in continuous mode (16.6 ms per full line) |
| Cutting | automatic, servo-driven, head-to-blade distance adjustable without recompiling |
| Power | USB‑C 5 V (designed for a USB‑C port that supplies up to 3 A) |
| Link | USB serial port, text protocol |

## V3 board

KiCad 10, 4-layer, 70 × 60 mm ([`hardware/V3`](hardware/V3), schematic as [PDF](hardware/V3/letratag-schematic.pdf)).

**Current budget.** Heating all 30 dots at once would draw about 3.4 A from USB. The firmware therefore heats each line in groups of 10 dots. The groups are interleaved, so the text stays straight while the tape moves.

**Power sequencing.** The switched +5 V rail comes up first, then +7.5 V, then the motor driver. Everything is switched off at the end of a series, or after one minute without activity.

[`hardware/V1`](hardware/V1) and [`hardware/V2`](hardware/V2) are earlier versions, built around a XIAO RP2350 and the original DC motor.

## Firmware

[`firmware/letratag_v3/letratag_v3.ino`](firmware/letratag_v3/letratag_v3.ino), written in Arduino.

To build it, install Earle Philhower's [arduino‑pico](https://github.com/earlephilhower/arduino-pico) core in the Arduino IDE and select the **Waveshare RP2350 Zero** board. The `Servo` and `EEPROM` libraries come with the core. From the command line:

```sh
arduino-cli compile --fqbn rp2040:rp2040:waveshare_rp2350_zero firmware/letratag_v3
```

The folder also contains two other programs:
- [`firmware/test_v3`](firmware/test_v3): a bring-up sketch for the V3 board, to test the power rails, motor, servo and head one at a time;
- [`firmware/letratag`](firmware/letratag): the firmware for the earlier versions.

### Console

The USB serial port runs at 115200 baud, with newline line endings. `help` (or any unknown command) lists all commands.

| Command | Action |
|---|---|
| `blocks`, `ruler`, `square`, `line <dot>` | test labels |
| `blade <mm>` | distance from the dot line to the blade (saved in flash) |
| `margin <mm>` | blank space before and after the text (saved in flash) |
| `group <n>` | number of dots heated at the same time (saved in flash) |
| `calib` | prints a thin bar and cuts through its middle if `blade` is right |
| `feed <mm>`, `cut`, `flush` | feed the tape; cut; feed out and cut what has been printed |
| `status`, `off` | voltages and settings; switch off all power rails |

**Buttons:**
- **A**, held: feeds the tape;
- **B**: cuts what has been printed;
- **either button** stops a print in progress.

### Cutter calibration

Do this once, then again whenever the cutter mechanism changes:

1. Type `calib`. The firmware prints a thin bar and cuts where its middle should be.
2. Look for the bar:
   - **on the piece that came out**, x mm from the edge: type `blade <current value − x>`;
   - **on the tape still in the machine**, x mm from the edge: type `blade <current value + x>`.
3. Repeat until the cut goes through the bar.

The cutter servo travel is set in the firmware by `SERVO_REST_US` (blade retracted) and `SERVO_CUT_US` (full cut). Measure it on each mechanism: beyond these limits, the servo pushes against its end stops.

### Protocol

One text command per line:

```
PC → INFO                     PC ← INFO first=1 dots=30 pitch_um=150 max_lines=4000 cut=2
PC → IMG <n> [us] [c] [k]     PC ← READY <n>
PC → L <hex>   (n times)      PC ← P <i>/<n> … SPEED … VOLTAGES … DONE
PC → FLUSH                    PC ← DONE
```

- **`L` lines:** each line is a 30-bit word in hexadecimal; bit 0 is the top dot of the label.
- **`us`:** heating time, 5000 µs by default.
- **`c`:** `1` (default) for continuous printing, `0` to stop the motor at each line.
- **`k`:** `1` (default) to feed out and cut the label, `0` if another label follows. In that case, the cut happens while the next label is printing.
- **`ERR <message>`** reports an error.

## Terminal program

[`software`](software) is a terminal application written in Rust with ratatui.

```sh
cd software
cargo build --release
./target/release/letratag
```

The program finds the board on USB by itself. Close the Arduino IDE serial monitor first, otherwise the port is busy.

| Option | |
|---|---|
| `--port <port>` | serial port, if automatic detection fails |
| `--font <file.ttf>` | font (default: macOS Arial Bold) |
| `--width <factor>` | character width, from 1 to 2 (default 1.08, matching an original Dymo label) |
| `--templates <file>` | saved templates file (default: `~/.config/letratag/templates.txt`) |
| `--mirror` | prints the text mirrored |

**Fields:**
- **Template**: the text. `{n}` or `{n:03}` is replaced by the number, and `|` starts a new line.
- **Start** and **End**: the numbers of the series.
- **Heat**: the heating time in µs.

A preview shows the label before printing.

| Key | |
|---|---|
| Ctrl‑P or F5 | print the series |
| Ctrl‑N | new line in the template |
| Ctrl‑S / Ctrl‑O | save the template / pick a saved template |
| Ctrl‑A | continuous / stop at each line |
| Esc | stop after the current label, or quit |

Saved templates are stored in a plain text file, one per line, which can also be edited by hand.

## Repository layout

```
firmware/     V3 firmware, bring-up sketch, firmware for earlier versions
software/     PC program (Rust)
hardware/     V1, V2, V3 boards (KiCad)
mechanical/   3D assembly (ASSY.glb)
docs/         project web page (GitHub Pages)
Datasheets/   component datasheets
scripts/      helper tools
```

## License

Copyright (c) 2026 Olivier Monnom.

| | License |
|---|---|
| Software: `firmware/`, `software/`, `scripts/` | [MIT](LICENSE) |
| Hardware: `hardware/` (KiCad boards), `mechanical/` (3D parts) | [CERN-OHL-P v2](LICENSE-HARDWARE) |
| Documentation: `docs/` (text, photos, videos), this README | [CC BY 4.0](LICENSE-DOCS) |

Not covered by these licenses, and belonging to their manufacturers:
- the datasheets in `Datasheets/`;
- the vendor 3D models included in `mechanical/ASSY.glb`: stepper motor, servo, TMC2209 module.
