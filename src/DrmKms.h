#pragma once

// Shared DRM/KMS UAPI glue. We use raw ioctls (no libdrm in the sysroot) and
// only the mode structs from drm_mode.h — the legacy drm.h has a `virtual`
// field that won't compile as C++. The mode ioctl numbers live in drm.h, so we
// define the few we need here (each is just DRM_IOWR('d', nr, struct)).

// drm_mode.h uses uint32_t/uint64_t without including anything itself.
#include <cstdint>

#include "vendor/drm/drm_mode.h"

#include <sys/ioctl.h>

#ifndef DRM_IOCTL_MODE_GETRESOURCES
#define DRM_IO_WR(nr, type) _IOWR('d', nr, type)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IO_WR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      DRM_IO_WR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IO_WR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   DRM_IO_WR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IO_WR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB        DRM_IO_WR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB         DRM_IO_WR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IO_WR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IO_WR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IO_WR(0xB4, struct drm_mode_destroy_dumb)
#endif
