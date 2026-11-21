# Anvil on the MPC Sample

The chrome ANVIL, warped checker ribbon and violet wire ring form one static print at native resolution. The eight-step score carries the motion. An ivory bracket marks the step selected for editing. Lime marks the running sequence position. A small spark in the header responds to actual hit-count changes, including auditions and ratchets. A chance-suppressed step can advance the playhead without firing the spark.

## Playing and editing

- **STEPS** shows all eight signed pitch offsets and velocities. The final shelf of each stair gives its quantized pitch. The axis opens in octaves as needed, up to ±60 semitones. The height of each striped peak gives its velocity. Steps beyond Length stay visible in dim ink and remain available to pick and audition.
- **Wheel** picks a step. **Push** auditions it. **K1** turns its pitch, **K2** its velocity and **K3** Cutoff. Minus and Plus switch sections.
- On the six list pages the wheel selects a row. Push toggles editing or presses Trigger or Advance. In an edit, the wheel moves continuous values by 1/128 of the range, or 1/1024 with Shift. Minus and Plus move them by 1/16. Choices and integers move one per detent.
- K1–K3 follow the selected row and the two below it. Buttons have no pot binding. Each card carries the host track colour, and a hollow marker locates a pot waiting for pickup. Shift shows the current controls.

Each list keeps a small pitch and velocity score on the right. VCO shows the two selected oscillator shapes. MIX shows the three mixer gains. VCF and VCA sketch a full-velocity envelope starting from silence over a one-second window. PATCH shows the three signed routing depths. SEQ marks the swing split against the straight midpoint.

## Reproducing the previews

From the checkout root, run the build and capture the actual plug-in in Melodee. The host executable comes from the adjacent hdslaveOS checkout (`../melodee-hdslave`, branch `hdslave`), mounted read-only; set `MELODEE_SCREEN_TREE` to use another. The capture script uses the running Colima `melodee` profile and `melodee-builder` image.

```sh
./build.sh
tools/ui_preview/melodee
tools/ui_preview/run --render
tools/ui_preview/run --test
tools/ui_preview/run --bench
```

The native C++ fixture loads factory patches and supplies the clock and hit state. The gallery uses Python and Pillow. The launcher also supports a uv environment with Pillow. The art generator can be run separately with `python3 tools/ui_preview/art.py`.

[Playing loop](../assets/previews/anvil-playing.gif) · [RGB565 contact sheet](../assets/previews/contact-sheet-2x.png) · [Step strip](../assets/previews/step-strip.png) · [Live Melodee Edit](../assets/previews/melodee/1.png) · [Live Stage](../assets/previews/melodee/10.png) · [Host log](../assets/previews/melodee/host.txt)

## Measurements and hardware status

Mean time per update and paint across 1,200 iterations. The Mac used an M4 Max and native Apple Clang at `-O3`. Linux used the arm64 Colima VM on the same Mac, with GCC at `-O3 -mcpu=cortex-a55`. The full-paint run redraws a settled frame. The partial runs count actual pixel differences.

| Operation | Mac | Linux VM | Peak changed pixels |
|---|---:|---:|---:|
| Whole STEPS page | 0.021 ms | 0.025 ms | 0 on an identical frame |
| Step advance | 0.013 ms | 0.015 ms | 1,614 |
| Idle | <0.001 ms | <0.001 ms | 0 |
| VCO cursor move | 0.011 ms | 0.012 ms | 6,473 |

The plug-in build passed its host tests and all 60 sound fingerprints against the clock-fix revision. The preview tests cover the seven groups, selected-step bindings and audition, the hit spark settling, full redraws with padded stride, partial redraw equivalence and idle paints touching nothing. The screen uses the existing engine interface and fixed drawing buffers.

The physical MPC Sample has yet to be tested. LCD legibility under ambient light, SPI update time and hands-on control feel remain hardware checks.
