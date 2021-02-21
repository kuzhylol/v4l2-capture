# v4l2-capture

## Build

```
gcc -O2 -std=gnu2x -Wall -Wextra -Wpedantic -Werror v4l2-capture.c -o v4l2-capture -lpthread
```

## Execute
```
./v4l2-capture
./v4l2-capture ovideo.raw
```

## Watch output RAW video file

```
gst-launch-1.0 filesrc location=demo.raw ! videoparse width=1280 height=960 format=4  interlaced=true ! autovideosink
```
