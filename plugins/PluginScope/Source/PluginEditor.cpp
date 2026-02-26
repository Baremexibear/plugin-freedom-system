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
}

//==============================================================================
PluginScopeAudioProcessorEditor::~PluginScopeAudioProcessorEditor()
{
    // Stop listening for scan updates before any members are destroyed
    processorRef.scanBroadcaster.removeChangeListener (this);

    // Remove hosted editor before unique_ptr destructs it
    removeHostedEditor();

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
        // Analysis is triggered by parameter changes (analysis_type, test_signal, analysis_mode)
        // which are handled by the relay/attachment system. No additional C++ action needed here
        // for Phase 4.1 (Phase 4.2 will add live data push).
        juce::Logger::writeToLog ("[PluginScope] analyzeRequested from WebView");
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
        processorRef.unloadPlugin();
        removeHostedEditor();

        if (webView != nullptr)
        {
            juce::String js = "if (typeof handlePluginUnloaded === 'function') handlePluginUnloaded();";
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
    else if (type == "exportRequested")
    {
        // Phase 4.2: Implement export. Phase 4.1: placeholder.
        juce::Logger::writeToLog ("[PluginScope] exportRequested — not yet implemented (Phase 4.2)");
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
        processorRef.unloadPlugin();
        hostedEditorWindow.reset();
        hostedPluginEditor.reset();

        if (webView != nullptr)
            webView->evaluateJavascript (
                "if (typeof handlePluginUnloaded === 'function') handlePluginUnloaded();",
                [] (juce::WebBrowserComponent::EvaluationResult) {});
    };
}

//==============================================================================
void PluginScopeAudioProcessorEditor::removeHostedEditor()
{
    if (hostedEditorWindow != nullptr)
    {
        hostedEditorWindow->onClose = nullptr; // prevent re-entrant unloadPlugin
        hostedEditorWindow->setVisible (false);
        hostedEditorWindow.reset();
    }

    hostedPluginEditor.reset();
}
