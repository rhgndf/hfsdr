const HFSDR_VENDOR_ID = 0xcafe
const USB_CONFIGURATION = 1
const VENDOR_REQUEST_SET_CLK_FREQ = 3
const VENDOR_REQUEST_GET_CLK_FREQ = 4
const CLK_FREQ_RESPONSE_LENGTH = 9

function ensureWebUsbAvailable() {
  if (!('usb' in navigator)) {
    throw new Error('WebUSB is not available in this browser.')
  }
}

async function ensureDeviceOpen(device) {
  if (!device.opened) {
    await device.open()
  }

  if (device.configuration?.configurationValue !== USB_CONFIGURATION) {
    await device.selectConfiguration(USB_CONFIGURATION)
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

export function describeDevice(device) {
  return device.productName || `0x${device.vendorId.toString(16)} device`
}

export async function requestHfsdrDevice() {
  ensureWebUsbAvailable()

  const device = await navigator.usb.requestDevice({
    filters: [{ vendorId: HFSDR_VENDOR_ID }],
  })

  await ensureDeviceOpen(device)

  return device
}

export async function readClockFrequency(device) {
  if (!device) {
    throw new Error('No device paired.')
  }

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
  if (!device) {
    throw new Error('No device paired.')
  }

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
