/*
  ==============================================================================
    AmpHead Custom  -  PluginEditor.cpp
  ==============================================================================
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Palette
namespace CT {
    // Backgrounds
    static const juce::Colour bg       { 0xff0e0f14 };
    static const juce::Colour bg2      { 0xff12141f };
    static const juce::Colour panelFg  { 0xff1b1e2c };
    static const juce::Colour panelRim { 0xff343954 };
    static const juce::Colour inputBg  { 0xff262a3c };

    // Functional accent - purple
    static const juce::Colour accent   { 0xff7552b8 };
    static const juce::Colour accentHi { 0xff9b7bff };

    // Brand accent - warm amber (for "Custom" word)
    static const juce::Colour amber    { 0xffC4863A };

    // Text
    static const juce::Colour textHi   { 0xffF0F0F8 };
    static const juce::Colour textMid  { 0xffD0D3DA };
    static const juce::Colour textLow  { 0xff8890A0 };
    static const juce::Colour textDim  { 0xff3E4055 };

    static const juce::Colour divider  { 0xff2c3050 };

    // LINE SYSTEM. Two roles, one weight, and nothing else draws a line.
    //   rim   the edge of a panel
    //   rule  any divider INSIDE a panel - header underline, column separator,
    //         knob-group separator, the EQ's 0 dB reference
    // Before this a panel edge was drawn five different ways (panelRim, panelRim at
    // 0.85, a near-black 0xff06070e at 1.5 px, a 0xff222440 at 0.7 alpha and 0.8 px,
    // and divider) and an internal rule came in four alphas. No line carried meaning
    // because every one of them was a separate decision. Alphas are gone: a line is
    // one of these two colours at lineW, or it is not a line.
    // Values chosen by measuring the rendered contrast against the surfaces they
    // are drawn on, not by eye. The first pass at 0x343954 / 0x262a44 measured
    // 1.5:1 and 1.3:1 - consistent, and invisible. Below about 1.5:1 a hairline is
    // not seen, it is inferred.
    //   rim   2.4 : 1 on a panel face
    //   rule  1.7 : 1  - present, subordinate to the edge that contains it
    static const juce::Colour rim      { 0xff4d5480 };
    static const juce::Colour rule     { 0xff3a4066 };
    static constexpr float    lineW = 1.0f;

    // TYPE SCALE. Ratio ~1.22, five steps, nothing below fMicro. Half-pixel
    // differences build no hierarchy, they only multiply the places to edit.
    // 11 px is the practical floor for a desktop UI label: smaller than that,
    // at normal viewing distance, the text is guessed rather than read.
    static constexpr float fMicro = 11.0f;   // units, readouts, fine print
    static constexpr float fLabel = 13.0f;   // control and section labels
    static constexpr float fPanel = 16.0f;   // panel titles
    static constexpr float fChan  = 22.0f;   // channel / emphasis
    static constexpr float fBrand = 28.0f;   // brand lockup
}

//==============================================================================
// AmpLookAndFeel

AmpLookAndFeel::AmpLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId,         CT::bg);
    setColour (juce::ComboBox::backgroundColourId,                CT::inputBg);
    setColour (juce::ComboBox::textColourId,                      CT::textMid);
    setColour (juce::ComboBox::arrowColourId,                     CT::accent);
    setColour (juce::ComboBox::outlineColourId,                   CT::divider);
    setColour (juce::PopupMenu::backgroundColourId,               CT::bg2);
    setColour (juce::PopupMenu::textColourId,                     CT::textMid);
    setColour (juce::PopupMenu::highlightedBackgroundColourId,    CT::accent.withAlpha (0.40f));
    setColour (juce::Label::textColourId,                         CT::textLow);
    setColour (juce::Slider::textBoxTextColourId,                 CT::textMid);
    setColour (juce::Slider::textBoxOutlineColourId,              juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId,           juce::Colours::transparentBlack);
}

void AmpLookAndFeel::drawRotarySlider (juce::Graphics& g,
                                        int x, int y, int w, int h,
                                        float sliderPos,
                                        float startAngle, float endAngle,
                                        juce::Slider&)
{
    using namespace juce;
    const float cx  = x + w * 0.5f,  cy = y + h * 0.5f;
    const float r   = jmin (w, h) * 0.5f - 3.f;
    const float ang = startAngle + sliderPos * (endAngle - startAngle);
    const float aw  = jmax (3.0f, r * 0.13f);
    const float ar  = r - aw * 0.5f - 0.5f;                       // value-arc ring radius
    const float kr  = jmax (6.0f, ar - aw * 0.5f - 2.5f);         // knob body radius (inside the ring)

    // soft drop shadow under the knob
    for (int i = 3; i >= 1; --i)
    {
        const float s = kr + i * 1.7f;
        g.setColour (Colours::black.withAlpha (0.22f / (float) i));
        g.fillEllipse (cx - s + 0.6f, cy - s + 2.6f, s * 2.f, s * 2.f);
    }

    // value track groove
    {
        Path tr;
        tr.addCentredArc (cx, cy, ar, ar, 0, startAngle, endAngle, true);
        g.setColour (CT::inputBg);
        g.strokePath (tr, PathStrokeType (aw, PathStrokeType::curved, PathStrokeType::rounded));
    }

    // value arc (purple) - glow, solid, bright tip - rings the knob
    if (sliderPos > 0.005f)
    {
        Path gl;
        gl.addCentredArc (cx, cy, ar, ar, 0, startAngle, ang, true);
        g.setColour (CT::accentHi.withAlpha (0.16f));
        g.strokePath (gl, PathStrokeType (aw * 3.0f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (CT::accent.withAlpha (0.28f));
        g.strokePath (gl, PathStrokeType (aw * 1.9f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (CT::accentHi);
        g.strokePath (gl, PathStrokeType (aw, PathStrokeType::curved, PathStrokeType::rounded));
        const float tx = cx + ar * std::sin (ang), ty = cy - ar * std::cos (ang);
        g.setColour (Colours::white.withAlpha (0.90f));
        g.fillEllipse (tx - aw * 0.55f, ty - aw * 0.55f, aw * 1.1f, aw * 1.1f);
    }

    // outer chrome bevel
    {
        const float rr = kr + 1.6f;
        g.setColour (Colour (0xff05060c));
        g.drawEllipse (cx - rr, cy - rr, rr * 2.f, rr * 2.f, 2.0f);
        g.setColour (Colour (0xff3a4060).withAlpha (0.90f));
        g.drawEllipse (cx - kr - 0.4f, cy - kr - 0.4f, (kr + 0.4f) * 2.f, (kr + 0.4f) * 2.f, 1.0f);
    }

    // knurled metal skirt (the physical grip)
    {
        // dark metallic base disc
        ColourGradient base (Colour (0xff20223a), cx - kr * 0.5f, cy - kr * 0.6f,
                             Colour (0xff070810), cx + kr * 0.5f, cy + kr * 0.7f, false);
        g.setGradientFill (base);
        g.fillEllipse (cx - kr, cy - kr, kr * 2.f, kr * 2.f);

        // serrated grip ridges, brightness shaded by a fixed top-left light
        const int   teeth    = jlimit (22, 60, roundToInt (kr * 1.25f));
        const float ridgeIn  = kr * 0.60f;
        const float ridgeOut = kr * 0.985f;
        const float lx = -0.55f, ly = -0.83f;     // direction toward the light
        for (int t = 0; t < teeth; ++t)
        {
            const float a  = (float) t / (float) teeth * MathConstants<float>::twoPi;
            const float sa = std::sin (a), ca = std::cos (a);
            const float d  = jlimit (0.f, 1.f, 0.5f + 0.5f * (sa * lx + (-ca) * ly));
            const float br = 0.09f + 0.44f * d;     // ridge highlight strength
            g.setColour (Colour::fromFloatRGBA (br * 0.72f, br * 0.78f, br, 1.f));
            g.drawLine (cx + sa * ridgeIn,  cy - ca * ridgeIn,
                        cx + sa * ridgeOut, cy - ca * ridgeOut, 1.3f);
        }
        // inner shadow ring where the skirt meets the cap
        g.setColour (Colour (0xff05060e).withAlpha (0.80f));
        g.drawEllipse (cx - kr * 0.60f, cy - kr * 0.60f, kr * 1.20f, kr * 1.20f, 1.4f);
    }

    // raised cap (the smooth top face)
    const float capR = kr * 0.58f;
    {
        ColourGradient cap (Colour (0xff2b2e48), cx - capR * 0.5f, cy - capR * 0.7f,
                            Colour (0xff0a0b14), cx + capR * 0.55f, cy + capR * 0.8f, false);
        g.setGradientFill (cap);
        g.fillEllipse (cx - capR, cy - capR, capR * 2.f, capR * 2.f);

        g.setColour (Colour (0xff404668).withAlpha (0.85f));
        g.drawEllipse (cx - capR + 0.4f, cy - capR + 0.4f, capR * 2.f - 0.8f, capR * 2.f - 0.8f, 0.9f);

        // specular highlight, top-left
        ColourGradient hl (Colours::white.withAlpha (0.30f), cx - capR * 0.4f, cy - capR * 0.6f,
                           Colours::white.withAlpha (0.00f), cx + capR * 0.2f, cy + capR * 0.25f, false);
        g.setGradientFill (hl);
        g.fillEllipse (cx - capR * 0.85f, cy - capR * 0.95f, capR * 1.5f, capR * 1.1f);
    }

    // pointer indicator
    {
        const float dx = std::sin (ang), dy = -std::cos (ang);
        // engraved dark channel
        g.setColour (Colour (0xff05060c).withAlpha (0.90f));
        g.drawLine (cx + dx * capR * 0.15f, cy + dy * capR * 0.15f,
                    cx + dx * kr * 0.92f,    cy + dy * kr * 0.92f, 3.0f);
        // bright pointer line
        g.setColour (CT::textHi);
        g.drawLine (cx + dx * capR * 0.20f, cy + dy * capR * 0.20f,
                    cx + dx * kr * 0.86f,    cy + dy * kr * 0.86f, 1.7f);
        // colored tip
        g.setColour (CT::accentHi);
        g.fillEllipse (cx + dx * kr * 0.86f - 2.4f, cy + dy * kr * 0.86f - 2.4f, 4.8f, 4.8f);
        g.setColour (Colours::white.withAlpha (0.85f));
        g.fillEllipse (cx + dx * kr * 0.86f - 1.0f, cy + dy * kr * 0.86f - 1.0f, 2.0f, 2.0f);
    }
}

void AmpLookAndFeel::drawLinearSlider (juce::Graphics& g,
                                        int x, int y, int w, int h,
                                        float pos,
                                        float /*min*/, float /*max*/,
                                        juce::Slider::SliderStyle style,
                                        juce::Slider&)
{
    using namespace juce;

    // Vertical branch. This did not exist: the method returned immediately for
    // anything that was not LinearHorizontal, and overriding a LookAndFeel method
    // replaces the base implementation rather than adding to it - so the five EQ
    // faders were live, draggable and completely invisible.
    if (style == juce::Slider::LinearVertical)
    {
        const float cx  = (float) x + (float) w * 0.5f;
        const float top = (float) y;
        const float bot = (float) (y + h);
        const float mid = (top + bot) * 0.5f;
        const float sw  = 6.f;                       // slot width

        // recessed slot
        g.setColour (Colour (0xff08090e));
        g.fillRoundedRectangle (cx - sw * 0.5f, top, sw, bot - top, 3.f);
        g.setColour (CT::divider);
        g.drawRoundedRectangle (cx - sw * 0.5f, top, sw, bot - top, 3.f, 0.8f);

        // 0 dB mark, wider than the slot so it reads as a scale and not as dirt
        g.setColour (CT::divider);
        g.fillRect (cx - (float) w * 0.40f, mid - 0.5f, (float) w * 0.80f, 1.f);

        // Fill from the centre out, not from the bottom: on a boost/cut control
        // the eye should see how far from flat the band is, and in which direction.
        if (std::abs (pos - mid) > 1.f)
        {
            const float t = jmin (pos, mid), b = jmax (pos, mid);
            g.setColour (pos < mid ? CT::accentHi : CT::accent);
            g.fillRoundedRectangle (cx - sw * 0.5f + 1.f, t, sw - 2.f, b - t, 2.f);
        }

        // cap
        const float capW = jmin ((float) w, 26.f), capH = 13.f;
        const Rectangle<float> cap (cx - capW * 0.5f, pos - capH * 0.5f, capW, capH);
        ColourGradient cg (Colour (0xff3d4260), 0.f, cap.getY(),
                           Colour (0xff191c2b), 0.f, cap.getBottom(), false);
        g.setGradientFill (cg);
        g.fillRoundedRectangle (cap, 2.5f);
        g.setColour (Colour (0xff06070e));
        g.drawRoundedRectangle (cap.reduced (0.5f), 2.5f, 1.f);
        g.setColour (CT::accentHi);
        g.fillRect (cap.getX() + 3.f, pos - 0.75f, cap.getWidth() - 6.f, 1.5f);
        return;
    }

    if (style != juce::Slider::LinearHorizontal) return;

    const float ty = y + h * 0.5f, th = 4.f;
    const float rx = (float) x, rw = (float) w;

    g.setColour (CT::inputBg);
    g.fillRoundedRectangle (rx, ty - th * 0.5f, rw, th, 2.f);
    g.setColour (CT::divider);
    g.drawRoundedRectangle (rx, ty - th * 0.5f, rw, th, 2.f, 0.8f);

    if (pos > rx + 2.f)
    {
        ColourGradient fg (CT::accentHi, rx, ty, CT::accent, pos, ty, false);
        g.setGradientFill (fg);
        g.fillRoundedRectangle (rx, ty - th * 0.5f, pos - rx, th, 2.f);
    }

    // thumb
    const float tr = 8.f;
    g.setColour (CT::textHi);
    g.fillEllipse (pos - tr, ty - tr, tr * 2.f, tr * 2.f);
    g.setColour (CT::divider);
    g.drawEllipse (pos - tr + 0.5f, ty - tr + 0.5f, tr * 2.f - 1.f, tr * 2.f - 1.f, 1.f);
    g.setColour (CT::accent);
    g.fillEllipse (pos - 2.5f, ty - 2.5f, 5.f, 5.f);
}

void AmpLookAndFeel::drawButtonBackground (juce::Graphics& g,
                                            juce::Button& btn,
                                            const juce::Colour& /*bg*/,
                                            bool hi, bool down)
{
    const auto  b = btn.getLocalBounds().toFloat().reduced (0.5f);
    const float r = 6.f;

    // Purple means "in the signal path" on every switch in this editor. BYPASS IR
    // is the one whose lit state means the opposite, so it lights amber instead -
    // otherwise the accent colour would mean two contradictory things at once.
    const bool  warn = (bool) btn.getProperties().getWithDefault ("warnWhenOn", false);
    const auto  onCol   = warn ? CT::amber : CT::accent;
    const auto  onColHi = warn ? CT::amber.brighter (0.35f) : CT::accentHi;

    if (btn.getToggleState())
    {
        // Active: solid base, subtle top gloss, bright border
        g.setColour (onCol);
        g.fillRoundedRectangle (b, r);

        // Micro top-gloss highlight
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRoundedRectangle (b.reduced (2.f).removeFromTop (b.getHeight() * 0.45f), r * 0.65f);

        // Bright border
        g.setColour (onColHi.withAlpha (0.70f));
        g.drawRoundedRectangle (b, r, 1.2f);
    }
    else
    {
        // Inactive: very dark fill, visible gray border
        const juce::Colour fill = down ? onCol.withAlpha (0.28f)
                                 : hi  ? CT::panelFg.brighter (0.18f)
                                       : CT::panelFg;
        g.setColour (fill);
        g.fillRoundedRectangle (b, r);

        // Visible border
        g.setColour (CT::panelRim.brighter (0.10f));
        g.drawRoundedRectangle (b, r, 1.2f);
    }
}

void AmpLookAndFeel::drawButtonText (juce::Graphics& g,
                                      juce::TextButton& btn,
                                      bool /*hi*/, bool /*dn*/)
{
    // Bold text: white when active, mid-gray when not. The channel buttons are the
    // tallest buttons in the editor, so height alone tells them apart from BYPASS
    // IR / ON / IR A / IR B, with no special-casing by name. They take the emphasis
    // step of the scale because switching channel is the biggest change this amp
    // makes. drawFittedText shrinks rather than clips, so the larger size is safe.
    const float fs = btn.getHeight() >= 34 ? CT::fChan : CT::fLabel;
    g.setFont (juce::Font (juce::FontOptions (fs, juce::Font::bold)));

    // Honour the colours the button was configured with. This used to be hard-coded
    // to textHi / textMid, which quietly made twelve setColour calls in the editor
    // dead - and put MORE contrast on the inactive label (textMid on the dark fill
    // is 11:1) than on the active one (textHi on accent is 5:1), so an off button
    // read as the selected one.
    g.setColour (btn.findColour (btn.getToggleState()
                                 ? juce::TextButton::textColourOnId
                                 : juce::TextButton::textColourOffId));

    // A switch captioned "ON" while it is off cannot be read: it could equally mean
    // "this is on" or "press to turn it on". These say which of the two it is.
    juce::String txt = btn.getButtonText();
    if ((bool) btn.getProperties().getWithDefault ("onOffCaption", false))
        txt = btn.getToggleState() ? "ON" : "OFF";

    g.drawFittedText (txt, btn.getLocalBounds().reduced (4, 2),
                      juce::Justification::centred, 1);
}

void AmpLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h,
                                    bool /*dn*/, int bx, int by, int bw, int bh,
                                    juce::ComboBox&)
{
    const auto b = juce::Rectangle<float> (0.f, 0.f, (float) w, (float) h);
    g.setColour (CT::inputBg);
    g.fillRoundedRectangle (b, 4.f);
    g.setColour (CT::divider);
    g.drawRoundedRectangle (b.reduced (0.5f), 4.f, 1.f);

    const float ax = bx + bw * 0.5f, ay = by + bh * 0.5f;
    juce::Path arr;
    arr.addTriangle (ax - 4.f, ay - 2.5f, ax + 4.f, ay - 2.5f, ax, ay + 3.f);
    g.setColour (CT::accent);
    g.fillPath (arr);
}

juce::Font AmpLookAndFeel::getLabelFont (juce::Label& l)
{
    // Return what the label was given. This used to force Font::bold on every
    // label in the editor, which silently overrode fonts set as plain 370 lines
    // away - the IR filename fields among them - and left no weight axis to build
    // hierarchy with, because everything was already bold. styleLabel() asks for
    // bold where bold is wanted.
    return l.getFont();
}

//==============================================================================
// FolderIconButton
void FolderIconButton::paintButton (juce::Graphics& g, bool hi, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (1.f);
    g.setColour (down ? CT::accent.withAlpha (0.55f)
                 : hi  ? CT::panelFg.brighter (0.16f)
                       : CT::inputBg);
    g.fillRoundedRectangle (b, 4.f);
    g.setColour (CT::divider);
    g.drawRoundedRectangle (b.reduced (0.5f), 4.f, 1.f);

    const float fx = b.getX() + 4.f, fy = b.getY() + 4.f;
    const float fw = b.getWidth() - 8.f, fh = b.getHeight() - 8.f;
    juce::Path p;
    p.addRoundedRectangle (fx, fy + fh * 0.28f, fw, fh * 0.72f, 1.5f);
    p.addRoundedRectangle (fx, fy + fh * 0.16f, fw * 0.44f, fh * 0.20f, 1.f);
    g.setColour (hi || down ? CT::textMid : CT::textLow);
    g.fillPath (p);
}

//==============================================================================
// helpers
// Every line in the editor goes through one of these two.
static void panelEdge (juce::Graphics& g, juce::Rectangle<float> r, float radius,
                       juce::Colour c = CT::rim)
{
    g.setColour (c);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, CT::lineW);
}

static void rule (juce::Graphics& g, float x, float y, float w, float h)
{
    g.setColour (CT::rule);
    g.fillRect (x, y, w, h);
}

static void styleKnob (juce::Slider& s, AmpLookAndFeel& laf, int tbW = 64)
{
    s.setLookAndFeel (&laf);
    s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, tbW, 16);
    s.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                           juce::MathConstants<float>::pi * 2.75f, true);
}

static void styleLabel (juce::Label& l, const juce::String& t, float fs = CT::fMicro)
{
    l.setText (t, juce::dontSendNotification);
    l.setFont (juce::Font (juce::FontOptions (fs, juce::Font::bold)));
    l.setColour (juce::Label::textColourId, CT::textLow);
    l.setJustificationType (juce::Justification::centred);
}

//==============================================================================
// Constructor
CopilotToneAudioProcessorEditor::CopilotToneAudioProcessorEditor (
    CopilotToneAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      gainAtt        (p.apvts, "drive",       gainSlider),
      charAtt        (p.apvts, "char",        charSlider),
      bassAtt        (p.apvts, "bass",        bassSlider),
      midAtt         (p.apvts, "mid",         midSlider),
      trebleAtt      (p.apvts, "treble",      trebleSlider),
      presAtt        (p.apvts, "presence",    presSlider),
      masterAtt      (p.apvts, "master",      masterSlider),
      outAtt         (p.apvts, "output",      outSlider),
      irMixAtt       (p.apvts, "irMix",       irMixKnob),
      irBlendAtt     (p.apvts, "irBlend",     irBlendSlider),
      gateThreshAtt  (p.apvts, "gateThresh",  gateThreshSlider),
      gateReleaseAtt (p.apvts, "gateRelease", gateReleaseSlider),
      reverbMixAtt   (p.apvts, "reverbMix",   reverbMixKnob),
      reverbDecayAtt (p.apvts, "reverbDecay", reverbDecayKnob),
      reverbToneAtt  (p.apvts, "reverbTone",  reverbToneKnob),
      delayMixAtt    (p.apvts, "delayMix",    delayMixKnob),
      delayTimeAtt   (p.apvts, "delayTime",   delayTimeKnob),
      delayFbAtt     (p.apvts, "delayFeedback", delayFbKnob),
      modDetuneAtt   (p.apvts, "modDetune",   modDetuneKnob),
      modChorusAtt   (p.apvts, "modChorus",   modChorusKnob),
      modRateAtt     (p.apvts, "modRate",     modRateKnob)
{
    setSize (1020, Layout::idealHeight());
    setLookAndFeel (&laf);

    // channel buttons
    const juce::String chNames[] = { "CLEAN", "CRUNCH", "LEAD" };
    for (int i = 0; i < 3; ++i)
    {
        chBtn[i].setLookAndFeel (&laf);
        chBtn[i].setButtonText (chNames[i]);
        chBtn[i].setClickingTogglesState (true);
        chBtn[i].setRadioGroupId (1001);
        chBtn[i].setColour (juce::TextButton::textColourOffId, CT::textLow);
        chBtn[i].setColour (juce::TextButton::textColourOnId,  CT::textHi);
        chBtn[i].onClick = [this, i] { setChannelIndex (i); };
        addAndMakeVisible (chBtn[i]);
    }

    // BRIGHT toggle (second row in header, under LEAD)
    brightBtn.setLookAndFeel (&laf);
    // BRIGHT sits in a row with two dropdowns of identical size. Without a caption
    // of its own it was the odd one out, and the OFF fill (panelFg) against the
    // combo fill (inputBg) is only 1.4:1 - three dark slabs, one unlabelled. Give it
    // the same caption-above-control shape as STACK and RECT, and let the button
    // itself report ON / OFF.
    styleLabel (brightLabel, "BRIGHT", CT::fMicro);
    addAndMakeVisible (brightLabel);
    brightBtn.getProperties().set ("onOffCaption", true);
    brightBtn.setButtonText ("BRIGHT");
    brightBtn.setClickingTogglesState (true);
    brightBtn.setColour (juce::TextButton::textColourOffId, CT::textLow);
    brightBtn.setColour (juce::TextButton::textColourOnId,  CT::textHi);
    brightBtn.onClick = [this]
    {
        if (auto* param = audioProcessor.apvts.getParameter ("bright"))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (brightBtn.getToggleState() ? 1.f : 0.f);
            param->endChangeGesture();
        }
    };
    addAndMakeVisible (brightBtn);

    chBtn[0].setTooltip ("CLEAN channel");
    chBtn[1].setTooltip ("CRUNCH channel");
    chBtn[2].setTooltip ("LEAD channel - most gain and harmonics");
    brightBtn.setTooltip ("Tube-Screamer-style OD boost: tightens lows, pushes mids");

    // 8 main knobs
    struct KI { juce::Slider& s; juce::Label& l; const char* n; const char* tip; };
    KI ki[] = {
        { gainSlider,   gainLabel,   "GAIN",     "Preamp gain / distortion amount" },
        { charSlider,   charLabel,   "DRIVE",    "Character: voicing + even-harmonic content" },
        { bassSlider,   bassLabel,   "BASS",     "Low end" },
        { midSlider,    midLabel,    "MID",      "Midrange (vocal/honk)" },
        { trebleSlider, trebleLabel, "TREBLE",   "High end / presence" },
        { presSlider,   presLabel,   "PRESENCE", "Power-amp presence shelf (2.5 kHz)" },
        { masterSlider, masterLabel, "MASTER",   "Power-amp drive: more = more compression/sag" },
        { outSlider,    outLabel,    "OUTPUT",   "Final output level (dB)" },
    };
    for (auto& k : ki)
    {
        styleKnob (k.s, laf, 64);
        styleLabel (k.l, k.n);
        k.s.setTooltip (k.tip);
        addAndMakeVisible (k.s);
        addAndMakeVisible (k.l);
    }

    // gate knobs
    styleKnob (gateThreshSlider, laf, 60);
    styleKnob (gateReleaseSlider, laf, 60);
    styleLabel (gateThreshLabel,  "THRESHOLD");
    styleLabel (gateReleaseLabel, "RELEASE");
    gateThreshSlider.setTooltip ("Noise-gate threshold (dB)");
    gateReleaseSlider.setTooltip ("Noise-gate release time (ms)");
    addAndMakeVisible (gateThreshSlider);
    addAndMakeVisible (gateReleaseSlider);

    // Gate ON/OFF - bypassed by default; only touches the sound when enabled.
    gateOnBtn.setLookAndFeel (&laf);
    gateOnBtn.setButtonText ("ON");
    gateOnBtn.setClickingTogglesState (true);
    gateOnBtn.getProperties().set ("onOffCaption", true);
    gateOnBtn.setColour (juce::TextButton::textColourOffId, CT::textLow);
    gateOnBtn.setColour (juce::TextButton::textColourOnId,  CT::textHi);
    gateOnBtn.setTooltip ("Enable the noise gate (off = fully out of the signal path)");
    addAndMakeVisible (gateOnBtn);
    gateOnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, "gateOn", gateOnBtn);
    addAndMakeVisible (gateThreshLabel);
    addAndMakeVisible (gateReleaseLabel);

    // GRAPHIC EQ - five vertical faders, Mark-series style.
    // Labelled with the faceplate frequencies (80/240/750/2200/6600) because that is
    // what players say; the filters themselves run the measured resonances
    // 87.61 / 371.74 / 723.43 / 1575.87 / 4822.88 Hz.
    {
        static const char* ids  [5] = { "eq80", "eq240", "eq750", "eq2200", "eq6600" };
        static const char* names[5] = { "80", "240", "750", "2.2k", "6.6k" };
        static const char* tips [5] = {
            "87.6 Hz - the thump. Boosted hard in the Classic V.",
            "372 Hz - low mids. Note the real centre is well above the printed 240.",
            "723 Hz - THE crucial one. Cutting this is what makes the V a V; the manual "
            "calls it by far the most important slider.",
            "1.58 kHz - upper mids and bite.",
            "4.82 kHz - presence and edge. Boosted hard in the Classic V." };
        for (int i = 0; i < 5; ++i)
        {
            eqSlider[i].setSliderStyle (juce::Slider::LinearVertical);
            eqSlider[i].setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
            eqSlider[i].setTooltip (tips[i]);
            addAndMakeVisible (eqSlider[i]);
            eqAtt[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                audioProcessor.apvts, ids[i], eqSlider[i]);
            styleLabel (eqLabel[i], names[i], CT::fLabel);
            addAndMakeVisible (eqLabel[i]);
        }
        eqOnBtn.setButtonText ("EQ");
        eqOnBtn.setClickingTogglesState (true);
        eqOnBtn.setColour (juce::TextButton::textColourOffId, CT::textLow);
        eqOnBtn.setColour (juce::TextButton::textColourOnId,  CT::textHi);
        eqOnBtn.setTooltip ("Graphic EQ in / out. Off by default - it is a large tone change.");
        eqOnBtn.setLookAndFeel (&laf);
        addAndMakeVisible (eqOnBtn);
        eqOnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            audioProcessor.apvts, "eqOn", eqOnBtn);
    }

    // Per-effect bypass. Each switch sits in its own FX column header, next to the
    // effect it switches.
    {
        struct FxOn { juce::TextButton& b; const char* id; const char* tip; };
        const FxOn fx[3] = {
            { revOnBtn, "revOn", "Reverb in / out" },
            { dlyOnBtn, "dlyOn", "Delay in / out"  },
            { modOnBtn, "modOn", "Detune and chorus in / out. The amp's background "
                                 "ensemble stays either way - it is part of the voice, "
                                 "not an effect." },
        };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>* att[3] =
            { &revOnAtt, &dlyOnAtt, &modOnAtt };

        for (int i = 0; i < 3; ++i)
        {
            fx[i].b.setButtonText ("ON");
            fx[i].b.setClickingTogglesState (true);
            fx[i].b.getProperties().set ("onOffCaption", true);
            fx[i].b.setColour (juce::TextButton::textColourOffId, CT::textLow);
            fx[i].b.setColour (juce::TextButton::textColourOnId,  CT::textHi);
            fx[i].b.setTooltip (fx[i].tip);
            fx[i].b.setLookAndFeel (&laf);
            addAndMakeVisible (fx[i].b);
            *att[i] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                audioProcessor.apvts, fx[i].id, fx[i].b);
        }
    }

    // TOPOLOGY switches, sharing the EQ strip because they belong to the same idea:
    // what kind of amp this is, rather than how it is dialled in.
    {
        stackPosBox.setLookAndFeel (&laf);
        stackPosBox.addItem ("POST", 1);
        stackPosBox.addItem ("MARK", 2);
        stackPosBox.setTooltip ("Where the tone stack sits. POST = tone controls colour the "
                                "finished distortion. MARK = tone stack ahead of the preamp, "
                                "Mesa Mark style, so the tone knobs decide what gets distorted. "
                                "MARK is clearer and far more level-stable across tone settings.");
        addAndMakeVisible (stackPosBox);
        stackPosAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            audioProcessor.apvts, "stackPos", stackPosBox);
        styleLabel (stackPosLabel, "STACK", CT::fMicro);
        addAndMakeVisible (stackPosLabel);

        rectBox.setLookAndFeel (&laf);
        rectBox.addItem ("SILICON", 1);
        rectBox.addItem ("TUBE", 2);
        rectBox.setTooltip ("Rectifier. SILICON is stiff and tight. TUBE sags - B+ drops when "
                            "the power stage draws current and recovers when it lets go, which "
                            "is a 5U4 on a Mesa.");
        addAndMakeVisible (rectBox);
        rectAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            audioProcessor.apvts, "rectifier", rectBox);
        styleLabel (rectLabel, "RECT", CT::fMicro);
        addAndMakeVisible (rectLabel);
    }

    // IR folder buttons
    for (auto* fb : { &irAFolderBtn, &irBFolderBtn })
    {
        fb->setLookAndFeel (&laf);
        addAndMakeVisible (fb);
    }
    irAFolderBtn.onClick = [this]
    {
        fcA = std::make_unique<juce::FileChooser> ("Load Impulse Response A",
            audioProcessor.irBrowseStart(),
            "*.wav;*.aif;*.aiff");
        fcA->launchAsync (juce::FileBrowserComponent::openMode |
                          juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f.existsAsFile()) {
                    audioProcessor.loadIR (f);
                    audioProcessor.lastIRDir = f.getParentDirectory().getFullPathName();
                    irANameLabel.setText (f.getFileNameWithoutExtension(),
                                          juce::dontSendNotification);
                }
            });
    };
    irBFolderBtn.onClick = [this]
    {
        fcB = std::make_unique<juce::FileChooser> ("Load Impulse Response B",
            audioProcessor.irBrowseStart(),
            "*.wav;*.aif;*.aiff");
        fcB->launchAsync (juce::FileBrowserComponent::openMode |
                          juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f.existsAsFile()) {
                    audioProcessor.loadIRB (f);
                    audioProcessor.lastIRDir = f.getParentDirectory().getFullPathName();
                    irBNameLabel.setText (f.getFileNameWithoutExtension(),
                                          juce::dontSendNotification);
                }
            });
    };

    for (auto* lbl : { &irANameLabel, &irBNameLabel })
    {
        lbl->setText ("No IR loaded", juce::dontSendNotification);
        lbl->setFont (juce::Font (juce::FontOptions (CT::fLabel)));
        lbl->setColour (juce::Label::textColourId, CT::textMid);
        lbl->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (lbl);
    }

    // IR reminder - shown (in amber) only while no IR is loaded; cleared once one is.
    // The only no-IR warning in the interface. It lives inside the cabinet panel,
    // where the fix is, instead of shouting from a banner across the whole window.
    irHintLabel.setText ("Load a cabinet IR", juce::dontSendNotification);
    irHintLabel.setFont (juce::Font (juce::FontOptions (CT::fLabel, juce::Font::bold)));
    irHintLabel.setColour (juce::Label::textColourId, CT::amber);
    irHintLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (irHintLabel);

    styleKnob (irMixKnob, laf, 52);
    irMixKnob.textFromValueFunction = [] (double v) {
        return juce::String (juce::roundToInt (v * 100)) + "%";
    };
    irMixKnob.valueFromTextFunction = [] (const juce::String& t) {
        return t.getDoubleValue() / 100.0;
    };
    styleLabel (irMixLabel, "BLEND");
    irMixKnob.setTooltip ("Cabinet IR wet/dry blend");
    // Not shown. The cab blend is left at 100% in practice, and the panel reads
    // better without it. The parameter and its attachment stay, so a host can
    // still automate irMix and saved sessions still recall it.

    irAFolderBtn.setTooltip ("Load impulse response A (.wav/.aif)");
    irBFolderBtn.setTooltip ("Load impulse response B (.wav/.aif)");
    irBlendSlider.setTooltip ("A / B impulse-response crossfade");
    bypIRBtn.setTooltip ("Bypass the cabinet IR (amp head only)");

    irBlendSlider.setLookAndFeel (&laf);
    irBlendSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    irBlendSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible (irBlendSlider);

    bypIRBtn.setLookAndFeel (&laf);
    bypIRBtn.setButtonText ("BYPASS IR");
    bypIRBtn.setClickingTogglesState (true);
    bypIRBtn.getProperties().set ("warnWhenOn", true);
    bypIRBtn.setColour (juce::TextButton::textColourOffId, CT::textLow);
    bypIRBtn.setColour (juce::TextButton::textColourOnId,  CT::textHi);
    addAndMakeVisible (bypIRBtn);
    bypIRAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, "bypassIR", bypIRBtn);

    // Post FX voices. The type dropdowns are choice params; each effect carries
    // three live knobs.
    reverbTypeBox.setLookAndFeel (&laf);
    reverbTypeBox.addItem ("Spring", 1);
    reverbTypeBox.addItem ("Hall",   2);
    reverbTypeBox.addItem ("Room",   3);
    reverbTypeBox.addItem ("Plate",  4);
    addAndMakeVisible (reverbTypeBox);
    reverbTypeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, "reverbType", reverbTypeBox);

    delayTypeBox.setLookAndFeel (&laf);
    delayTypeBox.addItem ("Digital", 1);
    delayTypeBox.addItem ("Analog",  2);
    delayTypeBox.addItem ("Tape",    3);
    addAndMakeVisible (delayTypeBox);
    delayTypeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, "delayType", delayTypeBox);

    // Small FX knobs - pedal-style (no numeric box), caption label below.
    const auto styleFxKnob = [&] (juce::Slider& s)
    {
        styleKnob (s, laf, 44);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    };
    for (auto* s : { &reverbMixKnob, &reverbDecayKnob, &reverbToneKnob,
                     &delayMixKnob,  &delayTimeKnob,   &delayFbKnob,
                     &modDetuneKnob, &modChorusKnob,   &modRateKnob })
    {
        styleFxKnob (*s);
        addAndMakeVisible (*s);
    }
    reverbTypeBox.setTooltip   ("Reverb voice: Spring / Hall / Room / Plate");
    delayTypeBox.setTooltip    ("Delay voice: Digital / Analog / Tape");
    reverbMixKnob.setTooltip   ("Reverb amount");
    reverbDecayKnob.setTooltip ("Reverb decay / size");
    reverbToneKnob.setTooltip  ("Reverb tone: dark - bright");
    delayMixKnob.setTooltip    ("Delay amount");
    delayTimeKnob.setTooltip   ("Delay time (ms)");
    delayFbKnob.setTooltip     ("Delay feedback / repeats");
    modDetuneKnob.setTooltip   ("Background detune width (ambient shimmer)");
    modChorusKnob.setTooltip   ("Background chorus depth");
    modRateKnob.setTooltip     ("Modulation rate");

    styleLabel (reverbLabel, "REVERB", CT::fMicro);
    styleLabel (delayLabel,  "DELAY",  CT::fMicro);
    styleLabel (modLabel,    "MODULATION", CT::fMicro);
    for (auto* l : { &reverbLabel, &delayLabel, &modLabel })
    {
        l->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (*l);
    }

    struct FxL { juce::Label& l; const char* t; };
    FxL fxl[] = {
        { reverbMixLabel,   "MIX"    }, { reverbDecayLabel, "DECAY"  }, { reverbToneLabel, "TONE" },
        { delayMixLabel,    "MIX"    }, { delayTimeLabel,   "TIME"   }, { delayFbLabel,    "FDBK" },
        { modDetuneLabel,   "DETUNE" }, { modChorusLabel,   "CHORUS" }, { modRateLabel,    "RATE" },
    };
    for (auto& f : fxl)
    {
        styleLabel (f.l, f.t, CT::fMicro);
        addAndMakeVisible (f.l);
    }

    // bottom bar combos
    // OVERSAMPLING (x1/x2/x4/x8) and QUALITY (Draft/Normal/High/Ultra) are two
    // views of the SAME real oversampling factor (param "osFactor", index 0-3).
    // Changing either drives the param; the timer keeps both combos in sync.
    oversamplingBox.setLookAndFeel (&laf);
    oversamplingBox.addItem ("x1", 1);
    oversamplingBox.addItem ("x2", 2);
    oversamplingBox.addItem ("x4", 3);
    oversamplingBox.addItem ("x8", 4);
    oversamplingBox.onChange = [this] { setOsFactorIndex (oversamplingBox.getSelectedId() - 1); };
    oversamplingBox.setTooltip ("Oversampling of the nonlinear stages: higher = less aliasing, more CPU");
    addAndMakeVisible (oversamplingBox);

    qualityBox.setLookAndFeel (&laf);
    qualityBox.addItem ("Draft",  1);
    qualityBox.addItem ("Normal", 2);
    qualityBox.addItem ("High",   3);
    qualityBox.addItem ("Ultra",  4);
    qualityBox.onChange = [this] { setOsFactorIndex (qualityBox.getSelectedId() - 1); };
    qualityBox.setTooltip ("Render quality (mirrors Oversampling): Draft=x1, Normal=x2, High=x4, Ultra=x8");
    addAndMakeVisible (qualityBox);

    startTimerHz (24);
}

//==============================================================================
CopilotToneAudioProcessorEditor::~CopilotToneAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
    for (auto& b : chBtn)            b.setLookAndFeel (nullptr);
    brightBtn.setLookAndFeel         (nullptr);
    for (auto* s : { &gainSlider, &charSlider, &bassSlider, &midSlider,
                     &trebleSlider, &presSlider, &masterSlider, &outSlider })
        s->setLookAndFeel (nullptr);
    gateThreshSlider.setLookAndFeel  (nullptr);
    gateReleaseSlider.setLookAndFeel (nullptr);
    gateOnBtn.setLookAndFeel         (nullptr);
    revOnBtn.setLookAndFeel          (nullptr);
    dlyOnBtn.setLookAndFeel          (nullptr);
    modOnBtn.setLookAndFeel          (nullptr);
    irAFolderBtn.setLookAndFeel      (nullptr);
    irBFolderBtn.setLookAndFeel      (nullptr);
    irMixKnob.setLookAndFeel         (nullptr);
    irBlendSlider.setLookAndFeel     (nullptr);
    bypIRBtn.setLookAndFeel          (nullptr);
    reverbTypeBox.setLookAndFeel     (nullptr);
    delayTypeBox.setLookAndFeel      (nullptr);
    for (auto* s : { &reverbMixKnob, &reverbDecayKnob, &reverbToneKnob,
                     &delayMixKnob,  &delayTimeKnob,   &delayFbKnob,
                     &modDetuneKnob, &modChorusKnob,   &modRateKnob })
        s->setLookAndFeel (nullptr);
    oversamplingBox.setLookAndFeel   (nullptr);
    qualityBox.setLookAndFeel        (nullptr);
}

//==============================================================================
void CopilotToneAudioProcessorEditor::timerCallback()
{
    const int ch = (int) audioProcessor.apvts.getRawParameterValue ("channel")->load();
    for (int i = 0; i < 3; ++i)
        chBtn[i].setToggleState (ch == i, juce::dontSendNotification);

    brightBtn.setToggleState (
        audioProcessor.apvts.getRawParameterValue ("bright")->load() > 0.5f,
        juce::dontSendNotification);

    // Three states, not two. "No IR loaded" and "that cab could not be found" are
    // different problems with different answers: one means load something, the other
    // means go and find THAT file. Naming the missing cab is the point, since a
    // generic "no IR" says nothing about what to look for.
    auto irText = [] (bool loaded, bool missing, const juce::String& name)
    {
        if (loaded)  return name;
        if (missing) return name + "  (not found)";
        return juce::String ("No IR loaded");
    };
    irANameLabel.setText (irText (audioProcessor.irSection.irALoaded.load(),
                                  audioProcessor.irSection.irAMissing.load(),
                                  audioProcessor.irSection.irAName),
                          juce::dontSendNotification);
    irBNameLabel.setText (irText (audioProcessor.irSection.irBLoaded.load(),
                                  audioProcessor.irSection.irBMissing.load(),
                                  audioProcessor.irSection.irBName),
                          juce::dontSendNotification);
    irANameLabel.setColour (juce::Label::textColourId,
        audioProcessor.irSection.irAMissing.load() ? juce::Colour (0xfffb923c) : CT::textMid);
    irBNameLabel.setColour (juce::Label::textColourId,
        audioProcessor.irSection.irBMissing.load() ? juce::Colour (0xfffb923c) : CT::textMid);

    // Reminder visible only while no IR is loaded
    const bool anyIR = audioProcessor.irSection.irALoaded.load()
                    || audioProcessor.irSection.irBLoaded.load();
    if (irHintLabel.isVisible() == anyIR)
        irHintLabel.setVisible (! anyIR);

    // The active-channel underline is drawn by paint() at y = 53, just BELOW the
    // channel buttons, so clicking one invalidates the button and not the underline:
    // the old channel's mark stayed on screen and the new one never appeared.
    // Watching the parameter here catches host automation too, which a repaint from
    // the button's own callback would miss.
    const int chanNow = (int) audioProcessor.apvts.getRawParameterValue ("channel")->load();
    if (chanNow != lastChan)
    {
        lastChan = chanNow;
        repaint (0, 0, getWidth(), 80);      // the header band
    }

    // Keep the OVERSAMPLING + QUALITY combos mirrored to the shared osFactor.
    const int osi = juce::jlimit (0, 3,
        (int) audioProcessor.apvts.getRawParameterValue ("osFactor")->load());
    if (oversamplingBox.getSelectedId() != osi + 1)
        oversamplingBox.setSelectedId (osi + 1, juce::dontSendNotification);
    if (qualityBox.getSelectedId() != osi + 1)
        qualityBox.setSelectedId (osi + 1, juce::dontSendNotification);

    // exchange(0) consumes the accumulated peak, so every block since the last
    // tick is accounted for even at a 24 Hz poll rate.
    const float inPk  = audioProcessor.inputPeakLin .exchange (0.f);
    const float outPk = audioProcessor.outputPeakLin.exchange (0.f);
    cachedInDb  = inPk  > 1e-7f ? juce::Decibels::gainToDecibels (inPk)  : -120.f;
    cachedOutDb = outPk > 1e-7f ? juce::Decibels::gainToDecibels (outPk) : -120.f;
    // Clip hold (~1.5 s at 24 Hz): light/keep the top segment red after a peak >= -0.3 dB.
    inClipHold  = (cachedInDb  >= -0.3f) ? 36 : juce::jmax (0, inClipHold  - 1);
    outClipHold = (cachedOutDb >= -0.3f) ? 36 : juce::jmax (0, outClipHold - 1);

    // Peak-hold feeding the gain-staging readout: catch the peak, sit on it for
    // ~1 s, then fall about 12 dB/s. Without this the text flickers on every note.
    if (cachedInDb > inPeakHold) { inPeakHold = cachedInDb; inPeakWait = 24; }
    else if (inPeakWait > 0)     { --inPeakWait; }
    else                         { inPeakHold = juce::jmax (-120.f, inPeakHold - 0.5f); }
    repaint (0, getHeight() - 38, getWidth(), 38);
}

void CopilotToneAudioProcessorEditor::setChannelIndex (int idx)
{
    if (auto* p = audioProcessor.apvts.getParameter ("channel"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost ((float) idx / 2.f);
        p->endChangeGesture();
    }
}

void CopilotToneAudioProcessorEditor::setOsFactorIndex (int idx)
{
    idx = juce::jlimit (0, 3, idx);
    if (auto* p = audioProcessor.apvts.getParameter ("osFactor"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost ((float) idx / 3.f);   // 4-choice param -> normalized
        p->endChangeGesture();
    }
}

//==============================================================================
// Level meter: 20 segments, 3 dB apart, running -60 dB up to -3 dB. The colours
// read as gain-staging advice rather than loudness, so levels below the target
// window are dimmed as well - too soft is a problem too.
//
// showTarget draws the -18 to -12 dBFS bracket used for setting input level. That
// window is the amp-sim convention, not the general -18 dBFS RMS tracking rule.
// Peak is the right metric because peak voltage drives the modelled input stage,
// and a guitar crest factor of 15-20 dB would put peaks near clipping if -18 dBFS
// average were the target. The bracket is placed by dB rather than by segment
// index, so it survives a change to the segment layout.
void CopilotToneAudioProcessorEditor::drawLevelMeter (juce::Graphics& g,
                                                        juce::Rectangle<int> b,
                                                        float dB, bool clip, bool showTarget)
{
    const int   N    = 20;
    const float sw   = (b.getWidth() - N + 1.f) / (float) N;
    auto xForDb = [&] (float d) { return b.getX() + ((d + 60.f) / 3.f) * (sw + 1.f); };

    for (int i = 0; i < N; ++i)
    {
        const float sd = -60.f + (float) i * 3.f;
        const float sx = b.getX() + i * (sw + 1.f);
        const bool  on = dB >= sd;

        juce::Colour c;
        if      (sd < -18.f) c = on ? juce::Colour (0xff15803d) : juce::Colour (0xff0c1c10);
        else if (sd < -12.f) c = on ? juce::Colour (0xff22c55e) : juce::Colour (0xff0c1c10);
        else if (sd <  -6.f) c = on ? juce::Colour (0xffecba08) : juce::Colour (0xff201800);
        else                 c = on ? juce::Colour (0xffef4444) : juce::Colour (0xff200808);

        // Hold a bright red on the top segment when a clip was detected.
        if (clip && i == N - 1) c = juce::Colour (0xffff5555);

        g.setColour (c);
        g.fillRoundedRectangle (sx, (float) b.getY(), sw, (float) b.getHeight(), 1.5f);
    }

    if (showTarget)
    {
        const float x1 = xForDb (-18.f);
        const float x2 = xForDb (-12.f);
        const float ty = (float) b.getY();
        g.setColour (juce::Colour (0xffd6f5e0).withAlpha (0.85f));
        g.fillRect (x1, ty, x2 - x1, 1.6f);            // span across the target window
        g.fillRect (x1, ty, 1.4f, 4.5f);               // end ticks
        g.fillRect (x2 - 1.4f, ty, 1.4f, 4.5f);
    }
}

//==============================================================================
// paint
void CopilotToneAudioProcessorEditor::paint (juce::Graphics& g)
{
    using namespace juce;

    // Geometry comes from Layout (PluginEditor.h), shared with resized(). These
    // locals keep the names the rest of the function already uses.
    const Layout L = Layout::compute (getWidth(), getHeight());
    const int W    = L.W,    H    = L.H;
    const int hdrH = L.hdrH, barH = L.barH;
    const int kpY  = L.kpY,  kpH  = L.kpH;

    // body background
    g.setColour (CT::bg);
    g.fillAll();

    //==============================================================================
    //  HEADER
    {
        // background gradient
        ColourGradient hg (Colour (0xff111222), 0.f, 0.f,
                           Colour (0xff08090e), 0.f, (float) hdrH, false);
        g.setGradientFill (hg);
        g.fillRect (0, 0, W, hdrH);

        // top purple stripe
        g.setColour (CT::accent);
        g.fillRect (0, 0, W, 3);

        // left vertical accent bar
        ColourGradient lb (CT::accent.withAlpha (0.85f), 0.f, 3.f,
                           CT::accent.withAlpha (0.08f), 0.f, (float) hdrH, false);
        g.setGradientFill (lb);
        g.fillRect (0, 3, 3, hdrH - 3);

        // bottom separator
        rule (g, 0.f, (float)(hdrH - 1), (float) W, CT::lineW);

        // "AmpHead" - bold white
        g.setFont (Font (FontOptions (CT::fBrand, Font::bold)));
        g.setColour (CT::textHi);
        g.drawText ("AmpHead", 22, 14, 162, 34, Justification::centredLeft);

        // "Custom" - amber accent
        g.setFont (Font (FontOptions (CT::fBrand, Font::plain)));
        g.setColour (CT::amber);
        g.drawText ("Custom", 22 + 152, 14, 112, 34, Justification::centredLeft);

        // "by" - small plain italic lead-in
        g.setFont (Font (FontOptions (CT::fLabel, Font::italic)));
        g.setColour (CT::textLow);
        g.drawText ("by", 22 + 272, 27, 22, 16, Justification::centredLeft);

        // "JCConcepcion" - script signature (Snell Roundhand on macOS, default face
        // elsewhere), with a gold gradient and a flourish underline so it reads as
        // an autograph.
        {
            const float sigX  = 22.f + 294.f;   // just after "by"
            const float baseY = 38.f;           // text baseline
            Font sig (FontOptions ("Snell Roundhand", 29.f, Font::bold));

            GlyphArrangement ga;
            ga.addLineOfText (sig, "JCConcepcion", sigX, baseY);
            const auto bb = ga.getBoundingBox (0, -1, true);

            // soft drop shadow
            g.setColour (Colours::black.withAlpha (0.40f));
            ga.draw (g, AffineTransform::translation (1.2f, 1.6f));

            // warm gold gradient fill
            ColourGradient grad (CT::amber.brighter (0.35f), bb.getX(), bb.getY(),
                                 CT::amber.darker   (0.12f), bb.getX(), bb.getBottom(), false);
            g.setGradientFill (grad);
            ga.draw (g);

            // flourish: swoop under the name, then flick upward at the end
            const float lx = bb.getX(), rx = bb.getRight(), uy = bb.getBottom() - 1.f;
            Path fl;
            fl.startNewSubPath (lx + 1.f, uy - 1.f);
            fl.cubicTo (lx + (rx - lx) * 0.30f, uy + 3.5f,
                        lx + (rx - lx) * 0.68f, uy + 3.5f,
                        rx - 5.f, uy - 2.f);
            fl.quadraticTo (rx + 7.f, uy - 5.f, rx + 16.f, uy - 14.f);
            g.setColour (CT::amber.withAlpha (0.85f));
            g.strokePath (fl, PathStrokeType (1.7f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        // subtitle with bullet separator
        g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
        g.setColour (CT::textDim);
        // bullet char U+2022 in UTF-8: \xe2\x80\xa2
        const auto bullet = juce::String::fromUTF8 ("\xe2\x80\xa2");
        g.drawText (juce::String ("BOUTIQUE AMP HEAD  ") + bullet + juce::String ("  PROTOTYPE"),
                    22, 50, 300, 14, Justification::centredLeft);

        // gear icon (drawn, not interactive)
        {
            const float gx = (float)(W - 34), gy = 22.f, gr = 11.f;
            // outer ring
            g.setColour (CT::divider);
            g.drawEllipse (gx - gr, gy - gr, gr * 2.f, gr * 2.f, 1.5f);
            g.setColour (CT::textDim);
            g.drawEllipse (gx - gr, gy - gr, gr * 2.f, gr * 2.f, 1.f);
            // teeth: 8 small rectangles radiating outward
            for (int ti = 0; ti < 8; ++ti)
            {
                const float ta = ti * juce::MathConstants<float>::pi / 4.f;
                const float tx = gx + (gr + 2.f) * std::sin (ta);
                const float ty = gy - (gr + 2.f) * std::cos (ta);
                g.fillEllipse (tx - 1.8f, ty - 1.8f, 3.6f, 3.6f);
            }
            // inner hub
            g.setColour (CT::textDim);
            g.fillEllipse (gx - 4.f, gy - 4.f, 8.f, 8.f);
        }
    }

    // active channel underline glow
    {
        const int chan = (int) audioProcessor.apvts.getRawParameterValue ("channel")->load();
        const int btnW = 104, btnGap = 4, gearW = 32;
        const int btnH = 36, bY = (hdrH - btnH) / 2 - 6;
        const int btnAreaRight = W - 14 - gearW - 10;
        const int btnAreaLeft  = btnAreaRight - (3 * btnW + 2 * btnGap);
        const float ix = (float)(btnAreaLeft + chan * (btnW + btnGap));
        const float iw = (float) btnW;
        const float iy = (float)(bY + btnH + 1);

        ColourGradient glow (CT::accent.withAlpha (0.30f), ix + iw * 0.5f, iy,
                             CT::accent.withAlpha (0.f),   ix + iw * 0.5f, iy - 14.f, false);
        g.setGradientFill (glow);
        g.fillRect (ix, iy - 14.f, iw, 14.f);
        g.setColour (CT::accent);
        g.fillRoundedRectangle (ix, iy, iw, 2.5f, 1.5f);
    }

    //==============================================================================
    //  KNOB PANEL
    {
        // GRAPHIC EQ - right half of row 2. Same panel language as the knob row so
        // it reads as part of the amp and not as a bolted-on plugin feature. Dimmed
        // while the EQ is switched out, so the panel says whether it is in the path.
        {
            const Rectangle<float> ER ((float) L.eqPX, (float) L.r2Y,
                                       (float) L.eqPW, (float) L.r2H);
            const bool on = audioProcessor.apvts.getRawParameterValue ("eqOn")->load() > 0.5f;
            for (int i = 4; i >= 1; --i)
            {
                g.setColour (Colours::black.withAlpha (0.18f / i));
                g.fillRoundedRectangle (ER.expanded (i * 1.4f).translated (0.f, i * 1.4f), 10.f);
            }
            ColourGradient ef (Colour (0xff181a2a), ER.getX(), ER.getY(),
                               Colour (0xff0c0d14), ER.getX(), ER.getBottom(), false);
            g.setGradientFill (ef);
            g.fillRoundedRectangle (ER, 8.f);
            // rim when the EQ is in the path, rule when it is out: the panel edge
            // itself reports whether the section is doing anything.
            panelEdge (g, ER, 8.f, on ? CT::rim : CT::rule);

            g.setColour (on ? CT::accentHi : CT::textDim);
            g.setFont (Font (FontOptions (CT::fPanel, Font::bold)));
            g.drawText ("5-BAND GRAPHIC EQ", L.eqPX + 16, L.r2Y + 8, 240, 20,
                        Justification::centredLeft);
            g.setColour (CT::textLow);
            g.setFont (Font (FontOptions (CT::fMicro)));
            g.drawText ("MARK V-CURVE", L.eqPX + 180, L.r2Y + 12, 130, 14,
                        Justification::centredLeft);

            // Response curve behind the faders: the composite magnitude of the five
            // peaking biquads, on the same axis the faders use. Each band's centre
            // frequency is placed at its own fader and +/-12 dB spans the fader
            // travel, so the curve and the caps can never disagree.
            //
            // The five resonances are not evenly spaced in log f (87.6, 371.7,
            // 723.4, 1575.9, 4822.9), so a plain log axis would drift the peaks off
            // their faders. The mapping is piecewise-linear in log f through the
            // five anchors instead, which pins every peak to its own cap.
            //
            // Known limit: the bands are broad (Q = 0.8) and overlap, so their
            // gains add. Five faders at +12 dB sum to +22.5 dB at 750 Hz, well past
            // the +/-12 the travel represents. The curve saturates at 13 dB rather
            // than rescaling, because rescaling would move a single band's peak off
            // the cap that set it - and the caps are what the player is reading.
            {
                const float fadeTop  = (float) L.eqFadeY;
                const float fadeBot  = (float) (L.eqFadeY + L.eqFadeH - 15);
                const float midY     = (fadeTop + fadeBot) * 0.5f;
                const float halfSpan = (fadeBot - fadeTop) * 0.5f;

                const double sr = audioProcessor.getSampleRate() > 0.0
                                ? audioProcessor.getSampleRate() : 48000.0;

                static const char* eqIds[GraphicEQ::kBands] =
                    { "eq80", "eq240", "eq750", "eq2200", "eq6600" };

                // Coefficients do not depend on x, so they are built once here
                // rather than inside the pixel loop.
                float ax[GraphicEQ::kBands], lf[GraphicEQ::kBands];
                double B0[GraphicEQ::kBands], B1[GraphicEQ::kBands], B2[GraphicEQ::kBands],
                       A1[GraphicEQ::kBands], A2[GraphicEQ::kBands];
                bool   live[GraphicEQ::kBands];

                for (int i = 0; i < GraphicEQ::kBands; ++i)
                {
                    ax[i] = (float) (L.eqX0 + L.eqBandW * i + L.eqBandW / 2);
                    lf[i] = std::log10 (GraphicEQ::kFreq[i]);

                    const float dB = audioProcessor.apvts.getRawParameterValue (eqIds[i])->load();
                    live[i] = std::abs (dB) > 0.05f;
                    if (! live[i]) continue;

                    const double A  = std::pow (10.0, dB / 40.0);
                    const double w0 = 2.0 * MathConstants<double>::pi * GraphicEQ::kFreq[i] / sr;
                    const double al = std::sin (w0) / (2.0 * GraphicEQ::kQ);
                    const double a0 = 1.0 + al / A;
                    B0[i] = (1.0 + al * A)        / a0;
                    B1[i] = (-2.0 * std::cos (w0)) / a0;
                    B2[i] = (1.0 - al * A)        / a0;
                    A1[i] = B1[i];
                    A2[i] = (1.0 - al / A)        / a0;
                }

                const float xL = (float) L.eqX0;
                const float xR = (float) (L.eqX0 + L.eqBandW * 5);

                Path curve;
                bool started = false;
                for (float px = xL; px <= xR; px += 2.f)
                {
                    int seg = 0;
                    while (seg < GraphicEQ::kBands - 2 && px > ax[seg + 1]) ++seg;
                    const float t  = (px - ax[seg]) / (ax[seg + 1] - ax[seg]);
                    const float f  = std::pow (10.f, lf[seg] + t * (lf[seg + 1] - lf[seg]));

                    const double w  = 2.0 * MathConstants<double>::pi * f / sr;
                    const double c1 = std::cos (w),       s1 = std::sin (w);
                    const double c2 = std::cos (2.0 * w), s2 = std::sin (2.0 * w);

                    double total = 0.0;
                    for (int i = 0; i < GraphicEQ::kBands; ++i)
                    {
                        if (! live[i]) continue;
                        const double nr = B0[i] + B1[i] * c1 + B2[i] * c2;
                        const double ni = -(B1[i] * s1 + B2[i] * s2);
                        const double dr = 1.0    + A1[i] * c1 + A2[i] * c2;
                        const double di = -(A1[i] * s1 + A2[i] * s2);
                        total += 10.0 * std::log10 ((nr * nr + ni * ni)
                                                  / jmax (1.0e-12, dr * dr + di * di));
                    }

                    const float y = midY - (float) jlimit (-13.0, 13.0, total) / 12.f * halfSpan;
                    if (! started) { curve.startNewSubPath (px, y); started = true; }
                    else             curve.lineTo (px, y);
                }

                Path fill (curve);
                fill.lineTo (xR, midY);
                fill.lineTo (xL, midY);
                fill.closeSubPath();
                g.setColour (CT::accent.withAlpha (on ? 0.20f : 0.06f));
                g.fillPath (fill);
                g.setColour (on ? CT::accentHi.withAlpha (0.85f) : CT::textDim);
                g.strokePath (curve, PathStrokeType (1.6f));
            }

            // 0 dB reference line across the fader travel. Without a centre line,
            // flat is hard to find.
            const float mid = (float)(L.eqFadeY + (L.eqFadeH - 15) / 2);
            rule (g, (float) L.eqX0, mid, (float)(L.eqBandW * 5), CT::lineW);
        }

        const Rectangle<float> PR (14.f, (float) kpY, (float)(W - 28), (float) kpH);

        // shadow
        for (int i = 4; i >= 1; --i)
        {
            g.setColour (Colours::black.withAlpha (0.18f / i));
            g.fillRoundedRectangle (PR.expanded (i * 1.4f).translated (0.f, i * 1.4f), 10.f);
        }

        // panel face
        ColourGradient pf (Colour (0xff181a2a), PR.getX(), PR.getY(),
                           Colour (0xff0c0d14), PR.getX(), PR.getBottom(), false);
        g.setGradientFill (pf);
        g.fillRoundedRectangle (PR, 8.f);

        // One edge, not two. There were a near-black 1.5 px ring outside and a
        // 0.8 px ring inside doing the same job; the drop shadow above already
        // provides the depth those were reaching for.
        panelEdge (g, PR, 8.f);

        // chamfer highlight
        g.setColour (Colour (0xff2d3055).withAlpha (0.5f));
        g.fillRoundedRectangle (PR.reduced (1.5f, 1.5f).withHeight (2.f), 1.f);

         // Two vertical separators, one centred in each gap of the 2 | 4 | 2 grouping.
        // NOTE: gap and cw are duplicated from resized(). If you change one, you MUST
        // change the other or the lines will drift off the gaps.
        {
            const int gap = L.knobGap;
            const int cw  = L.knobW;
            const float sx[2] = { 22.f + cw * 2.f + gap * 0.5f,
                                  22.f + cw * 6.f + gap * 1.5f };

            // Group captions. The gap alone does not read as grouping - 115 px inside
            // a group against 159 px between them is only a 38% difference, which the
            // eye does not resolve as separation. The caption is what makes it obvious.
            {
                const char* caps  [3] = { "PREAMP", "TONE", "OUTPUT" };
                const int   startK[3] = { 0, 2, 6 };
                const int   sizeK [3] = { 2, 4, 2 };

                // textMid, one step above the textLow the knob labels use. A group
                // caption sharing its members' weight flattens the hierarchy instead
                // of building one.
                g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
                g.setColour (CT::textMid);
                for (int gi = 0; gi < 3; ++gi)
                {
                    const int gx = 22 + cw * startK[gi] + gap * gi;
                    const int gw = cw * sizeK[gi];
                    g.drawText (caps[gi], gx, (int) PR.getY() + 10, gw, 14,
                                Justification::centred);
                }
            }

            // A 3 px accent glow at 0.06 alpha used to sit behind each of these.
            // At that alpha it was not visible on this panel - it was code, not
            // design. One rule, inset 12 px like every other rule in the window.
            for (float x : sx)
                rule (g, x, PR.getY() + 12.f, CT::lineW, PR.getHeight() - 24.f);
        }

        // corner screws
        const float sm = 11.f, sr = 4.f;
        const Point<float> scr[] = {
            { PR.getX() + sm, PR.getY() + sm },
            { PR.getRight() - sm, PR.getY() + sm },
            { PR.getX() + sm, PR.getBottom() - sm },
            { PR.getRight() - sm, PR.getBottom() - sm },
        };
        for (auto& sc : scr)
        {
            g.setColour (Colours::black.withAlpha (0.5f));
            g.fillEllipse (sc.x - sr + 0.5f, sc.y - sr + 2.f, sr * 2.f, sr * 2.f);
            ColourGradient sg (Colour (0xff1d2030), sc.x - sr, sc.y - sr,
                               Colour (0xff060710), sc.x + sr, sc.y + sr, false);
            g.setGradientFill (sg);
            g.fillEllipse (sc.x - sr, sc.y - sr, sr * 2.f, sr * 2.f);
            g.setColour (Colour (0xff2b2e48).withAlpha (0.7f));
            g.drawEllipse (sc.x - sr + 0.5f, sc.y - sr + 0.5f, sr * 2.f - 1.f, sr * 2.f - 1.f, 0.8f);
            g.setColour (Colour (0xff040610));
            g.drawLine (sc.x - sr * 0.48f, sc.y, sc.x + sr * 0.48f, sc.y, 1.f);
            g.drawLine (sc.x, sc.y - sr * 0.48f, sc.x, sc.y + sr * 0.48f, 1.f);
        }
    }

    //==============================================================================
    //  ROW 2 LEFT: CABINET     ROW 3: GATE | FX
    {
        // One drawer for every sub-panel in both rows. It used to assume a single
        // footer row, so it took only x and width; now that panels live on two
        // rows it takes y and height too.
        const auto drawPanel = [&] (int px, int py, int pw, int ph)
        {
            Rectangle<float> r ((float) px, (float) py, (float) pw, (float) ph);
            g.setColour (Colours::black.withAlpha (0.30f));
            g.fillRoundedRectangle (r.translated (0.f, 2.f).expanded (1.f), 8.f);
            g.setColour (CT::bg2);
            g.fillRoundedRectangle (r, 6.f);
            panelEdge (g, r, 6.f);
        };

        // A panel title is not a control. This used to fill CT::inputBg, stroke
        // CT::divider and round the corners at 4 - the exact recipe drawComboBox
        // uses - so "CABINET", "NOISE GATE" and a 758 px box holding the word "FX"
        // all read as empty text fields you could click into. Bare text plus a rule
        // instead, and a step up the scale so a title outranks the labels under it.
        const auto drawHdr = [&] (int hx, int hy, int hw, const juce::String& txt)
        {
            g.setFont (Font (FontOptions (CT::fPanel, Font::bold)));
            g.setColour (CT::textMid);
            g.drawText (txt, hx + 14, hy + 8, hw - 28, 22, Justification::centredLeft);
            rule (g, (float)(hx + 14), (float)(hy + 32), (float)(hw - 28), CT::lineW);
        };

        drawPanel (L.cabX,  L.r2Y, L.cabW,  L.r2H);
        drawPanel (L.gateX, L.r3Y, L.gateW, L.r3H);
        drawPanel (L.fxX,   L.r3Y, L.fxW,   L.r3H);

        drawHdr (L.cabX,  L.r2Y, L.cabW,  "CABINET");
        drawHdr (L.gateX, L.r3Y, L.gateW, "NOISE GATE");
        drawHdr (L.fxX,   L.r3Y, L.fxW,        "FX");

        //--- CABINET -----------------------------------------------------------
        const int rTop = L.r2Y + 46;
        g.setFont (Font (FontOptions (CT::fLabel, Font::bold)));
        g.setColour (CT::textLow);
        g.drawText ("IR A", L.cabX + 14, rTop,      34, 22, Justification::centredLeft);
        g.drawText ("IR B", L.cabX + 14, rTop + 30, 34, 22, Justification::centredLeft);

        const int fnX = L.cabX + 52, fnW = L.cabW - 52 - 22;
        for (int row = 0; row < 2; ++row)
        {
            // Field vocabulary, not panel vocabulary: inputBg + divider at radius 4,
            // identical to drawComboBox, so everything you can click into looks the
            // same. It was radius 3 here and 4 there for no reason.
            Rectangle<float> fr ((float) fnX, (float)(rTop + row * 30), (float)(fnW - 26), 22.f);
            g.setColour (CT::inputBg);
            g.fillRoundedRectangle (fr, 4.f);
            g.setColour (CT::divider);
            g.drawRoundedRectangle (fr.reduced (0.5f), 4.f, CT::lineW);
        }

        const int slY = L.r2Y + L.r2H - 30;
        g.setFont (Font (FontOptions (CT::fLabel, Font::bold)));
        g.setColour (CT::textLow);
        g.drawText ("A", L.cabX + 12,           slY, 16, 18, Justification::centred);
        g.drawText ("B", L.cabX + L.cabW - 28,  slY, 16, 18, Justification::centred);

        //--- FX: three independent columns -------------------------------------
        // The dividers are what make REVERB, DELAY and MODULATION read as three
        // separate effects rather than nine knobs in a box.
        const int colW = (L.fxW - 24) / 3;
        for (int i = 1; i < 3; ++i)
        {
            const float dx = (float)(L.fxX + 12 + colW * i);
            rule (g, dx, (float)(L.r3Y + 42), CT::lineW, (float)(L.r3H - 54));
        }

        // MODULATION has no type dropdown, so its column would carry a hole where
        // the other two have a control. Fill it with the fact that explains why the
        // MOD switch does not silence the stage: the background ensemble is part of
        // the amp's voice and runs at all times.
        g.setFont (Font (FontOptions (CT::fMicro)));
        g.setColour (CT::textLow);
        // textLow, not textDim: textDim on this panel is 1.8:1, which is texture
        // rather than words - and this line exists purely to explain something.
        g.drawText ("ENSEMBLE ALWAYS ON",
                    L.fxX + 20 + colW * 2, L.r3Y + 62, colW - 16, 22,
                    Justification::centredLeft);

        // Post FX captions are Labels placed in resized(), not drawn here.
    }

    //==============================================================================
    //  BOTTOM BAR
    {
        const int bY = H - barH;
        // No rule along the top. The bar's fill is already two steps darker than the
        // body, and a fill change is a boundary; a hairline on top of it put two
        // faint lines 8 px apart - the panel edge above and this one - with neither
        // reading as the edge of anything.
        g.setColour (Colour (0xff07080d));
        g.fillRect (0, bY, W, barH);

        // labels
        g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
        g.setColour (CT::textLow);
        g.drawText ("INPUT", 14, bY + 2, 48, 16, Justification::centredLeft);

        // meters. Only the input meter gets the target bracket - the output meter
        // is about staying under the ceiling, not hitting a window.
        drawLevelMeter (g, { 60,      bY + 2, 170, 14 }, cachedInDb,  inClipHold  > 0, true);
        drawLevelMeter (g, { W - 248, bY + 2, 170, 14 }, cachedOutDb, outClipHold > 0, false);

        // Right of the output meter: the OUTPUT label, which becomes a CLIP badge
        // while the clip hold is active. It sits clear of the meter, which ends at
        // W-65.
        if (outClipHold > 0)
        {
            g.setColour (Colour (0xffef4444));
            g.fillRoundedRectangle ((float) (W - 58), (float) (bY + 3), 34.f, 12.f, 2.f);
            g.setColour (Colours::white);
            g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
            g.drawText ("CLIP", W - 60, bY + 1, 38, 16, Justification::centred);
        }
        else
        {
            g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
            g.setColour (CT::textLow);
            g.drawText ("OUTPUT", W - 72, bY + 2, 58, 16, Justification::centredLeft);
        }

        // Live gain-staging readout under the INPUT meter. Run too hot and the
        // modelled input stage leaves its headroom window, which is what makes a
        // sim sound compressed and fizzy. How much signal arrives depends entirely
        // on the guitar - passive single coils and active humbuckers are more than
        // 15 dB apart - so the text reports where the level is, not just the target.
        // The fix always lives at the audio interface, and the wording says so:
        // there is no input trim in the plugin.
        {
            String  msg;
            Colour  col;
            if      (inPeakHold < -50.f) { msg = "NO SIGNAL";                            col = CT::textDim;         }
            else if (inPeakHold < -24.f) { msg = "TOO LOW - raise interface gain";       col = CT::textLow;         }
            else if (inPeakHold < -18.f) { msg = "A BIT LOW";                            col = Colour (0xff15803d); }
            else if (inPeakHold < -12.f) { msg = "GOOD";                                 col = Colour (0xff22c55e); }
            else if (inPeakHold <  -6.f) { msg = "HOT";                                  col = Colour (0xffecba08); }
            else                         { msg = "TOO HOT - lower interface gain";       col = Colour (0xffef4444); }

            g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
            g.setColour (col);
            // Three stacked lines in 46 px: meter, then status, then target. They used
        // to be placed at +2 / +16 / +27, so the status and the target overlapped
        // by 5 px.
        g.drawText (msg, 60, bY + 20, 240, 14, Justification::centredLeft);

            // Target reminder, sitting right under the bracket drawn on the meter.
            g.setFont (Font (FontOptions (CT::fMicro)));
            g.setColour (CT::textDim);
            g.drawText ("TARGET -18 to -12 dB peak", 14, bY + 38, 210, 14, Justification::centredLeft);
        }

        // Only shown while output is actually clipping - points at the control
        // that fixes it rather than leaving a bare red light.
        if (outClipHold > 0)
        {
            g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
            g.setColour (Colour (0xffef4444));
            g.drawText ("lower OUTPUT", W - 248, bY + 18, 170, 16, Justification::centredRight);
        }

        // OVERSAMPLING / QUALITY labels
        g.setFont (Font (FontOptions (CT::fMicro, Font::bold)));
        g.setColour (CT::textDim);
        // Right-aligned to end just before each combo starts. They used to overlap
        // their own dropdowns: OVERSAMPLING showed as "NG" and QUALITY not at all.
        // bY + 7 with height 20 matches the combos' own rectangle, so label and box
        // share a centre line. At bY + 2 they floated 8 px above their own control.
        g.drawText ("OVERSAMPLING", W / 2 - 232, bY + 7, 108, 20, Justification::centredRight);
        g.drawText ("QUALITY",      W / 2 - 22,  bY + 7, 74,  20, Justification::centredRight);

        // version footer text
        g.setFont (Font (FontOptions (CT::fMicro)));
        g.setColour (CT::textDim);
        // Right-aligned rather than centred across the full width: centred, its
        // rectangle spanned the whole bar and overlapped the TARGET line by 11 px.
        // Only the justification was keeping the two strings' ink apart.
        g.drawText ("AmpHead Custom by JCConcepcion v0.1.0  -  PROTOTYPE",
                    W - 320, bY + barH - 16, 306, 12, Justification::centredRight);
    }

}

//==============================================================================
// resized
void CopilotToneAudioProcessorEditor::resized()
{
    const Layout L = Layout::compute (getWidth(), getHeight());
    const int W    = L.W,    H    = L.H;
    const int hdrH = L.hdrH, barH = L.barH;

    // header
    {
        // Channel buttons: three equal buttons on the right, with room reserved
        // for the gear icon.
        const int btnW   = 104;  // each button width
        const int btnH   = 36;   // button height
        const int btnGap = 4;    // gap between buttons
        const int gearW  = 32;   // gear icon area
        const int bY     = (hdrH - btnH) / 2 - 6;  // vertically centred in top half
        const int rightEdge = W - 14;
        // The gear icon is drawn in paint() at rightEdge - gearW.
        const int btnAreaRight = rightEdge - gearW - 10;
        const int btnAreaLeft  = btnAreaRight - (3 * btnW + 2 * btnGap);
        for (int i = 0; i < 3; ++i)
            chBtn[i].setBounds (btnAreaLeft + i * (btnW + btnGap), bY, btnW, btnH);

    }

    // knob row - three groups instead of eight equal columns.
    // PREAMP (gain, drive) | TONE (bass, mid, treble, presence) | OUTPUT (master, out)
    // The gap does the grouping; the divider lines drawn in paint() only confirm it.
    {
        // +20 at the top reserves the band where paint() writes the group captions.
        auto row = juce::Rectangle<int> (22, L.knobRowY, W - 44, L.knobRowH);
        const int gap = L.knobGap;
        const int cw  = L.knobW;
        const int lH  = 16;

        struct KP { juce::Slider& s; juce::Label& l; };
        KP kp[] = {
            { gainSlider, gainLabel }, { charSlider, charLabel },
            { bassSlider, bassLabel }, { midSlider,  midLabel  },
            { trebleSlider, trebleLabel }, { presSlider, presLabel },
            { masterSlider, masterLabel }, { outSlider, outLabel },
        };

        const int groupSize[3] = { 2, 4, 2 };
        int k = 0;
        for (int gi = 0; gi < 3; ++gi)
        {
            if (gi > 0) row.removeFromLeft (gap);   // discard: this is the gap
            for (int j = 0; j < groupSize[gi]; ++j, ++k)
            {
                auto col = row.removeFromLeft (cw);
                kp[k].l.setBounds (col.removeFromBottom (lH));
                kp[k].s.setBounds (col.reduced (10, 2));
            }
        }

        // Topology switches, under the group each one belongs to: the tone stack
        // position under TONE, the rectifier under OUTPUT. They are amp controls,
        // not EQ controls, which is where they used to sit.
        const int cbW = 96, cbH = 20;
        const int preampCX = 22 + cw;                    // centre of the PREAMP group
        const int toneCX   = 22 + cw * 4 + gap;          // centre of the TONE group
        const int outputCX = 22 + cw * 7 + gap * 2;      // centre of the OUTPUT group

        // BRIGHT belongs here rather than in the header: it is a preamp control,
        // and this row is now one switch per knob group - bright cap, tone stack
        // position, rectifier.
        brightLabel.setBounds (preampCX - cbW / 2, L.topoY,      cbW, 12);
        brightBtn  .setBounds (preampCX - cbW / 2, L.topoY + 13, cbW, cbH);

        stackPosLabel.setBounds (toneCX   - cbW / 2, L.topoY,      cbW, 12);
        stackPosBox  .setBounds (toneCX   - cbW / 2, L.topoY + 13, cbW, cbH);
        rectLabel    .setBounds (outputCX - cbW / 2, L.topoY,      cbW, 12);
        rectBox      .setBounds (outputCX - cbW / 2, L.topoY + 13, cbW, cbH);
    }


    // graphic EQ - right half of row 2
    {
        for (int i = 0; i < 5; ++i)
        {
            const int cx = L.eqX0 + i * L.eqBandW;
            eqLabel [i].setBounds (cx, L.r2Y + 34, L.eqBandW, 14);
            eqSlider[i].setBounds (cx + L.eqBandW / 2 - 26, L.eqFadeY, 52, L.eqFadeH);
        }
        eqOnBtn.setBounds (L.eqPX + L.eqPW - 70, L.r2Y + 8, 56, 24);
    }
    // CABINET - row 2, left
    {
        const int rTop = L.r2Y + 46;
        const int fnX  = L.cabX + 52, fnW = L.cabW - 52 - 22;
        irANameLabel.setBounds (fnX,             rTop,      fnW - 26, 22);
        irAFolderBtn.setBounds (fnX + fnW - 22,  rTop,      22, 22);
        irBNameLabel.setBounds (fnX,             rTop + 30, fnW - 26, 22);
        irBFolderBtn.setBounds (fnX + fnW - 22,  rTop + 30, 22, 22);

        // The one no-IR warning, directly under the two file rows.
        irHintLabel .setBounds (L.cabX + 14, rTop + 62, L.cabW - 28, 18);
        bypIRBtn    .setBounds (L.cabX + L.cabW - 90, L.r2Y + 8, 82, 24);
        irBlendSlider.setBounds (L.cabX + 32, L.r2Y + L.r2H - 29, L.cabW - 64, 18);
    }

    // NOISE GATE - row 3, left. Two knobs and a switch, nothing more: it earns
    // far less space than it used to take.
    {
        const int kW = (L.gateW - 26) / 2, kH = 86, lH = 14;
        const int kY = L.r3Y + 44;
        gateThreshSlider .setBounds (L.gateX + 9,       kY,           kW, kH);
        gateThreshLabel  .setBounds (L.gateX + 9,       kY + kH + 2,  kW, lH);
        gateReleaseSlider.setBounds (L.gateX + 17 + kW, kY,           kW, kH);
        gateReleaseLabel .setBounds (L.gateX + 17 + kW, kY + kH + 2,  kW, lH);
        gateOnBtn        .setBounds (L.gateX + L.gateW - 70, L.r3Y + 8, 56, 24);
    }

    // FX - row 3, right. Three columns side by side instead of three stacked
    // rows: same nine knobs, but each effect is now its own block.
    {
        const int colW = (L.fxW - 24) / 3;

        const auto placeCol = [&] (int ci, juce::Label& cap, juce::ComboBox* box,
                                   juce::TextButton& onBtn,
                                   juce::Slider& k0, juce::Label& l0,
                                   juce::Slider& k1, juce::Label& l1,
                                   juce::Slider& k2, juce::Label& l2)
        {
            const int cx    = L.fxX + 12 + colW * ci;
            const int inner = colW - 16;
            // Caption stops 8 px short of the switch beside it; the switch clears
            // the type box below it by 8. Every gap in this column is 8.
            cap.setBounds (cx + 8, L.r3Y + 36, inner - 64, 12);
            if (box != nullptr) box->setBounds (cx + 8, L.r3Y + 62, inner, 22);
            // 56 x 24, the same as every other bypass switch. At 36 x 18 these were
            // the smallest targets in the window while carrying the same decision.
            // r3Y + 30 keeps 4 px clear of the combo at r3Y + 58; at + 41 the two
            // shared a pixel row and the combo painted over the button's border.
            onBtn.setBounds (cx + 8 + inner - 56, L.r3Y + 30, 56, 24);

            const int kSz = 42, kY = L.r3Y + 92;
            juce::Slider* ks[3] = { &k0, &k1, &k2 };
            juce::Label*  ls[3] = { &l0, &l1, &l2 };
            for (int j = 0; j < 3; ++j)
            {
                const int sub = inner / 3;
                const int scx = cx + 8 + sub * j + sub / 2;
                ks[j]->setBounds (scx - kSz / 2,  kY,       kSz, kSz);
                ls[j]->setBounds (scx - sub / 2,  kY + kSz, sub, 11);
            }
        };

        placeCol (0, reverbLabel, &reverbTypeBox, revOnBtn,
                  reverbMixKnob,   reverbMixLabel,
                  reverbDecayKnob, reverbDecayLabel,
                  reverbToneKnob,  reverbToneLabel);
        placeCol (1, delayLabel, &delayTypeBox, dlyOnBtn,
                  delayMixKnob,  delayMixLabel,
                  delayTimeKnob, delayTimeLabel,
                  delayFbKnob,   delayFbLabel);
        placeCol (2, modLabel, nullptr, modOnBtn,
                  modDetuneKnob, modDetuneLabel,
                  modChorusKnob, modChorusLabel,
                  modRateKnob,   modRateLabel);
    }
    // bottom bar
    {
        const int bY = H - barH;
        oversamplingBox.setBounds (W / 2 - 118, bY + 7, 72, 20);
        qualityBox.setBounds      (W / 2 + 60,  bY + 7, 68, 20);
    }
}
