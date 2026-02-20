#include "PluginProcessor.h"
#include "PluginEditor.h"

#if JUCE_MAC
 #include <AudioUnit/AudioUnit.h>   // AudioComponent, AudioComponentFindNext, OSType constants
 #include <CoreFoundation/CoreFoundation.h> // CFStringRef, CFRelease, noErr
#endif

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

    // Overload for numeric OSType values (used with AudioComponentFindNext)
    static juce::String makeAUIdentifier (uint32_t type, uint32_t sub, uint32_t mfr)
    {
        return makeAUIdentifier (osTypeToString (type),
                                 osTypeToString (sub),
                                 osTypeToString (mfr));
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
    // VST3: read Contents/moduleinfo.json (VST3 3.7+ standard, no DLL load)

    static bool scanVST3ModuleInfo (const juce::File& bundle,
                                     const juce::String& selfStem,
                                     juce::KnownPluginList& list)
    {
        auto moduleInfo = bundle.getChildFile ("Contents/moduleinfo.json");
        if (! moduleInfo.existsAsFile()) return false;

        auto jsonVar = juce::JSON::parse (moduleInfo);
        if (! jsonVar.isObject()) return false;

        auto factoryInfo = jsonVar["Factory Info"];
        juce::String vendor;
        if (factoryInfo.isObject())
            vendor = factoryInfo["Vendor"].toString();

        juce::String version = jsonVar["Version"].toString();

        auto classes = jsonVar["Classes"];
        if (! classes.isArray()) return false;

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
    // VST3 fallback: read Contents/Info.plist when moduleinfo.json is absent.
    // Older VST3 plugins (pre-3.7) don't have moduleinfo.json. Their Info.plist
    // (standard macOS bundle plist) contains CFBundleName and version. That's
    // enough for the list — JUCE loads VST3 by bundle path, not by class UID.

    static bool scanVST3InfoPlist (const juce::File& bundle,
                                    const juce::String& selfStem,
                                    juce::KnownPluginList& list)
    {
        auto plist = bundle.getChildFile ("Contents/Info.plist");
        if (! plist.existsAsFile()) return false;

        // XmlDocument::parse fails silently on binary plists — skip those
        auto xml = juce::XmlDocument::parse (plist);
        if (xml == nullptr) return false;

        auto* dict = xml->getFirstChildElement();
        if (dict == nullptr) return false;

        juce::String name, version;
        for (auto* kv = dict->getFirstChildElement(); kv != nullptr; kv = kv->getNextElement())
        {
            if (kv->getTagName() != "key") continue;
            const auto key = kv->getAllSubText();
            auto* val      = kv->getNextElement();
            if (val == nullptr) continue;

            if      (key == "CFBundleName" && name.isEmpty())
                name = val->getAllSubText();
            else if (key == "CFBundleExecutable" && name.isEmpty())
                name = val->getAllSubText();
            else if (key == "CFBundleShortVersionString" && version.isEmpty())
                version = val->getAllSubText();
            else if (key == "CFBundleVersion" && version.isEmpty())
                version = val->getAllSubText();
        }

        if (name.isEmpty())
            name = bundle.getFileNameWithoutExtension();

        if (name.equalsIgnoreCase (selfStem)) return false;

        juce::PluginDescription pd;
        pd.pluginFormatName = "VST3";
        pd.fileOrIdentifier = bundle.getFullPathName();
        pd.name             = name;
        pd.version          = version;
        pd.uniqueId         = (int) name.hashCode();

        list.addType (pd);
        return true;
    }

    //--------------------------------------------------------------------------
    // VST3 top-level: try moduleinfo.json first, fall back to Info.plist.

    static bool scanVST3Bundle (const juce::File& bundle,
                                 const juce::String& selfStem,
                                 juce::KnownPluginList& list)
    {
        if (bundle.getFileNameWithoutExtension().equalsIgnoreCase (selfStem))
            return false;

        if (scanVST3ModuleInfo (bundle, selfStem, list))
            return true;

        return scanVST3InfoPlist (bundle, selfStem, list);
    }

    //--------------------------------------------------------------------------
    // AU: enumerate via macOS Audio Component registry (AudioComponentFindNext).
    //
    // The system registry is populated by macOS from AU bundle metadata at boot
    // and on plugin installation — no plugin binary is loaded by this API.
    // This finds 100% of installed AUs regardless of plist format.

   #if JUCE_MAC
    static void scanAudioUnitsFromRegistry (const juce::String& selfStem,
                                             juce::KnownPluginList& list)
    {
        // Types that represent audio processors (excludes I/O, codecs, etc.)
        const OSType kInterestingTypes[] = {
            kAudioUnitType_Effect,
            kAudioUnitType_MusicDevice,
            kAudioUnitType_MusicEffect,
            kAudioUnitType_Generator,
            kAudioUnitType_Panner,
            kAudioUnitType_Mixer,
            kAudioUnitType_MIDIProcessor
        };

        for (auto auType : kInterestingTypes)
        {
            AudioComponentDescription searchDesc = {};
            searchDesc.componentType = auType;

            AudioComponent comp = nullptr;
            while ((comp = AudioComponentFindNext (comp, &searchDesc)) != nullptr)
            {
                AudioComponentDescription compDesc;
                if (AudioComponentGetDescription (comp, &compDesc) != noErr) continue;

                CFStringRef cfName = nullptr;
                if (AudioComponentCopyName (comp, &cfName) != noErr) continue;

                auto fullName = juce::String::fromCFString (cfName);
                CFRelease (cfName);

                juce::String mfr, name;
                if (fullName.containsChar (':'))
                {
                    mfr  = fullName.upToFirstOccurrenceOf (":", false, false).trim();
                    name = fullName.fromFirstOccurrenceOf (":", false, false).trim();
                }
                else
                {
                    name = fullName;
                }

                if (name.isEmpty() || name.equalsIgnoreCase (selfStem)) continue;

                juce::PluginDescription pd;
                pd.pluginFormatName = "AudioUnit";
                pd.fileOrIdentifier = makeAUIdentifier ((uint32_t) compDesc.componentType,
                                                         (uint32_t) compDesc.componentSubType,
                                                         (uint32_t) compDesc.componentManufacturer);
                pd.name             = name;
                pd.manufacturerName = mfr;
                pd.isInstrument     = (compDesc.componentType == kAudioUnitType_MusicDevice);
                pd.uniqueId         = (int) compDesc.componentSubType;

                list.addType (pd);
            }
        }
    }
   #endif // JUCE_MAC

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
        // Use macOS Audio Component registry — reads OS metadata, zero DLL loading.
        // Finds all registered AUs regardless of plist format or install location.
       #if JUCE_MAC
        if (! threadShouldExit())
        {
            SafeScanner::scanAudioUnitsFromRegistry (selfStem,
                                                      ownerProcessor.knownPluginList);
            SafeScanner::postProgress (ownerProcessor.processorAlive, ownerProcessor);
        }
       #endif

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
