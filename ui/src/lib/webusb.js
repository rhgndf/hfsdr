const HFSDR_VENDOR_ID = 0xcafe
const USB_CONFIGURATION = 1
const VENDOR_INTERFACE_CLASS = 0xff

function ensureWebUsbAvailable() {
  if (!('usb' in navigator)) {
    throw new Error('WebUSB is not available in this browser.')
  }
}

function isVendorAlternate(alternate) {
  return alternate.interfaceClass === VENDOR_INTERFACE_CLASS
}

export async function ensureDeviceOpen(device) {
  if (!device) {
    throw new Error('No device paired.')
  }

  if (!device.opened) {
    await device.open()
  }

  if (device.configuration?.configurationValue !== USB_CONFIGURATION) {
    await device.selectConfiguration(USB_CONFIGURATION)
  }
}

export function describeDevice(device) {
  return device.productName || `0x${device.vendorId.toString(16)} device`
}

export function getVendorInterface(device) {
  return device?.configuration?.interfaces.find((iface) =>
    iface.alternates.some(isVendorAlternate),
  ) ?? null
}

export function getVendorAlternate(device) {
  const vendorInterface = getVendorInterface(device)

  if (!vendorInterface) {
    return null
  }

  return vendorInterface.alternate ?? vendorInterface.alternates.find(isVendorAlternate) ?? null
}

export function getVendorInEndpointNumber(device) {
  const vendorAlternate = getVendorAlternate(device)
  const inEndpoint = vendorAlternate?.endpoints.find((endpoint) => endpoint.direction === 'in')

  if (!inEndpoint) {
    throw new Error('Vendor IN endpoint not found in the active USB configuration.')
  }

  return inEndpoint.endpointNumber
}

export async function claimVendorInterface(device) {
  await ensureDeviceOpen(device)

  const vendorInterface = getVendorInterface(device)
  if (!vendorInterface) {
    throw new Error('Vendor interface not found in the active USB configuration.')
  }

  const vendorAlternate = vendorInterface.alternate ?? vendorInterface.alternates.find(isVendorAlternate)

  if (!vendorInterface.claimed) {
    await device.claimInterface(vendorInterface.interfaceNumber)
  }

  if (
    vendorAlternate &&
    vendorInterface.alternate &&
    vendorInterface.alternate.alternateSetting !== vendorAlternate.alternateSetting
  ) {
    await device.selectAlternateInterface(
      vendorInterface.interfaceNumber,
      vendorAlternate.alternateSetting,
    )
  }

  return {
    interfaceNumber: vendorInterface.interfaceNumber,
    alternate: getVendorAlternate(device) ?? vendorAlternate,
  }
}

export async function requestHfsdrDevice() {
  ensureWebUsbAvailable()

  const device = await navigator.usb.requestDevice({
    filters: [{ vendorId: HFSDR_VENDOR_ID }],
  })

  await ensureDeviceOpen(device)

  return device
}
