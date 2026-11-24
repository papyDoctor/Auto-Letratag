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

* TOC
{:toc}

## Why

I have thousands of 12 mm labels to print, each with an increasing number: BOX-001, BOX-002, and so on. The Dymo LetraTag LT‑100H prints nice labels, but one at a time: type the text, print, cut by hand, change the number, again. As far as I know, no device prints series of 12 mm labels and cuts them automatically.

So I kept the parts of the LT‑100H that are hard to make: the thermal head, the rubber drive roller, the cutting blade and the tape cassettes. I replaced everything else: a custom board drives the head, a stepper motor turns the roller, a servo moves the original blade to cut each label, and a program on the PC sends the whole series over USB.


## Inside the LT‑100H

![The LT‑100H](images/ph01.jpg)
*The LT‑100H*

![The LT‑100H opened](images/ph01a.jpg)
*Opening the LT‑100H*

![The LT‑100H opened](images/ph01b.jpg)
*Opening the LT‑100H, removing some parts*

![The LT‑100H opened](images/ph01c.jpg)
*Removing some parts, cont.*

![The LT‑100H opened](images/ph01d.jpg)
*Removing some parts, cont.*

![The LT‑100H opened](images/ph01e.jpg)
*The main board with the print head connected*

![The LT‑100H opened](images/ph01f.jpg)
*Disconnecting the ribbon flex, unlike on the photo, this can be done with two hands to spread the force over the ribbon. The solder joints are weak, so this works (I did it twice). If in doubt, use a hot air gun.*

![The LT‑100H opened](images/ph01g.jpg)
*The ribbon flex disconnected*

![The LT‑100H opened](images/ph01h.jpg)
*The print head with its ribbon flex*

![The LT‑100H opened](images/ph01i.jpg)
*The motor block, we need to harvest the rubber sleeve*

![The LT‑100H opened](images/ph01j.jpg)
*The rubber sleeve on its support*

![The LT‑100H opened](images/ph01l.jpg)
*The print head and the rubber sleeve. Two of the three parts that we harvest*

![The LT‑100H opened](images/ph01m.jpg)
*The blade on its support (to be unscrewed) is the third part we harvest*

## Reverse engineering the print head

We need to solder the ribbon flex on a 0.635 mm pitch ribbon cable with a 1.27 mm pitch connector at the other end. This needs a 34-wire ribbon cable with solid (single-strand) wires. I soldered mine as shown in the photo and secured the joints with polyimide (kapton) tape.

![Setup to solder](images/ph02.jpg)
*Setup to solder the ribbon flex to the ribbon cable*

![Ribbon cable soldered](images/ph02a.jpg)
*Ribbon cable soldered*

![Ribbon cable other end](images/ph02b.jpg)
*Ribbon cable other end, dual-row 1.27 mm pitch IDC connector*

The head has no driver chip of its own: each of the 30 dots is a simple heating resistor, about **82 Ω**, with one common supply (two pins at the beginning and two pins at the end) and one wire per dot. There is no thermistor on the flex.

I mapped which connector pin heats which dot by printing one dot at a time. On my board the mapping turned out to be simple: connector pin *p* heats dot *p − 2*.

## First boards: V1 and V2 (not used here)

The first two boards were built around a Seeed **XIAO RP2350** and kept the original DC motor. A hall sensor and a small magnet on the motor shaft counted the rotation, about 72 pulses per millimetre of tape.

It printed, but the DC motor was the weak point:
- the motor kept turning after the power was cut, by a varying amount;
- stopping the tape completely before heating each line left thin white lines between the lines;
- at low speed the motor was close to stalling and its speed was irregular.

Then I decided to go for a direct drive with a stepper motor. Since the print had insufficient contrast, I also added a boost converter to V3.

## V3: stepper motor and a new board

![V3 board schematic](images/shema.jpg)
*V3 board schematic*

![Board V3 Top](images/pcb_recto.jpg)
*Board V3 Top*

![Board V3 Bottom](images/pcb_verso.jpg)
*Board V3 Bottom*

V3 replaces the DC motor and its gearbox with a **NEMA 17 stepper motor** (17HS19‑1684D) **in direct drive**: the original rubber tube of the roller sits directly on the motor shaft. With 1/32 microstepping, one print line (0.15 mm) is about 30.6 microsteps, so the tape position is known exactly, without any sensor.

To drive the dots I used four **TLC5916**, chained 8-bit constant-current LED drivers. Each output sinks a fixed current set by one resistor, which is exactly what a thermal dot needs: the heat no longer depends on the supply voltage. With 270 Ω, each dot gets about 69 mA, close to the 0.4 W per dot of the original machine.

**Current budget.** Everything runs from one USB‑C port. Heating all 30 dots at once would take about 3.4 A from USB, which is too much. The firmware heats each line in **groups of 10 dots**.

**Power sequencing.** The switched +5 V comes up first, then the 7.5 V boost, and only then the motor driver, which must never be enabled without its motor supply. At the end of a series, or after one minute without activity, everything is switched off.

**Main parts used**

| Function | Part |
|---|---|
| Microcontroller | Waveshare RP2350‑Zero |
| Motor driver | Watterott TMC2209 SilentStepStick, stealthChop, about 0.4 A |
| Head and motor supply | TPS61089 boost converter, 5 V → 7.5 V |
| Head driver | 4 × TLC5916 |
| Cutter | Hitec HS‑5085MG servo |

The KiCad files are in [`hardware/V3`](https://github.com/papyDoctor/Auto-Letratag/tree/main/hardware/V3), and the schematic is also available as a [PDF](https://github.com/papyDoctor/Auto-Letratag/blob/main/hardware/V3/letratag-schematic.pdf).

## Mechanics

All parts are FDM 3D-printed. I also used spacers for the tape guide, M2 and M3 threaded inserts and some screws. By chance the rubber sleeve has an internal diameter of 4mm that is just the size of the stepper shaft.

I designed the parts in Onshape. The CAD source is on Onshape (public document "AUTO LETRATAG"); the repo only contains the assembly as a GLB file, [`mechanical/ASSY.glb`](https://github.com/papyDoctor/Auto-Letratag/blob/main/mechanical/ASSY.glb).

![The printer 3D model](images/3dmodel.jpg)
*The printer 3D model*

![The electronic board mounted and connected](images/ph03.jpg)
*The electronic board mounted and connected. From left to right: the servo, the two buttons and the stepper connectors*

![Overall assembly](images/ph03a.jpg)
*Overall assembly*

![Zoom on the print head](images/ph03b.jpg)
*Zoom on the print head*

The cutter reuses the original LT‑100H blade. The manual lever is gone: the blade is held in 3D-printed parts and a servo closes it. I tried several servos for it. I ended up with a Hitec HS‑5085MG but I think a more basic servo would do. The servo travel is limited in the firmware to the range measured on the mechanism (1050–1710 µs).

![The cutter sub parts 1](images/ph03c.jpg)
*The cutter sub parts 1*

![The cutter sub parts 2](images/ph03d.jpg)
*The cutter sub parts 2*

Two mechanical problems took a long time:
- **Head pressure.** When the head is not exactly tangent to the roller, the print comes out blank or pale, even with perfect electronics (solved with a small hack, see photo).

![Small hack](images/ph04.jpg)
*Small hack: washer to tilt the print head*

- **Tape sticking.** After printing, the tape sometimes stopped moving until the head was lifted and put back (solved with sufficient head pressure).

![Print head on the rubber roller](images/ph04a.jpg)
*Print head on the rubber roller*

## Firmware

The firmware is written in Arduino, with Earle Philhower's arduino‑pico core. It receives the labels as bitmaps from the PC, one line of 30 dots at a time, and prints them.

**Continuous printing.** The tape never stops between lines: the motor runs at a constant speed, chosen for the line that takes the longest to heat. In practice that is about 16.6 ms per line, or **9 mm/s**.

**Straight text.** Since the tape moves while the head heats a line in three groups, the first group would print slightly before the last one, and the text came out slanted. The fix was to interleave the groups: dots 1, 4, 7… then 2, 5, 8… then 3, 6, 9… Each group now covers the whole height, and the text is straight.

![Two-line label printing](images/ph_a.jpg)
*Two lines printing*

**Chained labels and automatic cutting.** The blade is 57.5 mm away from the head, so a label is cut long after it is printed. Labels are printed back to back, each one with a 2 mm margin before and after the text. The firmware keeps track of every label boundary on the tape. When a boundary reaches the blade, the motor slows down and stops exactly there, the servo cuts, and printing goes on. No tape is wasted between labels.


<div class="video"><iframe src="https://www.youtube-nocookie.com/embed/QhKLfP7KmZg" title="Auto-Letratag" allowfullscreen></iframe></div>

**Calibration.** The head-to-blade distance depends on the mechanics, so it is a setting saved in flash, not a constant in the code. The `calib` command prints a thin bar and cuts where its middle should be. Look where the bar ended up, correct the value with `blade <mm>`, and repeat.

## PC software

![Software](images/softw.jpg)
*Software*

The PC program is written in Rust with ratatui and runs in a terminal. You type a template such as `BOX-{n:03}`, choose the first and last numbers, and it prints the whole series.

- The text is rendered with any TrueType font, scaled so that it uses all 30 dots of the head.
- The characters are widened by 8 %, to match the length of an original Dymo label (42 mm instead of 39 mm for the same text).
- A `|` in the template starts a second line.
- Frequent templates can be saved and picked from a list.

The board talks a simple text protocol over the USB serial port, described in the [README](https://github.com/papyDoctor/Auto-Letratag#protocol).

## Results

<div class="video"><iframe src="https://www.youtube-nocookie.com/embed/uRbXPzTRf6M" title="Auto-Letratag" allowfullscreen></iframe></div>


## What I learned and what's next

There is no simple project when you deal with hardware.

The project does what I need. The contrast could still be better, but that would require more careful power management within the 3 A that USB‑C can provide.

## Files

Everything is on GitHub: [papyDoctor/Auto-Letratag](https://github.com/papyDoctor/Auto-Letratag).

| | |
|---|---|
| Board (KiCad) | [`hardware/V3`](https://github.com/papyDoctor/Auto-Letratag/tree/main/hardware/V3) |
| Schematic (PDF) | [`hardware/V3/letratag-schematic.pdf`](https://github.com/papyDoctor/Auto-Letratag/blob/main/hardware/V3/letratag-schematic.pdf) |
| Firmware | [`firmware/letratag_v3`](https://github.com/papyDoctor/Auto-Letratag/tree/main/firmware/letratag_v3) |
| PC program | [`software`](https://github.com/papyDoctor/Auto-Letratag/tree/main/software) |
| 3D model | [`mechanical/ASSY.glb`](https://github.com/papyDoctor/Auto-Letratag/blob/main/mechanical/ASSY.glb) |

Software (firmware, PC program) under the [MIT license](https://github.com/papyDoctor/Auto-Letratag/blob/main/LICENSE). Hardware (boards, 3D parts) under the [CERN Open Hardware Licence v2, Permissive](https://github.com/papyDoctor/Auto-Letratag/blob/main/LICENSE-HARDWARE). This page, its photos and videos under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

*This is a personal project, I'm not affiliated with Dymo. Dymo and LetraTag are trademarks of their respective owners.*
