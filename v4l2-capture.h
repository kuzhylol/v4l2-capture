#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <linux/videodev2.h>

#define V4L2_REQUESTED_BUFFERS_NUM	10
#define V4L2_REQUESTED_PLANES_NUM	1

#define V4L2_WIDTH_DEFAULT		640
#define V4L2_HEIGHT_DEFAULT		480
#define V4L2_PIXEL_FORMAT_DEFAULT	V4L2_PIX_FMT_YUYV
#define V4L2_FIELD_DEFAULT		V4L2_FIELD_ANY

struct v4l2_frame {
	void    **head;          // Beginning of valid frame buffer
	size_t  *length;         // Length of whole frame buffer
};

struct v4l2_frame_buffer {
	struct  v4l2_frame f;
	size_t  index;          // Index of a valid buffer
	size_t  bytes_used;     // Number of used bytes to write frame, may be less then page_size
};

struct v4l2_camera_params {
	size_t width;
	size_t height;
	size_t pixel_format;
	size_t field;
	enum   v4l2_buf_type type;
};

struct v4l2_camera {
	/* Descriptor for /dev/videoN node */
	int vfd;

	/* Frame buffers number */
	size_t fb_num;

	/* Number of planes */
	size_t mplane_num;

	/* Pointer to frame buffers */
	struct v4l2_frame_buffer *fb;

	struct v4l2_camera_params params;

	/* Indicated that buffer is ready to be fetched */
	bool v4l2_is_frame_ready;

	/* Lock for v4l2_is_frame_ready */
	pthread_mutex_t c_lock;
};

extern bool v4l2_is_polling;

struct v4l2_camera *v4l2_start_video_capturing(const char *video_dev,
					       size_t w, size_t h, size_t pix_fmt,
					       size_t buf_num, size_t mplane_num);
