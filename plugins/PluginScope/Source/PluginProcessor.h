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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScopeAudioProcessor)
};
