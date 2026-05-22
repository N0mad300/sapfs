#include "asio_output.h"

#ifdef _WIN32

#include "ring_buffer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long ASIOBool;
typedef long ASIOError;
typedef long ASIOSampleType;
typedef double ASIOSampleRate;
typedef int64_t ASIOSamples;
typedef int64_t ASIOTimeStamp;

enum {
    ASIOFalse = 0,
    ASIOTrue  = 1
};

enum {
    ASE_OK               = 0,
    ASE_SUCCESS          = 0x3f4847a0,
    ASE_NotPresent       = -1000,
    ASE_HWMalfunction    = -999,
    ASE_InvalidParameter = -998,
    ASE_InvalidMode      = -997,
    ASE_SPNotAdvancing   = -996,
    ASE_NoClock          = -995,
    ASE_NoMemory         = -994
};

enum {
    ASIOSTInt16MSB    = 0,
    ASIOSTInt24MSB    = 1,
    ASIOSTInt32MSB    = 2,
    ASIOSTFloat32MSB  = 3,
    ASIOSTFloat64MSB  = 4,
    ASIOSTInt32MSB16  = 8,
    ASIOSTInt32MSB18  = 9,
    ASIOSTInt32MSB20  = 10,
    ASIOSTInt32MSB24  = 11,
    ASIOSTInt16LSB    = 16,
    ASIOSTInt24LSB    = 17,
    ASIOSTInt32LSB    = 18,
    ASIOSTFloat32LSB  = 19,
    ASIOSTFloat64LSB  = 20,
    ASIOSTInt32LSB16  = 24,
    ASIOSTInt32LSB18  = 25,
    ASIOSTInt32LSB20  = 26,
    ASIOSTInt32LSB24  = 27
};

enum {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion,
    kAsioResetRequest,
    kAsioBufferSizeChange,
    kAsioResyncRequest,
    kAsioLatenciesChanged,
    kAsioSupportsTimeInfo,
    kAsioSupportsTimeCode,
    kAsioOverload = 15
};

#pragma pack(push, 4)
typedef struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
} ASIOTimeCode;

typedef struct AsioTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
} AsioTimeInfo;

typedef struct ASIOTime {
    long reserved[4];
    AsioTimeInfo timeInfo;
    ASIOTimeCode timeCode;
} ASIOTime;

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
} ASIOBufferInfo;

typedef struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
} ASIOClockSource;
#pragma pack(pop)

struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

struct AsioOutput {
    IASIO* driver;
    CLSID clsid;
    int com_initialized;
    int buffers_created;
    int supports_output_ready;

    AudioFormat format;
    AudioState state;
    char driver_name[128];

    long input_channels;
    long output_channels;
    long output_channel_offset;
    long min_buffer_size;
    long max_buffer_size;
    long preferred_buffer_size;
    long granularity;
    long buffer_size;
    long input_latency;
    long output_latency;

    ASIOCallbacks callbacks;
    ASIOBufferInfo* buffer_infos;
    ASIOChannelInfo* channel_infos;

    RingBuffer* ring_buffer;
    float* conv_buffer;
    size_t conv_buffer_samples;

    char error_msg[512];
};

static AsioOutput* g_active_output = NULL;
static char g_last_error[512];

static void set_error(AsioOutput* output, const char* fmt, ...) {
    char buffer[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    snprintf(g_last_error, sizeof(g_last_error), "%s", buffer);
    if (output) {
        snprintf(output->error_msg, sizeof(output->error_msg), "%s", buffer);
    }
}

static const char* asio_error_name(ASIOError err) {
    switch (err) {
        case ASE_OK: return "ASE_OK";
        case ASE_SUCCESS: return "ASE_SUCCESS";
        case ASE_NotPresent: return "ASE_NotPresent";
        case ASE_HWMalfunction: return "ASE_HWMalfunction";
        case ASE_InvalidParameter: return "ASE_InvalidParameter";
        case ASE_InvalidMode: return "ASE_InvalidMode";
        case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
        case ASE_NoClock: return "ASE_NoClock";
        case ASE_NoMemory: return "ASE_NoMemory";
        default: return "ASIOError";
    }
}

static void set_driver_error(AsioOutput* output, const char* action, ASIOError err) {
    char driver_msg[128] = {0};

    if (output && output->driver) {
        output->driver->getErrorMessage(driver_msg);
    }

    if (driver_msg[0]) {
        set_error(output, "%s failed (%s): %s", action, asio_error_name(err), driver_msg);
    } else {
        set_error(output, "%s failed (%s)", action, asio_error_name(err));
    }
}

static int equals_ignore_case(const char* a, const char* b) {
    if (!a || !b) return 0;
    return _stricmp(a, b) == 0;
}

static int contains_ignore_case(const char* haystack, const char* needle) {
    size_t hay_len;
    size_t needle_len;

    if (!haystack || !needle) return 0;
    hay_len = strlen(haystack);
    needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > hay_len) return 0;

    for (size_t i = 0; i <= hay_len - needle_len; ++i) {
        if (_strnicmp(haystack + i, needle, needle_len) == 0) return 1;
    }

    return 0;
}

static int clsid_from_ansi(const char* clsid_text, CLSID* clsid) {
    wchar_t wide[64];
    int len;

    if (!clsid_text || !clsid) return -1;

    len = MultiByteToWideChar(CP_ACP, 0, clsid_text, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
    if (len <= 0) return -1;

    return SUCCEEDED(CLSIDFromString(wide, clsid)) ? 0 : -1;
}

static int read_driver_clsid(HKEY asio_key, const char* driver_name, CLSID* clsid) {
    HKEY driver_key = NULL;
    char clsid_text[128];
    DWORD type = 0;
    DWORD size = sizeof(clsid_text);
    LONG status;

    status = RegOpenKeyExA(asio_key, driver_name, 0, KEY_READ, &driver_key);
    if (status != ERROR_SUCCESS) return -1;

    status = RegQueryValueExA(driver_key, "CLSID", NULL, &type, (LPBYTE)clsid_text, &size);
    RegCloseKey(driver_key);

    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return -1;
    clsid_text[sizeof(clsid_text) - 1] = '\0';

    return clsid_from_ansi(clsid_text, clsid);
}

static int find_driver_in_root(HKEY root, const char* requested_name, int fuzzy, char* out_name, size_t out_name_size, CLSID* out_clsid) {
    HKEY asio_key = NULL;
    DWORD index = 0;
    LONG status;

    status = RegOpenKeyExA(root, "SOFTWARE\\ASIO", 0, KEY_READ, &asio_key);
    if (status != ERROR_SUCCESS) return -1;

    for (;;) {
        char name[256];
        DWORD name_len = (DWORD)sizeof(name);
        CLSID clsid;

        status = RegEnumKeyExA(asio_key, index++, name, &name_len, NULL, NULL, NULL, NULL);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;

        name[sizeof(name) - 1] = '\0';
        if (read_driver_clsid(asio_key, name, &clsid) != 0) continue;

        if (!requested_name ||
            (!fuzzy && equals_ignore_case(name, requested_name)) ||
            (fuzzy && contains_ignore_case(name, requested_name))) {
            snprintf(out_name, out_name_size, "%s", name);
            *out_clsid = clsid;
            RegCloseKey(asio_key);
            return 0;
        }
    }

    RegCloseKey(asio_key);
    return -1;
}

static int find_asio_driver(const char* requested_name, char* out_name, size_t out_name_size, CLSID* out_clsid) {
    if (find_driver_in_root(HKEY_LOCAL_MACHINE, requested_name, 0, out_name, out_name_size, out_clsid) == 0) return 0;
    if (find_driver_in_root(HKEY_CURRENT_USER, requested_name, 0, out_name, out_name_size, out_clsid) == 0) return 0;

    if (requested_name) {
        if (find_driver_in_root(HKEY_LOCAL_MACHINE, requested_name, 1, out_name, out_name_size, out_clsid) == 0) return 0;
        if (find_driver_in_root(HKEY_CURRENT_USER, requested_name, 1, out_name, out_name_size, out_clsid) == 0) return 0;
    }

    return -1;
}

static int enumerate_asio_root(HKEY root, AsioDriverListCallback callback, void* user_data) {
    HKEY asio_key = NULL;
    DWORD index = 0;
    int count = 0;
    LONG status;

    status = RegOpenKeyExA(root, "SOFTWARE\\ASIO", 0, KEY_READ, &asio_key);
    if (status != ERROR_SUCCESS) return 0;

    for (;;) {
        char name[256];
        DWORD name_len = (DWORD)sizeof(name);
        CLSID clsid;

        status = RegEnumKeyExA(asio_key, index++, name, &name_len, NULL, NULL, NULL, NULL);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;

        name[sizeof(name) - 1] = '\0';
        if (read_driver_clsid(asio_key, name, &clsid) != 0) continue;

        if (callback) callback(name, user_data);
        ++count;
    }

    RegCloseKey(asio_key);
    return count;
}

int asio_output_list_drivers(AsioDriverListCallback callback, void* user_data) {
    int count = 0;
    count += enumerate_asio_root(HKEY_LOCAL_MACHINE, callback, user_data);
    count += enumerate_asio_root(HKEY_CURRENT_USER, callback, user_data);
    return count;
}

static const char* sample_type_name(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB: return "ASIOSTInt16LSB";
        case ASIOSTInt24LSB: return "ASIOSTInt24LSB";
        case ASIOSTInt32LSB: return "ASIOSTInt32LSB";
        case ASIOSTFloat32LSB: return "ASIOSTFloat32LSB";
        case ASIOSTFloat64LSB: return "ASIOSTFloat64LSB";
        case ASIOSTInt32LSB16: return "ASIOSTInt32LSB16";
        case ASIOSTInt32LSB18: return "ASIOSTInt32LSB18";
        case ASIOSTInt32LSB20: return "ASIOSTInt32LSB20";
        case ASIOSTInt32LSB24: return "ASIOSTInt32LSB24";
        default: return "unsupported ASIO sample type";
    }
}

static int sample_type_bytes(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB:
            return 2;
        case ASIOSTInt24LSB:
            return 3;
        case ASIOSTInt32LSB:
        case ASIOSTFloat32LSB:
        case ASIOSTInt32LSB16:
        case ASIOSTInt32LSB18:
        case ASIOSTInt32LSB20:
        case ASIOSTInt32LSB24:
            return 4;
        case ASIOSTFloat64LSB:
            return 8;
        default:
            return 0;
    }
}

static int sample_type_supported(ASIOSampleType type) {
    return sample_type_bytes(type) != 0;
}

static int32_t float_to_signed_bits(float sample, int bits) {
    double scale = (double)(1ULL << (bits - 1));
    double value = (double)sample * scale;
    double max_value = scale - 1.0;
    double min_value = -scale;

    if (value >= max_value) return (int32_t)max_value;
    if (value <= min_value) return (int32_t)min_value;
    return (int32_t)value;
}

static void write_sample(void* dst, ASIOSampleType type, long frame, float sample) {
    switch (type) {
        case ASIOSTFloat32LSB:
            ((float*)dst)[frame] = sample;
            break;
        case ASIOSTFloat64LSB:
            ((double*)dst)[frame] = (double)sample;
            break;
        case ASIOSTInt16LSB:
            ((int16_t*)dst)[frame] = (int16_t)float_to_signed_bits(sample, 16);
            break;
        case ASIOSTInt24LSB: {
            uint8_t* bytes = ((uint8_t*)dst) + frame * 3;
            int32_t value = float_to_signed_bits(sample, 24);
            bytes[0] = (uint8_t)(value & 0xFF);
            bytes[1] = (uint8_t)((value >> 8) & 0xFF);
            bytes[2] = (uint8_t)((value >> 16) & 0xFF);
            break;
        }
        case ASIOSTInt32LSB:
            ((int32_t*)dst)[frame] = float_to_signed_bits(sample, 32);
            break;
        case ASIOSTInt32LSB16:
            ((int32_t*)dst)[frame] = float_to_signed_bits(sample, 16);
            break;
        case ASIOSTInt32LSB18:
            ((int32_t*)dst)[frame] = float_to_signed_bits(sample, 18);
            break;
        case ASIOSTInt32LSB20:
            ((int32_t*)dst)[frame] = float_to_signed_bits(sample, 20);
            break;
        case ASIOSTInt32LSB24:
            ((int32_t*)dst)[frame] = float_to_signed_bits(sample, 24);
            break;
        default:
            break;
    }
}

static void clear_asio_buffer(AsioOutput* output, long double_buffer_index) {
    if (!output || double_buffer_index < 0 || double_buffer_index > 1) return;

    for (uint16_t ch = 0; ch < output->format.num_channels; ++ch) {
        int bytes_per_sample = sample_type_bytes(output->channel_infos[ch].type);
        if (bytes_per_sample > 0 && output->buffer_infos[ch].buffers[double_buffer_index]) {
            memset(output->buffer_infos[ch].buffers[double_buffer_index],
                   0,
                   (size_t)output->buffer_size * (size_t)bytes_per_sample);
        }
    }
}

static void render_asio_buffer(AsioOutput* output, long double_buffer_index) {
    size_t frame_bytes;
    size_t bytes_needed;
    size_t bytes_read;
    const float* src;

    if (!output || double_buffer_index < 0 || double_buffer_index > 1) return;

    frame_bytes = (size_t)output->format.num_channels * sizeof(float);
    bytes_needed = (size_t)output->buffer_size * frame_bytes;

    if (output->conv_buffer_samples < (size_t)output->buffer_size * output->format.num_channels) {
        clear_asio_buffer(output, double_buffer_index);
        return;
    }

    bytes_read = ring_buffer_read(output->ring_buffer, output->conv_buffer, bytes_needed);
    if (bytes_read < bytes_needed) {
        memset(((uint8_t*)output->conv_buffer) + bytes_read, 0, bytes_needed - bytes_read);
    }

    src = output->conv_buffer;
    for (uint16_t ch = 0; ch < output->format.num_channels; ++ch) {
        void* dst = output->buffer_infos[ch].buffers[double_buffer_index];
        ASIOSampleType type = output->channel_infos[ch].type;

        if (!dst) continue;

        for (long frame = 0; frame < output->buffer_size; ++frame) {
            write_sample(dst, type, frame, src[(size_t)frame * output->format.num_channels + ch]);
        }
    }
}

static void asio_buffer_switch(long double_buffer_index, ASIOBool direct_process) {
    AsioOutput* output = g_active_output;
    (void)direct_process;

    if (!output) return;

    render_asio_buffer(output, double_buffer_index);

    if (output->supports_output_ready && output->driver) {
        output->driver->outputReady();
    }
}

static void asio_sample_rate_did_change(ASIOSampleRate sample_rate) {
    (void)sample_rate;
}

static long asio_message(long selector, long value, void* message, double* opt) {
    (void)message;
    (void)opt;

    switch (selector) {
        case kAsioSelectorSupported:
            switch (value) {
                case kAsioEngineVersion:
                case kAsioSupportsTimeInfo:
                case kAsioResetRequest:
                case kAsioResyncRequest:
                case kAsioLatenciesChanged:
                case kAsioOverload:
                    return 1;
                default:
                    return 0;
            }
        case kAsioEngineVersion:
            return 2;
        case kAsioSupportsTimeInfo:
            return 1;
        case kAsioResetRequest:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
            return 1;
        case kAsioBufferSizeChange:
        case kAsioOverload:
        default:
            return 0;
    }
}

static ASIOTime* asio_buffer_switch_time_info(ASIOTime* params, long double_buffer_index, ASIOBool direct_process) {
    asio_buffer_switch(double_buffer_index, direct_process);
    return params;
}

static int is_power_of_two(long value) {
    return value > 0 && (value & (value - 1)) == 0;
}

static int choose_buffer_size(AsioOutput* output, const AsioConfig* config) {
    long requested = config ? config->requested_buffer_size : 0;

    if (requested > 0) {
        int valid = requested >= output->min_buffer_size && requested <= output->max_buffer_size;
        if (valid && output->granularity > 0) {
            valid = ((requested - output->min_buffer_size) % output->granularity) == 0;
        } else if (valid && output->granularity == -1) {
            valid = is_power_of_two(requested);
        }

        if (!valid) {
            set_error(output,
                      "ASIO buffer size %ld is not supported by %s (min %ld, max %ld, preferred %ld)",
                      requested,
                      output->driver_name,
                      output->min_buffer_size,
                      output->max_buffer_size,
                      output->preferred_buffer_size);
            return -1;
        }

        output->buffer_size = requested;
        return 0;
    }

    output->buffer_size = output->preferred_buffer_size > 0
                        ? output->preferred_buffer_size
                        : output->min_buffer_size;
    return output->buffer_size > 0 ? 0 : -1;
}

static int configure_sample_rate(AsioOutput* output, uint32_t sample_rate) {
    ASIOSampleRate desired = (ASIOSampleRate)sample_rate;
    ASIOSampleRate current = 0.0;
    ASIOError err;

    err = output->driver->canSampleRate(desired);
    if (err != ASE_OK) {
        set_driver_error(output, "ASIO canSampleRate", err);
        return -1;
    }

    err = output->driver->getSampleRate(&current);
    if (err == ASE_OK && fabs(current - desired) < 0.5) return 0;

    err = output->driver->setSampleRate(desired);
    if (err != ASE_OK) {
        set_driver_error(output, "ASIO setSampleRate", err);
        return -1;
    }

    return 0;
}

AsioOutput* asio_output_create(const AudioFormat* format, const AsioConfig* config) {
    AsioOutput* output;
    HRESULT hr;
    ASIOError err;
    char registry_name[128] = {0};
    long offset;

    if (!format) return NULL;
    if (g_active_output) {
        set_error(NULL, "Only one ASIO output instance can be active at a time");
        return NULL;
    }

    output = (AsioOutput*)calloc(1, sizeof(AsioOutput));
    if (!output) {
        set_error(NULL, "Failed to allocate ASIO output");
        return NULL;
    }

    memcpy(&output->format, format, sizeof(AudioFormat));
    output->state = AUDIO_STATE_STOPPED;
    output->callbacks.bufferSwitch = asio_buffer_switch;
    output->callbacks.sampleRateDidChange = asio_sample_rate_did_change;
    output->callbacks.asioMessage = asio_message;
    output->callbacks.bufferSwitchTimeInfo = asio_buffer_switch_time_info;

    offset = config ? config->output_channel_offset : 0;
    output->output_channel_offset = offset > 0 ? offset : 0;

    if (find_asio_driver(config ? config->driver_name : NULL,
                         registry_name,
                         sizeof(registry_name),
                         &output->clsid) != 0) {
        if (config && config->driver_name) {
            set_error(output, "ASIO driver not found: %s", config->driver_name);
        } else {
            set_error(output, "No registered ASIO drivers found");
        }
        asio_output_close(output);
        return NULL;
    }

    snprintf(output->driver_name, sizeof(output->driver_name), "%s", registry_name);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        output->com_initialized = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        set_error(output, "Failed to initialize COM for ASIO (HRESULT 0x%08lX)", (unsigned long)hr);
        asio_output_close(output);
        return NULL;
    }

    hr = CoCreateInstance(output->clsid, NULL, CLSCTX_INPROC_SERVER, output->clsid, (void**)&output->driver);
    if (FAILED(hr) || !output->driver) {
        set_error(output, "Failed to instantiate ASIO driver %s (HRESULT 0x%08lX)", output->driver_name, (unsigned long)hr);
        asio_output_close(output);
        return NULL;
    }

    if (output->driver->init(GetConsoleWindow()) != ASIOTrue) {
        set_driver_error(output, "ASIO init", ASE_HWMalfunction);
        asio_output_close(output);
        return NULL;
    }

    {
        char runtime_name[32] = {0};
        output->driver->getDriverName(runtime_name);
        if (runtime_name[0]) {
            snprintf(output->driver_name, sizeof(output->driver_name), "%s", runtime_name);
        }
    }

    err = output->driver->getChannels(&output->input_channels, &output->output_channels);
    if (err != ASE_OK) {
        set_driver_error(output, "ASIO getChannels", err);
        asio_output_close(output);
        return NULL;
    }

    if (output->output_channels < output->output_channel_offset + format->num_channels) {
        set_error(output,
                  "ASIO driver %s exposes %ld output channels, need %u from offset %ld",
                  output->driver_name,
                  output->output_channels,
                  format->num_channels,
                  output->output_channel_offset);
        asio_output_close(output);
        return NULL;
    }

    if (configure_sample_rate(output, format->sample_rate) != 0) {
        asio_output_close(output);
        return NULL;
    }

    err = output->driver->getBufferSize(&output->min_buffer_size,
                                        &output->max_buffer_size,
                                        &output->preferred_buffer_size,
                                        &output->granularity);
    if (err != ASE_OK) {
        set_driver_error(output, "ASIO getBufferSize", err);
        asio_output_close(output);
        return NULL;
    }

    if (choose_buffer_size(output, config) != 0) {
        asio_output_close(output);
        return NULL;
    }

    output->buffer_infos = (ASIOBufferInfo*)calloc(format->num_channels, sizeof(ASIOBufferInfo));
    output->channel_infos = (ASIOChannelInfo*)calloc(format->num_channels, sizeof(ASIOChannelInfo));
    output->conv_buffer_samples = (size_t)output->buffer_size * format->num_channels;
    output->conv_buffer = (float*)calloc(output->conv_buffer_samples, sizeof(float));
    if (!output->buffer_infos || !output->channel_infos || !output->conv_buffer) {
        set_error(output, "Failed to allocate ASIO buffers");
        asio_output_close(output);
        return NULL;
    }

    for (uint16_t ch = 0; ch < format->num_channels; ++ch) {
        output->buffer_infos[ch].isInput = ASIOFalse;
        output->buffer_infos[ch].channelNum = output->output_channel_offset + ch;

        output->channel_infos[ch].channel = output->output_channel_offset + ch;
        output->channel_infos[ch].isInput = ASIOFalse;

        err = output->driver->getChannelInfo(&output->channel_infos[ch]);
        if (err != ASE_OK) {
            set_driver_error(output, "ASIO getChannelInfo", err);
            asio_output_close(output);
            return NULL;
        }

        if (!sample_type_supported(output->channel_infos[ch].type)) {
            set_error(output,
                      "ASIO output channel %ld uses unsupported sample type %ld (%s)",
                      output->channel_infos[ch].channel,
                      output->channel_infos[ch].type,
                      sample_type_name(output->channel_infos[ch].type));
            asio_output_close(output);
            return NULL;
        }
    }

    size_t ring_bytes = (size_t)format->sample_rate * format->num_channels * sizeof(float) * 2;
    output->ring_buffer = ring_buffer_create(ring_bytes);
    if (!output->ring_buffer) {
        set_error(output, "Failed to allocate ASIO ring buffer");
        asio_output_close(output);
        return NULL;
    }

    g_active_output = output;
    err = output->driver->createBuffers(output->buffer_infos,
                                        format->num_channels,
                                        output->buffer_size,
                                        &output->callbacks);
    if (err != ASE_OK) {
        g_active_output = NULL;
        set_driver_error(output, "ASIO createBuffers", err);
        asio_output_close(output);
        return NULL;
    }

    output->buffers_created = 1;
    clear_asio_buffer(output, 0);
    clear_asio_buffer(output, 1);

    err = output->driver->getLatencies(&output->input_latency, &output->output_latency);
    if (err != ASE_OK) {
        output->input_latency = 0;
        output->output_latency = output->buffer_size;
    }

    err = output->driver->outputReady();
    output->supports_output_ready = (err == ASE_OK || err == ASE_SUCCESS);

    g_last_error[0] = '\0';
    return output;
}

int asio_output_start(AsioOutput* output) {
    ASIOError err;

    if (!output) return -1;
    if (output->state == AUDIO_STATE_PLAYING) return 0;

    clear_asio_buffer(output, 0);
    clear_asio_buffer(output, 1);

    err = output->driver->start();
    if (err != ASE_OK) {
        set_driver_error(output, "ASIO start", err);
        return -1;
    }

    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int asio_output_stop(AsioOutput* output) {
    ASIOError err = ASE_OK;

    if (!output) return -1;
    if (output->state == AUDIO_STATE_STOPPED) return 0;

    if (output->state == AUDIO_STATE_PLAYING) {
        err = output->driver->stop();
    }

    if (output->ring_buffer) ring_buffer_cancel(output->ring_buffer);
    output->state = AUDIO_STATE_STOPPED;

    if (err != ASE_OK) {
        set_driver_error(output, "ASIO stop", err);
        return -1;
    }

    return 0;
}

int asio_output_pause(AsioOutput* output) {
    ASIOError err;

    if (!output || output->state != AUDIO_STATE_PLAYING) return -1;

    err = output->driver->stop();
    if (err != ASE_OK) {
        set_driver_error(output, "ASIO pause", err);
        return -1;
    }

    output->state = AUDIO_STATE_PAUSED;
    return 0;
}

int asio_output_resume(AsioOutput* output) {
    ASIOError err;

    if (!output || output->state != AUDIO_STATE_PAUSED) return -1;

    err = output->driver->start();
    if (err != ASE_OK) {
        set_driver_error(output, "ASIO resume", err);
        return -1;
    }

    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int asio_output_write(AsioOutput* output, const void* data, size_t num_samples) {
    size_t bytes_to_write;
    size_t written;

    if (!output || !data) return -1;

    bytes_to_write = num_samples * output->format.num_channels * sizeof(float);
    written = ring_buffer_write(output->ring_buffer, data, bytes_to_write);

    return (int)(written / (output->format.num_channels * sizeof(float)));
}

AudioState asio_output_get_state(AsioOutput* output) {
    if (!output) return AUDIO_STATE_STOPPED;
    return output->state;
}

int asio_output_get_available_frames(AsioOutput* output) {
    size_t capacity;
    size_t filled;
    size_t bytes_free;
    size_t frame_bytes;

    if (!output || !output->ring_buffer) return -1;

    capacity = ring_buffer_get_capacity(output->ring_buffer);
    filled = ring_buffer_available(output->ring_buffer);
    bytes_free = capacity - filled;
    frame_bytes = output->format.num_channels * sizeof(float);

    return (int)(bytes_free / frame_bytes);
}

const char* asio_output_get_error(AsioOutput* output) {
    if (!output) return g_last_error[0] ? g_last_error : "Invalid ASIO output handle";
    return output->error_msg[0] ? output->error_msg : NULL;
}

const char* asio_output_get_last_error(void) {
    return g_last_error[0] ? g_last_error : NULL;
}

void asio_output_close(AsioOutput* output) {
    if (!output) return;

    if (output->state != AUDIO_STATE_STOPPED) {
        asio_output_stop(output);
    }

    if (g_active_output == output) {
        g_active_output = NULL;
    }

    if (output->driver && output->buffers_created) {
        output->driver->disposeBuffers();
        output->buffers_created = 0;
    }

    if (output->driver) {
        output->driver->Release();
        output->driver = NULL;
    }

    if (output->ring_buffer) {
        ring_buffer_destroy(output->ring_buffer);
        output->ring_buffer = NULL;
    }

    free(output->buffer_infos);
    free(output->channel_infos);
    free(output->conv_buffer);

    if (output->com_initialized) {
        CoUninitialize();
    }

    free(output);
}

#endif /* _WIN32 */
