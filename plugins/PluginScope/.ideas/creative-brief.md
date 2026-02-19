# PluginScope - Creative Brief

## Overview

**Type:** Utility
**Core Concept:** A musician-friendly plugin analyzer that hosts other audio plugins internally, runs precision test signals through them, and presents measurements with layered plain-English summaries instead of raw technical data.
**Status:** 💡 Ideated
**Created:** 2026-02-19

## Vision

PluginScope is a direct response to the gap between powerful plugin analyzers (like Plugin Doctor by DDMF) and the musicians and engineers who could benefit most from them. Plugin Doctor is technically impressive but dense — cluttered with jargon, offering no interpretation, and providing no guidance on where to start. Most audio professionals look at the readout and don't know what it means.

PluginScope solves this by keeping the power of full plugin analysis (frequency response, dynamics behavior, harmonic distortion, phase response, latency) while completely rethinking how results are communicated. Every measurement is paired with a layered plain-English summary: a musical description of how it sounds, a practical note on what it does to your mix, and an educational explanation of what the behavior means technically. Users can read as much or as little as they want.

The workflow is guided from the start. On first launch, PluginScope asks what kind of plugin you're analyzing — this context-aware routing sets up the right analysis view and test signals automatically. From there, a simplified mode walks users through guided steps, while an expert toggle reveals the full technical readout for those who want it. Live mode lets you watch how measurements change as you adjust a plugin's parameters in real time; snapshot mode freezes measurements for comparison.

A/B comparison is a first-class feature: load two plugins and overlay their measurements on the same graph, or snapshot a plugin's response, tweak its settings, and compare before vs. after.

## Parameters

*PluginScope is an analyzer, not an audio processor — it has no audio parameters in the traditional sense. The following are the primary user-configurable settings:*

| Setting | Options | Default | Description |
|---------|---------|---------|-------------|
| View Mode | Simplified / Expert | Simplified | Controls amount of detail and technical data shown |
| Analysis Mode | Live / Snapshot | Live | Live updates in real-time; Snapshot freezes for comparison |
| Analysis Type | Frequency / Dynamics / Distortion / Phase / Latency | Frequency | Active measurement view |
| Test Signal | Sine Sweep / White Noise / Pink Noise / Impulse / Live Audio | Sine Sweep | Signal sent through the hosted plugin for measurement |
| Comparison | Off / A/B Plugins / Before-After | Off | Overlay two measurements on the same graph |

## UI Concept

**Layout:** Clean, modern app aesthetic — flat design, precise typography, color-coded data. Not clinical/oscilloscope-style, but not skeuomorphic either. Closer to a well-designed desktop analysis app (think Fabfilter's clarity applied to measurement tooling).

**First Launch Experience:** On open, PluginScope presents a "Select analysis type" menu before anything else:
- Analyzing an EQ
- Analyzing a Compressor / Limiter
- Analyzing a Saturator / Distortion
- Analyzing a Reverb / Effect
- General analysis

This context-aware routing pre-selects relevant test signals and measurements so the user doesn't need to configure anything.

**Plugin Loading:** PluginScope scans the system for installed plugins and presents a searchable list. Users select a plugin from the list to load it into the internal host.

**Main View:**
- Left panel: hosted plugin's UI (rendered inline)
- Right panel: analysis visualizations (tabs for Frequency / Dynamics / Distortion / Phase / Latency)
- Bottom panel: plain-English summary with three collapsible sections (Musical / Practical / Educational)

**Expert Toggle:** Collapses the plain-English summary panel and adds detailed technical overlays (precise dB values, exact Hz markers, phase degree readouts) to the main graphs.

**Comparison Mode:** Overlays two measurements on the same graph using distinct color pairs (e.g., blue vs. orange). Difference curve optional.

**Key Visual Elements:**
- Color-coded frequency bands (warm = lows, cool = highs) for intuitive spectrum reading
- Contextual tooltips on hover — e.g., hovering the 200Hz region on a dynamics curve shows "Low-mid body region"
- Guided step indicator in simplified mode (Step 1 of 3, etc.)

## Use Cases

- Understanding what a new EQ or compressor is actually doing to the signal before committing to a purchase or a sound
- Verifying a compressor's behavior (attack, release, knee shape) without relying on marketing copy
- Comparing two "transparent" EQs to see which actually has more color
- Teaching signal processing concepts visually — show a student what a high-shelf EQ boost looks like as a curve
- Debugging a plugin chain — identifying which plugin in a chain is causing unexpected frequency coloring

## Inspirations

- **Plugin Doctor by DDMF Audio Plugins** — the primary reference and direct inspiration; the gold standard for plugin analysis, but not musician-friendly
- **Fabfilter Pro-Q 3** — the clearest, most readable frequency analyzer in the industry; sets the standard for clean, color-coded analysis UI
- **iZotope Insight 2** — excellent example of layered metering with plain-language context
- **SPAN by Voxengo** — highly regarded spectrum analyzer; precision without unnecessary clutter

## Technical Notes

- **Plugin hosting architecture:** PluginScope must host other VST3/AU/VST2 plugins internally using JUCE's `AudioPluginInstance` API. This is the most technically complex aspect of the build and should be the focus of Stage 0 research.
- **Supported formats (hosted plugins):** VST3, AU (macOS), VST2 (subject to SDK availability — Steinberg no longer freely distributes VST2 SDK). AAX was requested but requires Avid certification and a proprietary SDK — likely deferred to a future version.
- **Test signal generation:** Logarithmic sine sweep (20Hz–20kHz), white noise, pink noise, Dirac impulse. JUCE DSP module handles signal generation.
- **FFT analysis:** JUCE's `dsp::FFT` for frequency domain measurements. Window functions (Hann, Blackman) for accuracy.
- **Dynamics curve measurement:** Sweep input level from -60dBFS to 0dBFS and measure output level at each point to construct gain reduction curve.
- **Harmonic distortion:** Inject a sine at a fixed frequency, measure harmonic content in the output via FFT.
- **Phase response:** Measure phase offset per frequency band using dual-channel comparison (dry vs. wet).
- **Latency measurement:** Compare sample offset between input and output buffers of the hosted plugin.
- **Live mode:** Continuous FFT/measurement update synchronized to the hosted plugin's output buffer.
- **Export:** PDF or text summary of all active measurements + plain-English findings.
- **Platform:** Mac (Apple Silicon + Intel universal binary) + Windows. VST3 + AU builds.
- **A/B comparison:** Hold two sets of measurement data in memory; render both on the same canvas with color differentiation.

## Scope Notes (v1)

- Analysis results are **session-only** — no saving or recall between sessions (reduces v1 complexity)
- AAX format deferred (certification required)
- VST2 support contingent on SDK access
- Standalone app mode deferred to future version (v1 is DAW plugin only)

## Next Steps

- [ ] Create UI mockup (`/dream PluginScope` → option 3)
- [ ] Start implementation (`/implement PluginScope`)
