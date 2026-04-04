import { claimVendorInterface, getVendorInEndpointNumber } from './webusb.js'

const DEFAULT_TRANSFER_BYTES = 16 * 1024
const DEFAULT_REPORT_EVERY_MS = 1000
const DEFAULT_IN_FLIGHT = 8
const BYTES_PER_MB = 1000 * 1000

function formatMb(value) {
  return value.toFixed(3)
}

function createTransferPromise(device, endpointNumber, transferSize, slot) {
  return device.transferIn(endpointNumber, transferSize).then((response) => ({
    response,
    slot,
  }))
}

export async function readVendorDataLoop(device, options = {}) {
  if (!device) {
    throw new Error('No device paired.')
  }

  const {
    transferSize = DEFAULT_TRANSFER_BYTES,
    reportEveryMs = DEFAULT_REPORT_EVERY_MS,
    inFlight = DEFAULT_IN_FLIGHT,
    signal,
  } = options

  const { interfaceNumber } = await claimVendorInterface(device)
  const endpointNumber = getVendorInEndpointNumber(device)
  const startedAt = performance.now()
  let reportStartedAt = startedAt
  let totalBytes = 0
  let reportBytes = 0
  const pendingTransfers = new Map()

  console.log(
    `Starting vendor read benchmark on interface ${interfaceNumber}, endpoint 0x${endpointNumber.toString(16)} with ${transferSize} byte requests and ${inFlight} transfers in flight.`,
  )

  for (let slot = 0; slot < inFlight; slot += 1) {
    pendingTransfers.set(slot, createTransferPromise(device, endpointNumber, transferSize, slot))
  }

  while (!signal?.aborted && pendingTransfers.size > 0) {
    const { response, slot } = await Promise.race(pendingTransfers.values())
    pendingTransfers.delete(slot)

    if (response.status !== 'ok' || !response.data) {
      throw new Error(`Vendor read failed with status "${response.status}".`)
    }

    const bytesRead = response.data.byteLength
    totalBytes += bytesRead
    reportBytes += bytesRead

    const now = performance.now()
    const reportElapsedMs = now - reportStartedAt

    if (reportElapsedMs >= reportEveryMs) {
      const totalElapsedSeconds = Math.max((now - startedAt) / 1000, Number.EPSILON)
      const reportElapsedSeconds = Math.max(reportElapsedMs / 1000, Number.EPSILON)
      const totalMb = totalBytes / BYTES_PER_MB
      const mbPerSecond = reportBytes / reportElapsedSeconds / BYTES_PER_MB

      console.log(
        `Vendor IN ${formatMb(mbPerSecond)} MB/s | total ${formatMb(totalMb)} MB | elapsed ${totalElapsedSeconds.toFixed(1)} s`,
      )

      reportBytes = 0
      reportStartedAt = now
    }

    if (!signal?.aborted) {
      pendingTransfers.set(slot, createTransferPromise(device, endpointNumber, transferSize, slot))
    }
  }
}

export function startVendorReadBenchmark(device, options = {}) {
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

  const done = readVendorDataLoop(device, {
    ...options,
    signal: controller.signal,
  })

  return {
    stop(reason = 'Stopped vendor read benchmark.') {
      controller.abort(reason)
    },
    done,
  }
}
