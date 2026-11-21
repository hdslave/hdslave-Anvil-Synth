# Anvil

An 8-step analog-style percussion synthesizer as a CLAP plug-in, built for the Akai MPC Sample running Melodee
(64-bit ARM Linux, Cortex-A55). Hosts list it as **hdslave Anvil** (id `com.hdslave.anvil`). It follows the front panel and normalled signal flow of the Moog DFAM and the
Behringer Edge, without the patch bay, plus one new control: **VCO EG > FM 1-2 Amt**.

Its sequencer is its own (8 steps of pitch and velocity, or fewer: Length), clocked by the host: steps fall on the
host's beat grid, swung if you like, while the host plays, stop when it stops, and start again from the song's place
when it starts or jumps.

## Signal flow

```
step PITCH ──► VCO 1 and/or VCO 2 (Seq Pitch Mod)        step VELOCITY ──► the peak of all three EGs
VCO EG ──► VCO 1 pitch (VCO 1 EG Amt), VCO 2 pitch (VCO 2 EG Amt), 1-2 FM depth (VCO EG > FM 1-2 Amt: new)
VCO 1 triangle ──linear FM──► VCO 2          VCO 1 ──hard sync──► VCO 2
VCO 1 + noise (white/pink) + VCO 2 ──► mixer (clips when hot)
  ──► 4-pole ladder, LP or HP (cutoff moved by VCF EG and by noise) ──► VCA (VCA EG) ──► Volume ──► out (stereo)
```

Every step triggers all three envelopes at its velocity. A softer step does not cut a louder tail still falling.

## Controls

The panel's names, in the order Melodee's Edit page lists them (three to a page).

| Anvil | DFAM | Edge | Range |
|---|---|---|---|
| VCO Decay | VCO DECAY | EG (VCO 1&2) DECAY | 10 ms – 10 s (time to -60 dB) |
| 1-2 FM Amt | 1→2 FM AMOUNT | 1-2 FM AMT | 0 – 100 %, linear FM from VCO 1's triangle, index 3 at 100 % |
| **VCO EG > FM 1-2 Amt** | — (new) | — (new) | ±100 %: the VCO EG added to the FM amount |
| VCO 1 Freq | VCO 1 FREQUENCY | FREQUENCY | 4.5 Hz – 4.6 kHz, 10 octaves |
| VCO 1 EG Amt | VCO 1 EG AMOUNT | ENVELOPE | ±8 octaves at full velocity |
| VCO 1 Wave | VCO 1 WAVE | SHAPE | Square / Triangle |
| VCO 2 Freq, VCO 2 EG Amt, VCO 2 Wave | VCO 2's | VCO 2's | as VCO 1's |
| Seq Pitch Mod | SEQ PITCH MOD | SEQ PITCH MOD | VCO 1&2 / Off / VCO 2 |
| Hard Sync | HARD SYNC | HARD SYNC | Off / On (VCO 2 to VCO 1) |
| Noise Color | — | NOISE COLOR | White / Pink |
| VCO 1 Level, Noise Level, VCO 2 Level | mixer levels | mixer levels | 0 – 100 %; together they clip when hot |
| Cutoff | CUTOFF | CUTOFF | 20 Hz – 20 kHz |
| Resonance | RESONANCE | RESONANCE | LP self-oscillates from about 80 %; HP rings but stays below |
| VCF Mode | VCF HP/LP | VCF SELECT | Low Pass / High Pass (both 4-pole) |
| VCF Decay | VCF DECAY | DECAY | 10 ms – 10 s |
| VCF EG Amt | VCF EG AMOUNT | ENVELOPE | ±8 octaves at full velocity |
| Noise/VCF Mod | NOISE / VCF MOD | NOISE/VCF MOD | 0 – 4 octaves of noise on the cutoff |
| VCA EG | VCA EG FAST/SLOW | VCA MODE | Fast (1 ms attack) / Slow (100 ms) |
| VCA Decay | VCA DECAY | DECAY | 10 ms – 10 s |
| Volume | VOLUME | VOLUME | 0 – 100 % |
| Clock Div | TEMPO (replaced) | SCALE | 1/2 … 1/64 of the host's beat, with triplets |
| Length | — (patch cables) | — (patch cables) | 1 – 8 steps: the sequence plays steps 1 to Length, then step 1 again (on the panels, a cable from a step's trigger to Reset) |
| Swing | — | — | 50 – 75 %: the second step of each pair comes late, at that share of the pair, as an MPC's swing does (66 %: a triplet shuffle) |
| Run | RUN/STOP | PLAY/STOP | Stop / Run |
| Trigger | TRIGGER | TRIGGER | plays the current step without moving |
| Advance | ADVANCE | ADVANCE | moves one step without playing |
| Pitch 1–8 | PITCH 1–8 | PITCH | ±5 octaves |
| Velocity 1–8 | VELOCITY 1–8 | VEL | 0 – 100 % |

A MIDI note (a pad on a Melodee plug-in track) does what Trigger does.

### The new control

**VCO EG > FM 1-2 Amt** adds the VCO EG to the 1-2 FM amount, as patching the VCO EG output into the FM amount's CV
input would. Positive: FM is deepest on the hit and fades with the VCO EG (a bright, metallic attack that settles).
Negative, with 1-2 FM Amt up: the hit starts clean and the FM grows as the envelope falls. The sum stays within
0 – 100 %. The VCO EG peaks at each step's velocity, so louder steps get more of it.

## Presets

Five factory presets, inside the plug-in: Melodee lists them on the plug-in's PRESETS page (any host with CLAP preset
discovery does). A preset sets every control, Clock Div and the eight steps too; Default puts everything back.

| Preset | What it is |
|---|---|
| Default | The panel as it opens: both VCOs into the low pass, 1/8 steps |
| Kick | VCO 1's triangle swept down by a fast VCO EG, a little noise, 1/8 steps |
| FM Perc | VCO 2 frequency-modulated, its depth riding the VCO EG (the new control at +70 %), a stepped pitch line in 1/16 |
| Sync Bass | VCO 2 hard-synced and swept, a resonant low pass, the slow VCA attack, a pitch line in 1/16 |
| Noise Hat | White noise through the high pass, accented on the odd 1/16 steps |

They are the patches the demo renders play: the tests load each preset and check it gives the same values and the
same samples as the demo's list.

## Clocking from the host

- A step starts on every multiple of Clock Div counted from the song's top, while the host plays (swung: every second
  one later by Swing). Play from the top starts on step 1, on its first frame; each later step lands on its frame. A
  Play or a jump places the sequence by the song's place: the clock's number from the top, counted round Length.
- Stop stops the sequence (sounds ring out). Play again starts from where the song is.
- A jump, or a loop going round, lines the sequence up with the song's place again. A wrap that falls inside a block
  still plays that step, at the start of the next block (at most a block late: 2.7 ms at 128 frames).
- Run off: the host plays but the sequence holds its step and stays silent; Advance and Trigger work as on the panel.
  Run on again plays the step it is on at once and goes on from there.
- In Melodee, keep the plug-in's **TRANSPORT** on (the default): with it off the plug-in never hears Play.

## Where values were inferred

The DFAM manual documents the routing and several ranges (envelope attacks, VCF decay 10 ms – 10 s, cutoff 20 Hz –
20 kHz, step pitch ±5 V). It does not give these, so they are chosen to match what is documented:

- VCO range: 10 octaves, centred so a deep bass sits a little left of the middle.
- VCO and VCA decay ranges: the same as the VCF's (the same circuit).
- EG depths: ±8 octaves (the EG's 8 V into 1 V/octave).
- FM depth: index 3 at full (VCO 2 swings by 3 times its frequency; it stops at 0 Hz rather than going through zero).
- Noise/VCF Mod: up to 4 octaves.
- Velocity also lengthens the decays, by up to ±10 % ("slightly longer" in the manual).

Left out on purpose: the patch bay and the external audio input, and the panel's audio-rate tempo (the clock is the
host's; 1/64 is the fastest step).

## Cost

One voice, run at twice the host's rate (so FM and sync stay clean) and filtered back down with a half-band FIR.
Instructions a 128-frame block takes, from `tools/bench.sh` (callgrind):

| Case | Instructions | Share of one A55 core at 1 instruction a cycle |
|---|---|---|
| Everything busy (FM, its envelope, sync, pink noise on the cutoff, resonance, the VCA never closing) | about 82,000 | 1.7 % |
| A short percussive pattern | about 77,000 | 1.6 % |
| Stopped | under 600 | 0.01 % |

While the VCA is closed between hits the oscillators and filter are skipped. The voice's two samples a frame, and the
synced oscillator's step, are compiled into the frame's loop rather than called (a fifth fewer instructions than
before, the same output to the bit). The real share on the MPC depends on
how many instructions the A55 retires a cycle on this code; measure it with Melodee's plug-in load figure.

## Build and test

Needs Docker with a Linux arm64 context (Melodee's `colima-melodee` and its `melodee-builder` image by default).

```sh
./build.sh          # build/Anvil.clap, then the tests (tests/host.cpp), the sound's fingerprint and demo renders build/demo-*.wav
./build.sh bench    # build/bench
tools/bench.sh      # instruction counts (Melodee's melodee-profiler image)
```

The tests load the plug-in through the CLAP API and check: description and ports; every parameter's range, text and
default; state save and load; the presets as preset discovery lists them and preset-load loads them; step timing to the frame at 44.1, 48 and 96 kHz and two tempos; stop, restart, jumps,
a loop wrapping inside a block, a host without a song position; the same output whatever the block size (1, 37,
128, 4096 frames); Trigger, Advance, Run and MIDI notes; Length (a cycle of 3, Advance wrapping in it) and Swing (66 and
75 %, and a jump between a step's grid line and its late clock); velocity and tails; VCO pitch, step pitch on each Seq Pitch
Mod setting; the new FM control both ways; and finite, bounded output at 60 random and extreme settings.

`tests/fingerprint.cpp` then renders 46 fixed scenarios (each preset, the bench's patches, 24 random patches with the
buttons, MIDI and knobs moving, other block sizes and rates, stop and jumps, Length and Swing) and hashes every output sample's bits
against `tests/fingerprint.txt`: work meant to keep the sound, such as a speed-up, must leave every hash as it is. A
change meant to change the sound updates them with `make fingerprint-update` (run inside the build image). The
hashes hold for Melodee's build image and these compiler flags.

## Install

Copy `build/Anvil.clap` to `Melodee/Plugins/hdslave Anvil/hdslave Anvil.clap` on the card (or the staged card's
folder), then pick it on an empty track: Stage, Plugin, hdslave Anvil. It needs only libc and libm (glibc 2.35 or
older).
