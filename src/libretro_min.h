// The subset of the libretro API that this player uses, taken from libretro.h
// (https://github.com/libretro/libretro-common, include/libretro.h).
// Values match upstream; only what the player needs is declared.
//
// Copyright (C) 2010-2020 The RetroArch team
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this libretro API header ("libretro.h"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify,
// merge, publish, distribute, sublicense, and/or sell copies of the Software,
// and to permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#pragma once

#include <cstddef>
#include <cstdint>

#define RETRO_API_VERSION 1

#define RETRO_DEVICE_JOYPAD 1
#define RETRO_DEVICE_ID_JOYPAD_B 0
#define RETRO_DEVICE_ID_JOYPAD_Y 1
#define RETRO_DEVICE_ID_JOYPAD_SELECT 2
#define RETRO_DEVICE_ID_JOYPAD_START 3
#define RETRO_DEVICE_ID_JOYPAD_UP 4
#define RETRO_DEVICE_ID_JOYPAD_DOWN 5
#define RETRO_DEVICE_ID_JOYPAD_LEFT 6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT 7
#define RETRO_DEVICE_ID_JOYPAD_A 8
#define RETRO_DEVICE_ID_JOYPAD_X 9
#define RETRO_DEVICE_ID_JOYPAD_L 10
#define RETRO_DEVICE_ID_JOYPAD_R 11
#define RETRO_DEVICE_ID_JOYPAD_L2 12
#define RETRO_DEVICE_ID_JOYPAD_R2 13
#define RETRO_DEVICE_ID_JOYPAD_L3 14
#define RETRO_DEVICE_ID_JOYPAD_R3 15
#define RETRO_DEVICE_ID_JOYPAD_MASK 256

#define RETRO_MEMORY_SAVE_RAM 0

#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000
#define RETRO_ENVIRONMENT_SET_ROTATION 1
#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3
#define RETRO_ENVIRONMENT_SET_MESSAGE 6
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS 11
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_SET_VARIABLES 16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME 18
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO 32
#define RETRO_ENVIRONMENT_SET_GEOMETRY 37
#define RETRO_ENVIRONMENT_GET_LANGUAGE 39
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565 = 2,
};

enum retro_log_level { RETRO_LOG_DEBUG = 0, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR };

typedef void (*retro_log_printf_t)(enum retro_log_level level, const char* fmt, ...);
struct retro_log_callback { retro_log_printf_t log; };

struct retro_message { const char* msg; unsigned frames; };
struct retro_variable { const char* key; const char* value; };

struct retro_system_info {
    const char* library_name;
    const char* library_version;
    const char* valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float aspect_ratio;
};

struct retro_system_timing { double fps; double sample_rate; };

struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};

struct retro_game_info {
    const char* path;
    const void* data;
    size_t size;
    const char* meta;
};

typedef bool (*retro_environment_t)(unsigned cmd, void* data);
typedef void (*retro_video_refresh_t)(const void* data, unsigned width, unsigned height, size_t pitch);
typedef void (*retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t* data, size_t frames);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

// Hardware (GPU) rendering, from libretro.h. The core draws with OpenGL ES into
// a framebuffer the front-end provides, then calls video_refresh with
// RETRO_HW_FRAME_BUFFER_VALID instead of pixels.
#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000
#define RETRO_ENVIRONMENT_SET_HW_RENDER 14
#define RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE (41 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE (43 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT (44 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER 56
#define RETRO_HW_FRAME_BUFFER_VALID ((void*)-1)

enum retro_hw_context_type {
    RETRO_HW_CONTEXT_NONE = 0,
    RETRO_HW_CONTEXT_OPENGL = 1,
    RETRO_HW_CONTEXT_OPENGLES2 = 2,
    RETRO_HW_CONTEXT_OPENGL_CORE = 3,
    RETRO_HW_CONTEXT_OPENGLES3 = 4,
    RETRO_HW_CONTEXT_OPENGLES_VERSION = 5,
    RETRO_HW_CONTEXT_VULKAN = 6,
    RETRO_HW_CONTEXT_DIRECT3D = 7,
    RETRO_HW_CONTEXT_DUMMY = 0x7fffffff
};

typedef void (*retro_proc_address_t)(void);
typedef void (*retro_hw_context_reset_t)(void);
typedef uintptr_t (*retro_hw_get_current_framebuffer_t)(void);
typedef retro_proc_address_t (*retro_hw_get_proc_address_t)(const char* sym);

struct retro_hw_render_callback {
    enum retro_hw_context_type context_type;
    retro_hw_context_reset_t context_reset;
    retro_hw_get_current_framebuffer_t get_current_framebuffer;
    retro_hw_get_proc_address_t get_proc_address;
    bool depth;
    bool stencil;
    bool bottom_left_origin;
    unsigned version_major;
    unsigned version_minor;
    bool cache_context;
    retro_hw_context_reset_t context_destroy;
    bool debug_context;
};
