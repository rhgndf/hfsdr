#include "recording.h"

extern "C" {
#include "FreeRTOS.h"
#include "task.h"

#include "debug.h"
#include "ff.h"
}

#include "freertos/task_stacks.h"
#include "hw/sdcard/sdcard.h"
#include "hw/sdcard/sdio.h"
#include "ui/hw_state.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace
{

constexpr UBaseType_t kRecorderPriority = 2U;
constexpr UBaseType_t kRecorderNotificationIndex = 1U;
constexpr uint32_t kDataPending = 1U;

constexpr size_t kSectorBytes = 512U;
constexpr size_t kRingBytes = 4U * 1024U;
constexpr size_t kI2sChunkWordCount = kSectorBytes / sizeof(uint16_t);
constexpr uint32_t kWavJunkBytes = 460U;

constexpr uint32_t kSampleRateHz = 192000U;
constexpr uint16_t kChannelCount = 2U;
constexpr uint16_t kBitsPerSample = 32U;
constexpr uint16_t kBlockAlign =
    kChannelCount * (kBitsPerSample / 8U);
constexpr uint32_t kByteRate = kSampleRateHz * kBlockAlign;

/* Largest sector-aligned payload whose 512-byte header still fits FAT32. */
constexpr uint32_t kMaximumDataBytes = 0xFFFFFC00U;

static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES >
              kRecorderNotificationIndex);
static_assert(std::endian::native == std::endian::little);
static_assert(kByteRate == 1536000U);
static_assert(std::has_single_bit(kRingBytes));
static_assert((kRingBytes % kSectorBytes) == 0U);
static_assert(
    static_cast<uint64_t>(kMaximumDataBytes) + kSectorBytes <=
    std::numeric_limits<uint32_t>::max());

enum class RecorderState : uint32_t
{
    Idle,
    Starting,
    Recording,
    Stopping,
};

/*
 * The JUNK payload pads the data chunk to byte 512. All fields are naturally
 * aligned, and the target's native little-endian representation is WAV-ready.
 */
struct WavHeader
{
    std::array<char, 4U> riff_tag;
    uint32_t riff_size;
    std::array<char, 4U> wave_tag;
    std::array<char, 4U> format_tag;
    uint32_t format_size;
    uint16_t audio_format;
    uint16_t channel_count;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    std::array<char, 4U> junk_tag;
    uint32_t junk_size;
    std::array<std::byte, kWavJunkBytes> junk;
    std::array<char, 4U> data_tag;
    uint32_t data_size;
};

static_assert(std::is_standard_layout_v<WavHeader>);
static_assert(sizeof(WavHeader) == kSectorBytes);
static_assert(offsetof(WavHeader, data_tag) == 504U);
static_assert(offsetof(WavHeader, data_size) == 508U);

struct RingBuffer
{
    alignas(uint32_t) std::array<std::byte, kRingBytes> bytes{};
    uint32_t read_bytes = 0U;
    uint32_t write_bytes = 0U;
    uint32_t dropped = 0U;

    void reset()
    {
        read_bytes = 0U;
        write_bytes = 0U;
        dropped = 0U;
    }

    void discard()
    {
        read_bytes = write_bytes;
    }

    bool empty() const
    {
        return read_bytes == write_bytes;
    }
};

struct Session
{
    FATFS filesystem{};
    FIL file{};
    WavHeader header{};
    std::array<char, 64U> filename{};
    uint32_t data_bytes = 0U;
    bool file_open = false;
};

StaticTask_t s_task_tcb;
StackType_t s_task_stack[RECORDING_TASK_STACK_WORDS]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
TaskHandle_t s_task_handle = nullptr;

Session s_session;
RingBuffer s_ring;
volatile RecorderState s_state = RecorderState::Idle;

void clear_session()
{
    s_session.filename.fill('\0');
    s_session.data_bytes = 0U;
    s_session.file_open = false;
    s_ring.reset();
}

void initialize_header()
{
    s_session.header = {
        .riff_tag = {'R', 'I', 'F', 'F'},
        .riff_size = kSectorBytes - 8U,
        .wave_tag = {'W', 'A', 'V', 'E'},
        .format_tag = {'f', 'm', 't', ' '},
        .format_size = 16U,
        .audio_format = 1U,
        .channel_count = kChannelCount,
        .sample_rate = kSampleRateHz,
        .byte_rate = kByteRate,
        .block_align = kBlockAlign,
        .bits_per_sample = kBitsPerSample,
        .junk_tag = {'J', 'U', 'N', 'K'},
        .junk_size = kWavJunkBytes,
        .data_tag = {'d', 'a', 't', 'a'},
    };
}

void unmount()
{
    FRESULT const result = f_mount(nullptr, "0:", 0U);
    if(result != FR_OK)
    {
        std::printf("Recording: unmount failed (%u)\r\n",
                    static_cast<unsigned int>(result));
    }
}

bool write_file(std::span<std::byte const> bytes,
                uint32_t *byte_counter = nullptr)
{
    UINT written = 0U;
    FRESULT const result =
        f_write(&s_session.file, bytes.data(), bytes.size(), &written);
    if(byte_counter != nullptr)
    {
        *byte_counter += written;
    }

    if((result == FR_OK) && (written == bytes.size()))
    {
        return true;
    }

    std::printf("Recording: f_write failed (%u, %u/%u)\r\n",
                static_cast<unsigned int>(result),
                static_cast<unsigned int>(written),
                static_cast<unsigned int>(bytes.size()));

    auto const& diagnostics = sdcard::last_write_diagnostics();
    std::printf(
        "SDIO: write stage=%u addr=%08lx buf=%08lx len=%lu "
        "cmd=%08lx r1=%08lx\r\n",
        static_cast<unsigned int>(diagnostics.stage),
        static_cast<unsigned long>(diagnostics.address),
        static_cast<unsigned long>(diagnostics.buffer),
        static_cast<unsigned long>(diagnostics.length),
        static_cast<unsigned long>(diagnostics.command_events),
        static_cast<unsigned long>(diagnostics.response));
    std::printf(
        "SDIO: data events=%08lx expected=%08lx STA=%08lx "
        "DCTRL=%08lx\r\n",
        static_cast<unsigned long>(diagnostics.data_events),
        static_cast<unsigned long>(diagnostics.expected_data_events),
        static_cast<unsigned long>(diagnostics.status),
        static_cast<unsigned long>(diagnostics.data_control));
    std::printf(
        "SDIO: DCOUNT=%lu FIFO=%lu DMA=%lu CFGR=%08lx\r\n",
        static_cast<unsigned long>(diagnostics.data_count),
        static_cast<unsigned long>(diagnostics.fifo_count),
        static_cast<unsigned long>(diagnostics.dma_remaining),
        static_cast<unsigned long>(diagnostics.dma_config));
    return false;
}

bool write_header()
{
    s_session.header.riff_size =
        kSectorBytes - 8U + s_session.data_bytes;
    s_session.header.data_size = s_session.data_bytes;
    return write_file(std::as_bytes(
        std::span{&s_session.header, 1U}));
}

void cancel_session(bool remove_file)
{
    if(s_session.file_open)
    {
        (void)f_close(&s_session.file);
        s_session.file_open = false;
    }
    if(remove_file && (s_session.filename.front() != '\0'))
    {
        (void)f_unlink(s_session.filename.data());
    }

    unmount();
    clear_session();
    s_state = RecorderState::Idle;
}

void finish_session()
{
    if(!s_session.file_open)
    {
        cancel_session(false);
        return;
    }

    bool const header_updated =
        (f_lseek(&s_session.file, 0U) == FR_OK) && write_header();

    FRESULT const sync_result = f_sync(&s_session.file);
    if(sync_result != FR_OK)
    {
        std::printf("Recording: f_sync failed (%u)\r\n",
                    static_cast<unsigned int>(sync_result));
    }

    FRESULT const close_result = f_close(&s_session.file);
    if(close_result != FR_OK)
    {
        std::printf("Recording: f_close failed (%u)\r\n",
                    static_cast<unsigned int>(close_result));
    }
    s_session.file_open = false;
    unmount();

    uint64_t const dropped_bytes =
        static_cast<uint64_t>(s_ring.dropped) * kSectorBytes;
    uint64_t const total_input_bytes =
        s_session.data_bytes + dropped_bytes;
    uint32_t const dropped_percent_x100 =
        total_input_bytes == 0U
            ? 0U
            : static_cast<uint32_t>(
                  (dropped_bytes * 10'000U +
                   total_input_bytes / 2U) /
                  total_input_bytes);

    std::printf(
        "Recording: stopped %s, %lu PCM bytes, %lu.%02lu%% dropped%s\r\n",
        s_session.filename.data(),
        static_cast<unsigned long>(s_session.data_bytes),
        static_cast<unsigned long>(dropped_percent_x100 / 100U),
        static_cast<unsigned long>(dropped_percent_x100 % 100U),
        header_updated ? "" : ", header update failed");

    clear_session();
    s_state = RecorderState::Idle;
}

bool open_output_file(uint32_t frequency_hz)
{
    for(uint32_t counter = 0U;; ++counter)
    {
        int const length =
            std::snprintf(s_session.filename.data(),
                          s_session.filename.size(),
                          "0:/hfsdr-%lu-IQ-%lu.wav",
                          static_cast<unsigned long>(frequency_hz),
                          static_cast<unsigned long>(counter));
        if((length < 0) ||
           (static_cast<size_t>(length) >= s_session.filename.size()))
        {
            std::printf("Recording: filename is too long\r\n");
            return false;
        }

        FRESULT const result =
            f_open(&s_session.file,
                   s_session.filename.data(),
                   FA_WRITE | FA_CREATE_NEW);
        if(result == FR_OK)
        {
            s_session.file_open = true;
            return true;
        }
        if(result != FR_EXIST)
        {
            std::printf("Recording: f_open failed (%u)\r\n",
                        static_cast<unsigned int>(result));
            return false;
        }
        if(counter == std::numeric_limits<uint32_t>::max())
        {
            std::printf("Recording: no available filename counter\r\n");
            return false;
        }
    }
}

bool drain_ring()
{
    while(!s_ring.empty())
    {
        uint32_t write_size;
        const uint32_t read_bytes = s_ring.read_bytes % kRingBytes;
        const uint32_t write_bytes = s_ring.write_bytes % kRingBytes;
        if (write_bytes < read_bytes) {
            write_size = kRingBytes - read_bytes;
        } else {
            write_size = write_bytes - read_bytes;
        }

        if (write_size == 0) {
            // Empty ring
            return true;
        }

        uint32_t const available = kMaximumDataBytes - s_session.data_bytes;
        if(available == 0)
        {
            s_state = RecorderState::Stopping;
            s_ring.discard();
            std::printf("Recording: classic WAV/FAT32 size limit reached\r\n");
            return false;
        }

        auto const bytes = std::span{s_ring.bytes};
        bool written = write_file(bytes.subspan(read_bytes, write_size), &s_session.data_bytes);

        s_ring.read_bytes += write_size;
        if(!written)
        {
            return false;
        }
    }
    return true;
}

bool start_session()
{
    clear_session();
    initialize_header();
    (void)f_mount(nullptr, "0:", 0U);

    if(sdcard::detect() != READY)
    {
        std::printf("Recording: SD card not detected\r\n");
        cancel_session(false);
        return false;
    }

    FRESULT const mount_result =
        f_mount(&s_session.filesystem, "0:", 1U);
    if(mount_result != FR_OK)
    {
        std::printf("Recording: f_mount failed (%u)\r\n",
                    static_cast<unsigned int>(mount_result));
        cancel_session(false);
        return false;
    }

    uint32_t const frequency_hz = hw_state_get_frequency();
    if(!open_output_file(frequency_hz))
    {
        cancel_session(false);
        return false;
    }
    if(!write_header())
    {
        cancel_session(true);
        return false;
    }

    s_state = RecorderState::Recording;

    std::printf("Recording: started %s, %lu Hz IQ PCM\r\n",
                s_session.filename.data(),
                static_cast<unsigned long>(frequency_hz));
    return true;
}

void recording_task(void *parameters)
{
    (void)parameters;
    sdcard::init();

    for(;;)
    {
        /*
         * Mask the button interrupt across the idle check and self-suspend so a
         * resume cannot arrive in the otherwise vulnerable check-to-suspend gap.
         */
        taskENTER_CRITICAL();
        if(s_state == RecorderState::Idle)
        {
            vTaskSuspend(nullptr);
        }
        taskEXIT_CRITICAL();

        if((s_state == RecorderState::Recording) && s_ring.empty())
        {
            (void)xTaskNotifyWaitIndexed(kRecorderNotificationIndex,
                                        0U,
                                        UINT32_MAX,
                                        nullptr,
                                        portMAX_DELAY);
        }

        switch(s_state)
        {
            case RecorderState::Idle:
                break;

            case RecorderState::Starting:
                (void)start_session();
                break;

            case RecorderState::Recording:
                if(!drain_ring())
                {
                    s_state = RecorderState::Stopping;
                }
                break;

            case RecorderState::Stopping:
                (void)drain_ring();
                finish_session();
                break;
        }
    }
}

void notify_recorder()
{
    (void)xTaskNotifyIndexed(s_task_handle,
                             kRecorderNotificationIndex,
                             kDataPending,
                             eSetBits);
}

} // namespace

extern "C" void recording_init(void)
{
    configASSERT(s_task_handle == nullptr);
    s_task_handle =
        xTaskCreateStatic(recording_task,
                          "recording",
                          RECORDING_TASK_STACK_WORDS,
                          nullptr,
                          kRecorderPriority,
                          s_task_stack,
                          &s_task_tcb);
    configASSERT(s_task_handle != nullptr);
}

extern "C" void recording_request_toggle_from_isr(void)
{
    configASSERT(s_task_handle != nullptr);

    switch(s_state)
    {
        case RecorderState::Idle:
            s_state = RecorderState::Starting;
            break;

        case RecorderState::Starting:
        case RecorderState::Recording:
            s_state = RecorderState::Stopping;
            break;

        case RecorderState::Stopping:
            return;
    }

    portYIELD_FROM_ISR(xTaskResumeFromISR(s_task_handle));
}

extern "C" void recording_submit_i2s(
    volatile uint16_t const *samples,
    size_t sample_word_count)
{
    if(s_state != RecorderState::Recording)
    {
        return;
    }
    if((samples == nullptr) ||
       (sample_word_count != kI2sChunkWordCount))
    {
        return;
    }

    uint32_t write_bytes = s_ring.write_bytes;
    uint32_t read_bytes = s_ring.read_bytes;
    if((write_bytes - read_bytes) >= kRingBytes)
    {
        ++s_ring.dropped;
        return;
    }

    write_bytes %= kRingBytes;
    read_bytes %= kRingBytes;

    std::memcpy(s_ring.bytes.data() + write_bytes,
                const_cast<uint16_t const *>(samples),
                kSectorBytes);
    s_ring.write_bytes += kSectorBytes;

    notify_recorder();
}

extern "C" bool recording_is_active(void)
{
    return s_state == RecorderState::Recording;
}
