#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

/*
 * PluginScopeAudioProcessorEditor — Phase 3.1: Plugin Hosting Foundation
 *
 * Left panel: native JUCE Component showing the plugin list and hosting controls.
 *   - ListBox of all discovered plugins (from KnownPluginList)
 *   - Load / Unload buttons
 *   - Status label (scan progress, load errors)
 *   - Embedded hosted plugin editor (plugin's own UI shown in the right panel)
 *
 * Right panel: hosted plugin editor embed area (native JUCE Component).
 *   The hosted plugin's editor is a juce::AudioProcessorEditor parented here,
 *   NOT rendered inside WebView.  WebView wiring is NOT done in Phase 3.1.
 *
 * WebView stubs:
 *   All relay/webView/attachment unique_ptrs remain nullptr in Phase 3.1.
 *   gui-agent will initialise them in Stage 3 (GUI integration), following the
 *   required declaration order: relays -> webView -> attachments.
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

    //==========================================================================
    // Called by ScanListener when scan progress changes (message thread)
    void updatePluginList();

private:
    //==========================================================================
    // Reference to processor (no ownership)
    PluginScopeAudioProcessor& processorRef;

    //==========================================================================
    // Phase 3.1: Native left panel — plugin list and hosting controls
    //
    // These are all plain JUCE Components, NOT WebView.
    // The right half of the window is used for the hosted plugin's own editor.

    // Container component for the right panel (hosted plugin editor parent)
    juce::Component hostedPluginPanel;

    // Currently displayed hosted plugin editor (owned here)
    std::unique_ptr<juce::AudioProcessorEditor> hostedPluginEditor;

    // Plugin list
    juce::ListBox   pluginListBox;
    juce::StringArray pluginListItems;

    // Stored plugin descriptions (parallel array to pluginListItems for index lookup)
    juce::Array<juce::PluginDescription> knownDescs;

    // Buttons
    juce::TextButton scanButton   { "Scan Plugins"   };
    juce::TextButton loadButton   { "Load Selected"  };
    juce::TextButton unloadButton { "Unload Plugin"  };

    // Status label (scan progress, load errors)
    juce::Label statusLabel;

    //==========================================================================
    // Change listener for scan progress — inner class

    class ScanListener : public juce::ChangeListener
    {
    public:
        explicit ScanListener (PluginScopeAudioProcessorEditor& owner) : ownerEditor (owner) {}
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            ownerEditor.updatePluginList();
        }
    private:
        PluginScopeAudioProcessorEditor& ownerEditor;
    };

    std::unique_ptr<ScanListener> scanListener;

    //==========================================================================
    // ListBoxModel for the plugin list — inner class

    class PluginListModel : public juce::ListBoxModel
    {
    public:
        explicit PluginListModel (PluginScopeAudioProcessorEditor& owner) : ownerEditor (owner) {}

        int getNumRows() override
        {
            return ownerEditor.pluginListItems.size();
        }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                               bool isRowSelected) override
        {
            if (isRowSelected)
                g.fillAll (juce::Colour (0xff1e3a6e));

            g.setColour (juce::Colours::white);
            g.setFont (13.0f);

            if (row < ownerEditor.pluginListItems.size())
                g.drawText (ownerEditor.pluginListItems[row], 6, 0, width - 12, height,
                            juce::Justification::centredLeft);
        }

    private:
        PluginScopeAudioProcessorEditor& ownerEditor;
    };

    std::unique_ptr<PluginListModel> pluginListModel;

    //==========================================================================
    // Phase 3.1 helper methods

    void loadSelectedPlugin();
    void embedHostedEditor();
    void removeHostedEditor();

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
