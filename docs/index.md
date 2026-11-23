---
layout: default
title: Auto-Letratag build log
---

<!--
  HOW TO EDIT THIS PAGE

  Markers still to replace (search for them):
    PHOTO —  a photo to take / insert
    VIDEO —  a video to embed
    TODO —   a fact or a sentence to check or complete

  Photo with a caption:
    ![Short description](images/03-head-flex.jpg)
    *Your caption here.*

  Prepare photos (resize to 1600 px, remove GPS/EXIF data):
    scripts/resize_photos.sh ~/Desktop/my-photos

  YouTube video (replace VIDEO_ID; on YouTube: Share → Embed):
    <div class="video"><iframe src="https://www.youtube-nocookie.com/embed/VIDEO_ID"
      title="Auto-Letratag" allowfullscreen></iframe></div>
-->

<style>
  .video { position: relative; padding-top: 56.25%; margin: 1em 0; }
  .video iframe { position: absolute; inset: 0; width: 100%; height: 100%; border: 0; }
  .main-content img { display: block; margin: 1em auto 0.3em; border-radius: 4px; }
  .main-content img + em { display: block; text-align: center; font-size: 0.9em; color: #606c71; }
</style>

> **PHOTO —** cover picture: the finished printer with a strip of numbered labels coming out.

> **VIDEO —** short demo: a series of 10 labels printed and cut automatically (30–60 s).

* TOC
{:toc}

## Why

I have thousands of 12 mm labels to print, each with an increasing number: BOX-001, BOX-002, and so on. The Dymo LetraTag LT‑100H prints nice labels, but one at a time: type the text, print, cut by hand, change the number, again. As far as I know, no device prints series of 12 mm labels and cuts them automatically.

So I kept the LT‑100H's mechanics, its thermal head and its tape cassettes, and replaced everything else: a custom board drives the head, a stepper motor feeds the tape, a servo cuts each label, and a program on the PC sends the whole series over USB.

> **TODO —** a few words about you and why you like this kind of project (optional).

## Inside the LT‑100H

> **PHOTO —** the LT‑100H closed, then opened, with the original board.

> **PHOTO —** close-up of the mechanics: thermal head, rubber drive roller, manual cutter.

The LT‑100H contains:
- a thermal print head with **30 dots** at a 0.15 mm pitch (about 170 dpi), which gives 4.5 mm of printable height on a 12 mm tape;
- a rubber roller that presses the tape against the head and pulls it;
- a small DC motor with a gearbox to drive the roller;
- a lever-operated blade.

> **TODO —** what you found on the original board, what you kept and what you removed.

## Reverse engineering the print head

> **PHOTO —** the head and its flex cable, with the connector pinout.

The head has no driver chip of its own: each of the 30 dots is a simple heating resistor, about **82 Ω**, with one common supply and one wire per dot. There is no thermistor on the flex.

I mapped which connector pin heats which dot by printing one dot at a time. On my board the mapping turned out to be simple: connector pin *p* heats dot *p − 2*.

> **PHOTO —** the first test print, with each dot printed alone.

To drive the dots I used four **TLC5916**, chained 8-bit constant-current LED drivers. Each output sinks a fixed current set by one resistor, which is exactly what a thermal dot needs: the heat no longer depends on the supply voltage. With 270 Ω, each dot gets about 69 mA, close to the 0.4 W per dot of the original machine.

> **TODO —** how you found the heating time (5 ms per line today), and what the first prints looked like.

## First boards: V1 and V2

> **PHOTO —** V1 and V2 boards.

The first two boards were built around a Seeed **XIAO RP2350** and kept the original DC motor. A hall sensor and a small magnet on the motor shaft counted the rotation, about 72 pulses per millimetre of tape.

It printed, but the DC motor was the weak point:
- the motor kept turning after the power was cut, by a varying amount;
- stopping the tape completely before heating each line left thin white lines between the lines;
- at low speed the motor was close to stalling and its speed was irregular.

> **TODO —** what V2 changed (7.5 V boost, H-bridge to brake the motor…) and why you decided to move to a stepper.

## V3: stepper motor and a new board

> **PHOTO —** the V3 board, top and bottom.

> **PHOTO —** the NEMA 17 motor coupled directly to the roller.

V3 replaces the DC motor and its gearbox with a **NEMA 17 stepper motor** (17HS19‑1684D) **in direct drive**: the original rubber tube of the roller sits directly on the motor shaft. With 1/32 microstepping, one print line (0.15 mm) is about 30.6 microsteps, so the tape position is known exactly, without any sensor.

| Function | Part |
|---|---|
| Microcontroller | Waveshare RP2350‑Zero |
| Motor driver | Watterott TMC2209 SilentStepStick, stealthChop, about 0.4 A |
| Head and motor supply | TPS61089 boost converter, 5 V → 7.5 V |
| Head driver | 4 × TLC5916 |
| Cutter | Hitec HS‑5085MG servo |

**Current budget.** Everything runs from one USB‑C port. Heating all 30 dots at once would take about 3.4 A from USB, which is too much. The firmware heats each line in **groups of 10 dots**.

**Power sequencing.** The switched +5 V comes up first, then the 7.5 V boost, and only then the motor driver, which must never be enabled without its motor supply. At the end of a series, or after one minute without activity, everything is switched off.

> **TODO —** anything special about the PCB design (4 layers, the StepStick soldered flat, the RP2350 GPIO pull-down erratum…).

The KiCad files are in [`hardware/V3`](https://github.com/papyDoctor/Auto-Letratag/tree/main/hardware/V3).

## Mechanics

> **PHOTO —** the 3D model of the assembly (screenshot), then the printed parts.

> **PHOTO —** the motor mount and the head support.

> **PHOTO —** the cutter: servo, blade, 3D-printed rotor and stator.

The printed parts hold the motor, the head and the cutter on a base.

Two mechanical problems took a long time:
- **Head pressure.** When the head is not exactly tangent to the roller, the print comes out blank or pale, even with perfect electronics.
- **Tape sticking.** After printing, the tape sometimes stopped moving until the head was lifted and put back.

For the cutter I tried several servos. I ended up with a Hitec HS‑5085MG. The servo travel is limited in the firmware to the range measured on the mechanism (1050–1710 µs), because beyond that the servo pushes against the end stops.

> **TODO —** your story: what you changed in the cutter, the servos you tried, what did not work.

## Firmware

The firmware is written in Arduino, with Earle Philhower's arduino‑pico core. It receives the labels as bitmaps from the PC, one line of 30 dots at a time, and prints them.

**Continuous printing.** The tape never stops between lines: the motor runs at a constant speed, chosen for the line that takes the longest to heat. In practice that is about 16.6 ms per line, or **9 mm/s**.

**Straight text.** Since the tape moves while the head heats a line in three groups, the first group would print slightly before the last one, and the text came out slanted. The fix was to interleave the groups: dots 1, 4, 7… then 2, 5, 8… then 3, 6, 9… Each group now covers the whole height, and the text is straight.

> **PHOTO —** slanted text before the fix, straight text after.

**Chained labels and automatic cutting.** The blade is 57.5 mm away from the head, so a label is cut long after it is printed. Labels are printed back to back, each one with a 2 mm margin before and after the text. The firmware keeps track of every label boundary on the tape. When a boundary reaches the blade, the motor slows down and stops exactly there, the servo cuts, and printing goes on. No tape is wasted between labels.

> **VIDEO —** close-up of the cut happening between two labels while the next one prints.

**Calibration.** The head-to-blade distance depends on the mechanics, so it is a setting saved in flash, not a constant in the code. The `calib` command prints a thin bar and cuts where its middle should be. Look where the bar ended up, correct the value with `blade <mm>`, and repeat.

> **PHOTO —** a `calib` strip, with the cut through the bar.

## PC software

> **PHOTO —** screenshot of the terminal program with the label preview.

The PC program is written in Rust with ratatui and runs in a terminal. You type a template such as `BOX-{n:03}`, choose the first and last numbers, and it prints the whole series.

- The text is rendered with any TrueType font, scaled so that it uses all 30 dots of the head.
- The characters are widened by 8 %, to match the length of an original Dymo label (42 mm instead of 39 mm for the same text).
- A `|` in the template starts a second line.
- Frequent templates can be saved and picked from a list.

The board talks a simple text protocol over the USB serial port, described in the [README](https://github.com/papyDoctor/Auto-Letratag#protocol).

## Results

> **VIDEO —** the full demo: typing a template, printing a series, labels coming out cut.

> **PHOTO —** a pile of finished labels next to labels printed by an original LT‑100H.

> **TODO —** speed, how many labels you have printed so far, what you are happy with.

## What I learned and what's next

> **TODO —** lessons learned, ideas for later (text rendering in the firmware, bold/italic, other tape widths…).

## Files

Everything is on GitHub: [papyDoctor/Auto-Letratag](https://github.com/papyDoctor/Auto-Letratag).

| | |
|---|---|
| Board (KiCad) | [`hardware/V3`](https://github.com/papyDoctor/Auto-Letratag/tree/main/hardware/V3) |
| Firmware | [`firmware/letratag_v3`](https://github.com/papyDoctor/Auto-Letratag/tree/main/firmware/letratag_v3) |
| PC program | [`software`](https://github.com/papyDoctor/Auto-Letratag/tree/main/software) |
| 3D model | [`mechanical/ASSY.glb`](https://github.com/papyDoctor/Auto-Letratag/blob/main/mechanical/ASSY.glb) |

> **TODO —** license.

*This is a personal project, I'm not affiliated with Dymo. Dymo and LetraTag are trademarks of their respective owners.*
