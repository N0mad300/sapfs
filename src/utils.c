#include "utils.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void utils_convert_8bit_to_16bit(const uint8_t* input, int16_t* output, size_t num_samples) {
    if (!input || !output) {
        return;
    }
    
    for (size_t i = 0; i < num_samples; i++) {
        /* Convert unsigned 8-bit (0-255) to signed 16-bit (-32768 to 32767) */
        /* Center point: 128 -> 0 */
        int16_t value = (int16_t)(input[i] - 128) * 256;
        output[i] = value;
    }
}

double utils_calculate_rms_16bit(const int16_t* samples, size_t num_samples, int num_channels) {
    if (!samples || num_samples == 0) {
        return 0.0;
    }
    
    double sum_squares = 0.0;
    size_t total_samples = num_samples * num_channels;
    
    for (size_t i = 0; i < total_samples; i++) {
        double normalized = (double)samples[i] / 32768.0; /* Normalize to -1.0 to 1.0 */
        sum_squares += normalized * normalized;
    }
    
    double mean_square = sum_squares / total_samples;
    return sqrt(mean_square);
}

void utils_normalize_16bit(int16_t* samples, size_t num_samples, int num_channels, double target_rms) {
    if (!samples || num_samples == 0 || target_rms <= 0.0) {
        return;
    }
    
    /* Calculate current RMS */
    double current_rms = utils_calculate_rms_16bit(samples, num_samples, num_channels);
    
    if (current_rms < 0.0001) {
        /* Audio is essentially silent, don't normalize */
        return;
    }
    
    /* Calculate gain factor */
    double gain = target_rms / current_rms;
    
    /* Limit gain to prevent excessive amplification */
    if (gain > 10.0) {
        gain = 10.0;
    }
    
    /* Apply gain to all samples */
    size_t total_samples = num_samples * num_channels;
    for (size_t i = 0; i < total_samples; i++) {
        double value = (double)samples[i] * gain;
        
        /* Clamp to prevent clipping */
        if (value > 32767.0) {
            value = 32767.0;
        } else if (value < -32768.0) {
            value = -32768.0;
        }
        
        samples[i] = (int16_t)value;
    }
}

void utils_format_time(double seconds, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 16) {
        return;
    }
    
    int hours = (int)seconds / 3600;
    int minutes = ((int)seconds % 3600) / 60;
    int secs = (int)seconds % 60;
    
    if (hours > 0) {
        snprintf(buffer, buffer_size, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        snprintf(buffer, buffer_size, "%d:%02d", minutes, secs);
    }
}

void utils_format_size(uint64_t bytes, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 32) {
        return;
    }
    
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = (double)bytes;
    
    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }
    
    if (unit_index == 0) {
        snprintf(buffer, buffer_size, "%llu %s", (unsigned long long)bytes, units[unit_index]);
    } else {
        snprintf(buffer, buffer_size, "%.2f %s", size, units[unit_index]);
    }
}