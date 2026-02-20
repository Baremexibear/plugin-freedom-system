#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Phase 3.1: Safe Metadata-Only Plugin Scanner
//
// PROBLEM WITH JUCE'S BUILT-IN SCANNER:
//   juce::PluginDirectoryScanner + VST3PluginFormatHeadless loads each
//   plugin's binary DLL in-process on a background thread to extract
//   descriptions. When PluginScope is hosted in a DAW alongside other
//   plugins (e.g. ScalerAudio2), this DLL loading races with those plugins'
//   message-thread initialization, corrupting shared JUCE internals and
//   crashing the host — even though PluginScope's own code is fine.
//
// SOLUTION:
//   Read only static metadata files shipped inside each plugin bundle.
//   No binary is loaded. No DLL is touched. Safe on any background thread.
//
//   VST3 (3.7+): Contents/moduleinfo.json — Steinberg standard
//   AU  (all):   Contents/Info.plist      — macOS bundle standard
//
//   Plugins that pre-date VST3 3.7 (no moduleinfo.json) are skipped.
//   Binary plist Info.plist files (unusual for distributed plugins) are also
//   skipped — most plugins ship XML plists.
//==============================================================================

namespace SafeScanner
{
    //--------------------------------------------------------------------------
    // 4-char OSType helpers — mirrors JUCE's internal osTypeToString()

    static juce::String osTypeToString (uint32_t t)
    {
        char s[5] = { (char) ((t >> 24) & 0xff), (char) ((t >> 16) & 0xff),
                      (char) ((t >>  8) & 0xff), (char)  (t & 0xff), 0 };
        return juce::String::fromUTF8 (s, 4);
    }

    // Build the JUCE AU fileOrIdentifier ("AudioUnit:Effects/aufx,XXXX,YYYY")
    // matching AudioUnitFormatHelpers::createPluginIdentifier() in JUCE source.
    static juce::String makeAUIdentifier (const juce::String& typeStr,
                                          const juce::String& subStr,
                                          const juce::String& mfrStr)
    {
        juce::String category;
        if      (typeStr == "aufx" || typeStr == "aumf") category = "Effects/";
        else if (typeStr == "aumu")                       category = "Synths/";
        else if (typeStr == "augn")                       category = "Generators/";
        else if (typeStr == "aupn")                       category = "Panners/";
        else if (typeStr == "aumx")                       category = "Mixers/";
        else if (typeStr == "aump")                       category = "MidiEffects/";

        return "AudioUnit:" + category + typeStr + "," + subStr + "," + mfrStr;
    }

    //--------------------------------------------------------------------------
    // Notify scan progress — helper shared by both format scanners

    static void postProgress (const std::shared_ptr<std::atomic<bool>>& alive,
                               PluginScopeAudioProcessor& owner)
    {
        owner.scanScannedCount.fetch_add (1);
        juce::MessageManager::callAsync ([alive, &ownerRef = owner]()
        {
            if (alive->load())
                ownerRef.scanBroadcaster.sendChangeMessage();
        });
    }

    //--------------------------------------------------------------------------
    // Scan a single VST3 bundle by reading Contents/moduleinfo.json.
    // Returns true if at least one class was found.

    static bool scanVST3Bundle (const juce::File& bundle,
                                 const juce::String& selfStem,
                                 juce::KnownPluginList& list)
    {
        if (bundle.getFileNameWithoutExtension().equalsIgnoreCase (selfStem))
            return false;

        auto moduleInfo = bundle.getChildFile ("Contents/moduleinfo.json");
        if (! moduleInfo.existsAsFile())
            return false;

        auto jsonVar = juce::JSON::parse (moduleInfo);
        if (! jsonVar.isObject())
            return false;

        auto factoryInfo = jsonVar["Factory Info"];
        juce::String vendor;
        if (factoryInfo.isObject())
            vendor = factoryInfo["Vendor"].toString();

        juce::String version = jsonVar["Version"].toString();

        auto classes = jsonVar["Classes"];
        if (! classes.isArray())
            return false;

        bool found = false;
        for (const auto& cls : *classes.getArray())
        {
            if (! cls.isObject()) continue;
            if (cls["Category"].toString() != "Audio Module Class") continue;

            juce::PluginDescription pd;
            pd.pluginFormatName = "VST3";
            pd.fileOrIdentifier = bundle.getFullPathName();
            pd.name             = cls["Name"].toString();
            pd.manufacturerName = vendor;
            pd.version          = version;
            pd.category         = cls["Sub-Categories"].toString();

            // UID is a 32-char hex string; use last 8 hex chars as uniqueId
            auto uid = cls["UID"].toString().removeCharacters ("-");
            if (uid.length() >= 8)
                pd.uniqueId = (int) uid.substring (uid.length() - 8).getHexValue64();

            pd.isInstrument = pd.category.containsIgnoreCase ("Instrument")
                           || pd.category.containsIgnoreCase ("Synth");

            if (pd.name.isNotEmpty() && ! pd.name.equalsIgnoreCase (selfStem))
            {
                list.addType (pd);
                found = true;
            }
        }
        return found;
    }

    //--------------------------------------------------------------------------
    // Scan a single AU bundle by reading Contents/Info.plist (XML format).
    // Returns true if at least one AudioComponent entry was found.

    static bool scanAUBundle (const juce::File& bundle,
                               const juce::String& selfStem,
                               juce::KnownPluginList& list)
    {
        if (bundle.getFileNameWithoutExtension().equalsIgnoreCase (selfStem))
            return false;

        auto plist = bundle.getChildFile ("Contents/Info.plist");
        if (! plist.existsAsFile()) return false;

        // XmlDocument::parse fails on binary plists — that's fine, we skip them
        auto xml = juce::XmlDocument::parse (plist);
        if (xml == nullptr) return false;

        // Plist structure: <plist><dict>...<key>AudioComponents</key><array>...
        auto* dict = xml->getFirstChildElement();
        if (dict == nullptr) return false;

        juce::XmlElement* acArray = nullptr;
        for (auto* kv = dict->getFirstChildElement(); kv != nullptr; kv = kv->getNextElement())
        {
            if (kv->getTagName() == "key" && kv->getAllSubText() == "AudioComponents")
            {
                acArray = kv->getNextElement();
                break;
            }
        }
        if (acArray == nullptr) return false;

        bool found = false;
        for (auto* entry = acArray->getFirstChildElement();
             entry != nullptr;
             entry = entry->getNextElement())
        {
            if (entry->getTagName() != "dict") continue;

            juce::String name, typeStr, subStr, mfrStr;

            for (auto* kv = entry->getFirstChildElement(); kv != nullptr; kv = kv->getNextElement())
            {
                if (kv->getTagName() != "key") continue;
                const auto key = kv->getAllSubText();
                auto* val      = kv->getNextElement();
                if (val == nullptr) continue;

                if      (key == "name")         name    = val->getAllSubText();
                else if (key == "type")         typeStr = val->getAllSubText();
                else if (key == "subtype")      subStr  = val->getAllSubText();
                else if (key == "manufacturer") mfrStr  = val->getAllSubText();
            }

            if (typeStr.length() < 4 || subStr.length() < 4 || mfrStr.length() < 4)
                continue;

            juce::PluginDescription pd;
            pd.pluginFormatName = "AudioUnit";
            pd.fileOrIdentifier = makeAUIdentifier (typeStr, subStr, mfrStr);

            // Info.plist name is "Vendor: Plugin Name" — split on ':'
            if (name.containsChar (':'))
            {
                pd.manufacturerName = name.upToFirstOccurrenceOf (":", false, false).trim();
                pd.name             = name.fromFirstOccurrenceOf (":", false, false).trim();
            }
            else
            {
                pd.name = name.isEmpty() ? bundle.getFileNameWithoutExtension() : name;
            }

            pd.isInstrument = (typeStr == "aumu");

            // uniqueId: sub-type bytes as int (enough to distinguish within a manufacturer)
            pd.uniqueId = (int) (((uint32_t)(uint8_t)subStr[0] << 24)
                               | ((uint32_t)(uint8_t)subStr[1] << 16)
                               | ((uint32_t)(uint8_t)subStr[2] <<  8)
                               |  (uint32_t)(uint8_t)subStr[3]);

            if (pd.name.isNotEmpty() && ! pd.name.equalsIgnoreCase (selfStem))
            {
                list.addType (pd);
                found = true;
            }
        }
        return found;
    }

} // namespace SafeScanner

//==============================================================================
// SafePluginScanThread — uses SafeScanner (no DLL loading)

class SafePluginScanThread : public juce::Thread
{
public:
    explicit SafePluginScanThread (PluginScopeAudioProcessor& owner)
        : juce::Thread ("PluginScanner"), ownerProcessor (owner)
    {
    }

    void run() override
    {
        // Derive self-bundle stem for exclusion (same logic regardless of format)
        auto selfExe  = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        auto selfStem = selfExe.getParentDirectory()  // MacOS/
                               .getParentDirectory()  // Contents/
                               .getParentDirectory()  // PluginScope.{vst3,component}
                               .getFileNameWithoutExtension(); // "PluginScope"

        // Rebuild the list from scratch each time
        ownerProcessor.knownPluginList.clear();

        // ---- VST3 -------------------------------------------------------
        for (auto& dir : { juce::File ("/Library/Audio/Plug-Ins/VST3"),
                            juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("Library/Audio/Plug-Ins/VST3") })
        {
            if (threadShouldExit() || ! dir.isDirectory()) continue;

            for (const auto& entry : juce::RangedDirectoryIterator (
                     dir, true, "*.vst3", juce::File::findDirectories))
            {
                if (threadShouldExit()) break;
                if (SafeScanner::scanVST3Bundle (entry.getFile(), selfStem,
                                                 ownerProcessor.knownPluginList))
                    SafeScanner::postProgress (ownerProcessor.processorAlive, ownerProcessor);
            }
        }

        // ---- AU ---------------------------------------------------------
        for (auto& dir : { juce::File ("/Library/Audio/Plug-Ins/Components"),
                            juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("Library/Audio/Plug-Ins/Components") })
        {
            if (threadShouldExit() || ! dir.isDirectory()) continue;

            for (const auto& entry : juce::RangedDirectoryIterator (
                     dir, false, "*.component", juce::File::findDirectories))
            {
                if (threadShouldExit()) break;
                if (SafeScanner::scanAUBundle (entry.getFile(), selfStem,
                                               ownerProcessor.knownPluginList))
                    SafeScanner::postProgress (ownerProcessor.processorAlive, ownerProcessor);
            }
        }

        // Persist results
        {
            std::unique_ptr<juce::XmlElement> xml (ownerProcessor.knownPluginList.createXml());
            if (xml != nullptr)
            {
                auto cacheFile = ownerProcessor.getScanCacheFile();
                cacheFile.getParentDirectory().createDirectory();
                xml->writeTo (cacheFile);
            }
        }

        // Signal completion
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

    // Load persisted scan cache if it exists (avoids full rescan on every launch).
    // Mark scanComplete=true immediately so the UI can display cached results.
    auto cacheFile = getScanCacheFile();
    if (cacheFile.existsAsFile())
    {
        auto xml = juce::parseXML (cacheFile);
        if (xml != nullptr)
        {
            knownPluginList.recreateFromXml (*xml);
            scanComplete.store (true);
        }
    }

    // NOTE: No auto-scan on startup.
    //
    // In-process plugin scanning loads VST3/AU binaries on a background thread.
    // This is inherently unsafe when the host has already loaded other plugins —
    // those plugins' message-thread initialization can race with the scanner's
    // in-process DLL loads, corrupting shared JUCE internals and causing crashes
    // in completely unrelated plugins (e.g. ScalerAudio2).
    //
    // Scanning is triggered only when the user explicitly clicks "Scan Plugins"
    // in the editor. The cache is loaded above so previously found plugins are
    // available immediately without scanning.
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
    if (scanThread == nullptr || ! scanThread->isThreadRunning())
    {
        scanThread = std::make_unique<SafePluginScanThread> (*this);
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
