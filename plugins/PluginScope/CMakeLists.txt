cmake_minimum_required(VERSION 3.22)

# NOTE: No project() or find_package(JUCE) here.
# JUCE is added once at the root CMakeLists.txt via add_subdirectory().
# This file is included via the root's auto-discovery loop.

juce_add_plugin(PluginScope
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE PlSc
    FORMATS VST3 AU Standalone
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

# WebView UI Resources — Phase 4.1 GUI integration
juce_add_binary_data(PluginScope_UIResources
    HEADER_NAME "BinaryData.h"
    NAMESPACE   "BinaryData"
    SOURCES
        Source/ui/public/index.html
        Source/ui/public/js/juce/index.js
        Source/ui/public/js/juce/check_native_interop.js
)

# Required JUCE modules
target_link_libraries(PluginScope
    PRIVATE
        PluginScope_UIResources  # Phase 4.1 GUI integration
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
        # Plugin hosting: enable format support so addDefaultFormatsToManager
        # actually registers VST3PluginFormat and AudioUnitPluginFormat.
        # Without these, JUCE_INTERNAL_HAS_VST3/AU remain 0 and no formats load.
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
        # Workaround for VST3 plugins that check process UID as copy protection.
        # Without this, those plugins refuse to load with "Unable to load" error.
        JUCE_PLUGINHOST_VST3_PROCESS_UID_MISMATCH_WORKAROUND=1
)
