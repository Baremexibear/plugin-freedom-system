#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

/*
 * PluginScopeAudioProcessorEditor — Phase 4.1: WebView UI Integration
 *
 * Replaces the Phase 3.1 native UI (plugin list, buttons, status label)
 * with a full-window WebBrowserComponent serving v1-ui.html.
 *
 * The hosted plugin's native editor (if present) is overlaid over the
 * left panel area of the WebView as a native JUCE Component child.
 *
 * MEMBER ORDER (CRITICAL — DO NOT REORDER):
 *   Members are destroyed in REVERSE declaration order.
 *   Attachments call evaluateJavascript() during destruction, so they MUST
 *   be declared AFTER webView (destroyed before webView).
 *   Order: relays -> webView -> attachments
 *
 * PARAMETER SUMMARY (5 AudioParameterChoice — all WebComboBoxRelay):
 *   view_mode      -> viewModeRelay      / viewModeAttachment
 *   analysis_mode  -> analysisModeRelay  / analysisModeAttachment
 *   analysis_type  -> analysisTypeRelay  / analysisTypeAttachment
 *   test_signal    -> testSignalRelay    / testSignalAttachment
 *   comparison     -> comparisonRelay    / comparisonAttachment
 */

// Floating window that hosts the loaded plugin's native editor UI.
// Using a separate DocumentWindow avoids the WKWebView z-ordering issue:
// native WebKit views always paint on top of JUCE components, so any
// component overlaid on the WebView is invisible.  A separate NSWindow
// has its own z-order and shows correctly above everything.
class HostedEditorWindow : public juce::DocumentWindow
{
public:
    std::function<void()> onClose;

    HostedEditorWindow (const juce::String& name, juce::AudioProcessorEditor* editor)
        : juce::DocumentWindow (name,
                                juce::Colour (0xff1a1a1a),
                                juce::DocumentWindow::closeButton,
                                true /* addToDesktop */)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        // Size the window to the editor's preferred bounds
        setContentNonOwned (editor, true);
        centreWithSize (juce::jmax (editor->getWidth(),  400),
                        juce::jmax (editor->getHeight(), 300));
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (onClose) onClose();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostedEditorWindow)
};

class PluginScopeAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        public juce::ChangeListener,
                                        public juce::Timer
{
public:
    explicit PluginScopeAudioProcessorEditor (PluginScopeAudioProcessor&);
    ~PluginScopeAudioProcessorEditor() override;

    //==========================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    //==========================================================================
    // ChangeListener — called by scanBroadcaster when scan progress changes
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    //==========================================================================
    // Timer — polls C++ measurement results and pushes JSON to WebView JS
    void timerCallback() override;

    //==========================================================================
    // Called to push the current plugin list to the WebView JavaScript
    void pushPluginListToWebView();

private:
    //==========================================================================
    // Reference to processor (no ownership — must come before relays)
    PluginScopeAudioProcessor& processorRef;

    //==========================================================================
    // Hosted plugin native editor — shown in a separate floating window so it
    // is not obscured by the WKWebView native view (which always paints on top
    // of JUCE components in the same window).
    std::unique_ptr<juce::AudioProcessorEditor> hostedPluginEditor;
    std::unique_ptr<HostedEditorWindow>          hostedEditorWindow;

    // Plugin B editor window (A/B comparison mode)
    std::unique_ptr<juce::AudioProcessorEditor> hostedPluginEditorB;
    std::unique_ptr<HostedEditorWindow>          hostedEditorWindowB;

    // Plugin descriptions (populated after scan, used for loadPlugin calls)
    juce::Array<juce::PluginDescription> knownDescs;

    //==========================================================================
    // *** CRITICAL MEMBER DECLARATION ORDER — DO NOT REORDER ***
    //
    // Destruction happens in REVERSE declaration order:
    //   attachments destroyed first -> webView destroyed second -> relays destroyed last
    //
    // This prevents use-after-free: attachments call evaluateJavascript()
    // during destruction, so webView must still be alive at that point.

    // 1. RELAYS FIRST (no dependencies — safe to destroy last)
    std::unique_ptr<juce::WebComboBoxRelay> viewModeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> analysisModeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> analysisTypeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> testSignalRelay;
    std::unique_ptr<juce::WebComboBoxRelay> comparisonRelay;

    // 2. WEBVIEW SECOND (depends on relays via .withOptionsFrom())
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // 3. ATTACHMENTS LAST (depend on relays AND webView — destroyed first)
    std::unique_ptr<juce::WebComboBoxParameterAttachment> viewModeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> analysisModeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> analysisTypeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> testSignalAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> comparisonAttachment;

    //==========================================================================
    // Resource provider — serves BinaryData resources to the WebView by URL.
    // Returns std::nullopt for unknown paths (404).
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    //==========================================================================
    // Native event handler — called when JS fires:
    //   window.__JUCE__.backend.emitEvent('pluginscopeEvent', data)
    void handleNativeEvent (const juce::var& eventData);

    int timerTick = 0;   // incremented each timerCallback() call (~30 Hz)

    //==========================================================================
    // Hosted editor helpers (message thread only)
    void embedHostedEditor();
    void removeHostedEditor();
    void embedHostedEditorB();
    void removeHostedEditorB();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScopeAudioProcessorEditor)
};
