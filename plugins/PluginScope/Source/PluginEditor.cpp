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
                  [this] (const auto& url) { return getResource (url); },
                  juce::String {"https://pluginscope.localhost/"})
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

    // Add hosted plugin overlay panel (sits on top of WebView left panel area)
    // Initially invisible — shown when a plugin loads
    hostedPluginPanel.setVisible (false);
    addAndMakeVisible (hostedPluginPanel);

    // Navigate to embedded resource root
    webView->goToURL ("https://pluginscope.localhost/index.html");

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

    // Hosted plugin panel occupies left panel area (first 32% of width, matching CSS #plugin-panel)
    // min 240px, max 400px — matches CSS constraints
    const int leftPanelWidth = juce::jlimit (240, 400, (int) (getWidth() * 0.32f));
    const int toolbarHeight  = 52;   // CSS: #toolbar height: 52px
    const int summaryHeight  = 148;  // CSS: #summary-panel height: 148px

    // Hosted plugin panel sits in the left panel below the toolbar
    hostedPluginPanel.setBounds (0, toolbarHeight,
                                 leftPanelWidth,
                                 getHeight() - toolbarHeight - summaryHeight);

    // Resize hosted editor to fill the panel
    if (hostedPluginEditor != nullptr)
        hostedPluginEditor->setBounds (hostedPluginPanel.getLocalBounds());
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
    // Strip query string and leading slash to get the path component
    auto path = juce::URL (url).getSubPath().trimCharactersAtStart ("/");

    // Root or explicit index.html request
    if (path.isEmpty() || path == "index.html")
    {
        return juce::WebBrowserComponent::Resource {
            makeByteVector (BinaryData::index_html, BinaryData::index_htmlSize),
            "text/html"
        };
    }

    // JUCE WebView JS bridge
    if (path == "js/juce/index.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeByteVector (BinaryData::index_js, BinaryData::index_jsSize),
            "application/javascript"
        };
    }

    // JUCE native interop check script (Pattern #13 — required for bridge stability)
    if (path == "js/juce/check_native_interop.js")
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

    // Scan complete — build JSON array and push full list
    knownDescs.clear();
    knownDescs = processorRef.getKnownPlugins();

    juce::String jsonArray = "[";
    bool first = true;
    for (int i = 0; i < knownDescs.size(); ++i)
    {
        const auto& desc = knownDescs[i];
        if (!first) jsonArray += ",";
        first = false;

        // Escape plugin name (remove double-quotes to avoid JSON injection)
        juce::String safeName = desc.name.replace ("\"", "\\\"");
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

                processorRef.loadPlugin (
                    desc,
                    [this, name = desc.name, sentinel] (bool success, const juce::String& error)
                    {
                        // Callback fires on message thread
                        if (!sentinel->load()) return;

                        juce::String js;
                        if (success)
                        {
                            embedHostedEditor();
                            js = "if (typeof handlePluginLoaded === 'function') "
                                 "handlePluginLoaded(\"" + name.replace ("\"", "\\\"") + "\");";
                        }
                        else
                        {
                            js = "if (typeof handlePluginLoadError === 'function') "
                                 "handlePluginLoadError(\"" + error.replace ("\"", "\\\"") + "\");";
                        }

                        if (webView != nullptr)
                            webView->evaluateJavascript (js,
                                                          [] (juce::WebBrowserComponent::EvaluationResult) {});
                    });
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
        if (hostedPluginEditor != nullptr)
        {
            hostedPluginPanel.setVisible (true);
            hostedPluginPanel.addAndMakeVisible (*hostedPluginEditor);
            hostedPluginEditor->setBounds (hostedPluginPanel.getLocalBounds());
            resized(); // Re-layout so hosted editor gets correct size
        }
    }
}

//==============================================================================
void PluginScopeAudioProcessorEditor::removeHostedEditor()
{
    if (hostedPluginEditor != nullptr)
    {
        hostedPluginPanel.removeChildComponent (hostedPluginEditor.get());
        hostedPluginEditor.reset();
    }
    hostedPluginPanel.setVisible (false);
}
