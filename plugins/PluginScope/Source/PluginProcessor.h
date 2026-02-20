#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

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

    // Accessor for hosted plugin (message thread only)
    juce::AudioPluginInstance* getHostedPlugin() const { return hostedPlugin.get(); }

    // Persistence helpers
    juce::File getScanCacheFile() const;
    juce::File getDeadMansPedalFile() const;

    // Start background scan (called once from constructor)
    void startPluginScan();

    //==========================================================================
    // Phase 3.1: Plugin Hosting Engine — public atomic state

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScopeAudioProcessor)
};
