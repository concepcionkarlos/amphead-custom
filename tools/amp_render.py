#!/usr/bin/env python
"""
Render a REAL audio file through the plugin and write the result to disk.

Why this exists: every test in amp_test.py feeds the amp a synthetic plucked
note with bypass_ir=True and gate_on=False. That is not the signal path anyone
actually listens to. When the player reports an audible defect the harness
cannot see, the first thing to check is whether the harness has ever measured
the path he is hearing - and it had not.

    ./.venv/bin/python tools/amp_render.py ga.wav --channel 2 --out out.wav
    ./.venv/bin/python tools/amp_render.py ga.wav --channel 2 --set bypass_ir=False
    ./.venv/bin/python tools/amp_render.py ga.wav --preset his --blocksize 64

--set takes any plugin parameter, INCLUDING bypass_ir and gate_on, which
amp_test.py hard-codes.
"""
import argparse
import sys

import numpy as np
import soundfile as sf

sys.path.insert(0, "tools")
from amp_test import make_plugin, SR  # noqa: E402
import amp_state  # noqa: E402

# The patch he actually plays, decoded from his saved standalone state.
# Measuring anything else and calling it "his tone" is guesswork.
HIS_PATCH = dict(gain=10.0, character=10.0, bass=0.41, mid=1.0, treble=0.66,
                 presence=1.0, master=1.0, output_db=-4.9, bright=True,
                 oversampling="x8", gate_on=False, bypass_ir=True,
                 reverb_mix=0.0, delay_mix=0.0, mod_chorus=0.0, mod_detune=0.0)


def parse_set(pairs):
    """--set key=value, with the value typed by what it looks like."""
    out = {}
    for p in pairs or []:
        if "=" not in p:
            raise SystemExit(f"--set needs key=value, got {p!r}")
        k, v = p.split("=", 1)
        if v.lower() in ("true", "false"):
            out[k] = (v.lower() == "true")
        else:
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v          # choice parameters like "x8" stay strings
    return out


def load_mono(path):
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    if sr != SR:
        raise SystemExit(f"{path} is {sr} Hz; the harness is fixed at {SR} Hz")
    return x.mean(axis=1).astype(np.float32), sr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("--channel", type=int, default=2)
    ap.add_argument("--out", default=None)
    ap.add_argument("--preset", choices=["his", "default"], default="his")
    ap.add_argument("--set", action="append", metavar="KEY=VAL")
    # 512 is the real buffer size of his Volt 2, from his saved audioSetup.
    # Block size is a diagnostic here, not a detail: any parameter recomputed
    # once per block steps at the block rate, so an artefact that moves when
    # this moves is being driven by block-rate modulation.
    ap.add_argument("--blocksize", type=int, default=512)
    ap.add_argument("--seconds", type=float, default=None,
                    help="render only the first N seconds")
    # The IR stage is the one part of the chain no test has ever measured, because
    # parameters cannot load a file. This injects his saved state with the IR paths
    # rewritten to files that exist, which is the only way to render the path he
    # actually hears.
    ap.add_argument("--ir-a", default=None)
    ap.add_argument("--ir-b", default=None)
    args = ap.parse_args()

    base = dict(HIS_PATCH) if args.preset == "his" else {}
    base.update(parse_set(args.set))

    sig, _ = load_mono(args.infile)
    if args.seconds:
        sig = sig[:int(SR * args.seconds)]

    p = make_plugin(args.channel, **base)

    if args.ir_a:
        p.raw_state = amp_state.his_state_with_irs(args.ir_a, args.ir_b or args.ir_a)
        # raw_state replaces EVERY parameter, so the channel and any --set have to
        # be re-applied on top of it or the render silently measures his saved patch
        # instead of the one asked for.
        for k, v in base.items():
            if k in p.parameters:
                try:
                    setattr(p, k, v)
                except Exception:
                    pass
        try:
            p.channel = list(p.parameters["channel"].valid_values)[args.channel]
        except Exception:
            pass
        print("IR A:", args.ir_a)
        print("IR B:", args.ir_b or args.ir_a)

    # Report back what the plugin ACTUALLY took. make_plugin swallows unknown
    # or unsettable keys silently, so without this a typo reads as a result.
    applied = {}
    for k in base:
        if k in p.parameters:
            applied[k] = getattr(p, k)
    print("patch applied:", ", ".join(f"{k}={v}" for k, v in sorted(applied.items())))
    missing = [k for k in base if k not in p.parameters]
    if missing:
        print("WARNING - these keys are not plugin parameters and were IGNORED:", missing)

    out = p(sig.reshape(1, -1), SR, buffer_size=args.blocksize, reset=True)
    out = out.mean(axis=0) if out.ndim > 1 else out

    dest = args.out or f"{args.infile.rsplit('.', 1)[0]}_ch{args.channel}_out.wav"
    # The input is written alongside, trimmed to the same length, so the
    # analysis step never has to guess how the two were aligned.
    sf.write(dest, out.astype(np.float32), SR)
    n = min(len(sig), len(out))
    sf.write(dest.replace(".wav", "_IN.wav"), sig[:n].astype(np.float32), SR)

    print(f"in  {len(sig)/SR:6.2f} s  peak {20*np.log10(np.abs(sig).max()+1e-12):+6.1f} dBFS")
    print(f"out {len(out)/SR:6.2f} s  peak {20*np.log10(np.abs(out).max()+1e-12):+6.1f} dBFS")
    print(f"wrote {dest}")


if __name__ == "__main__":
    main()
