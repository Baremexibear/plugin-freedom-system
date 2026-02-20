cmake_minimum_required(VERSION 3.22)

# NOTE: No project() or find_package(JUCE) here.
# JUCE is added once at the root CMakeLists.txt via add_subdirectory().
# This file is included via the root's auto-discovery loop.

juce_add_plugin(PluginScope
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE PlSc
    FORMATS VST3 AU
    PRODUCT_NAME "PluginScope"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_WEB_BROWSER TRUE   # PATTERN 9: Required for VST3 WebView support
)

# Source files
target_sources(PluginScope
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

# Include paths
target_include_directories(PluginScope
    PRIVATE
        Source
)

# WebView UI Resources — uncommented by gui-agent in Stage 3 (GUI)
# juce_add_binary_data(PluginScope_UIResources
#     HEADER_NAME "BinaryData.h"
#     NAMESPACE   "BinaryData"
#     SOURCES
#         Source/ui/public/index.html
#         Source/ui/public/js/juce/index.js
# )

# Required JUCE modules
target_link_libraries(PluginScope
    PRIVATE
        # PluginScope_UIResources  # Uncommented by gui-agent in Stage 3 (GUI)
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra      # Required for WebBrowserComponent (WebView UI)
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# CRITICAL: Generate JuceHeader.h AFTER target_link_libraries (JUCE 8 Pattern #1)
juce_generate_juce_header(PluginScope)

# Compile definitions
target_compile_definitions(PluginScope
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=1    # Required for WebBrowserComponent
        JUCE_USE_CURL=0       # Avoid libcurl dependency
)
