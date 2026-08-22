/*
  ==============================================================================
    CopilotTone  -  Neural-Ready Amp Host
    PluginProcessor.cpp

    Analytic amp model: filters, shapes and saturates in discrete stages. An
    RTNeural backend would replace the AmpEngine preamp block; ToneStack,
    PowerAmp and IRSection stay as they are.
  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

// std::atomic has no fetch_max for floats, so a peak is raised with a CAS loop.
static inline void raisePeak (std::atomic<float>& target, float value) noexcept
{
    float prev = target.load (std::memory_order_relaxed);
    while (value > prev
           && ! target.compare_exchange_weak (prev, value, std::memory_order_relaxed))
    {}
}

// Per-stage harmonic drive.
//
// Each stage contributes its own mix of harmonics and dynamics, so one global
// multiplier would push the fizzy stages as hard as the musical ones. Terms are
// listed in signal-chain order; 1.0 = original voicing. Only some are additive:
// kIH / kBias / kExc / kEdge / kHyst ADD signal, so scaling them adds harmonics,
// while kPiDrive and kPwrK are saturating limiters, so scaling those buys
// compression and flattens dynamics. All terms are bounded.
namespace HarmonicDrive
{
    // -- Preamp ---------------------------------------------------------------
    // Bias drift: even harmonics that grow over the note's life. It multiplies an
    // envelope, so scaling it scales how far the operating point moves over the
    // note - which is swell, not harmonics.
    constexpr float biasDrift   = 1.0f;

    // Even-harmonic injection at the interstage: warmth and thickness, octave-
    // related so it stays musical when pushed. Far more THD-efficient per unit of
    // lost pick dynamics than preamp drive, which compresses instead of adding.
    constexpr float preampEven  = 5.25f;

    // Upper-harmonic exciter: air and definition. Held back - it turns harsh first.
    constexpr float exciter     = 2.2f;

    // String edge: soft-clips a 2.4 kHz band, the only place steel survives a cab.
    constexpr float stringEdge  = 3.5f;

    // -- Power section --------------------------------------------------------
    // Phase inverter: odd harmonics and grind, with drive tracking an envelope so
    // it adds touch response too. Self-normalising, so it takes a lot safely.
    constexpr float phaseInv    = 2.0f;

    // Push-pull EL34 asymmetry. Held back - as much a compressor as a saturator.
    constexpr float pushPull    = 1.2f;

    // -- Iron -----------------------------------------------------------------
    // Transformer hysteresis: feeds back an 80 Hz lowpass. Iron memory, low-mid density.
    constexpr float transformer = 2.0f;
}

// Shared filter coefficient helpers (local to this translation unit).
namespace {
    // 1-pole HPF  alpha = SR / (SR + 2pi*fc)
    static float hpfAlpha (float fc, float sr)
    {
        return sr / (sr + juce::MathConstants<float>::twoPi * fc);
    }
    // 1-pole LPF  c = 1 - exp(-2pi*fc / SR)
    static float lpfCoeff (float fc, float sr)
    {
        return 1.f - std::exp (-juce::MathConstants<float>::twoPi * fc / sr);
    }
}

//==============================================================================
// AmpEngine

void AmpEngine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    const float fsr = (float) sampleRate;

    // Input front-end coefficients
    cDC    = hpfAlpha (2.f,     fsr);   // DC blocker
    cZHP   = hpfAlpha (7.f,     fsr);   // input impedance coupling cap ~7 Hz
    // Built by setCableLength, so prepare and the parameter cannot drift apart.
    // -1 forces the first design through.
    cblLenIdx = -1;
    setCableLength (1);                  // 10 ft, the default
    cHPF   = hpfAlpha (60.f,    fsr);   // input HPF 60 Hz (tighter sub, FM3-style lows)
    cInLP  = lpfCoeff  (16000.f, fsr);   // input LPF 16 kHz

    // Transient smoothing: 0.5 ms attack, 80 ms release
    cTsAtk = std::exp (-1.f / (0.0005f * fsr));
    cTsRel = std::exp (-1.f / (0.080f  * fsr));

    // Interstage coupling
    cIsHP  = hpfAlpha (100.f,  fsr);   // coupling cap between V1A and V1B
    cIsLP  = lpfCoeff  (6000.f, fsr);   // grid stopper LP into V1B

    // Cathode follower
    cCfAtk = std::exp (-1.f / (0.0003f * fsr));   // 0.3 ms attack
    cCfRel = std::exp (-1.f / (0.020f  * fsr));   // 20 ms release

    // Oversampling for V1A + interstage + V1B; per-factor coefficients built below.
    ovsBuf.allocate ((size_t) kMaxFactor * (size_t) maxBlockSize, true);
    buildOsTables();
    selectOs (2);

    // Upper harmonic exciter LP: content above ~3 kHz is soft-clipped and mixed back.
    cUhLP   = lpfCoeff (3000.f, fsr);

    cBrightHP = hpfAlpha (150.f, fsr);   // OD boost: low-cut below ~150 Hz (TS-style)

    // Low/mid shaper coefficients
    cLmPunch = lpfCoeff (95.f,  fsr);   // sub-bass "chug" punch (two-pole)
    cMud     = lpfCoeff (340.f, fsr);   // resonant anti-mud bandpass centre (~340 Hz)
    cVox     = lpfCoeff (700.f, fsr);   // resonant upper-mid presence centre (~700 Hz)

    // Chuck transient detector envelopes
    cChkFAtk = std::exp (-1.f / (0.0004f * fsr));   // 0.4 ms fast attack
    cChkFRel = std::exp (-1.f / (0.010f  * fsr));   // 10 ms fast release
    cChkSAtk = std::exp (-1.f / (0.006f  * fsr));   // 6 ms medium attack
    cChkSRel = std::exp (-1.f / (0.045f  * fsr));   // 45 ms medium release

    // cIhDC is not set here: it is applied oversampled, so it comes from tIhDC[].
    cMet = lpfCoeff (2400.f, fsr);   // metallic-string band centre (~2.4 kHz)
    cFat = lpfCoeff (210.f,  fsr);   // string-body resonance centre (~210 Hz)

    // Pick attack detector
    cPkFAtk = std::exp (-1.f / (0.0005f * fsr));   // 0.5 ms fast attack
    cPkFRel = std::exp (-1.f / (0.015f  * fsr));   // 15 ms fast release
    cPkSAtk = std::exp (-1.f / (0.005f  * fsr));   // 5 ms slow attack
    cPkSRel = std::exp (-1.f / (0.060f  * fsr));   // 60 ms slow release
    cPkHP   = hpfAlpha  (4000.f, fsr);              // 4 kHz pre-emphasis HP

    reset();
}

// Precompute every oversampled-domain coefficient. prepare() only.
void AmpEngine::buildOsTables()
{
    static constexpr int kF[4] = { 1, 2, 4, 8 };
    for (int i = 0; i < 4; ++i)
    {
        const float osr = (float) sr * (float) kF[i];
        tIsHP[i][0] = hpfAlpha ( 80.f, osr);   // CH1: tightest bass rejection
        tIsHP[i][1] = hpfAlpha (120.f, osr);   // CH2: moderate
        tIsHP[i][2] = hpfAlpha (160.f, osr);   // CH3: most bass kept out of V1B
        tIsLP[i]    = lpfCoeff (6000.f, osr);
        // V1B -> V1C coupling and grid stopper, tighter than the V1A->V1B pair.
        tIs2HP[i][0] = hpfAlpha (150.f, osr);
        tIs2HP[i][1] = hpfAlpha (180.f, osr);
        tIs2HP[i][2] = hpfAlpha (210.f, osr);
        tIs2LP[i]    = lpfCoeff (5000.f, osr);
        tBsA[i]     = lpfCoeff (400.f,  osr);
        tBsB[i]     = lpfCoeff (3000.f, osr);
        tHmA[i]     = std::exp (-1.f / (0.003f * osr));   // bloom attack 3 ms
        tHmR[i]     = std::exp (-1.f / (0.600f * osr));   // bloom release 600 ms
        tOsLP[i]    = OsBiquadChain::design (kOsCutoffFrac * (float) sr, osr);
        // Cathode bypass charge and grid conduction, tabled at osr like the rest.
        tCkA[i]     = 1.f - std::exp (-1.f / (0.025f * osr));   // V1A cathode cap ~25 ms
        tCkB[i]     = 1.f - std::exp (-1.f / (0.015f * osr));   // V1B cathode cap ~15 ms
        // Cathode bypass corner, per channel. A Fender-sized 25 uF on a 1.5k
        // cathode sits at 4 Hz - fully bypassed, every low kept. A Marshall 0.68 uF
        // on 820R lands near 285 Hz and throws most of them away before the next
        // stage. Swapping this cap is most of what a channel relay actually does.
        tCkFa[i][0] = lpfCoeff ( 25.f, osr);    // CH1 V1A: big cap, keeps its lows
        tCkFa[i][1] = lpfCoeff (150.f, osr);    // CH2
        tCkFa[i][2] = lpfCoeff (280.f, osr);    // CH3: small cap, tight
        tCkFb[i][0] = lpfCoeff ( 40.f, osr);    // CH1 V1B
        tCkFb[i][1] = lpfCoeff (190.f, osr);    // CH2
        tCkFb[i][2] = lpfCoeff (330.f, osr);    // CH3
        tGridA[i]   = 1.f - std::exp (-1.f / (0.0015f * osr));  // grid draws current fast
        tGridR[i]   =       std::exp (-1.f / (0.012f  * osr));  // coupling cap recovers ~12 ms
        // The ih DC tracker runs inside the oversampled loop, so its coefficient is
        // built at osr; at the native rate its corner would scale with the factor.
        tIhDC[i]    = lpfCoeff (12.f, osr);
    }
}

// RT-safe: just copy the chosen factor's coefficients into the active set.
void AmpEngine::selectOs (int factor)
{
    osFactor = juce::jlimit (1, kMaxFactor, factor);
    const int i = osIdx (osFactor);
    cIsHP2x[0] = tIsHP[i][0];  cIsHP2x[1] = tIsHP[i][1];  cIsHP2x[2] = tIsHP[i][2];
    cIsLP2x = tIsLP[i];
    cIs2HP[0] = tIs2HP[i][0];  cIs2HP[1] = tIs2HP[i][1];  cIs2HP[2] = tIs2HP[i][2];
    cIs2LP = tIs2LP[i];
    osUp.active = tOsLP[i];  osDown.active = tOsLP[i];
    cCkA = tCkA[i];  cCkB = tCkB[i];  cGridAtk = tGridA[i];  cGridRel = tGridR[i];
    for (int c = 0; c < 3; ++c) { cCkFa[c] = tCkFa[i][c];  cCkFb[c] = tCkFb[i][c]; }
    cBsLPa  = tBsA[i];   cBsLPb = tBsB[i];
    cHmBAtk = tHmA[i];   cHmBRel = tHmR[i];
    cIhDC   = tIhDC[i];
}

void AmpEngine::reset()
{
    dcX = dcY = 0.f;
    zHPx = zHPy = 0.f;
    cblZ1 = cblZ2 = 0.f;
    inLP = tsEnv = 0.f;
    hpfX = hpfY = 0.f;
    isHPx = isHPy = isLP = 0.f;
    is2HPx = is2HPy = is2LP = 0.f;
    cfEnv = 0.f;
    osUp.reset();  osDown.reset();
    ckBiasA = ckBiasB = gridChg = 0.f;
    ckLPa = ckLPb = 0.f;
    pkFast = pkSlow = pkHPx = pkHPy = 0.f;
    bsLPa = bsLPb = hmBloom = uhLP1 = ihDC = 0.f;
    brightHPx = brightHPy = 0.f;
    lmPunch1 = lmPunch2 = mudLP1 = mudLP2 = 0.f;
    voxLP1 = voxLP2 = 0.f;
    chkFast = chkSlow = 0.f;
    metLP1 = metLP2 = 0.f;
    fatLP1 = fatLP2 = 0.f;
}

// Cable lengths a guitarist actually owns. Capacitance is the pickup's own ~100 pF
// plus roughly 100 pF per metre of lead; against a 4 H humbucker that puts the
// resonance where the third column says. Q falls with length because a longer cable
// has more series resistance and more dielectric loss - which is the real reason a
// long lead sounds dull as well as dark.
//
//      3 ft   0.9 m    150 pF    6.5 kHz   Q 1.55
//     10 ft   3.0 m    400 pF    4.0 kHz   Q 1.40
//     15 ft   4.6 m    560 pF    3.4 kHz   Q 1.28
//     20 ft   6.1 m    710 pF    3.0 kHz   Q 1.16
//     30 ft   9.1 m   1010 pF    2.5 kHz   Q 0.95
void AmpEngine::setCableLength (int idx)
{
    idx = juce::jlimit (0, 4, idx);
    if (idx == cblLenIdx) return;
    cblLenIdx = idx;

    static constexpr float kMetres[5] = { 0.9f, 3.0f, 4.6f, 6.1f, 9.1f };
    static constexpr float kCblQ  [5] = { 1.55f, 1.40f, 1.28f, 1.16f, 0.95f };

    const float L  = 4.0f;                                   // humbucker, henries
    const float C  = 100e-12f + 100e-12f * kMetres[idx];     // pickup + lead
    const float f0 = juce::jlimit (500.f, (float) sr * 0.45f,
                                   1.f / (juce::MathConstants<float>::twoPi
                                          * std::sqrt (L * C)));

    const float w0 = juce::MathConstants<float>::twoPi * f0 / (float) sr;
    const float cw = std::cos (w0), al = std::sin (w0) / (2.f * kCblQ[idx]);
    const float a0 = 1.f + al;
    cblB0 = ((1.f - cw) * 0.5f) / a0;
    cblB1 = ( 1.f - cw)         / a0;
    cblB2 = cblB0;
    cblA1 = (-2.f * cw)         / a0;
    cblA2 = ( 1.f - al)         / a0;
}

// Front-end at native SR -> oversampled V1A + interstage + V1B -> CF at native SR.
void AmpEngine::process (float* data, int numSamples,
                          float gainNorm, float charNorm, bool brightEnabled, int ci, int factor, float tubeTemp,
                         PowerSupply& psu)
{
    if (factor != osFactor) selectOs (factor);   // RT-safe: table lookup, no exp
    // Per-channel amp output level calibration, set from measured peak dBFS so all
    // three channels land on CLEAN's reference level of about -15.2 dBFS. Equal
    // trim does not work: saturation supplies its own makeup gain, so a hotter
    // channel needs a LOWER base, not an equal one.
    static constexpr float kAmpBase[3]     = { 0.645f, 0.242f, 0.205f };
    static constexpr float kChannelTrim[3] = { 1.00f, 1.00f, 1.00f };
    // Gain-proportional level normalisation, flattened to 4.1 dB across the knob.
    // A steeper trim makes the amp quieter as GAIN rises while adding no
    // distortion. At GAIN 5.5 the value still matches the old law within 2%.
    const float            amp             = kAmpBase[ci] * kChannelTrim[ci]
                                           / (0.62f + gainNorm * 0.38f);
    // High threshold, so transient smoothing rarely fires and pick attacks flow.
    static constexpr float kTsThresh       = 0.90f;

    // CH3 nonlinear gain warp, so the channel saturates inside the usable part of
    // the knob. The warp ACCELERATES: a front-loaded one leaves the top third dead,
    // because by GAIN 7 both V1A and V1B sit on their asymptote and more drive into
    // a clipped stage changes nothing. The top also feeds gainTop below.
    const float gainW = (ci == 2)
        ? gainNorm * (1.10f + 0.30f * gainNorm)
        : gainNorm;
    // Near zero for the first half of the knob, then climbing - work for the top.
    const float gainTop = gainNorm * gainNorm;

    // V1A: first triode gain stage.
    // The drive bases are deliberately low. Preamp drive is the dominant control
    // over BOTH pick dynamics and THD and moves them in opposite directions, so it
    // is set from the dynamics target and the harmonics are restored with
    // preampEven. LEAD sits below CRUNCH here; its density comes from gainW and kIH.
    static constexpr float kV1ADriveBase[3] = { 0.45f, 1.30f, 1.55f };
    const float v1aDriveBase = kV1ADriveBase[ci] * (1.8f + gainW * (3.2f + gainW * 5.6f));

    // V1B: cold-biased second triode. CH3 drives hardest for a dense, sustaining lead.
    static constexpr float kV1BDriveBase[3] = { 0.50f, 1.42f, 1.55f };
    // DRIVE (char) pushes the LATE stages while GAIN pushes the first one, so the two
    // controls add saturation at different points in the cascade instead of duplicating
    // each other: GAIN buys early bite, DRIVE buys density further down.
    const float charDrive = 0.75f + 0.65f * charNorm;
    const float v1bDrive = kV1BDriveBase[ci] * charDrive
                         * (1.4f + gainW * (1.7f + gainW * 2.7f));
    // Input front-end at native SR
    // Ripple depth. What survives the reservoir is a couple of percent of B+ under
    // load; through several gain stages that is plenty to hear as ghost notes.
    // Deeper on the high-gain channels because there is more gain after it to turn
    // the modulation into audible sidebands.
    static constexpr float kRipple[3] = { 0.010f, 0.030f, 0.070f };

    for (int n = 0; n < numSamples; ++n)
    {
        float x = data[n];

        { const float y = cDC * (dcY + x - dcX);  dcX = x;  dcY = y;  x = y; }
        { const float y = cZHP * (zHPy + x - zHPx);  zHPx = x;  zHPy = y;  x = y; }
        {   // pickup + cable resonance
            const float y = cblB0 * x + cblZ1;
            cblZ1 = cblB1 * x - cblA1 * y + cblZ2;
            cblZ2 = cblB2 * x - cblA2 * y;
            x = y;
        }
        { const float y = cHPF * (hpfY + x - hpfX);  hpfX = x;  hpfY = y;  x = y; }
        inLP += cInLP * (x - inLP);  x = inLP;

        // Supply ripple modulating the stage gain. Applied at the input so that
        // everything downstream amplifies the modulation the way the real chain
        // does - the sidebands get distorted along with the note that made them.
        x *= 1.f + psu.ripple (kRipple[ci]);
        {
            const float absX = std::abs (x);
            tsEnv = (absX > tsEnv) ? cTsAtk * tsEnv + (1.f - cTsAtk) * absX
                                   : cTsRel * tsEnv + (1.f - cTsRel) * absX;
            if (tsEnv > kTsThresh)
                x *= 0.85f + 0.15f * (kTsThresh / tsEnv);
        }

        // Pick detector + dynamic high-mid pre-emphasis. Fast envelope minus slow
        // envelope = the pick transient window, which scales the 4 kHz HP added back.
        {
            // Pre-emphasis at 4 kHz scaled by the pick window. CH2 highest, for bark.
            static constexpr float kPickBite[3] = { 0.14f, 0.30f, 0.36f };
            const float absX = std::abs (x);
            if (absX > pkFast) pkFast += (1.f - cPkFAtk) * (absX - pkFast);
            else               pkFast  = cPkFRel * pkFast;
            if (absX > pkSlow) pkSlow += (1.f - cPkSAtk) * (absX - pkSlow);
            else               pkSlow  = cPkSRel * pkSlow;
            const float pickAtk = juce::jmax (0.f, pkFast - pkSlow);
            const float hmY = cPkHP * (pkHPy + x - pkHPx);  pkHPx = x;  pkHPy = hmY;
            x += kPickBite[ci] * pickAtk * hmY;
        }

        // OD boost: Tube Screamer-style pre-drive. The ~150 Hz HP removes bass
        // before soft-clipping, the clip is asymmetric to push mids and even
        // harmonics, and the HP state runs always so engaging it is click-free.
        {
            const float yLC = cBrightHP * (brightHPy + x - brightHPx);
            brightHPx = x;  brightHPy = yLC;
            if (brightEnabled)
            {
                static constexpr float kODDrive[3] = { 2.2f, 4.2f, 3.2f };
                static constexpr float kODMix[3]   = { 0.18f, 0.36f, 0.27f };
                const float od  = yLC * kODDrive[ci];
                const float sat = (od >= 0.f) ? od / (1.f + 0.55f * od)
                                              : od / (1.f - 0.90f * od);
                x = sat * kODMix[ci] + x * (1.f - kODMix[ci]);
            }
        }

        data[n] = x;   // front-end output; gain trim is applied after downsampling
    }

    // Pickup sensitivity: block-end brightness ratio. Brighter pickup, more drive.
    const float brightRatio = juce::jmin (1.f, std::abs (pkHPy) / (pkFast + 1e-6f));

    // Block-end pick transient, computed before the drives so both react to it.
    static constexpr float kPickPush[3] = { 0.12f, 0.26f, 0.36f };
    const float pickAtkBlk  = juce::jmax (0.f, pkFast - pkSlow);
    const float pickNorm    = juce::jmin (1.f, pickAtkBlk * 4.f);   // 0-1 normalized pick intensity

    // V1A drive: brightness and pick intensity both widen the saturation threshold.
    // B+ droop from the shared rail. A lower plate voltage means less gain from
    // every stage sitting on it, which is why a real amp goes softer under a big
    // chord and comes back as the reservoir refills - the whole amp breathes, not
    // just the output stage. Deeper on the high-gain channels: more stages hanging
    // off the same rail means more of them move when it does.
    // How much gain the stage gives up below its cathode corner. An unbypassed
    // triode loses roughly half, so these stay well under that.
    static constexpr float kCkShelfA[3] = { 0.08f, 0.26f, 0.40f };
    static constexpr float kCkShelfB[3] = { 0.06f, 0.22f, 0.34f };

    static constexpr float kRail[3] = { 0.06f, 0.13f, 0.20f };
    const float railGain = 1.f / (1.f + kRail[ci] * juce::jmin (psu.preNode, 2.5f));

    const float v1aDrive    = v1aDriveBase * railGain
                            * (0.84f + 0.26f * brightRatio + 0.14f * pickNorm);
    const float v1bPickMod  = 0.90f + 0.18f * brightRatio + 0.08f * pickNorm;
    const float v1bDriveEff = v1bDrive * railGain
                            * (1.f + kPickPush[ci] * pickAtkBlk) * v1bPickMod;

    // Character knob: scales V1A mid-band asymmetry and the even-harmonic injection.
    const float charIH      = 0.2f + 1.6f * charNorm;   // IH injection scale
    const float charMidAsym = 0.7f + 0.6f * charNorm;   // V1A mid positive headroom

    // Upsample by 'factor': zero insertion plus the anti-imaging LP, upGain compensating.
    const int   F      = osFactor;
    const int   ovsLen = F * numSamples;
    const float upGain = (float) F;
    for (int n = 0; n < numSamples; ++n)
    {
        ovsBuf[F * n] = data[n] * upGain;
        for (int k = 1; k < F; ++k) ovsBuf[F * n + k] = 0.0f;
    }
    for (int n = 0; n < ovsLen; ++n) ovsBuf[n] = osUp.process (ovsBuf[n]);

    // V1A + interstage + V1B run oversampled, so aliasing products land above the
    // new Nyquist where the decimation filter removes them.
    for (int n = 0; n < ovsLen; ++n)
    {
        float x = ovsBuf[n];

        // V1A - frequency-dependent saturation. The signal is split into three
        // bands after drive scaling, each saturated with its own k, then recombined.
        //   Low  (<~400 Hz):  tight and symmetric, so punch survives.
        //   Mid  (400-3 kHz): richest and asymmetric - the vocal 2nd-harmonic zone.
        //   Upper (>~3 kHz):  dynamic, scaling with pick brightness and attack.
        {
            // CATHODE BYPASS (Rk || Ck). The cap charges with average cathode
            // current, the bias goes more negative and the stage backs itself off:
            // this is touch compression. Modelled as GAIN, not DC - DC reads as swell.
            ckBiasA += cCkA * (std::abs (x) - ckBiasA);
            const float ckGainA = 1.f / (1.f + 0.09f * tubeTemp * ckBiasA);

            // The frequency half of the same cap: what it does not bypass runs
            // through Rk unbypassed and loses gain to local feedback. Subtracting
            // the lows below the corner IS that shelf.
            ckLPa += cCkFa[ci] * (x - ckLPa);
            x -= kCkShelfA[ci] * ckLPa;

            const float xd = x * v1aDrive * ckGainA;

            bsLPa += cBsLPa * (xd - bsLPa);   // low/mid split LP (~400 Hz)
            bsLPb += cBsLPb * (xd - bsLPb);   // mid/upper split LP (~3 kHz)

            const float xLow = bsLPa;
            const float xMid = bsLPb - bsLPa;
            const float xUpr = xd   - bsLPb;

            // Knee hardness per channel: how ABRUPTLY each band saturates, which is
            // not the same as how hard it is driven. A sharp knee collapses 19 dB of
            // pick range to 1-3 dB, where a real high-gain amp still passes 4-6.
            static constexpr float kL [3] = { 0.13f, 0.22f, 0.26f };
            static constexpr float kMp[3] = { 0.28f, 0.54f, 0.60f };
            static constexpr float kMn[3] = { 0.06f, 0.11f, 0.13f };
            const float kUpr = (0.18f + 0.35f * brightRatio + 0.30f * pickAtkBlk)
                               * (1.0f + (float) ci * 0.15f);

            // Gain-reactive low-end tightening, kept shallow. At three times this
            // depth LEAD cuts 8 dB out of the 80-160 Hz palm-mute band at GAIN 10 and
            // tilts the low strings 6.6 dB down. That is removal, not tightening.
            const float leCut = 1.f - gainNorm * (float) ci * 0.13f;
            const float satL  = leCut * ((xLow >= 0.f) ? xLow / (1.f + kL[ci] * xLow)
                                                        : xLow / (1.f - kL[ci] * xLow));
            const float kMpEff = kMp[ci] * charMidAsym;
            const float satM = (xMid >= 0.f) ? xMid / (1.f + kMpEff * xMid)
                                              : xMid / (1.f - kMn[ci] * xMid);
            const float satU = (xUpr >= 0.f) ? xUpr / (1.f + kUpr        * xUpr)
                                              : xUpr / (1.f - kUpr * 0.6f * xUpr);
            const float bandOut = satL + satM + satU;

            // HOT WIDEBAND TRIODE.
            // The band split above saturates each band on its own, which is
            // articulate but is not what a valve does: a real triode saturates the
            // whole signal at once, and the intermodulation between bass and treble
            // is much of an amp's density. Splitting also flattens the harmonic
            // series instead of letting it decay. So when the valves are hot, blend
            // toward one wideband curve. The bias offset is the hot operating point
            // and generates the even harmonics; softTube(b) is subtracted so it adds
            // no DC, and gn restores the small-signal gain the offset costs.
            static constexpr float kWideMix [3] = { 0.35f, 0.80f, 1.00f };
            static constexpr float kWideCeil[3] = { 3.20f, 2.60f, 2.10f };
            // NEGATIVE ON PURPOSE. The even harmonics this offset generates arrive in
            // opposite phase to those V1B produces downstream, so a positive bias
            // cancels H2 instead of adding it - taking it from -0.14 to +0.50 drops H2
            // by 9 dB and leaves the channel odd-dominant and hollow.
            static constexpr float kHotBias [3] = { -0.35f, -0.60f, -0.85f };
            const float A  = kWideCeil[ci];
            // DRIVE (char) and the top of GAIN both steer the hot operating point:
            // how far the valve is pushed into its asymmetric region.
            const float b  = kHotBias[ci] * tubeTemp
                           * (0.50f + 0.70f * charNorm + 0.45f * gainTop);
            const float tb = softTube (b);
            const float gn = 1.f / juce::jmax (0.5f, 1.f - tb * tb);
            const float wide = A * gn * (softTube (xd / A + b) - tb);

            x = bandOut + kWideMix[ci] * tubeTemp * (wide - bandOut);
        }

        // Harmonic bloom: a slow envelope on V1A's output, held through sustain by a
        // 600 ms release, so V1B's bias drift grows and the note opens up as it rings.
        {
            const float absVA = std::abs (x);
            if (absVA > hmBloom) hmBloom += (1.f - cHmBAtk) * (absVA - hmBloom);
            else                 hmBloom  = cHmBRel * hmBloom;
        }

        // Interstage coupling - per-channel HPF cutoff keeps bass out of V1B.
        { const float y = cIsHP2x[ci] * (isHPy + x - isHPx);  isHPx = x;  isHPy = y;  x = y; }
        isLP += cIsLP2x * (x - isLP);  x = isLP;

        // Even-harmonic injection (CH2/CH3) between interstage and V1B: thickness.
        {
            static constexpr float kIH[3] = { 0.07f, 0.24f, 0.46f };
            // x*x is always positive, so this term carries a DC component that
            // follows the note envelope. Injected raw it moves V1B's operating point
            // as the note decays, heard as the note swelling. A real stage has a
            // coupling capacitor: track the DC at ~12 Hz and inject only the AC.
            const float ih = charIH * x * x / (1.f + 2.0f * std::abs (x));
            ihDC += cIhDC * (ih - ihDC);
            // The hot wideband curve makes even harmonics from asymmetry, so this
            // squarer is redundant when hot - and it locks H2 and H4 together.
            x += HarmonicDrive::preampEven * kIH[ci] * (1.f - 0.70f * tubeTemp) * (ih - ihDC);
        }

        // Bias drift: the bloom envelope shifts V1B's operating point over the note.
        {
            static constexpr float kBias[3] = { 0.02f, 0.07f, 0.12f };
            // hmBloom is unbounded and follows V1A's output, which reaches 3-8 at
            // high gain. Multiplied by kBias that put an offset well past V1B's own
            // headroom, pinning the stage until the 600 ms release let go - audible
            // as the amp cutting out. Saturating the envelope bounds it to kBias.
            const float bloomN = hmBloom / (1.f + hmBloom);   // 0..1, never more
            x += HarmonicDrive::biasDrift * kBias[ci] * bloomN;
        }

        // V1B - cold-biased triode, asymmetric per channel: a looser positive knee
        // against a hard negative one. CH3 clips hardest, hence the lead density.
        {
            static constexpr float kV1Bp[3] = { 0.65f, 0.70f, 0.80f };
            static constexpr float kV1Bn[3] = { 1.70f, 1.90f, 2.30f };
            // GRID CONDUCTION / BLOCKING. Driven hard the grid goes positive, draws
            // current and charges the coupling cap feeding it, so the stage biases
            // too cold and partially cuts off, recovering over tens of milliseconds.
            {
                const float over = std::abs (x) - 1.10f;
                const float o    = (over > 0.f) ? over : 0.f;
                if (o > gridChg) gridChg += cGridAtk * (o - gridChg);
                else             gridChg  = cGridRel * gridChg;
                x *= 1.f / (1.f + 0.14f * tubeTemp * gridChg);
            }

            // V1B cathode bypass - same mechanism as V1A, shorter time constant.
            ckBiasB += cCkB * (std::abs (x) - ckBiasB);
            ckLPb += cCkFb[ci] * (x - ckLPb);
            x -= kCkShelfB[ci] * ckLPb;
            x *= v1bDriveEff / (1.f + 0.07f * tubeTemp * ckBiasB);
            x = (x >= 0.f) ? x / (1.f + kV1Bp[ci] * x)
                            : x / (1.f - kV1Bn[ci] * x);
        }

        // V1C - THIRD gain stage, brought in by the TOP of the GAIN knob.
        // More drive into two stages does nothing once both sit on their asymptote,
        // which they do by GAIN 7; the way to get more gain is another stage, with
        // INTERSTAGE ATTENUATION so it is driven rather than slammed. It fades in on
        // gainTop, so nothing at or below mid-GAIN changes and CLEAN gets none of it.
        {
            static constexpr float kV1CAmt  [3] = { 0.0f, 0.50f, 0.90f };
            static constexpr float kV1CDrive[3] = { 0.0f, 1.8f,  3.0f  };
            static constexpr float kV1CAtten    = 0.55f;
            const float amt = gainTop * kV1CAmt[ci];
            if (amt > 0.001f)
            {
                { const float y = cIs2HP[ci] * (is2HPy + x - is2HPx); is2HPx = x; is2HPy = y; }
                is2LP += cIs2LP * (is2HPy - is2LP);
                const float drv = 1.f + amt * kV1CDrive[ci];
                // softTube, not another rational saturator: its harmonic series
                // decays with order, which keeps a third stage out of fizz.
                // Normalise by the ATTENUATION only, never by the drive: dividing by
                // drv shrinks the stage the harder it is pushed.
                const float sat = softTube (is2LP * kV1CAtten * drv) / kV1CAtten;
                x += amt * (sat - x);
            }
        }

        // DS-2-style grind: an aggressive asymmetric hard clip with a silicon-diode
        // feel over the tube voice, oversampled. CH3 strong, CH2 light, CH1 none.
        {
            // A seasoning, not the main flavour: jlimit is a true discontinuity, so
            // it generates unbounded harmonics that alias even at 8x.
            static constexpr float kDs2Drv[3] = { 0.0f, 0.8f,  1.1f };   // extra drive
            static constexpr float kDs2Mix[3] = { 0.0f, 0.26f, 0.24f };  // blend amount
            if (kDs2Mix[ci] > 0.f)
            {
            // Not saturated at high GAIN, so the top of the knob still has room.
                const float grind = 0.55f + 0.45f * charNorm + 0.55f * gainTop;
                const float d = x * (1.f + kDs2Drv[ci]);
                float c = (d >= 0.f) ? d / (1.f + 0.85f * d)     // asymmetric soft knee...
                                     : d / (1.f - 1.15f * d);
                c = juce::jlimit (-0.88f, 0.88f, c * 1.35f);      // ...into a hard ceiling
                x += juce::jmin (0.85f, kDs2Mix[ci] * grind) * (c - x);   // blend toward the grind
            }
        }

        ovsBuf[n] = x;
    }

    // Anti-alias LP + decimate back to native SR
    for (int n = 0; n < ovsLen; ++n) ovsBuf[n] = osDown.process (ovsBuf[n]);

    // Cathode follower + gain trim at native SR - not a significant alias source.
    for (int n = 0; n < numSamples; ++n)
    {
        float x = ovsBuf[F * n];   // decimate: keep one sample per oversampled group

        {
            const float absX = std::abs (x);
            cfEnv = (absX > cfEnv) ? cCfAtk * cfEnv + (1.f - cCfAtk) * absX
                                   : cCfRel * cfEnv + (1.f - cCfRel) * absX;
            // High threshold and soft knee: level management without squashing
            // the harmonics.
            static constexpr float kCfThresh = 0.86f;
            const float cfComp = (cfEnv > kCfThresh)
                               ? 0.92f + 0.08f * (kCfThresh / cfEnv)
                               : 1.0f;
            x = 0.97f * cfComp * x / (1.f + 0.02f * std::abs (x));
        }

        // Micro-compression on the pick transient
        {
            static constexpr float kPickComp[3] = { 0.02f, 0.03f, 0.05f };
            x *= 1.f / (1.f + kPickComp[ci] * pickAtkBlk);
        }

        // Upper harmonic exciter, ~3-7 kHz: content above ~3 kHz is soft-clipped to
        // create new harmonics, mixed back with pickup brightness and pick attack.
        {
            uhLP1 += cUhLP * (x - uhLP1);
            const float excHF  = x - uhLP1;
            const float excSat = excHF / (1.f + std::abs (excHF));
            // Stronger exciter for string definition + more upper harmonics/air.
            static constexpr float kExc[3] = { 0.12f, 0.20f, 0.30f };
            const float excMod = HarmonicDrive::exciter * kExc[ci]
                                 * (0.6f + 0.9f * brightRatio + 1.0f * pickAtkBlk);
            x += excMod * excSat;
        }

        // Low/mid shaper + "chuck" transient enhancer
        {
            // Chuck transient: fast/medium envelope ratio, spiking on the attack.
            const float ax = std::abs (x);
            chkFast = (ax > chkFast) ? cChkFAtk * chkFast + (1.f - cChkFAtk) * ax
                                     : cChkFRel * chkFast + (1.f - cChkFRel) * ax;
            chkSlow = (ax > chkSlow) ? cChkSAtk * chkSlow + (1.f - cChkSAtk) * ax
                                     : cChkSRel * chkSlow + (1.f - cChkSRel) * ax;
            const float transN = juce::jlimit (0.f, 1.f,
                                               (chkFast / (chkSlow + 1e-6f) - 1.f) * 0.7f);

            // Sub-95 Hz punch tied to the attack: chug thump that stays out of sustain.
            lmPunch1 += cLmPunch * (x        - lmPunch1);
            lmPunch2 += cLmPunch * (lmPunch1 - lmPunch2);
            // Sub punch stays light: the chug body comes from the 210 Hz band.
            static constexpr float kPunch[3] = { 0.04f, 0.08f, 0.11f };
            x += kPunch[ci] * (0.25f + 1.30f * transN) * lmPunch2;

            // String-body boost at ~230 Hz: thickness between the sub-thump and scoop.
            static constexpr float kFatQ = 0.45f;
            fatLP1 += cFat * (x + kFatQ * (fatLP1 - fatLP2) - fatLP1);
            fatLP2 += cFat * (fatLP1 - fatLP2);
            static constexpr float kFat[3] = { 0.05f, 0.21f, 0.24f };
            x += kFat[ci] * (0.45f + 0.55f * gainNorm) * (fatLP1 - fatLP2);

            // Resonant ~340 Hz anti-mud scoop (narrow -> spares 200 Hz & 450 Hz+).
            static constexpr float kQ = 0.55f;
            mudLP1 += cMud * (x + kQ * (mudLP1 - mudLP2) - mudLP1);
            mudLP2 += cMud * (mudLP1 - mudLP2);
            // A shallow scoop with weak gain dependence, so high gain keeps its mids.
            const float mudBand = mudLP1 - mudLP2;
            static constexpr float kMud[3] = { 0.08f, 0.28f, 0.24f };
            x -= kMud[ci] * (0.45f + 0.40f * gainNorm) * mudBand;

            // Upper-mid presence (~700 Hz): broad resonant boost, blooms with gain.
            static constexpr float kVoxQ = 0.35f;
            voxLP1 += cVox * (x + kVoxQ * (voxLP1 - voxLP2) - voxLP1);
            voxLP2 += cVox * (voxLP1 - voxLP2);
            const float voxBand = voxLP1 - voxLP2;
            static constexpr float kVox[3] = { 0.10f, 0.16f, 0.22f };
            x += kVox[ci] * (0.45f + 0.55f * gainNorm) * voxBand;

            // Upper-mid voicing matched to the reference rig, which scoops 1.5-3.3 kHz.
            // The cab peaks there, so the notch is pre-IR and leaves a faint edge.
            static constexpr float kMetQ = 0.30f;   // broad -> covers 1.5-3.3 kHz
            metLP1 += cMet * (x + kMetQ * (metLP1 - metLP2) - metLP1);
            metLP2 += cMet * (metLP1 - metLP2);
            const float mb  = metLP1 - metLP2;
            // Shallow: 2-3 kHz is where pick attack lives, so a deep cut blunts it.
            static constexpr float kScoop[3] = { 0.24f, 0.10f, 0.16f };
            x -= kScoop[ci] * (0.50f + 0.40f * gainNorm) * mb;       // FM3 upper-mid scoop
            const float drv = mb * 3.0f;
            const float met = drv / (1.f + std::abs (drv));
            // The soft-clipped 2.4 kHz band spawns the 3-5 kHz steel the cab passes.
            static constexpr float kEdge[3] = { 0.04f, 0.06f, 0.12f };
            x += HarmonicDrive::stringEdge * kEdge[ci] * (0.40f + 0.60f * brightRatio) * met;

            // Broadband attack pop: the percussive snap of the chug.
            static constexpr float kChuck[3] = { 0.10f, 0.32f, 0.46f };
            x *= 1.f + kChuck[ci] * transN;
        }

        data[n] = x * amp;
    }
}

//==============================================================================
// ToneStack

void ToneStack::prepare (double sampleRate)
{
    const float sr = (float) sampleRate;

    cCoupHPF  = hpfAlpha (120.f, sr);   // coupling cap - input coupling HPF

    // Bass LP: wide enough to represent the range of a passive bass pot.
    cBassLP[0] = lpfCoeff (250.f, sr);
    cBassLP[1] = lpfCoeff (275.f, sr);
    cBassLP[2] = lpfCoeff (300.f, sr);

    // Treble HP at 1.6 kHz, so the shelf behaves like a passive pot, not a HPF.
    cTreHP = hpfAlpha (1600.f, sr);

    // Mid BP: per-channel bandpass for vocal character, CH3 widest for the vowel zone.
    cMidHP[0] = hpfAlpha (360.f, sr);   cMidLP[0] = lpfCoeff (1100.f, sr);
    cMidHP[1] = hpfAlpha (400.f, sr);   cMidLP[1] = lpfCoeff (950.f,  sr);
    cMidHP[2] = hpfAlpha (420.f, sr);   cMidLP[2] = lpfCoeff (1250.f, sr);

    reset();
}

void ToneStack::reset()
{
    coupX = coupY = 0.f;
    bassLP = 0.f;
    treHPx = treHPy = 0.f;
    midHPx = midHPy = midLP = 0.f;
}

void ToneStack::process (float* data, int numSamples,
                          float bass, float mid, float treble,
                          int ci)
{
    // Tone stack - three independent bands with no flat bleed path, so each
    // control has to earn its effect.
    //   Bass:   LP at 250-300 Hz per channel. 0 = cut, 0.5 = unity, 1 = boost.
    //   Mid:    BP 250-1200 Hz per channel.   0 = scoop, 0.5 = flat, 1 = forward.
    //   Treble: HP at 1.6 kHz.                0 = dark, 0.5 = flat, 1 = bright.
    // kIL is the natural passive insertion loss; the bands overlap in the mids.

    const float bassGain = bass * 2.40f;               // 0 -> 2.40 - fat to tight sweep
    const float treGain  = 0.06f + treble * 2.00f;    // 0.06 -> 2.06 - dark to bright, wider sweep
    // Passive-stack interaction: BASS loads the network and pulls the mid down,
    // TREBLE opens its top, so the controls behave interactively.
    const float midGain  = (0.10f + mid * 2.10f)
                         * (1.f - 0.22f * bass)
                         * (0.90f + 0.18f * treble);

    static constexpr float kIL = 0.68f;   // passive insertion loss (~-3.5 dB at flat)

    for (int n = 0; n < numSamples; ++n)
    {
        float x = data[n];

        // Input coupling HPF (120 Hz - removes LF buildup from cathode follower)
        { const float y = cCoupHPF * (coupY + x - coupX);
          coupX = x;  coupY = y;  x = y; }

        bassLP += cBassLP[ci] * (x - bassLP);
        const float xBass = bassLP;

        { const float y = cTreHP * (treHPy + x - treHPx);
          treHPx = x;  treHPy = y; }
        const float xTre = treHPy;

        // Mid band: HP at 250-300 Hz into LP at 900-1200 Hz - the vocal band.
        { const float y = cMidHP[ci] * (midHPy + x - midHPx);
          midHPx = x;  midHPy = y; }
        midLP += cMidLP[ci] * (midHPy - midLP);
        const float xMid = midLP;

        // Passive sum - no flat bleed, all three bands independently controlled.
        data[n] = kIL * (xBass * bassGain + xMid * midGain + xTre * treGain);
    }
}

//==============================================================================
// PowerAmp
// PI feel -> push-pull -> sag -> NFB -> resonance/depth -> post LP -> presence.

void PowerAmp::prepare (double sampleRate, int maxBlockSize)
{
    const float sr = (float) sampleRate;
    paSR = sampleRate;

    // Pre-power band-limit: shapes signal before power tubes, per channel warmth.
    cPwrLP[0] = lpfCoeff (5000.f, sr);
    cPwrLP[1] = lpfCoeff (5500.f, sr);
    cPwrLP[2] = lpfCoeff (6000.f, sr);

    // Post-power anti-fizz: two-pole cascaded roll-off
    cPostLP[0] = lpfCoeff (6500.f, sr);
    cPostLP[1] = lpfCoeff (6000.f, sr);
    cPostLP[2] = lpfCoeff (5500.f, sr);

    // Phase inverter envelope at native SR; the loop uses the oversampled pair.
    cPiAtk = std::exp (-1.f / (0.001f * sr));
    cPiRel = std::exp (-1.f / (0.020f * sr));

    // Sag: 8 ms attack (supply droops), 250 ms release (cap recharge -> bloom).
    cSagAtk = std::exp (-1.f / (0.008f * sr));
    cSagRel = std::exp (-1.f / (0.250f * sr));
    // Screen: 2 ms down, 60 ms back. The plate node is 8 / 250 - the screen cap is
    // a fraction of the reservoir, so it empties and refills far quicker.
    cScrAtk = std::exp (-1.f / (0.002f * sr));
    cScrRel = std::exp (-1.f / (0.060f * sr));

    // NFB low-pass (~120 Hz) and resonance two-pole LP (~100 Hz).
    cNFBLP = lpfCoeff (120.f, sr);
    cResLP = lpfCoeff (100.f, sr);

    // Presence shelf HP (~2.5 kHz)
    cPresHP = hpfAlpha (2500.f, sr);

    // Oversampling for PI + push-pull; per-factor coefficients are built below.
    paOvsBuf.allocate ((size_t) kMaxFactor * (size_t) maxBlockSize, true);
    buildOsTables();
    selectOs (2);

    reset();
}

// Precompute all-factor coefficients once (prepare). Never on the audio thread.
void PowerAmp::buildOsTables()
{
    static constexpr int kF[4] = { 1, 2, 4, 8 };
    for (int i = 0; i < 4; ++i)
    {
        const float osr = (float) paSR * (float) kF[i];
        tPiAtk[i] = std::exp (-1.f / (0.001f * osr));
        tPiRel[i] = std::exp (-1.f / (0.020f * osr));
        tPaOsLP[i] = OsBiquadChain::design (kOsCutoffFrac * (float) paSR, osr);
    }
}

// RT-safe: copy the chosen factor's coefficients into the active set.
void PowerAmp::selectOs (int factor)
{
    paFactor = juce::jlimit (1, kMaxFactor, factor);
    const int i = osIdx (paFactor);
    cPiAtk2x = tPiAtk[i];  cPiRel2x = tPiRel[i];
    paOsUp.active = tPaOsLP[i];  paOsDown.active = tPaOsLP[i];
}

void PowerAmp::reset()
{
    piEnv = pwrLP = sagEnv = nfbLP = 0.f;
    scrEnv = 0.f;
    resLP1 = resLP2 = postLP1 = postLP2 = 0.f;
    presHPx = presHPy = 0.f;
    paOsUp.reset();  paOsDown.reset();
}

void PowerAmp::process (float* data, int numSamples,
                         float master, float presence,
                         int ci, int factor, float tubeTemp, float rectMul, PowerSupply& psu)
{
    if (factor != paFactor) selectOs (factor);   // RT-safe: table lookup, no exp
    // Master range: low is tighter, high runs slightly above unity so the output
    // transformer and speaker compression are driven naturally.
    const float level = 0.32f + master * 0.72f;

    // Phase inverter drive: the power section's authority, odd harmonics and grind.
    static constexpr float kPiDrive[3] = { 0.18f, 0.50f, 0.78f };
    // Push-pull. This term is a COMPRESSOR, not a harmonic generator: raising it
    // squashes the crest of an already saturated wave, costing pick dynamics and
    // THD. It is the most effective channel-level equaliser in the amp, though.
    static constexpr float kPwrK[3]    = { 0.10f, 0.18f, 0.25f };
    // Sag: like kPwrK, pure level-dependent compression. A feel control, not tone.
    static constexpr float kSag[3]     = { 0.06f, 0.11f, 0.16f };
    // NFB: CH3 barely any feedback, open and vowel-like; CH1 tightest.
    static constexpr float kNFB[3]     = { 0.22f, 0.14f, 0.04f };
    // Resonance: CH2/CH3 carry more low-mid body.
    static constexpr float kRes[3]     = { 0.10f, 0.15f, 0.20f };

    // Step 1: Pre-power band-limit at native SR
    for (int n = 0; n < numSamples; ++n)
    {
        pwrLP += cPwrLP[ci] * (data[n] - pwrLP);
        data[n] = pwrLP;
    }

    // Step 2: Upsample by 'factor' - zero insertion + anti-imaging LP
    const int   F      = paFactor;
    const int   ovsLen = F * numSamples;
    const float upGain = (float) F;
    for (int n = 0; n < numSamples; ++n)
    {
        paOvsBuf[F * n] = data[n] * upGain;
        for (int k = 1; k < F; ++k) paOvsBuf[F * n + k] = 0.0f;
    }
    for (int n = 0; n < ovsLen; ++n) paOvsBuf[n] = paOsUp.process (paOvsBuf[n]);

    // Step 3: phase inverter + push-pull at the oversampled SR, with the
    // oversampled envelope coefficients so the times stay equal.
    for (int n = 0; n < ovsLen; ++n)
    {
        float x = paOvsBuf[n];

        // LONG-TAILED PAIR phase inverter.
        // A real Marshall or Mesa PI runs both triodes as a differential pair over a
        // shared tail resistor. The tail current is SHARED, so as one side conducts
        // harder the other starves and the halves clip by DIFFERENT amounts. That
        // asymmetry between the halves is much of a pushed power section's sound.
        {
            const float absX = std::abs (x);
            piEnv = (absX > piEnv) ? cPiAtk2x * piEnv + (1.f - cPiAtk2x) * absX
                                   : cPiRel2x * piEnv + (1.f - cPiRel2x) * absX;
            const float g = 1.f + HarmonicDrive::phaseInv * kPiDrive[ci] * (1.f + piEnv);
            const float a =  x * g;     // one plate
            const float b = -x * g;     // the other, inverted
            // Real triodes are never matched, so the halves get different knees.
            const float sa = a / (1.f + 0.13f * std::abs (a));
            const float sb = b / (1.f + 0.11f * std::abs (b));
            // Shared tail: total current is limited, so heavy conduction on one
            // side starves the other. Scaled by master, and kept shallow - deeper
            // tail limiting costs pick dynamics, and asymmetry is what is wanted.
            const float tail = 1.f / (1.f + (0.06f + 0.16f * master)
                                            * (std::abs (sa) + std::abs (sb)));
            x = (sa - sb) * 0.5f * tail / g;
        }

        // Push-pull - EL34-style asymmetric limiter weighted by master. The positive
        // half clips harder (grid current onset), which is the EL34 upper-mid grind.
        const float pwrKeff = HarmonicDrive::pushPull * kPwrK[ci] * (0.60f + 0.95f * master);
        // HOT POWER TUBES. A hot-biased output stage idles at more cathode current
        // and further into class A: an operating-point offset and the even harmonics
        // with it. Sag and kPwrK are deliberately not touched, both being pure
        // compression. The offset's DC is subtracted back out; an output transformer
        // cannot pass DC and the offset would swell notes.
        const float pb   = 0.18f * tubeTemp * (0.5f + 0.5f * master);
        const float xb   = x + pb;
        const float sat  = (xb >= 0.f) ? xb / (1.f + pwrKeff * 1.20f * xb * xb)
                                       : xb / (1.f + pwrKeff * 0.80f * xb * xb);
        const float satB = pb / (1.f + pwrKeff * 1.20f * pb * pb);
        x = sat - satB;

        paOvsBuf[n] = x;
    }

    // Step 4: Anti-alias LP + decimate back to native SR
    for (int n = 0; n < ovsLen; ++n) paOvsBuf[n] = paOsDown.process (paOvsBuf[n]);
    for (int n = 0; n < numSamples; ++n)
        data[n] = paOvsBuf[F * n];

    // Step 5: Sag + NFB + resonance + anti-fizz + presence at native SR
    for (int n = 0; n < numSamples; ++n)
    {
        float x = data[n];

        // Sag - supply droop: fast attack compresses, slow release blooms.
        {
            const float absX = std::abs (x);
            if (absX > sagEnv)
                sagEnv += (1.f - cSagAtk) * (absX - sagEnv);
            else
                sagEnv  = cSagRel * sagEnv;
        }
        // BOUNDED. Any envelope that touches gain gets a ceiling: this one feeds
        // both the sag gain and the NFB term. 2.5 only catches the runaway.
        const float sagC = juce::jmin (sagEnv, 2.5f);

        // Screen grid droop. Screen current goes as the square of the drive where
        // plate current is closer to linear, so the screen gives way abruptly once
        // the tube is working - that squareness IS the choke. It recovers in 60 ms
        // against the plate's 250, which is the bloom afterwards.
        //
        // Squared, with no threshold. A threshold version measured nothing: the
        // signal here sits around 0.125, not the ~0.6 I had assumed, so a cubic law
        // with a 0.10 knee produced 4e-4 and vanished. Worth knowing the operating
        // level of a node before writing a law that depends on it.
        {
            const float ax   = std::abs (x);
            const float scrI = ax * ax;
            const float c    = (scrI > scrEnv) ? cScrAtk : cScrRel;
            scrEnv += (1.f - c) * (scrI - scrEnv);
        }
        const float scrC = juce::jmin (scrEnv, 3.0f);

        // The same current that drops B+ here drops it everywhere. Hand it to the
        // shared rail; the preamp reads it at the top of the next block, which is
        // well inside the 30 / 320 ms the RC downstream takes to respond anyway.
        psu.draw (sagC + 0.4f * scrC);
        // Sag scales with master and with the RECTIFIER type: a tube rectifier has
        // real internal resistance, so B+ drops under draw. Silicon barely sags.
        const float sagScale = (0.50f + 1.15f * master) * rectMul;
        x *= 1.f / (1.f + kSag[ci] * sagScale * sagC);

        // Screens droop on the same rail, so their current belongs to it too.
        // Halved from a first pass at 2.0 / 4.5 / 7.5, which took CRUNCH's pick
        // dynamics under the 0.18 floor. Screen droop is a texture on the attack,
        // not another compressor.
        static constexpr float kScreen[3] = { 1.0f, 2.2f, 3.5f };
        x *= 1.f / (1.f + kScreen[ci] * sagScale * scrC);

        // NFB - subtracts LP bass content: tighter at rest, opens when pushed.
        {
            nfbLP += cNFBLP * (x - nfbLP);
            // CLAMPED AT ZERO. Above sagC = 2 the factor went negative, turning
            // negative feedback into unlimited positive bass feedback.
        x -= kNFB[ci] * juce::jmax (0.f, 1.f - sagC * 0.5f) * nfbLP;
        }

            // Resonance / depth: two-pole LP at ~100 Hz, scaling with master.
        {
            resLP1 += cResLP * (x - resLP1);
            resLP2 += cResLP * (resLP1 - resLP2);
            const float resAmt = kRes[ci] * (0.60f + 0.80f * master);
            x += resAmt * resLP2;
        }

        // Anti-fizz two-pole LP
        postLP1 += cPostLP[ci] * (x       - postLP1);
        postLP2 += cPostLP[ci] * (postLP1 - postLP2);
        x = postLP2;

        // Presence shelf - HP at ~2.5 kHz blended in post-saturation.
        {
            const float y = cPresHP * (presHPy + x - presHPx);
            presHPx = x;  presHPy = y;
            // Stronger presence shelf: Marshall-style NFB-loop feel - wide, musical, audible.
            x += presence * 0.92f * y;
        }

        data[n] = x * level;
    }
}

//==============================================================================
// ReactiveLoad
// Reactive speaker load - runs between PowerAmp and SpeakerSim.

void ReactiveLoad::prepare (double sampleRate)
{
    const float sr = (float) sampleRate;

    // Two-pole resonant LP at ~100 Hz: the cone resonance zone, shaped by kQ.
    cImpLP = lpfCoeff (100.f, sr);

    // Back EMF LP at ~32 Hz: slow enough to bloom, fast enough to follow attacks.
    cBEMFLP = lpfCoeff (32.f, sr);

    // Damping envelope: 3ms attack (catches fast transients), 60ms release.
    cDampAtk = std::exp (-1.f / (0.003f * sr));
    cDampRel = std::exp (-1.f / (0.060f * sr));

    reset();
}

void ReactiveLoad::reset()
{
    impLP1 = impLP2 = bemfLP = dampEnv = 0.f;
}

void ReactiveLoad::process (float* data, int numSamples, int ci)
{
    // Per-channel speaker load character.
    //   kQ:        Q-like feedback in the resonant LP. Low = damped and tight,
    //              high = the cone resonates freely, elastic and vocal.
    //   kImpBoost: resonance bandpass added back, modelling the voltage rise where
    //              speaker impedance peaks. Low-mid body.
    //   kBEMF:     back EMF - the slow-tracked bandpass fed back, so notes sustain.
    //   kDamp:     damping from signal energy. High = tight, low = breathing.
    static constexpr float kQ[3]        = { 0.14f, 0.30f, 0.38f };
    // ImpBoost: subtle CH2/CH3 lift for cab-like 80-100 Hz thump on palm mutes.
    static constexpr float kImpBoost[3] = { 0.05f, 0.12f, 0.19f };
    // BEMF: CH3 highest, for elastic resonance sustain in the low band.
    static constexpr float kBEMF[3]     = { 0.04f, 0.09f, 0.16f };
    // Damping: CH3 lowest, so the resonance breathes and the cone moves elastically.
    static constexpr float kDamp[3]     = { 0.22f, 0.11f, 0.06f };

    for (int n = 0; n < numSamples; ++n)
    {
        float x = data[n];

        // 1. Resonant two-pole LP at ~100 Hz. Positive feedback (kQ * xBP) raises
        // the Q; the bandpass peaks at 60-80 Hz. kQ < 1 keeps the poles stable.
        impLP1 += cImpLP * (x + kQ[ci] * (impLP1 - impLP2) - impLP1);
        impLP2 += cImpLP * (impLP1 - impLP2);
        const float xBP = impLP1 - impLP2;

        // 2. Cabinet impedance peak: speaker Z peaks at resonance, so more voltage
        // appears across it. Palm mutes feel physical, chords gain low-mid warmth.
        x += kImpBoost[ci] * xBP;

        // 3. Back EMF elastic feedback: the bandpass tracked by a slow LP and added
        // back, which extends the resonance zone briefly after each transient.
        bemfLP += cBEMFLP * (xBP - bemfLP);
        x += kBEMF[ci] * bemfLP;

        // 4. Dynamic damping from signal energy: harder playing damps the resonance
        // more. High kDamp is controlled and clean, low kDamp breathes.
        {
            const float absX = std::abs (x);
            dampEnv = (absX > dampEnv)
                    ? cDampAtk * dampEnv + (1.f - cDampAtk) * absX
                    : cDampRel * dampEnv + (1.f - cDampRel) * absX;
            const float dampAmt = juce::jmin (kDamp[ci] * dampEnv, 0.30f);
            x -= dampAmt * xBP;
        }

        data[n] = x;
    }
}

//==============================================================================
// OutputTransformer
// Iron saturation, magnetic hysteresis feel, B+ supply sag and bloom.

void OutputTransformer::prepare (double sampleRate)
{
    const float sr = (float) sampleRate;
    cXfLP   = lpfCoeff (400.f,  sr);
    cHystLP = lpfCoeff (80.f,   sr);
    cSupAtk = std::exp (-1.f / (0.0035f * sr));   // 3.5 ms - snappier sag onset (more touch feel)
    cSupRel = std::exp (-1.f / (0.150f  * sr));   // 150 ms
    cResAtk = std::exp (-1.f / (0.003f  * sr));   // 3 ms
    // 400 ms: longer releases leave the bloom audible as a swell after the attack.
    cResRel = std::exp (-1.f / (0.400f  * sr));   // 400 ms
    reset();
}

void OutputTransformer::reset()
{
    xfLP1 = xfLP2 = xfHyst = 0.f;
    supSag = supRes = 0.f;
}

void OutputTransformer::process (float* data, int numSamples, int ci)
{
    // LF saturation drive: iron-core saturation at low frequencies. The LF band is
    // normalised back out, so this is safe to push but buys little measurable THD.
    static constexpr float kXfDrive[3] = { 2.20f, 3.20f, 4.20f };

    // Hysteresis: iron memory that thickens sustained notes. Additive, but it feeds
    // back an 80 Hz lowpass, so it adds low-mid density rather than harmonics.
    static constexpr float kHyst[3]    = { 0.05f, 0.09f, 0.15f };

    // Supply sag: sets how much the supply breathes under a hard attack.
    static constexpr float kSupSag[3]  = { 0.03f, 0.055f, 0.085f };

    // Bloom: the note expands after the attack as the reservoir recovers. It has to
    // stay subtle - past about +1.3 dB it reads as swelling rather than sustain.
    static constexpr float kBloom[3]   = { 0.13f, 0.27f, 0.45f };

    const float drive = kXfDrive[ci];

    for (int n = 0; n < numSamples; ++n)
    {
        float x = data[n];

        // 1. LF isolation: two-pole LP at ~400 Hz
        xfLP1 += cXfLP * (x - xfLP1);
        xfLP2 += cXfLP * (xfLP1 - xfLP2);
        const float lf = xfLP2;
        const float hf = x - lf;   // upper mids + highs stay clean

        // 2. Iron saturation: LF-first and asymmetric - the positive half has
        // slightly more headroom, the negative half clips marginally earlier.
        const float lfDrv = lf * drive;
        const float lfSat = (lfDrv >= 0.f)
                            ?  lfDrv / (1.f + 0.10f *  lfDrv)
                            :  lfDrv / (1.f - 0.15f * -lfDrv);
        const float lfOut = lfSat / drive;   // normalize back to input scale

        // 3. Magnetic hysteresis: an 80 Hz lowpass of x is where the core was a
        // cycle ago. A small fraction added back gives low-mid density.
        xfHyst += cHystLP * (x - xfHyst);
        x = lfOut + hf + HarmonicDrive::transformer * kHyst[ci] * xfHyst;

        // 4. Supply sag: fast attack, moderate release
        const float absX = std::abs (x);
        if (absX > supSag) supSag += (1.f - cSupAtk) * (absX - supSag);
        else               supSag  = cSupRel * supSag;

        // 5. Reservoir capacitor: tracks sag up quickly, falls back over 400 ms.
        // When supSag decays into sustain, supRes > supSag and the note blooms.
        if (supSag > supRes) supRes += (1.f - cResAtk) * (supSag - supRes);
        else                 supRes  = cResRel * supRes;

        // Supply headroom compression (level-dependent, not harmonic distortion).
        const float supGain = 1.f / (1.f + kSupSag[ci] * supSag);

        // Bloom: positive only in the recovery window, 100-250 ms after the attack.
        const float bloom = kBloom[ci] * juce::jmax (0.f, supRes - supSag);

        data[n] = x * supGain * (1.f + bloom);
    }
}

//==============================================================================
// SpeakerSim
// Dynamic speaker / cab interaction - runs between PowerAmp and IR.

void SpeakerSim::prepare (double sampleRate)
{
    const float sr = (float) sampleRate;

    // HF extractor: HP at ~3 kHz isolates brightness. Envelope 2 ms / 60 ms.
    cHFHP  = hpfAlpha (3000.f, sr);
    cHFAtk = std::exp (-1.f / (0.002f * sr));
    cHFRel = std::exp (-1.f / (0.060f * sr));

    // Cone inertia LP pair: bright is the cone at rest, dark is under HF load.
    // The HF envelope blends between them, so there is no static darkening.
    cDynBrt = lpfCoeff (14000.f, sr);   // brighter cone-at-rest = more air/string clarity
    cDynDrk = lpfCoeff (7000.f,  sr);

    // Cone resonance: two-pole LP at ~140 Hz. A fraction added back gives body.
    cResLP = lpfCoeff (140.f, sr);

    // Speaker compression: models voice-coil impedance rise at high excursion.
    cCmpAtk = std::exp (-1.f / (0.001f * sr));
    cCmpRel = std::exp (-1.f / (0.100f * sr));

    // Pre-IR ultrasonic cleanup: gentle LP at ~16 kHz, above the guitar range.
    cPreLP = lpfCoeff (16000.f, sr);

    reset();
}

void SpeakerSim::reset()
{
    hfX = hfY = hfEnv = 0.f;
    dynBrt = dynDrk = 0.f;
    resLP1 = resLP2 = 0.f;
    cmpEnv = 0.f;
    preLP  = 0.f;
}

void SpeakerSim::process (float* data, int numSamples, int ci)
{
    // Per-channel speaker loading feel.
    //   kDynDepth: how far the HF envelope pushes toward the dark LP (cone inertia).
    //   kResAmt:   fraction of the two-pole cone-resonance path added back.
    static constexpr float kDynDepth[3] = { 0.08f, 0.13f, 0.18f };
    // ResAmt: more cone resonance at ~140 Hz gives palm mutes cab-like depth.
    static constexpr float kResAmt[3]   = { 0.03f, 0.07f, 0.11f };
    // High compression threshold, so only extreme peaks are touched.
    static constexpr float kCmpThresh   = 0.20f;

    for (int n = 0; n < numSamples; ++n)
    {
        float x = data[n];

        // 1. HF extractor + envelope. Content above ~3 kHz senses brightness: pick
        // transients drive it, palm mutes do not.
        {
            const float hfOut = cHFHP * (hfY + x - hfX);
            hfX = x;  hfY = hfOut;
            const float absHF = std::abs (hfOut);
            hfEnv = (absHF > hfEnv) ? cHFAtk * hfEnv + (1.f - cHFAtk) * absHF
                                    : cHFRel * hfEnv + (1.f - cHFRel) * absHF;
        }

        // 2. Dynamic cone-inertia LP. Two fixed LPs run continuously and the HF
        // envelope blends between them, so the cone only darkens under bright playing.
        {
            dynBrt += cDynBrt * (x - dynBrt);
            dynDrk += cDynDrk * (x - dynDrk);
            const float blend = juce::jmin (hfEnv * kDynDepth[ci], 1.f);
            x = dynBrt + blend * (dynDrk - dynBrt);
        }

        // 3. Cone resonance interaction: a two-pole LP at ~140 Hz for the cone's
        // mechanical resonance, complementing the 100 Hz power-amp resonance.
        {
            resLP1 += cResLP * (x - resLP1);
            resLP2 += cResLP * (resLP1 - resLP2);
            x += kResAmt[ci] * resLP2;
        }

        // 4. Speaker compression: voice-coil impedance rise at maximum excursion.
        {
            const float absX = std::abs (x);
            cmpEnv = (absX > cmpEnv) ? cCmpAtk * cmpEnv + (1.f - cCmpAtk) * absX
                                     : cCmpRel * cmpEnv + (1.f - cCmpRel) * absX;
            if (cmpEnv > kCmpThresh)
                x *= 0.93f + 0.07f * (kCmpThresh / cmpEnv);
        }

        // 5. Pre-IR cleanup: a gentle LP at ~16 kHz, above the guitar range.
        preLP += cPreLP * (x - preLP);

        data[n] = preLP;
    }
}

//==============================================================================
// IRSection

void IRSection::prepare (const juce::dsp::ProcessSpec& spec)
{
    preparedChannels = (int) spec.numChannels;
    convA.prepare (spec);  convA.reset();
    convB.prepare (spec);  convB.reset();
    const int maxSamples = (int) spec.maximumBlockSize;
    dryBuf.setSize (preparedChannels, maxSamples, false, true, true);
    wetBuf.setSize (preparedChannels, maxSamples, false, true, true);
    const float fsr = (float) spec.sampleRate;
    cAirHP  = hpfAlpha (6000.f, fsr);
    cPresHP = hpfAlpha (2000.f, fsr);
    cPostAtk  = std::exp (-1.f / (0.002f * fsr));
    cPostRel  = std::exp (-1.f / (0.120f * fsr));
    cCabResLP = lpfCoeff  (150.f, fsr);
    cPostClean = lpfCoeff (11000.f, fsr);   // anti-alias cleanup after post-IR nonlinearity
    airHPxPrev[0]  = airHPxPrev[1]  = airHPy[0]  = airHPy[1]  = 0.f;
    presHPxPrev[0] = presHPxPrev[1] = presHPy[0] = presHPy[1] = 0.f;
    postEnv[0]    = postEnv[1]    = 0.f;
    cabResLP1[0]  = cabResLP1[1]  = 0.f;
    cabResLP2[0]  = cabResLP2[1]  = 0.f;
    postClean[0]  = postClean[1]  = 0.f;
}

void IRSection::reset()
{
    convA.reset();
    convB.reset();
    dryBuf.clear();
    wetBuf.clear();
    airHPxPrev[0]  = airHPxPrev[1]  = airHPy[0]  = airHPy[1]  = 0.f;
    presHPxPrev[0] = presHPxPrev[1] = presHPy[0] = presHPy[1] = 0.f;
    postEnv[0]    = postEnv[1]    = 0.f;
    cabResLP1[0]  = cabResLP1[1]  = 0.f;
    cabResLP2[0]  = cabResLP2[1]  = 0.f;
    postClean[0]  = postClean[1]  = 0.f;
}

int IRSection::latencySamples()
{
    return (int) convA.getLatency();
}

// Cabinet IRs are short, so the tap count is capped: a mistakenly loaded reverb
// IR would otherwise balloon CPU and latency. JUCE normalises the IR, so loud and
// quiet cabs land at a consistent level (see kIRMakeup in process()).
// loadImpulseResponse is asynchronous and reports nothing, so the file has to be
// opened first, or an unreadable one leaves the whole IR path running through an
// empty convolution engine while the UI reports a loaded cab. Message thread only.
bool IRSection::irIsReadable (const juce::File& file)
{
    if (! file.existsAsFile() || file.getSize() <= 0)
        return false;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    return r != nullptr && r->numChannels > 0 && r->lengthInSamples > 0;
}

bool IRSection::loadIRA (const juce::File& file)
{
    if (! irIsReadable (file))
    {
        // Remember what was asked for so the UI can name it, but do not claim it
        // loaded and do not touch an IR that is already loaded and working.
        irAPath = file.getFullPathName();
        irAName = file.getFileNameWithoutExtension();
        irAMissing.store (true);
        return false;
    }
    juce::File f = file;
    convA.loadImpulseResponse (std::move (f),
        juce::dsp::Convolution::Stereo::yes,
        juce::dsp::Convolution::Trim::yes, 4096,
        juce::dsp::Convolution::Normalise::yes);
    irAName = file.getFileNameWithoutExtension();
    irAPath = file.getFullPathName();
    irAMissing.store (false);
    irALoaded.store (true);
    return true;
}

bool IRSection::loadIRB (const juce::File& file)
{
    if (! irIsReadable (file))
    {
        irBPath = file.getFullPathName();
        irBName = file.getFileNameWithoutExtension();
        irBMissing.store (true);
        return false;
    }
    juce::File f = file;
    convB.loadImpulseResponse (std::move (f),
        juce::dsp::Convolution::Stereo::yes,
        juce::dsp::Convolution::Trim::yes, 4096,
        juce::dsp::Convolution::Normalise::yes);
    irBName = file.getFileNameWithoutExtension();
    irBPath = file.getFullPathName();
    irBMissing.store (false);
    irBLoaded.store (true);
    return true;
}

void IRSection::clearIRA()
{
    convA.reset();
    irALoaded.store (false);
    irAMissing.store (false);
    irAName = {};
    irAPath = {};
}

void IRSection::clearIRB()
{
    convB.reset();
    irBLoaded.store (false);
    irBMissing.store (false);
    irBName = {};
    irBPath = {};
}

// blend: 0 = full A, 1 = full B.  mix: 0 = dry, 1 = wet.
// With no IR loaded the buffer passes through unchanged and the plugin runs as a
// head-only chain. irBlend only matters when both A and B are loaded.
void IRSection::process (juce::AudioBuffer<float>& buffer,
                          int numSamples, float blend, float mix)
{
    const bool hasA = irALoaded.load();
    const bool hasB = irBLoaded.load();

    if ((!hasA && !hasB) || mix < 0.001f)
        return;   // head-only mode: no IR loaded, or mix knob at zero

    const int ch = juce::jmin (preparedChannels, buffer.getNumChannels());

    // Pre-IR harmonic exciter: a mild voice-coil nonlinearity before convolution,
    // so the cab IR propagates the generated harmonics through its resonant peaks.
    for (int c = 0; c < ch; ++c)
    {
        float* p = buffer.getWritePointer (c);
        for (int n = 0; n < numSamples; ++n)
        {
            const float x = p[n];
            const float sat = (x >= 0.f) ? x / (1.f + 0.40f * x)
                                         : x / (1.f - 0.60f * x);
            p[n] = x + 0.18f * (sat - x);
        }
    }

    for (int c = 0; c < ch; ++c)
        dryBuf.copyFrom (c, 0, buffer, c, 0, numSamples);

    if (hasA && hasB)
    {
        for (int c = 0; c < ch; ++c)
            wetBuf.copyFrom (c, 0, dryBuf, c, 0, numSamples);
        {
            juce::dsp::AudioBlock<float> blkA (wetBuf.getArrayOfWritePointers(),
                                               (size_t) ch, (size_t) numSamples);
            convA.process (juce::dsp::ProcessContextReplacing<float> (blkA));
        }

        for (int c = 0; c < ch; ++c)
            buffer.copyFrom (c, 0, dryBuf, c, 0, numSamples);
        {
            juce::dsp::AudioBlock<float> blkB (buffer.getArrayOfWritePointers(),
                                               (size_t) ch, (size_t) numSamples);
            convB.process (juce::dsp::ProcessContextReplacing<float> (blkB));
        }

        // Cross-blend A+B with makeup gain, then apply the wet/dry mix. kIRMakeup
        // recovers the cabinet's insertion loss: the wet path sits about 6 dB down.
        static constexpr float kIRMakeup = 2.0f;  // +6 dB - recovers insertion loss
        const float wA = 1.f - blend, wB = blend;
        for (int c = 0; c < ch; ++c)
        {
            float*       wet  = buffer.getWritePointer (c);
            const float* wetA = wetBuf.getReadPointer (c);
            const float* dry  = dryBuf.getReadPointer (c);
            for (int n = 0; n < numSamples; ++n)
                wet[n] = dry[n] * (1.f - mix) + kIRMakeup * (wetA[n] * wA + wet[n] * wB) * mix;
        }
    }
    else
    {
        static constexpr float kIRMakeup = 2.0f;
        auto& conv = hasA ? convA : convB;
        {
            juce::dsp::AudioBlock<float> blk (buffer.getArrayOfWritePointers(),
                                              (size_t) ch, (size_t) numSamples);
            conv.process (juce::dsp::ProcessContextReplacing<float> (blk));
        }
        for (int c = 0; c < ch; ++c)
        {
            float*       wet = buffer.getWritePointer (c);
            const float* dry = dryBuf.getReadPointer (c);
            for (int n = 0; n < numSamples; ++n)
                wet[n] = dry[n] * (1.f - mix) + kIRMakeup * wet[n] * mix;
        }
    }

    // Post-IR: cone nonlinearity + cab body resonance + presence + air. All of it
    // is level-adaptive: clean when quiet, more harmonics and ring when pushed.
    {
        for (int c = 0; c < ch; ++c)
        {
            float* buf = buffer.getWritePointer (c);
            for (int n = 0; n < numSamples; ++n)
            {
                float x = buf[n];

                // 1. Level envelope (2ms / 120ms)
                const float absX = std::abs (x);
                postEnv[c] = (absX > postEnv[c])
                    ? cPostAtk * postEnv[c] + (1.f - cPostAtk) * absX
                    : cPostRel * postEnv[c] + (1.f - cPostRel) * absX;

                // 2. Speaker cone nonlinearity. The negative half clips harder, so
                // the asymmetry gives 2nd-harmonic warmth; kp/kn scale with level.
                {
                    const float kp = 0.09f + 0.28f * postEnv[c];
                    const float kn = 0.16f + 0.46f * postEnv[c];
                    x = (x >= 0.f) ? x / (1.f + kp * x)
                                   : x / (1.f - kn * x);
                    // Anti-alias cleanup: the waveshaper runs at native SR, so the
                    // harmonics it folds back are band-limited (~11 kHz LP).
                    postClean[c] += cPostClean * (x - postClean[c]);
                    x = postClean[c];
                }

                // 3. Cab body resonance (~150 Hz): a resonant two-pole with a small
                // Q feedback. The amount grows with level, so harder playing rings.
                {
                    static constexpr float kCabQ = 0.22f;
                    cabResLP1[c] += cCabResLP * (x + kCabQ * (cabResLP1[c] - cabResLP2[c]) - cabResLP1[c]);
                    cabResLP2[c] += cCabResLP * (cabResLP1[c] - cabResLP2[c]);
                    const float cabBP = cabResLP1[c] - cabResLP2[c];
                    x += (0.08f + 0.30f * postEnv[c]) * cabBP;
                }

                // 4. Dynamic presence shelf (~2 kHz): a driven cab pushes 2-4 kHz.
                {
                    const float kPres = (0.14f + 0.20f * postEnv[c]) * mix;
                    const float yP = cPresHP * (presHPy[c] + x - presHPxPrev[c]);
                    presHPxPrev[c] = x;  presHPy[c] = yP;
                    x += kPres * yP;
                }

                // 5. Air shelf (~6 kHz), restrained. Real cabs roll off hard above
                // ~6 kHz, so the steely top comes from 3-5 kHz string harmonics.
                {
                    const float yA = cAirHP * (airHPy[c] + x - airHPxPrev[c]);
                    airHPxPrev[c] = x;  airHPy[c] = yA;
                    x += 0.08f * mix * yA;   // FM3 reference is very dark above ~5 kHz
                }

                buf[n] = x;
            }
        }
    }
}

//==============================================================================
// NoiseGate

void NoiseGate::prepare (double sampleRate)
{
    sr   = sampleRate;
    cAtk = std::exp (-1.f / (0.001f * (float) sampleRate));   // 1ms attack
    reset();
}

void NoiseGate::reset() { env = 0.f; lastReleaseMs = -1.f; }

void NoiseGate::process (float* data, int numSamples, float threshDb, float releaseMs)
{
    const float thresh = juce::Decibels::decibelsToGain (threshDb);
    if (releaseMs != lastReleaseMs)   // recompute the exp only when it changes
    {
        cRel = std::exp (-1.f / (releaseMs * 0.001f * (float) sr));
        lastReleaseMs = releaseMs;
    }
    for (int n = 0; n < numSamples; ++n)
    {
        const float absX = std::abs (data[n]);
        if (absX > env) env = cAtk * env + (1.f - cAtk) * absX;
        else            env = cRel * env;
        // Wide, smooth knee: fully open at thresh, fully closed ~12 dB below, with a
        // squared fade so sustain tails fade out instead of being chopped.
        const float shut = thresh * 0.25f;
        float gain;
        if      (env >= thresh) gain = 1.f;
        else if (env <= shut)   gain = 0.f;
        else { const float t = (env - shut) / (thresh - shut); gain = t * t; }
        data[n] *= gain;
    }
}

//==============================================================================
// Delay / Reverb shared helpers
namespace {
    // Smallest power of two >= n (min 2), so buffer wrap is a single AND.
    static int ceilPow2 (int n)
    {
        int p = 2;
        while (p < n) p <<= 1;
        return p;
    }
    // Linear-interpolated read from a power-of-two circular buffer. (write - delay)
    // can go negative, so + size keeps the index non-negative before masking.
    static inline float dlRead (const float* buf, int write, int mask, float delay)
    {
        const float rp = (float) write - delay;
        const int   i0 = (int) std::floor (rp);
        const float f  = rp - (float) i0;
        const int   a  = (i0 + mask + 1) & mask;
        const int   b  = (a + 1) & mask;
        return buf[a] + f * (buf[b] - buf[a]);
    }
    // One pitch-shifted voice from a delay-line shifter with two crossfading taps.
    // The taps sit half a window apart and are equal-power crossfaded, which masks
    // the wrap discontinuity.
    static inline float pitchVoice (const float* buf, int write, int mask,
                                    float baseDt, float window, float ph)
    {
        const float p2 = (ph >= 0.5f) ? ph - 0.5f : ph + 0.5f;
        // Parabolic window 4x(1-x) ~ sin(pi x): 0 at the wrap, 1 at mid. Avoids a
        // per-sample std::sin in every detune voice.
        const float g1 = 4.f * ph * (1.f - ph);
        const float g2 = 4.f * p2 * (1.f - p2);
        return g1 * dlRead (buf, write, mask, baseDt + ph * window)
             + g2 * dlRead (buf, write, mask, baseDt + p2 * window);
    }
    // Schroeder allpass over a power-of-two buffer with an
    // integer length. Advances 'write'. g is the diffusion coefficient.
    static inline float apProcess (float* buf, int& write, int mask, int len, float x, float g)
    {
        const int   r  = (write - len + mask + 1) & mask;
        const float bo = buf[r];
        const float y  = -g * x + bo;
        // Feed the OUTPUT back, not the delayed sample. The Freeverb form that was
        // here is not a true allpass: its DC gain is g/(1-g), which is unity at the
        // g = 0.5 used for diffusion but 1.5 at the g = 0.6 the spring cascade uses -
        // and eight of those in series is +28 dB. This form is unity at every
        // frequency, which is what an allpass is for.
        buf[write] = x + g * y;
        write = (write + 1) & mask;
        return y;
    }
}

//==============================================================================
// Delay

void Delay::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sr = sampleRate;
    // Max delay time = 1000 ms + headroom for modulation + interpolation guard.
    const int maxSamps = (int) (sr * 1.02) + 8;
    size = ceilPow2 (maxSamps);
    mask = size - 1;
    bufL.allocate ((size_t) size, true);
    bufR.allocate ((size_t) size, true);
    reset();
}

void Delay::reset()
{
    if (bufL.get() != nullptr) bufL.clear ((size_t) size);
    if (bufR.get() != nullptr) bufR.clear ((size_t) size);
    writeL = writeR = 0;
    dampL = dampR = 0.f;
    hpLx = hpLy = hpRx = hpRy = 0.f;
    curTime = 0.f;
    mixZ = 0.f;
    wowC = flutC = 1.f;
    wowS = flutS = 0.f;
}

// type: 0 = Digital, 1 = Analog, 2 = Tape.  mix: 0 = dry, wet is added on top.
void Delay::process (juce::AudioBuffer<float>& buffer, int numSamples,
                     int type, float timeMs, float feedback, float mix)
{
    if (size < 4) return;
    const int  nCh    = buffer.getNumChannels();
    const bool stereo = nCh >= 2;

    // Per-voice character
    float dampHz, locutHz, satAmt, wowDepth, flutDepth, wowHz, flutHz;
    switch (type)
    {
        case 1:  // Analog (BBD): darker repeats, gentle clip, slow chorus-y drift
            dampHz = 3200.f; locutHz = 130.f; satAmt = 0.18f;
            wowDepth = 1.6f; flutDepth = 0.f;   wowHz = 1.1f; flutHz = 0.f;   break;
        case 2:  // Tape: warm, wow + flutter, tape saturation
            dampHz = 4200.f; locutHz = 110.f; satAmt = 0.24f;
            wowDepth = 2.8f; flutDepth = 0.7f;  wowHz = 0.6f; flutHz = 6.3f;  break;
        default: // Digital: clean, full bandwidth, no modulation
            dampHz = 15000.f; locutHz = 22.f; satAmt = 0.f;
            wowDepth = 0.f;  flutDepth = 0.f;   wowHz = 0.f; flutHz = 0.f;    break;
    }

    const float fsr   = (float) sr;
    const float cDamp = lpfCoeff (dampHz,  fsr);
    const float cHP   = hpfAlpha (locutHz, fsr);
    const float cTime = std::exp (-1.f / (0.040f * fsr));   // delay-time glide (~40 ms)
    const float cMix  = 1.f - std::exp (-1.f / (0.008f * fsr));
    const float fb    = juce::jlimit (0.f, 0.95f, feedback);
    const float kDryCut  = 0.55f;   // dry pull-back at MIX = 1 (leaves ~45% dry)
    const float kWetTrim = 0.95f;   // delay wet level

    const float maxT   = (float) (size - 4);
    const float target = juce::jlimit (1.f, maxT, timeMs * 0.001f * fsr);
    if (curTime <= 0.f) curTime = target;   // no glide on first run

    const float wowW  = juce::MathConstants<float>::twoPi * wowHz  / fsr;
    const float flutW = juce::MathConstants<float>::twoPi * flutHz / fsr;
    const float wowRc = std::cos (wowW),  wowRs = std::sin (wowW);
    const float flRc  = std::cos (flutW), flRs  = std::sin (flutW);

    float* L = buffer.getWritePointer (0);
    float* R = stereo ? buffer.getWritePointer (1) : nullptr;

    for (int n = 0; n < numSamples; ++n)
    {
        curTime += (1.f - cTime) * (target - curTime);
        mixZ    += cMix * (mix - mixZ);

        { const float c = wowC * wowRc - wowS * wowRs;
          const float s = wowC * wowRs + wowS * wowRc;  wowC = c; wowS = s; }
        { const float c = flutC * flRc - flutS * flRs;
          const float s = flutC * flRs + flutS * flRc;  flutC = c; flutS = s; }
        const float modL =  wowDepth * wowS + flutDepth * flutS;
        const float modR = -wowDepth * wowS - flutDepth * flutS;   // opposite phase => width

        const float inL = L[n];
        const float inR = stereo ? R[n] : inL;
        const float m   = 0.5f * (inL + inR);

        if (stereo)
        {
            const float dl = dlRead (bufL, writeL, mask, juce::jlimit (1.f, maxT, curTime + modL));
            const float dr = dlRead (bufR, writeR, mask, juce::jlimit (1.f, maxT, curTime + modR));

            // Ping-pong feedback: each line recirculates the OTHER line's output.
            float toL = dr, toR = dl;
            dampL += cDamp * (toL - dampL);  toL = dampL;
            dampR += cDamp * (toR - dampR);  toR = dampR;
            { const float y = cHP * (hpLy + toL - hpLx);  hpLx = toL;  hpLy = y;  toL = y; }
            { const float y = cHP * (hpRy + toR - hpRx);  hpRx = toR;  hpRy = y;  toR = y; }
            if (satAmt > 0.f)
            {
                toL = toL / (1.f + satAmt * std::abs (toL));
                toR = toR / (1.f + satAmt * std::abs (toR));
            }

            bufL[writeL] = m  + fb * toL;   // dry injected into the left line
            bufR[writeR] =      fb * toR;   // right carries the bounced repeats
            writeL = (writeL + 1) & mask;
            writeR = (writeR + 1) & mask;

            // Compensated mix: dry eases back as MIX rises, so repeats sit in the mix.
            const float dryG = 1.f - kDryCut * mixZ;
            L[n] = inL * dryG + mixZ * kWetTrim * dl;
            R[n] = inR * dryG + mixZ * kWetTrim * dr;
        }
        else
        {
            const float d = dlRead (bufL, writeL, mask, juce::jlimit (1.f, maxT, curTime + modL));
            float to = d;
            dampL += cDamp * (to - dampL);  to = dampL;
            { const float y = cHP * (hpLy + to - hpLx);  hpLx = to;  hpLy = y;  to = y; }
            if (satAmt > 0.f) to = to / (1.f + satAmt * std::abs (to));

            bufL[writeL] = m + fb * to;
            writeL = (writeL + 1) & mask;

            const float dryG = 1.f - kDryCut * mixZ;
            L[n] = inL * dryG + mixZ * kWetTrim * d;
        }
    }
}

//==============================================================================
// Reverb

void ReverbEngine::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sr = sampleRate;
    const float ratio = (float) sr / 44100.f;

    // Predelay: up to ~60 ms
    {
        const int n = ceilPow2 ((int) (0.060 * sr) + 4);
        preBuf.allocate ((size_t) n, true);
        preMask = n - 1;  preWrite = 0;
    }
    // Input diffusion allpass lengths (Schroeder/Freeverb-style), scaled by SR.
    const int apBase[4] = { 556, 441, 341, 225 };
    for (int k = 0; k < 4; ++k)
    {
        apLen[k] = juce::jmax (4, (int) (apBase[k] * ratio));
        const int n = ceilPow2 (apLen[k] + 4);
        apBuf[k].allocate ((size_t) n, true);
        apMask[k] = n - 1;  apWrite[k] = 0;
    }
    // Spring dispersion allpass: short, mutually prime, scaled by SR.
    const int spBase[8] = { 67, 97, 113, 137, 157, 181, 199, 223 };
    for (int k = 0; k < 8; ++k)
    {
        spLen[k] = juce::jmax (4, (int) (spBase[k] * ratio));
        const int n = ceilPow2 (spLen[k] + 4);
        spBuf[k].allocate ((size_t) n, true);
        spMask[k] = n - 1;  spWrite[k] = 0;
    }
    // FDN lines: sized for the longest voice (Hall ~74 ms) + modulation headroom.
    {
        const int n = ceilPow2 ((int) (0.090 * sr) + 8);
        for (int i = 0; i < 4; ++i)
        {
            fdn[i].allocate ((size_t) n, true);
            fdnMask[i] = n - 1;  fdnWrite[i] = 0;
        }
    }
    reset();
}

void ReverbEngine::reset()
{
    if (preBuf.get() != nullptr) preBuf.clear ((size_t) (preMask + 1));
    preWrite = 0;
    for (int k = 0; k < 4; ++k) { if (apBuf[k].get() != nullptr) apBuf[k].clear ((size_t) (apMask[k] + 1)); apWrite[k] = 0; }
    for (int k = 0; k < 8; ++k) { if (spBuf[k].get() != nullptr) spBuf[k].clear ((size_t) (spMask[k] + 1)); spWrite[k] = 0; }
    for (int i = 0; i < 4; ++i)
    {
        if (fdn[i].get() != nullptr) fdn[i].clear ((size_t) (fdnMask[i] + 1));
        fdnWrite[i] = 0;  damp[i] = 0.f;
        lfoC[i] = 1.f;    lfoS[i] = 0.f;
    }
    spLp = spHpX = spHpY = 0.f;
    outHpX[0] = outHpX[1] = outHpY[0] = outHpY[1] = 0.f;
    mixZ = 0.f;
}

// type: 0 = Spring, 1 = Hall, 2 = Room, 3 = Plate.  decay/tone/mix in [0,1].
void ReverbEngine::process (juce::AudioBuffer<float>& buffer, int numSamples,
                      int type, float decay, float tone, float mix)
{
    if (fdn[0].get() == nullptr) return;
    type = juce::jlimit (0, 3, type);
    const int  nCh    = buffer.getNumChannels();
    const bool stereo = nCh >= 2;
    const float fsr   = (float) sr;

    // Per-voice tuning
    static const float kLen[4][4] = {
        { 28.0f, 35.7f, 41.3f, 47.9f },   // Spring
        { 38.3f, 49.7f, 61.1f, 73.9f },   // Hall
        { 18.1f, 24.3f, 30.7f, 36.9f },   // Room
        { 21.7f, 28.9f, 35.3f, 42.1f } }; // Plate
    static const float kPreMs[4]   = { 14.f, 28.f,  8.f, 12.f };
    static const float kT60Max[4]  = { 3.0f,  7.0f,  2.6f, 4.5f };
    static const float kModDepth[4]= { 1.8f,  1.6f,  0.9f, 1.5f };   // samples (deeper -> less metallic)
    static const float kModRate[4] = { 1.9f,  0.5f,  0.7f,  0.9f };  // Hz
    static const float kWetTrim[4] = { 0.50f, 0.52f, 0.46f, 0.52f };
    const bool isSpring = (type == 0);

    const float t60   = 0.25f + decay * (kT60Max[type] - 0.25f);
    const float dampHz = 1500.f + tone * tone * 7500.f;     // dark -> bright
    const float cDamp  = lpfCoeff (dampHz, fsr);
    const float cOutHP = hpfAlpha (110.f, fsr);
    const float cMix   = 1.f - std::exp (-1.f / (0.010f * fsr));

    float fdnLen[4], g[4], lfoRc[4], lfoRs[4];
    for (int i = 0; i < 4; ++i)
    {
        fdnLen[i] = juce::jmin ((float) (fdnMask[i] - 2), kLen[type][i] * 0.001f * fsr);
        g[i]      = std::pow (10.f, -3.f * (fdnLen[i] / fsr) / t60);
        const float w = juce::MathConstants<float>::twoPi * (kModRate[type] * (1.f + 0.13f * i)) / fsr;
        lfoRc[i] = std::cos (w);  lfoRs[i] = std::sin (w);
    }
    const float depth   = kModDepth[type];
    const float wetTrim = kWetTrim[type];
    const int   preSamps = juce::jmin (preMask - 2, (int) (kPreMs[type] * 0.001f * fsr));
    const float cSpLp = lpfCoeff (4200.f, fsr);
    const float cSpHp = hpfAlpha (170.f,  fsr);

    float* L = buffer.getWritePointer (0);
    float* R = stereo ? buffer.getWritePointer (1) : nullptr;

    for (int n = 0; n < numSamples; ++n)
    {
        mixZ += cMix * (mix - mixZ);
        const float inL = L[n];
        const float inR = stereo ? R[n] : inL;
        const float m   = 0.5f * (inL + inR);

        preBuf[preWrite] = m;
        float in = preBuf[(preWrite - preSamps + preMask + 1) & preMask];
        preWrite = (preWrite + 1) & preMask;

        // Spring dispersion front-end: mid band -> cascade of allpass (the "drip").
        if (isSpring)
        {
            spLp += cSpLp * (in - spLp);
            const float hp = cSpHp * (spHpY + spLp - spHpX);  spHpX = spLp;  spHpY = hp;
            float y = hp;
            for (int k = 0; k < 8; ++k)
                y = apProcess (spBuf[k], spWrite[k], spMask[k], spLen[k], y, 0.6f);
            in = y * 1.2f;
        }

        for (int k = 0; k < 4; ++k)
            in = apProcess (apBuf[k], apWrite[k], apMask[k], apLen[k], in, 0.5f);

        // Read + damp the four FDN lines (with modulation)
        float r[4];
        for (int i = 0; i < 4; ++i)
        {
            const float c = lfoC[i] * lfoRc[i] - lfoS[i] * lfoRs[i];
            const float s = lfoC[i] * lfoRs[i] + lfoS[i] * lfoRc[i];
            lfoC[i] = c;  lfoS[i] = s;
            const float rd = dlRead (fdn[i], fdnWrite[i], fdnMask[i], fdnLen[i] + depth * s);
            damp[i] += cDamp * (rd - damp[i]);
            r[i] = damp[i];
        }

        // Householder feedback mix (energy-preserving) + decay + input injection.
        const float sum = (r[0] + r[1] + r[2] + r[3]) * 0.5f;
        const float inj = in * 0.5f;
        for (int i = 0; i < 4; ++i)
        {
            fdn[i][fdnWrite[i]] = inj + g[i] * (r[i] - sum);
            fdnWrite[i] = (fdnWrite[i] + 1) & fdnMask[i];
        }

        // Decorrelated stereo taps
        float wetL = r[0] + 0.6f * r[2] - 0.4f * r[3];
        float wetR = r[1] + 0.6f * r[3] - 0.4f * r[2];

        { const float y = cOutHP * (outHpY[0] + wetL - outHpX[0]);  outHpX[0] = wetL;  outHpY[0] = y;  wetL = y; }
        { const float y = cOutHP * (outHpY[1] + wetR - outHpX[1]);  outHpX[1] = wetR;  outHpY[1] = y;  wetR = y; }

        // Compensated mix: dry eases back as MIX rises, so the tail blooms behind.
        const float dryG = 1.f - 0.5f * mixZ;
        const float w    = mixZ * wetTrim;
        if (stereo)
        {
            L[n] = inL * dryG + w * wetL;
            R[n] = inR * dryG + w * wetR;
        }
        else
        {
            L[n] = inL * dryG + w * 0.5f * (wetL + wetR);
        }
    }
}

//==============================================================================
// Modulation

void Modulation::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sr = sampleRate;
    // Detune line: base (2 ms) + crossfade window (60 ms) + guard.
    {
        const int n = ceilPow2 ((int) (0.075 * sr) + 8);
        dtL.allocate ((size_t) n, true);
        dtR.allocate ((size_t) n, true);
        dtMask = n - 1;
    }
    // Chorus line: base (18 ms) + depth (8 ms) + guard
    {
        const int n = ceilPow2 ((int) (0.040 * sr) + 8);
        chL.allocate ((size_t) n, true);
        chR.allocate ((size_t) n, true);
        chMask = n - 1;
    }
    reset();
}

void Modulation::reset()
{
    if (dtL.get() != nullptr) dtL.clear ((size_t) (dtMask + 1));
    if (dtR.get() != nullptr) dtR.clear ((size_t) (dtMask + 1));
    if (chL.get() != nullptr) chL.clear ((size_t) (chMask + 1));
    if (chR.get() != nullptr) chR.clear ((size_t) (chMask + 1));
    dtWriteL = dtWriteR = chWriteL = chWriteR = 0;
    dtPhL = dtPhR = 0.f;
    bgAPhL = bgAPhR = bgBPhL = bgBPhR = bgCPhL = bgCPhR = 0.f;
    chC_L = 1.f;  chS_L = 0.f;
    detZ = chorZ = 0.f;
}

// detune / chorus / rate in [0,1]. Parallel-blended so the voices sit behind the
// signal. detune = 0 and chorus = 0 -> no effect (blend folds to zero).
void Modulation::process (juce::AudioBuffer<float>& buffer, int numSamples,
                          float detune, float chorus, float rate)
{
    if (dtL.get() == nullptr) return;
    const int  nCh    = buffer.getNumChannels();
    const bool stereo = nCh >= 2;
    const float fsr   = (float) sr;
    const float cSm   = 1.f - std::exp (-1.f / (0.010f * fsr));

    const float maxCents = 16.f;
    const float window   = 0.060f * fsr;     // detune crossfade window
    const float baseDt   = 0.002f * fsr;     // min detune read delay

    // Chorus LFO (slow + lush) and depth
    const float chorusHz = 0.10f + rate * 2.2f;
    const float w        = juce::MathConstants<float>::twoPi * chorusHz / fsr;
    const float rotC = std::cos (w), rotS = std::sin (w);
    const float chBase  = 0.018f * fsr;
    const float chDepth = 0.006f * fsr;

    const float detTrim  = 0.55f;            // how far back the detune voice sits
    const float chorTrim = 0.60f;

    // Background detune ensemble: three stacked voices at growing widths,
    // alternating L/R direction, scaled by the DETUNE control.
    const float rA = std::pow (2.f,  8.f / 1200.f);    // +/- 8 cents
    const float rB = std::pow (2.f, 16.f / 1200.f);    // +/- 16 cents
    const float rC = std::pow (2.f, 27.f / 1200.f);    // +/- 27 cents
    const float incAL = (1.f - rA)       / window, incAR = (1.f - 1.f / rA) / window;  // A: L up / R down
    const float incBL = (1.f - 1.f / rB) / window, incBR = (1.f - rB)       / window;  // B: L down / R up
    const float incCL = (1.f - rC)       / window, incCR = (1.f - 1.f / rC) / window;  // C: L up / R down
    // These blends were fixed, so 22% of the signal was pitch-shifted copies whatever
    // the knob said - thickening the tone at the cost of transient definition. The
    // floor keeps a trace of ensemble at zero; the knob buys the rest.
    const float bgAmt = 0.15f + 0.85f * detune;
    const float bgAB = 0.11f * bgAmt, bgBB = 0.07f * bgAmt, bgCB = 0.045f * bgAmt;

    float* L = buffer.getWritePointer (0);
    float* R = stereo ? buffer.getWritePointer (1) : nullptr;

    for (int n = 0; n < numSamples; ++n)
    {
        detZ  += cSm * (detune - detZ);
        chorZ += cSm * (chorus - chorZ);

        const float inL = L[n];
        const float inR = stereo ? R[n] : inL;

        dtL[dtWriteL] = inL;  dtR[dtWriteR] = inR;
        chL[chWriteL] = inL;  chR[chWriteR] = inR;

        // DETUNE: L pitched up, R pitched down
        const float ratio = std::pow (2.f, (maxCents * detZ) / 1200.f);   // >= 1
        // delay shrinks for up-shift, grows for down-shift
        dtPhL += (1.f - ratio)        / window;     // up  (phase decreases)
        dtPhR += (1.f - 1.f / ratio)  / window;     // down (phase increases)
        if (dtPhL < 0.f) dtPhL += 1.f;  else if (dtPhL >= 1.f) dtPhL -= 1.f;
        if (dtPhR < 0.f) dtPhR += 1.f;  else if (dtPhR >= 1.f) dtPhR -= 1.f;

        const float detL = pitchVoice (dtL, dtWriteL, dtMask, baseDt, window, dtPhL);
        const float detR = pitchVoice (dtR, dtWriteR, dtMask, baseDt, window, dtPhR);

        // CHORUS: one quadrature LFO - sin for L, cos for R (exact 90 deg)
        { const float c = chC_L * rotC - chS_L * rotS;
          const float s = chC_L * rotS + chS_L * rotC;  chC_L = c; chS_L = s; }
        const float md   = chDepth * (0.4f + 0.6f * chorZ);
        const float chDL = chBase + md * chS_L;        // sine
        const float chDR = chBase + md * chC_L;        // cosine -> 90 deg apart
        const float choL = dlRead (chL, chWriteL, chMask, chDL);
        const float choR = dlRead (chR, chWriteR, chMask, chDR);

        bgAPhL += incAL; bgAPhR += incAR;
        bgBPhL += incBL; bgBPhR += incBR;
        bgCPhL += incCL; bgCPhR += incCR;
        if (bgAPhL < 0.f) bgAPhL += 1.f; else if (bgAPhL >= 1.f) bgAPhL -= 1.f;
        if (bgAPhR < 0.f) bgAPhR += 1.f; else if (bgAPhR >= 1.f) bgAPhR -= 1.f;
        if (bgBPhL < 0.f) bgBPhL += 1.f; else if (bgBPhL >= 1.f) bgBPhL -= 1.f;
        if (bgBPhR < 0.f) bgBPhR += 1.f; else if (bgBPhR >= 1.f) bgBPhR -= 1.f;
        if (bgCPhL < 0.f) bgCPhL += 1.f; else if (bgCPhL >= 1.f) bgCPhL -= 1.f;
        if (bgCPhR < 0.f) bgCPhR += 1.f; else if (bgCPhR >= 1.f) bgCPhR -= 1.f;
        const float bgL = bgAB * pitchVoice (dtL, dtWriteL, dtMask, baseDt, window, bgAPhL)
                        + bgBB * pitchVoice (dtL, dtWriteL, dtMask, baseDt, window, bgBPhL)
                        + bgCB * pitchVoice (dtL, dtWriteL, dtMask, baseDt, window, bgCPhL);
        const float bgR = bgAB * pitchVoice (dtR, dtWriteR, dtMask, baseDt, window, bgAPhR)
                        + bgBB * pitchVoice (dtR, dtWriteR, dtMask, baseDt, window, bgBPhR)
                        + bgCB * pitchVoice (dtR, dtWriteR, dtMask, baseDt, window, bgCPhR);

        dtWriteL = (dtWriteL + 1) & dtMask;  dtWriteR = (dtWriteR + 1) & dtMask;
        chWriteL = (chWriteL + 1) & chMask;  chWriteR = (chWriteR + 1) & chMask;

        const float db = detZ  * detTrim;
        const float cb = chorZ * chorTrim;
        if (stereo)
        {
            L[n] = inL + db * detL + cb * choL + bgL;
            R[n] = inR + db * detR + cb * choR + bgR;
        }
        else
        {
            L[n] = inL + db * 0.5f * (detL + detR) + cb * 0.5f * (choL + choR)
                       + 0.5f * (bgL + bgR);
        }
    }
}

//==============================================================================
// Parameter layout

juce::AudioProcessorValueTreeState::ParameterLayout
CopilotToneAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "channel", 1 }, "Channel",
        juce::StringArray { "CH1", "CH2", "CH3" }, 0));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "drive", 1 }, "Gain",
        juce::NormalisableRange<float> (0.f, 10.f, 0.01f), 4.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "char", 1 }, "Character",
        juce::NormalisableRange<float> (0.f, 10.f, 0.01f), 5.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "bass", 1 }, "Bass",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mid", 1 }, "Mid",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "treble", 1 }, "Treble",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "master", 1 }, "Master",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.70f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "presence", 1 }, "Presence",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.30f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "irMix", 1 }, "IR Mix",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 1.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "irBlend", 1 }, "IR Blend",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bright", 1 }, "Bright", false));

    // Noise gate is off by default and out of the signal path until enabled.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "gateOn", 1 }, "Gate On", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "gateThresh", 1 }, "Gate Threshold",
        juce::NormalisableRange<float> (-90.f, -20.f, 0.1f), -70.f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "gateRelease", 1 }, "Gate Release",
        juce::NormalisableRange<float> (10.f, 500.f, 1.f), 120.f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    // Guitar cable length. Not a tone control with an invented curve: it sets the
    // capacitance that resonates against the pickup, which is the actual reason a
    // long lead sounds duller than a short one.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "cableLen", 1 }, "Cable Length",
        juce::StringArray { "3 ft", "10 ft", "15 ft", "20 ft", "30 ft" }, 1));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "reverbType", 1 }, "Reverb Type",
        juce::StringArray { "Spring", "Hall", "Room", "Plate" }, 0));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "reverbMix", 1 }, "Reverb Mix",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "reverbDecay", 1 }, "Reverb Decay",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "reverbTone", 1 }, "Reverb Tone",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "delayType", 1 }, "Delay Type",
        juce::StringArray { "Digital", "Analog", "Tape" }, 0));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "delayMix", 1 }, "Delay Mix",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "delayTime", 1 }, "Delay Time",
        juce::NormalisableRange<float> (40.f, 1000.f, 1.f), 350.f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "delayFeedback", 1 }, "Delay Feedback",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.35f));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "eqOn", 1 }, "Graphic EQ", false));

    // Tone stack position. Post is the default voicing; Mark puts the stack ahead
    // of the preamp, so the tone knobs shape the distortion itself.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "stackPos", 1 }, "Tone Stack",
        juce::StringArray { "Post", "Mark" }, 0));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "rectifier", 1 }, "Rectifier",
        juce::StringArray { "Silicon", "Tube" }, 0));

    // Mark-series five-band graphic EQ, named by the FACEPLATE labels because that
    // is what players say, while the DSP runs the measured resonances. +/-12 dB.
    for (const auto& b : { std::pair<const char*, const char*> { "eq80",   "EQ 80" },
                           { "eq240",  "EQ 240" },
                           { "eq750",  "EQ 750" },
                           { "eq2200", "EQ 2200" },
                           { "eq6600", "EQ 6600" } })
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { b.first, 1 }, b.second,
            juce::NormalisableRange<float> (-12.f, 12.f, 0.1f), 0.f));

    // Post-FX bypasses. Additive - no existing ID changes, so older sessions still
    // recall the same. Default on.
    for (const auto& b : { std::pair<const char*, const char*> { "revOn", "Reverb On" },
                           { "dlyOn", "Delay On" },
                           { "modOn", "Modulation On" } })
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { b.first, 1 }, b.second, true));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "modDetune", 1 }, "Mod Detune",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "modChorus", 1 }, "Mod Chorus",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "modRate", 1 }, "Mod Rate",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.35f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> (-24.f, 6.f, 0.1f), 0.f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    // Oversampling factor for the nonlinear stages. Index -> 0=x1, 1=x2, 2=x4, 3=x8.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "osFactor", 1 }, "Oversampling",
        // Default x4. At x2 a high-gain LEAD setting aliases at about -34 dBc, past
        // the -40 target, and no filter fixes it. x4 measures -48, x8 -59.
        juce::StringArray { "x1", "x2", "x4", "x8" }, 2));

    // IR bypass as a real parameter so it saves with the preset and automates.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypassIR", 1 }, "Bypass IR", false));

    return layout;
}

//==============================================================================
CopilotToneAudioProcessor::CopilotToneAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       // Guitar amp: mono input (guitar on any single jack) -> stereo output.
                       // DAW hosts that need stereo in are accepted via isBusesLayoutSupported.
                       .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       apvts (*this, nullptr, "STATE", createParameterLayout())
#else
     : apvts (*this, nullptr, "STATE", createParameterLayout())
#endif
{
    // Standalone build only: request mic permission so the OS shows its dialog. In
    // a plugin the host owns the device, so no OS prompt may fire at construction.
    if (wrapperType == wrapperType_Standalone
        && ! juce::RuntimePermissions::isGranted (juce::RuntimePermissions::recordAudio))
        juce::RuntimePermissions::request (juce::RuntimePermissions::recordAudio,
                                           [] (bool /*granted*/) {});
}

CopilotToneAudioProcessor::~CopilotToneAudioProcessor() {}

//==============================================================================
const juce::String CopilotToneAudioProcessor::getName() const { return JucePlugin_Name; }
bool CopilotToneAudioProcessor::acceptsMidi()  const { return false; }
bool CopilotToneAudioProcessor::producesMidi() const { return false; }
bool CopilotToneAudioProcessor::isMidiEffect() const { return false; }
double CopilotToneAudioProcessor::getTailLengthSeconds() const
{
    // Honest tail so hosts don't truncate reverb/delay on bounce/freeze.
    double tail = 0.1;
    if (apvts.getRawParameterValue ("reverbMix")->load() > 0.001f)
        tail = juce::jmax (tail, 7.0);   // worst-case reverb RT60 (Hall)
    if (apvts.getRawParameterValue ("delayMix")->load() > 0.001f)
    {
        const double t  = (double) apvts.getRawParameterValue ("delayTime")->load() * 0.001;
        const float  fb = apvts.getRawParameterValue ("delayFeedback")->load();
        // approximate -60 dB decay = time * (-3 / log10(fb))
        const double reps = (fb > 0.05f) ? (-3.0 / std::log10 ((double) juce::jmin (0.97f, fb))) : 1.0;
        tail = juce::jmax (tail, t * reps);
    }
    return juce::jlimit (0.1, 12.0, tail);
}
int  CopilotToneAudioProcessor::getNumPrograms()    { return 1; }
int  CopilotToneAudioProcessor::getCurrentProgram() { return 0; }
void CopilotToneAudioProcessor::setCurrentProgram (int) {}
const juce::String CopilotToneAudioProcessor::getProgramName (int)  { return {}; }
void CopilotToneAudioProcessor::changeProgramName (int, const juce::String&) {}

//==============================================================================
void CopilotToneAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate   = sampleRate;
    preparedNumChannels = juce::jmin (juce::jmax (1, getTotalNumOutputChannels()), 2);

    // Prepare each DSP stage. The valves start hot on all three channels: the wait
    // for a real amp to reach its open operating point is physics, not a feature.
    // The ramp stays because temp is the one scalar the hot-tube path scales by.
    thermal.prepare (sampleRate, samplesPerBlock, 25.f, 1.0f);
    thermal.reset();
    touch.prepare (sampleRate, samplesPerBlock);
    touch.reset();
    ampEngine.prepare  (sampleRate, samplesPerBlock);
    toneStack.prepare  (sampleRate);
    toneStackEarly.prepare (sampleRate);
    graphicEQ.prepare  (sampleRate, samplesPerBlock);

    // ~50 ms for a bypass to ramp, expressed per block so the time is the same
    // whatever buffer size the host hands us.
    fxOnSm = 1.f - std::exp (-(float) samplesPerBlock
                             / juce::jmax (1.f, 0.05f * (float) sampleRate));
    revOnZ = dlyOnZ = modOnZ = 1.f;
    powerAmp.prepare   (sampleRate, samplesPerBlock);
    supply.prepare     (sampleRate);
    outputTransformer.prepare (sampleRate);
    speakerSim.prepare        (sampleRate);
    reactiveLoad.prepare      (sampleRate);
    noiseGate.prepare         (sampleRate);
    delay.prepare             (sampleRate, samplesPerBlock);
    modulation.prepare        (sampleRate, samplesPerBlock);
    reverb.prepare            (sampleRate, samplesPerBlock);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = (uint32_t) samplesPerBlock;
    spec.numChannels      = (uint32_t) preparedNumChannels;
    irSection.prepare (spec);

    // Report convolution latency so the host time-aligns the plugin.
    setLatencySamples (irSection.latencySamples());
}

void CopilotToneAudioProcessor::releaseResources()
{
    ampEngine.reset();
    toneStack.reset();
    powerAmp.reset();
    outputTransformer.reset();
    speakerSim.reset();
    reactiveLoad.reset();
    noiseGate.reset();
    delay.reset();
    modulation.reset();
    reverb.reset();
    irSection.reset();
}

//==============================================================================
#ifndef JucePlugin_PreferredChannelConfigurations
bool CopilotToneAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // Guitar amp: channel 0 is always processed and the output is stereo. The
    // output set must be mono or stereo; any input channel count is accepted.
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    // Input bus must have at least 1 channel; beyond that, any layout is accepted.
    if (layouts.getMainInputChannelSet().size() < 1)
        return false;
   #endif
    return true;
  #endif
}
#endif

//==============================================================================
void CopilotToneAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const int numIn      = getTotalNumInputChannels();
    const int numOut     = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    const int bufCh      = buffer.getNumChannels();

    if (bufCh < 1 || numSamples < 1) return;

    // Sum all input channels into channel 0, so the guitar is found on whichever
    // jack it is plugged into. The summed channels are then cleared.
    {
        float* ch0 = buffer.getWritePointer (0);
        for (int ch = 1; ch < numIn && ch < bufCh; ++ch)
        {
            const float* chN = buffer.getReadPointer (ch);
            for (int n = 0; n < numSamples; ++n)
                ch0[n] += chN[n];
            buffer.clear (ch, 0, numSamples);
        }
    }

    // Input level, measured AFTER the sum and on channel 0, because that is the
    // signal the amp receives. Before the sum it under-reads by up to 6 dB.
    {
        const float* r = buffer.getReadPointer (0);
        float peak = 0.f;
        for (int n = 0; n < numSamples; ++n)
            peak = juce::jmax (peak, std::abs (r[n]));
        raisePeak (inputPeakLin, peak);
    }

    const int   ci       = juce::jlimit (0, 2, (int) apvts.getRawParameterValue ("channel")->load());
    const float gainRaw  = apvts.getRawParameterValue ("drive")->load();
    const float bass     = apvts.getRawParameterValue ("bass")->load();
    const float mid      = apvts.getRawParameterValue ("mid")->load();
    const float treble   = apvts.getRawParameterValue ("treble")->load();
    const float master   = apvts.getRawParameterValue ("master")->load();
    const float presence = apvts.getRawParameterValue ("presence")->load();
    const float irMix    = apvts.getRawParameterValue ("irMix")->load();
    const float irBlend  = apvts.getRawParameterValue ("irBlend")->load();
    const float outGain  = juce::Decibels::decibelsToGain (
                               apvts.getRawParameterValue ("output")->load());
    const bool  gateOn      = apvts.getRawParameterValue ("gateOn")->load() > 0.5f;
    const float gateThresh  = apvts.getRawParameterValue ("gateThresh")->load();
    const float gateRelease = apvts.getRawParameterValue ("gateRelease")->load();

    const int   delayType   = juce::jlimit (0, 2, (int) apvts.getRawParameterValue ("delayType")->load());
    const float delayTime   = apvts.getRawParameterValue ("delayTime")->load();
    const float delayFb     = apvts.getRawParameterValue ("delayFeedback")->load();
    const float delayMix    = apvts.getRawParameterValue ("delayMix")->load();
    const int   reverbType  = juce::jlimit (0, 3, (int) apvts.getRawParameterValue ("reverbType")->load());
    const float reverbDecay = apvts.getRawParameterValue ("reverbDecay")->load();
    const float reverbTone  = apvts.getRawParameterValue ("reverbTone")->load();
    const float reverbMix   = apvts.getRawParameterValue ("reverbMix")->load();
    ampEngine.setCableLength ((int) apvts.getRawParameterValue ("cableLen")->load());
    const bool  revOn       = apvts.getRawParameterValue ("revOn")->load() > 0.5f;
    const bool  dlyOn       = apvts.getRawParameterValue ("dlyOn")->load() > 0.5f;
    const bool  modOn       = apvts.getRawParameterValue ("modOn")->load() > 0.5f;
    const float modDetune   = apvts.getRawParameterValue ("modDetune")->load();
    const float modChorus   = apvts.getRawParameterValue ("modChorus")->load();
    const float modRate     = apvts.getRawParameterValue ("modRate")->load();

    const float gainNorm = gainRaw / 10.f;   // 0-1 linear
    const float charNorm = apvts.getRawParameterValue ("char")->load() / 10.f;
    const bool brightEnabled = apvts.getRawParameterValue ("bright")->load() > 0.5f;

    // Oversampling factor: param index 0..3 -> 1x / 2x / 4x / 8x.
    static constexpr int kOsFactor[4] = { 1, 2, 4, 8 };
    const int osIdx  = juce::jlimit (0, 3, (int) apvts.getRawParameterValue ("osFactor")->load());
    const int factor = kOsFactor[osIdx];

    // Cathode temperature: one advance per block. Everything hot-tube reads this.
    thermal.advance();
    const float tubeTemp = thermal.temp;

    // DISABLED, and it must stay disabled unless the idea itself changes.
    // TouchRestore did what it was designed to do: 2.2 dB less swell on LEAD and
    // double the pick response, THD untouched. It also ate palm mutes and staccato,
    // because handing back level slope makes quiet playing quieter. The swell IS
    // level-dependent gain, and cancelling it with level-dependent gain in the
    // other direction is audible as a gate. The honest lever is less saturation.
    static constexpr float kTouch[3] = { 0.0f, 0.0f, 0.0f };
    if (kTouch[ci] > 0.f)
    {
        const float* rd = buffer.getReadPointer (0);
        for (int n = 0; n < numSamples; ++n) touch.follow (rd[n]);
        touch.updateBlock (kTouch[ci]);
    }

    // Mono processing on channel 0. The cabinet IR creates the stereo image.
    {
        float* data = buffer.getWritePointer (0);

        // 0. NoiseGate - pre-preamp gate, fully out of the path when disabled.
        if (gateOn)
            noiseGate.process (data, numSamples, gateThresh, gateRelease);

        // 1 + 2. Preamp and tone stack. The ORDER is the switch.
        // POST (default): amp then tone stack, so the controls colour the finished
        //   distortion. MARK: tone stack first, so they decide what gets distorted -
        //   the Mark IV trait, its V1B being a recovery stage after the tone controls.
        // kMarkTrim compensates the level, since cutting mids ahead of the preamp
        //   feeds the saturation less signal.
        const bool markMode = apvts.getRawParameterValue ("stackPos")->load() > 0.5f;
        if (markMode)
        {
            static constexpr float kMarkTrim = 1.14f;   // level match between the Post and Mark paths
            toneStackEarly.process (data, numSamples, bass, mid, treble, ci);
            for (int n = 0; n < numSamples; ++n) data[n] *= kMarkTrim;
            ampEngine.process (data, numSamples, gainNorm, charNorm, brightEnabled, ci, factor, tubeTemp,
                               supply);
        }
        else
        {
            ampEngine.process (data, numSamples, gainNorm, charNorm, brightEnabled, ci, factor, tubeTemp,
                               supply);
            toneStack.process (data, numSamples, bass, mid, treble, ci);
        }

        // Graphic EQ - on a real Mark this sits after the tone controls and ahead of
        // the power amp. Off by default: it is a large tone change.
        if (apvts.getRawParameterValue ("eqOn")->load() > 0.5f)
        {
            const float eqDb[GraphicEQ::kBands] = {
                apvts.getRawParameterValue ("eq80")->load(),
                apvts.getRawParameterValue ("eq240")->load(),
                apvts.getRawParameterValue ("eq750")->load(),
                apvts.getRawParameterValue ("eq2200")->load(),
                apvts.getRawParameterValue ("eq6600")->load() };
            graphicEQ.update (eqDb);
            for (int n = 0; n < numSamples; ++n)
                data[n] = graphicEQ.process (data[n]);
        }

        // 3. PowerAmp. Rectifier: silicon is the default (1.0), tube sags harder.
        const float rectMul = apvts.getRawParameterValue ("rectifier")->load() > 0.5f ? 1.75f : 1.0f;
        powerAmp.process         (data, numSamples, master, presence, ci, factor, tubeTemp, rectMul, supply);

        outputTransformer.process (data, numSamples, ci);

        reactiveLoad.process     (data, numSamples, ci);

        speakerSim.process       (data, numSamples, ci);

    }

    // Spread mono -> stereo. Channel 0 carries the processed amp signal; it is
    // copied to channel 1 before IRSection so convolution sees it on both.
    if (numOut >= 2 && bufCh >= 2)
        buffer.copyFrom (1, 0, buffer.getReadPointer (0), numSamples);
    for (int ch = 2; ch < numOut && ch < bufCh; ++ch)
        buffer.clear (ch, 0, numSamples);

    if (apvts.getRawParameterValue ("bypassIR")->load() < 0.5f)
        irSection.process (buffer, numSamples, irBlend, irMix);

    // Post FX - delay -> modulation -> reverb on the post-cabinet stereo signal.
    // Dry stays at unity and each stage adds its wet. Modulation sits between delay
    // and reverb so the detuned wash is reverberated too. All run every block.
    revOnZ += fxOnSm * ((revOn ? 1.f : 0.f) - revOnZ);
    dlyOnZ += fxOnSm * ((dlyOn ? 1.f : 0.f) - dlyOnZ);
    modOnZ += fxOnSm * ((modOn ? 1.f : 0.f) - modOnZ);

    // Bypass by taking the wet amount to zero, not by skipping the call - skipping
    // freezes the delay lines and dumps a stale tail when it comes back.
    // MOD OFF zeroes DETUNE and CHORUS only. Modulation's background ensemble has a
    // floor that is always on (bgAmt = 0.15 + 0.85 * detune); it is part of the
    // amp's voice, not an effect, so a bypass has no business killing it.
    delay.process      (buffer, numSamples, delayType,  delayTime,   delayFb,
                        delayMix * dlyOnZ);
    modulation.process (buffer, numSamples, modDetune * modOnZ, modChorus * modOnZ,
                        modRate);
    reverb.process     (buffer, numSamples, reverbType, reverbDecay, reverbTone,
                        reverbMix * revOnZ);

    const int outCh = juce::jmin (numOut, bufCh);
    // TouchRestore - off (see kTouch above). Costs nothing while disabled.
    if (kTouch[ci] > 0.f)
    {
        const int nch = buffer.getNumChannels();
        for (int n = 0; n < numSamples; ++n)
        {
            const float g = touch.next();
            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[n] *= g;
        }
    }

    buffer.applyGain (outGain);

    {
        float peak = 0.f;
        for (int ch = 0; ch < outCh; ++ch)
        {
            const float* r = buffer.getReadPointer (ch);
            for (int n = 0; n < numSamples; ++n)
                peak = juce::jmax (peak, std::abs (r[n]));
        }
        raisePeak (outputPeakLin, peak);
    }
}

//==============================================================================
bool CopilotToneAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* CopilotToneAudioProcessor::createEditor()
{
    return new CopilotToneAudioProcessorEditor (*this);
}

void CopilotToneAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    // Persist the loaded IR file paths so cabs survive a session reload.
    auto* irXml = xml->createNewChildElement ("IRFILES");
    irXml->setAttribute ("a", irSection.irALoaded.load() ? irSection.irAPath : juce::String());
    irXml->setAttribute ("b", irSection.irBLoaded.load() ? irSection.irBPath : juce::String());
    irXml->setAttribute ("dir", lastIRDir);
    copyXmlToBinary (*xml, destData);
}

void CopilotToneAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    // Restore IR files (non-blocking load) before replacing the APVTS state.
    if (auto* irXml = xml->getChildByName ("IRFILES"))
    {
        const juce::String a = irXml->getStringAttribute ("a");
        const juce::String b = irXml->getStringAttribute ("b");
        lastIRDir = irXml->getStringAttribute ("dir");
        const juce::String savedDir = irXml->getStringAttribute ("dir");

        // A saved path breaks for ordinary reasons: an unmounted drive, or folders
        // the user reorganised. Two rules, in order: relink by filename first, and
        // NEVER clear an IR that is currently loaded and working - a stale path is
        // not a reason to throw away a cab that is loaded and working.
        if (const juce::File fa = relinkIR (a, savedDir); fa.existsAsFile())
            irSection.loadIRA (fa);
        else if (a.isEmpty() && ! irSection.irALoaded.load())
            irSection.clearIRA();
        else if (a.isNotEmpty() && ! irSection.irALoaded.load())
            irSection.irAMissing.store (true);   // name it, do not silently drop it

        if (const juce::File fb = relinkIR (b, savedDir); fb.existsAsFile())
            irSection.loadIRB (fb);
        else if (b.isEmpty() && ! irSection.irBLoaded.load())
            irSection.clearIRB();
        else if (b.isNotEmpty() && ! irSection.irBLoaded.load())
            irSection.irBMissing.store (true);
    }

    apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CopilotToneAudioProcessor();
}
