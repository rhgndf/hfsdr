const DEFAULT_AUDIO_SAMPLE_RATE_HZ = 48000
const DEFAULT_DEEMPHASIS_US = 75
const DEFAULT_AUDIO_GAIN = 0.8
const OUTPUT_WORKLET_NAME = 'queued-audio-output'
const WORKLET_MODULE_URL = new URL('./audio-output-worklet.js', import.meta.url)

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max)
}

function createFmDemodulator({
  iqSampleRateHz,
  audioSampleRateHz,
  deemphasisTimeConstantUs = DEFAULT_DEEMPHASIS_US,
  audioGain = DEFAULT_AUDIO_GAIN,
}) {
  const decimationFactor = Math.max(1, Math.round(iqSampleRateHz / audioSampleRateHz))
  const actualAudioSampleRateHz = iqSampleRateHz / decimationFactor
  const dt = 1 / actualAudioSampleRateHz
  const deemphasisAlpha =
    deemphasisTimeConstantUs <= 0
      ? 1
      : dt / (deemphasisTimeConstantUs * 1e-6 + dt)

  let previousI = 1
  let previousQ = 0
  let previousInput = 0
  let dcBlocked = 0
  let deemphasis = 0
  let decimationSum = 0
  let decimationCount = 0

  function reset() {
    previousI = 1
    previousQ = 0
    previousInput = 0
    dcBlocked = 0
    deemphasis = 0
    decimationSum = 0
    decimationCount = 0
  }

  function demodulate(iqSamples) {
    const output = new Float32Array(Math.ceil(iqSamples.length / (2 * decimationFactor)))
    let outputIndex = 0

    for (let sampleIndex = 0; sampleIndex < iqSamples.length; sampleIndex += 2) {
      const i = iqSamples[sampleIndex]
      const q = iqSamples[sampleIndex + 1]
      const cross = previousI * q - previousQ * i
      const dot = previousI * i + previousQ * q
      const phaseDelta = Math.atan2(cross, dot + Number.EPSILON)

      previousI = i
      previousQ = q

      const dcBlockInput = phaseDelta
      dcBlocked = dcBlockInput - previousInput + 0.995 * dcBlocked
      previousInput = dcBlockInput

      decimationSum += dcBlocked
      decimationCount += 1

      if (decimationCount === decimationFactor) {
        const averagedSample = decimationSum / decimationFactor
        decimationSum = 0
        decimationCount = 0

        deemphasis += deemphasisAlpha * (averagedSample - deemphasis)
        output[outputIndex] = clamp(deemphasis * audioGain, -1, 1)
        outputIndex += 1
      }
    }

    return output.subarray(0, outputIndex)
  }

  return {
    actualAudioSampleRateHz,
    demodulate,
    reset,
  }
}

export async function createFmAudioPlayer(options = {}) {
  const {
    iqSampleRateHz,
    audioSampleRateHz = DEFAULT_AUDIO_SAMPLE_RATE_HZ,
    audioGain = DEFAULT_AUDIO_GAIN,
  } = options

  if (!iqSampleRateHz) {
    throw new Error('An IQ sample rate is required for FM audio playback.')
  }

  const demodulator = createFmDemodulator({
    iqSampleRateHz,
    audioSampleRateHz,
    audioGain,
  })

  const audioContext = new AudioContext({
    sampleRate: demodulator.actualAudioSampleRateHz,
    latencyHint: 'interactive',
  })

  await audioContext.audioWorklet.addModule(WORKLET_MODULE_URL)

  const node = new AudioWorkletNode(audioContext, OUTPUT_WORKLET_NAME, {
    numberOfInputs: 0,
    numberOfOutputs: 1,
    outputChannelCount: [2],
  })

  node.connect(audioContext.destination)

  function reset() {
    demodulator.reset()
    node.port.postMessage({ type: 'reset' })
  }

  return {
    audioSampleRateHz: audioContext.sampleRate,
    async start() {
      await audioContext.resume()
    },
    async stop() {
      reset()
      await audioContext.suspend()
    },
    pushIqSamples(iqSamples) {
      if (audioContext.state !== 'running') {
        return
      }

      const audioSamples = demodulator.demodulate(iqSamples)
      if (audioSamples.length === 0) {
        return
      }

      node.port.postMessage(
        {
          type: 'push',
          payload: audioSamples.buffer,
        },
        [audioSamples.buffer],
      )
    },
    reset,
    async close() {
      reset()
      node.disconnect()
      await audioContext.close()
    },
  }
}
