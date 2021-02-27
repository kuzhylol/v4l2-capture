#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <linux/videodev2.h>

#define V4L2_DEFAULT_VIDEO_DEVICE "/dev/video0"

#define V4L2_REQUESTED_BUFFERS_NUM      6
#define V4L2_REQUESTED_PLANES_NUM       1

#define V4L2_WIDTH_DEFAULT              640
#define V4L2_HEIGHT_DEFAULT             480
#define V4L2_PIXEL_FORMAT_DEFAULT       V4L2_PIX_FMT_YUYV
#define V4L2_FIELD_DEFAULT              V4L2_FIELD_ANY

struct v4l2_mplane_buffer {
	void    *head[V4L2_REQUESTED_PLANES_NUM];          // Beginning of valid buffer
	size_t  length[V4L2_REQUESTED_PLANES_NUM];         // Size of whole buffer
};

struct v4l2_frame_buffer {
	struct  v4l2_mplane_buffer mp_buff;
	size_t  index;          // Id number of the valid buffer
	size_t  bytes_used;     // Number of used bytes, may be less then page_size
};

struct v4l2_camera_params {
	size_t width;
	size_t height;
	size_t pixel_format;
	size_t field;
	enum   v4l2_buf_type type;
};

struct v4l2_camera {
	int vfd;
	struct v4l2_frame_buffer *fb;
	struct v4l2_camera_params params;
};

extern bool v4l2_frame_ready;

int v4l2_try_set_format(int *vfd);
struct v4l2_camera *v4l2_start_video_capturing(const char *video_dev);
