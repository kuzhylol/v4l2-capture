#include "v4l2_capture.h"

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
static bool v4l2_is_polling;

static struct v4l2_frame_buffer *fb;

static int v4l2_push_frame(int fd, const unsigned char *payload, size_t size)
{
	ssize_t n = write(fd, payload, size);

	if (n != size) {
		fprintf(stderr, "Payload write failed: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	return n;
}

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

static int xioctl(int vfd, int req, void *arg)
{
	int rc;

	do {
		rc = ioctl(vfd, req, arg);
	} while (rc == -1 && errno == EINTR);

	return rc;
}

static int *v4l2_open_device(const char *dev_name)
{
	bool is_chardev;
	int *persistent_fd;
	int fd;
	struct stat s;

	memset(&s, 0, sizeof(s));

	if (stat(dev_name, &s) == -1) {
		fprintf(stderr, "Cannot identify '%s': %d, %s\n",
		       dev_name, errno, strerror(errno));
		return NULL;
	}

	is_chardev = S_ISCHR(s.st_mode);
	if (!is_chardev) {
		fprintf(stderr, "%s is not a character device", dev_name);
		return NULL;
	}

	fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
	if (fd == -1) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n",
		       dev_name, errno, strerror(errno));
		return NULL;
	}

	persistent_fd = malloc(sizeof(persistent_fd));
	if (persistent_fd == NULL) {
		fprintf(stderr, "[%d] Not able to copy FD to persistent FD\n", fd);
		return NULL;
	}
	*persistent_fd = fd;

	fprintf(stderr, "[%d] V4L2 device was opened: %s\n", fd, dev_name);

	return persistent_fd;
}

static int v4l2_set_format(int *vfd)
{
	int rc = EXIT_SUCCESS;
	struct v4l2_format f;

	memset(&f, 0, sizeof(f));

	if (vfd == NULL)
		return EXIT_FAILURE;

	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width       = V4L2_WIDTH_DEFAULT;
	f.fmt.pix.height      = V4L2_HEIGHT_DEFAULT;
	f.fmt.pix.pixelformat = V4L2_PIXEL_FORMAT_DEFAULT;
	f.fmt.pix.field       = V4L2_FIELD_DEFAULT;

	rc = xioctl(*vfd, VIDIOC_S_FMT, &f);
	if (rc == -1) {
		fprintf(stderr, "VIDIOC_S_FMT failed: %d, %s\n", errno, strerror(errno));
		rc = EXIT_FAILURE;
	}

	fprintf(stderr, "[%d] V4L2 parameters were set\n", *vfd);
	return rc;
}

static struct v4l2_frame_buffer *v4l2_create_buffs(void)
{
	struct v4l2_frame_buffer *buff = calloc(V4L2_REQUESTED_BUFFERS_NUM,
					       sizeof(*(buff)));
	if (buff == NULL)
		fprintf(stderr, "Not able to allocate %d video buffers\n", V4L2_REQUESTED_BUFFERS_NUM);

	return buff;
}

static int v4l2_mmap_device(int *vfd, struct v4l2_frame_buffer *b)
{
	int rc = EXIT_SUCCESS;
	struct v4l2_requestbuffers r;

	memset(&r, 0, sizeof(r));

	if (vfd == NULL && b == NULL)
		return EXIT_FAILURE;


	r.count = V4L2_REQUESTED_BUFFERS_NUM;
	r.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	r.memory = V4L2_MEMORY_MMAP;

	rc = xioctl(*vfd, VIDIOC_REQBUFS, &r);
	if (rc == -1) {
		fprintf(stderr, "VIDIOC_REQBUFS failed: %d, %s\n", errno, strerror(errno));
		rc = EXIT_FAILURE;
	}

	if (!rc) {
		for (size_t n_buff = 0; n_buff < V4L2_REQUESTED_BUFFERS_NUM; n_buff++) {
			struct v4l2_buffer v4l2_buff;

			memset(&v4l2_buff, 0, sizeof(v4l2_buff));

			v4l2_buff.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l2_buff.memory      = V4L2_MEMORY_MMAP;
			v4l2_buff.index       = n_buff;

			rc = xioctl(*vfd, VIDIOC_QUERYBUF, &v4l2_buff);
			if (rc == -1) {
				fprintf(stderr, "VIDIOC_QUERYBUF failed: %d, %s\n", errno, strerror(errno));
				rc = EXIT_FAILURE;
			}

			b[n_buff].head = mmap(NULL, v4l2_buff.length, PROT_READ | PROT_WRITE, MAP_SHARED, *vfd, v4l2_buff.m.offset);
			b[n_buff].page_size = v4l2_buff.length;

			if (b[n_buff].head == MAP_FAILED) {
				fprintf(stderr, "Video buffer wasn't mmaped: %d, %s\n", errno, strerror(errno));
				rc = EXIT_FAILURE;
			}
		}
	}

	fprintf(stderr, "[%d] V4L2 buffers were mmaped\n", *vfd);
	return EXIT_SUCCESS;
}

static int v4l2_buff_enqueue(int *vfd)
{
	int rc = EXIT_SUCCESS;

	if (vfd == NULL)
		return EXIT_FAILURE;

	for (size_t n_buff = 0; n_buff < V4L2_REQUESTED_BUFFERS_NUM; n_buff++) {
		struct v4l2_buffer v4l2_buff;

		memset(&v4l2_buff, 0, sizeof(v4l2_buff));

		v4l2_buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		v4l2_buff.memory = V4L2_MEMORY_MMAP;
		v4l2_buff.index = n_buff;

		rc = xioctl(*vfd, VIDIOC_QBUF, &v4l2_buff);
		if (rc == -1) {
			fprintf(stderr, "VIDIOC_QBUF failed: %d, %s\n", errno, strerror(errno));
			rc = EXIT_FAILURE;
		}
	}

	fprintf(stderr, "[%d] First V4L2 buffer was enqueued\n", *vfd);
	return rc;
}

static int v4l2_stream_on(int *vfd)
{
	int rc = EXIT_SUCCESS;
	enum v4l2_buf_type t;

	t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rc = xioctl(*vfd, VIDIOC_STREAMON, &t);
	if (-1 == rc) {
		fprintf(stderr, "VIDIOC_STREAMON failed: %d, %s\n", errno, strerror(errno));
		rc = EXIT_FAILURE;
	}

	fprintf(stderr, "[%d] V4L2 stream has been started\n", *vfd);
	return rc;
}

static int v4l2_dequeue_enqueue_buff(int *vfd)
{
	int rc = EXIT_SUCCESS;
	struct v4l2_buffer v4l2_buff;

	memset(&v4l2_buff, 0, sizeof(v4l2_buff));

	if (vfd == NULL)
		return EXIT_FAILURE;

	v4l2_buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	v4l2_buff.memory = V4L2_MEMORY_MMAP;

	rc = xioctl(*vfd, VIDIOC_DQBUF, &v4l2_buff);
	if (rc == -1) {
		if (errno == EAGAIN)
			return rc;

		fprintf(stderr, "VIDIOC_DQBUF failed: %d, %s\n", errno, strerror(errno));
		rc = EXIT_FAILURE;
	}

	if (!rc) {
		fb->len = v4l2_buff.bytesused;
		fb->index = v4l2_buff.index;
		rc = kill(getpid(), SIGUSR1);
		if (rc == -1)
			fprintf(stderr, "V4L2 frame signaling failure: %d, %s\n", errno, strerror(errno));

		rc = xioctl(*vfd, VIDIOC_QBUF, &v4l2_buff);
		if (rc == -1) {
			fprintf(stderr, "VIDIOC_QBUF failed: %d, %s\n", errno, strerror(errno));
			rc = EXIT_FAILURE;
		}
	}

	return rc;
}

static void v4l2_release_device(struct v4l2_frame_buffer *b)
{
	int rc;

	if (b == NULL)
		return;

	for (size_t n_buff = 0; n_buff < V4L2_REQUESTED_BUFFERS_NUM; n_buff++) {
		rc = munmap(b[n_buff].head, b[n_buff].page_size);
		if (rc == -1)
			fprintf(stderr, "munmap failed: %d, %s\n", errno, strerror(errno));
	}

	free(b);
}

static void v4l2_close_device(int *vfd)
{
	int rc = close(*vfd);

	if (rc == -1)
		fprintf(stderr, "v4l2 device closing failure: %d, %s\n", errno, strerror(errno));

	if (vfd != NULL)
		free(vfd);
}

static void v4l2_poll_frame(void *pfd)
{
	int rc = EXIT_SUCCESS;
	int *vfd = (int *)pfd;
	struct pollfd fds;

	memset(&fds, 0, sizeof(fds));

	fds.fd = *vfd;
	fds.events = POLLIN;

	fprintf(stderr, "[%d] V4L2 device has been polled\n", *vfd);
	do {
		rc = poll(&fds, 1, 2);

		if (rc < 0) {
			if (errno == EINTR)
				continue;

			fprintf(stderr, "[%d] V4L2 Polling failed %d, %s\n", *vfd, errno, strerror(errno));
			break;
		}

		if (rc == 0)
			continue;

		if (fds.revents & POLLIN)
			v4l2_dequeue_enqueue_buff(vfd);

	} while (v4l2_is_polling == true);

	v4l2_release_device(fb);
	v4l2_close_device(vfd);

	fprintf(stderr, "V4L2 device has been released\n");
}

struct v4l2_frame_buffer *v4l2_start_video_capturing(const char *video_dev)
{
	int rc = EXIT_FAILURE;
	pthread_t v4l2_thread;
	int *vfd = v4l2_open_device(video_dev);

	if (vfd == NULL || *vfd == -1)
		return NULL;

	rc = v4l2_set_format(vfd);

	if (!rc)
		fb = v4l2_create_buffs();

	if (!rc && fb != NULL)
		rc = v4l2_mmap_device(vfd, fb);

	if (!rc)
		rc = v4l2_buff_enqueue(vfd);

	if (!rc)
		rc = v4l2_stream_on(vfd);

	if (!rc) {
		v4l2_frame_ready = false;
		v4l2_is_polling = true;
		signal(SIGINT, v4l2_poll_exit);
		signal(SIGUSR1, v4l2_grab_frame);

		rc = pthread_create((pthread_t *)&v4l2_thread,
				   NULL,
				   (void *)v4l2_poll_frame,
				   (void *)vfd);

		if (rc < 0)
			fprintf(stderr, "Not able to start video thread\n");
	}

	if (!rc)
		return fb;

	v4l2_release_device(fb);
	v4l2_close_device(vfd);
	return NULL;
}

int main(int argc, char **argv)
{
	int rc = EXIT_FAILURE;
	int record_fd = open("demo.raw", O_CREAT | O_TRUNC | O_RDWR | O_NONBLOCK, 0644);

	fb = v4l2_start_video_capturing(V4L2_DEFAULT_VIDEO_DEVICE);

	if (fb != NULL) {
		do {
			if (v4l2_frame_ready == true) {
				v4l2_push_frame(record_fd, fb[fb->index].head, fb->len);
				v4l2_frame_ready = false;
			}
		} while (v4l2_is_polling == true);

		rc = EXIT_SUCCESS;
	}


	close(record_fd);
	return rc;
}
