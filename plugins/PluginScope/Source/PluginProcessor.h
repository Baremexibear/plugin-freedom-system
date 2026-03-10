#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

// Forward declaration for friend
class FrequencyAnalysisThread;

class PluginScopeAudioProcessor : public juce::AudioProcessor
{
public:
    PluginScopeAudioProcessor();
    ~PluginScopeAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    const juce::String getName() const override { return "PluginScope"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    //==========================================================================
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // APVTS — public so PluginEditor can create attachments
    juce::AudioProcessorValueTreeState parameters;

    //==========================================================================
    // Plugin loading API (called from PluginEditor on message thread)

    // Returns all known plugins as a sorted array (call after scanComplete)
    juce::Array<juce::PluginDescription> getKnownPlugins() const;

    // Load a plugin asynchronously (callback fires on message thread when ready or failed)
    void loadPlugin (const juce::PluginDescription& desc,
                     std::function<void(bool success, const juce::String& error)> callback);

    // Unload the currently hosted plugin (message thread only)
    void unloadPlugin();

    // Activate the plugin for audio processing — call this AFTER embedHostedEditor()
    // to ensure createEditor() does not race with the audio thread's processBlock.
    // loadPlugin() leaves pluginReady=false; the editor callback must call this
    // once it has finished embedding the hosted editor UI.
    void activatePlugin() { pluginReady.store (true); }

    // Accessor for hosted plugin (message thread only)
    juce::AudioPluginInstance* getHostedPlugin() const { return hostedPlugin.get(); }

    // Persistence helpers
    juce::File getScanCacheFile() const;
    juce::File getDeadMansPedalFile() const;

    // Start background scan (called once from constructor)
    void startPluginScan();

    //==========================================================================
    // Phase 3.2: Capture buffer accessors (read by analysis thread in Phases 3.3+)
    juce::AbstractFifo&       getCaptureFifo()   { return captureFifo; }
    juce::AudioBuffer<float>& getCaptureBuffer() { return captureBuffer; }

    //==========================================================================
    // Phase 3.3: Thread-safe frequency response result read by UI (Phase 4)
    // Returns (frequency_hz, magnitude_db) pairs for kFreqBins bins.
    std::vector<std::pair<float,float>> getFreqResponse() const;

    // Real-time spectrum (raw FFT magnitude) for the Pro-Q-style background display.
    // drySpectrum = input signal, wetSpectrum = post-plugin signal, both in dBFS.
    std::vector<std::pair<float,float>> getDrySpectrum() const;
    std::vector<std::pair<float,float>> getWetSpectrum() const;

    //==========================================================================
    // Phase 3.4: THD result accessors (thread-safe, protected by resultMutex)
    // Returns (harmonic_number 1..8, amplitude_db) pairs.
    std::vector<std::pair<int,float>> getThdHarmonics() const;
    float getThdPercent() const;

    //==========================================================================
    // Phase 3.5: Phase response accessors (thread-safe, protected by resultMutex)
    // Returns (freq_hz, phase_degrees) and (freq_hz, group_delay_ms) pairs.
    std::vector<std::pair<float,float>> getPhaseResponse() const;
    std::vector<std::pair<float,float>> getGroupDelay() const;

    //==========================================================================
    // Phase 3.6: Dynamics Analysis Engine accessors (thread-safe)
    // Returns (input_dbfs, output_dbfs) pairs for the gain transfer function.
    std::vector<std::pair<float,float>> getDynamicsResult() const;

    //==========================================================================
    // Plugin B measurement accessors — parallel results for A/B comparison view.
    // Each mirrors the corresponding Plugin A accessor but reads from B result storage.
    std::vector<std::pair<float,float>> getFreqResponseB() const;
    std::vector<std::pair<float,float>> getPhaseResponseB() const;
    std::vector<std::pair<float,float>> getDynamicsResultB() const;
    int  getDynamicsSweepProgress() const { return dynamicsSweepProgress.load(); }
    bool isDynamicsSweepRunning()   const { return dynamicsSweepRunning.load(); }

    //==========================================================================
    // Phase 3.7: Latency accessors (thread-safe, atomic reads)
    int   getLatencyMethodA()   const { return latencyMethodA.load(); }
    int   getLatencyMethodB()   const { return latencyMethodB.load(); }
    float getLatencyMsA()       const { return latencyMsA.load(); }
    float getLatencyMsB()       const { return latencyMsB.load(); }
    // Plugin B latency (-1 = not yet measured / no plugin loaded)
    int   getLatencyMethodAB()  const { return latencyMethodAB.load(); }
    int   getLatencyMethodBB()  const { return latencyMethodBB.load(); }
    float getLatencyMsAB()      const { return latencyMsAB.load(); }
    float getLatencyMsBB()      const { return latencyMsBB.load(); }
    void  triggerLatencyMeasurement() { latencyImpulsePending.store (true); }

    // Flush stale capture FIFOs and reset averaging accumulators on the analysis thread.
    // Called by the editor when the user clicks Analyze.
    void  triggerAnalysisReset()      { analysisResetPending.store (true); }

    // Phase 3.7: Plugin B API (message thread only — mirrors loadPlugin/unloadPlugin)
    void loadPluginB (const juce::PluginDescription& desc,
                      std::function<void(bool, const juce::String&)> callback);
    void unloadPluginB();
    juce::AudioPluginInstance* getHostedPluginB() const { return hostedPluginB.get(); }

    // Phase 3.7: Before-After snapshot
    void takeSnapshot();
    void clearSnapshot();
    std::vector<std::pair<float,float>> getSnapshotFreqResponse() const;
    bool getHasSnapshot() const;

    //==========================================================================
    // Phase 3.1: Plugin Hosting Engine — public atomic state

    // Lifetime sentinel: shared_ptr shared with all callAsync lambdas.
    // Destructor sets this to false first; lambdas check it before touching *this.
    std::shared_ptr<std::atomic<bool>> processorAlive { std::make_shared<std::atomic<bool>>(true) };

    // Thread safety: UI reads knownPluginList only after scan completes
    std::atomic<bool> scanComplete { false };

    // Scan progress notifier (broadcast on message thread when scan progress updates)
    juce::ChangeBroadcaster scanBroadcaster;
    std::atomic<int> scanProgressPercent { 0 };
    std::atomic<int> scanTotalPlugins    { 0 };
    std::atomic<int> scanScannedCount   { 0 };

    //==========================================================================
    // Format manager — public so PluginScanThread can access it
    juce::AudioPluginFormatManager formatManager;

    // Known plugin list — populated by scanner, persisted to disk
    juce::KnownPluginList knownPluginList;

private:
    //==========================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Phase 3.2: Write numSamples from hostedOutputBuffer into the lock-free capture FIFO
    void captureOutputSamples (int numSamples);

    // Cached atomic parameter value pointers (set in prepareToPlay for efficiency)
    // All 5 parameters are AudioParameterChoice — values are stored as float indices
    std::atomic<float>* viewModeParam     { nullptr };
    std::atomic<float>* analysisModeParam { nullptr };
    std::atomic<float>* analysisTypeParam { nullptr };
    std::atomic<float>* testSignalParam   { nullptr };
    std::atomic<float>* comparisonParam   { nullptr };

    //==========================================================================
    // Phase 3.1: Plugin Hosting Engine — private state

    // Currently hosted plugin instance
    std::unique_ptr<juce::AudioPluginInstance> hostedPlugin;

    // Thread safety: audio thread checks pluginReady before calling hostedPlugin->processBlock()
    std::atomic<bool> pluginReady { false };

    // Background scan thread
    std::unique_ptr<juce::Thread> scanThread;

    // Scratch buffers for hosted plugin (pre-allocated in prepareToPlay, never in processBlock)
    juce::AudioBuffer<float> hostedInputBuffer;
    juce::AudioBuffer<float> hostedOutputBuffer;

    // Cached sample rate and block size (set in prepareToPlay, read by plugin loading code)
    double currentSampleRate { 44100.0 };
    int    currentBlockSize  { 512 };

    //==========================================================================
    // Phase 3.2: Test Signal Generator state
    // (all pre-allocated, no heap allocation in processBlock)

    // Logarithmic sine sweep state
    float sweepPhase    { 0.0f };
    float sweepPosition { 0.0f };   // [0, 1] across full sweep duration
    static constexpr float kSweepDurationSeconds = 3.0f;
    static constexpr float kSweepFStart          = 20.0f;
    static constexpr float kSweepFEnd            = 20000.0f;

    // Pink noise state (Paul Kellet 6-stage IIR)
    float pinkB0 { 0.0f }, pinkB1 { 0.0f }, pinkB2 { 0.0f },
          pinkB3 { 0.0f }, pinkB4 { 0.0f }, pinkB5 { 0.0f }, pinkB6 { 0.0f };
    juce::Random noiseRandom;

    // Impulse state
    bool impulsePending        { true };  // true = next processBlock fires the impulse
    int  previousTestSignalMode { -1 };   // For detecting test signal type changes

    // Phase 3.2: Lock-free output capture buffer (audio thread -> analysis thread)
    // Sized for 1 full second at the highest likely sample rate (192kHz x 2ch)
    static constexpr int kCaptureFifoSize = 192000 * 2;   // samples (interleaved stereo)
    juce::AbstractFifo       captureFifo   { kCaptureFifoSize };
    juce::AudioBuffer<float> captureBuffer;   // Sized in prepareToPlay

    //==========================================================================
    // Phase 3.3: Dry reference capture (parallel to captureBuffer, same FIFO size)
    juce::AbstractFifo       dryCaptureFifo   { kCaptureFifoSize };
    juce::AudioBuffer<float> dryCaptureBuffer;   // Sized in prepareToPlay

    // Phase 3.3: Write numSamples from hostedInputBuffer into the dry capture FIFO
    void captureDrySamples (int numSamples);

    // Phase 3.3: Read numSamples mono samples (left channel) from a FIFO into a float buffer
    void readFifoIntoBuffer (juce::AbstractFifo& fifo, juce::AudioBuffer<float>& srcBuf,
                             float* dest, int numSamples);

    //==========================================================================
    // Phase 3.3: FFT Frequency Response Analysis Engine

    static constexpr int kFftOrder  = 12;            // 2^12 = 4096 points
    static constexpr int kFftSize   = 1 << kFftOrder; // 4096
    static constexpr int kFreqBins  = kFftSize / 2;   // 2048 output bins

    juce::dsp::FFT                      fft    { kFftOrder };
    juce::dsp::WindowingFunction<float> window {
        static_cast<size_t> (kFftSize),
        juce::dsp::WindowingFunction<float>::hann
    };

    // Complex FFT work buffers — accessed ONLY by the analysis thread, no locking needed
    std::vector<float> fftDryBuf;   // kFftSize * 2 floats (interleaved Re/Im)
    std::vector<float> fftWetBuf;   // kFftSize * 2 floats (interleaved Re/Im)

    // Frequency response result — protected by resultMutex
    std::vector<std::pair<float,float>> freqResponseResult;   // (freq_hz, mag_db) per bin
    std::vector<std::pair<float,float>> freqResponseAccum;    // Running average accumulator
    int freqResponseFrameCount { 0 };

    // Raw spectrum (dBFS) for background spectrum display — protected by resultMutex
    std::vector<std::pair<float,float>> drySpectrumResult;
    std::vector<std::pair<float,float>> wetSpectrumResult;
    std::vector<std::pair<float,float>> drySpectrumAccum;
    std::vector<std::pair<float,float>> wetSpectrumAccum;

    // Plugin B freq response result — computed alongside A when B is loaded
    std::vector<std::pair<float,float>> freqResponseResultB;
    std::vector<std::pair<float,float>> freqResponseAccumB;
    int freqResponseFrameCountB { 0 };

    mutable juce::CriticalSection resultMutex;

    // Background analysis thread
    std::unique_ptr<juce::Thread> analysisThread;
    std::atomic<bool> analysisThreadShouldRun { false };

    //==========================================================================
    // Phase 3.4: THD Harmonic Distortion Measurement

    // Flat-top window for THD (amplitude accuracy over frequency resolution)
    juce::dsp::WindowingFunction<float> windowFlatTop {
        static_cast<size_t> (kFftSize),
        juce::dsp::WindowingFunction<float>::flatTop
    };

    // Internal 1 kHz sine generator for THD injection (independent of test_signal param)
    float thdSinePhase { 0.0f };
    static constexpr float kThdFundamental = 1000.0f;   // Hz

    // THD results (protected by resultMutex, same lock as freqResponseResult)
    std::vector<std::pair<int,float>> thdHarmonics;   // (harmonic_number 1..8, amplitude_db)
    float thdPercent { 0.0f };

    //==========================================================================
    // Phase 3.5: Phase Response + Group Delay results (protected by resultMutex)
    std::vector<std::pair<float,float>> phaseResponseResult;  // (freq_hz, phase_degrees)
    std::vector<std::pair<float,float>> groupDelayResult;     // (freq_hz, group_delay_ms)

    // Plugin B phase response result (protected by resultMutex)
    std::vector<std::pair<float,float>> phaseResponseResultB;

    //==========================================================================
    // Phase 3.7: Latency Detection

    // Triggers impulse injection in processBlock when analysis_type == 4
    std::atomic<bool> latencyImpulsePending { false };

    // Set by message thread when user clicks Analyze.
    // The analysis thread picks it up, flushes stale FIFOs, and resets
    // averaging accumulators so the next measurement starts fresh.
    std::atomic<bool> analysisResetPending { false };

    // Latency results — written exclusively by analysis thread, read by UI via atomics
    std::atomic<int>   latencyMethodA  { 0 };    // Plugin A: getLatencySamples()
    std::atomic<int>   latencyMethodB  { 0 };    // Plugin A: empirical impulse
    std::atomic<float> latencyMsA      { 0.0f };
    std::atomic<float> latencyMsB      { 0.0f };
    // Plugin B latency (-1 = not yet measured)
    std::atomic<int>   latencyMethodAB { -1 };   // Plugin B: getLatencySamples()
    std::atomic<int>   latencyMethodBB { -1 };   // Plugin B: empirical impulse
    std::atomic<float> latencyMsAB     { 0.0f };
    std::atomic<float> latencyMsBB     { 0.0f };

    // Phase 3.7: A/B Plugin Hosting
    std::unique_ptr<juce::AudioPluginInstance> hostedPluginB;
    std::atomic<bool> pluginBReady { false };
    juce::AudioBuffer<float> hostedOutputBufferB;  // sized in prepareToPlay

    // Second capture FIFO for plugin B output (same size as primary)
    juce::AbstractFifo       captureFifoB { kCaptureFifoSize };
    juce::AudioBuffer<float> captureBufferB;

    // Phase 3.7: captureOutputSamplesB helper — mirrors captureOutputSamples but uses B buffers
    void captureOutputSamplesB (int numSamples);

    // SpinLocks — prevent processBlock from using a plugin pointer that the message
    // thread is concurrently resetting.  Audio thread uses ScopedTryLockType (never
    // blocks), message thread uses ScopedLockType (waits for audio thread to finish).
    juce::SpinLock pluginLock;
    juce::SpinLock pluginBLock;

    // Phase 3.7: Before-After snapshot storage (protected by resultMutex)
    std::vector<std::pair<float,float>> snapshotFreqResponse;
    bool hasSnapshot { false };

    //==========================================================================
    // Phase 3.6: Dynamics Analysis Engine

    // Linear amplitude of 1kHz sine injected in dynamics mode.
    // Written by analysis thread, read by audio thread — must be atomic.
    std::atomic<float> dynamicsSineLevel { 0.0f };

    // Phase accumulator for 1kHz sine — audio thread only, no atomic needed.
    float dynamicsSinePhaseAccum { 0.0f };

    // Sweep progress (0-100) and running flag — read by UI thread via accessors.
    std::atomic<int>  dynamicsSweepProgress { 0 };
    std::atomic<bool> dynamicsSweepRunning  { false };

    // Dynamics gain transfer function result — protected by resultMutex.
    std::vector<std::pair<float,float>> dynamicsResult;   // (input_dbfs, output_dbfs)
    std::vector<std::pair<float,float>> dynamicsResultB;  // Plugin B parallel result

    // FrequencyAnalysisThread accesses private members directly
    friend class FrequencyAnalysisThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScopeAudioProcessor)
};
