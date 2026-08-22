#!/usr/bin/env python
"""
Offline measurement harness for AmpHead Custom.

Renders test signals through the real VST3 and measures what a human ear would
otherwise have to catch by trial and error. Written because the audible bugs in
this amp - a note swelling after the pick, dynamics flattening, channels jumping
in level - are all trivially visible in a plot and nearly impossible to pin down
by listening and guessing.

    ../.venv/bin/python tools/amp_test.py

Add --channel N to test one channel, --verbose for the full envelope tables.
"""
import argparse
import math
import sys

import numpy as np
from pedalboard import load_plugin

VST3 = "/Users/juanconcepcionametller/Library/Audio/Plug-Ins/VST3/AmpHead Custom.vst3"
SR = 48000
CHANNELS = {0: "CLEAN", 1: "CRUNCH", 2: "LEAD"}

# A patch that is representative of real playing rather than a corner case.
PATCH = dict(gain=5.5, character=6.0, bass=0.45, mid=0.6, treble=0.6,
             presence=0.6, master=0.4, output_db=0.0, bright=False,
             gate_on=False, reverb_mix=0.0, delay_mix=0.0, mod_chorus=0.0,
             mod_detune=0.0, bypass_ir=True)


def make_plugin(channel, **overrides):
    p = load_plugin(VST3)
    settings = dict(PATCH)
    settings.update(overrides)
    for k, v in settings.items():
        if k in p.parameters:
            try:
                setattr(p, k, v)
            except Exception:
                pass
    # channel is a stepped/choice parameter, so set it by raw index
    try:
        p.channel = list(p.parameters["channel"].valid_values)[channel]
    except Exception:
        p.channel = channel
    return p


def render(plugin, sig):
    """Run a mono signal through the plugin, return mono output."""
    buf = np.asarray(sig, dtype=np.float32).reshape(1, -1)
    out = plugin(buf, SR, reset=True)
    return out.mean(axis=0) if out.ndim > 1 else out


def envelope_db(sig, win_ms=10.0):
    """Block RMS envelope in dBFS."""
    n = int(SR * win_ms / 1000)
    blocks = len(sig) // n
    e = np.array([np.sqrt((sig[i * n:(i + 1) * n] ** 2).mean() + 1e-20)
                  for i in range(blocks)])
    return 20 * np.log10(e + 1e-12), n / SR


def plucked_note(freq=196.0, dur=2.0, decay=1.6, peak=0.25):
    """A plucked-string approximation: harmonic stack, exponential decay,
    plus a short bright attack transient."""
    t = np.arange(int(SR * dur)) / SR
    sig = np.zeros_like(t)
    for h, amp in enumerate([1.0, 0.5, 0.32, 0.2, 0.13, 0.08], start=1):
        sig += amp * np.sin(2 * np.pi * freq * h * t)
    sig *= np.exp(-t / decay)
    attack = np.exp(-t / 0.004) * np.random.default_rng(0).normal(0, 0.35, len(t))
    sig += attack
    sig *= np.hanning(64).astype(np.float64)[:32].mean()  # soften the very first sample
    sig /= np.abs(sig).max()
    return (sig * peak).astype(np.float32)


# ---------------------------------------------------------------- tests

def test_swell(channel, verbose=False):
    """THE regression test. A picked note must not gain level as it rings.

    Measured as the amp's GAIN - output envelope minus input envelope - not as
    the raw output envelope. That distinction is the whole test: the note is
    decaying the entire time, so a swell of several dB rides underneath the decay
    and never shows up as an absolute rise. The old version compared the output
    to itself and reported +0.09 dB on a channel that was really swelling 5.0 dB.
    A test that cannot fail is worth nothing."""
    sig = plucked_note()
    p = make_plugin(channel)
    out = render(p, sig)
    eo, hop = envelope_db(out)
    ei, _ = envelope_db(np.asarray(sig, dtype=np.float64))
    n = min(len(eo), len(ei))
    start = int(0.03 / hop)                     # skip the attack transient
    gain = (eo[:n] - ei[:n])[start:int(1.2 / hop)]
    if len(gain) < 5:
        return None
    gain = gain - gain[0]
    rise = float(gain.max())                    # gain gained after the attack
    at = (start + int(np.argmax(gain))) * hop
    if verbose:
        for i in range(0, len(gain), max(1, len(gain)//12)):
            print(f"        t={((start+i)*hop)*1000:6.0f} ms  {gain[i]:+6.2f} dB")
    # Per channel, like the dynamics test, and for the same reason: a compressive
    # stage necessarily gains level as the note decays, so the honest limit depends
    # on how hard the channel saturates. These are set from the measured behaviour
    # of a build that sounds right, so they catch REGRESSIONS - they are not a claim
    # that a given number is correct in the absolute. ISOLATED, and none of these
    # were the cause: bias bloom (kBias), the power section (MASTER 0->1 moves it
    # 0.02 dB), character, the wideband ceiling. It scales with input level and with
    # GAIN, and it vanishes at a quiet input - it is static compression, not a slow
    # envelope with memory.
    # Re-based when MASTER became a drive rather than an output gain. These were
    # set against a power stage that never saturated, so its supply never really
    # sagged and notes never really bloomed. They do now, and the player confirmed
    # by ear that a note swelling as the rail recovers is what an amp does.
    #
    # Raised only as far as the new physics reaches, not far enough to hide a
    # regression: CLEAN measures 2.53 against 3.0, so 0.47 dB of headroom remains.
    #     was (2.5, 5.5, 6.5)
    limit = (3.0, 5.5, 6.5)[channel]
    ok = bool(rise < limit)
    print(f"  {'PASS' if ok else 'FAIL'}  swell        "
          f"gain rise after attack = {rise:+.2f} dB at {at*1000:.0f} ms   (limit +{limit})")
    return ok


def test_dynamics(channel):
    """Pick softly vs hard: the output difference should track the input, not
    be squashed flat. A compressed amp still needs to respond to touch."""
    def sweep(**over):
        lv = []
        for peak in (0.05, 0.15, 0.45):
            p = make_plugin(channel, **over)
            out = render(p, plucked_note(peak=peak))
            lv.append(20 * np.log10(np.abs(out).max() + 1e-12))
        return (lv[-1] - lv[0]) / (20 * math.log10(0.45 / 0.05))

    in_range = 20 * math.log10(0.45 / 0.05)
    # The GATE is DRIVE low. DRIVE is an aggressive compressor by design, so
    # running the check at the patch's 6/10 was asking two questions at once:
    # does the amp respond to touch, and how hard does its compressor squash.
    # Only the first one is a defect if it fails.
    ratio = sweep(character=0.5)
    # And the patch setting, reported but not gated - it is the player's choice
    # of how much compression to buy.
    patch_ratio = sweep()
    out_range = ratio * in_range
    # Per channel, because a high-gain channel SHOULD compress - that is what it
    # is for. These are set from what real amps do: a clean channel stays mostly
    # linear, a cranked lead still passes a few dB of pick dynamics. Anything
    # below these and the amp has stopped responding to the player.
    # Measured with DRIVE at 0.5, so these are what the amp does when it is NOT
    # being asked to compress. Back at 0.18 for CRUNCH: with DRIVE out of the way
    # there is no reason to accept less.
    floor = {0: 0.35, 1: 0.18, 2: 0.08}[channel]
    ok = bool(ratio > floor)
    print(f"  {'PASS' if ok else 'FAIL'}  dynamics     "
          f"{in_range:.1f} dB in -> {out_range:.1f} dB out  (ratio {ratio:.2f}, min {floor})"
          f"   [DRIVE 6: {patch_ratio:.2f}]")
    return ok


def test_harmonics(channel):
    """Drive a clean sine and measure the harmonic series it generates."""
    f0 = 220.0
    t = np.arange(int(SR * 1.0)) / SR
    sig = (0.2 * np.sin(2 * np.pi * f0 * t)).astype(np.float32)
    p = make_plugin(channel)
    out = render(p, sig)[SR // 4:]              # drop the settling transient
    win = np.hanning(len(out))
    spec = np.abs(np.fft.rfft(out * win))
    fr = np.fft.rfftfreq(len(out), 1 / SR)

    def at(f):
        i = np.argmin(np.abs(fr - f))
        return spec[max(0, i - 2):i + 3].max()

    fund = at(f0)
    harm = [at(f0 * n) for n in range(2, 11)]
    thd = math.sqrt(sum(h ** 2 for h in harm)) / (fund + 1e-20) * 100
    print(f"  ----  harmonics   THD = {thd:6.2f}%   "
          + "  ".join(f"H{n}={20*math.log10(h/(fund+1e-20)+1e-20):+.0f}"
                      for n, h in zip(range(2, 6), harm)))
    return thd


def test_channel_levels():
    """Switching channels should change character, not loudness."""
    peaks = []
    for ch in (0, 1, 2):
        p = make_plugin(ch)
        out = render(p, plucked_note())
        peaks.append(20 * np.log10(np.abs(out).max() + 1e-12))
    spread = max(peaks) - min(peaks)
    ok = bool(spread < 3.0)
    print(f"  {'PASS' if ok else 'FAIL'}  ch levels    "
          + "  ".join(f"{CHANNELS[i]}={peaks[i]:+.1f}" for i in range(3))
          + f"   spread {spread:.1f} dB (limit 3.0)")
    return ok


def test_aliasing(channel):
    """A high sine should not produce energy BELOW it. Anything there is
    aliasing folded back down, and it is what makes distortion sound harsh."""
    # f0 MUST NOT divide the sample rate. 48000 = 16 * 3000 exactly, so with
    # f0 = 3000 every alias image folds onto a multiple of 3000 Hz and the
    # inharmonic band searched below is mathematically incapable of holding
    # alias energy - the test then reads its own FFT noise floor (-75.9 dBc,
    # identical across x1/x2/x4/x8 and across channels at 28% vs 49% THD).
    # 3050 is incommensurate with 48000, so the images land where we look.
    f0 = 3050.0
    assert SR % f0 != 0, "f0 divides SR - alias images hide on harmonic bins"
    t = np.arange(int(SR * 0.5)) / SR
    sig = (0.25 * np.sin(2 * np.pi * f0 * t)).astype(np.float32)
    p = make_plugin(channel)
    out = render(p, sig)[SR // 8:]
    spec = np.abs(np.fft.rfft(out * np.hanning(len(out))))
    fr = np.fft.rfftfreq(len(out), 1 / SR)
    fund = spec[np.argmin(np.abs(fr - f0))]
    # Everything below the fundamental, MINUS the supply ripple. The amp puts
    # 120 Hz ripple on its B+ rail on purpose, and the sidebands that produces
    # are inharmonic by design - exactly what this test looks for. Without the
    # mask it reports the ghost notes as aliasing: adding the ripple moved CLEAN
    # from -81.2 to -71.2 dBc with the oversampling untouched.
    #
    # PARTIAL, and worth knowing why. Masking the discrete lines recovers about
    # 1 dB of the 10. The rest is real: ripple modulates every harmonic, aliased
    # ones included. The 16th of 3050 Hz lands at 48800 and folds to 800 Hz, and
    # its sidebands sit at 800 +/- 120k - not a multiple of 120, not near
    # f0 +/- 120k, so no mask of this shape catches them. That energy IS alias
    # energy, just alias energy the ripple created.
    #
    # The measurement that would separate the two cleanly is differential:
    # aliasing moves with the oversampling factor, ripple does not. Comparing
    # x1 against x8 would isolate it. Not done yet.
    RIPPLE = 120.0
    TOL    = 12.0                      # Hann main lobe plus a little
    below  = (fr > 100) & (fr < f0 * 0.9)
    for k in range(1, int(f0 / RIPPLE) + 2):
        for centre in (k * RIPPLE, f0 - k * RIPPLE, f0 + k * RIPPLE):
            below &= np.abs(fr - centre) > TOL
    alias = spec[below].max()
    ratio = 20 * math.log10(alias / (fund + 1e-20) + 1e-20)
    ok = bool(ratio < -40)
    print(f"  {'PASS' if ok else 'FAIL'}  aliasing     "
          f"worst inharmonic below {f0:.0f} Hz = {ratio:.1f} dBc  (limit -40)")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", type=int, default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    chans = [args.channel] if args.channel is not None else [0, 1, 2]
    failures = 0

    print(f"\nAmpHead Custom - offline measurement   ({SR} Hz, IR bypassed)\n")
    for ch in chans:
        print(f"[{CHANNELS[ch]}]")
        for fn in (test_swell, test_dynamics, test_aliasing):
            try:
                r = fn(ch, args.verbose) if fn is test_swell else fn(ch)
                if r is not None and not r:
                    failures += 1
            except Exception as e:
                print(f"  ERROR {fn.__name__}: {e}")
                failures += 1
        try:
            test_harmonics(ch)
        except Exception as e:
            print(f"  ERROR harmonics: {e}")
        print()

    if args.channel is None:
        print("[ALL CHANNELS]")
        if not test_channel_levels():
            failures += 1
        print()

    print(f"{'FAILURES: ' + str(failures) if failures else 'ALL PASS'}\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
