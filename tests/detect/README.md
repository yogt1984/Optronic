# Reference scene

`scene.gray` is 768x576 raw 8-bit luma - the darknet sample image, decoded, so
the test needs no image decoder of its own and no network. Regenerate with:

    gst-launch-1.0 filesrc location=dog.jpg ! jpegdec ! videoconvert \
      ! video/x-raw,format=GRAY8 ! filesink location=tests/detect/scene.gray

Greyscale on purpose: the detector runs on the luma plane of an NV12 frame, so
the test feeds it exactly what the pipeline would.
