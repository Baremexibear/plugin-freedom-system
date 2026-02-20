#include "PluginEditor.h"

//==============================================================================
PluginScopeAudioProcessorEditor::PluginScopeAudioProcessorEditor (PluginScopeAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setSize (1100, 700);

    //--------------------------------------------------------------------------
    // Phase 3.1: Native hosted plugin panel (right area, fills remaining width)

    addAndMakeVisible (hostedPluginPanel);

    //--------------------------------------------------------------------------
    // Plugin list (left panel, upper area)

    pluginListModel = std::make_unique<PluginListModel> (*this);
    pluginListBox.setModel (pluginListModel.get());
    pluginListBox.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff111122));
    pluginListBox.setColour (juce::ListBox::outlineColourId,    juce::Colour (0xff333355));
    pluginListBox.setRowHeight (22);
    addAndMakeVisible (pluginListBox);

    //--------------------------------------------------------------------------
    // Load / Unload buttons

    loadButton.onClick = [this] { loadSelectedPlugin(); };
    loadButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff1e3a6e));
    loadButton.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
    addAndMakeVisible (loadButton);

    unloadButton.onClick = [this]
    {
        processorRef.unloadPlugin();
        removeHostedEditor();
        statusLabel.setText ("No plugin loaded", juce::dontSendNotification);
    };
    unloadButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff3a1e1e));
    unloadButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible (unloadButton);

    //--------------------------------------------------------------------------
    // Status label

    statusLabel.setText ("Scanning for plugins...", juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    //--------------------------------------------------------------------------
    // Listen for scan progress (message thread callback)

    scanListener = std::make_unique<ScanListener> (*this);
    processorRef.scanBroadcaster.addChangeListener (scanListener.get());

    // Populate from any already-cached results
    updatePluginList();
}

PluginScopeAudioProcessorEditor::~PluginScopeAudioProcessorEditor()
{
    // Stop listening for scan updates before any members are destroyed
    processorRef.scanBroadcaster.removeChangeListener (scanListener.get());

    // Remove hosted editor from its parent before unique_ptr destructs it
    removeHostedEditor();

    // Destruction order is guaranteed by declaration order in the header:
    //   attachments -> webView -> relays  (reverse of declaration)
    // No manual reset() calls required.
}

//==============================================================================
void PluginScopeAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Dark background consistent with the v1 mockup aesthetic
    g.fillAll (juce::Colour (0xff0d0d1a));

    // Subtle accent line at the top
    g.setColour (juce::Colour (0xff4a90d9));
    g.fillRect (0, 0, getWidth(), 2);

    // Divider line between left panel and hosted editor area
    g.setColour (juce::Colour (0xff333355));
    g.fillRect (280, 0, 1, getHeight());
}

//==============================================================================
void PluginScopeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // Left panel: 280px wide — plugin list + controls
    auto leftPanel = area.removeFromLeft (280);

    // Status label at the top
    statusLabel.setBounds (leftPanel.removeFromTop (28).reduced (4, 4));

    // Buttons at the bottom
    unloadButton.setBounds (leftPanel.removeFromBottom (36).reduced (4, 4));
    loadButton  .setBounds (leftPanel.removeFromBottom (36).reduced (4, 4));

    // Plugin list fills remaining left panel area
    pluginListBox.setBounds (leftPanel.reduced (4, 4));

    // Right side: hosted plugin editor panel (fills remaining area after divider)
    // Skip the 1-pixel divider drawn in paint()
    area.removeFromLeft (1);
    hostedPluginPanel.setBounds (area);

    // If a hosted editor is embedded, resize it to fill the panel
    if (hostedPluginEditor != nullptr)
        hostedPluginEditor->setBounds (hostedPluginPanel.getLocalBounds());
}

//==============================================================================
void PluginScopeAudioProcessorEditor::updatePluginList()
{
    // Must be called on the message thread (ScanListener ensures this).
    //
    // THREADING: KnownPluginList is NOT thread-safe. The scan thread writes to
    // it via PluginDirectoryScanner::scanNextFile() while intermediate callAsync
    // messages arrive here. Reading getTypes() concurrently with scanNextFile()
    // is a data race that causes crashes.
    //
    // FIX: During scanning, only update the progress label — never read the list.
    // Once scanComplete is true, the scan thread has stopped writing and all
    // writes are visible (seq_cst store/load fence), so getTypes() is safe.

    if (!processorRef.scanComplete.load())
    {
        // Scan in progress — just show progress count, do NOT read the list
        statusLabel.setText ("Scanning... " + juce::String (processorRef.scanScannedCount.load()) + " found",
                             juce::dontSendNotification);
        return;
    }

    // Scan complete — safe to read the full list exactly once
    pluginListItems.clear();
    knownDescs.clear();

    auto plugins = processorRef.getKnownPlugins();
    for (auto& desc : plugins)
    {
        pluginListItems.add (desc.name + " [" + desc.pluginFormatName + "]");
        knownDescs.add (desc);
    }

    statusLabel.setText (juce::String (plugins.size()) + " plugins found",
                         juce::dontSendNotification);

    pluginListBox.updateContent();
    pluginListBox.repaint();
}

//==============================================================================
void PluginScopeAudioProcessorEditor::loadSelectedPlugin()
{
    const int selectedRow = pluginListBox.getSelectedRow();

    if (selectedRow < 0 || selectedRow >= knownDescs.size())
    {
        statusLabel.setText ("Select a plugin first", juce::dontSendNotification);
        return;
    }

    const auto desc = knownDescs[selectedRow];
    statusLabel.setText ("Loading " + desc.name + "...", juce::dontSendNotification);

    // Remove any currently embedded editor before loading the new plugin
    removeHostedEditor();

    processorRef.loadPlugin (
        desc,
        [this, name = desc.name] (bool success, const juce::String& error)
        {
            // Callback fires on message thread
            if (success)
            {
                statusLabel.setText (name + " loaded", juce::dontSendNotification);
                embedHostedEditor();
            }
            else
            {
                statusLabel.setText ("Failed: " + error, juce::dontSendNotification);
            }
        });
}

//==============================================================================
void PluginScopeAudioProcessorEditor::embedHostedEditor()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    removeHostedEditor();

    auto* plugin = processorRef.getHostedPlugin();
    if (plugin == nullptr)
        return;

    if (plugin->hasEditor())
    {
        hostedPluginEditor.reset (plugin->createEditor());
        if (hostedPluginEditor != nullptr)
        {
            hostedPluginPanel.addAndMakeVisible (*hostedPluginEditor);
            hostedPluginEditor->setBounds (hostedPluginPanel.getLocalBounds());
        }
    }
}

void PluginScopeAudioProcessorEditor::removeHostedEditor()
{
    if (hostedPluginEditor != nullptr)
    {
        hostedPluginPanel.removeChildComponent (hostedPluginEditor.get());
        hostedPluginEditor.reset();
    }
}
