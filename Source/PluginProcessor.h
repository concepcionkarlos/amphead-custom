/*
  ==============================================================================
    CopilotTone  -  Neural-Ready Amp Host
    PluginProcessor.h

    Signal flow: Input -> AmpEngine -> ToneStack -> PowerAmp -> OutputTransformer
                 -> ReactiveLoad -> SpeakerSim -> IRSection -> Output
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

//==============================================================================
// OsBiquadChain - 8th-order Butterworth lowpass (4 cascaded biquads, transposed
// direct form II). Anti-imaging into an oversampled block, anti-alias on the way
// back down. A 12 dB/octave one-pole pair leaves images that intermodulate in the
// nonlinear stages, putting their products back inside the audio band.
struct OsBiquadChain
{
    static constexpr int kStages = 4;              // 4 biquads = 8th order
    struct Coeffs { float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f; };
    struct Set    { Coeffs s[kStages]; };

    Set   active;
    float z1[kStages] = {}, z2[kStages] = {};

    // Butterworth pole Q for stage i of a (2*kStages)-order cascade.
    static float stageQ (int i)
    {
        const float order = 2.f * (float) kStages;
        const float theta = juce::MathConstants<float>::pi
                          * (2.f * (float) i + 1.f) / (2.f * order);
        return 1.f / (2.f * std::cos (theta));
    }

    // Coefficient design. Has transcendentals - prepare() only, never the audio thread.
    static Set design (float fc, float osr)
    {
        Set out;
        fc = juce::jlimit (100.f, 0.49f * osr, fc);
        const float w0 = juce::MathConstants<float>::twoPi * fc / osr;
        const float cw = std::cos (w0), sw = std::sin (w0);
        for (int i = 0; i < kStages; ++i)
        {
            const float alpha = sw / (2.f * stageQ (i));
            const float a0    = 1.f + alpha;
            out.s[i].b0 = ((1.f - cw) * 0.5f) / a0;
            out.s[i].b1 = (1.f - cw) / a0;
            out.s[i].b2 = out.s[i].b0;
            out.s[i].a1 = (-2.f * cw) / a0;
            out.s[i].a2 = (1.f - alpha) / a0;
        }
        return out;
    }

    inline float process (float x) noexcept
    {
        for (int i = 0; i < kStages; ++i)
        {
            const Coeffs& c = active.s[i];
            const float y = c.b0 * x + z1[i];
            z1[i] = c.b1 * x - c.a1 * y + z2[i];
            z2[i] = c.b2 * x - c.a2 * y;
            x = y;
        }
        return x;
    }

    void reset() noexcept { for (int i = 0; i < kStages; ++i) z1[i] = z2[i] = 0.f; }
};

// Anti-alias / anti-imaging cutoff as a fraction of the NATIVE rate (18 kHz at
// 48 k): inaudible, and low enough that an 8th-order Butterworth still has
// stopband before Nyquist. Aliasing is limited by stages outside the OS block.
static constexpr float kOsCutoffFrac = 0.375f;

//==============================================================================
// softTube - Pade approximant of tanh, clamped so it saturates at +/-1, cheap
// enough for 8x. Its harmonic series decays with order (H2 > H3 > H4) like a
// valve; a rational band-split saturator gives a flat series that reads as fizz.
static inline float softTube (float x) noexcept
{
    x = juce::jlimit (-3.f, 3.f, x);
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

//==============================================================================
// TouchRestore - re-couples output level to the input envelope. Not a compressor
// and not a gate: it puts a fraction `amount` of the input's dB slope back onto
// the output, flattening the gain-versus-level curve. amount = 0 is a bypass.
struct TouchRestore
{
    float env = 0.f, gain = 1.f, target = 1.f;
    float cAtk = 0.f, cRel = 0.f, cSmooth = 0.f;
    static constexpr float kRef = 0.126f;   // -18 dBFS: the level the amp is voiced at

    void prepare (double sr, int /*blockSize*/)
    {
        // Per-sample, so the time constants stay in seconds at any block size.
        cAtk    = 1.f - std::exp (-1.f / juce::jmax (1.f, 0.008f * (float) sr));
        // Release tracks playing intensity, not one note's decay. Short values gate.
        cRel    = 1.f - std::exp (-1.f / juce::jmax (1.f, 0.350f * (float) sr));
        cSmooth = 1.f - std::exp (-1.f / juce::jmax (1.f, 0.006f * (float) sr));
    }
    void reset() noexcept { env = 0.f; gain = 1.f; target = 1.f; }

    // Per sample, on the amp's INPUT.
    inline void follow (float x) noexcept
    {
        const float a = std::abs (x);
        env += (a > env ? cAtk : cRel) * (a - env);
    }

    void updateBlock (float amount) noexcept
    {
        // Wide clamps on purpose: tighter ones disengage the correction on the tail.
        const float r = juce::jlimit (0.02f, 8.f, env / kRef);
        target = (amount > 0.f) ? std::pow (r, amount) : 1.f;
        // Floor bounded at -4.4 dB. This must never duck a note audibly.
        target = juce::jlimit (0.60f, 4.f, target);
    }
    inline float next() noexcept { gain += cSmooth * (target - gain); return gain; }
};

//==============================================================================
// GraphicEQ - Mesa Mark-series five-band graphic EQ, +/-12 dB per band.
// The band centres are NOT the faceplate labels. The sliders read 80 / 240 / 750 /
// 2200 / 6600, but the measured resonances of the real circuit are 87.61, 371.74,
// 723.43, 1575.87 and 4822.88 Hz; built to the printed numbers the famous curves
// come out wrong. In the manual's "Classic V" the 750 band is the crucial one:
// with the other four boosted, it alone sets the depth of the effect.
struct GraphicEQ
{
    static constexpr int kBands = 5;
    // Measured resonances, not the faceplate labels.
    static constexpr float kFreq[kBands] = { 87.61f, 371.74f, 723.43f, 1575.87f, 4822.88f };
    // Broad and overlapping, like a real graphic EQ: shaping, not notching.
    static constexpr float kQ = 0.80f;

    struct BQ { float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f; };
    BQ    c[kBands];
    float z1[kBands] = {}, z2[kBands] = {};
    float curDb[kBands] = {};        // smoothed, so dragging a slider cannot click
    float sr = 48000.f;
    float cSmooth = 0.f;
    bool  dirty = true;

    void prepare (double sampleRate, int blockSize)
    {
        sr = (float) sampleRate;
        const float bps = sr / juce::jmax (1.f, (float) blockSize);
        cSmooth = 1.f - std::exp (-1.f / juce::jmax (1.f, 0.03f * bps));   // ~30 ms
        reset();
        dirty = true;
    }
    void reset() noexcept
    {
        for (int i = 0; i < kBands; ++i) z1[i] = z2[i] = 0.f;
    }

    // Peaking biquad, RBJ. prepare/block rate only - never per sample.
    void setBand (int i, float dB)
    {
        const float A     = std::pow (10.f, dB / 40.f);
        const float w0    = juce::MathConstants<float>::twoPi * kFreq[i] / sr;
        const float cw    = std::cos (w0), sw = std::sin (w0);
        const float alpha = sw / (2.f * kQ);
        const float a0    = 1.f + alpha / A;
        c[i].b0 = (1.f + alpha * A) / a0;
        c[i].b1 = (-2.f * cw)       / a0;
        c[i].b2 = (1.f - alpha * A) / a0;
        c[i].a1 = (-2.f * cw)       / a0;
        c[i].a2 = (1.f - alpha / A) / a0;
    }

    void update (const float* targetDb)
    {
        for (int i = 0; i < kBands; ++i)
        {
            const float t = targetDb[i];
            if (std::abs (t - curDb[i]) > 0.001f || dirty)
            {
                curDb[i] += cSmooth * (t - curDb[i]);
                if (std::abs (t - curDb[i]) < 0.01f) curDb[i] = t;
                setBand (i, curDb[i]);
            }
        }
        dirty = false;
    }

    inline float process (float x) noexcept
    {
        for (int i = 0; i < kBands; ++i)
        {
            const BQ& q = c[i];
            const float y = q.b0 * x + z1[i];
            z1[i] = q.b1 * x - q.a1 * y + z2[i];
            z2[i] = q.b2 * x - q.a2 * y;
            x = y;
        }
        return x;
    }
};

//==============================================================================
// TubeThermal - cathode temperature / emission state, 0 = cold, 1 = fully hot.
// A real amp drifts as it heats: EL34 cathode current climbs from about 40 mA to
// 62-68 mA per side in the first half hour. Every hot-tube term scales by this.
struct TubeThermal
{
    float temp  = 0.f;      // 0..1
    float coeff = 0.f;      // per-block advance towards 1
    float start = 0.f;      // temperature at power-on

    // tauSeconds = warm-up time constant. Advanced once per block.
    void prepare (double sr, int blockSize, float tauSeconds, float startTemp)
    {
        const float blocksPerSec = (float) sr / juce::jmax (1.f, (float) blockSize);
        coeff = 1.f - std::exp (-1.f / juce::jmax (1.f, tauSeconds * blocksPerSec));
        start = juce::jlimit (0.f, 1.f, startTemp);
    }
    inline void advance() noexcept { temp += coeff * (1.f - temp); }
    void reset() noexcept { temp = start; }
};

//==============================================================================
// AmpEngine - all preamp processing: input front-end (DC block, cable, HPF, LPF,
// transient smoothing) -> V1A -> interstage -> V1B cold-biased triode -> V1C ->
// cathode follower. RTNeural would replace all of that with model->forward().
// Shared B+ rail.
//
// A real amp has ONE high-voltage supply feeding everything through a chain of
// RC filter stages. The power tubes hang off the first node and see the deepest,
// fastest sag; the preamp sits further down the chain, where the droop is
// shallower, slower, and lags - it is still falling after the chord has stopped
// pulling current.
//
// Until now every stage sagged on its own signal alone, so nothing the power
// section did could be felt in the preamp. This is the node they share. The
// power stage already computes its own draw envelope; this only runs the RC
// that carries it downstream.
struct PowerSupply
{
    float preNode = 0.f;            // droop seen by the preamp, 0 = full B+
    float cAtk = 0.f, cRel = 0.f;

    // Ripple. The rectifier does not deliver a smooth voltage - it charges the
    // reservoir in pulses, twice per mains cycle, so 120 Hz on a 60 Hz supply. The
    // reservoir smooths it, but never to nothing, and the harder the amp pulls the
    // less it smooths. That leftover ripple rides on the plate voltage and
    // amplitude-modulates whatever the stage is amplifying, which is where the
    // ghost notes in a cranked amp come from: sidebands at f +/- 120 Hz that
    // nobody played.
    //
    // Held as a rotating phasor rather than a sin() per sample - two multiply-adds
    // and no transcendental on the audio thread.
    float ripX = 1.f, ripY = 0.f;   // unit phasor: cos, sin
    float ripC = 1.f, ripS = 0.f;   // per-sample rotation

    void prepare (double sampleRate)
    {
        const float fsr = (float) sampleRate;
        cAtk = std::exp (-1.f / (0.030f * fsr));    // 30 ms  to follow a draw down
        cRel = std::exp (-1.f / (0.320f * fsr));    // 320 ms for the reservoir to refill
        preNode = 0.f;

        const float w = juce::MathConstants<float>::twoPi * 120.f / fsr;
        ripC = std::cos (w);  ripS = std::sin (w);
        ripX = 1.f;  ripY = 0.f;
    }
    void reset() noexcept { preNode = 0.f;  ripX = 1.f;  ripY = 0.f; }

    // Ripple as a fraction, scaled by how hard the amp is pulling: near silent at
    // idle, biggest under a chord. Full-wave rectified ripple is a sawtooth rather
    // than a sine, so a little second harmonic gets the shape closer.
    inline float ripple (float depth) noexcept
    {
        const float nx = ripX * ripC - ripY * ripS;
        ripY           = ripX * ripS + ripY * ripC;
        ripX           = nx;
        // One Newton step back onto the unit circle, so the phasor cannot drift.
        const float k = 1.5f - 0.5f * (ripX * ripX + ripY * ripY);
        ripX *= k;  ripY *= k;

        return depth * preNode * (ripY + 0.35f * (2.f * ripX * ripY));
    }

    // Called per sample by the power stage with its (already bounded) draw.
    inline void draw (float pwrDroop) noexcept
    {
        const float c = (pwrDroop > preNode) ? cAtk : cRel;
        preNode += (1.f - c) * (pwrDroop - preNode);
    }
};

struct AmpEngine
{
    void prepare (double sampleRate, int maxBlockSize);
    void process (float* data, int numSamples, float gainNorm, float charNorm,
                  bool brightEnabled, int channelIndex, int factor, float tubeTemp,
                  PowerSupply& psu);   // the shared rail: droop and ripple
    void reset();

    // Rebuild the pickup + cable resonance for a cable length. Returns immediately
    // unless the choice actually moved - the design needs a sin and a cos, which is
    // not per-block work.
    void setCableLength (int idx);

    // RTNeural integration point. Model members and loadModel() belong here, and
    // process() dispatches to model->forward() for a loaded channel.
    //==============================================================================

private:
    // Input front-end filter states
    float dcX    = 0.f, dcY   = 0.f;   // DC blocker
    float zHPx   = 0.f, zHPy  = 0.f;   // input impedance coupling HPF
    // Pickup + cable resonance. A pickup is an inductor (~4 H for a humbucker)
    // and the cable is capacitance (~100 pF per metre). Together they ring: a peak
    // a few dB high, then 12 dB/oct down. That peak IS the bright, alive top of an
    // electric guitar - a plain one-pole has no peak at all and rolls off half as
    // fast. Second order, so it needs two states rather than one.
    float cblZ1 = 0.f, cblZ2 = 0.f;
    float hpfX   = 0.f, hpfY  = 0.f;   // input HPF (60 Hz)
    float inLP   = 0.f;                  // input LPF
    float tsEnv  = 0.f;                  // transient smoothing envelope

    // Interstage filter states
    float isHPx  = 0.f, isHPy = 0.f;   // interstage coupling HPF
    float isLP   = 0.f;                  // grid stopper LP before V1B
    // Second interstage, feeding the third gain stage V1C.
    float is2HPx = 0.f, is2HPy = 0.f;
    float is2LP  = 0.f;
    float cIs2HP[3] = {}, cIs2LP = 0.f;
    float tIs2HP[4][3] = {}, tIs2LP[4] = {};

    // Filter coefficients (computed in prepare, stable across all sample rates)
    float cDC    = 0.f;   // DC blocker alpha (~2 Hz)
    float cZHP   = 0.f;   // coupling HPF alpha (~7 Hz)
    float cblB0 = 1.f, cblB1 = 0.f, cblB2 = 0.f, cblA1 = 0.f, cblA2 = 0.f;
    int   cblLenIdx = -1;                 // cached, so the design only runs on a change
    float cHPF   = 0.f;   // input HPF alpha (60 Hz)
    float cInLP  = 0.f;   // input LP coeff (16 kHz)
    float cTsAtk = 0.f;   // transient smooth attack coeff (0.5 ms)
    float cTsRel = 0.f;   // transient smooth release coeff (80 ms)
    float cIsHP  = 0.f;   // interstage HPF alpha (~100 Hz)
    float cIsLP  = 0.f;   // grid stopper LP coeff (~6 kHz)

    // Cathode follower
    float cfEnv  = 0.f;   // CF peak envelope
    float cCfAtk = 0.f;   // CF attack coeff (0.3 ms)
    float cCfRel = 0.f;   // CF release coeff (20 ms)
    double sr    = 44100.0;

    // Oversampling for V1A + interstage + V1B, factor selectable at runtime
    // (1/2/4/8). All coefficients are tabled in prepare(); selectOs() picks a row.
    void buildOsTables();                   // precompute coeff tables (prepare only)
    void selectOs (int factor);             // pick a factor's coeffs (RT-safe)
    static int osIdx (int f) { return f >= 8 ? 3 : f >= 4 ? 2 : f >= 2 ? 1 : 0; }
    static constexpr int kMaxFactor = 8;
    int   osFactor = 0;                     // current factor (0 = not yet set)
    juce::HeapBlock<float> ovsBuf;
    OsBiquadChain osUp, osDown;           // anti-imaging / anti-alias chains

    // Cathode bypass (Rk || Ck) and grid conduction / blocking state. Both run in
    // the oversampled loop, so their coefficients live in the per-factor tables.
    float ckBiasA = 0.f, ckBiasB = 0.f;   // cathode cap charge, V1A / V1B
    float gridChg = 0.f;                  // coupling-cap charge from grid current
    float cCkA = 0.f, cCkB = 0.f;         // cathode cap time constants

    // Cathode bypass, the FREQUENCY half of it. Rk || Ck is a low shelf, not just
    // a bias envelope: above 1/(2*pi*Rk*Ck) the cap shorts Rk and the stage runs at
    // full gain, below it Rk is unbypassed and local feedback takes gain away. That
    // is how a channel relay tightens the bass before the next gain stage - a big
    // cap keeps the lows, a small one throws them away. Per channel, because
    // swapping this cap is most of what switching channels does in a real amp.
    float ckLPa = 0.f, ckLPb = 0.f;       // the lows the cap does not bypass
    float cCkFa[3] = {}, cCkFb[3] = {};   // corner, active oversampling factor
    float tCkFa[4][3] = {}, tCkFb[4][3] = {};
    float cGridAtk = 0.f, cGridRel = 0.f; // grid conduction charge / recovery
    float tCkA[4] = {}, tCkB[4] = {}, tGridA[4] = {}, tGridR[4] = {};
    float cIsHP2x[3] = {}, cIsLP2x = 0.f;  // interstage coefficients at oversampled SR

    // per-factor coefficient tables [idx: 0=x1, 1=x2, 2=x4, 3=x8]
    float tIsHP[4][3] = {}, tIsLP[4] = {};
    float tBsA[4] = {}, tBsB[4] = {}, tHmA[4] = {}, tHmR[4] = {};
    OsBiquadChain::Set tOsLP[4];
    float tIhDC[4] = {};   // ih DC-tracker, per factor (it runs oversampled)

    // Pick attack detector + dynamic high-mid bite
    float pkFast = 0.f, pkSlow  = 0.f;   // fast / slow pick envelopes
    float pkHPx  = 0.f, pkHPy  = 0.f;   // high-mid pre-emphasis HP state
    float cPkFAtk = 0.f, cPkFRel = 0.f; // fast envelope: 0.5 ms atk / 15 ms rel
    float cPkSAtk = 0.f, cPkSRel = 0.f; // slow envelope: 5 ms atk / 60 ms rel
    float cPkHP   = 0.f;                  // pre-emphasis HP alpha (~4 kHz)

    // Frequency-dependent V1A + harmonic bloom + upper exciter
    float bsLPa   = 0.f, bsLPb  = 0.f;   // V1A band-split LP states (2x SR)
    float hmBloom  = 0.f;                   // harmonic bloom envelope (2x SR)
    float ihDC     = 0.f;                   // DC tracker for the even-harmonic injection
    float uhLP1    = 0.f;                   // upper harmonic exciter HP state (native SR)
    float cBsLPa  = 0.f, cBsLPb = 0.f;   // band-split LP coeffs (~400 Hz, ~3 kHz at 2x SR)
    float cHmBAtk = 0.f, cHmBRel = 0.f;  // bloom envelope: 3 ms atk / 600 ms rel (2x SR)
    float cIhDC   = 0.f;                 // ~12 Hz LP - the DC/envelope part of the injection
    float cUhLP   = 0.f;                   // exciter extraction LP coeff (~3 kHz native SR)

    // Bright cap: pre-saturation high-shelf emphasis (~3 kHz) before V1A.
    float brightHPx = 0.f, brightHPy = 0.f;
    float cBrightHP = 0.f;

    // Low-end shaper after the preamp: a sub-95 Hz punch weighted by the pick
    // attack, and a resonant ~340 Hz scoop narrow enough to spare the 200 Hz body.
    float lmPunch1 = 0.f, lmPunch2 = 0.f;
    float mudLP1 = 0.f, mudLP2 = 0.f;
    float cLmPunch = 0.f, cMud = 0.f;
    // Upper-mid presence: broad resonant bandpass at ~700 Hz, blooms with drive.
    float voxLP1 = 0.f, voxLP2 = 0.f;
    float cVox = 0.f;

    // "Chuck" transient detector: fast/medium envelope ratio, level-independent.
    float chkFast = 0.f, chkSlow = 0.f;
    float cChkFAtk = 0.f, cChkFRel = 0.f, cChkSAtk = 0.f, cChkSRel = 0.f;

    // Metallic string generator: resonant ~2.4 kHz band, soft-clipped to spawn
    // 3-5 kHz harmonics. Placed where the cab still passes, so it survives the IR.
    float metLP1 = 0.f, metLP2 = 0.f;
    float cMet = 0.f;

    // String-body boost: resonant ~230 Hz, gain-weighted, above the sub-95 thump.
    float fatLP1 = 0.f, fatLP2 = 0.f;
    float cFat = 0.f;
};

//==============================================================================
// ToneStack - three-band shaping. NOTE: a parallel band sum, not a true
// interactive passive RC stack; the controls do not load each other. Natural
// insertion loss preserved, no makeup gain. Runs after AmpEngine at native SR.
struct ToneStack
{
    void prepare (double sampleRate);
    void process (float* data, int numSamples,
                  float bass, float mid, float treble,
                  int channelIndex);
    void reset();

private:
    float coupX = 0.f, coupY = 0.f;
    float bassLP = 0.f;
    float treHPx = 0.f, treHPy = 0.f;
    float midHPx = 0.f, midHPy = 0.f, midLP = 0.f;

    float cCoupHPF = 0.f;
    float cBassLP[3] = {};
    float cTreHP    = 0.f;
    float cMidHP[3] = {}, cMidLP[3] = {};
};

//==============================================================================
// PowerAmp - push-pull power section. The phase inverter and push-pull stage run
// oversampled; sag, NFB, resonance and presence run at the native rate.
struct PowerAmp
{
    void prepare (double sampleRate, int maxBlockSize);
    void process (float* data, int numSamples,
                  float master, float presence,
                  int channelIndex, int factor, float tubeTemp,
                  float rectMul,    // 1.0 = silicon, higher = tube rectifier sag
                  PowerSupply& psu);
    void reset();

private:
    void buildOsTables();                 // precompute coeff tables (prepare only)
    void selectOs (int factor);           // pick a factor's coeffs (RT-safe)
    static int osIdx (int f) { return f >= 8 ? 3 : f >= 4 ? 2 : f >= 2 ? 1 : 0; }
    static constexpr int kMaxFactor = 8;
    int   paFactor = 0;                    // current factor (0 = not yet set)
    double paSR   = 44100.0;               // base sample rate (for table build)
    float piEnv   = 0.f;                  // phase inverter drive envelope
    float pwrLP   = 0.f;                  // pre-power band-limit LP
    float sagEnv  = 0.f;                  // power supply droop envelope

    // Screen grid. A beam tetrode's screen is fed from the same B+ through a
    // dropping resistor and has its own, much smaller, filter cap - so it sags
    // harder and faster than the plate ever does. And screen current is not
    // proportional to drive: it stays near nothing until the tube is pushed, then
    // climbs steeply. That is why a cranked amp CHOKES on a big chord and blooms
    // back, instead of compressing smoothly the way plate sag alone would.
    float scrEnv = 0.f;
    float nfbLP   = 0.f;                  // NFB low-pass filter state
    float resLP1  = 0.f, resLP2 = 0.f;   // resonance two-pole path
    float postLP1 = 0.f, postLP2 = 0.f;  // anti-fizz two-pole
    float presHPx = 0.f, presHPy = 0.f;  // presence shelf HP state

    float cPwrLP[3]  = {};
    float cPostLP[3] = {};
    float cPiAtk  = 0.f, cPiRel  = 0.f;
    float cSagAtk = 0.f, cSagRel = 0.f;
    float cScrAtk = 0.f, cScrRel = 0.f;   // screen node: smaller cap, quicker both ways
    float cNFBLP  = 0.f;
    float cResLP  = 0.f;
    float cPresHP = 0.f;

    // Oversampling for phase inverter + push-pull
    juce::HeapBlock<float> paOvsBuf;
    OsBiquadChain paOsUp, paOsDown;         // anti-imaging / anti-alias chains
    float cPiAtk2x = 0.f, cPiRel2x = 0.f;  // PI envelope coefficients at oversampled SR

    // per-factor coefficient tables [idx: 0=x1, 1=x2, 2=x4, 3=x8]
    float tPiAtk[4] = {}, tPiRel[4] = {};
    OsBiquadChain::Set tPaOsLP[4];
};

//==============================================================================
// ReactiveLoad - the speaker load between power amp and cabinet. A speaker is a
// frequency-dependent impedance, not a flat resistive load: resonance peaks, back
// EMF, and damping of the cone. Resonant LP -> impedance peak -> back EMF -> damp.
struct ReactiveLoad
{
    void prepare (double sampleRate);
    void process (float* data, int numSamples, int channelIndex);
    void reset();

private:
    float impLP1  = 0.f, impLP2  = 0.f;   // resonant two-pole LP at ~100 Hz
    float bemfLP  = 0.f;                    // back EMF LP tracker of resonance zone
    float dampEnv = 0.f;                    // energy envelope for dynamic damping

    float cImpLP   = 0.f;
    float cBEMFLP  = 0.f;
    float cDampAtk = 0.f, cDampRel = 0.f;
};

//==============================================================================
// OutputTransformer - iron saturation plus B+ supply behaviour. Saturation reaches
// low frequencies first (LF split off by a two-pole LP at ~400 Hz), so upper mids
// stay articulate, and the soft limit is asymmetric for magnetic character without
// fuzz. Hysteresis feeds back an 80 Hz lowpass. Sustain after attack blooms the sag.
struct OutputTransformer
{
    void prepare (double sampleRate);
    void process (float* data, int numSamples, int channelIndex);
    void reset();

private:
    float xfLP1  = 0.f, xfLP2  = 0.f;   // two-pole LP: LF isolation ~400 Hz
    float xfHyst = 0.f;                   // hysteresis integrator (80 Hz LP)

    float supSag = 0.f;   // supply fast-attack peak envelope
    float supRes = 0.f;   // reservoir capacitor (slow recharge)

    float cXfLP   = 0.f;
    float cHystLP = 0.f;
    float cSupAtk = 0.f;   // 5 ms - supply sags quickly on attack
    float cSupRel = 0.f;   // 150 ms - moderate sag release
    float cResAtk = 0.f;   // 3 ms - reservoir discharges fast with sag
    float cResRel = 0.f;   // 400 ms - slow capacitor recharge => bloom
};

//==============================================================================
// SpeakerSim - dynamic speaker / cab interaction before IR convolution: HF
// damping -> cone inertia LP -> cone resonance -> compression -> pre-IR cleanup.
// Signal-driven throughout: open at rest, smoother under harder playing.
struct SpeakerSim
{
    void prepare (double sampleRate);
    void process (float* data, int numSamples, int channelIndex);
    void reset();

private:
    float hfX    = 0.f, hfY    = 0.f;   // HF extractor HP states
    float hfEnv  = 0.f;                   // HF content envelope
    float dynBrt = 0.f;                   // cone LP state: bright (~12 kHz)
    float dynDrk = 0.f;                   // cone LP state: dark (~7 kHz)
    float resLP1 = 0.f, resLP2 = 0.f;   // cone resonance two-pole path
    float cmpEnv = 0.f;                   // speaker compression peak envelope
    float preLP  = 0.f;                   // pre-IR ultrasonic cleanup LP

    float cHFHP   = 0.f;
    float cHFAtk  = 0.f, cHFRel  = 0.f;
    float cDynBrt = 0.f, cDynDrk = 0.f;
    float cResLP  = 0.f;
    float cCmpAtk = 0.f, cCmpRel = 0.f;
    float cPreLP  = 0.f;
};

//==============================================================================
// IRSection - dual convolution cabinet IR, A + B with crossfade blend. Loading
// is non-blocking (juce::dsp::Convolution owns the thread).
struct IRSection
{
    void prepare  (const juce::dsp::ProcessSpec& spec);
    void process  (juce::AudioBuffer<float>& buffer, int numSamples,
                   float blend, float mix);
    void reset();
    // Return false when nothing was loaded: IR paths break often, so callers check.
    bool loadIRA  (const juce::File& file);
    bool loadIRB  (const juce::File& file);
    static bool irIsReadable (const juce::File& file);
    void clearIRA ();
    void clearIRB ();
    int  latencySamples();    // convolution latency (host compensation)

    std::atomic<bool> irALoaded { false };
    std::atomic<bool> irBLoaded { false };
    // A remembered path that no longer resolves, kept so the UI can name the cab.
    std::atomic<bool> irAMissing { false };
    std::atomic<bool> irBMissing { false };
    juce::String      irAName, irBName;
    juce::String      irAPath, irBPath;   // full paths, for state save/restore

private:
    juce::dsp::Convolution convA, convB;
    juce::AudioBuffer<float> dryBuf, wetBuf;
    int preparedChannels = 2;
    float airHPxPrev[2] = {}, airHPy[2] = {};    // post-IR air shelf HP state (~6 kHz, per channel)
    float presHPxPrev[2] = {}, presHPy[2] = {}; // post-IR presence shelf HP state (~2 kHz)
    float cAirHP  = 0.f;                          // air shelf 1-pole HP coeff (~6 kHz)
    float cPresHP = 0.f;                          // presence shelf 1-pole HP coeff (~2 kHz)

    float postEnv[2]   = {};   // post-IR level envelope (per channel)
    float cPostAtk     = 0.f;  // 2ms attack
    float cPostRel     = 0.f;  // 120ms release
    float cabResLP1[2] = {};   // cab body resonance two-pole LP (per channel)
    float cabResLP2[2] = {};
    float cCabResLP    = 0.f;  // resonance LP coeff (~150 Hz)
    float postClean[2] = {};   // anti-alias cleanup LP after post-IR nonlinearity
    float cPostClean   = 0.f;  // ~11 kHz
};

//==============================================================================
// NoiseGate
struct NoiseGate
{
    void prepare (double sampleRate);
    void process (float* data, int numSamples, float threshDb, float releaseMs);
    void reset();
private:
    float env  = 0.f;
    float cAtk = 0.f;
    float cRel = 0.f;              // cached release coeff
    float lastReleaseMs = -1.f;    // recompute cRel only when this changes
    double sr  = 44100.0;
};

//==============================================================================
// Delay - stereo delay with three voices: Digital (clean, full bandwidth),
// Analog (BBD: darker repeats, gentle saturation, slow drift) and Tape (wow and
// flutter, saturation, HF loss per repeat). Ping-pong when stereo. No allocation.
struct Delay
{
    void prepare (double sampleRate, int maxBlockSize);
    void process (juce::AudioBuffer<float>& buffer, int numSamples,
                  int type, float timeMs, float feedback, float mix);
    void reset();

private:
    double sr   = 44100.0;
    int    size = 0, mask = 0;
    juce::HeapBlock<float> bufL, bufR;
    int   writeL = 0, writeR = 0;
    float dampL = 0.f, dampR = 0.f;       // feedback HF damping LP
    float hpLx  = 0.f, hpLy  = 0.f;       // feedback low-cut HP (L)
    float hpRx  = 0.f, hpRy  = 0.f;       // feedback low-cut HP (R)
    float curTime = 0.f;                  // smoothed delay length (samples)
    float mixZ    = 0.f;                  // smoothed mix
    // modulation oscillators (rotation form - no trig in the loop)
    float wowC = 1.f, wowS = 0.f, flutC = 1.f, flutS = 0.f;
};

//==============================================================================
// ReverbEngine - 4-line feedback delay network (Householder matrix) with input diffusion,
// per-line damping and modulation, plus a dispersive allpass front-end for Spring.
// Voices: Spring (the drip), Hall (long, diffuse), Room (short, tight), Plate
// (dense, bright). Decay sets the RT60, Tone the damping. No allocation in process().
struct ReverbEngine
{
    void prepare (double sampleRate, int maxBlockSize);
    void process (juce::AudioBuffer<float>& buffer, int numSamples,
                  int type, float decay, float tone, float mix);
    void reset();

private:
    double sr = 44100.0;
    // predelay (mono)
    juce::HeapBlock<float> preBuf;  int preMask = 0, preWrite = 0;
    juce::HeapBlock<float> apBuf[4];  int apMask[4] = {}, apWrite[4] = {}, apLen[4] = {};
    juce::HeapBlock<float> spBuf[8];  int spMask[8] = {}, spWrite[8] = {}, spLen[8] = {};
    float spLp = 0.f, spHpX = 0.f, spHpY = 0.f;
    juce::HeapBlock<float> fdn[4];  int fdnMask[4] = {}, fdnWrite[4] = {};
    float damp[4] = {};
    float lfoC[4] = { 1.f, 1.f, 1.f, 1.f }, lfoS[4] = {};
    float outHpX[2] = {}, outHpY[2] = {};
    float mixZ = 0.f;
};

//==============================================================================
// Modulation - stereo pitch DETUNE (L up / R down, delay-line shifter with
// crossfaded taps) plus a slow CHORUS, blended in parallel behind the signal.
// Sits between Delay and ReverbEngine so the detuned layer is reverberated too.
struct Modulation
{
    void prepare (double sampleRate, int maxBlockSize);
    void process (juce::AudioBuffer<float>& buffer, int numSamples,
                  float detune, float chorus, float rate);
    void reset();

private:
    double sr = 44100.0;
    // detune (stereo) - delay-line pitch shifter, two crossfading taps per side
    juce::HeapBlock<float> dtL, dtR;  int dtMask = 0, dtWriteL = 0, dtWriteR = 0;
    float dtPhL = 0.f, dtPhR = 0.f;                 // crossfade phasors [0,1)
    // Hidden always-on background detune layers, three stacked, never UI-exposed.
    float bgAPhL = 0.f, bgAPhR = 0.f;               // narrow detune
    float bgBPhL = 0.f, bgBPhR = 0.f;               // medium detune (opposite L/R)
    float bgCPhL = 0.f, bgCPhR = 0.f;               // wide detune
    juce::HeapBlock<float> chL, chR;  int chMask = 0, chWriteL = 0, chWriteR = 0;
    float chC_L = 1.f, chS_L = 0.f;        // single quadrature LFO (sin=L, cos=R)
    float detZ = 0.f, chorZ = 0.f;
};

//==============================================================================
class CopilotToneAudioProcessor  : public juce::AudioProcessor
{
public:
    CopilotToneAudioProcessor();
    ~CopilotToneAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi()  const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Convenience wrappers so the editor can call these directly.
    bool loadIR  (const juce::File& f) { return irSection.loadIRA (f); }
    bool loadIRB (const juce::File& f) { return irSection.loadIRB (f); }
    // Look for a remembered IR whose path no longer resolves: same filename in the
    // last browsed folder, the other IR's folder, or the folder saved with state.
    juce::File relinkIR (const juce::String& savedPath,
                         const juce::String& savedDir) const
    {
        if (savedPath.isEmpty()) return {};
        if (juce::File (savedPath).existsAsFile()) return juce::File (savedPath);
        const juce::String name = juce::File (savedPath).getFileName();
        juce::StringArray dirs { savedDir, lastIRDir };
        for (const auto& p : { irSection.irAPath, irSection.irBPath })
            if (p.isNotEmpty()) dirs.add (juce::File (p).getParentDirectory().getFullPathName());
        for (const auto& d : dirs)
        {
            if (d.isEmpty()) continue;
            const juce::File cand = juce::File (d).getChildFile (name);
            if (cand.existsAsFile()) return cand;
        }
        return {};
    }
    void clearIRA ()                   { irSection.clearIRA ();  }
    void clearIRB ()                   { irSection.clearIRB ();  }

    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;

    // Expose IR state for the editor to read filenames / loaded flags.
    IRSection irSection;

    // Last folder an IR was picked from. Message thread only.
    juce::String lastIRDir;

    // Browser start: the last folder used, else a loaded IR's folder, else home.
    juce::File irBrowseStart() const
    {
        if (lastIRDir.isNotEmpty() && juce::File (lastIRDir).isDirectory())
            return juce::File (lastIRDir);
        for (const auto& p : { irSection.irAPath, irSection.irBPath })
            if (p.isNotEmpty() && juce::File (p).existsAsFile())
                return juce::File (p).getParentDirectory();
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    }

    // Peak accumulators, not instantaneous levels: processBlock keeps the running
    // maximum and the editor consumes it with exchange(0). Linear, not dB.
    std::atomic<float> inputPeakLin  { 0.f };
    std::atomic<float> outputPeakLin { 0.f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    AmpEngine         ampEngine;
    TubeThermal       thermal;        // cathode temperature, drives the hot-tube physics
    TouchRestore      touch;          // gives back part of the level slope the amp eats
    ToneStack         toneStack;
    // Second instance for MARK mode, where the tone stack sits ahead of the preamp
    // and shapes the distortion itself. Two instances keep the Post path identical.
    ToneStack         toneStackEarly;
    GraphicEQ         graphicEQ;      // Mark-series five-band, sits between tone stack and power amp
    PowerAmp          powerAmp;
    OutputTransformer outputTransformer;
    ReactiveLoad      reactiveLoad;
    SpeakerSim        speakerSim;
    NoiseGate         noiseGate;
    PowerSupply       supply;      // the B+ rail the preamp and power stage share
    Delay             delay;
    Modulation        modulation;
    ReverbEngine      reverb;

    // Smoothed on/off for the three post-FX blocks. A bypass that jumps the wet
    // amount to zero in one block clicks; these ramp it over ~50 ms. Per block,
    // not per sample - the effects already smooth internally from there.
    float revOnZ = 1.f, dlyOnZ = 1.f, modOnZ = 1.f;
    float fxOnSm = 0.2f;

    double currentSampleRate   = 44100.0;
    int    preparedNumChannels = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CopilotToneAudioProcessor)
};
