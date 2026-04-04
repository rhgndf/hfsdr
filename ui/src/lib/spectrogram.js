import FFT from 'fft.js'

export const IQ_SAMPLE_RATE_HZ = 192000
export const FFT_SIZE = 1024
export const FFT_ROW_INTERVAL = 4096

const INITIAL_DB_FLOOR = -110
const INITIAL_DB_CEILING = -55

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max)
}

function lerp(start, end, amount) {
  return start + (end - start) * amount
}

function createPalette() {
  const stops = [
    { t: 0.0, color: [2, 6, 23] },
    { t: 0.18, color: [8, 43, 70] },
    { t: 0.38, color: [29, 120, 122] },
    { t: 0.58, color: [244, 180, 27] },
    { t: 0.78, color: [239, 98, 57] },
    { t: 1.0, color: [255, 245, 234] },
  ]
  const palette = new Uint8ClampedArray(256 * 4)

  for (let index = 0; index < 256; index += 1) {
    const t = index / 255
    let lower = stops[0]
    let upper = stops[stops.length - 1]

    for (let stopIndex = 1; stopIndex < stops.length; stopIndex += 1) {
      if (t <= stops[stopIndex].t) {
        lower = stops[stopIndex - 1]
        upper = stops[stopIndex]
        break
      }
    }

    const range = Math.max(upper.t - lower.t, Number.EPSILON)
    const amount = (t - lower.t) / range
    const offset = index * 4

    palette[offset] = Math.round(lerp(lower.color[0], upper.color[0], amount))
    palette[offset + 1] = Math.round(lerp(lower.color[1], upper.color[1], amount))
    palette[offset + 2] = Math.round(lerp(lower.color[2], upper.color[2], amount))
    palette[offset + 3] = 255
  }

  return palette
}

const palette = createPalette()
const fft = new FFT(FFT_SIZE)
const windowCoefficients = new Float32Array(FFT_SIZE)
let windowSum = 0

for (let sampleIndex = 0; sampleIndex < FFT_SIZE; sampleIndex += 1) {
  windowCoefficients[sampleIndex] =
    0.5 - 0.5 * Math.cos((2 * Math.PI * sampleIndex) / (FFT_SIZE - 1))
  windowSum += windowCoefficients[sampleIndex]
}

function percentile(sortedValues, fraction) {
  const index = Math.min(
    sortedValues.length - 1,
    Math.max(0, Math.floor(fraction * (sortedValues.length - 1))),
  )
  return sortedValues[index]
}

export function createSpectrogramRenderer(canvas, options = {}) {
  const context = canvas.getContext('2d', { alpha: false })
  const historyI = new Float32Array(FFT_SIZE)
  const historyQ = new Float32Array(FFT_SIZE)
  const fftInput = fft.createComplexArray()
  const fftOutput = fft.createComplexArray()
  const lineMagnitudes = new Float32Array(FFT_SIZE)
  const dbValues = new Float32Array(FFT_SIZE)

  let rowImage = null
  let writeIndex = 0
  let storedSamples = 0
  let samplesSinceLastRow = 0
  let displayFloorDb = INITIAL_DB_FLOOR
  let displayCeilingDb = INITIAL_DB_CEILING

  context.imageSmoothingEnabled = false

  function clear() {
    context.save()
    context.setTransform(1, 0, 0, 1, 0, 0)
    context.fillStyle = '#020617'
    context.fillRect(0, 0, canvas.width, canvas.height)
    context.restore()
  }

  function resize() {
    const width = Math.max(Math.floor(canvas.clientWidth * window.devicePixelRatio), 1)
    const height = Math.max(Math.floor(canvas.clientHeight * window.devicePixelRatio), 1)

    if (canvas.width === width && canvas.height === height) {
      return
    }

    canvas.width = width
    canvas.height = height
    rowImage = context.createImageData(canvas.width, 1)
    clear()
  }

  function renderRow() {
    if (!rowImage || canvas.width === 0 || canvas.height === 0) {
      resize()
    }

    let iMean = 0
    let qMean = 0

    for (let sampleIndex = 0; sampleIndex < FFT_SIZE; sampleIndex += 1) {
      const historyIndex = (writeIndex + sampleIndex) % FFT_SIZE
      iMean += historyI[historyIndex]
      qMean += historyQ[historyIndex]
    }

    iMean /= FFT_SIZE
    qMean /= FFT_SIZE

    for (let sampleIndex = 0; sampleIndex < FFT_SIZE; sampleIndex += 1) {
      const historyIndex = (writeIndex + sampleIndex) % FFT_SIZE

      fftInput[sampleIndex * 2] = (historyI[historyIndex] - iMean) * windowCoefficients[sampleIndex]
      fftInput[sampleIndex * 2 + 1] = (historyQ[historyIndex] - qMean) * windowCoefficients[sampleIndex]
    }

    fft.transform(fftOutput, fftInput)

    for (let binIndex = 0; binIndex < FFT_SIZE; binIndex += 1) {
      const shiftedIndex = (binIndex + FFT_SIZE / 2) % FFT_SIZE
      const real = fftOutput[shiftedIndex * 2]
      const imag = fftOutput[shiftedIndex * 2 + 1]
      const magnitude = Math.sqrt(real * real + imag * imag)
      const magnitudeDb = 20 * Math.log10(magnitude / windowSum + 1e-12)
      dbValues[binIndex] = magnitudeDb
    }

    const sortedDbValues = dbValues.slice().sort()
    const p50Db = percentile(sortedDbValues, 0.5)
    const p95Db = percentile(sortedDbValues, 0.95)
    const targetFloorDb = p50Db - 18
    const targetCeilingDb = Math.max(p95Db + 8, targetFloorDb + 24)

    displayFloorDb = lerp(displayFloorDb, targetFloorDb, 0.12)
    displayCeilingDb = lerp(displayCeilingDb, targetCeilingDb, 0.12)

    for (let binIndex = 0; binIndex < FFT_SIZE; binIndex += 1) {
      const magnitudeDb = dbValues[binIndex]
      lineMagnitudes[binIndex] = clamp(
        (magnitudeDb - displayFloorDb) / (displayCeilingDb - displayFloorDb),
        0,
        1,
      )
    }

    context.drawImage(canvas, 0, 0, canvas.width, canvas.height - 1, 0, 1, canvas.width, canvas.height - 1)

    for (let x = 0; x < canvas.width; x += 1) {
      const sourceIndex = Math.min(
        Math.floor((x / Math.max(canvas.width - 1, 1)) * (FFT_SIZE - 1)),
        FFT_SIZE - 1,
      )
      const paletteIndex = Math.round(lineMagnitudes[sourceIndex] * 255) * 4
      const pixelOffset = x * 4

      rowImage.data[pixelOffset] = palette[paletteIndex]
      rowImage.data[pixelOffset + 1] = palette[paletteIndex + 1]
      rowImage.data[pixelOffset + 2] = palette[paletteIndex + 2]
      rowImage.data[pixelOffset + 3] = 255
    }

    context.putImageData(rowImage, 0, 0)
  }

  function pushIqSamples(iqSamples) {
    for (let sampleIndex = 0; sampleIndex < iqSamples.length; sampleIndex += 2) {
      historyI[writeIndex] = iqSamples[sampleIndex]
      historyQ[writeIndex] = iqSamples[sampleIndex + 1]
      writeIndex = (writeIndex + 1) % FFT_SIZE
      storedSamples = Math.min(storedSamples + 1, FFT_SIZE)
      samplesSinceLastRow += 1

      if (storedSamples === FFT_SIZE && samplesSinceLastRow >= FFT_ROW_INTERVAL) {
        samplesSinceLastRow = 0
        renderRow()
      }
    }
  }

  resize()

  return {
    clear,
    pushIqSamples,
    resize,
  }
}
