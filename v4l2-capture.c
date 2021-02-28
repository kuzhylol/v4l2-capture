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

bool v4l2_frame_ready;
bool v4l2_is_polling;

static void v4l2_poll_exit(int sig)
{
	if (sig == SIGINT)
		v4l2_is_polling = false;
}

static void v4l2_grab_frame(int sig)
{
	if (sig == SIGUSR1)
		v4l2_frame_ready = true;
}

static int v4l2_push_frame(int fd, const unsigned char *payload, ssize_t size)
{
	ssize_t n = write(fd, payload, size);

	if (n != size) {
		fprintf(stderr, "Payload write failed: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	return n;
}

static int xioctl(int vfd, size_t req, void *arg)
{
	int rc;

	do {
		rc = ioctl(vfd, req, arg);
	} while (rc == -1 && errno == EINTR);

	return rc;
}

static struct v4l2_camera *v4l2_create_camera(void)
{
	struct v4l2_camera *camera = calloc(1, sizeof(struct v4l2_camera));

	if (camera == NULL)
		fprintf(stderr, "Not able to allocate V4L2 camera device\n");

	if (camera != NULL) {
		camera->params.width = V4L2_WIDTH_DEFAULT;
		camera->params.height = V4L2_HEIGHT_DEFAULT;
		camera->params.pixel_format = V4L2_PIXEL_FORMAT_DEFAULT;
	}

	return camera;
}

static int v4l2_open_device(const char *dev_name)
{
	int vfd = open(dev_name, O_RDWR | O_NONBLOCK, 0);

	if (vfd == -1) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n",
		       dev_name, errno, strerror(errno));
	}

	fprintf(stderr, "[%d] V4L2 device was opened: %s\n", vfd, dev_name);
	return vfd;
}

static int v4l2_query_capabilities(struct v4l2_camera *c)
{
	int rc = EXIT_SUCCESS;
	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));

	rc = xioctl(c->vfd, VIDIOC_QUERYCAP, &cap);
	if (rc == -1) {
		if (errno == EINVAL) {
			fprintf(stderr, "[%d] V4L2 device not a V4L2 device\n", c->vfd);
		} else {
			fprintf(stderr, "[%d] VIDIOC_QUERYCAP failed\n", c->vfd);
		}
	}

	if (!rc) {
		if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
			c->params.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			c->params.field = V4L2_FIELD_DEFAULT;
			fprintf(stdout, "[%d] V4L2 capture mode\n", c->vfd);
		} else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
			fprintf(stdout, "[%d] V4L2 capture mplane mode\n", c->vfd);
			c->params.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			c->params.field = V4L2_FIELD_NONE;
		} else {
			fprintf(stderr, "[%d] V4L2 device doesn't support specified capabilities\n", c->vfd);
			rc = EXIT_FAILURE;
		}
	}

	return rc;
}

static int v4l2_set_format(struct v4l2_camera *c)
{
	int rc = EXIT_SUCCESS;

	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));

	fmt.type = c->params.type;
	rc = xioctl(c->vfd, VIDIOC_G_FMT, &fmt);
	if (rc == -1)
		fprintf(stderr, "[%d] VIDIOC_G_FMT failed\n", c->vfd);

	if (!rc) {
		switch(c->params.type) {
		case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
			fmt.fmt.pix_mp.width  = c->params.width;
			fmt.fmt.pix_mp.height = c->params.height;
			fmt.fmt.pix_mp.field  = c->params.field;
			fmt.fmt.pix_mp.num_planes  = V4L2_REQUESTED_PLANES_NUM;
			fmt.fmt.pix_mp.pixelformat = c->params.pixel_format;
			break;

		case V4L2_BUF_TYPE_VIDEO_CAPTURE:
			fmt.fmt.pix.width  = c->params.width;
			fmt.fmt.pix.height = c->params.height;
			fmt.fmt.pix.field  = c->params.field;
			fmt.fmt.pix.pixelformat = c->params.pixel_format;
			break;

		default:
			fprintf(stderr, "No one format is supported\n");
			return EXIT_FAILURE;
			break;
		}

		rc = xioctl(c->vfd, VIDIOC_S_FMT, &fmt);
		if (rc == -1)
			fprintf(stderr, "[%d] VIDIOC_S_FMT failed\n", c->vfd);
		else
			fprintf(stderr, "[%d] V4L2 format was set\n", c->vfd);
	}

	return rc;
}

static struct v4l2_frame_buffer *v4l2_create_buffs(void)
{
	struct v4l2_frame_buffer *buff = calloc(V4L2_REQUESTED_BUFFERS_NUM, sizeof(*buff));

	if (buff == NULL)
		fprintf(stderr, "Not able to allocate %d video buffers\n", V4L2_REQUESTED_BUFFERS_NUM);

	return buff;
}

static int v4l2_mmap_device(struct v4l2_camera *c)
{
	int rc = EXIT_SUCCESS;
	void *m_ptr = MAP_FAILED;
	struct v4l2_requestbuffers req;
	bool is_mplanes = false;
	size_t length = 0;
	size_t planes_num = 1;

	memset(&req, 0, sizeof(req));

	if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		planes_num = V4L2_REQUESTED_PLANES_NUM;
		is_mplanes = true;
	}

	req.type   = c->params.type;
	req.count  = V4L2_REQUESTED_BUFFERS_NUM;
	req.memory = V4L2_MEMORY_MMAP;

	rc = xioctl(c->vfd, VIDIOC_REQBUFS, &req);
	if (rc == -1)
		fprintf(stderr, "VIDIOC_REQBUFS failed: %d, %s\n", errno, strerror(errno));

	if (!rc) {
		for (size_t n_buff = 0; n_buff < req.count; n_buff++) {
			struct v4l2_buffer v4l2_buff;
			memset(&v4l2_buff, 0, sizeof(v4l2_buff));

			v4l2_buff.type        = c->params.type;
			v4l2_buff.memory      = V4L2_MEMORY_MMAP;
			v4l2_buff.index       = n_buff;

			if (is_mplanes) {
				struct v4l2_plane mplanes[V4L2_REQUESTED_PLANES_NUM];
				memset(mplanes, 0, planes_num * sizeof(struct v4l2_plane));
				v4l2_buff.length   = planes_num;
				v4l2_buff.m.planes = mplanes;

				fprintf(stdout, "[%d] V4L2 mplanes were assigned\n", c->vfd);
			}

			rc = xioctl(c->vfd, VIDIOC_QUERYBUF, &v4l2_buff);
			if (rc == -1) {
				if (errno == EINVAL)
					fprintf(stderr, "VIDIOC_QUERYBUF failed: %d, The buffer type is not supported, or the index is out of bounds\n", errno);
				else
					fprintf(stderr, "VIDIOC_QUERYBUF failed: %d, %s\n", errno, strerror(errno));

				return EXIT_FAILURE;
			}

			fprintf(stdout, "[%d] V4L2 buff flag: 0x%x \n", c->vfd, v4l2_buff.flags);

			for (size_t n_plane = 0; n_plane < planes_num; n_plane++) {
				if (is_mplanes) {
					length = v4l2_buff.m.planes[n_plane].length;
					m_ptr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, c->vfd, v4l2_buff.m.planes[n_plane].m.mem_offset);

					fprintf(stdout, "[%d] V4L2 mmap plane[%zu] length: %zu\n", c->vfd, n_plane, length);
					fprintf(stdout, "[%d] V4L2 mmap plane[%zu] offset: 0x%p\n", c->vfd, n_plane, m_ptr);

					c->fb[n_buff].mp_buff.length[n_plane] = length;

				} else {
					length = v4l2_buff.length;
					m_ptr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, c->vfd, v4l2_buff.m.offset);
					c->fb[n_buff].mp_buff.length[n_plane] = length;
				}

				if (m_ptr == MAP_FAILED) {
					fprintf(stderr, "Video mplane buffer wasn't mmaped: %d, %s\n", errno, strerror(errno));
					return EXIT_FAILURE;
				}

				c->fb[n_buff].mp_buff.head[n_plane] = m_ptr;
			}
		}
		fprintf(stderr, "[%d] V4L2 buffers were mmaped\n", c->vfd);
	}

	return rc;
}

static int v4l2_buff_enqueue(struct v4l2_camera *c)
{
	int rc = EXIT_SUCCESS;
	bool is_mplanes = false;
	size_t planes_num = 1;

	if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		planes_num = V4L2_REQUESTED_PLANES_NUM;
		is_mplanes = true;
	}

	for (size_t n_buff = 0; n_buff < V4L2_REQUESTED_BUFFERS_NUM; n_buff++) {
		struct v4l2_buffer v4l2_buff;
		memset(&v4l2_buff, 0, sizeof(v4l2_buff));

		v4l2_buff.type = c->params.type;
		v4l2_buff.field = c->params.field;
		v4l2_buff.memory = V4L2_MEMORY_MMAP;
		v4l2_buff.index = n_buff;

		if (is_mplanes) {
			struct v4l2_plane planes[V4L2_REQUESTED_PLANES_NUM];
			v4l2_buff.m.planes = planes;
			v4l2_buff.length = planes_num;
		}

		rc = xioctl(c->vfd, VIDIOC_QBUF, &v4l2_buff);
		if (rc == -1) {
			fprintf(stderr, "VIDIOC_QBUF failed: %d, %s\n", errno, strerror(errno));
			return EXIT_FAILURE;
		}

		fprintf(stdout, "[%d] V4L2 buff[%zu] flag: 0x%x \n", c->vfd, n_buff, v4l2_buff.flags);
	}

	fprintf(stderr, "[%d] First V4L2 buffer was enqueued\n", c->vfd);
	return rc;
}

static int v4l2_stream_on(struct v4l2_camera *c)
{
	int rc = EXIT_SUCCESS;
	enum v4l2_buf_type type;

	type = c->params.type;
	rc = xioctl(c->vfd, VIDIOC_STREAMON, &type);
	if (rc == -1)
		fprintf(stderr, "VIDIOC_STREAMON failed: %d, %s\n", errno, strerror(errno));
	else
		fprintf(stderr, "[%d] V4L2 stream has been started\n", c->vfd);

	return rc;
}

static int v4l2_stream_off(struct v4l2_camera *c)
{
	int rc = EXIT_SUCCESS;
	enum v4l2_buf_type type;

	type = c->params.type;
	rc = xioctl(c->vfd, VIDIOC_STREAMOFF, &type);
	if (rc == -1)
		fprintf(stderr, "VIDIOC_STREAMOFF failed: %d, %s\n", errno, strerror(errno));
	else
		fprintf(stderr, "[%d] V4L2 stream has been stopped\n", c->vfd);

	return rc;
}

static int v4l2_dequeue_enqueue_buff(struct v4l2_camera *c)
{
	int rc = EXIT_SUCCESS;
	struct v4l2_buffer v4l2_buff;
	bool is_mplanes = false;
	size_t planes_num = 1;

	memset(&v4l2_buff, 0, sizeof(v4l2_buff));

	v4l2_buff.field  = c->params.field;
	v4l2_buff.type   = c->params.type;
	v4l2_buff.memory = V4L2_MEMORY_MMAP;

	if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		struct v4l2_plane mplanes[V4L2_REQUESTED_PLANES_NUM];
		memset(mplanes, 0, planes_num * sizeof(struct v4l2_plane));
		planes_num = V4L2_REQUESTED_PLANES_NUM;
		v4l2_buff.m.planes = mplanes;
		v4l2_buff.length = planes_num;
		is_mplanes = true;
	}

	rc = xioctl(c->vfd, VIDIOC_DQBUF, &v4l2_buff);
	if (rc == -1) {
		if (errno == EAGAIN) {
			fprintf(stderr, "VIDIOC_DQBUF - frame not ready: %d, %s\n", errno, strerror(errno));
			return EXIT_SUCCESS;
		}

		fprintf(stdout, "[%d] V4L2 DQ buff flag: 0x%x \n", c->vfd, v4l2_buff.flags);
		fprintf(stderr, "VIDIOC_DQBUF failed: %d, %s\n", errno, strerror(errno));
	}

	if (!rc) {
		if (!is_mplanes)
			c->fb->bytes_used = v4l2_buff.bytesused;

		c->fb->index = v4l2_buff.index;
		rc = kill(getpid(), SIGUSR1);
		if (rc == -1)
			fprintf(stderr, "V4L2 frame signaling failure: %d, %s\n", errno, strerror(errno));

		if (!rc) {
			rc = xioctl(c->vfd, VIDIOC_QBUF, &v4l2_buff);
			if (rc == -1)
				fprintf(stderr, "VIDIOC_QBUF failed: %d, %s\n", errno, strerror(errno));
		}
	}

	return rc;
}

static void v4l2_close_device(struct v4l2_camera *c)
{
	int rc;

	if (c == NULL)
		return;

	rc = close(c->vfd);

	if (rc == -1)
		fprintf(stderr, "v4l2 device closing failure: %d, %s\n", errno, strerror(errno));
	else
		fprintf(stderr, "V4L2 device closed\n");
}

static void v4l2_release_device(struct v4l2_camera *c)
{
	int rc;
	size_t planes_num = 1;

	if (c == NULL) {
		fprintf(stderr, "[%d] Not able to release device, camera is broken\n", c->vfd);
		return;
	}

	v4l2_stream_off(c);
	v4l2_close_device(c);

	if (c->params.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		planes_num = V4L2_REQUESTED_PLANES_NUM;

	for (size_t n_buff = 0; n_buff < V4L2_REQUESTED_BUFFERS_NUM; n_buff++) {
		for (size_t n_plane = 0; n_plane < planes_num; n_plane++) {
			rc = munmap(c->fb[n_buff].mp_buff.head[n_plane], c->fb[n_buff].mp_buff.length[n_plane]);
			if (rc == -1)
				fprintf(stderr, "V4L2 munmap failed: %d, %s\n", errno, strerror(errno));
			fprintf(stdout, "V4L2 munmap %p with length %zu \n", c->fb[n_buff].mp_buff.head[n_plane], c->fb[n_buff].mp_buff.length[n_plane]);
		}
	}

	free(c->fb);
	free(c);

	fprintf(stderr, "V4L2 device released\n");
}

static void *v4l2_poll_frame(void *camera)
{
	int rc = EXIT_SUCCESS;
	struct v4l2_camera *c = (struct v4l2_camera *)camera;
	struct pollfd fds;

	memset(&fds, 0, sizeof(fds));

	fds.fd = c->vfd;
	fds.events = POLLIN;

	fprintf(stdout, "[%d] V4L2 device is polled\n", c->vfd);
	do {
		rc = poll(&fds, 1, 2);

		if (rc < 0) {
			fprintf(stderr, "[%d] V4L2 polling failed %d, %s\n", c->vfd, errno, strerror(errno));

			if (errno == EINTR)
				continue;

			break;
		}

		if (rc == 0)
			continue;

		if (fds.revents & POLLIN)
			v4l2_dequeue_enqueue_buff(c);

	} while (v4l2_is_polling == true);

	fprintf(stdout, "[%d] V4L2 device isn't being polled\n", c->vfd);

	v4l2_release_device(c);

	return NULL;
}

struct v4l2_camera *v4l2_start_video_capturing(const char *video_dev)
{
	int rc = EXIT_FAILURE;
	struct v4l2_camera *camera;
	pthread_t v4l2_thread;

	camera = v4l2_create_camera();

	if (camera == NULL)
		return NULL;

	camera->fb = v4l2_create_buffs();

	if (camera->fb == NULL) {
		free(camera);
		return NULL;
	}

	camera->vfd = v4l2_open_device(video_dev ? video_dev : V4L2_DEFAULT_VIDEO_DEVICE);

	if (camera->vfd == -1) {
		free(camera->fb);
		free(camera);
		return NULL;
	}

	rc = v4l2_query_capabilities(camera);

	if (!rc)
		rc = v4l2_set_format(camera);

	if (!rc)
		rc = v4l2_mmap_device(camera);

	if (!rc)
		rc = v4l2_buff_enqueue(camera);

	if (!rc)
		rc = v4l2_stream_on(camera);

	if (!rc) {
		v4l2_frame_ready = false;
		v4l2_is_polling = true;
		signal(SIGINT, v4l2_poll_exit);
		signal(SIGUSR1, v4l2_grab_frame);

		fprintf(stderr, "[%d] Control signal are initialized\n", camera->vfd);
		rc = pthread_create((pthread_t *)&v4l2_thread,
				   NULL,
				   v4l2_poll_frame,
				   (void *)camera);
		if (rc < 0)
			fprintf(stderr, "Not able to start video thread\n");

		pthread_detach(v4l2_thread);
	}

	if (!rc)
		return camera;

	v4l2_release_device(camera);
	return NULL;
}


int main(int argc, char *argv[])
{
	int rc = EXIT_FAILURE;
	const char *raw_file_default = "demo.raw";

	int record_fd = open(argc > 1 ? argv[1] : raw_file_default, O_CREAT | O_TRUNC | O_RDWR | O_NONBLOCK, 0644);
	struct v4l2_camera *c;

	c = v4l2_start_video_capturing(V4L2_DEFAULT_VIDEO_DEVICE);
	if (c != NULL && c->fb != NULL) {
		do {
			if (v4l2_frame_ready == true) {
				v4l2_push_frame(record_fd, c->fb[c->fb->index].mp_buff.head[0], c->fb->bytes_used);
				v4l2_frame_ready = false;
			}
		}
		while (v4l2_is_polling == true);

		rc = EXIT_SUCCESS;
	}
	pthread_exit(0);
	close(record_fd);
	return rc;
}


