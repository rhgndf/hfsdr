<script>
  import { onDestroy, onMount } from 'svelte'
  import { describeDevice, requestHfsdrDevice } from './lib/webusb.js'
  import {
    parseFrequencyInput,
    parseTlv320GainInput,
    readClockFrequency as readDeviceClockFrequency,
    readPllLock as readDevicePllLock,
    setClockFrequency as writeClockFrequency,
    setTlv320Gain as writeTlv320Gain,
    tlv320GainDbToRaw,
    TLV320_GAIN_MAX_DB,
    TLV320_GAIN_MIN_DB,
    TLV320_GAIN_STEP_DB,
  } from './lib/control.js'
  import { startVendorIqStream } from './lib/data.js'
  import { createFmAudioPlayer } from './lib/fm-audio.js'
  import {
    createSpectrogramRenderer,
    FFT_ROW_INTERVAL,
    FFT_SIZE,
    IQ_SAMPLE_RATE_HZ,
  } from './lib/spectrogram.js'

  const READY_STATUS = 1
  const ROW_RATE_HZ = IQ_SAMPLE_RATE_HZ / FFT_ROW_INTERVAL
  const BIN_WIDTH_HZ = IQ_SAMPLE_RATE_HZ / FFT_SIZE
  const HALF_BANDWIDTH_KHZ = IQ_SAMPLE_RATE_HZ / 2000
  const QUARTER_BANDWIDTH_KHZ = IQ_SAMPLE_RATE_HZ / 4000
  const TLV320_GAIN_DEBOUNCE_MS = 100
  const PLL_POLL_INTERVAL_MS = 200
  const FREQUENCY_LABELS = [
    `-${HALF_BANDWIDTH_KHZ.toFixed(0)} kHz`,
    `-${QUARTER_BANDWIDTH_KHZ.toFixed(0)} kHz`,
    'DC',
    `+${QUARTER_BANDWIDTH_KHZ.toFixed(0)} kHz`,
    `+${HALF_BANDWIDTH_KHZ.toFixed(0)} kHz`,
  ]

  let device = null
  let frequency = ''
  let tlv320Gain = 0
  let pllLocked = null
  let deviceLabel = 'No device paired'
  let statusMessage = 'Pair a WebUSB device to read or set the clock frequency.'
  let streamMessage = 'Pair a device to start the live I/Q spectrogram.'
  let isConnecting = false
  let isReading = false
  let isSetting = false
  let isSettingGain = false
  let streamStats = null
  let streamHandle = null
  let spectrogramCanvas = null
  let spectrogramRenderer = null
  let historySeconds = 0
  let pendingTlv320Gain = null
  let tlv320GainDebounceTimer = null
  let tlv320GainFlushRequested = false
  let lastTlv320GainSentAt = 0
  let fmAudioPlayer = null
  let isAudioStarting = false
  let isAudioPlaying = false
  let audioMessage = 'Pair a device, then start FM audio playback.'
  let hoverFrequencyInfo = null
  let pllPollTimer = null
  let pllPollGeneration = 0
  let pllPollInFlight = false

  function stopIqStream(message = 'Stream stopped.') {
    if (streamHandle) {
      streamHandle.stop()
      streamHandle = null
    }

    streamStats = null
    streamMessage = message
    fmAudioPlayer?.reset()

    if (isAudioPlaying) {
      audioMessage = 'FM audio is running and waiting for live I/Q samples.'
    }
  }

  function startIqStream(selectedDevice) {
    stopIqStream('Waiting for incoming I/Q samples...')
    spectrogramRenderer?.clear()

    streamHandle = startVendorIqStream(selectedDevice, {
      onIqSamples(iqSamples) {
        spectrogramRenderer?.pushIqSamples(iqSamples)
        fmAudioPlayer?.pushIqSamples(iqSamples)
      },
      onStatus(nextStats) {
        streamStats = nextStats
        streamMessage = `Streaming ${Math.round(nextStats.framesPerSecond).toLocaleString()} complex samples/s at ${nextStats.mbPerSecond.toFixed(3)} MB/s.`
      },
    })

    streamHandle.done.catch((error) => {
      if (error?.name === 'AbortError') {
        return
      }

      console.error('Vendor I/Q stream stopped:', error)
      streamMessage = error?.message || 'I/Q stream stopped unexpectedly.'
    })
  }

  function stopPllLockPolling() {
    pllPollGeneration += 1
    pllPollInFlight = false

    if (pllPollTimer !== null) {
      clearInterval(pllPollTimer)
      pllPollTimer = null
    }
  }

  async function pollPllLock(selectedDevice = device, generation = pllPollGeneration) {
    if (!selectedDevice || pllPollInFlight || generation !== pllPollGeneration) {
      return
    }

    pllPollInFlight = true

    try {
      const { status, locked } = await readDevicePllLock(selectedDevice)

      if (generation !== pllPollGeneration || selectedDevice !== device) {
        return
      }

      pllLocked = status === READY_STATUS ? locked : null
    } catch {
      if (generation !== pllPollGeneration || selectedDevice !== device) {
        return
      }

      pllLocked = null
    } finally {
      if (generation === pllPollGeneration) {
        pllPollInFlight = false
      }
    }
  }

  function startPllLockPolling(selectedDevice = device) {
    stopPllLockPolling()

    if (!selectedDevice) {
      pllLocked = null
      return
    }

    const generation = pllPollGeneration
    pllPollTimer = setInterval(() => {
      void pollPllLock(selectedDevice, generation)
    }, PLL_POLL_INTERVAL_MS)
    void pollPllLock(selectedDevice, generation)
  }

  async function startFmAudio() {
    if (!device) {
      audioMessage = 'Pair a device before starting FM audio.'
      return
    }

    isAudioStarting = true

    try {
      if (!fmAudioPlayer) {
        fmAudioPlayer = await createFmAudioPlayer({
          iqSampleRateHz: IQ_SAMPLE_RATE_HZ,
        })
      }

      fmAudioPlayer.reset()
      await fmAudioPlayer.start()
      isAudioPlaying = true
      audioMessage = `Playing FM-demodulated audio at ${(fmAudioPlayer.audioSampleRateHz / 1000).toFixed(0)} kHz.`
    } catch (error) {
      isAudioPlaying = false
      audioMessage = error?.message || 'Failed to start FM audio playback.'
    } finally {
      isAudioStarting = false
    }
  }

  async function stopFmAudio() {
    if (!fmAudioPlayer) {
      isAudioPlaying = false
      audioMessage = 'FM audio stopped.'
      return
    }

    try {
      await fmAudioPlayer.stop()
    } catch (error) {
      audioMessage = error?.message || 'Failed to stop FM audio playback cleanly.'
    } finally {
      isAudioPlaying = false
    }

    if (audioMessage.startsWith('Playing FM-demodulated audio')) {
      audioMessage = 'FM audio stopped.'
    }
  }

  async function toggleFmAudio() {
    if (isAudioPlaying) {
      await stopFmAudio()
      return
    }

    await startFmAudio()
  }

  async function closeFmAudioPlayer() {
    if (!fmAudioPlayer) {
      isAudioPlaying = false
      isAudioStarting = false
      return
    }

    const player = fmAudioPlayer
    fmAudioPlayer = null
    isAudioPlaying = false
    isAudioStarting = false

    try {
      await player.close()
    } catch (error) {
      console.error('Failed to close FM audio player:', error)
    }
  }
  async function readClockFrequency(selectedDevice = device) {
    if (!selectedDevice) {
      throw new Error('No device paired.')
    }

    isReading = true

    try {
      const [{ status, frequencyHz }, { status: pllStatus, locked }] = await Promise.all([
        readDeviceClockFrequency(selectedDevice),
        readDevicePllLock(selectedDevice),
      ])

      frequency = frequencyHz.toString()
      pllLocked = pllStatus === READY_STATUS ? locked : null
      statusMessage =
        status === READY_STATUS
          ? `Current device frequency: ${frequency} Hz`
          : `Device reported status ${status} while returning ${frequency} Hz`
    } finally {
      isReading = false
    }
  }

  async function pairDevice() {
    isConnecting = true
    pllLocked = null
    stopPllLockPolling()
    stopIqStream('Pairing device...')

    try {
      const selectedDevice = await requestHfsdrDevice()

      device = selectedDevice
      deviceLabel = describeDevice(selectedDevice)
      statusMessage = 'Device paired. Reading current frequency...'

      await readClockFrequency(selectedDevice)
      //startPllLockPolling(selectedDevice)
      startIqStream(selectedDevice)
    } catch (error) {
      pllLocked = null
      statusMessage = error?.message || 'Failed to pair with the WebUSB device.'
      streamMessage = 'Pair a device to start the live I/Q spectrogram.'
    } finally {
      isConnecting = false
    }
  }

  async function setClockFrequency() {
    if (!device) {
      statusMessage = 'Pair a device before setting the frequency.'
      return
    }

    isSetting = true

    try {
      const parsedFrequency = parseFrequencyInput(frequency)
      await writeClockFrequency(device, parsedFrequency)

      statusMessage = `Frequency update sent: ${parsedFrequency.toString()} Hz. Refreshing...`
      await readClockFrequency(device)
    } catch (error) {
      statusMessage = error?.message || 'Failed to set the frequency.'
    } finally {
      isSetting = false
    }
  }

  function clearTlv320GainDebounceTimer() {
    if (tlv320GainDebounceTimer !== null) {
      clearTimeout(tlv320GainDebounceTimer)
      tlv320GainDebounceTimer = null
    }
  }

  function scheduleTlv320GainSend(delayMs) {
    clearTlv320GainDebounceTimer()
    tlv320GainDebounceTimer = setTimeout(() => {
      tlv320GainDebounceTimer = null
      void flushTlv320Gain()
    }, delayMs)
  }

  async function flushTlv320Gain(forceImmediate = false) {
    if (!device || pendingTlv320Gain === null) {
      return
    }

    if (isSettingGain) {
      if (forceImmediate) {
        tlv320GainFlushRequested = true
      }
      return
    }

    const elapsedMs = performance.now() - lastTlv320GainSentAt
    if (!forceImmediate && lastTlv320GainSentAt !== 0 && elapsedMs < TLV320_GAIN_DEBOUNCE_MS) {
      scheduleTlv320GainSend(TLV320_GAIN_DEBOUNCE_MS - elapsedMs)
      return
    }

    const gainToSend = pendingTlv320Gain
    pendingTlv320Gain = null
    isSettingGain = true

    try {
      await writeTlv320Gain(device, gainToSend)
      const gainRaw = tlv320GainDbToRaw(gainToSend)
      statusMessage = `TLV320 gain update sent: ${gainToSend.toFixed(1)} dB (CHx_CFG1 0x${gainRaw.toString(16).padStart(2, '0').toUpperCase()})`
      lastTlv320GainSentAt = performance.now()
    } catch (error) {
      statusMessage = error?.message || 'Failed to set the TLV320 gain.'
      pendingTlv320Gain = null
      tlv320GainFlushRequested = false
    } finally {
      isSettingGain = false
    }

    if (pendingTlv320Gain !== null) {
      if (tlv320GainFlushRequested) {
        tlv320GainFlushRequested = false
        void flushTlv320Gain(true)
      } else {
        scheduleTlv320GainSend(TLV320_GAIN_DEBOUNCE_MS)
      }
    }
  }

  function queueTlv320Gain(nextGain = tlv320Gain, { forceImmediate = false } = {}) {
    const parsedGain = parseTlv320GainInput(String(nextGain))
    tlv320Gain = parsedGain
    pendingTlv320Gain = parsedGain

    if (!device) {
      statusMessage = 'Pair a device before setting the ADC gain.'
      return
    }

    if (forceImmediate) {
      tlv320GainFlushRequested = true
      clearTlv320GainDebounceTimer()
      void flushTlv320Gain(true)
      return
    }

    if (isSettingGain || tlv320GainDebounceTimer !== null) {
      return
    }

    const elapsedMs = performance.now() - lastTlv320GainSentAt
    const delayMs =
      lastTlv320GainSentAt === 0
        ? TLV320_GAIN_DEBOUNCE_MS
        : Math.max(0, TLV320_GAIN_DEBOUNCE_MS - elapsedMs)

    scheduleTlv320GainSend(delayMs)
  }

  function handleTlv320SliderInput() {
    queueTlv320Gain(tlv320Gain)
  }

  function handleTlv320GainCommit() {
    queueTlv320Gain(tlv320Gain, { forceImmediate: true })
  }

  function formatHz(value) {
    return `${value.toLocaleString()} Hz`
  }

  function clearSpectrogramHover() {
    hoverFrequencyInfo = null
  }

  function updateSpectrogramHover(event) {
    if (!spectrogramCanvas) {
      hoverFrequencyInfo = null
      return
    }

    const normalizedFrequency = frequency.trim()
    if (!/^\d+$/.test(normalizedFrequency)) {
      hoverFrequencyInfo = null
      return
    }

    const rect = spectrogramCanvas.getBoundingClientRect()
    const relativeX = Math.min(Math.max(event.clientX - rect.left, 0), rect.width)
    const normalizedX = rect.width > 0 ? relativeX / rect.width : 0
    const offsetHz = Math.round((normalizedX - 0.5) * IQ_SAMPLE_RATE_HZ)
    const absoluteHz = BigInt(normalizedFrequency) + BigInt(offsetHz)
    const dbfs = spectrogramRenderer?.getDbfsAtNormalizedX(normalizedX) ?? null
    const anchor =
      normalizedX <= 0.18 ? 'left' : normalizedX >= 0.82 ? 'right' : 'center'

    hoverFrequencyInfo = {
      x: relativeX,
      y: Math.min(Math.max(event.clientY - rect.top, 0), rect.height),
      offsetHz,
      absoluteHz,
      dbfs,
      anchor,
    }
  }

  onMount(() => {
    if (spectrogramCanvas) {
      spectrogramRenderer = createSpectrogramRenderer(spectrogramCanvas)
    }

    const handleResize = () => spectrogramRenderer?.resize()

    window.addEventListener('resize', handleResize)

    return () => {
      window.removeEventListener('resize', handleResize)
      clearTlv320GainDebounceTimer()
      stopPllLockPolling()
      stopIqStream()
      void closeFmAudioPlayer()
    }
  })

  onDestroy(() => {
    clearTlv320GainDebounceTimer()
    stopPllLockPolling()
    stopIqStream()
    void closeFmAudioPlayer()
  })

  $: historySeconds = spectrogramCanvas ? spectrogramCanvas.height / ROW_RATE_HZ : 0
</script>

<main class="min-h-screen bg-slate-950 px-6 py-10 text-slate-100">
  <div class="mx-auto flex w-full max-w-6xl flex-col gap-6">
    <section class="rounded-[2rem] border border-cyan-500/20 bg-[radial-gradient(circle_at_top,rgba(34,211,238,0.16),transparent_36%),linear-gradient(180deg,rgba(2,6,23,0.98),rgba(15,23,42,0.96))] p-8 shadow-[0_30px_120px_rgba(8,145,178,0.16)] sm:p-10">
      <div class="flex flex-col gap-8 lg:flex-row lg:items-start lg:justify-between">
        <div class="max-w-2xl">
          <p class="text-sm font-medium uppercase tracking-[0.28em] text-cyan-300/80">HFSDR Control</p>
          <h1 class="mt-3 text-4xl font-semibold tracking-tight text-white sm:text-5xl">
            Live I/Q spectrogram
          </h1>
          <p class="mt-4 max-w-xl text-base leading-7 text-slate-300">
            The vendor endpoint now streams fixed-point complex I/Q from the ADC. This view decodes
            the raw I2S slots in the browser and paints a scrolling waterfall centered on DC.
          </p>
        </div>

        <div class="grid gap-3 rounded-2xl border border-white/10 bg-white/5 p-4 text-sm text-slate-200 sm:grid-cols-2">
          <div class="rounded-xl bg-slate-950/50 p-3">
            <p class="text-xs uppercase tracking-[0.22em] text-slate-400">Sample Rate</p>
            <p class="mt-2 text-lg font-semibold text-white">{(IQ_SAMPLE_RATE_HZ / 1000).toFixed(0)} kS/s</p>
          </div>
          <div class="rounded-xl bg-slate-950/50 p-3">
            <p class="text-xs uppercase tracking-[0.22em] text-slate-400">FFT Size</p>
            <p class="mt-2 text-lg font-semibold text-white">{FFT_SIZE} bins</p>
          </div>
          <div class="rounded-xl bg-slate-950/50 p-3">
            <p class="text-xs uppercase tracking-[0.22em] text-slate-400">Bin Width</p>
            <p class="mt-2 text-lg font-semibold text-white">{BIN_WIDTH_HZ.toFixed(1)} Hz</p>
          </div>
          <div class="rounded-xl bg-slate-950/50 p-3">
            <p class="text-xs uppercase tracking-[0.22em] text-slate-400">Waterfall Rate</p>
            <p class="mt-2 text-lg font-semibold text-white">{ROW_RATE_HZ.toFixed(1)} rows/s</p>
          </div>
        </div>
      </div>
    </section>

    <section class="grid gap-6 lg:grid-cols-[minmax(0,1.8fr)_minmax(21rem,1fr)]">
      <div class="rounded-[2rem] border border-cyan-500/20 bg-slate-900/80 p-5 shadow-[0_24px_90px_rgba(15,23,42,0.35)] sm:p-6">
        <div class="flex flex-col gap-3 border-b border-white/10 pb-4 sm:flex-row sm:items-end sm:justify-between">
          <div>
            <p class="text-sm font-medium uppercase tracking-[0.24em] text-cyan-300/75">Waterfall</p>
            <h2 class="mt-2 text-2xl font-semibold text-white">Complex baseband spectrum</h2>
          </div>
          <div class="text-sm text-slate-400">
            {#if historySeconds > 0}
              {historySeconds.toFixed(1)} s visible history
            {:else}
              Waiting for canvas
            {/if}
          </div>
        </div>

        <div class="mt-5">
          <div class="relative rounded-[1.5rem] border border-white/10 bg-slate-950">
            <div class="overflow-hidden rounded-[inherit]">
              <canvas
                bind:this={spectrogramCanvas}
                class="block h-[26rem] w-full"
                on:mousemove={updateSpectrogramHover}
                on:mouseleave={clearSpectrogramHover}
              ></canvas>

              <div class="pointer-events-none absolute inset-x-0 top-0 flex justify-between px-4 py-3 text-xs font-medium uppercase tracking-[0.22em] text-white/60">
                {#each FREQUENCY_LABELS as label}
                  <span>{label}</span>
                {/each}
              </div>

              <div class="pointer-events-none absolute inset-x-0 bottom-0 border-t border-white/10 bg-gradient-to-t from-slate-950 via-slate-950/90 to-transparent px-4 py-3 text-sm text-slate-300">
                {streamMessage}
              </div>
            </div>

            {#if hoverFrequencyInfo}
              <div
                class="pointer-events-none absolute z-20 rounded-xl border border-cyan-300/20 bg-slate-950/95 px-3 py-2 text-xs text-slate-100 shadow-[0_16px_40px_rgba(8,145,178,0.2)] whitespace-nowrap"
                style={`left: ${hoverFrequencyInfo.x}px; top: ${Math.min(hoverFrequencyInfo.y + 18, (spectrogramCanvas?.clientHeight ?? hoverFrequencyInfo.y) - 8)}px; transform: ${
                  hoverFrequencyInfo.anchor === 'left'
                    ? 'translate(0, 0)'
                    : hoverFrequencyInfo.anchor === 'right'
                      ? 'translate(-100%, 0)'
                      : 'translate(-50%, 0)'
                };`}
              >
                <p class="font-semibold text-cyan-200">{formatHz(hoverFrequencyInfo.absoluteHz)}</p>
                {#if hoverFrequencyInfo.dbfs !== null}
                  <p class="mt-1 text-amber-200">{hoverFrequencyInfo.dbfs.toFixed(1)} dBFS</p>
                {/if}
              </div>
            {/if}
          </div>
        </div>
      </div>

      <div class="flex flex-col gap-6">
        <section class="rounded-[2rem] border border-white/10 bg-slate-900/80 p-6 shadow-[0_24px_90px_rgba(15,23,42,0.35)]">
          <div class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
            <div>
              <p class="text-sm font-medium uppercase tracking-[0.24em] text-slate-400">Paired Device</p>
              <p class="mt-2 text-lg font-semibold text-white">{deviceLabel}</p>
            </div>
            <button
              class="inline-flex items-center justify-center rounded-xl bg-cyan-400 px-4 py-2.5 text-sm font-semibold text-slate-950 transition hover:bg-cyan-300 disabled:cursor-not-allowed disabled:bg-slate-600 disabled:text-slate-300"
              on:click={pairDevice}
              disabled={isConnecting || isReading || isSetting || isSettingGain}
            >
              {isConnecting ? 'Pairing...' : 'Pair device'}
            </button>
          </div>

          <div class="mt-6 rounded-2xl border border-white/10 bg-slate-950/60 p-4">
            <div class="flex items-start justify-between gap-4">
              <p class="text-sm font-medium text-slate-400">Clock frequency (Hz)</p>
              <div class="flex items-center gap-2 text-sm font-medium">
                <span
                  class={`h-3 w-3 rounded-full ${
                    pllLocked === true
                      ? 'bg-emerald-400'
                      : pllLocked === false
                        ? 'bg-red-500'
                        : 'bg-slate-500'
                  }`}
                ></span>
                <span class={pllLocked === true ? 'text-emerald-300' : pllLocked === false ? 'text-red-300' : 'text-slate-400'}>
                  {pllLocked === true
                    ? 'PLL locked'
                    : pllLocked === false
                      ? 'PLL not locked'
                      : 'PLL status unknown'}
                </span>
              </div>
            </div>
            <div class="mt-3 flex flex-col gap-3">
              <input
                id="frequency"
                class="w-full rounded-xl border border-white/10 bg-slate-900 px-4 py-3 text-base text-white outline-none transition focus:border-cyan-400 focus:ring-2 focus:ring-cyan-400/20"
                type="text"
                inputmode="numeric"
                bind:value={frequency}
                placeholder="7067333"
                disabled={!device || isConnecting || isReading || isSetting || isSettingGain}
              />
              <button
                class="inline-flex items-center justify-center rounded-xl bg-emerald-400 px-4 py-3 text-sm font-semibold text-slate-950 transition hover:bg-emerald-300 disabled:cursor-not-allowed disabled:bg-slate-700 disabled:text-slate-300"
                on:click={setClockFrequency}
                disabled={!device || isConnecting || isReading || isSetting || isSettingGain}
              >
                {isSetting ? 'Setting...' : 'Set frequency'}
              </button>
            </div>
          </div>

          <div class="mt-6 rounded-2xl border border-white/10 bg-slate-950/60 p-4">
            <div class="flex items-start justify-between gap-4">
              <div>
                <p class="text-sm font-medium text-slate-400">ADC gain</p>
                <p class="mt-2 text-sm leading-6 text-slate-500">
                  Sends the same gain to TLV320 `CH1_CFG1` and `CH2_CFG1` in 0.5 dB steps.
                </p>
              </div>
              <p class="text-sm font-semibold text-white">
                {tlv320Gain.toFixed(1)} dB
              </p>
            </div>

            <div class="mt-4 flex flex-col gap-4">
              <input
                class="w-full accent-amber-400"
                type="range"
                min={TLV320_GAIN_MIN_DB}
                max={TLV320_GAIN_MAX_DB}
                step={TLV320_GAIN_STEP_DB}
                bind:value={tlv320Gain}
                on:input={handleTlv320SliderInput}
                on:change={handleTlv320GainCommit}
                disabled={!device || isConnecting || isReading || isSetting}
              />

              <div class="flex flex-col gap-3 sm:flex-row sm:items-center">
                <input
                  class="w-full rounded-xl border border-white/10 bg-slate-900 px-4 py-3 text-base text-white outline-none transition focus:border-cyan-400 focus:ring-2 focus:ring-cyan-400/20"
                  type="number"
                  min={TLV320_GAIN_MIN_DB}
                  max={TLV320_GAIN_MAX_DB}
                  step={TLV320_GAIN_STEP_DB}
                  bind:value={tlv320Gain}
                  on:change={handleTlv320GainCommit}
                  disabled={!device || isConnecting || isReading || isSetting}
                />
                <p class="text-sm text-slate-500">
                  CHx_CFG1 = 0x{tlv320GainDbToRaw(tlv320Gain).toString(16).padStart(2, '0').toUpperCase()}
                </p>
              </div>
            </div>
          </div>

          <div class="mt-6 rounded-2xl border border-white/10 bg-slate-950/60 p-4">
            <div class="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
              <div>
                <p class="text-sm font-medium text-slate-400">FM audio</p>
                <p class="mt-2 text-sm leading-6 text-slate-500">
                  Demodulates the live DC-centered I/Q stream into mono audio with a phase
                  discriminator, 4:1 decimation, and de-emphasis.
                </p>
              </div>
              <button
                class="inline-flex items-center justify-center rounded-xl bg-fuchsia-400 px-4 py-2.5 text-sm font-semibold text-slate-950 transition hover:bg-fuchsia-300 disabled:cursor-not-allowed disabled:bg-slate-700 disabled:text-slate-300"
                on:click={toggleFmAudio}
                disabled={!device || isConnecting || isAudioStarting}
              >
                {#if isAudioStarting}
                  Starting...
                {:else if isAudioPlaying}
                  Stop audio
                {:else}
                  Start audio
                {/if}
              </button>
            </div>

            <div class="mt-4 rounded-xl border border-white/10 bg-slate-900/70 p-4">
              <p class="text-sm leading-6 text-slate-200">{audioMessage}</p>
            </div>
          </div>

          <div class="mt-6 rounded-2xl border border-white/10 bg-slate-950/60 p-4">
            <p class="text-sm font-medium text-slate-400">Status</p>
            <p class="mt-2 text-sm leading-6 text-slate-200">{statusMessage}</p>
          </div>
        </section>

        <section class="rounded-[2rem] border border-amber-400/20 bg-slate-900/80 p-6 shadow-[0_24px_90px_rgba(15,23,42,0.35)]">
          <p class="text-sm font-medium uppercase tracking-[0.24em] text-amber-300/80">Stream Telemetry</p>
          <div class="mt-4 grid gap-3 sm:grid-cols-2">
            <div class="rounded-2xl bg-slate-950/60 p-4">
              <p class="text-xs uppercase tracking-[0.2em] text-slate-400">Throughput</p>
              <p class="mt-2 text-xl font-semibold text-white">
                {streamStats ? `${streamStats.mbPerSecond.toFixed(3)} MB/s` : 'Idle'}
              </p>
            </div>
            <div class="rounded-2xl bg-slate-950/60 p-4">
              <p class="text-xs uppercase tracking-[0.2em] text-slate-400">Complex Samples</p>
              <p class="mt-2 text-xl font-semibold text-white">
                {streamStats ? Math.round(streamStats.framesPerSecond).toLocaleString() : '0'} /s
              </p>
            </div>
            <div class="rounded-2xl bg-slate-950/60 p-4">
              <p class="text-xs uppercase tracking-[0.2em] text-slate-400">Captured</p>
              <p class="mt-2 text-xl font-semibold text-white">
                {streamStats ? `${streamStats.totalMb.toFixed(2)} MB` : '0.00 MB'}
              </p>
            </div>
            <div class="rounded-2xl bg-slate-950/60 p-4">
              <p class="text-xs uppercase tracking-[0.2em] text-slate-400">FFT Cadence</p>
              <p class="mt-2 text-xl font-semibold text-white">1 row / {FFT_ROW_INTERVAL} samples</p>
            </div>
          </div>
        </section>
      </div>
    </section>
  </div>
</main>
