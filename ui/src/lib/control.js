import { ensureDeviceOpen } from './webusb.js'

const VENDOR_REQUEST_SET_CLK_FREQ = 3
const VENDOR_REQUEST_GET_CLK_FREQ = 4
const CLK_FREQ_RESPONSE_LENGTH = 9

function uint64ToLittleEndianBytes(value) {
  const bytes = new Uint8Array(8)
  let remaining = value

  for (let index = 0; index < bytes.length; index += 1) {
    bytes[index] = Number(remaining & 0xffn)
    remaining >>= 8n
  }

  return bytes
}

function littleEndianBytesToUint64(bytes) {
  let value = 0n

  for (let index = 0; index < bytes.length; index += 1) {
    value |= BigInt(bytes[index]) << BigInt(index * 8)
  }

  return value
}

export function parseFrequencyInput(value) {
  const normalized = value.trim()

  if (!/^\d+$/.test(normalized)) {
    throw new Error('Frequency must be a whole number in Hz.')
  }

  return BigInt(normalized)
}

export async function readClockFrequency(device) {
  await ensureDeviceOpen(device)

  const response = await device.controlTransferIn(
    {
      requestType: 'vendor',
      recipient: 'device',
      request: VENDOR_REQUEST_GET_CLK_FREQ,
      value: 0,
      index: 0,
    },
    CLK_FREQ_RESPONSE_LENGTH,
  )

  if (response.status !== 'ok' || !response.data) {
    throw new Error('Failed to read the clock frequency from the device.')
  }

  const data = new Uint8Array(response.data.buffer, response.data.byteOffset, response.data.byteLength)

  return {
    status: data[0],
    frequencyHz: littleEndianBytesToUint64(data.slice(1, 9)),
  }
}

export async function setClockFrequency(device, frequencyHz) {
  await ensureDeviceOpen(device)

  const response = await device.controlTransferOut(
    {
      requestType: 'vendor',
      recipient: 'device',
      request: VENDOR_REQUEST_SET_CLK_FREQ,
      value: 0,
      index: 0,
    },
    uint64ToLittleEndianBytes(frequencyHz),
  )

  if (response.status !== 'ok') {
    throw new Error('Device rejected the frequency update.')
  }
}
