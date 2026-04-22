---
name: USB Audio Out Path
overview: Brainstorm and sequence how to expose the CH32V305 board as a USB audio playback sink, receive host audio, and play it on the board jack using TinyUSB + DAC, without touching the current USB mic path.
todos:
  - id: add-usb-audio-out-descriptor
    content: Design and add USB Audio OUT (speaker) descriptors and TinyUSB config for OUT endpoint support, leaving existing mic/source path unchanged.
    status: completed
  - id: implement-usb-out-to-dac-bridge
    content: Implement USB OUT packet reception, PCM-to-DAC conversion, and ring-buffered DAC DMA playback.
    status: completed
  - id: add-activity-gated-playback
    content: Add playback activity gating so conversion/refill work runs only when USB OUT data is received, with idle timeout fallback to quiet output.
    status: completed
  - id: stabilize-clocking-and-buffering
    content: Add underrun protection and basic buffer-level drift control for glitch-free playback.
    status: completed
  - id: validate-usb-speaker-path
    content: Test host enumeration as a playback device and verify stable audio out at the board jack.
    status: completed
isProject: false
---

# USB Audio OUT Brainstorm Plan

## Code Organization Constraint

- Implement new functionality in `feature/usb_dac_out` as much as possible.
- Keep edits to existing files minimal and limited to integration points:
  - USB descriptor/config wiring for Audio OUT,
  - startup hook(s) in `main.c`,
  - small `dac_hw` API hooks only when unavoidable.
- Prefer forwarding/wrapper glue in existing files and place state machines, buffering, conversion, and metrics in `feature/usb_dac_out`.

## Current Baseline (already present)

- Device already enumerates USB Audio class as a **microphone source** (UAC2 IN path) in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/usb_descriptors.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/usb_descriptors.c)` and `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/usb_descriptors.h](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/usb_descriptors.h)`.
- Existing USB mic/source path is intentionally **out of scope** for this work and should remain untouched.
- TinyUSB audio config is IN-only (`CFG_TUD_AUDIO_ENABLE_EP_OUT 0`) in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/tusb_config.h](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/tusb_config.h)`.
- Capture pipeline exists from I2S DMA but is currently routed to vendor endpoint rather than USB audio endpoint in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/i2s_hw.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/i2s_hw.c)`.
- On-board analog output path exists (dual DAC + DMA, currently noise/sine generation) in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c)` and startup in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/main.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/main.c)`.

## Practical Implementation Strategy

- **Step 1: Add USB Audio Sink (host -> board)**
  - Extend descriptors to include speaker/output capability while preserving current mic entities unchanged: add AudioStreaming OUT interface + isoch OUT endpoint + speaker entities (input terminal, feature unit, output terminal).
  - Enable OUT support in TinyUSB config and add receive callbacks (`tud_audio_rx_done_`* / OUT FIFO handling) in audio module.
- **Step 2: Bridge USB OUT samples to DAC DMA**
  - Add ring buffer between USB OUT packets and DAC DMA refill ISR to absorb USB microframe jitter.
  - Convert host PCM format to DAC format (e.g., signed 16/24-bit stereo -> unsigned 12-bit centered at 2048) with clipping and optional simple volume.
  - Replace `dac_hw_static_noise_start(...)` runtime mode with `dac_hw_usb_stream_start(...)` and underrun fallback (hold last sample or midscale).
- **Step 3: Add activity-gated playback (performance guard)**
  - Process and enqueue DAC audio only when USB OUT packets are arriving.
  - If no packets arrive for a timeout window (e.g., 20-100 ms), enter idle mode and output quiet midscale.
  - Use soft-idle first: keep DAC/TIM/DMA armed but emit constant 2048 to minimize restart jitter.
- **Step 4: Clocking/rate policy (critical for stable playback)**
  - Start with a single rate first (e.g., 48 kHz) for both USB OUT and DAC timer, then add 192 kHz later.
  - Implement simple drift control by adjusting DAC playback pacing from ring-buffer fill level (or use async feedback endpoint if needed later).
- **Step 5: Host validation matrix**
  - Verify enumeration as a USB playback/speaker endpoint on Windows/Linux/macOS.
  - Confirm host playback reaches jack cleanly and auto-idles when no data is sent.
  - Track drop/underrun counters and active-vs-idle duty metrics in periodic logs.

## Function and Peripheral Plan

- **USB device stack (TinyUSB)**
  - Add a dedicated module under `feature/usb_dac_out` (for example `usb_dac_out.c/.h`) as the central handler for playback state.
  - Keep `audio.c` changes thin: forward speaker-side entity handling and OUT stream callbacks to `feature/usb_dac_out` helpers, while preserving current mic entities.
  - Handle OUT stream data (`tud_audio_read(...)`) in `feature/usb_dac_out` and feed its ring buffer there.
  - Keep USB bring-up unchanged via `usb_hw_init()` and `tud_task()` in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/usb_hw.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/usb_hw.c)` and `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/main.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/main.c)`.
- **USB descriptors/config**
  - Extend descriptor macros in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/usb_descriptors.h](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/usb_descriptors.h)` to include AudioStreaming OUT interface + OUT endpoint while preserving existing mic/interface numbers.
  - Update configuration assembly in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/usb_descriptors.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/usb_descriptors.c)` (`CONFIG_TOTAL_LEN`, endpoint IDs, interface count) to expose a playback endpoint.
  - Enable TinyUSB OUT path in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/tusb_config.h](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/tusb_config.h)` by setting `CFG_TUD_AUDIO_ENABLE_EP_OUT` and matching OUT format/rate macros.
- **Playback datapath (software)**
  - Implement lock-free ring buffer in `feature/usb_dac_out`: producer = USB OUT callback context, consumer = DAC refill context.
  - Implement PCM conversion helper in `feature/usb_dac_out` (target initial format: 16-bit stereo host PCM -> 12-bit unsigned DAC code): clamp, recenter to midscale 2048, optional mono mix if needed by existing DAC dual-same-sample mode.
  - Implement activity state (`audio_active` / `audio_idle`) in `feature/usb_dac_out`, derived from time since last USB OUT packet.
  - Implement runtime counters in `feature/usb_dac_out` for `usb_rx_frames`, `dac_played_frames`, `underruns`, `overruns`, `audio_active_ms`, and `audio_idle_ms`.
- **Peripherals for analog output**
  - Reuse existing DAC subsystem in `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c)`:
    - `DAC` peripheral channels 1/2 on PA4/PA5.
    - `TIM7` as sample-rate trigger (`TIM_TRGO` update).
    - `DMA2_Channel3` circular transfer into `DAC->RD12BDHR`.
    - `DMA2_Channel3_IRQHandler()` half/full-transfer interrupts for chunk refills.
  - Add the smallest new API surface in `dac_hw` for external sample feed (for example `dac_hw_stream_pcm_start(sample_rate_hz)` + `dac_hw_stream_pcm_write_chunk(...)`), while keeping refill policy and sample preparation in `feature/usb_dac_out`.
- **Main-loop/peripheral orchestration**
  - In `[C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/main.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/main.c)`, replace `dac_hw_static_noise_start(...)` with a single `feature/usb_dac_out` init/start hook after USB init.
  - Keep `DAC_Poll()` as diagnostics source but source counters/state from `feature/usb_dac_out`.
  - Keep existing mic/capture/I2S code untouched for this phase.
- **Initial parameter set (safe first bring-up)**
  - 48 kHz, 2 channels, 16-bit PCM on USB OUT.
  - DAC output initially same sample on both PA4/PA5 (matches current dual-write architecture), then optional true stereo split in a follow-up.
  - Ring buffer target depth: 10-30 ms equivalent audio (tunable after first sound).

## Key Risks and Mitigations

- **USB isochronous timing mismatch** -> use deep enough ring buffer + buffer-level servo.
- **Sample-format mismatch** (24-in-32 USB, 12-bit DAC) -> explicit conversion function with saturation and DC center.
- **CPU load in ISR** -> keep ISR minimal; move conversion to chunk worker if needed.
- **Descriptor complexity (UAC2)** -> stage implementation around output path first; avoid refactoring mic/source path during this iteration.

## Suggested Build Order

1. Add output-only USB speaker path and play host tone to jack.
2. Replace DAC noise mode with USB-fed stream mode.
3. Add activity gating so playback work is active only during real USB audio traffic.
4. Tune buffering/latency and stress-test playback stability.
5. Optional follow-up: upgrade from mirrored dual-mono DAC to true stereo mapping.

## Target File Layout (preferred)

- `src/feature/usb_dac_out/usb_dac_out.h`
  - Public API for init, TinyUSB callback forwarding, and diagnostics getters.
- `src/feature/usb_dac_out/usb_dac_out.c`
  - Playback state machine, ring buffer, PCM conversion, activity gating, and stats.
- Optional split for clarity if needed:
  - `src/feature/usb_dac_out/usb_dac_out_pcm.c`
  - `src/feature/usb_dac_out/usb_dac_out_stats.c`

