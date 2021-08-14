#include "v4l2-capture.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>

#include <linux/videodev2.h>

static const char *def_dev_path = "/dev/video0";

bool v4l2_is_polling;

#define LOG(...)	do { \
			     printf("v4l2_capture: "); \
			     printf(__VA_ARGS__); printf("\n"); \
			   } while (0)

static int xioctl(int fh, int request, void *arg)
{
        int rc;

        do {
		rc = ioctl(fh, request, arg);
        } while (-1 == rc && EINTR == errno);

        return rc;
}

static void v4l2_poll_exit(int sig)
{
	if (sig == SIGINT)
		v4l2_is_polling = false;
}

static struct v4l2_camera *v4l2_create_camera(size_t width,
					      size_t height,
					      size_t pix_fmt)
{
	struct v4l2_camera *c;

	c = calloc(1, sizeof(*c));

	if (c != NULL) {
		c->params.width = width ? width : V4L2_WIDTH_DEFAULT;
		c->params.height = height ? height : V4L2_HEIGHT_DEFAULT;
		c->params.pixel_format = pix_fmt ? pix_fmt : V4L2_PIXEL_FORMAT_DEFAULT;

		LOG("Camera has been created");
	} else
		LOG("Not able to create camera");

	return c;
}

static int v4l2_open_device(const char *dev_path)
{
	int vfd;

	vfd = open(dev_path ? dev_path : def_dev_path, O_RDWR | O_NONBLOCK, 0);

	if (vfd < 0)
		LOG("Cannot open '%s': %d, %s", dev_path, errno, strerror(errno));
	else
		LOG("Camera was opened: %s", dev_path ? dev_path : def_dev_path);

	return vfd;
}

static int v4l2_getset_capability(struct v4l2_camera *c)
{
	struct v4l2_capability cap;
	int		       rc;

	memset(&cap, 0, sizeof(cap));

	rc = xioctl(c->vfd, VIDIOC_QUERYCAP, &cap);
	if (rc != 0) {
		LOG("Camera capability wasn't fetched");
		if (errno == EINVAL)
			LOG("Opened device not a V4L2 device");
	}

	if (rc == 0) {
		if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
			c->params.field = V4L2_FIELD_DEFAULT;
			c->params.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			LOG("V4L2_CAP_VIDEO_CAPTURE mode");
		} else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
			c->params.field = V4L2_FIELD_NONE;
			c->params.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			LOG("V4L2_CAP_VIDEO_CAPTURE_MPLANE mode");
		} else {
			LOG("Camera doesn't support specified capabilities");
			rc = -1;
		}
	}

	return rc;
}

static int v4l2_getset_format(struct v4l2_camera *c)
{
	struct v4l2_format	fmt;
	int			rc;

	memset(&fmt, 0, sizeof(struct v4l2_format));

	fmt.type = c->params.type;
	rc = xioctl(c->vfd, VIDIOC_G_FMT, &fmt);
	if (rc != 0)
		LOG("VIDIOC_G_FMT failed");

	if (rc == 0) {
		switch(c->params.type) {
			case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
				fmt.fmt.pix_mp.width  = c->params.width;
				fmt.fmt.pix_mp.height = c->params.height;
				fmt.fmt.pix_mp.field  = c->params.field;
				fmt.fmt.pix_mp.num_planes  = c->mplane_num;
				fmt.fmt.pix_mp.pixelformat = c->params.pixel_format;
			break;
			case V4L2_BUF_TYPE_VIDEO_CAPTURE:
				fmt.fmt.pix.width  = c->params.width;
				fmt.fmt.pix.height = c->params.height;
				fmt.fmt.pix.field  = c->params.field;
				fmt.fmt.pix.pixelformat = c->params.pixel_format;
			break;
			default:
				LOG("Camera format is not supported");
				rc = -1;
			break;
			}
	}

	if (rc == 0) {
		rc = xioctl(c->vfd, VIDIOC_S_FMT, &fmt);
		if (rc == 0) {
			LOG("Camera width: %zu", c->params.width);
			LOG("Camera height: %zu", c->params.height);
			LOG("Camera field was set %zu", c->params.field);
			LOG("Camera type was set %u type", c->params.type);
		} else
			LOG("VIDIOC_S_FMT failed");
	}

	return rc;
}

static void v4l2_mplane_destroy(struct v4l2_camera *c, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free(c->fb[i].f.head);
		free(c->fb[i].f.length);
	}
}

static int v4l2_allocate_fb(struct v4l2_camera *c, size_t fb_num, size_t mplane_num)
{
	size_t			 i;
	int			 rc;

	rc = 0;
	i  = 0;
	c->fb_num = fb_num ? fb_num : V4L2_REQUESTED_BUFFERS_NUM;
	c->mplane_num = mplane_num ? mplane_num : V4L2_REQUESTED_PLANES_NUM;
	c->fb = calloc(c->fb_num, sizeof(*c->fb));

	LOG("Buffers number: %zu", c->fb_num);
	LOG("Mplane number: %zu", c->mplane_num);

	if (c->fb == NULL) {
		rc = -ENOMEM;
		goto enomem;
	}

	for ( ; i < c->fb_num && rc == 0; i++) {
		c->fb[i].f.head = calloc(c->mplane_num, sizeof(*(c->fb[i].f.head)));
		if (c->fb[i].f.head == NULL) {
			rc = -ENOMEM;
			break;
		}

		c->fb[i].f.length = calloc(c->mplane_num, sizeof(*(c->fb[i].f.length)));
		if (c->fb[i].f.length == NULL) {
			rc = -ENOMEM;
			break;
		}
	}

	if (rc != 0) {
		v4l2_mplane_destroy(c, i);
		free(c->fb);
	}

enomem:

	if (rc == 0)
		LOG("Camera buffers have been allocated");
	else
		LOG("Not sufficient memory for %zu buffers", c->fb_num);

	return rc;
}

static int v4l2_mmap_fb(struct v4l2_camera *c, struct v4l2_buffer *mmap_fb, size_t buf_index)
{
	int rc;

	rc = 0;
	for (size_t j = 0; j < c->mplane_num && rc == 0; j++) {
		switch(c->params.type) {
			case V4L2_BUF_TYPE_VIDEO_CAPTURE:
				c->fb[buf_index].f.length[j] = mmap_fb->length;
				c->fb[buf_index].f.head[j] = mmap(NULL,
							mmap_fb->length,
							PROT_READ | PROT_WRITE,
							MAP_SHARED,
							c->vfd,
							mmap_fb->m.offset);
			break;
			case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
				c->fb[buf_index].f.length[j] = mmap_fb->m.planes[j].length;
				c->fb[buf_index].f.head[j] = mmap(NULL,
							c->fb->f.length[j],
							PROT_READ | PROT_WRITE,
							MAP_SHARED,
							c->vfd,
							mmap_fb->m.planes[j].m.mem_offset);
			break;
			default:
				rc = -1;
			break;
		}

		LOG("Camera mmap mplane[%zu] length: %zu", j, c->fb[buf_index].f.length[j]);
		LOG("Camera frame buffer [%zu] address: %p", buf_index, c->fb[buf_index].f.head[j]);

		if (rc == 0 && c->fb->f.head[j] == MAP_FAILED) {
			LOG("Camera mmap failed");
			rc = -1;
		}
	}

	return rc;
}

static int v4l2_mmap_camera(struct v4l2_camera *c)
{
	struct v4l2_requestbuffers req;
	int			   rc;

	memset(&req, 0, sizeof(req));

	req.count	= c->fb_num;
	req.type	= c->params.type;
	req.memory	= V4L2_MEMORY_MMAP;

	rc = xioctl(c->vfd, VIDIOC_REQBUFS, &req);
	if (rc != 0)
		LOG("VIDIOC_REQBUFS failed: %d, %s", errno, strerror(errno));

	for (size_t i = 0; i < c->fb_num && rc == 0; i++) {
		struct v4l2_buffer mmap_fb;
		memset(&mmap_fb, 0, sizeof(mmap_fb));

		mmap_fb.index  = i;
		mmap_fb.memory = V4L2_MEMORY_MMAP;
		mmap_fb.type   = c->params.type;

		if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			struct v4l2_plane mmap_mplane;
			memset(&mmap_mplane, 0, sizeof(mmap_mplane));

			mmap_fb.length   = c->mplane_num;
			mmap_fb.m.planes = &mmap_mplane;
		}

		rc = xioctl(c->vfd, VIDIOC_QUERYBUF, &mmap_fb);

		if (rc != 0) {
			LOG("VIDIOC_QUERYBUF failed: %d, %s\n", errno, strerror(errno));

			if (errno == EINVAL)
				LOG("The buffer type is not supported, or the index is out of bounds");
		}

		if (rc == 0)
			rc = v4l2_mmap_fb(c, &mmap_fb, i);
	}

	if (rc == 0)
		LOG("Camera has been mapped");

	return rc;
}

static int v4l2_enqueue_all_buf(struct v4l2_camera *c)
{
	int		   rc;

	rc = 0;
	for (size_t i = 0; i < c->fb_num && rc == 0; i++) {
		struct v4l2_buffer v4l2_buf;
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));

		v4l2_buf.memory	= V4L2_MEMORY_MMAP;
		v4l2_buf.field	= c->params.field;
		v4l2_buf.type	= c->params.type;
		v4l2_buf.index	= i;

		if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			struct v4l2_plane  mplane;
			memset(&mplane, 0, sizeof(mplane));

			v4l2_buf.m.planes = &mplane;
			v4l2_buf.length = c->mplane_num;
		}

		rc = xioctl(c->vfd, VIDIOC_QBUF, &v4l2_buf);
		if (rc != 0)
			LOG("VIDIOC_QBUF failed");
		LOG("Camera buffer[%zu] flag: 0x%x", i, v4l2_buf.flags);
	}

	if (rc == 0)
		LOG("The initial camera buffer was enqueued");

	return rc;
}

static int v4l2_dequeue_enqueue_buf(struct v4l2_camera *c)
{
	int		   rc;
	struct v4l2_buffer v4l2_buff;

	memset(&v4l2_buff, 0, sizeof(v4l2_buff));

	v4l2_buff.memory = V4L2_MEMORY_MMAP;
	v4l2_buff.field  = c->params.field;
	v4l2_buff.type   = c->params.type;

	/* TODO: Not sure that it will correct for mplanes dequeue */
	if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		struct v4l2_plane  mplanes[c->mplane_num];
		memset(mplanes, 0, c->mplane_num * sizeof(mplanes));

		v4l2_buff.m.planes = mplanes;
		v4l2_buff.length = c->mplane_num;
	}

	rc = xioctl(c->vfd, VIDIOC_DQBUF, &v4l2_buff);
	if (rc != 0) {
		if (errno == EAGAIN)
			LOG("VIDIOC_DQBUF - frame not ready: %d, %s", errno, strerror(errno));

		LOG("Camera flags: 0x%x", v4l2_buff.flags);
		LOG("VIDIOC_DQBUF failed: %d, %s", errno, strerror(errno));
	}

	if (rc == 0) {
		if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			c->fb->bytes_used = v4l2_buff.bytesused;
			c->fb->index = v4l2_buff.index;
			LOG("Bytes num:%zu", c->fb->bytes_used);
			LOG("Index of frame buffer:%zu", c->fb->index);
			LOG("Address of frame buffer:%p", c->fb[c->fb->index].f.head);
		}

		pthread_mutex_lock(&c->c_lock);
		c->v4l2_is_frame_ready = true;
		pthread_mutex_unlock(&c->c_lock);

		rc = xioctl(c->vfd, VIDIOC_QBUF, &v4l2_buff);
		if (rc != 0)
			LOG("VIDIOC_QBUF failed");
	}

	if (rc !=0 )
		LOG("Enqueue dequeue frame buffer failed");

	return rc;
}

static int v4l2_stream_on(struct v4l2_camera *c)
{
	int rc;

	rc = xioctl(c->vfd, VIDIOC_STREAMON, &c->params.type);
	if (rc != 0)
		LOG("VIDIOC_STREAMON failed: %d, %s", errno, strerror(errno));
	else
		LOG("Camera stream has been started");

	return rc;
}

static int v4l2_stream_off(struct v4l2_camera *c)
{
	int rc;

	rc = xioctl(c->vfd, VIDIOC_STREAMOFF, &c->params.type);
	if (rc != 0)
		LOG("VIDIOC_STREAMOFF failed: %d, %s", errno, strerror(errno));
	else
		LOG("Camera stream has been stopped");

	return rc;
}

static void v4l2_close_camera(struct v4l2_camera *c)
{
	int rc;

	rc = close(c->vfd);

	if (rc != 0)
		LOG("Camera device closing failure: %d, %s", errno, strerror(errno));
	else
		LOG("Camera was closed");
}

static int v4l2_munmap_camera(struct v4l2_camera *c)
{
	int rc;

	rc = 0;

	for (size_t i = 0; i < c->fb_num; i++) {
		for (size_t j = 0; j < c->mplane_num; j++) {
			rc = munmap(c->fb[i].f.head[j], c->fb[i].f.length[j]);
			if (rc != 0)
				LOG("V4L2 munmap[%zu] failed: %d, %s", i, errno, strerror(errno));
			else
				LOG("Camera munmap %p buffer [%zu] with length %zu",
					c->fb[i].f.head[j], i, c->fb[i].f.length[j]);
		}
	}

	return rc;
}

static void v4l2_release_device(struct v4l2_camera *c)
{
	v4l2_stream_off(c);
	pthread_mutex_destroy(&c->c_lock);
	v4l2_munmap_camera(c);
	v4l2_mplane_destroy(c, c->fb_num);
	free(c->fb);
	v4l2_close_camera(c);
	free(c);

	LOG("Camera was released");
}

static void *v4l2_poll_frame_thread(void *camera)
{
	struct v4l2_camera *c;
	struct pollfd	   fds;
	int		   rc;

	c = (struct v4l2_camera *)camera;

	memset(&fds, 0, sizeof(fds));

	LOG("Control signal were initialized");

	fds.fd = c->vfd;
	fds.events = POLLIN;

	do {
		rc = poll(&fds, 1, 2);

		if (rc < 0) {
			if (errno != EINTR)
				break;
			continue;
		}

		if (fds.revents & POLLIN && c->v4l2_is_frame_ready == false)
			v4l2_dequeue_enqueue_buf(c);


	} while (v4l2_is_polling == true);

	v4l2_release_device(c);

	return NULL;
}

static int v4l2_start_thread(struct v4l2_camera *c)
{
	pthread_t	v4l2_thread;
	int		rc;

	rc = pthread_mutex_init(&c->c_lock, NULL);
	if (rc == 0) {
		rc = pthread_create((pthread_t *)&v4l2_thread,
			    NULL,
			    v4l2_poll_frame_thread,
			    (void*)c);
		if (rc == 0)
			pthread_detach(v4l2_thread);
	}

	if (rc < 0)
		LOG("Not able to start video thread");


	return rc;
}

struct v4l2_camera *v4l2_start_video_capturing(const char *video_dev,
					       size_t w, size_t h, size_t pix_fmt,
					       size_t buf_num, size_t mplane_num)
{
	struct v4l2_camera	 *c;
	int			 rc;

	v4l2_is_polling = true;

	c = v4l2_create_camera(w, h, pix_fmt);
	if (c == NULL)
		goto camera_not_ready;

	c->vfd = v4l2_open_device(video_dev);
	if (c->vfd < 0)
		goto null_camera;

	rc = v4l2_getset_capability(c);
	if (rc != 0)
		goto broken_camera;

	rc = v4l2_allocate_fb(c, buf_num, mplane_num);
	if (rc != 0)
		goto broken_camera;

	rc = v4l2_getset_format(c);
	if (rc != 0)
		goto null_buf;

	rc = v4l2_mmap_camera(c);
	if (rc != 0)
		goto null_buf;

	rc = v4l2_enqueue_all_buf(c);
	if (rc != 0)
		goto mmap_failed;

	rc = v4l2_stream_on(c);
	if (rc != 0)
		goto mmap_failed;

	rc = v4l2_start_thread(c);
	if (rc != 0)
		goto stream_off;

	signal(SIGINT, v4l2_poll_exit);

	return c;

stream_off:
	v4l2_stream_off(c);
	pthread_mutex_destroy(&c->c_lock);

mmap_failed:
	v4l2_munmap_camera(c);
	v4l2_mplane_destroy(c, c->fb_num);

null_buf:
	free(c->fb);

broken_camera:
	v4l2_close_camera(c);

null_camera:
	free(c);

camera_not_ready:
	LOG("Camera start failed");

	return NULL;
}

static int v4l2_capture_test(void)
{
	struct v4l2_camera *c;
	int		    rc;
	int		    record_fd;

	record_fd = open("demo.raw", O_CREAT | O_TRUNC | O_RDWR | O_NONBLOCK, 0644);
	rc = record_fd < 0 ? -1 : 0;

	if (rc != 0)
		LOG("Not able to create output video file");

	if (rc == 0)
		c = v4l2_start_video_capturing(NULL, 0 ,0 ,0, 0, 0);

	if (c != NULL && c->fb != NULL && rc == 0) {
		do {
			if (c->v4l2_is_frame_ready) {
				switch(c->params.type) {
				case V4L2_BUF_TYPE_VIDEO_CAPTURE:
					write(record_fd, c->fb[c->fb->index].f.head[0], c->fb->bytes_used);
				break;
				case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
					write(record_fd, c->fb->f.head[0], c->params.width * c->params.height * 2);
				break;
				default:
				break;
				}
				c->v4l2_is_frame_ready = false;
			}
		}
		while (v4l2_is_polling);
	}
	pthread_exit(0);
	close(record_fd);
	return rc;
}

int main(void)
{
	return v4l2_capture_test();
}
