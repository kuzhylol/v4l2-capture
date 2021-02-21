#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <linux/videodev2.h>

#define V4L2_DEFAULT_VIDEO_DEVICE "/dev/video0"

#define V4L2_REQUESTED_BUFFERS_NUM	5
#define V4L2_WIDTH_DEFAULT		1280
#define V4L2_HEIGHT_DEFAULT		960
#define V4L2_PIXEL_FORMAT_DEFAULT	V4L2_PIX_FMT_YUYV
#define V4L2_FIELD_DEFAULT		V4L2_FIELD_ANY

struct v4l2_frame_buffer {
	void    *head;     // Beginning of valid buffer
	size_t  index;     // Id number of the valid buffer
	size_t  len;       // Number of used bytes, may be less then page_size
	size_t  page_size; // Size of whole buffer
};

extern bool v4l2_frame_ready;

int v4l2_try_set_format(int *vfd);
struct v4l2_frame_buffer *v4l2_start_video_capturing(const char *video_dev);
