/*
  ==============================================================================
    AmpHead Custom  -  PluginEditor.h
    Window: 1020 x Layout::idealHeight() - derived from the bands, not fixed
  ==============================================================================
*/
#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// One source of truth for the window geometry. paint() and resized() both read
// it, so a panel and the controls inside it cannot drift apart. Change a number
// here and everything that depends on it follows.
struct Layout
{
    int W, H;
    int hdrH, barH;
    int chX, chY, chW, chH, chGap;   // channel buttons, in the header
    int kpY, kpH;            // amp knob panel
    int knobRowY, knobRowH;  // the 8 knobs inside it
    int topoY;               // tone-stack / rectifier switches, below the knobs
    int mg;                  // outer margin
    int r2Y, r2H;            // row 2: CABINET | GRAPHIC EQ
    int cabX, cabW;
    int eqPX, eqPW;
    int eqX0, eqBandW;       // first band x, band column width
    int eqFadeY, eqFadeH;    // fader travel inside the EQ panel
    int r3Y, r3H;            // row 3: GATE | FX
    int gateX, gateW;
    int fxX, fxW;
    int knobGap, knobW;      // amp row: gap between groups, column width

    // The window is as tall as the bands, rather than a number kept in step by
    // hand. setSize() calls this.
    static int idealHeight();

    static Layout compute (int w, int h)
    {
        Layout L;
        L.W    = w;
        L.H    = h;
        L.hdrH = 80;
        // Channel buttons. Right-aligned to the same 14 px margin as the panels
        // below, so the header lines up with the rest of the window.
        L.chW   = 104;
        L.chGap = 4;
        L.chH   = 36;
        L.chY   = (L.hdrH - L.chH) / 2 - 6;
        // Four rows in here: meters, input status, target reminder, version.
        L.barH = 58;
        L.kpY      = L.hdrH + 8;
        // Height comes from the contents: caption band, knob row, topology row.
        L.knobRowY = L.kpY + 30;
        L.knobRowH = 182;
        L.topoY    = L.knobRowY + L.knobRowH + 2;
        L.kpH      = (L.topoY - L.kpY) + 36;

        L.mg = 14;
        L.chX = L.W - L.mg - (3 * L.chW + 2 * L.chGap);
        const int usable = L.W - L.mg * 2;
        const int colGap = 8;

        // Row 2: CABINET | GRAPHIC EQ. Split so the EQ panel is tall enough for a
        // vertical fader - a full-width strip is not.
        L.r2Y  = L.kpY + L.kpH + 8;
        L.r2H  = 160;
        L.cabX = L.mg;
        L.cabW = 420;
        L.eqPX = L.cabX + L.cabW + colGap;
        L.eqPW = usable - L.cabW - colGap;

        L.eqX0    = L.eqPX + 20;
        L.eqBandW = (L.eqPW - 40) / 5;
        L.eqFadeY = L.r2Y + 52;
        L.eqFadeH = L.r2H - 66;

        // Row 3: GATE | FX.
        L.r3Y   = L.r2Y + L.r2H + 8;
        L.r3H   = 150;
        L.gateX = L.mg;
        L.gateW = 210;
        L.fxX   = L.gateX + L.gateW + colGap;
        L.fxW   = usable - L.gateW - colGap;

        L.knobGap = 44;
        L.knobW   = (L.W - 44 - 2 * L.knobGap) / 8;

        return L;
    }
};

inline int Layout::idealHeight()
{
    const Layout L = compute (1020, 0);      // nothing above depends on H
    return L.r3Y + L.r3H + 8 + L.barH;
}

//==============================================================================
class AmpLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AmpLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& bg,
                               bool isHighlighted, bool isDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool isDown,
                       int bx, int by, int bw, int bh,
                       juce::ComboBox&) override;

    juce::Font getLabelFont (juce::Label&) override;
};

//==============================================================================
class FolderIconButton : public juce::Button
{
public:
    FolderIconButton() : juce::Button ("folder") {}
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
};

//==============================================================================
class CopilotToneAudioProcessorEditor : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit CopilotToneAudioProcessorEditor (CopilotToneAudioProcessor&);
    ~CopilotToneAudioProcessorEditor() override;

    void paint   (juce::Graphics&) override;
    void resized () override;

private:
    void timerCallback() override;
    void setChannelIndex (int idx);
    void setOsFactorIndex (int idx);
    void drawLevelMeter  (juce::Graphics& g, juce::Rectangle<int> bounds, float dB,
                          bool clip, bool showTarget);

    CopilotToneAudioProcessor& audioProcessor;
    AmpLookAndFeel laf;
    juce::TooltipWindow tooltipWindow { this, 600 };   // hover help for every control

    int lastChan = -1;     // drives the header repaint when the channel changes
    int inClipHold  = 0;   // clip-indicator hold counters (timer ticks)
    int outClipHold = 0;

    // Decaying peak-hold behind the input gain-staging readout. The raw per-tick
    // peak moves far too fast to read, so the maximum is held for ~1 s and then falls.
    float inPeakHold = -120.f;
    int   inPeakWait = 0;

    // Header
    juce::TextButton chBtn[3];
    juce::TextButton brightBtn;
    juce::Label      brightLabel;   // caption, matching STACK and RECT

    // Main 8 knobs
    juce::Slider gainSlider,   charSlider,   bassSlider,   midSlider,
                 trebleSlider, presSlider,   masterSlider, outSlider;
    juce::Label  gainLabel,    charLabel,    bassLabel,    midLabel,
                 trebleLabel,  presLabel,    masterLabel,  outLabel;

    // Noise Gate
    juce::Slider     gateThreshSlider, gateReleaseSlider;
    juce::Label      gateThreshLabel,  gateReleaseLabel;
    juce::TextButton gateOnBtn;        // ON/OFF - gate is bypassed (default) until enabled

    // Mark-series five-band graphic EQ, vertical faders as on the real amp. The
    // sliders carry the faceplate frequencies, the language players use, while the
    // filters run the measured resonances.
    juce::Slider     eqSlider[5];
    juce::Label      eqLabel[5];
    juce::TextButton eqOnBtn;

    // IR Loader
    FolderIconButton            irAFolderBtn, irBFolderBtn;
    juce::TextButton            bypIRBtn;
    juce::Label                 irANameLabel, irBNameLabel;
    juce::Label                 irHintLabel;       // "Don't forget to load an IR!" when none loaded
    juce::Slider                irMixKnob;
    juce::Label                 irMixLabel;
    juce::Slider                irBlendSlider;
    std::unique_ptr<juce::FileChooser> fcA, fcB;

    // Post FX
    juce::ComboBox reverbTypeBox, delayTypeBox;
    juce::Slider   reverbMixKnob, reverbDecayKnob, reverbToneKnob;
    juce::Slider   delayMixKnob,  delayTimeKnob,   delayFbKnob;
    juce::Slider   modDetuneKnob, modChorusKnob,   modRateKnob;
    juce::Label    reverbLabel,   delayLabel,       modLabel;
    juce::TextButton revOnBtn, dlyOnBtn, modOnBtn;   // per-effect bypass
    juce::Label    reverbMixLabel, reverbDecayLabel, reverbToneLabel;
    juce::Label    delayMixLabel,  delayTimeLabel,   delayFbLabel;
    juce::Label    modDetuneLabel, modChorusLabel,   modRateLabel;

    // Bottom bar
    juce::ComboBox oversamplingBox, qualityBox;
    // Topology switches. Tone-stack position is the Mark IV's defining structural
    // trait; rectifier type is the Mesa tube-vs-silicon choice.
    juce::ComboBox stackPosBox, rectBox;
    juce::Label    stackPosLabel, rectLabel;

    // APVTS attachments
    juce::AudioProcessorValueTreeState::SliderAttachment
        gainAtt, charAtt, bassAtt, midAtt, trebleAtt,
        presAtt, masterAtt, outAtt,
        irMixAtt, irBlendAtt,
        gateThreshAtt, gateReleaseAtt,
        reverbMixAtt,  reverbDecayAtt, reverbToneAtt,
        delayMixAtt,   delayTimeAtt,   delayFbAtt,
        modDetuneAtt,  modChorusAtt,   modRateAtt;

    // ComboBox attachments are built in the constructor body (after items are
    // added) so the attachment can read the parameter and select the right item.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        reverbTypeAtt, delayTypeAtt, stackPosAtt, rectAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        bypIRAtt, gateOnAtt, eqOnAtt, revOnAtt, dlyOnAtt, modOnAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> eqAtt[5];

    float cachedInDb  = -120.f;
    float cachedOutDb = -120.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CopilotToneAudioProcessorEditor)
};
