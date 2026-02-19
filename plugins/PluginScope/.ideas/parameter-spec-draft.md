# Parameter Specification (Draft)

**Status:** Draft - Awaiting UI mockup for full specification
**Created:** 2026-02-19
**Source:** Auto-generated from creative brief during ideation

This is a lightweight specification to enable parallel DSP research.
Full specification will be generated from finalized UI mockup.

## Parameters

### viewMode
- **Type:** Choice
- **Options:** Simplified, Expert
- **Default:** Simplified
- **DSP Purpose:** Controls the level of detail shown in the analysis UI. Simplified mode shows guided steps, summary dashboard, and plain-English summaries. Expert mode reveals full technical overlays with precise dB values, Hz markers, and phase degree readouts.

### analysisMode
- **Type:** Choice
- **Options:** Live, Snapshot
- **Default:** Live
- **DSP Purpose:** Determines how measurements are updated. Live mode continuously re-runs FFT and measurement routines on the hosted plugin's output buffer. Snapshot mode freezes the current measurement state for comparison or export.

### analysisType
- **Type:** Choice
- **Options:** Frequency Response, Dynamics Curve, Harmonic Distortion, Phase Response, Latency
- **Default:** Frequency Response
- **DSP Purpose:** Selects the active measurement view. Each type uses a different test signal and analysis algorithm — FFT for frequency/phase, level sweep for dynamics, harmonic FFT for distortion, sample offset for latency.

### testSignal
- **Type:** Choice
- **Options:** Sine Sweep, White Noise, Pink Noise, Impulse, Live Audio
- **Default:** Sine Sweep
- **DSP Purpose:** The signal injected into the hosted plugin for measurement. Sine sweep (log 20Hz–20kHz) for frequency and phase. Level-swept sine for dynamics curve. Fixed-frequency sine for THD. Dirac impulse for impulse response. Live Audio passes the DAW's real-time audio through the hosted plugin.

### comparison
- **Type:** Choice
- **Options:** Off, A/B Plugins, Before-After
- **Default:** Off
- **DSP Purpose:** Controls the overlay comparison mode. Off shows single-plugin measurements. A/B Plugins loads two hosted plugins and renders their measurements overlaid in contrasting colors. Before-After snapshots the current state, allows parameter changes, and overlays the pre/post measurements.

## Technical Notes

- PluginScope is an analyzer, not an audio processor — these parameters control the analysis engine and UI state, not audio signal processing
- The hosted plugin (loaded by the user) handles all audio processing; PluginScope measures its input/output relationship
- Plugin hosting requires JUCE AudioPluginInstance API — VST3, AU, VST2 (SDK-dependent)
- Test signal generation and FFT analysis use JUCE dsp:: module
- Dynamics curve: sweep input level from -60dBFS to 0dBFS, measure output to construct gain reduction curve
- THD measurement: inject sine at fixed frequency, measure harmonic content via FFT
- Latency: compare sample offset between input and output buffers of hosted plugin
- PDF export: serialize all active measurement data + plain-English summaries to PDF/text

## Next Steps

- [ ] Complete UI mockup workflow (/dream PluginScope → option 3)
- [ ] Finalize design and generate full parameter-spec.md
- [ ] Validate consistency between draft and final spec
