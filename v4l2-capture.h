#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <linux/videodev2.h>

#define V4L2_DEFAULT_VIDEO_DEVICE "/dev/video0"

#define V4L2_REQUESTED_BUFFERS_NUM      3
#define V4L2_REQUESTED_PLANES_NUM       1

#define V4L2_WIDTH_DEFAULT              1920
#define V4L2_HEIGHT_DEFAULT             1080
#define V4L2_PIXEL_FORMAT_DEFAULT       V4L2_PIX_FMT_YUV420
#define V4L2_FIELD_DEFAULT              V4L2_FIELD_ANY

extern bool v4l2_frame_ready;

int v4l2_try_set_format(int *vfd);
struct v4l2_camera *v4l2_start_video_capturing(const char *video_dev);
