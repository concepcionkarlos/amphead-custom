# Architecture

A map of the codebase: where everything lives, what each stage does, and why it is built
that way. Read this before the source and nothing in `PluginProcessor.cpp` will be a surprise.

The whole plugin is four files. There is no framework of our own, no dependency injection,
no plugin system — one processor, one editor, and a Python harness that measures the result.

```
Source/PluginProcessor.h     structs and state for every stage
Source/PluginProcessor.cpp   the DSP itself
Source/PluginEditor.h/.cpp   the interface, drawn by hand
tools/                       offline measurement harness (Python)
```

---

## The signal chain

Audio enters `CopilotToneAudioProcessor::processBlock`
(`PluginProcessor.cpp:2256`) and leaves it 13 stages later. Guitar is mono, so every input
channel is summed into channel 0 first and the cabinet stage creates the stereo image at the
end.

| # | Stage | Struct | `.h` | `.cpp` process |
|---|---|---|---|---|
| 1 | Noise gate | `NoiseGate` | 535 | 1454 |
| 2 | Preamp | `AmpEngine` | 232 | 210 |
| 3 | Tone stack | `ToneStack` | 351 | 653 |
| 4 | Graphic EQ | `GraphicEQ` | 134 | inline in `processBlock` |
| 5 | Power amp | `PowerAmp` | 374 | 772 |
| 6 | Output transformer | `OutputTransformer` | 441 | 1018 |
| 7 | Reactive load | `ReactiveLoad` | 420 | 944 |
| 8 | Speaker sim | `SpeakerSim` | 466 | 1120 |
| 9 | Cabinet IR | `IRSection` | 492 | 1304 |
| 10 | Delay | `Delay` | 552 | 1555 |
| 11 | Modulation | `Modulation` | 603 | 1871 |
| 12 | Reverb | `ReverbEngine` | 578 | 1720 |
| 13 | Output gain | — | — | in `processBlock` |

Every stage follows the same shape: `prepare()` computes coefficients once when the sample
rate is known, `process()` runs on the audio thread and never allocates, and `reset()` clears
state. If you are looking for a constant, it is computed in `prepare()`; if you are looking
for behaviour, it is in `process()`.

---

## Support structures

These are not stages. They are used by the stages above.

**`OsBiquadChain`** (`.h:19`) — eighth-order Butterworth lowpass, four cascaded biquads.
Used as the anti-imaging filter going into an oversampled block and the anti-alias filter
coming out. Both `AmpEngine` and `PowerAmp` own a pair.

**`TubeThermal`** (`.h:211`) — one scalar, `temp`, pinned to 1. Everything modelling a hot
valve is scaled by it, so setting it to 0 restores the pre-thermal voicing in one place
instead of thirty.

**`GraphicEQ`** (`.h:134`) — five peaking biquads at the Mark series' measured resonances.

**`TouchRestore`** (`.h:93`) — present but disabled. Kept with its reasoning because the idea
looks correct and is not; see the comment at `kTouch` in `processBlock`.

---

## Oversampling

The nonlinear stages alias, so `AmpEngine` and `PowerAmp` run their saturating sections at
1x, 2x, 4x or 8x the host rate, chosen by the `osFactor` parameter (default 4x).

The pattern in both:

1. Zero-stuff by the factor, scale by the factor to keep the level.
2. Filter with `OsBiquadChain` to remove the images the zero-stuffing created.
3. Run the nonlinear stages at the higher rate.
4. Filter again to remove what the nonlinearity generated above the host Nyquist.
5. Keep one sample in every N.

Coefficients for all four factors are precomputed in `buildOsTables()` during `prepare()`.
`selectOs()` copies one row into the active set. Nothing transcendental runs on the audio
thread, and switching the factor is a table lookup.

**Rule to preserve:** any coefficient used *inside* an oversampled loop must be built at the
oversampled rate. Building one at the host rate multiplies its corner frequency by the factor.
This is not hypothetical — a DC tracker built at the host rate ran at 12 Hz at 1x and 96 Hz
at 8x before it was tabled.

---

## Preamp detail

`AmpEngine::process` is the longest function in the project. It runs in this order:

1. **Front end at host rate** — DC blocker, cable capacitance, input high-pass, transient
   smoothing, pick detection. Sets `v1aDrive` and `v1bDriveEff` once per block.
2. **Upsample.**
3. **V1A** — the signal is split into three bands and each is saturated separately, then
   recombined. Deliberately articulate rather than physical.
4. **Hot wideband triode** — crossfades from that band-split toward a single smooth curve over
   the whole signal, which restores the intermodulation the split removes.
5. **Bloom and interstage** — envelope, coupling high-pass, grid-stopper low-pass.
6. **V1B** — cold-biased stage, with grid conduction and cathode bypass.
7. **V1C** — third gain stage, faded in by the top of the GAIN knob with interstage
   attenuation.
8. **DS-2 grind** — a hard clip blended in as seasoning on CRUNCH and LEAD only.
9. **Downsample**, then the cathode follower and the voicing filters at host rate.

Per-channel constants are arrays indexed `[0] = CLEAN, [1] = CRUNCH, [2] = LEAD`. That
indexing is used everywhere; `ci` is always the channel index.

---

## Parameters

All parameters live in `createParameterLayout()` (`PluginProcessor.cpp`) and are read in
`processBlock` through `apvts.getRawParameterValue`. The editor never talks to the DSP
directly — it attaches to the same parameter tree.

Two parameters change topology rather than tone:

- **`stackPos`** — `Post` puts the tone stack after the preamp, `Mark` puts it before, so the
  tone controls shape what gets distorted instead of colouring the result.
- **`rectifier`** — `Silicon` is stiff, `Tube` sags.

---

## Editor

`PluginEditor.cpp` draws everything by hand through a single `AmpLookAndFeel`. Layout
constants are declared identically at the top of `paint()` and `resized()`; if you move a
panel you must change both.

There is a type scale in the `CT` namespace (`fMicro` through `fBrand`). Use it. Nothing
should be smaller than `fMicro`.

Note that `drawText` **clips** when the text does not fit, while `drawFittedText` shrinks.
Most of the editor uses `drawText`, so growing a font means growing its rectangle too.

---

## Measurement harness

`tools/` loads the compiled VST3 through pedalboard and renders offline. It exists because
the failure modes here — a note swelling, dynamics flattening, a stage cutting out — are hard
to hear reliably and trivial to see in numbers.

Start with `tools/amp_test.py` for the suite and `tools/cut_detect.py` for artefacts on real
playing. `README.md` lists them all.

**The habit worth copying:** when a measurement does not move, suspect the measurement. Two
tests in this project silently passed for weeks because one probed at a frequency that hid
the defect and the other compared a signal to itself.

---

## Constraints

- No allocation, file access, locking or UI work inside `processBlock`.
- ASCII only in source.
- `JuceLibraryCode/` is generated. Do not edit it, do not commit it.
- The Projucer project and the CMake build compile the same sources. Changing the file list
  means changing both.
