#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

/*
 * PluginScopeAudioProcessorEditor — Stage 1 Shell
 *
 * This is the minimal Stage 1 editor. WebView wiring is NOT done here.
 * gui-agent will replace the placeholder label with a full WebBrowserComponent
 * during Stage 3 (GUI integration).
 *
 * MEMBER ORDER (CRITICAL — DO NOT REORDER):
 *   Members are destroyed in REVERSE declaration order.
 *   Order: relays -> webView -> attachments
 *   Even as nullptr stubs the correct order is maintained so gui-agent
 *   can fill them in without restructuring the class.
 *
 * PARAMETER SUMMARY (5 AudioParameterChoice — all WebComboBoxRelay):
 *   view_mode      -> viewModeRelay      / viewModeAttachment
 *   analysis_mode  -> analysisModeRelay  / analysisModeAttachment
 *   analysis_type  -> analysisTypeRelay  / analysisTypeAttachment
 *   test_signal    -> testSignalRelay    / testSignalAttachment
 *   comparison     -> comparisonRelay    / comparisonAttachment
 */

class PluginScopeAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit PluginScopeAudioProcessorEditor (PluginScopeAudioProcessor&);
    ~PluginScopeAudioProcessorEditor() override;

    //==========================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    //==========================================================================
    // Reference to processor (no ownership)
    PluginScopeAudioProcessor& processorRef;

    //==========================================================================
    // Placeholder label — removed by gui-agent when WebView is integrated
    juce::Label placeholderLabel;

    //==========================================================================
    // *** CRITICAL MEMBER DECLARATION ORDER — DO NOT REORDER ***
    //
    // Destruction happens in REVERSE declaration order:
    //   attachments destroyed first -> webView destroyed second -> relays destroyed last
    //
    // This prevents use-after-free: attachments call evaluateJavascript()
    // during destruction, so webView must still be alive at that point.

    // 1. RELAYS FIRST (no dependencies — safe to destroy last)
    //    Stub nullptr — gui-agent initialises these in Stage 3 (GUI)
    std::unique_ptr<juce::WebComboBoxRelay> viewModeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> analysisModeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> analysisTypeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> testSignalRelay;
    std::unique_ptr<juce::WebComboBoxRelay> comparisonRelay;

    // 2. WEBVIEW SECOND (depends on relays via .withOptionsFrom())
    //    Stub nullptr — gui-agent initialises this in Stage 3 (GUI)
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // 3. ATTACHMENTS LAST (depend on relays AND webView — destroyed first)
    //    Stub nullptr — gui-agent initialises these in Stage 3 (GUI)
    std::unique_ptr<juce::WebComboBoxParameterAttachment> viewModeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> analysisModeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> analysisTypeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> testSignalAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> comparisonAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScopeAudioProcessorEditor)
};
