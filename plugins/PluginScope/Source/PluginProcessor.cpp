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
// Phase 3.3: FrequencyAnalysisThread
//
// Background thread that reads accumulated samples from the wet and dry capture
// FIFOs, computes complex FFTs of both, divides wet by dry to get the transfer
// function H(f), extracts magnitude in dB per bin, and applies a 32-frame
// exponential moving average.  The result is stored in freqResponseResult,
// protected by resultMutex for safe UI reads.
//
// Thread safety model:
//   - captureFifo / dryCaptureFifo: juce::AbstractFifo (single producer on
//     audio thread, single consumer here) — no additional locking needed.
//   - fftDryBuf / fftWetBuf / freqResponseAccum: accessed ONLY here — no lock.
//   - freqResponseResult: protected by resultMutex (CriticalSection).
//   - currentSampleRate: benign scalar data race (double, written in prepareToPlay
//     on message thread, read here on analysis thread).  Acceptable for a scalar
//     that only changes on stream restart.
//==============================================================================

class FrequencyAnalysisThread : public juce::Thread
{
public:
    explicit FrequencyAnalysisThread (PluginScopeAudioProcessor& owner)
        : juce::Thread ("FFTAnalysis"), proc (owner) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            // Guard against analysisModeParam being null before prepareToPlay() is called
            if (proc.analysisModeParam == nullptr || proc.analysisTypeParam == nullptr)
            {
                juce::Thread::sleep (20);
                continue;
            }

            // Snapshot mode — freeze result, don't analyse new data
            const int analysisMode = static_cast<int> (proc.analysisModeParam->load());
            if (analysisMode == 1)
            {
                juce::Thread::sleep (50);
                continue;
            }

            const int analysisType = static_cast<int> (proc.analysisTypeParam->load());

            if (analysisType == 0)   // Frequency Response (Phase 3.3)
            {
                computeFrequencyResponse();
            }
            else if (analysisType == 2)   // Harmonic Distortion (Phase 3.4)
            {
                computeThdMeasurement();
            }
            else
            {
                juce::Thread::sleep (20);   // Other analysis types not yet implemented
            }
        }
    }

private:
    PluginScopeAudioProcessor& proc;

    //==========================================================================
    // Phase 3.3: Frequency response computation (extracted from original run())

    void computeFrequencyResponse()
    {
        // Wait until both FIFOs contain at least one full FFT frame
        const int needed = proc.kFftSize;
        if (proc.captureFifo.getNumReady() / 2 < needed ||
            proc.dryCaptureFifo.getNumReady() / 2 < needed)
        {
            juce::Thread::sleep (10);
            return;
        }

        // Read kFftSize mono samples (left channel) from each FIFO
        proc.readFifoIntoBuffer (proc.captureFifo,    proc.captureBuffer,
                                 proc.fftWetBuf.data(), needed);
        proc.readFifoIntoBuffer (proc.dryCaptureFifo, proc.dryCaptureBuffer,
                                 proc.fftDryBuf.data(), needed);

        // Apply Hann window to reduce spectral leakage
        proc.window.multiplyWithWindowingTable (proc.fftWetBuf.data(), (size_t) proc.kFftSize);
        proc.window.multiplyWithWindowingTable (proc.fftDryBuf.data(), (size_t) proc.kFftSize);

        // Forward real FFT — output is interleaved Re/Im pairs (bin 0 … N/2)
        proc.fft.performRealOnlyForwardTransform (proc.fftWetBuf.data(), true);
        proc.fft.performRealOnlyForwardTransform (proc.fftDryBuf.data(), true);

        // Compute transfer function H(f) = Wet / Dry per bin, extract |H| in dB
        const float sampleRate = static_cast<float> (proc.currentSampleRate);
        std::vector<std::pair<float,float>> frame (proc.kFreqBins);

        for (int bin = 0; bin < proc.kFreqBins; ++bin)
        {
            const float binHz = static_cast<float> (bin) * sampleRate
                                / static_cast<float> (proc.kFftSize);

            const float wetRe = proc.fftWetBuf[bin * 2];
            const float wetIm = proc.fftWetBuf[bin * 2 + 1];
            const float dryRe = proc.fftDryBuf[bin * 2];
            const float dryIm = proc.fftDryBuf[bin * 2 + 1];

            // H(f) = Wet / Dry (complex division)
            const float dryMagSq = dryRe * dryRe + dryIm * dryIm + 1e-20f;
            const float hRe = (wetRe * dryRe + wetIm * dryIm) / dryMagSq;
            const float hIm = (wetIm * dryRe - wetRe * dryIm) / dryMagSq;
            const float hMag = std::sqrt (hRe * hRe + hIm * hIm);

            frame[bin] = { binHz, 20.0f * std::log10 (hMag + 1e-10f) };
        }

        // 32-frame exponential moving average
        proc.freqResponseFrameCount++;
        if (proc.freqResponseFrameCount == 1 ||
            (int) proc.freqResponseAccum.size() != proc.kFreqBins)
        {
            proc.freqResponseAccum = frame;
        }
        else
        {
            const float alpha = 1.0f / static_cast<float> (
                juce::jmin (proc.freqResponseFrameCount, 32));
            for (int bin = 0; bin < proc.kFreqBins; ++bin)
                proc.freqResponseAccum[bin].second +=
                    alpha * (frame[bin].second - proc.freqResponseAccum[bin].second);
        }

        // Publish result (lock protected — read by UI thread via getFreqResponse())
        {
            const juce::ScopedLock lock (proc.resultMutex);
            proc.freqResponseResult = proc.freqResponseAccum;
        }

        juce::Thread::sleep (5);   // ~200 Hz max analysis rate; UI will throttle further
    }

    //==========================================================================
    // Phase 3.4: THD harmonic distortion computation

    void computeThdMeasurement()
    {
        // Need kFftSize wet samples
        const int needed = proc.kFftSize;
        if (proc.captureFifo.getNumReady() / 2 < needed)
        {
            juce::Thread::sleep (10);
            return;
        }

        // Read wet output into work buffer
        proc.readFifoIntoBuffer (proc.captureFifo, proc.captureBuffer,
                                 proc.fftWetBuf.data(), needed);

        // Discard dry FIFO contents (1 kHz sine — we don't need dry for THD)
        if (proc.dryCaptureFifo.getNumReady() / 2 >= needed)
        {
            int s1, sz1, s2, sz2;
            proc.dryCaptureFifo.prepareToRead (needed * 2, s1, sz1, s2, sz2);
            proc.dryCaptureFifo.finishedRead (sz1 + sz2);
        }

        // Apply flat-top window (amplitude accuracy over frequency resolution)
        proc.windowFlatTop.multiplyWithWindowingTable (proc.fftWetBuf.data(),
                                                       (size_t) proc.kFftSize);

        // Forward real FFT — output is interleaved Re/Im pairs
        proc.fft.performRealOnlyForwardTransform (proc.fftWetBuf.data(), true);

        // Bin spacing at current sample rate
        const float sampleRate = static_cast<float> (proc.currentSampleRate);
        const float binHz      = sampleRate / static_cast<float> (proc.kFftSize);

        // Helper: get peak magnitude in a ±2-bin window around a target frequency
        auto getMagAtFreq = [&] (float targetHz) -> float
        {
            const int centerBin = static_cast<int> (std::round (targetHz / binHz));
            const int lo = juce::jmax (0, centerBin - 2);
            const int hi = juce::jmin (proc.kFreqBins - 1, centerBin + 2);
            float peak = 0.0f;
            for (int b = lo; b <= hi; ++b)
            {
                const float re  = proc.fftWetBuf[static_cast<size_t> (b * 2)];
                const float im  = proc.fftWetBuf[static_cast<size_t> (b * 2 + 1)];
                const float mag = std::sqrt (re * re + im * im);
                if (mag > peak) peak = mag;
            }
            return peak;
        };

        // Extract H1..H8 (stop at Nyquist)
        const float nyquist = sampleRate * 0.5f;
        std::vector<std::pair<int,float>> harmonics;
        harmonics.reserve (8);

        float h1Linear      = 1e-10f;
        float harmonicSumSq = 0.0f;

        for (int n = 1; n <= 8; ++n)
        {
            const float freqHz = proc.kThdFundamental * static_cast<float> (n);
            if (freqHz >= nyquist) break;

            const float mag    = getMagAtFreq (freqHz);
            const float mag_db = 20.0f * std::log10 (mag + 1e-10f);
            harmonics.push_back ({ n, mag_db });

            if (n == 1)
                h1Linear = mag + 1e-10f;
            else
                harmonicSumSq += mag * mag;
        }

        const float thd = std::sqrt (harmonicSumSq) / h1Linear * 100.0f;

        // Publish results (protected by the shared resultMutex)
        {
            const juce::ScopedLock lock (proc.resultMutex);
            proc.thdHarmonics = harmonics;
            proc.thdPercent   = thd;
        }

        juce::Thread::sleep (100);   // THD updates at ~10 Hz (slow measurement)
    }
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

    // Phase 3.3: Pre-allocate FFT work buffers and result vectors
    fftDryBuf.resize ((size_t) kFftSize * 2, 0.0f);
    fftWetBuf.resize ((size_t) kFftSize * 2, 0.0f);
    freqResponseResult.resize ((size_t) kFreqBins, { 0.0f, 0.0f });
    freqResponseAccum .resize ((size_t) kFreqBins, { 0.0f, 0.0f });

    // Phase 3.4: Pre-allocate THD result storage (up to 8 harmonics)
    thdHarmonics.reserve (8);

    // Start background FFT analysis thread (low priority — never blocks audio)
    analysisThreadShouldRun.store (true);
    analysisThread = std::make_unique<FrequencyAnalysisThread> (*this);
    analysisThread->startThread (juce::Thread::Priority::low);
}

PluginScopeAudioProcessor::~PluginScopeAudioProcessor()
{
    // Invalidate all pending callAsync lambdas FIRST — they hold a shared_ptr
    // copy of processorAlive and will bail out before touching *this.
    processorAlive->store (false);

    // Signal audio thread to stop using hosted plugin BEFORE we destroy it
    pluginReady.store (false);

    // Phase 3.3: Stop analysis thread BEFORE destroying buffers it may be reading
    analysisThreadShouldRun.store (false);
    if (analysisThread != nullptr)
        analysisThread->stopThread (3000);

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

    // Phase 3.2: Resize and reset the lock-free wet capture buffer
    captureBuffer.setSize (2, kCaptureFifoSize);
    captureBuffer.clear();
    captureFifo.reset();

    // Phase 3.3: Resize and reset the lock-free dry reference capture buffer
    dryCaptureBuffer.setSize (2, kCaptureFifoSize);
    dryCaptureBuffer.clear();
    dryCaptureFifo.reset();

    // Reset sweep state on sample rate change
    sweepPhase    = 0.0f;
    sweepPosition = 0.0f;

    // Phase 3.4: Reset THD sine generator phase
    thdSinePhase = 0.0f;
}

void PluginScopeAudioProcessor::releaseResources()
{
    // Release scratch buffers to save memory when plugin is not in use
    hostedInputBuffer .setSize (0, 0);
    hostedOutputBuffer.setSize (0, 0);
    captureBuffer   .setSize (0, 0);
    dryCaptureBuffer.setSize (0, 0);
}

//==============================================================================
// Phase 3.2: Helper — write hostedOutputBuffer samples into the lock-free
// capture FIFO so analysis engines (Phases 3.3+) can read them.
// Called from processBlock() on the audio thread; must be lock-free.

void PluginScopeAudioProcessor::captureOutputSamples (int numSamples)
{
    // We store two channels (L and R) using separate rows of captureBuffer.
    // Each "slot" in the FIFO represents ONE sample for ONE channel, but we
    // reserve pairs (L then R) so analysis engines always read aligned pairs.
    // For simplicity in Phase 3.2 we just write numSamples slots per channel
    // into captureBuffer row 0 (L) and row 1 (R) independently.  Phase 3.3
    // will decide the exact convention when it reads.

    const int numToWrite = juce::jmin (numSamples,
                                       captureFifo.getFreeSpace() / 2);
    if (numToWrite <= 0)
        return;

    int start1, size1, start2, size2;
    captureFifo.prepareToWrite (numToWrite * 2, start1, size1, start2, size2);

    // Write region 1 — interleave L/R into sequential FIFO indices
    for (int i = 0; i < size1 / 2; ++i)
    {
        captureBuffer.setSample (0, start1 + i * 2,     hostedOutputBuffer.getSample (0, i));
        captureBuffer.setSample (0, start1 + i * 2 + 1, hostedOutputBuffer.getSample (1, i));
    }

    // Write region 2 (wrap-around)
    const int offset = size1 / 2;
    for (int i = 0; i < size2 / 2; ++i)
    {
        captureBuffer.setSample (0, start2 + i * 2,     hostedOutputBuffer.getSample (0, offset + i));
        captureBuffer.setSample (0, start2 + i * 2 + 1, hostedOutputBuffer.getSample (1, offset + i));
    }

    captureFifo.finishedWrite (numToWrite * 2);
}

//==============================================================================
// Phase 3.3: Helper — write hostedInputBuffer samples into the dry reference
// capture FIFO.  Mirrors captureOutputSamples() but reads from hostedInputBuffer.
// Called from processBlock() on the audio thread; must be lock-free.

void PluginScopeAudioProcessor::captureDrySamples (int numSamples)
{
    const int numToWrite = juce::jmin (numSamples,
                                       dryCaptureFifo.getFreeSpace() / 2);
    if (numToWrite <= 0)
        return;

    int start1, size1, start2, size2;
    dryCaptureFifo.prepareToWrite (numToWrite * 2, start1, size1, start2, size2);

    // Write region 1 — interleave L/R into sequential FIFO indices
    for (int i = 0; i < size1 / 2; ++i)
    {
        dryCaptureBuffer.setSample (0, start1 + i * 2,     hostedInputBuffer.getSample (0, i));
        dryCaptureBuffer.setSample (0, start1 + i * 2 + 1, hostedInputBuffer.getSample (1, i));
    }

    // Write region 2 (wrap-around)
    const int offset = size1 / 2;
    for (int i = 0; i < size2 / 2; ++i)
    {
        dryCaptureBuffer.setSample (0, start2 + i * 2,     hostedInputBuffer.getSample (0, offset + i));
        dryCaptureBuffer.setSample (0, start2 + i * 2 + 1, hostedInputBuffer.getSample (1, offset + i));
    }

    dryCaptureFifo.finishedWrite (numToWrite * 2);
}

//==============================================================================
// Phase 3.3: Helper — read numSamples mono samples from a FIFO into a float
// buffer for FFT processing.  The FIFO stores interleaved stereo (L, R, L, R …)
// so we consume numSamples * 2 FIFO slots and extract every other slot (L only).
// Called from the analysis thread (single consumer); juce::AbstractFifo is safe.

void PluginScopeAudioProcessor::readFifoIntoBuffer (juce::AbstractFifo& fifo,
                                                     juce::AudioBuffer<float>& srcBuf,
                                                     float* dest,
                                                     int numSamples)
{
    const int numSlots = numSamples * 2;   // interleaved stereo slots
    int start1, size1, start2, size2;
    fifo.prepareToRead (numSlots, start1, size1, start2, size2);

    // Extract left channel samples (even indices within each region)
    int destIdx = 0;
    for (int i = 0; i < size1; i += 2)
        dest[destIdx++] = srcBuf.getSample (0, start1 + i);
    for (int i = 0; i < size2; i += 2)
        dest[destIdx++] = srcBuf.getSample (0, start2 + i);

    fifo.finishedRead (numSlots);
}

//==============================================================================
// Phase 3.3: Public accessor — returns a copy of the latest frequency response
// result vector, protected by resultMutex so the UI thread can call safely.

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getFreqResponse() const
{
    const juce::ScopedLock lock (resultMutex);
    return freqResponseResult;
}

//==============================================================================
// Phase 3.4: THD result accessors — thread-safe reads of thdHarmonics/thdPercent.
// Both protected by resultMutex (same lock used by freqResponseResult).

std::vector<std::pair<int,float>> PluginScopeAudioProcessor::getThdHarmonics() const
{
    const juce::ScopedLock lock (resultMutex);
    return thdHarmonics;
}

float PluginScopeAudioProcessor::getThdPercent() const
{
    const juce::ScopedLock lock (resultMutex);
    return thdPercent;
}

//==============================================================================
void PluginScopeAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    // Clear any unused output channels
    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, numSamples);

    // Read active test signal mode (0-4, atomic load — real-time safe)
    const int testSignalMode = static_cast<int> (testSignalParam->load());

    // --- Detect test signal mode change ---
    // Reset impulsePending when the user re-selects Impulse mode (index 3)
    if (testSignalMode != previousTestSignalMode)
    {
        if (testSignalMode == 3)
            impulsePending = true;
        previousTestSignalMode = testSignalMode;
    }

    // -----------------------------------------------------------------------
    // Mode 4: Live Audio — copy DAW input through the hosted plugin
    // -----------------------------------------------------------------------
    if (testSignalMode == 4)
    {
        if (pluginReady.load() && hostedPlugin != nullptr)
        {
            // Copy DAW input into hosted plugin's pre-allocated input buffer
            for (int ch = 0; ch < numChannels; ++ch)
                hostedInputBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

            // Phase 3.3: Capture DAW input as the dry (pre-plugin) reference
            captureDrySamples (numSamples);

            // Clear hosted output buffer before processing
            hostedOutputBuffer.clear();

            juce::MidiBuffer emptyMidi;
            try
            {
                hostedPlugin->processBlock (hostedOutputBuffer, emptyMidi);
            }
            catch (...)
            {
                pluginReady.store (false);
            }

            if (pluginReady.load())
            {
                // Copy hosted output back to the DAW buffer
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.copyFrom (ch, 0, hostedOutputBuffer, ch, 0, numSamples);

                // Capture output for analysis engines
                captureOutputSamples (numSamples);
            }
        }
        // If no hosted plugin in Live Audio mode: pass through unchanged
        return;
    }

    // -----------------------------------------------------------------------
    // THD override: when analysis_type == 2 (Harmonic Distortion), inject a
    // spectrally pure 1 kHz sine regardless of the test_signal parameter.
    // This block handles capture and early-return so normal signal path is skipped.
    // -----------------------------------------------------------------------
    {
        const int analysisType = static_cast<int> (analysisTypeParam->load());
        if (analysisType == 2)
        {
            const float levelScale    = juce::Decibels::decibelsToGain (-12.0f);
            const float phaseIncrement = 2.0f * juce::MathConstants<float>::pi
                                         * kThdFundamental
                                         / static_cast<float> (currentSampleRate);
            for (int i = 0; i < numSamples; ++i)
            {
                const float sample = std::sin (thdSinePhase) * levelScale;
                hostedInputBuffer.setSample (0, i, sample);
                hostedInputBuffer.setSample (1, i, sample);
                thdSinePhase += phaseIncrement;
                if (thdSinePhase > juce::MathConstants<float>::twoPi)
                    thdSinePhase -= juce::MathConstants<float>::twoPi;
            }

            // Capture the 1 kHz sine as the dry reference
            captureDrySamples (numSamples);

            // Route through hosted plugin and capture wet output
            if (pluginReady.load() && hostedPlugin != nullptr)
            {
                hostedOutputBuffer.clear();
                juce::MidiBuffer emptyMidi;
                try
                {
                    hostedPlugin->processBlock (hostedOutputBuffer, emptyMidi);
                }
                catch (...)
                {
                    pluginReady.store (false);
                }
                if (pluginReady.load())
                    captureOutputSamples (numSamples);
            }

            buffer.clear();   // Output silence to DAW
            return;           // Skip normal test signal switch
        }
    }

    // -----------------------------------------------------------------------
    // Modes 0-3: Generated test signals — fill hostedInputBuffer, output silence
    // -----------------------------------------------------------------------

    const float levelScale = juce::Decibels::decibelsToGain (-12.0f);

    switch (testSignalMode)
    {
        // --- Mode 0: Logarithmic Sine Sweep (20 Hz – 20 kHz) ---
        case 0:
        {
            // Log-chirp formula: instantaneous frequency grows exponentially.
            // K and L are constants derived from sweep duration and start/end freqs.
            const float K = kSweepDurationSeconds / std::log (kSweepFEnd / kSweepFStart);

            for (int i = 0; i < numSamples; ++i)
            {
                const float t = sweepPosition * kSweepDurationSeconds;
                // Increment phase by 2*pi * instantaneous_frequency / sampleRate
                sweepPhase += 2.0f * juce::MathConstants<float>::pi
                              * kSweepFStart * std::exp (t / K)
                              / static_cast<float> (currentSampleRate);

                const float sample = std::sin (sweepPhase) * levelScale;
                hostedInputBuffer.setSample (0, i, sample);
                hostedInputBuffer.setSample (1, i, sample);

                sweepPosition += 1.0f / (kSweepDurationSeconds
                                         * static_cast<float> (currentSampleRate));
                if (sweepPosition >= 1.0f)
                {
                    sweepPosition = 0.0f;
                    sweepPhase    = 0.0f;   // Reset to prevent float overflow on loop
                }
            }
            break;
        }

        // --- Mode 1: White Noise ---
        case 1:
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float sample = (noiseRandom.nextFloat() * 2.0f - 1.0f) * levelScale;
                hostedInputBuffer.setSample (0, i, sample);
                hostedInputBuffer.setSample (1, i, sample);
            }
            break;
        }

        // --- Mode 2: Pink Noise (Paul Kellet 6-stage IIR) ---
        case 2:
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float white = noiseRandom.nextFloat() * 2.0f - 1.0f;
                pinkB0 = 0.99886f * pinkB0 + white * 0.0555179f;
                pinkB1 = 0.99332f * pinkB1 + white * 0.0750759f;
                pinkB2 = 0.96900f * pinkB2 + white * 0.1538520f;
                pinkB3 = 0.86650f * pinkB3 + white * 0.3104856f;
                pinkB4 = 0.55000f * pinkB4 + white * 0.5329522f;
                pinkB5 = -0.7616f * pinkB5 - white * 0.0168980f;
                float pink = (pinkB0 + pinkB1 + pinkB2 + pinkB3
                              + pinkB4 + pinkB5 + pinkB6 + white * 0.5362f) * 0.11f;
                pinkB6 = white * 0.115926f;
                pink  *= levelScale;
                hostedInputBuffer.setSample (0, i, pink);
                hostedInputBuffer.setSample (1, i, pink);
            }
            break;
        }

        // --- Mode 3: Impulse (Dirac delta) ---
        case 3:
        {
            hostedInputBuffer.clear();
            if (impulsePending)
            {
                hostedInputBuffer.setSample (0, 0, levelScale);
                hostedInputBuffer.setSample (1, 0, levelScale);
                impulsePending = false;   // One-shot; reset when mode is re-selected
            }
            break;
        }

        default:
            hostedInputBuffer.clear();
            break;
    }

    // --- Route test signal through hosted plugin (if ready) and capture output ---
    if (pluginReady.load() && hostedPlugin != nullptr)
    {
        // Phase 3.3: Capture generated test signal as the dry (pre-plugin) reference
        captureDrySamples (numSamples);

        hostedOutputBuffer.clear();

        juce::MidiBuffer emptyMidi;
        try
        {
            hostedPlugin->processBlock (hostedOutputBuffer, emptyMidi);
        }
        catch (...)
        {
            pluginReady.store (false);
        }

        // Capture output for analysis engines (Phases 3.3+)
        captureOutputSamples (numSamples);
    }

    // Output SILENCE to the DAW — PluginScope does not emit test signals downstream
    buffer.clear();
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
