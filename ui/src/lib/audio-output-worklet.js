class QueuedAudioOutputProcessor extends AudioWorkletProcessor {
  constructor() {
    super()

    this.queue = []
    this.readOffset = 0

    this.port.onmessage = (event) => {
      const { type, payload } = event.data ?? {}

      if (type === 'reset') {
        this.queue = []
        this.readOffset = 0
        return
      }

      if (type === 'push' && payload) {
        this.queue.push(new Float32Array(payload))
      }
    }
  }

  process(_inputs, outputs) {
    const outputChannels = outputs[0]
    if (!outputChannels || outputChannels.length === 0) {
      return true
    }

    const frameCount = outputChannels[0].length

    for (let frameIndex = 0; frameIndex < frameCount; frameIndex += 1) {
      let sample = 0

      while (this.queue.length > 0) {
        const chunk = this.queue[0]

        if (this.readOffset < chunk.length) {
          sample = chunk[this.readOffset]
          this.readOffset += 1
          break
        }

        this.queue.shift()
        this.readOffset = 0
      }

      for (let channelIndex = 0; channelIndex < outputChannels.length; channelIndex += 1) {
        outputChannels[channelIndex][frameIndex] = sample
      }
    }

    return true
  }
}

registerProcessor('queued-audio-output', QueuedAudioOutputProcessor)
