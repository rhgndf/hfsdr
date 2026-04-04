import { claimVendorInterface, ensureDeviceOpen } from './webusb.js'

const VENDOR_REQUEST_SET_CLK_FREQ = 3
const VENDOR_REQUEST_GET_CLK_FREQ = 4
const VENDOR_REQUEST_SET_TLV320_GAIN = 5
const VENDOR_REQUEST_GET_PLL_LOCK = 6
const CLK_FREQ_RESPONSE_LENGTH = 9
const PLL_LOCK_RESPONSE_LENGTH = 2
export const TLV320_GAIN_MIN_DB = -11
export const TLV320_GAIN_MAX_DB = 42
export const TLV320_GAIN_STEP_DB = 0.5

async function getVendorControlSetup(device, request) {
  await ensureDeviceOpen(device)
  const { interfaceNumber } = await claimVendorInterface(device)

  return {
    requestType: 'vendor',
    recipient: 'interface',
    request,
    value: 0,
    index: interfaceNumber,
  }
}

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

export function parseTlv320GainInput(value) {
  const normalized = value.trim()
  const gainDb = Number.parseFloat(normalized)

  if (!Number.isFinite(gainDb)) {
    throw new Error('TLV320 gain must be a number in dB.')
  }

  if (gainDb < TLV320_GAIN_MIN_DB || gainDb > TLV320_GAIN_MAX_DB) {
    throw new Error(`TLV320 gain must be between ${TLV320_GAIN_MIN_DB} dB and ${TLV320_GAIN_MAX_DB} dB.`)
  }

  const halfDbSteps = Math.round(gainDb / TLV320_GAIN_STEP_DB)
  if (Math.abs(gainDb - halfDbSteps * TLV320_GAIN_STEP_DB) > 1e-9) {
    throw new Error('TLV320 gain must use 0.5 dB steps.')
  }

  return halfDbSteps * TLV320_GAIN_STEP_DB
}

export function tlv320GainDbToRaw(gainDb) {
  const normalizedGainDb = parseTlv320GainInput(String(gainDb))
  const magnitudeSteps = Math.round(Math.abs(normalizedGainDb) / TLV320_GAIN_STEP_DB)
  const signBit = normalizedGainDb < 0 ? 1 : 0
  return (magnitudeSteps << 1) | signBit
}

export async function readClockFrequency(device) {
  const setup = await getVendorControlSetup(device, VENDOR_REQUEST_GET_CLK_FREQ)

  const response = await device.controlTransferIn(
    setup,
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
  const setup = await getVendorControlSetup(device, VENDOR_REQUEST_SET_CLK_FREQ)

  const response = await device.controlTransferOut(
    setup,
    uint64ToLittleEndianBytes(frequencyHz),
  )

  if (response.status !== 'ok') {
    throw new Error('Device rejected the frequency update.')
  }
}

export async function readPllLock(device) {
  const setup = await getVendorControlSetup(device, VENDOR_REQUEST_GET_PLL_LOCK)

  const response = await device.controlTransferIn(
    setup,
    PLL_LOCK_RESPONSE_LENGTH,
  )

  if (response.status !== 'ok' || !response.data) {
    throw new Error('Failed to read PLL lock status from the device.')
  }

  const data = new Uint8Array(response.data.buffer, response.data.byteOffset, response.data.byteLength)

  return {
    status: data[0],
    locked: data[1] !== 0,
  }
}

export async function setTlv320Gain(device, gainDb) {
  const gainRaw = tlv320GainDbToRaw(gainDb)
  const setup = await getVendorControlSetup(device, VENDOR_REQUEST_SET_TLV320_GAIN)

  const response = await device.controlTransferOut(
    setup,
    new Uint8Array([gainRaw]),
  )

  if (response.status !== 'ok') {
    throw new Error('Device rejected the TLV320 gain update.')
  }
}
