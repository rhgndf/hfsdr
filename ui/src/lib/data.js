import { claimVendorInterface, getVendorInEndpointNumber } from './webusb.js'

const DEFAULT_TRANSFER_BYTES = 64 * 1024
const DEFAULT_REPORT_EVERY_MS = 1000
const DEFAULT_IN_FLIGHT_TRANSFERS = 32
const IQ_FRAME_BYTES = 8
const IQ_FIXED_POINT_SCALE = 1 / 0x80000000
const BYTES_PER_MB = 1000 * 1000

function decodeSlotSample(view, offset) {
  const hiWord = view.getUint16(offset, true)
  const loWord = view.getUint16(offset + 2, true)
  const sample = ((hiWord << 16) | loWord) >> 0
  return sample * IQ_FIXED_POINT_SCALE
}

function decodeIqFrames(dataView) {
  const frameCount = Math.floor(dataView.byteLength / IQ_FRAME_BYTES)
  const iqSamples = new Float32Array(frameCount * 2)

  for (let frameIndex = 0; frameIndex < frameCount; frameIndex += 1) {
    const frameOffset = frameIndex * IQ_FRAME_BYTES
    iqSamples[frameIndex * 2] = decodeSlotSample(dataView, frameOffset)
    iqSamples[frameIndex * 2 + 1] = decodeSlotSample(dataView, frameOffset + 4)
  }

  return iqSamples
}

function appendCarry(carryBytes, nextBytes) {
  if (carryBytes.length === 0) {
    return nextBytes
  }

  const merged = new Uint8Array(carryBytes.length + nextBytes.length)
  merged.set(carryBytes, 0)
  merged.set(nextBytes, carryBytes.length)
  return merged
}

function clampInFlightTransfers(value) {
  if (!Number.isFinite(value)) {
    return DEFAULT_IN_FLIGHT_TRANSFERS
  }

  return Math.max(1, Math.floor(value))
}

function submitTransferIn(device, endpointNumber, transferSize) {
  return device
    .transferIn(endpointNumber, transferSize)
    .then((response) => ({ response }))
    .catch((error) => ({ error }))
}

export async function readVendorIqLoop(device, options = {}) {
  if (!device) {
    throw new Error('No device paired.')
  }

  const {
    transferSize = DEFAULT_TRANSFER_BYTES,
    reportEveryMs = DEFAULT_REPORT_EVERY_MS,
    inFlightTransfers = DEFAULT_IN_FLIGHT_TRANSFERS,
    onIqSamples,
    onStatus,
    signal,
  } = options

  await claimVendorInterface(device)
  const endpointNumber = getVendorInEndpointNumber(device)
  const transferCount = clampInFlightTransfers(inFlightTransfers)
  const startedAt = performance.now()
  let reportStartedAt = startedAt
  let totalBytes = 0
  let totalFrames = 0
  let reportBytes = 0
  let reportFrames = 0
  let carryBytes = new Uint8Array(0)
  const pendingTransfers = Array.from({ length: transferCount }, () =>
    submitTransferIn(device, endpointNumber, transferSize),
  )
  let nextTransferIndex = 0

  while (!signal?.aborted) {
    const { response, error } = await pendingTransfers[nextTransferIndex]

    if (signal?.aborted) {
      break
    }

    if (error) {
      throw error
    }

    if (response.status !== 'ok' || !response.data) {
      throw new Error(`Vendor read failed with status "${response.status}".`)
    }

    // Copy out the payload and immediately resubmit this transfer before any
    // decode/render/audio work so the browser keeps more reads in flight.
    const chunkBytes = new Uint8Array(
      response.data.buffer.slice(
        response.data.byteOffset,
        response.data.byteOffset + response.data.byteLength,
      ),
    )
    pendingTransfers[nextTransferIndex] = submitTransferIn(device, endpointNumber, transferSize)
    nextTransferIndex = (nextTransferIndex + 1) % transferCount

    
    const mergedBytes = appendCarry(carryBytes, chunkBytes)
    const wholeByteLength = mergedBytes.byteLength - (mergedBytes.byteLength % IQ_FRAME_BYTES)

    if (wholeByteLength > 0) {
      const frameBytes = mergedBytes.subarray(0, wholeByteLength)
      const iqSamples = decodeIqFrames(
        new DataView(frameBytes.buffer, frameBytes.byteOffset, frameBytes.byteLength),
      )

      totalBytes += frameBytes.byteLength
      totalFrames += iqSamples.length / 2
      reportBytes += frameBytes.byteLength
      reportFrames += iqSamples.length / 2

      onIqSamples?.(iqSamples)
    }

    carryBytes = mergedBytes.subarray(wholeByteLength)

    const now = performance.now()
    const reportElapsedMs = now - reportStartedAt

    if (reportElapsedMs >= reportEveryMs) {
      const totalElapsedSeconds = Math.max((now - startedAt) / 1000, Number.EPSILON)
      const reportElapsedSeconds = Math.max(reportElapsedMs / 1000, Number.EPSILON)
      const totalMb = totalBytes / BYTES_PER_MB
      const mbPerSecond = reportBytes / reportElapsedSeconds / BYTES_PER_MB
      const framesPerSecond = reportFrames / reportElapsedSeconds

      onStatus?.({
        totalBytes,
        totalFrames,
        totalMb,
        mbPerSecond,
        framesPerSecond,
      })

      reportBytes = 0
      reportFrames = 0
      reportStartedAt = now
    }
  }
}

export function startVendorIqStream(device, options = {}) {
  const controller = new AbortController()
  const externalSignal = options.signal

  if (externalSignal) {
    if (externalSignal.aborted) {
      controller.abort(externalSignal.reason)
    } else {
      externalSignal.addEventListener('abort', () => controller.abort(externalSignal.reason), {
        once: true,
      })
    }
  }

  const done = readVendorIqLoop(device, {
    ...options,
    signal: controller.signal,
  })

  return {
    stop(reason = 'Stopped vendor I/Q stream.') {
      controller.abort(reason)
    },
    done,
  }
}
