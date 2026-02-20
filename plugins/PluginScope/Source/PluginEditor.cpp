#include "PluginEditor.h"

//==============================================================================
PluginScopeAudioProcessorEditor::PluginScopeAudioProcessorEditor (PluginScopeAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    // Stage 1 placeholder — WebView integration happens in Stage 3 (GUI)
    //
    // All relay/webView/attachment unique_ptrs are intentionally nullptr here.
    // gui-agent will initialise them following the required order:
    //   1. Create relays
    //   2. Create webView (using .withOptionsFrom(relay) for each relay)
    //   3. Create attachments (passing relay + parameter to each)

    placeholderLabel.setText ("PluginScope - Stage 1\n5 parameters implemented",
                              juce::dontSendNotification);
    placeholderLabel.setJustificationType (juce::Justification::centred);
    placeholderLabel.setFont (juce::Font (20.0f));
    placeholderLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (placeholderLabel);

    // UI size matches the finalized v1 mockup (1100 x 700)
    setSize (1100, 700);
}

PluginScopeAudioProcessorEditor::~PluginScopeAudioProcessorEditor()
{
    // Destruction order is guaranteed by declaration order in the header:
    //   attachments -> webView -> relays  (reverse of declaration)
    // No manual reset() calls required.
}

//==============================================================================
void PluginScopeAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Dark background consistent with the v1 mockup aesthetic
    g.fillAll (juce::Colour (0xff1a1a2e));

    // Subtle accent line at the top
    g.setColour (juce::Colour (0xff4a90d9));
    g.fillRect (0, 0, getWidth(), 2);
}

//==============================================================================
void PluginScopeAudioProcessorEditor::resized()
{
    placeholderLabel.setBounds (getLocalBounds());
}
