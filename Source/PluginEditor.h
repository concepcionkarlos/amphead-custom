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

    // Drives the "no IR loaded" banner. Cached so the timer repaints that strip
    // only when the state flips, rather than on every tick.
    bool noIRLoaded = true;

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
