# Notes for coding agents

**hdslave Anvil** is a DFAM-style percussion synth with an 8-step sequencer on the host's clock: a CLAP plug-in for the Akai MPC Sample. It is made to run in hdslaveOS, the hdslave fork of Kerem Kiliç's Melodee. Its CLAP id is `com.hdslave.anvil`, and it builds to `build/Anvil.clap`.

## Read the workspace guide first

On the author's machine, this repository sits in one folder beside the other hdslave projects. That folder's `../AGENTS.md` and `../Workflow/` hold the shared workflow:
- building in the Linux VM;
- what hdslaveOS does to plug-ins;
- screen pages;
- reviews;
- publishing, the hub repository and staging for the SD card.

Read `../AGENTS.md`, then `../Workflow/plugins.md` and `../Workflow/host-facts.md`, before you change anything. The rules below hold either way.

## Build and test

- `./build.sh` builds the plug-in in a Linux arm64 container (image `melodee-builder`, Colima VM `melodee`). It runs `make all test`: the host tests in `tests/host.cpp` and the sound's fingerprint. Pass make targets to run others.
- `tools/bench.sh` counts the instructions of a 128-frame block with callgrind. A block's budget is about 4.8 M cycles on one of the MPC's Cortex-A55 cores; Anvil's busiest block takes about 82k instructions of it (README, "Cost").
- `./build.sh sanitize` runs the tests and the fingerprint's scenarios under AddressSanitizer and UBSan.
- Check the build's own exit code: `./build.sh | grep …` hides a failure.

## Rules

- **Fixed for saved projects.** Parameter ids, preset keys and choice orders never change once released, because saved projects and presets refer to them.
  - Add new ones at the end.
  - Ranges stay as they are too: the host stores bindings normalized to the range.
- **The sound is held to the bit** by `tests/fingerprint.txt`.
  - A refactor or speed-up must leave every hash unchanged. With `-ffp-contract=fast`, even reshaping a loop can change the last bit, so check after every edit.
  - Only a change meant to change the sound runs `make fingerprint-update`, and its commit says which hashes moved and why.
- **Real-time and determinism.** The output is the same at every block size. No allocation, locks or file I/O on the audio thread.
- **Notes** must survive how hdslaveOS sends them:
  - every MIDI event of a block arrives at frame 0;
  - a re-pressed key arrives as on, on, off;
  - CC 120 and 123 arrive at 0 on every channel when the host silences.
- **Commits** are made as `hdslave <111106843+hdslave@users.noreply.github.com>`, with Conventional Commits. Never push, publish or stage for the card without the owner's word.
- **Other agents' work.** Others may be working here. Check `git status` and `git worktree list`, and never edit, commit or reset files you did not change.
- **Names and art.** No trademarked names in the plug-in's names, presets, UI text or art. Art is original; fonts are OFL, with their licences in `licenses/`.
- **Read the README** before you change the sound. It lists the sources, the inferred values and the deliberate deviations. Keep it current: version, costs, and "not yet run on the MPC" until it has been.

## This plug-in

- `src/params.h` holds the version and the parameter table, `src/engine.h` the DSP and sequencer, `src/presets.h` the factory presets. The page is `src/screen.h` plus `src/ui/`.
- Presets: `presetTable` is the list hosts show, in order, and it only grows: Default and the ten of 0.2-0.7, then the fifteen FM presets of 0.8.0. Never remove a released preset, change its values or reuse its key. A new preset goes at the end, gets a `preset-<key>` fingerprint scenario, and joins the demo list in `tests/host.cpp` (it checks the values and writes `build/demo-<key>.wav`).
- `src/ui/page.h` is Anvil's own (its own `ink::` palette, `identity.h` included): it is not shared with any sibling, and must not be overwritten from one.
- The fingerprint's random scenarios draw values for fixed parameter lists (`order01`, `order02`…). Add a new list when adding parameters, so the old hashes hold.
- The clock code (song-anchored steps, a late loop wrap, Run with Play, Chance and Ratchet) has a test for each case in `tests/host.cpp`. Keep them passing.
- Pads (MIDI notes): the drum map in `src/anvil.cpp` (`padMapJson`, kept by `padsJson()`) makes hdslaveOS send pads A01-A16 as 36-51 whatever the octave, and − / + move the bank (16 keys a bank). Where the pads are steps (Run on, or Notes: Steps) a key is read by `padOf()` (`src/params.h`): slot (key-36) mod 16, bank clamped to -2..+3, an octave a bank; the pad plays its slot's step (slot mod 8) an octave a bank up or down, without moving the sequence, stopped or playing. With Run off and Notes: Pitch (index 0, once called Trigger) every key plays its own pitch chromatically from note 36 = the Freq knobs, clamped to a step's ±5 octaves, exactly as 0.6.8 did: the bank never re-maps it (the user's choice, 2026-11-27). `testDrumMap` and `testPads` hold it. A choke, or MIDI's All Sound Off (CC 120 at 0), fades the hit sounding out in 1.5 ms (its key's, or key -1's / All Sound Off anything) and drops its Ratchet hits; the fade is on the output, not in the envelopes' per-sample loop, which must stay as it is (0.6.0's 81,827 a block). Note-offs, CC 121 and CC 123 change nothing. `testPads` and `testChokes` in `tests/host.cpp` hold it.

## Release packaging

Every release ZIP includes the full repository `LICENSE.txt` beside the third-party notices, outside signed plug-in bundles. Preserve both when packaging the plug-in for another project.
