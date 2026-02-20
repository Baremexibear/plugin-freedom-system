#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Phase 3.1: Background Plugin Scanner Thread
//
// Scans VST3 and AU directories on a low-priority background thread.
// Uses dead man's pedal to skip plugins that crashed on previous scans.
// Results are persisted to XML after scan completes.
//==============================================================================

class PluginScanThread : public juce::Thread
{
public:
    explicit PluginScanThread (PluginScopeAudioProcessor& owner)
        : juce::Thread ("PluginScanner"), ownerProcessor (owner)
    {
    }

    void run() override
    {
        // Ensure data directory exists before writing dead man's pedal / cache
        auto pedalFile = ownerProcessor.getDeadMansPedalFile();
        pedalFile.getParentDirectory().createDirectory();

        // SELF-EXCLUSION: Write our own bundle path to the dead man's pedal BEFORE
        // creating any PluginDirectoryScanner. The scanner reads the pedal file in its
        // constructor and skips listed paths. Without this, the scanner loads
        // PluginScope.vst3 inside PluginScope, creating a recursive instantiation crash.
        //
        // On macOS the executable lives at:
        //   .../PluginScope.vst3/Contents/MacOS/PluginScope   (3 dirs up = .vst3 bundle)
        //   .../PluginScope.component/Contents/MacOS/PluginScope
        auto selfExe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        auto selfBundle = selfExe.getParentDirectory()   // MacOS/
                                 .getParentDirectory()   // Contents/
                                 .getParentDirectory();  // PluginScope.vst3  (or .component)
        if (selfBundle.exists())
            pedalFile.replaceWithText (selfBundle.getFullPathName());

        // Iterate over every registered format (VST3 + AU on macOS)
        for (int formatIdx = 0; formatIdx < ownerProcessor.formatManager.getNumFormats(); ++formatIdx)
        {
            if (threadShouldExit())
                break;

            auto* format = ownerProcessor.formatManager.getFormat (formatIdx);

            // Build search paths per format
            juce::FileSearchPath paths;

            if (format->getName().containsIgnoreCase ("VST3"))
            {
                paths.add (juce::File ("/Library/Audio/Plug-Ins/VST3"));
                paths.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                               .getChildFile ("Library/Audio/Plug-Ins/VST3"));
            }
            else
            {
                // AU and other formats: use JUCE defaults
                paths = format->getDefaultLocationsToSearch();
            }

            // Scanner reads pedal file in constructor — our own bundle is already listed,
            // so the scanner will skip PluginScope.vst3/.component automatically.
            juce::PluginDirectoryScanner scanner (
                ownerProcessor.knownPluginList,
                *format,
                paths,
                true,   // dontRescanIfAlreadyInList
                pedalFile);

            juce::String pluginBeingScanned;
            while (!threadShouldExit() && scanner.scanNextFile (true, pluginBeingScanned))
            {
                ownerProcessor.scanScannedCount.fetch_add (1);

                // Post progress to message thread.
                // Capture processorAlive by value so the lambda can bail out safely
                // if the processor is destroyed before this message is dispatched.
                juce::MessageManager::callAsync ([alive = ownerProcessor.processorAlive,
                                                  &ownerRef = ownerProcessor]()
                {
                    if (alive->load())
                        ownerRef.scanBroadcaster.sendChangeMessage();
                });
            }
        }

        // Clear dead man's pedal now that scan completed without crash.
        // Next launch will re-populate it before the next scan.
        pedalFile.replaceWithText ({});

        // Persist scan results to disk so subsequent launches skip the full scan
        {
            std::unique_ptr<juce::XmlElement> xml (ownerProcessor.knownPluginList.createXml());
            if (xml != nullptr)
            {
                auto cacheFile = ownerProcessor.getScanCacheFile();
                cacheFile.getParentDirectory().createDirectory();
                xml->writeTo (cacheFile);
            }
        }

        // Signal completion and notify UI
        ownerProcessor.scanComplete.store (true);

        juce::MessageManager::callAsync ([alive = ownerProcessor.processorAlive,
                                          &ownerRef = ownerProcessor]()
        {
            if (alive->load())
                ownerRef.scanBroadcaster.sendChangeMessage();
        });
    }

private:
    PluginScopeAudioProcessor& ownerProcessor;
};

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout PluginScopeAudioProcessor::createParameterLayout()
{
    // All 5 parameters are AudioParameterChoice (snake_case IDs — IMMUTABLE CONTRACT)

    juce::StringArray viewModeChoices     { "Simplified", "Expert" };
    juce::StringArray analysisModeChoices { "Live", "Snapshot" };
    juce::StringArray analysisTypeChoices { "Frequency Response", "Dynamics Curve",
                                            "Harmonic Distortion", "Phase Response",
                                            "Latency" };
    juce::StringArray testSignalChoices   { "Sine Sweep", "White Noise", "Pink Noise",
                                            "Impulse", "Live Audio" };
    juce::StringArray comparisonChoices   { "Off", "A/B Plugins", "Before-After" };

    return {
        // view_mode — Simplified / Expert UI toggle
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "view_mode", 1 },
            "View Mode",
            viewModeChoices,
            0   // Default: Simplified
        ),

        // analysis_mode — Live analysis vs. snapshot capture
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "analysis_mode", 1 },
            "Analysis Mode",
            analysisModeChoices,
            0   // Default: Live
        ),

        // analysis_type — Which measurement is displayed
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "analysis_type", 1 },
            "Analysis Type",
            analysisTypeChoices,
            0   // Default: Frequency Response
        ),

        // test_signal — Signal source for measurement
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "test_signal", 1 },
            "Test Signal",
            testSignalChoices,
            0   // Default: Sine Sweep
        ),

        // comparison — Comparison mode (off / A-B / before-after)
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "comparison", 1 },
            "Comparison",
            comparisonChoices,
            0   // Default: Off
        ),
    };
}

//==============================================================================
PluginScopeAudioProcessor::PluginScopeAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , parameters (*this, nullptr, "Parameters", createParameterLayout())
{
    // Register VST3 and AU formats (AudioUnitPluginFormat included on macOS)
    // JUCE 8: addDefaultFormats() is deleted — use free function instead
    juce::addDefaultFormatsToManager (formatManager);

    // Load persisted scan cache if it exists (avoids full rescan on every launch)
    auto cacheFile = getScanCacheFile();
    if (cacheFile.existsAsFile())
    {
        auto xml = juce::parseXML (cacheFile);
        if (xml != nullptr)
            knownPluginList.recreateFromXml (*xml);
    }

    // Start background scan to pick up any new/changed plugins since last cache
    startPluginScan();
}

PluginScopeAudioProcessor::~PluginScopeAudioProcessor()
{
    // Invalidate all pending callAsync lambdas FIRST — they hold a shared_ptr
    // copy of processorAlive and will bail out before touching *this.
    processorAlive->store (false);

    // Signal audio thread to stop using hosted plugin BEFORE we destroy it
    pluginReady.store (false);

    // Stop scanner thread cleanly (wait up to 3 seconds)
    if (scanThread != nullptr)
        scanThread->stopThread (3000);

    // Release hosted plugin on message thread
    hostedPlugin.reset();
}

//==============================================================================
void PluginScopeAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // Cache atomic parameter pointers for real-time-safe access in processBlock
    viewModeParam     = parameters.getRawParameterValue ("view_mode");
    analysisModeParam = parameters.getRawParameterValue ("analysis_mode");
    analysisTypeParam = parameters.getRawParameterValue ("analysis_type");
    testSignalParam   = parameters.getRawParameterValue ("test_signal");
    comparisonParam   = parameters.getRawParameterValue ("comparison");

    // Pre-allocate scratch buffers — NEVER allocate inside processBlock
    hostedInputBuffer .setSize (2, samplesPerBlock);
    hostedOutputBuffer.setSize (2, samplesPerBlock);

    // Re-prepare hosted plugin if one is already loaded
    if (hostedPlugin != nullptr && pluginReady.load())
        hostedPlugin->prepareToPlay (sampleRate, samplesPerBlock);
}

void PluginScopeAudioProcessor::releaseResources()
{
    // Release scratch buffers to save memory when plugin is not in use
    hostedInputBuffer .setSize (0, 0);
    hostedOutputBuffer.setSize (0, 0);
}

//==============================================================================
void PluginScopeAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    // Clear any unused output channels
    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // If a hosted plugin is ready, route audio through it (pass-through mode).
    // In Phase 3.1 this serves as architectural validation.
    // Measurement engines (Phases 3.2+) will inject test signals instead of
    // the live DAW audio in most modes.
    if (pluginReady.load() && hostedPlugin != nullptr)
    {
        const int numSamples   = buffer.getNumSamples();
        const int numChannels  = juce::jmin (buffer.getNumChannels(), 2);

        // Copy DAW input into hosted plugin's pre-allocated input buffer
        for (int ch = 0; ch < numChannels; ++ch)
            hostedInputBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        // Clear hosted output buffer before processing
        hostedOutputBuffer.clear();

        // Run hosted plugin on the audio thread — real-time safe
        juce::MidiBuffer emptyMidi;
        try
        {
            hostedPlugin->processBlock (hostedOutputBuffer, emptyMidi);
        }
        catch (...)
        {
            // Hosted plugin crashed — disable it safely and let audio pass-through
            pluginReady.store (false);
        }

        // Copy hosted output back to the DAW buffer (if plugin is still ready)
        if (pluginReady.load())
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom (ch, 0, hostedOutputBuffer, ch, 0, numSamples);
        }
    }

    // If no hosted plugin: pass-through DAW audio unchanged (neutral behaviour)
    // PluginScope does NOT modify the audio stream when acting as an analyser.
}

//==============================================================================
void PluginScopeAudioProcessor::loadPlugin (
    const juce::PluginDescription& desc,
    std::function<void(bool success, const juce::String& error)> callback)
{
    // Must be called from the message thread
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    // Tell the audio thread to stop using the current hosted plugin
    pluginReady.store (false);

    // Brief yield to allow the audio thread to observe the flag change
    // (one audio callback cycle at typical buffer sizes is well under 10ms)
    juce::Thread::sleep (10);

    // Release the old plugin instance (destructor called on message thread)
    hostedPlugin.reset();

    // Load the new plugin asynchronously — required for AUv3 plugins
    formatManager.createPluginInstanceAsync (
        desc,
        currentSampleRate,
        currentBlockSize,
        [this, callback] (std::unique_ptr<juce::AudioPluginInstance> instance,
                          const juce::String& error)
        {
            // This callback fires on the message thread
            if (instance != nullptr)
            {
                instance->prepareToPlay (currentSampleRate, currentBlockSize);
                hostedPlugin = std::move (instance);
                pluginReady.store (true);

                if (callback)
                    callback (true, {});
            }
            else
            {
                if (callback)
                    callback (false, error);
            }
        });
}

void PluginScopeAudioProcessor::unloadPlugin()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    pluginReady.store (false);
    juce::Thread::sleep (10);
    hostedPlugin.reset();
}

//==============================================================================
juce::Array<juce::PluginDescription> PluginScopeAudioProcessor::getKnownPlugins() const
{
    // JUCE 8: getType(int) is deprecated — use getTypes() which returns Array<PluginDescription>
    // Safety filter: exclude ourselves. Loading PluginScope inside PluginScope causes
    // recursive instantiation. The dead man's pedal normally prevents this, but filter
    // here as a second layer of defence.
    juce::Array<juce::PluginDescription> result;
    for (auto& desc : knownPluginList.getTypes())
        if (! desc.name.equalsIgnoreCase ("PluginScope"))
            result.add (desc);
    return result;
}

//==============================================================================
juce::File PluginScopeAudioProcessor::getScanCacheFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("PluginScope")
               .getChildFile ("plugin_scan_cache.xml");
}

juce::File PluginScopeAudioProcessor::getDeadMansPedalFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("PluginScope")
               .getChildFile ("dead_mans_pedal.txt");
}

//==============================================================================
void PluginScopeAudioProcessor::startPluginScan()
{
    if (scanThread == nullptr || !scanThread->isThreadRunning())
    {
        scanThread = std::make_unique<PluginScanThread> (*this);
        scanThread->startThread (juce::Thread::Priority::low);
    }
}

//==============================================================================
juce::AudioProcessorEditor* PluginScopeAudioProcessor::createEditor()
{
    return new PluginScopeAudioProcessorEditor (*this);
}

//==============================================================================
void PluginScopeAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void PluginScopeAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginScopeAudioProcessor();
}
