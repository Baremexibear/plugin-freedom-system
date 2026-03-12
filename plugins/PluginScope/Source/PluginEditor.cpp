#include "PluginEditor.h"
#include "BinaryData.h"

//==============================================================================
// Helper: wrap a BinaryData pointer into the Resource vector format.
static std::vector<std::byte> makeByteVector (const char* data, int size)
{
    return std::vector<std::byte> (
        reinterpret_cast<const std::byte*> (data),
        reinterpret_cast<const std::byte*> (data) + size);
}

//==============================================================================
PluginScopeAudioProcessorEditor::PluginScopeAudioProcessorEditor (PluginScopeAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),

      //------------------------------------------------------------------------
      // 1. RELAYS FIRST — create before webView.
      //    Parameter ID strings MUST exactly match APVTS parameter IDs.
      //------------------------------------------------------------------------
      viewModeRelay     (std::make_unique<juce::WebComboBoxRelay> ("view_mode")),
      analysisModeRelay (std::make_unique<juce::WebComboBoxRelay> ("analysis_mode")),
      analysisTypeRelay (std::make_unique<juce::WebComboBoxRelay> ("analysis_type")),
      testSignalRelay   (std::make_unique<juce::WebComboBoxRelay> ("test_signal")),
      comparisonRelay   (std::make_unique<juce::WebComboBoxRelay> ("comparison")),

      //------------------------------------------------------------------------
      // 2. WEBVIEW SECOND — constructed with Options referencing all relays.
      //    .withOptionsFrom() registers each relay with the WebView backend.
      //    Resource provider serves index.html and JS from BinaryData.
      //    Native functions handle custom events from JavaScript.
      //------------------------------------------------------------------------
      webView (std::make_unique<juce::WebBrowserComponent> (
          juce::WebBrowserComponent::Options{}
              .withWinWebView2Options (
                  juce::WebBrowserComponent::Options::WinWebView2{}
                      .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)))
              .withResourceProvider (
                  [this] (const auto& url) { return getResource (url); })
              // Native functions: one per event type emitted by the HTML.
              // The HTML calls window.__JUCE__.backend.emitEvent(eventName, data)
              // which dispatches to the native function registered with that name.
              .withNativeFunction (
                  "analyzeRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "analyzeRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "loadPluginRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "loadPluginRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "unloadPluginRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "unloadPluginRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "loadPluginBRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "loadPluginBRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "unloadPluginBRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "unloadPluginBRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "scanPluginsRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "scanPluginsRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "exportRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "exportRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "snapshotRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "snapshotRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "triggerLatencyRequested",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      juce::var data = args.size() > 0 ? args[0] : juce::var (new juce::DynamicObject());
                      if (auto* obj = data.getDynamicObject())
                          obj->setProperty ("type", "triggerLatencyRequested");
                      handleNativeEvent (data);
                      completion ("ok");
                  })
              .withNativeFunction (
                  "parameterChanged",
                  [this] (const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      // Relay system handles parameter sync automatically.
                      // This native function accepts the call gracefully and ignores it.
                      juce::ignoreUnused (args);
                      completion ("ok");
                  })
              // Register all 5 relays — enables bidirectional parameter sync
              .withOptionsFrom (*viewModeRelay)
              .withOptionsFrom (*analysisModeRelay)
              .withOptionsFrom (*analysisTypeRelay)
              .withOptionsFrom (*testSignalRelay)
              .withOptionsFrom (*comparisonRelay)
      )),

      //------------------------------------------------------------------------
      // 3. ATTACHMENTS LAST — created after relays AND webView exist.
      //    WebComboBoxParameterAttachment(parameter, relay)
      //    Uses processorRef.parameters (NOT processorRef.apvts).
      //------------------------------------------------------------------------
      viewModeAttachment (std::make_unique<juce::WebComboBoxParameterAttachment> (
          *processorRef.parameters.getParameter ("view_mode"),
          *viewModeRelay)),

      analysisModeAttachment (std::make_unique<juce::WebComboBoxParameterAttachment> (
          *processorRef.parameters.getParameter ("analysis_mode"),
          *analysisModeRelay)),

      analysisTypeAttachment (std::make_unique<juce::WebComboBoxParameterAttachment> (
          *processorRef.parameters.getParameter ("analysis_type"),
          *analysisTypeRelay)),

      testSignalAttachment (std::make_unique<juce::WebComboBoxParameterAttachment> (
          *processorRef.parameters.getParameter ("test_signal"),
          *testSignalRelay)),

      comparisonAttachment (std::make_unique<juce::WebComboBoxParameterAttachment> (
          *processorRef.parameters.getParameter ("comparison"),
          *comparisonRelay))
{
    // Add WebView (fills entire window)
    addAndMakeVisible (*webView);

    // Navigate to embedded resource root
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    // Listen for scan progress / completion updates
    processorRef.scanBroadcaster.addChangeListener (this);

    // Default window size (1100 x 650) — resizable
    setSize (1100, 650);
    setResizable (true, true);

    // Enforce min/max resize bounds
    if (auto* constrainer = getConstrainer())
    {
        constrainer->setMinimumSize (900, 520);
        constrainer->setMaximumSize (2560, 1600);
    }

    // Populate plugin list if scan already completed
    pushPluginListToWebView();

    // Start polling timer — pushes measurement data to WebView JS at ~30 Hz
    startTimer (33);
}

//==============================================================================
PluginScopeAudioProcessorEditor::~PluginScopeAudioProcessorEditor()
{
    // Stop timer first — prevents timerCallback() running during teardown
    stopTimer();

    // Stop listening for scan updates before any members are destroyed
    processorRef.scanBroadcaster.removeChangeListener (this);

    // Remove hosted editor before unique_ptr destructs it
    removeHostedEditor();

    // Remove Plugin B editor before unique_ptr destructs it
    removeHostedEditorB();

    // Member destruction order (reverse of declaration):
    //   comparisonAttachment, testSignalAttachment, analysisTypeAttachment,
    //   analysisModeAttachment, viewModeAttachment  (attachments destroyed first)
    //   webView                                      (then webView)
    //   comparisonRelay, testSignalRelay, analysisTypeRelay,
    //   analysisModeRelay, viewModeRelay             (relays destroyed last)
    // No manual reset() calls required.
}

//==============================================================================
void PluginScopeAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Background colour matches CSS --bg: #121212
    // WebView covers the entire bounds; this only shows if WebView hasn't loaded yet.
    g.fillAll (juce::Colour (0xff121212));
}

//==============================================================================
void PluginScopeAudioProcessorEditor::resized()
{
    // WebView fills the entire editor window.
    // HTML/CSS handles all internal layout via percentage-based flexbox.
    webView->setBounds (getLocalBounds());
}

//==============================================================================
// Resource provider — called by WebBrowserComponent when JS requests a URL.
// Uses EXPLICIT URL mapping (Pattern #8) — no generic BinaryData loop.
//
// BinaryData symbol naming:
//   juce_add_binary_data strips path components and replaces . and / with _
//   "index.html"                     -> BinaryData::index_html
//   "js/juce/index.js"               -> BinaryData::index_js
//   "js/juce/check_native_interop.js"-> BinaryData::check_native_interop_js
//
// MIME types:
//   .html  -> "text/html"
//   .js    -> "application/javascript"   (MIME standard, not "text/javascript")
//==============================================================================
std::optional<juce::WebBrowserComponent::Resource>
PluginScopeAudioProcessorEditor::getResource (const juce::String& url)
{
    // Root or explicit index.html request
    if (url == "/" || url == "/index.html")
    {
        return juce::WebBrowserComponent::Resource {
            makeByteVector (BinaryData::index_html, BinaryData::index_htmlSize),
            "text/html"
        };
    }

    // JUCE WebView JS bridge
    if (url == "/js/juce/index.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeByteVector (BinaryData::index_js, BinaryData::index_jsSize),
            "application/javascript"
        };
    }

    // JUCE native interop check script (Pattern #13 — required for bridge stability)
    if (url == "/js/juce/check_native_interop.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeByteVector (BinaryData::check_native_interop_js,
                            BinaryData::check_native_interop_jsSize),
            "application/javascript"
        };
    }

    // Unknown path — return 404
    juce::Logger::writeToLog ("[PluginScope] Resource not found: " + url);
    return std::nullopt;
}

//==============================================================================
// ChangeListener callback — fires on message thread when scan progress updates.
// Pushes plugin list to WebView when scan completes.
//==============================================================================
void PluginScopeAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster* /*source*/)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    pushPluginListToWebView();
}

//==============================================================================
// Push the current plugin list to JavaScript as a JSON array.
// Called after scan completes or when the editor opens with cached results.
//==============================================================================
void PluginScopeAudioProcessorEditor::pushPluginListToWebView()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (!processorRef.scanComplete.load())
    {
        // Scan still in progress — push progress status
        int count = processorRef.scanScannedCount.load();
        juce::String js = "if (typeof handleScanProgress === 'function') handleScanProgress("
                          + juce::String (count) + ");";
        if (webView != nullptr)
            webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
        return;
    }

    // Scan complete — deduplicate by plugin name and build JSON array.
    //
    // The KnownPluginList can contain multiple entries for the same plugin
    // (e.g. both VST3 and AudioUnit versions).  We show only ONE entry per
    // name, choosing the most reliable format on macOS:
    //   AudioUnit  >  VST3
    // AU plugins are native macOS and load via the CoreAudio API, making
    // them far less likely to fail with "Unable to load plug-in file" errors
    // caused by code-signing, quarantine, or architecture mismatches in VST3
    // bundles.  VST3 is used as a fallback only when no AU version exists.

    knownDescs.clear();
    {
        // Format priority: lower index = preferred
        auto formatPriority = [] (const juce::String& fmt) -> int
        {
            if (fmt == "AudioUnit") return 0;
            if (fmt == "VST3")      return 1;
            return 2;
        };

        // Build name -> best description map
        std::map<juce::String, juce::PluginDescription> bestByName;
        for (const auto& desc : processorRef.getKnownPlugins())
        {
            auto it = bestByName.find (desc.name);
            if (it == bestByName.end())
            {
                bestByName[desc.name] = desc;
            }
            else if (formatPriority (desc.pluginFormatName)
                     < formatPriority (it->second.pluginFormatName))
            {
                it->second = desc;  // Replace with higher-priority format
            }
        }

        for (auto& [name, desc] : bestByName)
            knownDescs.add (desc);
    }

    juce::String jsonArray = "[";
    bool first = true;
    for (int i = 0; i < knownDescs.size(); ++i)
    {
        const auto& desc = knownDescs[i];
        if (!first) jsonArray += ",";
        first = false;

        juce::String safeName   = desc.name.replace ("\"", "\\\"");
        juce::String safeFormat = desc.pluginFormatName.replace ("\"", "\\\"");

        jsonArray += "{\"index\":" + juce::String (i)
                   + ",\"name\":\"" + safeName + "\""
                   + ",\"format\":\"" + safeFormat + "\""
                   + "}";
    }
    jsonArray += "]";

    juce::String js = "if (typeof handlePluginListUpdate === 'function') handlePluginListUpdate("
                      + jsonArray + ");";

    if (webView != nullptr)
        webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
}

//==============================================================================
// Native event handler — called when JavaScript fires:
//   window.__JUCE__.backend.emitEvent('pluginscopeEvent', data)
//
// Expected event data format (juce::var object):
//   { "type": "analyzeRequested",    "testSignal": 0, "analysisType": 0 }
//   { "type": "loadPluginRequested", "pluginIndex": N }
//   { "type": "unloadPluginRequested" }
//   { "type": "scanPluginsRequested" }
//   { "type": "exportRequested" }
//   { "type": "parameterChanged",    "id": "view_mode", "index": 0 }
//
// NOTE: "parameterChanged" events are redundant when WebComboBoxParameterAttachment
// is used (the relay handles bidirectional sync automatically), but they are
// handled here gracefully for completeness.
//==============================================================================
void PluginScopeAudioProcessorEditor::handleNativeEvent (const juce::var& eventData)
{
    if (!eventData.isObject())
        return;

    auto* obj = eventData.getDynamicObject();
    if (obj == nullptr)
        return;

    auto type = obj->getProperty ("type").toString();

    if (type == "analyzeRequested")
    {
        // Signal the analysis thread to flush stale capture FIFOs and reset
        // averaging accumulators.  The thread picks this up on its next loop
        // iteration and starts a genuinely fresh measurement from clean state.
        processorRef.triggerAnalysisReset();
        juce::Logger::writeToLog ("[PluginScope] analyzeRequested - reset pending");
    }
    else if (type == "loadPluginRequested")
    {
        // JavaScript requests loading a plugin by index in knownDescs
        // In Phase 4.1, the WebView "Load Plugin..." button fires this with no index —
        // it opens the plugin browser overlay in the native component.
        // If pluginIndex is provided, load that specific plugin.
        auto pluginIndexVar = obj->getProperty ("pluginIndex");
        if (pluginIndexVar.isInt() || pluginIndexVar.isDouble())
        {
            const int pluginIndex = static_cast<int> (pluginIndexVar);
            if (pluginIndex >= 0 && pluginIndex < knownDescs.size())
            {
                const auto desc = knownDescs[pluginIndex];

                // Remove any currently embedded editor
                removeHostedEditor();

                // Notify JS of loading state
                if (webView != nullptr)
                {
                    juce::String loadingJs = "if (typeof handlePluginLoading === 'function') "
                                             "handlePluginLoading(\"" +
                                             desc.name.replace ("\"", "\\\"") + "\");";
                    webView->evaluateJavascript (loadingJs,
                                                 [] (juce::WebBrowserComponent::EvaluationResult) {});
                }

                // Use lifetime sentinel to guard the callback
                auto sentinel = processorRef.processorAlive;

                // Build a helper that tries to load a description and,
                // on failure, retries with any alternative format for the
                // same plugin name found in knownPluginList.
                auto onLoadResult = [this, name = desc.name, sentinel]
                    (bool success, const juce::String& error)
                {
                    if (!sentinel->load()) return;

                    if (success)
                    {
                        // Notify JS immediately so the "loading" spinner clears.
                        if (webView != nullptr)
                        {
                            juce::String js = "if (typeof handlePluginLoaded === 'function') "
                                "handlePluginLoaded(\"" + name.replace ("\"", "\\\"") + "\");";
                            webView->evaluateJavascript (js,
                                [] (juce::WebBrowserComponent::EvaluationResult) {});
                        }

                        // Defer editor window creation to the next event-loop iteration
                        // so createEditor() (which can take seconds) doesn't block the
                        // JS notification above from reaching the WebView.
                        juce::MessageManager::callAsync ([this, sentinel]
                        {
                            if (! sentinel->load()) return;
                            embedHostedEditor();
                            processorRef.activatePlugin();
                        });
                    }
                    else
                    {
                        juce::String js = "if (typeof handlePluginLoadError === 'function') "
                            "handlePluginLoadError(\"" + error.replace ("\"", "\\\"") + "\");";
                        if (webView != nullptr)
                            webView->evaluateJavascript (js,
                                [] (juce::WebBrowserComponent::EvaluationResult) {});
                    }
                };

                // If the preferred format fails, fall back to any other
                // format for the same plugin name (e.g. AU when VST3 fails).
                auto onLoadWithFallback = [this, desc, sentinel,
                                           onLoadResult = std::move (onLoadResult)]
                    (bool success, const juce::String& error) mutable
                {
                    if (success || !sentinel->load()) { onLoadResult (success, error); return; }

                    // Search for an alternative format in the full known list
                    juce::PluginDescription fallback;
                    bool foundFallback = false;
                    for (const auto& p : processorRef.getKnownPlugins())
                    {
                        if (p.name == desc.name && p.pluginFormatName != desc.pluginFormatName)
                        {
                            fallback     = p;
                            foundFallback = true;
                            break;
                        }
                    }

                    if (foundFallback)
                    {
                        juce::Logger::writeToLog ("[PluginScope] " + desc.pluginFormatName
                            + " failed for \"" + desc.name + "\", retrying as "
                            + fallback.pluginFormatName);
                        processorRef.loadPlugin (fallback, std::move (onLoadResult));
                    }
                    else
                    {
                        onLoadResult (false, error);
                    }
                };

                processorRef.loadPlugin (desc, std::move (onLoadWithFallback));
            }
        }
        else
        {
            // No index — just push the current plugin list so the JS can show a picker
            pushPluginListToWebView();
        }
    }
    else if (type == "unloadPluginRequested")
    {
        // Editor MUST be destroyed before the plugin instance (see removeHostedEditor comment)
        removeHostedEditor();
        processorRef.unloadPlugin();

        if (webView != nullptr)
        {
            juce::String js = "if (typeof handlePluginUnloaded === 'function') handlePluginUnloaded();";
            webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
        }
    }
    else if (type == "loadPluginBRequested")
    {
        auto pluginIndexVar = obj->getProperty ("pluginIndex");
        if (! (pluginIndexVar.isInt() || pluginIndexVar.isDouble()))
            return;

        const int pluginIndex = static_cast<int> (pluginIndexVar);
        if (pluginIndex < 0 || pluginIndex >= knownDescs.size())
            return;

        const auto desc = knownDescs[pluginIndex];
        removeHostedEditorB();

        if (webView != nullptr)
        {
            juce::String js = "if(typeof handlePluginBLoading==='function')"
                              "handlePluginBLoading(\""
                              + desc.name.replace ("\"", "\\\"") + "\");";
            webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
        }

        auto sentinel = processorRef.processorAlive;

        processorRef.loadPluginB (desc, [this, name = desc.name, sentinel]
            (bool success, const juce::String& error)
        {
            if (! sentinel->load()) return;

            if (success)
            {
                if (webView != nullptr)
                {
                    juce::String js = "if(typeof handlePluginBLoaded==='function')"
                                      "handlePluginBLoaded(\""
                                      + name.replace ("\"", "\\\"") + "\");";
                    webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
                }

                juce::MessageManager::callAsync ([this, sentinel]
                {
                    if (sentinel->load()) embedHostedEditorB();
                });
            }
            else
            {
                if (webView != nullptr)
                {
                    juce::String js = "if(typeof handlePluginBLoadError==='function')"
                                      "handlePluginBLoadError(\""
                                      + error.replace ("\"", "\\\"") + "\");";
                    webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
                }
            }
        });
    }
    else if (type == "unloadPluginBRequested")
    {
        removeHostedEditorB();
        processorRef.unloadPluginB();

        if (webView != nullptr)
        {
            juce::String js = "if(typeof handlePluginBUnloaded==='function')handlePluginBUnloaded();";
            webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
        }
    }
    else if (type == "scanPluginsRequested")
    {
        juce::Logger::writeToLog ("[PluginScope] scanPluginsRequested received from WebView");

        // Immediately confirm to JS that we received the request
        if (webView != nullptr)
        {
            webView->evaluateJavascript (
                "if (typeof handleScanProgress === 'function') handleScanProgress(0);",
                [] (juce::WebBrowserComponent::EvaluationResult) {});
        }

        processorRef.scanScannedCount.store (0);
        processorRef.scanComplete.store (false);
        processorRef.startPluginScan();
    }
    else if (type == "snapshotRequested")
    {
        processorRef.takeSnapshot();
        juce::Logger::writeToLog ("[PluginScope] snapshotRequested - snapshot taken");
    }
    else if (type == "triggerLatencyRequested")
    {
        processorRef.triggerLatencyMeasurement();
        juce::Logger::writeToLog ("[PluginScope] triggerLatencyRequested - impulse pending");
    }
    else if (type == "exportRequested")
    {
        // Notify JS: export is starting
        if (webView != nullptr)
            webView->evaluateJavascript (
                "if(typeof handleExportStarted==='function')handleExportStarted();",
                [] (juce::WebBrowserComponent::EvaluationResult) {});

        // Open async save dialog.  fileChooser is a member so it stays alive
        // for the entire async lifetime of the dialog.
        auto pluginName = processorRef.getHostedPlugin() != nullptr
                          ? processorRef.getHostedPlugin()->getName()
                          : juce::String ("PluginScope");

        auto safePluginName = pluginName.replaceCharacters (" /\\:*?\"<>|", "_________");
        auto defaultName = safePluginName + "_analysis.csv";

        fileChooser = std::make_unique<juce::FileChooser> (
            "Export Analysis Data",
            juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (defaultName),
            "*.csv");

        auto sentinel = processorRef.processorAlive;

        fileChooser->launchAsync (
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this, sentinel] (const juce::FileChooser& chooser)
            {
                if (! sentinel->load()) return;

                auto result = chooser.getResult();
                if (result == juce::File{})
                {
                    // User cancelled — clear the "exporting" state
                    if (webView != nullptr)
                        webView->evaluateJavascript (
                            "if(typeof handleExportCancelled==='function')handleExportCancelled();",
                            [] (juce::WebBrowserComponent::EvaluationResult) {});
                    return;
                }

                // Add .csv extension if user didn't type one
                auto dest = result.hasFileExtension ("csv") ? result : result.withFileExtension ("csv");
                performExport (dest);
            });
    }
    else if (type == "parameterChanged")
    {
        // Relay system handles this automatically via WebComboBoxParameterAttachment.
        // This branch exists only for debugging / future override use.
        juce::Logger::writeToLog ("[PluginScope] parameterChanged from WebView (relay handles this automatically)");
    }
}

//==============================================================================
void PluginScopeAudioProcessorEditor::embedHostedEditor()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    removeHostedEditor();

    auto* plugin = processorRef.getHostedPlugin();
    if (plugin == nullptr)
        return;

    if (plugin->hasEditor())
    {
        hostedPluginEditor.reset (plugin->createEditor());
        if (hostedPluginEditor == nullptr)
            return;
    }
    else
    {
        // Plugin has no GUI — nothing to show
        return;
    }

    // Show the editor in a separate floating window.
    // We cannot embed it as a child of this editor because WKWebView (the
    // macOS WebKit native view) always paints on top of all JUCE components
    // in the same NSWindow, making any overlay invisible.  A separate
    // DocumentWindow has its own NSWindow and correct z-order.
    hostedEditorWindow = std::make_unique<HostedEditorWindow> (
        plugin->getName(), hostedPluginEditor.get());

    // When the user closes the floating window, unload the plugin and
    // notify the WebView JS so the browser panel updates.
    hostedEditorWindow->onClose = [this]
    {
        // Null out immediately to prevent re-entry
        hostedEditorWindow->onClose = nullptr;

        // 1. Destroy the editor BEFORE the plugin (VST3PluginWindow::~
        //    calls view->removed() which needs a live plugin DLL)
        hostedPluginEditor.reset();

        // 2. Release the plugin instance (safe: editor already gone)
        processorRef.unloadPlugin();

        // 3. Defer window destruction — we are currently executing inside
        //    HostedEditorWindow::closeButtonPressed().  Deleting the window
        //    from within its own call stack causes a stack-use-after-free.
        juce::MessageManager::callAsync ([this]
        {
            hostedEditorWindow.reset();

            if (webView != nullptr)
                webView->evaluateJavascript (
                    "if (typeof handlePluginUnloaded === 'function') handlePluginUnloaded();",
                    [] (juce::WebBrowserComponent::EvaluationResult) {});
        });
    };
}

//==============================================================================
void PluginScopeAudioProcessorEditor::removeHostedEditor()
{
    // Null out the callback first to prevent any re-entrant call during destruction
    if (hostedEditorWindow != nullptr)
        hostedEditorWindow->onClose = nullptr;

    // CRITICAL ORDER: destroy the editor (VST3PluginWindow) BEFORE the window,
    // and BEFORE the plugin instance is unloaded by the caller.
    // VST3PluginWindow::~VST3PluginWindow calls view->removed() which accesses
    // the live plugin DLL.  Destroying the window first orphans the editor and
    // then freeing the plugin causes a null-deref inside the VST3 destructor.
    hostedPluginEditor.reset();

    if (hostedEditorWindow != nullptr)
    {
        hostedEditorWindow->setVisible (false);
        hostedEditorWindow.reset();
    }
}

//==============================================================================
void PluginScopeAudioProcessorEditor::embedHostedEditorB()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    removeHostedEditorB();

    auto* pluginB = processorRef.getHostedPluginB();
    if (pluginB == nullptr || !pluginB->hasEditor())
        return;

    hostedPluginEditorB.reset (pluginB->createEditor());
    if (hostedPluginEditorB == nullptr)
        return;

    hostedEditorWindowB = std::make_unique<HostedEditorWindow> (
        pluginB->getName() + " [B]", hostedPluginEditorB.get());

    hostedEditorWindowB->onClose = [this]
    {
        hostedEditorWindowB->onClose = nullptr;

        hostedPluginEditorB.reset();       // editor before plugin
        processorRef.unloadPluginB();

        juce::MessageManager::callAsync ([this]
        {
            hostedEditorWindowB.reset();

            if (webView != nullptr)
                webView->evaluateJavascript (
                    "if(typeof handlePluginBUnloaded==='function')handlePluginBUnloaded();",
                    [] (juce::WebBrowserComponent::EvaluationResult) {});
        });
    };
}

//==============================================================================
void PluginScopeAudioProcessorEditor::removeHostedEditorB()
{
    if (hostedEditorWindowB != nullptr)
        hostedEditorWindowB->onClose = nullptr;

    hostedPluginEditorB.reset();  // editor before window — same reason as removeHostedEditor()

    if (hostedEditorWindowB != nullptr)
    {
        hostedEditorWindowB->setVisible (false);
        hostedEditorWindowB.reset();
    }
}

//==============================================================================
// timerCallback — fires at ~30 Hz on the message thread.
//
// Reads measurement results from the processor and pushes compact JSON to the
// WebView via evaluateJavascript().  All draw calls happen inside JS handlers
// (handleFreqResponse, handleThdResult, handlePhaseResponse,
//  handleDynamicsResult, handleLatencyResult) defined in index.html.
//
// Data rates:
//   Freq response  — every tick (live chart, ~30 Hz)
//   Snapshot freq  — every tick when snapshot exists
//   Dynamics       — every tick while sweep running; every 500 ms otherwise
//   THD / Phase / Latency — every 500 ms (15 ticks × 33 ms)
//==============================================================================
void PluginScopeAudioProcessorEditor::timerCallback()
{
    if (webView == nullptr)
        return;

    ++timerTick;

    // Fire-and-forget JS evaluation helper
    auto call = [this] (const juce::String& js)
    {
        webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult) {});
    };

    // Build compact [[freq_hz, value], ...] JSON log-spaced across 6-30000 Hz.
    // Uses linear interpolation between adjacent FFT bins so that the many
    // log-spaced output points below ~300 Hz (where bin width > step width)
    // get smoothly varying values rather than staircased plateaus.
    auto logSpacedJson = [] (const std::vector<std::pair<float,float>>& data, int n) -> juce::String
    {
        if (data.empty()) return "null";

        const float minL = std::log10 (6.0f);
        const float maxL = std::log10 (30000.0f);
        juce::String j ("[");
        bool firstPt = true;

        for (int i = 0; i < n; ++i)
        {
            const float f  = std::pow (10.0f, minL + (maxL - minL) * i / (n - 1));
            int lo = 0, hi = (int) data.size() - 1;

            while (lo < hi)
            {
                const int mid = (lo + hi) / 2;
                if (data[(size_t) mid].first < f) lo = mid + 1;
                else                              hi = mid;
            }

            // Linear interpolation between the two bracketing bins.
            // t is clamped to [0,1] so queries above Nyquist (where lo is
            // pinned to the last bin) don't extrapolate into garbage values.
            float db = data[(size_t) lo].second;
            if (lo > 0)
            {
                const float f0 = data[(size_t)(lo - 1)].first;
                const float f1 = data[(size_t) lo       ].first;
                if (f1 > f0)
                {
                    const float t = juce::jlimit (0.0f, 1.0f, (f - f0) / (f1 - f0));
                    db = data[(size_t)(lo - 1)].second * (1.0f - t)
                       + data[(size_t) lo       ].second * t;
                }
            }

            if (!firstPt) j += ",";
            j += "[" + juce::String (f,  0)
               + "," + juce::String (db, 2) + "]";
            firstPt = false;
        }

        return j + "]";
    };

    // === Frequency Response (live — every tick) =============================
    {
        const auto d = processorRef.getFreqResponse();
        if (!d.empty())
            call ("if(typeof handleFreqResponse==='function')"
                  "handleFreqResponse(" + logSpacedJson (d, 256) + ");");
    }

    // === Raw Spectrum — dry (gray) + wet (blue) background display ===========
    {
        const auto dry = processorRef.getDrySpectrum();
        if (!dry.empty())
            call ("if(typeof handleDrySpectrum==='function')"
                  "handleDrySpectrum(" + logSpacedJson (dry, 256) + ");");

        const auto wet = processorRef.getWetSpectrum();
        if (!wet.empty())
            call ("if(typeof handleWetSpectrum==='function')"
                  "handleWetSpectrum(" + logSpacedJson (wet, 256) + ");");
    }

    // === Plugin B Frequency Response + Spectrum (every tick when B loaded) ==
    {
        const auto d = processorRef.getFreqResponseB();
        if (!d.empty())
            call ("if(typeof handleFreqResponseB==='function')"
                  "handleFreqResponseB(" + logSpacedJson (d, 256) + ");");

        const auto wb = processorRef.getWetSpectrumB();
        if (!wb.empty())
            call ("if(typeof handleWetSpectrumB==='function')"
                  "handleWetSpectrumB(" + logSpacedJson (wb, 256) + ");");
    }

    // === Snapshot Frequency Response (every tick while snapshot exists) =====
    if (processorRef.getHasSnapshot())
    {
        const auto d = processorRef.getSnapshotFreqResponse();
        if (!d.empty())
            call ("if(typeof handleSnapshotFreqResponse==='function')"
                  "handleSnapshotFreqResponse(" + logSpacedJson (d, 256) + ");");
    }

    // === Dynamics (every tick while running; every 500 ms at rest) ===========
    const bool dynRunning = processorRef.isDynamicsSweepRunning();
    const bool slowTick   = (timerTick % 15 == 0);

    if (dynRunning || slowTick)
    {
        const auto  pts  = processorRef.getDynamicsResult();
        const int   prog = processorRef.getDynamicsSweepProgress();

        if (!pts.empty() || dynRunning)
        {
            juce::String j ("{\"running\":"   + juce::String (dynRunning ? "true" : "false")
                          + ",\"progress\":"  + juce::String (prog)
                          + ",\"points\":[");
            bool firstPt = true;

            for (const auto& p : pts)
            {
                if (!firstPt) j += ",";
                j += "[" + juce::String (p.first,  1)
                   + "," + juce::String (p.second, 1) + "]";
                firstPt = false;
            }

            j += "]}";
            call ("if(typeof handleDynamicsResult==='function')handleDynamicsResult(" + j + ");");
        }

        // Plugin B dynamics (same cadence — every tick while running, else slow tick)
        {
            const auto ptsB = processorRef.getDynamicsResultB();
            if (!ptsB.empty())
            {
                juce::String jb ("{\"running\":"  + juce::String (dynRunning ? "true" : "false")
                               + ",\"points\":[");
                bool firstPt = true;
                for (const auto& p : ptsB)
                {
                    if (!firstPt) jb += ",";
                    jb += "[" + juce::String (p.first,  1)
                        + "," + juce::String (p.second, 1) + "]";
                    firstPt = false;
                }
                jb += "]}";
                call ("if(typeof handleDynamicsResultB==='function')handleDynamicsResultB(" + jb + ");");
            }
        }
    }

    // === Slow measurements (every ~500 ms) ===================================
    if (slowTick)
    {
        // THD — Plugin A
        {
            const auto  h   = processorRef.getThdHarmonics();
            const float pct = processorRef.getThdPercent();

            if (!h.empty())
            {
                juce::String j ("{\"percent\":" + juce::String (pct, 4)
                              + ",\"harmonics\":[");
                bool firstPt = true;
                for (const auto& p : h)
                {
                    if (!firstPt) j += ",";
                    j += "[" + juce::String ((int) p.first)
                       + "," + juce::String (p.second, 6) + "]";
                    firstPt = false;
                }
                j += "]}";
                call ("if(typeof handleThdResult==='function')handleThdResult(" + j + ");");
            }
        }

        // THD — Plugin B
        {
            const auto  hB   = processorRef.getThdHarmonicsB();
            const float pctB = processorRef.getThdPercentB();

            if (!hB.empty())
            {
                juce::String j ("{\"percent\":" + juce::String (pctB, 4)
                              + ",\"harmonics\":[");
                bool firstPt = true;
                for (const auto& p : hB)
                {
                    if (!firstPt) j += ",";
                    j += "[" + juce::String ((int) p.first)
                       + "," + juce::String (p.second, 6) + "]";
                    firstPt = false;
                }
                j += "]}";
                call ("if(typeof handleThdResultB==='function')handleThdResultB(" + j + ");");
            }
        }

        // Phase Response (A)
        {
            const auto d = processorRef.getPhaseResponse();
            if (!d.empty())
                call ("if(typeof handlePhaseResponse==='function')"
                      "handlePhaseResponse(" + logSpacedJson (d, 256) + ");");
        }

        // Phase Response (B)
        {
            const auto d = processorRef.getPhaseResponseB();
            if (!d.empty())
                call ("if(typeof handlePhaseResponseB==='function')"
                      "handlePhaseResponseB(" + logSpacedJson (d, 256) + ");");
        }

        // Latency
        {
            const int   latA = processorRef.getLatencyMethodA();
            const int   latB = processorRef.getLatencyMethodB();

            if (latA >= 0 || latB >= 0)
            {
                const juce::String j (
                    "{\"methodA\":"  + juce::String (latA)
                  + ",\"methodB\":"  + juce::String (latB)
                  + ",\"msA\":"      + juce::String (processorRef.getLatencyMsA(), 2)
                  + ",\"msB\":"      + juce::String (processorRef.getLatencyMsB(), 2)
                  + ",\"methodAB\":" + juce::String (processorRef.getLatencyMethodAB())
                  + ",\"methodBB\":" + juce::String (processorRef.getLatencyMethodBB())
                  + ",\"msAB\":"     + juce::String (processorRef.getLatencyMsAB(), 2)
                  + ",\"msBB\":"     + juce::String (processorRef.getLatencyMsBB(), 2)
                  + "}");
                call ("if(typeof handleLatencyResult==='function')handleLatencyResult(" + j + ");");
            }
        }
    }
}

//==============================================================================
// performExport — called on message thread after user selects a save path.
//
// Writes a multi-section CSV file:
//   # FREQUENCY RESPONSE   — freq_hz, mag_db_A [, mag_db_B if B loaded]
//   # THD HARMONICS        — harmonic_hz, amplitude_db
//   # PHASE RESPONSE       — freq_hz, phase_deg_A [, phase_deg_B if B loaded]
//   # DYNAMICS             — input_dbfs, output_dbfs_A [, output_dbfs_B if B loaded]
//   # LATENCY              — method_A_samples, method_B_samples, ms_A, ms_B
//
// Empty sections (no data yet measured) are skipped with a note line.
//==============================================================================
void PluginScopeAudioProcessorEditor::performExport (const juce::File& destFile)
{
    juce::String csv;

    auto pluginName = processorRef.getHostedPlugin() != nullptr
                      ? processorRef.getHostedPlugin()->getName()
                      : juce::String ("(no plugin loaded)");

    auto pluginNameB = processorRef.getHostedPluginB() != nullptr
                       ? processorRef.getHostedPluginB()->getName()
                       : juce::String ("");

    const bool hasB = !pluginNameB.isEmpty();

    csv << "# PluginScope Analysis Export\n";
    csv << "# Plugin A: " << pluginName << "\n";
    if (hasB)
        csv << "# Plugin B: " << pluginNameB << "\n";
    csv << "# Date: " << juce::Time::getCurrentTime().toString (true, true) << "\n";
    csv << "\n";

    // ---- Frequency Response ------------------------------------------------
    const auto freqA = processorRef.getFreqResponse();
    const auto freqB = hasB ? processorRef.getFreqResponseB()
                            : std::vector<std::pair<float,float>>{};

    csv << "# FREQUENCY RESPONSE\n";
    if (freqA.empty())
    {
        csv << "# (no frequency response data - run analysis first)\n\n";
    }
    else
    {
        csv << (hasB ? "freq_hz,mag_db_A,mag_db_B\n" : "freq_hz,mag_db\n");

        // Log-space 512 points across 6–30000 Hz using binary search on source
        const int kPoints = 512;
        const float minL  = std::log10 (6.0f);
        const float maxL  = std::log10 (30000.0f);

        for (int i = 0; i < kPoints; ++i)
        {
            const float f  = std::pow (10.0f, minL + (maxL - minL) * i / (kPoints - 1));

            auto nearestDb = [&] (const std::vector<std::pair<float,float>>& data) -> float
            {
                if (data.empty()) return 0.0f;
                int lo = 0, hi = (int) data.size() - 1;
                while (lo < hi)
                {
                    const int mid = (lo + hi) / 2;
                    if (data[(size_t) mid].first < f) lo = mid + 1;
                    else                               hi = mid;
                }
                return data[(size_t) lo].second;
            };

            csv << juce::String (f, 1) << "," << juce::String (nearestDb (freqA), 3);
            if (hasB && !freqB.empty())
                csv << "," << juce::String (nearestDb (freqB), 3);
            csv << "\n";
        }
        csv << "\n";
    }

    // ---- Snapshot Frequency Response ---------------------------------------
    if (processorRef.getHasSnapshot())
    {
        const auto snap = processorRef.getSnapshotFreqResponse();
        csv << "# SNAPSHOT FREQUENCY RESPONSE (before)\n";
        csv << "freq_hz,mag_db\n";

        const int kPoints = 512;
        const float minL  = std::log10 (6.0f);
        const float maxL  = std::log10 (30000.0f);

        for (int i = 0; i < kPoints; ++i)
        {
            const float f = std::pow (10.0f, minL + (maxL - minL) * i / (kPoints - 1));
            int lo = 0, hi = (int) snap.size() - 1;
            while (lo < hi)
            {
                const int mid = (lo + hi) / 2;
                if (snap[(size_t) mid].first < f) lo = mid + 1;
                else                               hi = mid;
            }
            csv << juce::String (f, 1) << "," << juce::String (snap[(size_t) lo].second, 3) << "\n";
        }
        csv << "\n";
    }

    // ---- THD Harmonics -----------------------------------------------------
    const auto harmonics = processorRef.getThdHarmonics();
    const float thdPct   = processorRef.getThdPercent();

    csv << "# THD HARMONICS\n";
    if (harmonics.empty())
    {
        csv << "# (no THD data - switch to Distortion analysis and run)\n\n";
    }
    else
    {
        csv << "# THD: " << juce::String (thdPct * 100.0f, 4) << "%\n";
        csv << "harmonic_hz,amplitude_db\n";
        for (const auto& h : harmonics)
            csv << juce::String (h.first) << "," << juce::String (h.second, 6) << "\n";
        csv << "\n";
    }

    // ---- Phase Response ----------------------------------------------------
    const auto phaseA = processorRef.getPhaseResponse();
    const auto phaseB = hasB ? processorRef.getPhaseResponseB()
                              : std::vector<std::pair<float,float>>{};

    csv << "# PHASE RESPONSE\n";
    if (phaseA.empty())
    {
        csv << "# (no phase data - switch to Phase analysis and run)\n\n";
    }
    else
    {
        csv << (hasB ? "freq_hz,phase_deg_A,phase_deg_B\n" : "freq_hz,phase_deg\n");

        const int kPoints = 512;
        const float minL  = std::log10 (6.0f);
        const float maxL  = std::log10 (30000.0f);

        for (int i = 0; i < kPoints; ++i)
        {
            const float f = std::pow (10.0f, minL + (maxL - minL) * i / (kPoints - 1));

            auto nearestPhase = [&] (const std::vector<std::pair<float,float>>& data) -> float
            {
                if (data.empty()) return 0.0f;
                int lo = 0, hi = (int) data.size() - 1;
                while (lo < hi)
                {
                    const int mid = (lo + hi) / 2;
                    if (data[(size_t) mid].first < f) lo = mid + 1;
                    else                               hi = mid;
                }
                return data[(size_t) lo].second;
            };

            csv << juce::String (f, 1) << "," << juce::String (nearestPhase (phaseA), 2);
            if (hasB && !phaseB.empty())
                csv << "," << juce::String (nearestPhase (phaseB), 2);
            csv << "\n";
        }
        csv << "\n";
    }

    // ---- Dynamics ----------------------------------------------------------
    const auto dynA = processorRef.getDynamicsResult();
    const auto dynB = hasB ? processorRef.getDynamicsResultB()
                           : std::vector<std::pair<float,float>>{};

    csv << "# DYNAMICS (gain transfer function)\n";
    if (dynA.empty())
    {
        csv << "# (no dynamics data - switch to Dynamics analysis and run)\n\n";
    }
    else
    {
        csv << (hasB ? "input_dbfs,output_dbfs_A,output_dbfs_B\n" : "input_dbfs,output_dbfs\n");
        for (size_t i = 0; i < dynA.size(); ++i)
        {
            csv << juce::String (dynA[i].first, 1) << "," << juce::String (dynA[i].second, 2);
            if (hasB && i < dynB.size())
                csv << "," << juce::String (dynB[i].second, 2);
            csv << "\n";
        }
        csv << "\n";
    }

    // ---- Latency -----------------------------------------------------------
    const int   latA  = processorRef.getLatencyMethodA();
    const int   latB2 = processorRef.getLatencyMethodB();
    const float msA   = processorRef.getLatencyMsA();
    const float msB2  = processorRef.getLatencyMsB();

    csv << "# LATENCY\n";
    if (latA < 0 && latB2 < 0)
    {
        csv << "# (no latency data - switch to Latency analysis and run)\n\n";
    }
    else
    {
        csv << "method_A_samples,method_B_samples,ms_A,ms_B\n";
        csv << juce::String (latA) << "," << juce::String (latB2)
            << "," << juce::String (msA, 2) << "," << juce::String (msB2, 2) << "\n\n";
    }

    // ---- Write to file -----------------------------------------------------
    const bool ok = destFile.replaceWithText (csv);

    if (webView != nullptr)
    {
        if (ok)
        {
            auto safePath = destFile.getFullPathName().replace ("\"", "\\\"");
            webView->evaluateJavascript (
                "if(typeof handleExportDone==='function')handleExportDone(\""
                + safePath + "\");",
                [] (juce::WebBrowserComponent::EvaluationResult) {});
        }
        else
        {
            webView->evaluateJavascript (
                "if(typeof handleExportError==='function')"
                "handleExportError('Failed to write file');",
                [] (juce::WebBrowserComponent::EvaluationResult) {});
        }
    }
}
