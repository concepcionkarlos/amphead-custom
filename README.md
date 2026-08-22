# AmpHead Custom

[![Build](https://github.com/concepcionkarlos/amphead-custom/actions/workflows/build.yml/badge.svg)](https://github.com/concepcionkarlos/amphead-custom/actions/workflows/build.yml)

A three-channel guitar amplifier head, written in C++ with JUCE. VST3 / AU / standalone.

The interesting part of this project is not the amp. It is that the amp is **measured**
rather than dialled in by ear alone: there is an offline harness that renders test signals
and real playing through the compiled VST3 and reports numbers for the things that are hard
to hear and easy to get wrong.

---

New to the code? Start with [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) - it maps every
stage to the struct and line that implements it.

---

## Signal chain

```
Noise gate -> Preamp -> Tone stack -> Graphic EQ -> Phase inverter -> Power amp
           -> Output transformer -> Reactive load -> Speaker sim -> Cabinet IR
           -> Delay -> Modulation -> Reverb -> Output
```

**Preamp.** Three cascaded gain stages plus a cathode follower. V1A splits into three bands
and saturates each separately; V1B is a cold-biased stage; V1C is brought in by the top of
the GAIN control with interstage attenuation, which is how a real 4+ stage design keeps each
stage driven rather than slammed. Cathode-bypass and grid-conduction behaviour are modelled
per stage.

**Tone stack position is switchable.** In `POST` the tone controls colour the finished
distortion. In `MARK` the stack sits ahead of the preamp, the way a Mesa Mark does, so the
tone controls decide what gets distorted. Measured side effect worth knowing: `MARK` is far
more level-stable, swinging 0.8 dB across tone settings where `POST` swings 7.8 dB.

**Graphic EQ.** Five bands, Mark-series style. The sliders are labelled 80 / 240 / 750 /
2200 / 6600 because that is what players call them, but the filters run the measured
resonances of the real circuit: 87.61, 371.74, 723.43, 1575.87 and 4822.88 Hz. The "240"
band is really at 372 Hz.

**Phase inverter.** A long-tailed pair: two plates in opposition, each with its own knee, and
a shared tail term so one side starves as the other conducts. The asymmetry *between* halves
is what a single-ended stage cannot produce.

**Power section.** Push-pull with sag, negative feedback, resonance and a switchable
rectifier (silicon or tube).

**Cabinet.** Two IR slots with a crossfade, or head-only mode when no IR is loaded.

Nonlinear stages run oversampled at a selectable 1x / 2x / 4x / 8x, with an eighth-order
Butterworth anti-imaging and anti-alias filter on each end.

---

Every push is built on both macOS and Windows by GitHub Actions, so "it builds on Windows"
is something you can check rather than something I claim. The DSP and the interface contain
no platform-specific code.

---

## Measurement harness

`tools/` contains a Python harness that loads the built VST3 through
[pedalboard](https://github.com/spotify/pedalboard) and renders offline, so DSP claims can be
checked instead of assumed.

| Tool | What it does |
|---|---|
| `amp_test.py` | Suite: note swell, pick dynamics, aliasing, harmonic series, channel level match |
| `amp_render.py` | Renders a real audio file through the plugin, with any parameter overridden and cabinet IRs loaded |
| `cut_detect.py` | Finds genuine gate artefacts on real playing, and reports what the amp does to playing dynamics |
| `amp_state.py` | Builds a VST3 state blob, which is the only way to load cabinet IRs from a script |
| `ir_persist_test.py` | Regression test: a stale saved IR path must never destroy a loaded cabinet |

```
./.venv/bin/python tools/amp_test.py
./.venv/bin/python tools/amp_render.py guitar.wav --channel 2 --out out.wav
./.venv/bin/python tools/cut_detect.py out.wav out_IN.wav
```

### Why it earns its place

Three defects in this project were found by measurement after passing by ear, and two of them
were invisible to the tests until the tests themselves were fixed:

- **The aliasing test could not fail.** It probed at 3000 Hz against a 48 kHz sample rate.
  Since 48000 = 16 x 3000, every alias image landed on a harmonic bin, and the test was
  reading its own noise floor: it returned the same value at 1x and 8x oversampling. Moving
  the probe to 3050 Hz exposed a channel that was 6 dB past spec.
- **The swell test compared the output to itself**, so a 5.6 dB gain rise rode underneath the
  note's own decay and reported as +0.09 dB. Measuring output *minus input* found it.
- **A note-cut artefact** traced to an unbounded envelope adding a DC offset into a stage
  whose asymptote was smaller than the offset. It pinned the stage, then took the envelope's
  600 ms release to let go.

The lesson each time: a metric that never moves is broken, not clean.

---

## Building

Two ways, and they build the same sources.

**CMake** - cross-platform, no IDE, and what CI uses. JUCE is fetched at a pinned version:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

**Projucer** - the local macOS workflow. `JuceLibraryCode/` is generated, not committed, so
open `CopilotTone.jucer` in Projucer and save it once first:

```
cd Builds/MacOSX
xcodebuild -project "CustomAmpHead JConcepcion.xcodeproj" \
           -target "CustomAmpHead JConcepcion - All" \
           -configuration Release build
```

For the harness:

```
python3 -m venv .venv
./.venv/bin/pip install pedalboard numpy scipy soundfile
```

---

## License

All rights reserved - see [`LICENSE`](LICENSE). This repository is published so the source
can be read, not reused. You are welcome to clone it and build it to try the plugin; you
are not licensed to use the source in anything else.

The ideas are not claimed, only this expression of them.

---

## History

Started in May 2026 and developed locally for the first three months, which is why the
history here begins with an import rather than the first line of code. Published in August
2026.

---

## Status

Prototype. The amp is voiced and the suite passes, but this is a personal instrument rather
than a finished product: presets, tempo-synced delay and a proper passive tone-stack transfer
function are not implemented.
