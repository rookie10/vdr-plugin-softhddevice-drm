///
///	@file video.h	@brief Video module header file
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 by zille.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

/// @addtogroup Video
/// @{

#ifndef __VIDEO_H
#define __VIDEO_H

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libavfilter/avfilter.h>

#ifdef USE_GLES
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#endif

#include "iatomic.h"
#include "softhddev.h"

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define VIDEO_SURFACES_MAX	3	///< video output surfaces for queue

#define VIDEO_PLANE		0
#define OSD_PLANE		1
#define MAX_PLANES		2

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------
#ifdef MMAL
    /// Video hardware decoder typedef
typedef struct _Mmal_Render_ VideoRender;
#else
struct drm_buf {
	uint32_t width, height, size, pitch[4], handle[4], offset[4], fb_id;
	uint8_t *plane[4];
	uint32_t pix_fmt;
	int fd_prime;
	AVFrame *frame;
	int dirty;
#ifdef USE_GLES
	struct gbm_bo *bo;
#endif
};

struct plane {
	uint32_t plane_id;
	drmModePlane *plane;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
};

struct _Drm_Render_
{
	AVFrame  *FramesDeintRb[VIDEO_SURFACES_MAX];
	int FramesDeintWrite;			///< write pointer
	int FramesDeintRead;			///< read pointer
	atomic_t FramesDeintFilled;		///< how many of the buffer is used

	AVFrame  *FramesRb[VIDEO_SURFACES_MAX];
	int FramesWrite;			///< write pointer
	int FramesRead;			///< read pointer
	atomic_t FramesFilled;		///< how many of the buffer is used

	VideoStream *Stream;		///< video stream
	int TrickSpeed;			///< current trick speed
//	int TrickCounter;			///< current trick speed counter
	int VideoPaused;
	int Closing;			///< flag about closing current stream
	int Filter_Close;
	int Filter_Bug;

	int StartCounter;			///< counter for video start
	int FramesDuped;			///< number of frames duplicated
	int FramesDropped;			///< number of frames dropped
	AVRational *timebase;		///< pointer to AVCodecContext pkts_timebase
	int64_t pts;

	int CodecMode;			/// 0: find codec by id, 1: set _mmal, 2: no mpeg hw,
							/// 3: set _v4l2m2m for H264
	int NoHwDeint;			/// set if no hw deinterlacer

	AVFilterGraph *filter_graph;
	AVFilterContext *buffersrc_ctx, *buffersink_ctx;

	int fd_drm;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	drmEventContext ev;
	struct drm_buf *act_buf;
	struct drm_buf bufs[36];
	struct drm_buf buf_osd;
#ifdef USE_GLES
	struct drm_buf *buf_osd_gl;
#endif
	struct drm_buf buf_black;
	int use_zpos;
	uint64_t zpos_overlay;
	uint64_t zpos_primary;
	uint32_t connector_id, crtc_id;
	struct plane *planes[MAX_PLANES];
	AVFrame *lastframe;
	int buffers;
	int enqueue_buffer;
	int OsdShown;

#ifdef USE_GLES
	struct gbm_device *gbm_device;
	struct gbm_surface *gbm_surface;
	EGLSurface eglSurface;
	EGLDisplay eglDisplay;
	EGLContext eglContext;
	struct gbm_bo *bo;
	struct gbm_bo *old_bo;
	struct gbm_bo *next_bo;
	int GlInit;
#endif
};

    /// Video hardware decoder typedef
typedef struct _Drm_Render_ VideoRender;
#endif
    /// Video output stream typedef
typedef struct __video_stream__ VideoStream; 		// in softhddev.h ?

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

extern int VideoAudioDelay;		///< audio/video delay

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate new video hardware decoder.
extern VideoRender *VideoNewRender(VideoStream *);

    /// Deallocate video hardware decoder.
extern void VideoDelRender(VideoRender *);

    /// Callback to negotiate the PixelFormat.
extern enum AVPixelFormat Video_get_format(VideoRender *, AVCodecContext *,
    const enum AVPixelFormat *);

    /// Render a ffmpeg frame.
extern void VideoRenderFrame(VideoRender *, AVCodecContext *,
    AVFrame *);

    /// Set audio delay.
extern void VideoSetAudioDelay(int);

    /// Clear OSD.
extern void VideoOsdClear(VideoRender *);

    /// Draw an OSD ARGB image.
extern void VideoOsdDrawARGB(VideoRender *, int, int, int,
		int, int, const uint8_t *, int, int);

    /// Set closing flag.
extern void VideoSetClosing(VideoRender *);

    /// Set trick play speed.
extern void VideoSetTrickSpeed(VideoRender *, int);

extern void VideoFlushBuffers(VideoRender *);

extern void VideoPause(VideoRender *);

extern void VideoPlay(VideoRender *);

    /// Grab screen.
extern uint8_t *VideoGrab(int *, int *, int *, int);

    /// Grab screen raw.
extern uint8_t *VideoGrabService(int *, int *, int *);

    /// Get decoder statistics.
extern void VideoGetStats(VideoRender *, int *, int *, int *);

    /// Get screen size
extern void VideoGetScreenSize(VideoRender *, int *, int *, double *);

    /// Get video clock.
extern int64_t VideoGetClock(const VideoRender *);

    /// Display handler.
extern void VideoThreadWakeup(VideoRender *);
extern void VideoThreadExit(void);

extern void VideoInit(VideoRender *);	///< Setup video module.
extern void VideoExit(VideoRender *);		///< Cleanup and exit video module.

extern int VideoCodecMode(VideoRender *);

extern const char * VideoGetDecoderName(const char *);

/// @}
#endif
