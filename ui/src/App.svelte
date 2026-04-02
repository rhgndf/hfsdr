<script>
  import {
    describeDevice,
    parseFrequencyInput,
    readClockFrequency as readDeviceClockFrequency,
    requestHfsdrDevice,
    setClockFrequency as writeClockFrequency,
  } from './lib/webusb.js'

  const READY_STATUS = 1

  let device = null
  let frequency = ''
  let deviceLabel = 'No device paired'
  let statusMessage = 'Pair a WebUSB device to read or set the clock frequency.'
  let isConnecting = false
  let isReading = false
  let isSetting = false

  async function readClockFrequency(selectedDevice = device) {
    if (!selectedDevice) {
      throw new Error('No device paired.')
    }

    isReading = true

    try {
      const { status, frequencyHz } = await readClockFrequencyFromDevice(selectedDevice)

      frequency = frequencyHz.toString()
      statusMessage =
        status === READY_STATUS
          ? `Current device frequency: ${frequency} Hz`
          : `Device reported status ${status} while returning ${frequency} Hz`
    } finally {
      isReading = false
    }
  }

  async function readClockFrequencyFromDevice(selectedDevice) {
    return readDeviceClockFrequency(selectedDevice)
  }

  async function pairDevice() {
    isConnecting = true

    try {
      const selectedDevice = await requestHfsdrDevice()

      device = selectedDevice
      deviceLabel = describeDevice(selectedDevice)
      statusMessage = 'Device paired. Reading current frequency...'

      await readClockFrequency(selectedDevice)
    } catch (error) {
      statusMessage = error?.message || 'Failed to pair with the WebUSB device.'
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
</script>

<main class="flex min-h-screen items-center justify-center bg-slate-50 px-6 py-16 text-slate-900">
  <div class="w-full max-w-2xl rounded-2xl border border-slate-200 bg-white p-8 shadow-sm sm:p-10">
    <p class="text-sm font-medium uppercase tracking-[0.24em] text-slate-500">HFSDR Control</p>
    <h1 class="mt-3 text-3xl font-semibold tracking-tight sm:text-4xl">WebUSB clock control</h1>
    <p class="mt-4 text-base leading-7 text-slate-600">
      Pair the device, read its current clock frequency, then update it with the vendor control requests
      already implemented in firmware.
    </p>

    <div class="mt-8 rounded-xl border border-slate-200 bg-slate-50 p-5">
      <div class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
        <div>
          <p class="text-sm font-medium text-slate-500">Paired device</p>
          <p class="mt-1 text-base font-semibold text-slate-900">{deviceLabel}</p>
        </div>
        <button
          class="inline-flex items-center justify-center rounded-lg bg-slate-900 px-4 py-2.5 text-sm font-medium text-white transition hover:bg-slate-700 disabled:cursor-not-allowed disabled:bg-slate-400"
          on:click={pairDevice}
          disabled={isConnecting || isReading || isSetting}
        >
          {isConnecting ? 'Pairing...' : 'Pair device'}
        </button>
      </div>
    </div>

    <div class="mt-6">
      <label class="block text-sm font-medium text-slate-700" for="frequency">Clock frequency (Hz)</label>
      <div class="mt-2 flex flex-col gap-3 sm:flex-row">
        <input
          id="frequency"
          class="w-full rounded-lg border border-slate-300 bg-white px-4 py-3 text-base text-slate-900 outline-none transition focus:border-slate-500 focus:ring-2 focus:ring-slate-200"
          type="text"
          inputmode="numeric"
          bind:value={frequency}
          placeholder="7067333"
          disabled={!device || isConnecting || isReading || isSetting}
        />
        <button
          class="inline-flex items-center justify-center rounded-lg bg-emerald-600 px-4 py-3 text-sm font-medium text-white transition hover:bg-emerald-500 disabled:cursor-not-allowed disabled:bg-emerald-300"
          on:click={setClockFrequency}
          disabled={!device || isConnecting || isReading || isSetting}
        >
          {isSetting ? 'Setting...' : 'Set frequency'}
        </button>
      </div>
    </div>

    <div class="mt-6 rounded-xl border border-slate-200 bg-white p-4">
      <p class="text-sm font-medium text-slate-500">Status</p>
      <p class="mt-2 text-sm leading-6 text-slate-700">{statusMessage}</p>
    </div>
  </div>
</main>
