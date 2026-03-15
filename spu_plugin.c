///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SPU/VAG Playback Plugin
//
// Implements RVPlaybackPlugin interface for PlayStation SPU ADPCM audio files.
// Supported formats: VAG (with header), VB (raw ADPCM blocks)
// Self-contained decoder - no external library needed.
//
// PS-ADPCM format: each 16-byte block decodes to 28 S16 samples.
// Block format:
//   Byte 0: shift (bits 0-3) + filter (bits 4-6)
//   Byte 1: flags (loop markers: 1=loop end, 2=loop region, 4=loop start)
//   Bytes 2-15: packed 4-bit nibbles (28 samples, low nibble first)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef nullptr
#define nullptr ((void*)0)
#endif

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define SPU_DEFAULT_SAMPLE_RATE 44100
#define SPU_VAG_HEADER_SIZE 48
#define SPU_ADPCM_BLOCK_SIZE 16
#define SPU_SAMPLES_PER_BLOCK 28

#define SPU_SCOPE_BUFFER_SIZE 1024
#define SPU_SCOPE_BUFFER_MASK (SPU_SCOPE_BUFFER_SIZE - 1)

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// PS-ADPCM filter coefficients (fixed-point, 5 filters)

static const int s_adpcm_coeff[5][2] = {
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 },
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct SpuReplayerData {
    uint8_t* file_data;
    size_t file_size;
    uint8_t* adpcm_data;
    size_t adpcm_size;
    size_t current_offset;
    uint32_t sample_rate;
    int32_t prev1;
    int32_t prev2;
    char name[17];
    bool has_vag_header;
    bool scope_enabled;
    float scope_buffer[SPU_SCOPE_BUFFER_SIZE];
    uint32_t scope_write_pos;
} SpuReplayerData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* spu_plugin_supported_extensions(void) {
    return "vag,vb,spu";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void spu_plugin_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* spu_plugin_create(const RVService* service_api) {
    SpuReplayerData* data = malloc(sizeof(SpuReplayerData));
    if (data == nullptr) {
        return nullptr;
    }
    memset(data, 0, sizeof(SpuReplayerData));

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int spu_plugin_destroy(void* user_data) {
    SpuReplayerData* data = (SpuReplayerData*)user_data;
    if (data->file_data != nullptr) {
        rv_io_free_url_to_memory(data->file_data);
    }
    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult spu_plugin_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url,
                                               uint64_t total_size) {
    (void)url;
    (void)total_size;

    // Check for "VAGp" magic at offset 0
    if (data_size >= 4 && probe_data[0] == 'V' && probe_data[1] == 'A' && probe_data[2] == 'G'
        && probe_data[3] == 'p') {
        return RVProbeResult_Supported;
    }

    // For .vb files (raw ADPCM), return Unsure based on extension
    if (url != nullptr) {
        const char* dot = strrchr(url, '.');
        if (dot != nullptr) {
            if (strcasecmp(dot, ".vag") == 0 || strcasecmp(dot, ".vb") == 0) {
                return RVProbeResult_Unsure;
            }
        }
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Read a big-endian uint32 from a byte buffer

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int spu_plugin_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)subsong;
    (void)service_api;

    SpuReplayerData* data = (SpuReplayerData*)user_data;

    RVIoReadUrlResult read_res;
    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("SPU: Failed to load %s to memory", url);
        return -1;
    }

    // Free previous data
    if (data->file_data != nullptr) {
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
    }

    data->file_data = read_res.data;
    data->file_size = read_res.data_size;
    data->prev1 = 0;
    data->prev2 = 0;
    memset(data->name, 0, sizeof(data->name));

    // Check for VAG header
    if (data->file_size >= SPU_VAG_HEADER_SIZE && data->file_data[0] == 'V' && data->file_data[1] == 'A'
        && data->file_data[2] == 'G' && data->file_data[3] == 'p') {
        data->has_vag_header = true;
        data->sample_rate = read_be32(data->file_data + 16);
        if (data->sample_rate == 0 || data->sample_rate > 96000) {
            data->sample_rate = SPU_DEFAULT_SAMPLE_RATE;
        }

        // Extract name from header (16 bytes at offset 32)
        memcpy(data->name, data->file_data + 32, 16);
        data->name[16] = '\0';

        data->adpcm_data = data->file_data + SPU_VAG_HEADER_SIZE;
        data->adpcm_size = data->file_size - SPU_VAG_HEADER_SIZE;
    } else {
        // Raw ADPCM (VB files)
        data->has_vag_header = false;
        data->sample_rate = SPU_DEFAULT_SAMPLE_RATE;
        data->adpcm_data = data->file_data;
        data->adpcm_size = data->file_size;
    }

    data->current_offset = 0;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void spu_plugin_close(void* user_data) {
    SpuReplayerData* data = (SpuReplayerData*)user_data;

    if (data->file_data != nullptr) {
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
    }
    data->adpcm_data = nullptr;
    data->adpcm_size = 0;
    data->current_offset = 0;
    data->prev1 = 0;
    data->prev2 = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Decode one 16-byte ADPCM block into 28 S16 samples

static int spu_decode_block(const uint8_t* block, int16_t* out, int32_t* prev1, int32_t* prev2) {
    int shift = block[0] & 0x0F;
    int filter = (block[0] >> 4) & 0x07;
    int flags = block[1];

    if (filter > 4) {
        filter = 4;
    }

    int coeff1 = s_adpcm_coeff[filter][0];
    int coeff2 = s_adpcm_coeff[filter][1];

    for (int i = 0; i < SPU_SAMPLES_PER_BLOCK; i++) {
        int byte_index = 2 + (i / 2);
        int nibble;
        if ((i & 1) == 0) {
            nibble = block[byte_index] & 0x0F;
        } else {
            nibble = (block[byte_index] >> 4) & 0x0F;
        }

        // Sign-extend 4-bit nibble to int
        if (nibble >= 8) {
            nibble -= 16;
        }

        // Apply shift and filter
        int32_t sample = (nibble << (12 - shift)) + ((*prev1 * coeff1 + *prev2 * coeff2 + 32) >> 6);

        // Clamp to S16 range
        if (sample > 32767) {
            sample = 32767;
        }
        if (sample < -32768) {
            sample = -32768;
        }

        out[i] = (int16_t)sample;
        *prev2 = *prev1;
        *prev1 = sample;
    }

    return flags;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo spu_plugin_read_data(void* user_data, RVReadData dest) {
    SpuReplayerData* data = (SpuReplayerData*)user_data;

    // Report native format: S16 mono at the file's sample rate
    RVAudioFormat format = { RVAudioStreamFormat_S16, 1, data->sample_rate };

    if (data->adpcm_data == nullptr) {
        return (RVReadInfo) { format, 0, RVReadStatus_Error};
    }

    // Check if we've reached the end
    if (data->current_offset >= data->adpcm_size) {
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    uint32_t max_frames = dest.channels_output_max_bytes_size / sizeof(int16_t);
    int16_t* output = (int16_t*)dest.channels_output;
    uint32_t frames_written = 0;
    bool end_reached = false;

    while (frames_written < max_frames && !end_reached) {
        if (data->current_offset + SPU_ADPCM_BLOCK_SIZE > data->adpcm_size) {
            end_reached = true;
            break;
        }

        int16_t samples[SPU_SAMPLES_PER_BLOCK];
        int flags = spu_decode_block(data->adpcm_data + data->current_offset, samples, &data->prev1, &data->prev2);
        data->current_offset += SPU_ADPCM_BLOCK_SIZE;

        // Check for end flag (bit 0 set, bit 1 not set = end without loop)
        if ((flags & 1) && !(flags & 2)) {
            end_reached = true;
        }

        // Copy decoded S16 samples directly to mono output
        uint32_t samples_to_copy = SPU_SAMPLES_PER_BLOCK;
        if (frames_written + samples_to_copy > max_frames) {
            samples_to_copy = max_frames - frames_written;
        }

        memcpy(&output[frames_written], samples, samples_to_copy * sizeof(int16_t));

        // Convert to F32 for scope visualization
        if (data->scope_enabled) {
            for (uint32_t i = 0; i < samples_to_copy; i++) {
                data->scope_buffer[data->scope_write_pos & SPU_SCOPE_BUFFER_MASK]
                    = (float)samples[i] / 32768.0f;
                data->scope_write_pos++;
            }
        }

        frames_written += samples_to_copy;
    }

    RVReadStatus status;
    if (end_reached && frames_written == 0) {
        status = RVReadStatus_Finished;
    } else {
        status = RVReadStatus_Ok;
    }

    return (RVReadInfo) { format, frames_written, status};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t spu_plugin_seek(void* user_data, int64_t ms) {
    SpuReplayerData* data = (SpuReplayerData*)user_data;

    if (data->adpcm_data == nullptr) {
        return -1;
    }

    // Calculate target block from milliseconds
    uint64_t target_sample = (uint64_t)ms * data->sample_rate / 1000;
    uint64_t target_block = target_sample / SPU_SAMPLES_PER_BLOCK;
    size_t target_offset = target_block * SPU_ADPCM_BLOCK_SIZE;

    if (target_offset >= data->adpcm_size) {
        target_offset = data->adpcm_size;
    }

    // Reset decoder state and re-decode from start for accurate seeking
    data->prev1 = 0;
    data->prev2 = 0;
    data->current_offset = 0;

    // Decode blocks to rebuild filter state
    int16_t dummy[SPU_SAMPLES_PER_BLOCK];
    while (data->current_offset < target_offset) {
        if (data->current_offset + SPU_ADPCM_BLOCK_SIZE > data->adpcm_size) {
            break;
        }
        spu_decode_block(data->adpcm_data + data->current_offset, dummy, &data->prev1, &data->prev2);
        data->current_offset += SPU_ADPCM_BLOCK_SIZE;
    }

    return ms;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int spu_plugin_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res;
    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        return -1;
    }

    uint8_t* file_data = read_res.data;
    size_t file_size = read_res.data_size;

    RVMetadataId id = rv_metadata_create_url(url);
    rv_metadata_set_tag(id, RV_METADATA_SONGTYPE_TAG, "SPU/VAG");

    uint32_t sample_rate = SPU_DEFAULT_SAMPLE_RATE;
    size_t adpcm_size = file_size;

    // Parse VAG header if present
    if (file_size >= SPU_VAG_HEADER_SIZE && file_data[0] == 'V' && file_data[1] == 'A' && file_data[2] == 'G'
        && file_data[3] == 'p') {
        sample_rate = read_be32(file_data + 16);
        if (sample_rate == 0 || sample_rate > 96000) {
            sample_rate = SPU_DEFAULT_SAMPLE_RATE;
        }

        // Title from header
        char name[17] = { 0 };
        memcpy(name, file_data + 32, 16);
        if (name[0] != '\0') {
            rv_metadata_set_tag(id, RV_METADATA_TITLE_TAG, name);
        }

        adpcm_size = file_size - SPU_VAG_HEADER_SIZE;
    }

    // Calculate duration: (total_blocks * samples_per_block) / sample_rate
    size_t total_blocks = adpcm_size / SPU_ADPCM_BLOCK_SIZE;
    double total_samples = (double)(total_blocks * SPU_SAMPLES_PER_BLOCK);
    double length_seconds = total_samples / (double)sample_rate;
    if (length_seconds > 0.0) {
        rv_metadata_set_tag_f64(id, RV_METADATA_LENGTH_TAG, length_seconds);
    }

    rv_io_free_url_to_memory(read_res.data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void spu_plugin_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t spu_plugin_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    SpuReplayerData* data = (SpuReplayerData*)user_data;
    if (data == nullptr || data->adpcm_data == nullptr || buffer == nullptr) {
        return 0;
    }

    // SPU is mono - only channel 0
    if (channel != 0) {
        return 0;
    }

    if (!data->scope_enabled) {
        data->scope_enabled = true;
        memset(data->scope_buffer, 0, sizeof(data->scope_buffer));
        data->scope_write_pos = 0;
    }

    if (num_samples > SPU_SCOPE_BUFFER_SIZE) {
        num_samples = SPU_SCOPE_BUFFER_SIZE;
    }

    uint32_t read_pos = (data->scope_write_pos - num_samples + SPU_SCOPE_BUFFER_SIZE) & SPU_SCOPE_BUFFER_MASK;
    for (uint32_t i = 0; i < num_samples; i++) {
        buffer[i] = data->scope_buffer[read_pos];
        read_pos = (read_pos + 1) & SPU_SCOPE_BUFFER_MASK;
    }

    return num_samples;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t spu_plugin_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    (void)user_data;
    if (max_channels < 1)
        return 0;
    names[0] = "ADPCM";
    return 1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_spu_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "spu",
    "0.0.1",
    "built-in PS-ADPCM",
    spu_plugin_probe_can_play,
    spu_plugin_supported_extensions,
    spu_plugin_create,
    spu_plugin_destroy,
    spu_plugin_event,
    spu_plugin_open,
    spu_plugin_close,
    spu_plugin_read_data,
    spu_plugin_seek,
    spu_plugin_metadata,
    spu_plugin_static_init,
    nullptr, // settings_updated
    nullptr, // get_tracker_info
    nullptr, // get_pattern_cell
    nullptr, // get_pattern_num_rows
    spu_plugin_get_scope_data,
    nullptr, // static_destroy
    spu_plugin_get_scope_channel_names,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_spu_plugin;
}
