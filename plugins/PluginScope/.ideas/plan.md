# PluginScope - Implementation Plan

**Date:** 2026-02-19
**Complexity Score:** 5.0 (Complex — capped maximum)
**Strategy:** Phase-based implementation (7 DSP phases + 2 GUI phases)

---

## Complexity Factors

- **Parameters:** 5 parameters (5/5 = 1.0 points)
  - VIEW_MODE, ANALYSIS_MODE, ANALYSIS_TYPE, TEST_SIGNAL, COMPARISON
- **Algorithms:** 10 subsystems = 10.0 (capped contribution)
  - Plugin Hosting Engine (AudioPluginFormatManager + AudioPluginInstance)
  - Plugin Scanner (KnownPluginList + PluginDirectoryScanner)
  - Test Signal Generator (log sine sweep, noise, impulse)
  - FFT Analysis Engine (juce::dsp::FFT + WindowingFunction)
  - Frequency Response Measurement (transfer function)
  - Dynamics Analysis Engine (gain transfer function via level sweep)
  - THD Analysis Engine (harmonic FFT measurement)
  - Phase Response Measurement (complex FFT + unwrapping)
  - Latency Detection (impulse + sample offset)
  - A/B Comparison Engine (dual instance + overlay rendering)
- **Features:** +1 (FFT processing)
- **Additional:** Plain-English Summary Generator, real-time WebView chart rendering
- **Total:** 1.0 + 10.0 + 1.0 = 12.0 (capped at 5.0)

**Note:** PluginScope is the most architecturally complex plugin in this system. It is a plugin-in-plugin host with 9 distinct measurement subsystems and a 4-thread architecture. Score is firmly at the 5.0 maximum.

---

## Stages

- Stage 0: Research & Planning (COMPLETE)
- Stage 1: Foundation — CMakeLists.txt, directory structure
- Stage 2: Shell — APVTS parameters (5 choice parameters)
- Stage 3: DSP — 7 phases (see below)
- Stage 4: GUI — 2 phases (see below)
- Stage 5: Validation — pluginval, presets, changelog

---

## CRITICAL PRE-IMPLEMENTATION GATE

**Before writing any measurement code, Stage 3 Phase 1 MUST establish that plugin hosting works inside a plugin in Logic Pro and Ableton Live.**

If Phase 1 succeeds: proceed with all measurement phases.
If Phase 1 fails: evaluate Fallback (standalone app, VST3-only, or architecture change).

This is the architectural validation gate for the entire project.

---

## Complex Implementation (Score 5.0)

### Stage 3: DSP Phases

---

#### Phase 3.1: Plugin Hosting Foundation (VALIDATION GATE)

**Goal:** Prove that PluginScope can load another VST3/AU plugin internally while running inside Ableton Live and Logic Pro

**Components:**
- AudioPluginFormatManager with VST3 and AU formats
- KnownPluginList + PluginDirectoryScanner on background thread
- AsyncCallback plugin instantiation from KnownPluginList selection
- Hosted plugin prepareToPlay() with PluginScope's sampleRate and blockSize
- Hosted plugin processBlock() routed inside PluginScope's processBlock()
- Atomic pluginReady flag for thread-safe plugin swap
- Basic embedded UI: display hosted plugin's editor inside PluginScope's native left panel
- Output: pass-through mode (hosted plugin's output routed to PluginScope's DAW output)

**Test Criteria:**
- [ ] Plugin loads in Ableton Live (VST3) without crashes
- [ ] Plugin loads in Logic Pro (AU) without crashes
- [ ] Plugin scanner finds and lists installed VST3 plugins
- [ ] Plugin scanner finds and lists installed AU plugins (macOS)
- [ ] Selecting a VST3 plugin from list loads it into the host
- [ ] Selecting an AU plugin from list loads it into the host
- [ ] Hosted plugin's UI renders inside PluginScope's editor panel
- [ ] Hosted plugin processes audio (output audible in DAW)
- [ ] Switching plugins (unload A, load B) works without crashes
- [ ] Hosted plugin crash does not crash PluginScope or the DAW

**Risk:** HIGH — this is the architectural validation gate. If this phase fails, the entire project must be redesigned.

---

#### Phase 3.2: Test Signal Generator

**Goal:** Implement all five test signal types and route them through the hosted plugin

**Components:**
- Logarithmic sine sweep (20Hz–20kHz, configurable duration 2-5s)
- White noise generator (juce::Random per-sample)
- Pink noise generator (Paul Kellet 6-stage IIR pinking filter)
- Dirac impulse (single 1.0 sample, then silence)
- Live Audio mode (route DAW audio through hosted plugin instead of generated signal)
- TEST_SIGNAL parameter switches active generator
- Signal level: -12dBFS peak for all generated signals
- Output buffer capture for measurement engines (lock-free circular buffer)

**Test Criteria:**
- [ ] Sine sweep produces audible frequency sweep through hosted plugin
- [ ] White noise produces flat-spectrum noise through hosted plugin
- [ ] Pink noise produces -3dB/octave spectrum through hosted plugin
- [ ] Impulse produces single-sample burst followed by silence
- [ ] Live Audio passes DAW input through hosted plugin
- [ ] Test signal level is -12dBFS peak (no clipping of hosted plugin input)
- [ ] Test signal switching is clean (no clicks or pops)
- [ ] Output samples correctly captured to analysis buffer

---

#### Phase 3.3: FFT Frequency Response Measurement

**Goal:** Measure frequency response of hosted plugin using log sine sweep + FFT transfer function

**Components:**
- juce::dsp::FFT (order 12, 4096 points)
- juce::dsp::WindowingFunction (Hann for frequency response)
- Dry reference signal capture (unprocessed sweep)
- Wet output capture (processed by hosted plugin)
- Complex FFT of both → complex division → transfer function H(f)
- Magnitude response: |H(f)| in dB per frequency bin
- Running average for live mode (32-frame moving average)
- Snapshot freeze on ANALYSIS_MODE = Snapshot
- Result: array of [frequency_hz, magnitude_db] pairs

**Test Criteria:**
- [ ] Flat-response plugin (gain plugin at 0dB) shows flat frequency curve (±0.5dB)
- [ ] High-shelf EQ shows correct shelf shape and dB value
- [ ] Low-cut filter shows correct rolloff slope and cutoff frequency
- [ ] Live mode updates at ~10-30Hz without UI glitching
- [ ] Snapshot mode freezes correctly and does not update
- [ ] Frequency axis covers 20Hz–20kHz with correct log scaling
- [ ] Amplitude range shows -40dB to +20dB minimum

---

#### Phase 3.4: THD Harmonic Distortion Measurement

**Goal:** Measure total harmonic distortion and individual harmonics via FFT

**Components:**
- Fixed 1kHz sine injection (spectrally pure)
- juce::dsp::FFT with flat-top window (amplitude accuracy)
- Harmonic bin extraction at H1 through H8 (or Nyquist)
- THD% calculation: sqrt(H2^2 + H3^2 + ...) / H1 * 100
- Even/odd harmonic ratio (character indicator)
- THD vs frequency sweep option (100Hz–10kHz fundamental)
- Result: [harmonic_number, amplitude_db] + THD% summary

**Test Criteria:**
- [ ] Clean gain plugin at unity shows < 0.001% THD (noise floor only)
- [ ] Known saturation plugin shows correct harmonic content
- [ ] H2 dominant pattern detected for tube-style saturation
- [ ] H3 dominant pattern detected for transistor-style saturation
- [ ] THD% value matches expected value for reference plugin
- [ ] Frequency sweep produces THD vs. frequency curve

---

#### Phase 3.5: Phase Response Measurement

**Goal:** Measure phase shift and group delay per frequency band

**Components:**
- Log sine sweep → complex FFT of both dry reference and wet output
- Complex spectral division to get transfer function H(f)
- Phase response: atan2(imag, real) per bin
- Phase unwrapping algorithm (sequential difference correction)
- Group delay: -d(phase)/dω (numerical derivative of unwrapped phase)
- Result: [frequency_hz, phase_degrees] and [frequency_hz, group_delay_ms]

**Test Criteria:**
- [ ] Linear-phase EQ shows flat group delay (constant across frequencies)
- [ ] Minimum-phase EQ shows increasing group delay at filter edges
- [ ] All-pass filter shows expected 180° phase shift at crossover frequency
- [ ] Phase unwrapping produces monotonic phase for simple test cases
- [ ] Group delay values are in expected ms range for known plugins

---

#### Phase 3.6: Dynamics Analysis Engine

**Goal:** Measure gain transfer function (compressor/limiter/gate behavior)

**Components:**
- Background thread for level sweep (not audio thread)
- Stepped sine input: -60dBFS to 0dBFS in 1dB increments
- 100ms settling time per level step
- RMS measurement window (100ms) for each step output level
- Lock-free buffer hand-off between audio and analysis threads
- Result: [input_dbfs, output_dbfs] pairs (transfer function curve)
- Attack/release detection: observe transient behavior at level transitions (post-v1 optional)

**Test Criteria:**
- [ ] Gain plugin at 0dB shows 1:1 linear transfer function (diagonal line)
- [ ] Compressor shows bend in curve above threshold (ratio visible)
- [ ] Limiter shows flat ceiling at output level limit
- [ ] Gate shows sharp dropout below threshold
- [ ] Sweep takes < 15 seconds total for -60 to 0 dBFS
- [ ] Sweep runs on background thread (audio thread not blocked)
- [ ] Progress indicator updates during sweep

---

#### Phase 3.7: Latency Detection + A/B Comparison Engine

**Goal:** Implement latency measurement and A/B plugin comparison

**Latency components:**
- Method A: `hostedPlugin->getLatencySamples()` direct query
- Method B: Impulse send → scan output buffer for first peak → calculate sample offset
- Report both values; flag discrepancy if > 5 samples difference
- Convert samples to milliseconds via sampleRate

**A/B Comparison components:**
- COMPARISON = "A/B Plugins": Two plugin instances simultaneously (pluginA, pluginB)
- COMPARISON = "Before-After": MeasurementData snapshot + continuous current measurement
- Both curves rendered in WebView with distinct color pairs (blue vs. orange)
- Optional difference curve (current - snapshot per bin)
- Plugin loading UI for second plugin slot (A/B mode)

**Test Criteria:**
- [ ] Latency query returns correct value for known plugin (e.g., iZotope plugin with reported PDC)
- [ ] Impulse method measures correct sample offset for a known-latency plugin
- [ ] Both A and B plugins load and process independently
- [ ] A/B overlay renders correctly in chart (two distinct curves)
- [ ] Before-After: snapshot freezes correctly; "after" updates continuously
- [ ] Switching comparison mode cleans up correctly (no memory leaks on plugin unload)

---

### Stage 4: GUI Phases

---

#### Phase 4.1: Layout, Chart Rendering, and Basic Navigation

**Goal:** Integrate WebView UI with chart rendering infrastructure and main navigation

**Components:**
- WebView setup with all 5 APVTS parameters wired via relay system
- Three-panel layout: left (hosted plugin UI embed), right (charts + tabs), bottom (plain-English)
- First Launch flow: "Select analysis type" modal on open
- Analysis type tab navigation (Frequency / Dynamics / Distortion / Phase / Latency)
- HTML5 Canvas chart rendering for each analysis type:
  - Frequency: log x-axis 20Hz-20kHz line chart
  - Dynamics: linear dBFS x/y scatter + curve fit
  - Distortion: bar chart for harmonics + THD% display
  - Phase: log x-axis line chart for phase degrees
  - Latency: text display with sample + ms values
- Plugin list panel: searchable list, scroll, select to load
- Color-coded frequency bands (warm lows, cool highs) on spectrum chart
- Contextual hover tooltips ("200Hz region = Low-mid body")

**Test Criteria:**
- [ ] WebView loads with correct plugin size
- [ ] All 5 APVTS parameter choice switches work correctly
- [ ] First launch modal displays and pre-selects parameters on choice
- [ ] Frequency chart renders with correct log x-axis and dB y-axis
- [ ] Dynamics chart renders with dBFS axes
- [ ] Distortion chart renders harmonic bars
- [ ] Phase chart renders with phase degree y-axis
- [ ] Latency view shows text values
- [ ] Plugin list displays scanned plugins with search filter
- [ ] Hover tooltips appear on chart hover

---

#### Phase 4.2: Real-time Data Bridge and Advanced Features

**Goal:** Connect live measurement data from C++ to JavaScript chart rendering; implement expert mode and comparison UI

**Components:**
- Native function calls: C++ → JavaScript measurement data push (JSON arrays)
- Live mode: C++ pushes FFT magnitude array at 10-30Hz
- Chart canvas update on data push (requestAnimationFrame loop)
- Expert mode toggle: collapse plain-English panel, show dB/Hz/phase annotations
- Simplified mode: step indicator ("Step 1 of 3") for guided workflow
- Plain-English summary 3-layer collapsible panels (Musical / Practical / Educational)
- Comparison overlay: render two curves in different colors on same canvas
- Difference curve toggle (for Before-After mode)
- ViewMode toggle (Simplified ↔ Expert) with layout transition
- Snapshot button: trigger freeze from UI
- Export button: placeholder for v2 (disabled with "Coming soon" tooltip)

**Test Criteria:**
- [ ] Live mode: chart updates visually at consistent rate without flickering
- [ ] FFT data from C++ renders correctly as frequency curve in chart
- [ ] Dynamics sweep progress visible during 8-second sweep
- [ ] Expert mode: technical annotations appear on chart; plain-English panel collapses
- [ ] Simplified mode: guided step indicator visible and advances
- [ ] Plain-English summaries populate with relevant text for each analysis type
- [ ] Musical / Practical / Educational layers expand/collapse independently
- [ ] Comparison overlay shows both curves with blue/orange color pair
- [ ] Difference curve can be toggled on/off
- [ ] All performance: no UI freeze or jitter during measurement

---

### Implementation Flow

- Stage 1: Foundation — CMakeLists.txt, project structure
- Stage 2: Shell — APVTS (5 choice parameters: VIEW_MODE, ANALYSIS_MODE, ANALYSIS_TYPE, TEST_SIGNAL, COMPARISON)
- Stage 3: DSP — 7 phases
  - Phase 3.1: Plugin Hosting Foundation (VALIDATION GATE) ← START HERE
  - Phase 3.2: Test Signal Generator
  - Phase 3.3: FFT Frequency Response Measurement
  - Phase 3.4: THD Harmonic Distortion Measurement
  - Phase 3.5: Phase Response Measurement
  - Phase 3.6: Dynamics Analysis Engine
  - Phase 3.7: Latency Detection + A/B Comparison Engine
- Stage 4: GUI — 2 phases
  - Phase 4.1: Layout, Chart Rendering, Navigation
  - Phase 4.2: Real-time Data Bridge and Advanced Features
- Stage 5: Validation — pluginval, edge case testing, changelog

---

## Implementation Notes

### Thread Safety

- `std::atomic<bool> pluginReady`: Audio thread checks before calling `hostedPlugin->processBlock()`
- `std::atomic<bool> scanComplete`: UI thread checks before reading KnownPluginList
- `juce::AbstractFifo` lock-free buffer: Audio thread writes output samples; analysis thread reads
- Plugin swap protocol:
  1. Set `pluginReady = false`
  2. Yield to allow audio thread to see the flag (spin briefly)
  3. Delete old plugin instance
  4. Create and prepare new plugin instance
  5. Set `pluginReady = true`
- NEVER call `hostedPlugin->processBlock()` from message thread
- NEVER allocate memory in processBlock (pre-allocate all buffers in prepareToPlay)

### Performance

- **FFT (4096 pts):** ~1-2ms per transform — run on background analysis thread for sweeps; OK on message thread for live mode
- **Dynamics sweep:** ~8-12 seconds total — MUST run on background thread with progress updates
- **Live FFT update:** Budget 5ms for C++ → JavaScript JSON push at 10-30Hz; use throttled dispatch
- **Canvas rendering:** Use `requestAnimationFrame` loop; avoid direct DOM manipulation in data callbacks
- **Plugin scanner:** Can take 30-90 seconds for 500+ plugin collections on first scan; must show progress indicator and allow cancellation

### Latency

- PluginScope reports `0` latency samples to DAW via `setLatencySamples(0)`
- PluginScope does NOT apply PDC to its own audio buses
- Hosted plugin's latency is the SUBJECT of measurement, not something to compensate

### Denormal Protection

- Use `juce::ScopedNoDenormals` in PluginScope's processBlock()
- Cannot control denormal behavior inside hosted plugin

### CMakeLists.txt Critical Flags

```cmake
juce_add_plugin(PluginScope
    COMPANY_NAME "YourCompany"
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE PlSc
    FORMATS VST3 AU
    PRODUCT_NAME "PluginScope"
    NEEDS_WEB_BROWSER TRUE   # Required for WebView (see juce8-critical-patterns.md #9)
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
)

target_link_libraries(PluginScope PRIVATE
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_gui_basics
    juce::juce_core
)

# REQUIRED for JUCE 8 (see juce8-critical-patterns.md #1)
juce_generate_juce_header(PluginScope)

target_compile_definitions(PluginScope PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
```

### BusesProperties

PluginScope is an effect plugin (audio in → audio out) but its audio buses are for Live Audio passthrough only. It does NOT process the DAW signal in any conventional sense.

```cpp
// In PluginProcessor constructor:
AudioProcessor(BusesProperties()
    .withInput("Input", juce::AudioChannelSet::stereo(), true)
    .withOutput("Output", juce::AudioChannelSet::stereo(), true))
```

### Known Challenges

1. **Plugin-in-plugin hosting sandbox:** Test in Logic Pro and Ableton Live BEFORE implementing any measurement code. This is the critical architectural validation.

2. **AUv3 async instantiation:** Use `createPluginInstanceAsync` always. Some AUv3 plugins (running in separate process) will fail with synchronous instantiation.

3. **Hosted plugin UI embedding:** `hostedPlugin->createEditor()` returns a JUCE Component. This must be added as a child of a native JUCE panel (not inside the WebView). The layout must accommodate variable-size hosted plugin editors.

4. **KnownPluginList serialization:** Persist scan results between sessions to avoid full rescan on every launch. Serialize to `juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)`.

5. **Dead man's pedal file:** Required to prevent PluginScope from repeatedly trying to load plugins that crash during scanning. Must be passed to `PluginDirectoryScanner`.

6. **FFT amplitude calibration:** JUCE FFT output requires normalization by `(fftSize * windowSum)`. The Hann window has sum ≈ 0.5 × fftSize. Failure to normalize produces incorrect dB readings. Validate with a known-amplitude sine wave.

7. **Live mode FFT throttling:** Do not run full FFT on every audio callback. Accumulate samples into an FFT buffer and compute when buffer is full (every fftSize samples). Post results to message thread via timer or async call.

8. **JavaScript native function naming:** For pushing measurement data to WebView, use `juce::WebBrowserComponent::emitEventIfBrowserIsVisible` or the getNativeFunction pattern. Must define a consistent API between C++ and JavaScript for data arrays.

9. **Canvas log x-axis:** Must compute pixel position for log frequency scale manually. Formula: `x = width * log(f / f_min) / log(f_max / f_min)`. No built-in Canvas log axis support.

---

## Duration Estimates

- **Stage 1: Foundation** — 20 min
- **Stage 2: Shell** — 30 min (5 choice parameters, no float ranges)
- **Stage 3: DSP** — 12-20 hours
  - Phase 3.1: Plugin Hosting (GATE) — 3-5 hours (most uncertain; architecture proof-of-concept)
  - Phase 3.2: Test Signal Generator — 1-2 hours
  - Phase 3.3: Frequency Response — 2-3 hours
  - Phase 3.4: THD Measurement — 1-2 hours
  - Phase 3.5: Phase Response — 1-2 hours
  - Phase 3.6: Dynamics Engine — 2-3 hours (background thread complexity)
  - Phase 3.7: Latency + A/B — 2-3 hours
- **Stage 4: GUI** — 6-10 hours
  - Phase 4.1: Layout + Charts — 3-5 hours (Canvas chart rendering is non-trivial)
  - Phase 4.2: Data Bridge + Advanced — 3-5 hours (live mode C++→JS data pipeline)
- **Stage 5: Validation** — 2-3 hours

**Total estimated:** 22-35 hours (significantly more complex than any previous plugin in this system)

---

## References

- Creative brief: `plugins/PluginScope/.ideas/creative-brief.md`
- Parameter spec: `plugins/PluginScope/.ideas/parameter-spec-draft.md`
- DSP architecture: `plugins/PluginScope/.ideas/architecture.md`
- JUCE Critical Patterns: `troubleshooting/patterns/juce8-critical-patterns.md`
- JUCE AudioPluginHost example: `JUCE/extras/AudioPluginHost/`

### Reference Plugins / Similar Work

- **DDMF Plugin Doctor** — Primary reference for plugin hosting architecture and measurement methodology
- **DrumRoulette** — Background thread pattern (file scanning → sample loading) mirrors plugin scanning architecture
- **AngelGrain** — FFT infrastructure reference (uses juce::dsp::FFT); reference for background thread communication pattern
- **GainKnob** — WebView parameter binding reference (standard relay pattern for all parameters)
