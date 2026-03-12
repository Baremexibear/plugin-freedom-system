#include "PluginProcessor.h"
#include "PluginEditor.h"

#if JUCE_MAC
 #include <AudioUnit/AudioUnit.h>   // AudioComponent, AudioComponentFindNext, OSType constants
 #include <CoreFoundation/CoreFoundation.h> // CFStringRef, CFRelease, noErr
 #include <sys/xattr.h>             // removexattr — clear quarantine before dlopen
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

            // Compute uniqueId / deprecatedUid using JUCE's internal polynomial
            // hash so that VST3ModuleHandle::findClassMatchingDescription() can
            // match this description against the factory class at load time.
            //
            // JUCE requires (from juce_VST3PluginFormatImpl.h):
            //   uniqueId     = getHashForRange(getNormalisedTUID(tuid))
            //                  where getNormalisedTUID returns 4 big-endian uint32s
            //   deprecatedUid = getHashForRange(tuid) — hash of 16 raw bytes
            //   hash fn: value = (value*31 + item) for each item in range
            //
            // The matching check is:
            //   if (uniqueId != desc.uniqueId && deprecatedUid != desc.deprecatedUid)
            //       skip class;   ← BOTH must mismatch to fail
            //
            // Our old code took only the last 4 bytes of the UUID, which never
            // matched JUCE's hash, causing createPluginInstanceAsync to return null.
            {
                auto uidStr = cls["UID"].toString().removeCharacters ("-");
                if (uidStr.length() == 32)
                {
                    uint8_t tuid[16];
                    for (int b = 0; b < 16; ++b)
                        tuid[b] = (uint8_t) uidStr.substring (b * 2, b * 2 + 2).getHexValue32();

                    // deprecatedUid: hash the 16 raw bytes
                    uint32_t depHash = 0;
                    for (auto b : tuid)
                        depHash = (depHash * 31u) + b;
                    pd.deprecatedUid = (int) depHash;

                    // uniqueId: hash the 4 big-endian uint32 groups (normalised TUID)
                    uint32_t longs[4];
                    for (int g = 0; g < 4; ++g)
                        longs[g] = ((uint32_t) tuid[g * 4 + 0] << 24)
                                 | ((uint32_t) tuid[g * 4 + 1] << 16)
                                 | ((uint32_t) tuid[g * 4 + 2] <<  8)
                                 |  (uint32_t) tuid[g * 4 + 3];
                    uint32_t uniqHash = 0;
                    for (auto l : longs)
                        uniqHash = (uniqHash * 31u) + l;
                    pd.uniqueId = (int) uniqHash;
                }
            }

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

            // ----------------------------------------------------------------
            // Reset requested by user (Analyze button) — flush stale FIFOs
            // and clear averaging accumulators so the next measurement is fresh.
            // exchange(false) atomically reads and clears the flag.
            // ----------------------------------------------------------------
            if (proc.analysisResetPending.exchange (false))
            {
                // Drain all capture FIFOs (analysis thread is sole consumer — safe)
                auto drainFifo = [] (juce::AbstractFifo& fifo)
                {
                    const int avail = fifo.getNumReady();
                    if (avail > 0)
                    {
                        int s1, sz1, s2, sz2;
                        fifo.prepareToRead (avail, s1, sz1, s2, sz2);
                        fifo.finishedRead (sz1 + sz2);
                    }
                };

                drainFifo (proc.captureFifo);
                drainFifo (proc.dryCaptureFifo);
                drainFifo (proc.captureFifoB);

                // Reset EMA accumulators so freq/phase averaging starts fresh
                proc.freqResponseFrameCount  = 0;
                proc.freqResponseAccum.clear();
                proc.freqResponseFrameCountB = 0;
                proc.freqResponseAccumB.clear();
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
            else if (analysisType == 1)   // Dynamics Curve (Phase 3.6)
            {
                computeDynamicsSweep();

                // One-shot: the sweep must NOT auto-restart.  Block here until
                // the user clicks Analyze (analysisResetPending) or switches tabs.
                while (!threadShouldExit()
                       && static_cast<int> (proc.analysisTypeParam->load()) == 1
                       && !proc.analysisResetPending.load())
                {
                    juce::Thread::sleep (100);
                }
            }
            else if (analysisType == 2)   // Harmonic Distortion (Phase 3.4)
            {
                computeThdMeasurement();
            }
            else if (analysisType == 3)   // Phase Response (Phase 3.5)
            {
                computePhaseResponse();
            }
            else if (analysisType == 4)   // Latency (Phase 3.7)
            {
                computeLatencyMeasurement();
            }
            else
            {
                juce::Thread::sleep (20);
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

        // Publish Plugin A result
        {
            const juce::ScopedLock lock (proc.resultMutex);
            proc.freqResponseResult = proc.freqResponseAccum;
        }

        // === Raw spectrum (dBFS) — dry + wet, for background spectrum display =========
        // Uses the same FFT buffers already computed above.
        // Normalise by 2/N to convert FFT magnitudes to approximate dBFS.
        {
            const float norm = 2.0f / static_cast<float> (proc.kFftSize);
            std::vector<std::pair<float,float>> dryFr (proc.kFreqBins);
            std::vector<std::pair<float,float>> wetFr (proc.kFreqBins);

            for (int bin = 0; bin < proc.kFreqBins; ++bin)
            {
                const float binHz = static_cast<float> (bin) * sampleRate
                                    / static_cast<float> (proc.kFftSize);
                const float dR = proc.fftDryBuf[(size_t)(bin * 2)];
                const float dI = proc.fftDryBuf[(size_t)(bin * 2 + 1)];
                const float wR = proc.fftWetBuf[(size_t)(bin * 2)];
                const float wI = proc.fftWetBuf[(size_t)(bin * 2 + 1)];
                dryFr[bin] = { binHz, 20.0f * std::log10 (std::sqrt (dR*dR + dI*dI) * norm + 1e-10f) };
                wetFr[bin] = { binHz, 20.0f * std::log10 (std::sqrt (wR*wR + wI*wI) * norm + 1e-10f) };
            }

            // 8-frame EMA — faster response than the transfer function average
            const float alpha = 0.125f;
            if (proc.drySpectrumAccum.size() != (size_t) proc.kFreqBins)
            {
                proc.drySpectrumAccum = dryFr;
                proc.wetSpectrumAccum = wetFr;
            }
            else
            {
                for (int bin = 0; bin < proc.kFreqBins; ++bin)
                {
                    proc.drySpectrumAccum[(size_t) bin].second +=
                        alpha * (dryFr[(size_t) bin].second - proc.drySpectrumAccum[(size_t) bin].second);
                    proc.wetSpectrumAccum[(size_t) bin].second +=
                        alpha * (wetFr[(size_t) bin].second - proc.wetSpectrumAccum[(size_t) bin].second);
                }
            }

            const juce::ScopedLock lock (proc.resultMutex);
            proc.drySpectrumResult = proc.drySpectrumAccum;
            proc.wetSpectrumResult = proc.wetSpectrumAccum;
        }

        // === Plugin B freq response (computed in the same pass when B is active) ===
        // fftDryBuf still contains the valid dry FFT from the A computation above.
        // Read B's wet output, apply window+FFT, compute H_B = wet_B / dry.
        if (proc.pluginBReady.load()
            && proc.captureFifoB.getNumReady() / 2 >= needed)
        {
            proc.readFifoIntoBuffer (proc.captureFifoB, proc.captureBufferB,
                                     proc.fftWetBuf.data(), needed);

            proc.window.multiplyWithWindowingTable (proc.fftWetBuf.data(),
                                                     (size_t) proc.kFftSize);
            proc.fft.performRealOnlyForwardTransform (proc.fftWetBuf.data(), true);

            std::vector<std::pair<float,float>> frameB (proc.kFreqBins);
            for (int bin = 0; bin < proc.kFreqBins; ++bin)
            {
                const float binHz  = static_cast<float> (bin) * sampleRate
                                     / static_cast<float> (proc.kFftSize);
                const float wetRe  = proc.fftWetBuf[bin * 2];
                const float wetIm  = proc.fftWetBuf[bin * 2 + 1];
                const float dryRe  = proc.fftDryBuf[bin * 2];
                const float dryIm  = proc.fftDryBuf[bin * 2 + 1];
                const float dryMagSq = dryRe * dryRe + dryIm * dryIm + 1e-20f;
                const float hRe    = (wetRe * dryRe + wetIm * dryIm) / dryMagSq;
                const float hIm    = (wetIm * dryRe - wetRe * dryIm) / dryMagSq;
                const float hMag   = std::sqrt (hRe * hRe + hIm * hIm);
                frameB[bin] = { binHz, 20.0f * std::log10 (hMag + 1e-10f) };
            }

            proc.freqResponseFrameCountB++;
            if (proc.freqResponseFrameCountB == 1 ||
                (int) proc.freqResponseAccumB.size() != proc.kFreqBins)
            {
                proc.freqResponseAccumB = frameB;
            }
            else
            {
                const float alpha = 1.0f / static_cast<float> (
                    juce::jmin (proc.freqResponseFrameCountB, 32));
                for (int bin = 0; bin < proc.kFreqBins; ++bin)
                    proc.freqResponseAccumB[bin].second +=
                        alpha * (frameB[bin].second - proc.freqResponseAccumB[bin].second);
            }

            const juce::ScopedLock lock (proc.resultMutex);
            proc.freqResponseResultB = proc.freqResponseAccumB;

            // Raw dBFS spectrum of Plugin B output — used for orange spectrum background
            {
                const float norm = 2.0f / static_cast<float> (proc.kFftSize);
                std::vector<std::pair<float,float>> wetFrB (proc.kFreqBins);
                for (int bin = 0; bin < proc.kFreqBins; ++bin)
                {
                    const float wR = proc.fftWetBuf[(size_t)(bin * 2)];
                    const float wI = proc.fftWetBuf[(size_t)(bin * 2 + 1)];
                    const float binHz = static_cast<float> (bin) * sampleRate
                                        / static_cast<float> (proc.kFftSize);
                    wetFrB[bin] = { binHz, 20.0f * std::log10 (std::sqrt (wR*wR + wI*wI) * norm + 1e-10f) };
                }
                const float alpha = 0.125f;
                if (proc.wetSpectrumAccumB.size() != (size_t) proc.kFreqBins)
                    proc.wetSpectrumAccumB = wetFrB;
                else
                    for (int bin = 0; bin < proc.kFreqBins; ++bin)
                        proc.wetSpectrumAccumB[(size_t)bin].second +=
                            alpha * (wetFrB[(size_t)bin].second - proc.wetSpectrumAccumB[(size_t)bin].second);
                proc.wetSpectrumResultB = proc.wetSpectrumAccumB;
            }
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

        // Plugin B THD — measure if B has enough FIFO data
        std::vector<std::pair<int,float>> harmonicsB;
        float thdB = 0.0f;
        if (proc.captureFifoB.getNumReady() / 2 >= needed)
        {
            // Reuse fftWetBuf (we've already consumed the A data above)
            proc.readFifoIntoBuffer (proc.captureFifoB, proc.captureBufferB,
                                     proc.fftWetBuf.data(), needed);
            proc.windowFlatTop.multiplyWithWindowingTable (proc.fftWetBuf.data(),
                                                           (size_t) proc.kFftSize);
            proc.fft.performRealOnlyForwardTransform (proc.fftWetBuf.data(), true);

            harmonicsB.reserve (8);
            float h1B = 1e-10f, sumSqB = 0.0f;
            for (int n = 1; n <= 8; ++n)
            {
                const float freqHz = proc.kThdFundamental * static_cast<float> (n);
                if (freqHz >= sampleRate * 0.5f) break;
                const float mag = getMagAtFreq (freqHz);
                harmonicsB.push_back ({ n, 20.0f * std::log10 (mag + 1e-10f) });
                if (n == 1) h1B = mag + 1e-10f;
                else        sumSqB += mag * mag;
            }
            thdB = std::sqrt (sumSqB) / h1B * 100.0f;
        }

        // Publish results (protected by the shared resultMutex)
        {
            const juce::ScopedLock lock (proc.resultMutex);
            proc.thdHarmonics  = harmonics;
            proc.thdPercent    = thd;
            if (!harmonicsB.empty())
            {
                proc.thdHarmonicsB = harmonicsB;
                proc.thdPercentB   = thdB;
            }
        }

        juce::Thread::sleep (100);   // THD updates at ~10 Hz (slow measurement)
    }

    //==========================================================================
    // Phase 3.5: Phase response + group delay computation
    //
    // Algorithm:
    //   1. Read kFftSize wet + dry samples (reuses same FIFOs as frequency response)
    //   2. Apply Hann window (good phase preservation, same as freq response)
    //   3. Complex FFT both signals
    //   4. H(f) = Wet / Dry per bin (complex division)
    //   5. φ(f) = atan2(Im(H), Re(H)) — wrapped phase in radians
    //   6. Unwrap phase (remove 2π jumps)
    //   7. Convert to degrees for display
    //   8. Group delay: τ_g(f) = -dφ/dω, central difference, result in ms
    //
    // Thread safety: fftWetBuf/fftDryBuf accessed only here (no lock needed).
    //               phaseResponseResult/groupDelayResult protected by resultMutex.

    void computePhaseResponse()
    {
        const int needed = proc.kFftSize;
        if (proc.captureFifo.getNumReady() / 2 < needed ||
            proc.dryCaptureFifo.getNumReady() / 2 < needed)
        {
            juce::Thread::sleep (10);
            return;
        }

        // Read wet and dry samples into FFT work buffers
        proc.readFifoIntoBuffer (proc.captureFifo,    proc.captureBuffer,
                                 proc.fftWetBuf.data(), needed);
        proc.readFifoIntoBuffer (proc.dryCaptureFifo, proc.dryCaptureBuffer,
                                 proc.fftDryBuf.data(), needed);

        // Apply Hann window — same as frequency response (preserves phase)
        proc.window.multiplyWithWindowingTable (proc.fftWetBuf.data(), (size_t) proc.kFftSize);
        proc.window.multiplyWithWindowingTable (proc.fftDryBuf.data(), (size_t) proc.kFftSize);

        // Complex FFT — output is interleaved Re/Im pairs (bin 0 … N/2)
        proc.fft.performRealOnlyForwardTransform (proc.fftWetBuf.data(), true);
        proc.fft.performRealOnlyForwardTransform (proc.fftDryBuf.data(), true);

        const float sampleRate = static_cast<float> (proc.currentSampleRate);
        const float binHz      = sampleRate / static_cast<float> (proc.kFftSize);

        // Compute wrapped phase: φ(f) = arg(H(f)) = atan2(Im(Wet/Dry), Re(Wet/Dry))
        // Magnitude gate: skip bins where the dry signal is too weak to give a reliable
        // phase estimate.  Without this, very low-energy bins (near DC or above 18 kHz
        // with a sine sweep) produce random phase that causes visible noise.
        std::vector<float> wrappedPhase (proc.kFreqBins, 0.0f);
        std::vector<std::pair<float,float>> phaseFrame (proc.kFreqBins);

        // Compute the median dry magnitude to set the gate threshold adaptively
        float maxDryMag = 0.0f;
        for (int bin = 0; bin < proc.kFreqBins; ++bin)
        {
            const float dRe = proc.fftDryBuf[(size_t) (bin * 2)];
            const float dIm = proc.fftDryBuf[(size_t) (bin * 2 + 1)];
            const float mag = std::sqrt (dRe * dRe + dIm * dIm);
            if (mag > maxDryMag) maxDryMag = mag;
        }
        // Only compute phase for bins at least 1% of the peak dry magnitude (-40 dB)
        const float gateThreshold = maxDryMag * 0.01f + 1e-10f;

        for (int bin = 0; bin < proc.kFreqBins; ++bin)
        {
            const float wetRe = proc.fftWetBuf[(size_t) (bin * 2)];
            const float wetIm = proc.fftWetBuf[(size_t) (bin * 2 + 1)];
            const float dryRe = proc.fftDryBuf[(size_t) (bin * 2)];
            const float dryIm = proc.fftDryBuf[(size_t) (bin * 2 + 1)];

            const float dryMag = std::sqrt (dryRe * dryRe + dryIm * dryIm);
            phaseFrame[bin].first = (float) bin * binHz;

            if (dryMag < gateThreshold)
            {
                // Not enough signal — interpolate from nearest valid neighbour later,
                // for now set to previous bin's phase to avoid wild jumps.
                wrappedPhase[bin] = (bin > 0) ? wrappedPhase[bin - 1] : 0.0f;
                continue;
            }

            // H(f) = Wet / Dry (complex division)
            const float dryMagSq = dryMag * dryMag + 1e-20f;
            const float hRe      = (wetRe * dryRe + wetIm * dryIm) / dryMagSq;
            const float hIm      = (wetIm * dryRe - wetRe * dryIm) / dryMagSq;

            wrappedPhase[bin] = std::atan2 (hIm, hRe);   // radians, [-π, π]
        }

        // Phase unwrapping — remove 2π discontinuities
        const std::vector<float> unwrapped = unwrapPhase (wrappedPhase);

        // Convert to degrees
        const float radToDeg = 180.0f / juce::MathConstants<float>::pi;
        for (int bin = 0; bin < proc.kFreqBins; ++bin)
            phaseFrame[bin].second = unwrapped[bin] * radToDeg;

        // Group delay: τ_g(f) = -dφ/dω  (ω = 2π·f)
        // Numerical central difference: dφ/dω ≈ (φ[n+1] - φ[n-1]) / (2·Δω)
        // Result in seconds → converted to milliseconds
        const float deltaOmega = 2.0f * juce::MathConstants<float>::pi * binHz;
        std::vector<std::pair<float,float>> groupDelayFrame (proc.kFreqBins);

        groupDelayFrame[0] = { 0.0f, 0.0f };   // DC bin — undefined, set to 0

        for (int bin = 1; bin < proc.kFreqBins - 1; ++bin)
        {
            const float dPhi        = unwrapped[bin + 1] - unwrapped[bin - 1];
            const float groupDelaySec = -dPhi / (2.0f * deltaOmega);
            groupDelayFrame[bin] = { (float) bin * binHz, groupDelaySec * 1000.0f };
        }
        // Copy last valid bin to avoid undefined boundary
        groupDelayFrame[proc.kFreqBins - 1] = groupDelayFrame[proc.kFreqBins - 2];

        // Publish Plugin A phase results
        {
            const juce::ScopedLock lock (proc.resultMutex);
            proc.phaseResponseResult = phaseFrame;
            proc.groupDelayResult    = groupDelayFrame;
        }

        // === Plugin B phase response (reuses the dry FFT still in fftDryBuf) ===
        if (proc.pluginBReady.load()
            && proc.captureFifoB.getNumReady() / 2 >= needed)
        {
            proc.readFifoIntoBuffer (proc.captureFifoB, proc.captureBufferB,
                                     proc.fftWetBuf.data(), needed);

            proc.window.multiplyWithWindowingTable (proc.fftWetBuf.data(),
                                                     (size_t) proc.kFftSize);
            proc.fft.performRealOnlyForwardTransform (proc.fftWetBuf.data(), true);

            std::vector<float> wrappedPhaseB (proc.kFreqBins);
            std::vector<std::pair<float,float>> phaseFrameB (proc.kFreqBins);

            for (int bin = 0; bin < proc.kFreqBins; ++bin)
            {
                const float wRe  = proc.fftWetBuf[(size_t) (bin * 2)];
                const float wIm  = proc.fftWetBuf[(size_t) (bin * 2 + 1)];
                const float dRe  = proc.fftDryBuf[(size_t) (bin * 2)];
                const float dIm  = proc.fftDryBuf[(size_t) (bin * 2 + 1)];
                phaseFrameB[bin].first = (float) bin * binHz;
                const float dMag = std::sqrt (dRe * dRe + dIm * dIm);
                if (dMag < gateThreshold)
                {
                    wrappedPhaseB[bin] = (bin > 0) ? wrappedPhaseB[bin - 1] : 0.0f;
                    continue;
                }
                const float dMagSq = dMag * dMag + 1e-20f;
                const float hRe    = (wRe * dRe + wIm * dIm) / dMagSq;
                const float hIm    = (wIm * dRe - wRe * dIm) / dMagSq;
                wrappedPhaseB[bin]    = std::atan2 (hIm, hRe);
            }

            const std::vector<float> unwrappedB = unwrapPhase (wrappedPhaseB);
            for (int bin = 0; bin < proc.kFreqBins; ++bin)
                phaseFrameB[bin].second = unwrappedB[bin] * radToDeg;

            const juce::ScopedLock lock (proc.resultMutex);
            proc.phaseResponseResultB = phaseFrameB;
        }

        juce::Thread::sleep (20);   // ~50 Hz update rate
    }

    //--------------------------------------------------------------------------
    // Phase 3.5 helper: unwrap a wrapped phase sequence to remove 2π jumps.
    // Sequential correction: each step is clamped to [-π, π] and accumulated.

    static std::vector<float> unwrapPhase (const std::vector<float>& wrapped)
    {
        std::vector<float> unwrapped = wrapped;
        const float pi = juce::MathConstants<float>::pi;
        for (size_t i = 1; i < unwrapped.size(); ++i)
        {
            float diff = unwrapped[i] - unwrapped[i - 1];
            // Wrap diff to [-π, π]
            while (diff >  pi) diff -= 2.0f * pi;
            while (diff < -pi) diff += 2.0f * pi;
            unwrapped[i] = unwrapped[i - 1] + diff;
        }
        return unwrapped;
    }

    //==========================================================================
    // Phase 3.6: Dynamics Analysis Engine helpers
    //
    // These methods run entirely on the analysis background thread.
    // The audio thread is the single producer for captureFifo (juce::AbstractFifo
    // is designed for single-producer + single-consumer, so no additional locking
    // is required for FIFO access on this thread).

    // Flush all available samples from the wet capture FIFO (discard stale output).
    void flushCaptureFifo()
    {
        const int available = proc.captureFifo.getNumReady();
        if (available <= 0) return;
        int s1, sz1, s2, sz2;
        proc.captureFifo.prepareToRead (available, s1, sz1, s2, sz2);
        proc.captureFifo.finishedRead (sz1 + sz2);
    }

    // Plugin B equivalent: flush stale samples from captureFifoB.
    void flushCaptureFifoB()
    {
        const int available = proc.captureFifoB.getNumReady();
        if (available <= 0) return;
        int s1, sz1, s2, sz2;
        proc.captureFifoB.prepareToRead (available, s1, sz1, s2, sz2);
        proc.captureFifoB.finishedRead (sz1 + sz2);
    }

    // Block until the wet capture FIFO contains at least numSamples×2 slots
    // (×2 because the FIFO stores interleaved L+R pairs).
    // Returns immediately if threadShouldExit() becomes true.
    void waitForFifoSamples (int numSamples)
    {
        const int needed = numSamples * 2;
        while (! threadShouldExit())
        {
            if (proc.captureFifo.getNumReady() >= needed)
                return;
            juce::Thread::sleep (5);
        }
    }

    // Read numSamples×2 slots from captureFifoB and return the RMS of the left channel.
    // Mirrors measureRmsFromFifo() but operates on Plugin B's capture buffer.
    float measureRmsFromFifoB (int numSamples)
    {
        const int numSlots = numSamples * 2;
        int s1, sz1, s2, sz2;
        proc.captureFifoB.prepareToRead (numSlots, s1, sz1, s2, sz2);

        float sumSq = 0.0f;
        int count   = 0;

        for (int i = 0; i < sz1; i += 2)
        {
            const float s = proc.captureBufferB.getSample (0, s1 + i);
            sumSq += s * s;
            ++count;
        }
        for (int i = 0; i < sz2; i += 2)
        {
            const float s = proc.captureBufferB.getSample (0, s2 + i);
            sumSq += s * s;
            ++count;
        }

        proc.captureFifoB.finishedRead (sz1 + sz2);

        return (count > 0) ? std::sqrt (sumSq / (float) count) : 0.0f;
    }

    // Read numSamples×2 slots from the wet capture FIFO and return the RMS
    // amplitude of the left channel samples.  The FIFO stores interleaved
    // L/R pairs (even indices = L, odd indices = R), so we step by 2.
    float measureRmsFromFifo (int numSamples)
    {
        const int numSlots = numSamples * 2;
        int s1, sz1, s2, sz2;
        proc.captureFifo.prepareToRead (numSlots, s1, sz1, s2, sz2);

        float sumSq = 0.0f;
        int count   = 0;

        // Region 1 — left channel samples are at even offsets within the region
        for (int i = 0; i < sz1; i += 2)
        {
            const float s = proc.captureBuffer.getSample (0, s1 + i);
            sumSq += s * s;
            ++count;
        }
        // Region 2 (wrap-around)
        for (int i = 0; i < sz2; i += 2)
        {
            const float s = proc.captureBuffer.getSample (0, s2 + i);
            sumSq += s * s;
            ++count;
        }

        proc.captureFifo.finishedRead (sz1 + sz2);

        return (count > 0) ? std::sqrt (sumSq / (float) count) : 0.0f;
    }

    //--------------------------------------------------------------------------
    // Phase 3.6: Full dynamics sweep — runs synchronously on analysis thread.
    //
    // Injects a 1 kHz sine at stepped levels (-60 to 0 dBFS, 1 dB per step).
    // For each step:
    //   1. Set dynamicsSineLevel  → audio thread picks it up next processBlock.
    //   2. Flush stale FIFO samples from the previous level.
    //   3. Wait for settling period (100 ms worth of samples) → flush again.
    //   4. Wait for measurement period (100 ms) → read RMS.
    // Results stored as (input_dbfs, output_dbfs) pairs and published under
    // resultMutex so the UI thread can call getDynamicsResult() safely.

    void computeDynamicsSweep()
    {
        // Re-trigger guard: bail out if a sweep is already in progress.
        if (proc.dynamicsSweepRunning.load())
        {
            juce::Thread::sleep (50);
            return;
        }

        proc.dynamicsSweepRunning.store (true);
        proc.dynamicsSweepProgress.store (0);

        const float sampleRate       = (float) proc.currentSampleRate;
        const int   settlingMs       = 100;
        const int   measurementMs    = 100;
        const int   settlingSamples  = (int) (sampleRate * settlingMs    / 1000.0f);
        const int   measureSamples   = (int) (sampleRate * measurementMs / 1000.0f);

        constexpr float kStartDb = -60.0f;
        constexpr float kEndDb   =   0.0f;
        constexpr float kStepDb  =   1.0f;
        const int numSteps = (int) ((kEndDb - kStartDb) / kStepDb) + 1;   // 61 steps

        std::vector<std::pair<float,float>> results;
        std::vector<std::pair<float,float>> resultsB;
        results.reserve (numSteps);
        resultsB.reserve (numSteps);

        const bool bActive = proc.pluginBReady.load();

        for (int step = 0; step < numSteps && ! threadShouldExit(); ++step)
        {
            // Abort if the user switched tabs or clicked Analyze to restart.
            if ((int) proc.analysisTypeParam->load() != 1
                || proc.analysisResetPending.load())
                break;

            const float inputDb     = kStartDb + (float) step * kStepDb;
            const float inputLinear = juce::Decibels::decibelsToGain (inputDb);

            // Set level — audio thread reads this atomically on next processBlock.
            proc.dynamicsSineLevel.store (inputLinear);

            // Flush stale FIFO samples from both A and B.
            flushCaptureFifo();
            if (bActive) flushCaptureFifoB();

            // Wait for the hosted plugin to settle at the new level, then discard.
            waitForFifoSamples (settlingSamples);
            flushCaptureFifo();
            if (bActive) flushCaptureFifoB();

            // Safety check before measuring.
            if (threadShouldExit()
                || (int) proc.analysisTypeParam->load() != 1
                || proc.analysisResetPending.load())
                break;

            // Accumulate measurement window and compute RMS for both A and B.
            waitForFifoSamples (measureSamples);
            const float outputRmsLinear = measureRmsFromFifo (measureSamples);
            const float outputDb        = juce::Decibels::gainToDecibels (outputRmsLinear + 1e-10f);
            results.push_back ({ inputDb, outputDb });

            if (bActive && proc.captureFifoB.getNumReady() >= measureSamples * 2)
            {
                const float rmsB  = measureRmsFromFifoB (measureSamples);
                const float dbB   = juce::Decibels::gainToDecibels (rmsB + 1e-10f);
                resultsB.push_back ({ inputDb, dbB });
            }

            // Update progress for UI display (0-100).
            proc.dynamicsSweepProgress.store ((int) (100.0f * (float) (step + 1)
                                                               / (float) numSteps));
        }

        // Publish results under the shared result lock.
        {
            const juce::ScopedLock lock (proc.resultMutex);
            proc.dynamicsResult  = results;
            proc.dynamicsResultB = resultsB;
        }

        // Silence the dynamics sine now that the sweep is complete.
        proc.dynamicsSineLevel.store (0.0f);
        proc.dynamicsSweepRunning.store (false);
        proc.dynamicsSweepProgress.store (100);

        // Brief pause before allowing a re-trigger to prevent accidental double-sweep.
        juce::Thread::sleep (500);
    }

    //==========================================================================
    // Phase 3.7: Latency measurement
    //
    // Method A: Direct query via getLatencySamples() — always available when a
    //           plugin is loaded.
    // Method B: Empirical — inject a Dirac impulse via latencyImpulsePending,
    //           scan the wet capture FIFO for the first sample above threshold,
    //           and report that sample index as the measured latency.

    void computeLatencyMeasurement()
    {
        // Method A: direct query (always available)
        if (proc.hostedPlugin != nullptr && proc.pluginReady.load())
        {
            const int samplesA = proc.hostedPlugin->getLatencySamples();
            proc.latencyMethodA.store (samplesA);
            proc.latencyMsA.store ((float) samplesA * 1000.0f
                                   / (float) proc.currentSampleRate);
        }

        // Method B: empirical impulse measurement
        // 1. Flush any stale samples from both capture FIFOs
        {
            auto flushFifo = [] (juce::AbstractFifo& f)
            {
                const int avail = f.getNumReady();
                if (avail > 0)
                {
                    int s1, sz1, s2, sz2;
                    f.prepareToRead (avail, s1, sz1, s2, sz2);
                    f.finishedRead (sz1 + sz2);
                }
            };
            flushFifo (proc.captureFifo);
            flushFifo (proc.captureFifoB);
        }

        // 2. Trigger impulse injection in processBlock (goes through A and B simultaneously)
        proc.latencyImpulsePending.store (true);

        // 3. Wait for up to 500 ms worth of samples to arrive (both FIFOs when B is loaded)
        const bool bLoaded       = proc.pluginBReady.load();
        const int maxWaitSamples = (int) (proc.currentSampleRate * 0.5);
        const int needed         = maxWaitSamples * 2;   // interleaved L+R pairs
        int waited               = 0;
        while (! threadShouldExit() && waited < 100)
        {
            const bool aReady = proc.captureFifo.getNumReady()  >= needed;
            const bool bReady = !bLoaded || proc.captureFifoB.getNumReady() >= needed;
            if (aReady && bReady) break;
            juce::Thread::sleep (5);
            ++waited;
        }

        // Helper: scan a capture FIFO for first sample above threshold and return sample index
        auto scanFifoForPeak = [&] (juce::AbstractFifo& fifo,
                                    juce::AudioBuffer<float>& buf) -> int
        {
            const int ready = fifo.getNumReady();
            if (ready <= 0) return -1;
            int s1, sz1, s2, sz2;
            fifo.prepareToRead (ready, s1, sz1, s2, sz2);
            constexpr float kThreshold = 0.001f;   // -60 dBFS
            int peakIndex = -1;
            for (int i = 0; i < sz1 && peakIndex < 0; i += 2)
                if (std::abs (buf.getSample (0, s1 + i)) > kThreshold)
                    peakIndex = (s1 + i) / 2;
            for (int i = 0; i < sz2 && peakIndex < 0; i += 2)
                if (std::abs (buf.getSample (0, s2 + i)) > kThreshold)
                    peakIndex = (sz1 / 2) + (s2 + i) / 2;
            fifo.finishedRead (sz1 + sz2);
            return peakIndex;
        };

        // 4. Plugin A — empirical scan
        {
            const int peakIndex = scanFifoForPeak (proc.captureFifo, proc.captureBuffer);
            if (peakIndex >= 0)
            {
                proc.latencyMethodB.store (peakIndex);
                proc.latencyMsB.store ((float) peakIndex * 1000.0f
                                       / (float) proc.currentSampleRate);
            }
        }

        // 5. Plugin B — Method A (direct query) + Method B (empirical scan)
        if (bLoaded && proc.hostedPluginB != nullptr)
        {
            // Method A: self-reported
            const int samplesAB = proc.hostedPluginB->getLatencySamples();
            proc.latencyMethodAB.store (samplesAB);
            proc.latencyMsAB.store ((float) samplesAB * 1000.0f
                                    / (float) proc.currentSampleRate);

            // Method B: empirical
            const int peakIndexB = scanFifoForPeak (proc.captureFifoB, proc.captureBufferB);
            if (peakIndexB >= 0)
            {
                proc.latencyMethodBB.store (peakIndexB);
                proc.latencyMsBB.store ((float) peakIndexB * 1000.0f
                                        / (float) proc.currentSampleRate);
            }
        }

        // Rate-limit: don't hammer the FIFO — wait before next measurement
        juce::Thread::sleep (200);
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

    // Phase 3.5: Pre-allocate phase response and group delay result storage
    phaseResponseResult.resize ((size_t) kFreqBins, { 0.0f, 0.0f });
    groupDelayResult   .resize ((size_t) kFreqBins, { 0.0f, 0.0f });

    // Phase 3.6: Reserve dynamics result storage (at most 61 steps: -60dBFS to 0dBFS)
    dynamicsResult.reserve (64);

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

    // Signal audio thread to stop using hosted plugins BEFORE we destroy them
    pluginReady.store (false);
    pluginBReady.store (false);   // Phase 3.7: also stop plugin B

    // Phase 3.3: Stop analysis thread BEFORE destroying buffers it may be reading
    analysisThreadShouldRun.store (false);
    if (analysisThread != nullptr)
        analysisThread->stopThread (3000);

    // Stop scanner thread cleanly (wait up to 3 seconds)
    if (scanThread != nullptr)
        scanThread->stopThread (3000);

    // Phase 3.7: Release plugin B before plugin A.
    // SpinLock ensures the audio thread is not inside processBlock when we destroy.
    {
        const juce::SpinLock::ScopedLockType sl (pluginBLock);
        hostedPluginB.reset();
    }

    // Release hosted plugin on message thread
    {
        const juce::SpinLock::ScopedLockType sl (pluginLock);
        hostedPlugin.reset();
    }
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

    // Phase 3.7: Allocate plugin B scratch buffer and re-prepare if loaded
    hostedOutputBufferB.setSize (2, samplesPerBlock);
    if (hostedPluginB != nullptr && pluginBReady.load())
        hostedPluginB->prepareToPlay (sampleRate, samplesPerBlock);

    // Phase 3.7: Resize and reset plugin B capture buffer + FIFO
    captureBufferB.setSize (2, kCaptureFifoSize);
    captureBufferB.clear();
    captureFifoB.reset();

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

    // Phase 3.6: Reset dynamics sine phase accumulator
    dynamicsSinePhaseAccum = 0.0f;
}

void PluginScopeAudioProcessor::releaseResources()
{
    // Reset FIFOs FIRST — analysis thread checks getNumReady() before touching
    // the backing buffers.  Without resetting, the thread can find stale data in
    // a FIFO whose backing buffer is about to become 0-channel, triggering the
    // jassert(isPositiveAndBelow(channel, numChannels)) at AudioBuffer line 307.
    captureFifo.reset();
    dryCaptureFifo.reset();
    captureFifoB.reset();

    // Release scratch buffers to save memory when plugin is not in use
    hostedInputBuffer .setSize (0, 0);
    hostedOutputBuffer.setSize (0, 0);
    captureBuffer   .setSize (0, 0);
    dryCaptureBuffer.setSize (0, 0);

    // Phase 3.7: Release plugin B buffers
    hostedOutputBufferB.setSize (0, 0);
    captureBufferB.setSize (0, 0);
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
// Phase 3.7: Helper — write hostedOutputBufferB samples into the B capture FIFO.
// Mirrors captureOutputSamples() but uses captureFifoB / captureBufferB.
// Called from processBlock() on the audio thread; must be lock-free.

void PluginScopeAudioProcessor::captureOutputSamplesB (int numSamples)
{
    const int numToWrite = juce::jmin (numSamples,
                                       captureFifoB.getFreeSpace() / 2);
    if (numToWrite <= 0)
        return;

    int start1, size1, start2, size2;
    captureFifoB.prepareToWrite (numToWrite * 2, start1, size1, start2, size2);

    // Write region 1 — interleave L/R into sequential FIFO indices
    for (int i = 0; i < size1 / 2; ++i)
    {
        captureBufferB.setSample (0, start1 + i * 2,     hostedOutputBufferB.getSample (0, i));
        captureBufferB.setSample (0, start1 + i * 2 + 1, hostedOutputBufferB.getSample (1, i));
    }

    // Write region 2 (wrap-around)
    const int offset = size1 / 2;
    for (int i = 0; i < size2 / 2; ++i)
    {
        captureBufferB.setSample (0, start2 + i * 2,     hostedOutputBufferB.getSample (0, offset + i));
        captureBufferB.setSample (0, start2 + i * 2 + 1, hostedOutputBufferB.getSample (1, offset + i));
    }

    captureFifoB.finishedWrite (numToWrite * 2);
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

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getDrySpectrum() const
{
    const juce::ScopedLock lock (resultMutex);
    return drySpectrumResult;
}

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getWetSpectrum() const
{
    const juce::ScopedLock lock (resultMutex);
    return wetSpectrumResult;
}

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getWetSpectrumB() const
{
    const juce::ScopedLock lock (resultMutex);
    return wetSpectrumResultB;
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

std::vector<std::pair<int,float>> PluginScopeAudioProcessor::getThdHarmonicsB() const
{
    const juce::ScopedLock lock (resultMutex);
    return thdHarmonicsB;
}

float PluginScopeAudioProcessor::getThdPercentB() const
{
    const juce::ScopedLock lock (resultMutex);
    return thdPercentB;
}

//==============================================================================
// Phase 3.5: Phase response + group delay accessors — thread-safe reads.
// Both vectors are protected by resultMutex (same lock as all other results).

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getPhaseResponse() const
{
    const juce::ScopedLock lock (resultMutex);
    return phaseResponseResult;
}

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getGroupDelay() const
{
    const juce::ScopedLock lock (resultMutex);
    return groupDelayResult;
}

//==============================================================================
// Phase 3.6: Dynamics result accessor — thread-safe copy of the gain transfer
// function data.  Protected by the shared resultMutex.

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getDynamicsResult() const
{
    const juce::ScopedLock lock (resultMutex);
    return dynamicsResult;
}

//==============================================================================
// Plugin B measurement accessors — mirrors A accessors, reads B result vectors.

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getFreqResponseB() const
{
    const juce::ScopedLock lock (resultMutex);
    return freqResponseResultB;
}

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getPhaseResponseB() const
{
    const juce::ScopedLock lock (resultMutex);
    return phaseResponseResultB;
}

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getDynamicsResultB() const
{
    const juce::ScopedLock lock (resultMutex);
    return dynamicsResultB;
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
    // Phase 3.7: Latency Detection — when analysis_type == 4, inject a Dirac
    // impulse (triggered by latencyImpulsePending set by analysis thread) and
    // capture the result for empirical latency measurement.
    // -----------------------------------------------------------------------
    {
        const int analysisType = static_cast<int> (analysisTypeParam->load());
        if (analysisType == 4)
        {
            hostedInputBuffer.clear();
            if (latencyImpulsePending.load())
            {
                hostedInputBuffer.setSample (0, 0, juce::Decibels::decibelsToGain (-12.0f));
                hostedInputBuffer.setSample (1, 0, juce::Decibels::decibelsToGain (-12.0f));
                latencyImpulsePending.store (false);
            }

            // Plugin A — impulse through A, capture to captureFifo
            {
                const juce::SpinLock::ScopedTryLockType sl (pluginLock);
                if (sl.isLocked() && pluginReady.load() && hostedPlugin != nullptr)
                {
                    const int nCh = hostedOutputBuffer.getNumChannels();
                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                        hostedOutputBuffer.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                    }
                    juce::MidiBuffer emptyMidi;
                    try { hostedPlugin->processBlock (hostedOutputBuffer, emptyMidi); }
                    catch (...) { pluginReady.store (false); }
                    if (pluginReady.load())
                        captureOutputSamples (numSamples);
                }
            }

            // Plugin B — same impulse through B, capture to captureFifoB
            {
                const juce::SpinLock::ScopedTryLockType slB (pluginBLock);
                if (slB.isLocked() && pluginBReady.load() && hostedPluginB != nullptr)
                {
                    const int nCh = hostedOutputBufferB.getNumChannels();
                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                        hostedOutputBufferB.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                    }
                    juce::MidiBuffer emptyMidi;
                    try { hostedPluginB->processBlock (hostedOutputBufferB, emptyMidi); }
                    catch (...) { pluginBReady.store (false); }
                    if (pluginBReady.load())
                        captureOutputSamplesB (numSamples);
                }
            }

            buffer.clear();   // Output silence to DAW
            return;
        }
    }

    // -----------------------------------------------------------------------
    // Mode 4: Live Audio — copy DAW input through the hosted plugin
    // -----------------------------------------------------------------------
    if (testSignalMode == 4)
    {
        // When dynamics sweep is running, inject the controlled test sine just like
        // generated signal modes do — overrides DAW audio for the duration of the sweep.
        const int liveAnalysisType = static_cast<int> (analysisTypeParam->load());
        if (liveAnalysisType == 1)
        {
            // Dynamics: inject controlled sine at the level set by the analysis thread.
            const float level    = dynamicsSineLevel.load();
            const float phaseInc = 2.0f * juce::MathConstants<float>::pi
                                   * 1000.0f / static_cast<float> (currentSampleRate);
            for (int i = 0; i < numSamples; ++i)
            {
                const float sample = std::sin (dynamicsSinePhaseAccum) * level;
                hostedInputBuffer.setSample (0, i, sample);
                hostedInputBuffer.setSample (1, i, sample);
                dynamicsSinePhaseAccum += phaseInc;
                if (dynamicsSinePhaseAccum > juce::MathConstants<float>::twoPi)
                    dynamicsSinePhaseAccum -= juce::MathConstants<float>::twoPi;
            }
        }
        else if (liveAnalysisType == 2)
        {
            // THD: inject a clean 1 kHz sine at -12 dBFS, same as the non-live THD path.
            // Live DAW audio contains energy at harmonic frequencies that would be
            // misread as distortion, so we always use a controlled test tone here.
            const float levelScale = juce::Decibels::decibelsToGain (-12.0f);
            const float phaseInc   = 2.0f * juce::MathConstants<float>::pi
                                     * kThdFundamental / static_cast<float> (currentSampleRate);
            for (int i = 0; i < numSamples; ++i)
            {
                const float sample = std::sin (thdSinePhase) * levelScale;
                hostedInputBuffer.setSample (0, i, sample);
                hostedInputBuffer.setSample (1, i, sample);
                thdSinePhase += phaseInc;
                if (thdSinePhase > juce::MathConstants<float>::twoPi)
                    thdSinePhase -= juce::MathConstants<float>::twoPi;
            }
        }
        else
        {
            // All other analysis types: pass live DAW audio through.
            const int nCh = hostedInputBuffer.getNumChannels();
            for (int ch = 0; ch < nCh; ++ch)
            {
                if (ch < numChannels)
                    hostedInputBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
                else
                    hostedInputBuffer.clear (ch, 0, numSamples);
            }
        }
        captureDrySamples (numSamples);

        const juce::SpinLock::ScopedTryLockType sl (pluginLock);
        if (sl.isLocked() && pluginReady.load() && hostedPlugin != nullptr)
        {
            // Feed hostedInputBuffer into the plugin.
            // In dynamics mode (liveAnalysisType==1) hostedInputBuffer holds the test sine;
            // otherwise it holds the live DAW audio (copied above).
            // Extra plugin channels (beyond DAW channels) are cleared to silence.
            {
                const int nCh = hostedOutputBuffer.getNumChannels();
                for (int ch = 0; ch < nCh; ++ch)
                {
                    if (ch < hostedInputBuffer.getNumChannels())
                        hostedOutputBuffer.copyFrom (ch, 0, hostedInputBuffer, ch, 0, numSamples);
                    else
                        hostedOutputBuffer.clear (ch, 0, numSamples);
                }
            }

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
                // Copy hosted output back to the DAW buffer (only up to DAW channel count)
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    const int srcCh = juce::jmin (ch, hostedOutputBuffer.getNumChannels() - 1);
                    buffer.copyFrom (ch, 0, hostedOutputBuffer, srcCh, 0, numSamples);
                }

                // Capture output for analysis engines
                captureOutputSamples (numSamples);
            }
        }
        else
        {
            // No plugin loaded — mirror DAW input into hostedOutputBuffer so
            // captureOutputSamples reads real audio (dry == wet → flat 0 dB response)
            if (hostedOutputBuffer.getNumChannels() >= 2 &&
                hostedOutputBuffer.getNumSamples() >= numSamples)
            {
                for (int ch = 0; ch < juce::jmin (2, numChannels); ++ch)
                    hostedOutputBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
            }
            captureOutputSamples (numSamples);
        }

        // Plugin B — route same DAW input through B for A/B comparison in Live Audio mode
        {
            const juce::SpinLock::ScopedTryLockType slB (pluginBLock);
            if (slB.isLocked() && pluginBReady.load() && hostedPluginB != nullptr)
            {
                const int nChB = hostedOutputBufferB.getNumChannels();
                for (int ch = 0; ch < nChB; ++ch)
                {
                    if (ch < numChannels)
                        hostedOutputBufferB.copyFrom (ch, 0, hostedInputBuffer, ch, 0, numSamples);
                    else
                        hostedOutputBufferB.clear (ch, 0, numSamples);
                }

                juce::MidiBuffer emptyMidiB;
                try { hostedPluginB->processBlock (hostedOutputBufferB, emptyMidiB); }
                catch (...) { pluginBReady.store (false); }

                if (pluginBReady.load())
                    captureOutputSamplesB (numSamples);
            }
        }

        return;
    }

    // -----------------------------------------------------------------------
    // Phase 3.6: Dynamics Curve override — when analysis_type == 1, inject a
    // controlled 1 kHz sine at the level set by the analysis thread via
    // dynamicsSineLevel (atomic).  The analysis thread steps this level from
    // -60 dBFS to 0 dBFS and measures the RMS output after each settling period.
    // -----------------------------------------------------------------------
    {
        const int analysisType = static_cast<int> (analysisTypeParam->load());
        if (analysisType == 1)  // Dynamics Curve — controlled sine at dynamicsSineLevel
        {
            const float level    = dynamicsSineLevel.load();
            const float phaseInc = 2.0f * juce::MathConstants<float>::pi
                                   * 1000.0f   // 1 kHz
                                   / static_cast<float> (currentSampleRate);

            for (int i = 0; i < numSamples; ++i)
            {
                const float sample = std::sin (dynamicsSinePhaseAccum) * level;
                hostedInputBuffer.setSample (0, i, sample);
                hostedInputBuffer.setSample (1, i, sample);
                dynamicsSinePhaseAccum += phaseInc;
                if (dynamicsSinePhaseAccum > juce::MathConstants<float>::twoPi)
                    dynamicsSinePhaseAccum -= juce::MathConstants<float>::twoPi;
            }

            // Plugin A
            {
                const juce::SpinLock::ScopedTryLockType sl (pluginLock);
                if (sl.isLocked() && pluginReady.load() && hostedPlugin != nullptr)
                {
                    {
                        const int nCh = hostedOutputBuffer.getNumChannels();
                        for (int ch = 0; ch < nCh; ++ch)
                        {
                            const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                            hostedOutputBuffer.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                        }
                    }
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
            }

            // Plugin B — feed the same dynamics sine so B's curve is measured in parallel
            {
                const juce::SpinLock::ScopedTryLockType slB (pluginBLock);
                if (slB.isLocked() && pluginBReady.load() && hostedPluginB != nullptr)
                {
                    const int nChB = hostedOutputBufferB.getNumChannels();
                    for (int ch = 0; ch < nChB; ++ch)
                    {
                        const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                        hostedOutputBufferB.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                    }
                    juce::MidiBuffer emptyMidiB;
                    try
                    {
                        hostedPluginB->processBlock (hostedOutputBufferB, emptyMidiB);
                    }
                    catch (...)
                    {
                        pluginBReady.store (false);
                    }
                    if (pluginBReady.load())
                        captureOutputSamplesB (numSamples);
                }
            }

            buffer.clear();   // Output silence to DAW
            return;           // Skip normal test signal switch
        }
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

            // Route through hosted plugin and capture wet output.
            // If no plugin is loaded, mirror the sine so THD shows 0% (clean passthrough).
            {
                const juce::SpinLock::ScopedTryLockType sl (pluginLock);
                if (sl.isLocked() && pluginReady.load() && hostedPlugin != nullptr)
                {
                    {
                        const int nCh = hostedOutputBuffer.getNumChannels();
                        for (int ch = 0; ch < nCh; ++ch)
                        {
                            const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                            hostedOutputBuffer.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                        }
                    }
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
                else
                {
                    // No plugin loaded — mirror sine directly so THD reads 0% (clean)
                    const int nCh = hostedOutputBuffer.getNumChannels();
                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                        hostedOutputBuffer.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                    }
                    captureOutputSamples (numSamples);
                }
            }

            // Plugin B — same sine through B for A/B THD comparison
            {
                const juce::SpinLock::ScopedTryLockType slB (pluginBLock);
                if (slB.isLocked() && pluginBReady.load() && hostedPluginB != nullptr)
                {
                    const int nChB = hostedOutputBufferB.getNumChannels();
                    for (int ch = 0; ch < nChB; ++ch)
                    {
                        const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                        hostedOutputBufferB.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                    }
                    juce::MidiBuffer emptyMidiB;
                    try { hostedPluginB->processBlock (hostedOutputBufferB, emptyMidiB); }
                    catch (...) { pluginBReady.store (false); }
                    if (pluginBReady.load())
                        captureOutputSamplesB (numSamples);
                }
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
    {
        const juce::SpinLock::ScopedTryLockType sl (pluginLock);
        if (sl.isLocked() && pluginReady.load() && hostedPlugin != nullptr)
        {
            // Phase 3.3: Capture generated test signal as the dry (pre-plugin) reference
            captureDrySamples (numSamples);

            // Copy test signal into hostedOutputBuffer so the plugin receives it as input.
            // hostedInputBuffer is always 2-ch (stereo test signal).  For plugins with
            // more than 2 channels, mirror ch0/ch1 into ch2/ch3 etc., or clear them.
            {
                const int nCh = hostedOutputBuffer.getNumChannels();
                for (int ch = 0; ch < nCh; ++ch)
                {
                    const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                    hostedOutputBuffer.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                }
            }

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
            if (pluginReady.load())
                captureOutputSamples (numSamples);
        }
    }

    // Phase 3.7: A/B mode — also route same test signal through plugin B
    {
        const juce::SpinLock::ScopedTryLockType slB (pluginBLock);
        const int comparisonMode = static_cast<int> (comparisonParam->load());
        if (slB.isLocked() && comparisonMode == 1
            && pluginBReady.load() && hostedPluginB != nullptr)
        {
            {
                const int nCh = hostedOutputBufferB.getNumChannels();
                for (int ch = 0; ch < nCh; ++ch)
                {
                    const int srcCh = juce::jmin (ch, hostedInputBuffer.getNumChannels() - 1);
                    hostedOutputBufferB.copyFrom (ch, 0, hostedInputBuffer, srcCh, 0, numSamples);
                }
            }
            juce::MidiBuffer emptyMidiB;
            try
            {
                hostedPluginB->processBlock (hostedOutputBufferB, emptyMidiB);
            }
            catch (...)
            {
                pluginBReady.store (false);
            }
            if (pluginBReady.load())
                captureOutputSamplesB (numSamples);
        }
    }

    // Output SILENCE to the DAW — PluginScope does not emit test signals downstream
    buffer.clear();
}

//==============================================================================
// resolveVST3Description
//
// Our safe scanner reads metadata only (no DLL load).  For VST3 plugins that
// ship moduleinfo.json (VST3 3.7+) the scanner computes the correct
// uniqueId/deprecatedUid.  Older plugins (no moduleinfo.json) fall back to
// Info.plist; their IDs are set to name.hashCode() which does NOT match
// JUCE's polynomial hash of the factory TUID — causing
// findClassMatchingDescription() to silently reject every class and return
// "Unable to load VST-3 plug-in file".
//
// Fix: at load time (user has clicked Load — not during bulk scan), ask the
// VST3 format to open the bundle and return fresh PluginDescriptions.  These
// come directly from the factory and have correct IDs.  We then match by name
// and substitute the scanned description.
//
// The DLL is already going to be opened by createPluginInstanceAsync(); JUCE's
// RefCountedDllHandle cache means the bundle is only dlopen'd once.
//
static juce::PluginDescription resolveVST3Description (
    juce::AudioPluginFormatManager& mgr,
    const juce::PluginDescription& desc)
{
    if (desc.pluginFormatName != "VST3")
        return desc;

    for (auto* fmt : mgr.getFormats())
    {
        if (fmt->getName() != "VST3") continue;

        juce::OwnedArray<juce::PluginDescription> fresh;
        fmt->findAllTypesForFile (fresh, desc.fileOrIdentifier);

        // Log what the factory actually returned — helps diagnose name mismatches
        juce::Logger::writeToLog ("[PluginScope] resolveVST3: factory returned "
            + juce::String (fresh.size()) + " class(es) for \"" + desc.name + "\"");
        for (auto* d : fresh)
            juce::Logger::writeToLog ("[PluginScope]   factory name=\"" + d->name + "\"");

        // 1. Exact name match
        for (auto* d : fresh)
        {
            if (d->name.trim() == desc.name.trim())
            {
                juce::Logger::writeToLog ("[PluginScope] resolved VST3 IDs for \""
                    + desc.name + "\" (exact): uniqueId=" + juce::String (d->uniqueId)
                    + " deprecatedUid=" + juce::String (d->deprecatedUid));
                return *d;
            }
        }

        // 2. Case-insensitive match — handles capitalisation differences
        for (auto* d : fresh)
        {
            if (d->name.trim().equalsIgnoreCase (desc.name.trim()))
            {
                juce::Logger::writeToLog ("[PluginScope] resolved VST3 IDs for \""
                    + desc.name + "\" (case-insensitive): factory name=\"" + d->name + "\"");
                return *d;
            }
        }

        // 3. Single-plugin bundle — the bundle contains exactly one class so the
        //    name mismatch (e.g. scan cache has "FabFilter Pro-C 2", factory
        //    returns "Pro-C 2") is irrelevant; just use the live factory description.
        if (fresh.size() == 1)
        {
            juce::Logger::writeToLog ("[PluginScope] resolved VST3 IDs for \""
                + desc.name + "\" (single-class fallback): factory name=\""
                + fresh[0]->name + "\"");
            return *fresh[0];
        }

        break;
    }

    juce::Logger::writeToLog ("[PluginScope] WARNING: could not resolve VST3 IDs for \""
        + desc.name + "\" - loading with scanned description");
    return desc;
}

//==============================================================================
void PluginScopeAudioProcessor::loadPlugin (
    const juce::PluginDescription& desc,
    std::function<void(bool success, const juce::String& error)> callback)
{
    // Must be called from the message thread
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    // Stop the audio thread from entering the plugin processing block, then
    // wait until any in-progress processBlock has exited (SpinLock guarantees this).
    pluginReady.store (false);
    {
        const juce::SpinLock::ScopedLockType sl (pluginLock);
        hostedPlugin.reset();
    }

    // Log what we're about to load so failures are diagnosable
    juce::Logger::writeToLog ("[PluginScope] loadPlugin: \"" + desc.name
        + "\" format=" + desc.pluginFormatName
        + " file=" + desc.fileOrIdentifier);

   #if JUCE_MAC
    if (desc.pluginFormatName == "VST3")
    {
        // Recursively remove com.apple.quarantine from the entire VST3 bundle.
        // Quarantine blocks dlopen on macOS; removing it at the bundle root
        // alone is not enough because individual files inside may also carry
        // the attribute.
        const auto& bundlePath = desc.fileOrIdentifier;
        removexattr (bundlePath.toRawUTF8(), "com.apple.quarantine", 0);
        for (auto& child : juce::File (bundlePath)
                               .findChildFiles (juce::File::findFilesAndDirectories, true))
            removexattr (child.getFullPathName().toRawUTF8(), "com.apple.quarantine", 0);
    }
   #endif

    // Resolve the description: for VST3 plugins without moduleinfo.json our
    // safe scanner cannot compute the correct uniqueId/deprecatedUid.  Query
    // the actual factory now (single DLL open, cached by JUCE internally).
    const auto resolvedDesc = resolveVST3Description (formatManager, desc);

    // Load the new plugin asynchronously — required for AUv3 plugins
    formatManager.createPluginInstanceAsync (
        resolvedDesc,
        currentSampleRate,
        currentBlockSize,
        [this, callback, descName = desc.name, descFmt = desc.pluginFormatName]
        (std::unique_ptr<juce::AudioPluginInstance> instance,
                          const juce::String& error)
        {
            // This callback fires on the message thread
            if (instance != nullptr)
            {
                instance->prepareToPlay (currentSampleRate, currentBlockSize);

                // Resize hosted scratch buffers to match the plugin's actual channel
                // count.  Our buffers are pre-allocated as 2-ch stereo but some
                // plugins are mono (1 ch) or multi-channel (4+).  If the buffer
                // passed to processBlock has fewer channels than the plugin expects,
                // JUCE's AU renderGetInput reads channel pointers beyond the buffer
                // array — those slots are null/garbage → memmove to 0x0 → SIGSEGV.
                // pluginReady is still false here so the audio thread is safely gated.
                {
                    const int numCh = juce::jmax (instance->getTotalNumInputChannels(),
                                                  instance->getTotalNumOutputChannels(),
                                                  2);  // Always at least stereo
                    hostedInputBuffer .setSize (numCh, currentBlockSize, false, true, false);
                    hostedOutputBuffer.setSize (numCh, currentBlockSize, false, true, false);
                }

                {
                    const juce::SpinLock::ScopedLockType sl (pluginLock);
                    hostedPlugin = std::move (instance);
                }
                // NOTE: pluginReady is intentionally NOT set here.
                // The editor callback must call activatePlugin() AFTER embedHostedEditor()
                // to prevent createEditor() from racing with processBlock on the audio
                // thread (AU renderGetInput crash at address 0x0).

                if (callback)
                    callback (true, {});
            }
            else
            {
                juce::Logger::writeToLog ("[PluginScope] loadPlugin FAILED: \""
                    + descName + "\" (" + descFmt + ") - " + error);
                if (callback)
                    callback (false, error);
            }
        });
}

void PluginScopeAudioProcessor::unloadPlugin()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    pluginReady.store (false);
    {
        const juce::SpinLock::ScopedLockType sl (pluginLock);
        hostedPlugin.reset();
    }
}

//==============================================================================
// Phase 3.7: Plugin B loading API — mirrors loadPlugin/unloadPlugin for A/B mode.
// Must be called from the message thread only.

void PluginScopeAudioProcessor::loadPluginB (
    const juce::PluginDescription& desc,
    std::function<void(bool, const juce::String&)> callback)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    // Signal audio thread to stop, then wait for any in-progress processBlock to finish
    pluginBReady.store (false);
    {
        const juce::SpinLock::ScopedLockType sl (pluginBLock);
        hostedPluginB.reset();
    }

    const auto resolvedDescB = resolveVST3Description (formatManager, desc);

    formatManager.createPluginInstanceAsync (
        resolvedDescB,
        currentSampleRate,
        currentBlockSize,
        [this, callback] (std::unique_ptr<juce::AudioPluginInstance> instance,
                          const juce::String& error)
        {
            // Callback fires on the message thread
            if (instance != nullptr)
            {
                instance->prepareToPlay (currentSampleRate, currentBlockSize);

                // Resize plugin B scratch buffer to match the plugin's channel count
                // (same reason as for plugin A — prevents AU renderGetInput null crash).
                {
                    const int numCh = juce::jmax (instance->getTotalNumInputChannels(),
                                                  instance->getTotalNumOutputChannels(),
                                                  2);
                    hostedOutputBufferB.setSize (numCh, currentBlockSize, false, true, false);
                }

                {
                    const juce::SpinLock::ScopedLockType sl (pluginBLock);
                    hostedPluginB = std::move (instance);
                }
                pluginBReady.store (true);
                if (callback) callback (true, {});
            }
            else
            {
                if (callback) callback (false, error);
            }
        });
}

void PluginScopeAudioProcessor::unloadPluginB()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    pluginBReady.store (false);
    {
        const juce::SpinLock::ScopedLockType sl (pluginBLock);
        hostedPluginB.reset();
    }
    // Reset B latency results so the UI shows "not measured" until next Analyze
    latencyMethodAB.store (-1);
    latencyMethodBB.store (-1);
    latencyMsAB.store (0.0f);
    latencyMsBB.store (0.0f);
}

//==============================================================================
// Phase 3.7: Before-After snapshot API.
// takeSnapshot() copies the current freqResponseResult → snapshotFreqResponse.
// Both protected by resultMutex so the UI thread can call these safely.

void PluginScopeAudioProcessor::takeSnapshot()
{
    const juce::ScopedLock lock (resultMutex);
    snapshotFreqResponse = freqResponseResult;
    hasSnapshot = true;
}

void PluginScopeAudioProcessor::clearSnapshot()
{
    const juce::ScopedLock lock (resultMutex);
    snapshotFreqResponse.clear();
    hasSnapshot = false;
}

std::vector<std::pair<float,float>> PluginScopeAudioProcessor::getSnapshotFreqResponse() const
{
    const juce::ScopedLock lock (resultMutex);
    return snapshotFreqResponse;
}

bool PluginScopeAudioProcessor::getHasSnapshot() const
{
    const juce::ScopedLock lock (resultMutex);
    return hasSnapshot;
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
