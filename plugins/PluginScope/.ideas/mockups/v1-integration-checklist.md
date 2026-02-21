# Stage 3 (GUI) Integration Checklist — PluginScope v1

**Plugin:** PluginScope
**Mockup Version:** v1
**Generated:** 2026-02-19
**UI Type:** Full-window WebBrowserComponent (no JUCE native widgets)
**Parameters:** 5 x AudioParameterChoice (all WebComboBoxRelay / WebComboBoxParameterAttachment)

---

## Pre-Integration Requirements

- [ ] Stage 2 (Shell) complete: APVTS exists with all 5 AudioParameterChoice parameters
- [ ] Parameter IDs verified in PluginProcessor.cpp match exactly (case-sensitive):
  - `view_mode`
  - `analysis_mode`
  - `analysis_type`
  - `test_signal`
  - `comparison`
- [ ] JUCE 8 or newer confirmed (WebComboBoxRelay requires JUCE 8+)
- [ ] `juce_gui_extra` module is included in CMakeLists.txt

---

## Step 1: Create UI Directory Structure

- [ ] Create directory: `Source/ui/public/`
- [ ] Create directory: `Source/ui/public/js/`
- [ ] Create directory: `Source/ui/public/js/juce/`

```
Source/
  ui/
    public/
      index.html
      js/
        juce/
          index.js
```

---

## Step 2: Copy Production HTML

- [ ] Copy `plugins/PluginScope/.ideas/mockups/v1-ui.html`
      to `Source/ui/public/index.html`
- [ ] Verify the file contains NO viewport units (search for `100vh`, `100vw`, `dvh`, `svh`)
- [ ] Verify `user-select: none` is present in CSS
- [ ] Verify context menu is disabled: `document.addEventListener('contextmenu', ...)`

---

## Step 3: Copy JUCE WebView Frontend Library

- [ ] Locate the JUCE frontend JS library:
      `<JUCE_ROOT>/modules/juce_gui_extra/native/javascript/index.js`
- [ ] Copy to: `Source/ui/public/js/juce/index.js`
- [ ] Verify the file exists and is non-empty

**Note:** This library provides `window.__JUCE__` which enables the C++/JS bridge.
The production HTML (v1-ui.html) references it via the resource provider URL
`js/juce/index.js`. The JUCE frontend library is NOT loaded via a `<script>` tag
in the HTML — it is injected automatically by WebBrowserComponent on JUCE 8.

---

## Step 4: Update PluginEditor.h

- [ ] Open `Source/PluginEditor.h`
- [ ] Use `plugins/PluginScope/.ideas/mockups/v1-PluginEditor-TEMPLATE.h` as reference
- [ ] Verify class name matches processor: `PluginScopeAudioProcessorEditor`
- [ ] Verify member declaration order (CRITICAL — crashes in release build if wrong):
  1. `audioProcessor` reference
  2. Relays (5x `std::unique_ptr<juce::WebComboBoxRelay>`)
  3. `webView` (`std::unique_ptr<juce::WebBrowserComponent>`)
  4. Attachments (5x `std::unique_ptr<juce::WebComboBoxParameterAttachment>`)
- [ ] Add `getResource()` private method declaration
- [ ] Add `handleNativeEvent()` private method declaration

**Member order check (visual inspection):**
```cpp
// Should appear in this exact vertical order in the private section:
std::unique_ptr<juce::WebComboBoxRelay> viewModeRelay;
// ... (other relays)
std::unique_ptr<juce::WebBrowserComponent> webView;
std::unique_ptr<juce::WebComboBoxParameterAttachment> viewModeAttachment;
// ... (other attachments)
```

---

## Step 5: Update PluginEditor.cpp

- [ ] Open `Source/PluginEditor.cpp`
- [ ] Use `plugins/PluginScope/.ideas/mockups/v1-PluginEditor-TEMPLATE.cpp` as reference
- [ ] Verify constructor initializer list order matches declaration order in .h:
  1. `AudioProcessorEditor(&p)` base
  2. `audioProcessor(p)`
  3. Relays (5 relays, in declaration order)
  4. `webView(...)` with `.withOptionsFrom()` for all 5 relays
  5. Attachments (5 attachments, in declaration order)
- [ ] Verify `webView->goToURL("https://pluginscope.localhost/index.html")` in constructor body
- [ ] Verify `setSize(1100, 650)` and `setResizable(true, true)` in constructor body
- [ ] Verify `webView->setBounds(getLocalBounds())` in `resized()`
- [ ] Verify `getResource()` implementation covers `/index.html` and `/js/juce/index.js`
- [ ] Verify resource provider MIME type for `.js` is `"application/javascript"` (not `"text/javascript"`)
- [ ] Implement `handleNativeEvent()` for: `analyzeRequested`, `loadPluginRequested`, `exportRequested`

---

## Step 6: Update CMakeLists.txt

- [ ] Open `CMakeLists.txt`
- [ ] Use `plugins/PluginScope/.ideas/mockups/v1-CMakeLists-SNIPPET.txt` as reference
- [ ] Add `juce_add_binary_data(PluginScope_UIResources ...)` block
      with `Source/ui/public/index.html` and `Source/ui/public/js/juce/index.js`
- [ ] Add `PluginScope_UIResources` to `target_link_libraries()`
- [ ] Verify `juce::juce_gui_extra` is in `target_link_libraries()`
- [ ] Add `JUCE_WEB_BROWSER=1` to `target_compile_definitions()`
- [ ] Add `JUCE_USE_CURL=0` to `target_compile_definitions()`
- [ ] Add `juce_gui_extra` to `NEEDS_JUCE_MODULES` in `juce_add_plugin()` (if not present)

---

## Step 7: Verify BinaryData Symbol Names

After first build, verify symbol names in generated `BinaryData.h`:

- [ ] Open build directory, find `BinaryData.h`
- [ ] Verify `BinaryData::index_html` exists (from `index.html`)
- [ ] Verify `BinaryData::index_html_size` exists
- [ ] Verify JS symbol exists — may be `BinaryData::index_js` or similar
      (path components are stripped and special chars become underscores)
- [ ] Update `getResource()` in PluginEditor.cpp if symbol names differ

---

## Step 8: Debug Build Test

- [ ] Build succeeds (no compile errors, no warnings about missing symbols)
- [ ] Standalone opens (if building standalone target)
- [ ] WebView loads — NOT a blank white/black window
- [ ] PluginScope toolbar is visible with correct dark theme (#1A1A1A background)
- [ ] Right-click on WebView -> "Inspect" opens DevTools (macOS: Command+Option+I)
- [ ] DevTools console shows NO JavaScript errors
- [ ] `window.__JUCE__` exists in DevTools console
- [ ] `window.__JUCE__.backend` is defined

---

## Step 9: Parameter Binding Test

Test each parameter round-trip (UI -> C++ -> UI):

### view_mode (pill toggle: Simplified / Expert)
- [ ] Click "Expert" button -> pill turns blue on "Expert"
- [ ] Open DAW automation -> verify `view_mode` parameter changes
- [ ] Automate `view_mode` to 1 (Expert) -> UI updates to Expert pill active
- [ ] Summary panel hides in Expert mode, shows in Simplified mode

### analysis_mode (pill toggle: Live / Snapshot)
- [ ] Click "Snapshot" button -> pill turns amber
- [ ] Live dot disappears when Snapshot is active
- [ ] Automate `analysis_mode` -> UI updates

### analysis_type (tab bar: 5 tabs)
- [ ] Click each tab -> active pill moves, graph meta label updates
- [ ] Y/X axis labels update correctly per tab
- [ ] Automate `analysis_type` -> active tab updates in WebView

### test_signal (dropdown)
- [ ] Change dropdown -> console logs parameter change
- [ ] Automate `test_signal` -> dropdown selection updates in WebView

### comparison (dropdown)
- [ ] Select "A/B Plugins" -> A/B badges appear, comparison legend shows
- [ ] Select "Off" -> badges hide, legend hides
- [ ] Automate `comparison` -> WebView updates

---

## Step 10: Release Build Test

- [ ] Release build succeeds without warnings
- [ ] Plugin loads in DAW (Logic Pro / Ableton Live / Reaper)
- [ ] WebView displays correctly on first load
- [ ] Close and reopen plugin window 10 times — no crashes
  (This specifically tests member destruction order correctness)
- [ ] Load preset -> all 5 parameters recall correctly and WebView updates
- [ ] Save preset -> values persist after DAW restart

---

## Step 11: Platform-Specific Checks

### macOS
- [ ] AU and VST3 build and scan in Logic Pro
- [ ] WebView uses WKWebView (System WebKit) — visible in Activity Monitor
- [ ] No entitlement errors in Console.app during plugin load

### Windows (if applicable)
- [ ] WebView2 runtime is installed (Microsoft.Web.WebView2)
- [ ] VST3 scans and loads in Ableton/Reaper
- [ ] User data folder for WebView2 is accessible (temp directory)

---

## Parameter Reference

| APVTS ID      | Type   | Options                                                        | Default     | Relay Type             |
|---------------|--------|----------------------------------------------------------------|-------------|------------------------|
| view_mode     | Choice | Simplified (0), Expert (1)                                     | Simplified  | WebComboBoxRelay       |
| analysis_mode | Choice | Live (0), Snapshot (1)                                         | Live        | WebComboBoxRelay       |
| analysis_type | Choice | Frequency Response (0), Dynamics Curve (1), Harmonic Distortion (2), Phase Response (3), Latency (4) | Frequency Response | WebComboBoxRelay |
| test_signal   | Choice | Sine Sweep (0), White Noise (1), Pink Noise (2), Impulse (3), Live Audio (4) | Sine Sweep  | WebComboBoxRelay       |
| comparison    | Choice | Off (0), A/B Plugins (1), Before-After (2)                     | Off         | WebComboBoxRelay       |

**Note:** All 5 relays are `WebComboBoxRelay`. All 5 attachments are `WebComboBoxParameterAttachment`.
There are NO WebSliderRelay or WebToggleButtonRelay in this plugin.

---

## Common Pitfalls

**Blank WebView window:**
- Resource provider returned wrong MIME type or empty resource
- BinaryData symbol name is wrong (check generated BinaryData.h)
- `goToURL()` was not called in constructor body

**Parameter changes not reflected in UI:**
- Relay not registered with `.withOptionsFrom()` in WebView options
- Attachment uses wrong parameter pointer (check APVTS ID string)
- Attachment created before webView in constructor initializer list (wrong order)

**Crash on DAW plugin reload:**
- Member declaration order is wrong in .h (relays not before attachments)
- Only crashes in Release build — always test release build

**JS bridge not working (window.__JUCE__ undefined):**
- `juce_gui_extra` module not linked
- JUCE_WEB_BROWSER=1 not defined
- JUCE frontend library (index.js) not returned by resource provider
