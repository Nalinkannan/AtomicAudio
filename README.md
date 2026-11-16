# Atomic Audio

![Atomic Audio](docs/screenshot.png)

Dolby Atmos–style sound for laptop speakers on Linux. Pick a mode (Dynamic,
Movie, Music, Game, Voice) or shape it yourself with a surround virtualizer,
dialogue enhancer, virtual bass, volume leveler, loudness maximizer and a
ten-band equalizer drawn over a live spectrum. Speakers and headphones keep
separate settings, and it switches between them when you plug in.

It runs as a PipeWire filter in front of the built-in speakers, so every app
goes through it, the volume keys keep working, and turning it off puts
everything back exactly as it was.

## Why laptop speakers sound thin on Linux

On the ROG Zephyrus G14 (GA403) all four speakers already play under Linux:
the two Cirrus CS35L56 amps load their ASUS firmware and split the signal
between woofers and tweeters themselves. What Windows adds on top is Dolby's
processing. Measured with the laptop's own microphone:

- the amps cut everything below ~95 Hz, so bass simply disappears;
- the woofer cavity rings at 630 Hz (+12 dB), the "boxy" laptop sound;
- the woofer/tweeter handover leaves a dip around 2.5 kHz.

Atomic Audio's **speaker optimizer** is built from that measurement, and its
**bass enhancer** rebuilds the missing low notes as harmonics the speakers
can play (your ear fills the fundamental back in). The same measurement with
it on: the 630 Hz peak is 5 dB lower and the bass of 50–100 Hz notes comes
through 10–20 dB louder.

## Install

```sh
bin/install          # builds and installs the Arch package (asks for sudo)
atomicaudio on       # starts the engine now and at every login
```

Then open **Atomic Audio** from the launcher (Super + Space), or run
`atomicaudio tui` in a terminal.

To remove it: `atomicaudio off`, then `sudo pacman -R atomicaudio`.

## Use it

| Key | Does |
| --- | --- |
| Space | Turn Atomic Audio on or off |
| A | Compare with the original sound |
| 1 – 6 | Dynamic, Movie, Music, Game, Voice, Custom |
| E | Next equalizer preset (Flat, Open, Rich, Focused) |
| ↑ ↓ / K J | Pick an effect |
| ← → | Turn the effect down or up |
| O | Speaker optimizer on or off |
| H | Edit the speakers' or the headphones' sound |
| R | Reset this output to the defaults |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo |
| ? | All shortcuts |
| Q | Quit (the sound stays on) |

Drag the points on the equalizer, double-click one to reset it, or scroll
over it for half-decibel steps.

From the command line (and scripts, keybindings or agents):

```sh
atomicaudio status [--json]
atomicaudio mode movie
atomicaudio set bass 70            # surround, dialogue, bass, leveler, loudness: 0-100
atomicaudio set tuning off
atomicaudio eq rich                # or ten gains in dB: atomicaudio eq 2 3 2 0 0 0 0 1 1 1
atomicaudio bypass                 # toggle the original sound
atomicaudio reset
```

Add `--speakers` or `--headphones` to change that output's sound instead of
the one playing now.

## How it works

- `dsp/` is the engine: a LADSPA plugin (plain C++, no allocation in the
  audio thread, ~1.5% of one core) with the leveler, speaker optimizer,
  virtual bass, dialogue enhancer, surround virtualizer (side widening,
  decorrelated ambience and crosstalk cancellation on speakers, crossfeed on
  headphones), EQ, loudness compressor and a 2 ms lookahead limiter that
  never lets a peak past −1 dBFS.
- `atomicaudio engine` (the `atomicaudio.service` user unit) hosts it in
  PipeWire's filter-chain as a WirePlumber *smart filter* targeting the
  built-in speakers, applies settings changes live through `pw-cli`, and
  follows the headphone jack through `pactl subscribe`.
- The window, the TUI and the CLI only edit
  `~/.config/atomicaudio/settings.json`; the engine picks changes up within
  milliseconds.

## Develop

```sh
bin/build    # build/atomicaudio and build/dsp/atomicaudio.so
bin/test     # DSP, settings, backend, analyzer, theme and language tests
build/atomicaudio
```

A development build that isn't installed writes its own user unit when you
turn it on; installing the package replaces it.
