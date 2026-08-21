/*
  ==============================================================================
    AmpHead Custom  -  PluginEditor.h
    Window: 1020 x 696
  ==============================================================================
*/
#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// One source of truth for the window geometry.
//
// paint() and resized() used to declare these numbers independently: the same
// constants, written twice, in two functions that never compare notes. Moving a
// section meant editing both, and any mismatch showed up as a panel drawn away
// from the controls it is meant to contain. pH was already being computed by two
// different formulas that happened to agree.
//
// Both functions read this now. Change a number here and the panel and its
// contents move together.
struct Layout
{
    int W, H;
    int hdrH, barH;
    int kpY, kpH;            // amp knob panel
    int knobRowY, knobRowH;  // the 8 knobs inside it
    int topoY;               // tone-stack / rectifier switches, below the knobs
    int eqY, eqH;            // graphic EQ strip
    int eqX0, eqBandW;       // first band x, band column width
    int ftrY, ftrH;          // footer band
    int mg, gW, gX;          // margin, noise gate panel
    int irW, irX;            // IR loader panel
    int pfW, pfX;            // post FX panel
    int pY, pH, rTop;        // footer panel top, height, first control row
    int knobGap, knobW;      // amp row: gap between groups, column width

    static Layout compute (int w, int h)
    {
        Layout L;
        L.W    = w;
        L.H    = h;
        L.hdrH = 80;
        L.barH = 38;
        L.kpY      = L.hdrH + 8;
        // The panel is as tall as its contents, not a number someone has to keep in
        // sync: 30 for the group captions, the knob row, then the topology switches.
        L.knobRowY = L.kpY + 30;
        L.knobRowH = 182;
        L.topoY    = L.knobRowY + L.knobRowH + 2;
        L.kpH      = (L.topoY - L.kpY) + 36;

        L.eqY  = L.kpY + L.kpH + 8;
        L.eqH  = 88;
        L.ftrY = L.eqY + L.eqH + 8;
        L.ftrH = L.H - L.barH - L.ftrY;

        L.mg   = 14;
        L.gW   = 196;
        L.pfW  = 336;
        L.irW  = L.W - L.mg * 2 - L.gW - L.pfW - 16;
        L.gX   = L.mg;
        L.irX  = L.gX + L.gW + 8;
        L.pfX  = L.irX + L.irW + 8;
        L.pY   = L.ftrY + 8;
        L.pH   = L.ftrH - 12;
        L.rTop = L.pY + 40;

        L.knobGap = 44;
        L.knobW   = (L.W - 44 - 2 * L.knobGap) / 8;

        // Graphic EQ. The legend on the left used to reserve 226 px for itself plus
        // the two topology combos; those now live in the amp panel, so the bands get
        // the space back.
        const int eqBtnW = 54, eqTitleW = 130;
        L.eqX0    = L.mg + eqTitleW;
        L.eqBandW = (L.W - L.eqX0 - L.mg - eqBtnW - 10) / 5;
        return L;
    }
};

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

    int inClipHold  = 0;   // clip-indicator hold counters (timer ticks)
    int outClipHold = 0;

    // Decaying peak-hold behind the input gain-staging readout. The raw per-tick
    // peak moves far too fast to read, so the maximum is held for ~1 s and then falls.
    float inPeakHold = -120.f;
    int   inPeakWait = 0;

    // Header
    juce::TextButton chBtn[3];
    juce::TextButton brightBtn;

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
        bypIRAtt, gateOnAtt, eqOnAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> eqAtt[5];

    float cachedInDb  = -120.f;
    float cachedOutDb = -120.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CopilotToneAudioProcessorEditor)
};
