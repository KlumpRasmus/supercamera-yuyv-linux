// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * supercamera_yuyv.c - V4L2 driver for a Geek/Szitman "SuperCamera"
 * USB endoscope variant.
 *
 * Variant matched by this driver:
 *   USB VID:PID     2ce3:3828
 *   Interface       0
 *   Bulk IN         0x82
 *   Bulk OUT        0x02
 *   Video format    320x240 YUYV (YUY2), 2 bytes/pixel
 *
 * Protocol behavior is based on the working userspace implementation:
 *   https://github.com/Bognabon/Endoscope_Viewer
 *
 * Other devices using the same VID:PID are known to use a different protocol.
 */

#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/videodev2.h>
#include <linux/kthread.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define DRIVER_NAME "supercamera_yuyv"

#define SC_VENDOR_ID  0x2ce3
#define SC_PRODUCT_ID 0x3828

#define SC_INTERFACE 0
#define SC_EP_IN     0x82
#define SC_EP_OUT    0x02

#define SC_WIDTH      320U
#define SC_HEIGHT     240U
#define SC_BPP        2U
#define SC_FRAME_SIZE (SC_WIDTH * SC_HEIGHT * SC_BPP)

#define SC_PREAMBLE_SIZE 512U
#define SC_READ_SIZE     (SC_FRAME_SIZE + SC_PREAMBLE_SIZE)
#define SC_USB_TIMEOUT_MS 1000

struct sc_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct sc_dev {
	struct usb_device *udev;
	struct usb_interface *intf;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vbq;

	struct mutex lock;
	spinlock_t qlock;
	struct list_head queued;

	struct task_struct *thread;
	u8 *xfer_buf;

	bool disconnected;
	bool first_block;
	u32 sequence;
};

static inline struct sc_buffer *to_sc_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct sc_buffer, vb);
}

static void sc_fill_pix(struct v4l2_pix_format *pix)
{
	pix->width = SC_WIDTH;
	pix->height = SC_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = SC_WIDTH * SC_BPP;
	pix->sizeimage = SC_FRAME_SIZE;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_DEFAULT;
	pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int sc_getinfo(struct sc_dev *sc)
{
	u8 *buf;
	int ret;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = usb_control_msg(sc->udev, usb_rcvctrlpipe(sc->udev, 0),
			      0x00, 0xA0, 0x0005, 0x0000,
			      buf, 512, SC_USB_TIMEOUT_MS);
	kfree(buf);

	return ret < 0 ? ret : 0;
}

static int sc_camera_up(struct sc_dev *sc)
{
	u8 data[64] = { 0 };
	int ret;

	/*
	 * usb_control_msg() requires a DMA-capable data buffer.  The protocol
	 * payload is naturally a small stack buffer, so use the send helper,
	 * which copies it to suitable memory internally.
	 */
	ret = usb_control_msg_send(sc->udev, 0, 0x01, 0x20,
			           0x0005, 0x0000, data, sizeof(data),
			           SC_USB_TIMEOUT_MS, GFP_KERNEL);
	if (ret)
		return ret;

	msleep(200);
	return 0;
}

static void sc_camera_down(struct sc_dev *sc)
{
	int ret;

	if (READ_ONCE(sc->disconnected))
		return;

	ret = usb_control_msg(sc->udev, usb_sndctrlpipe(sc->udev, 0),
			      0x02, 0x20, 0x0005, 0x0000,
			      NULL, 0, SC_USB_TIMEOUT_MS);
	if (ret < 0)
		dev_dbg(&sc->intf->dev, "camera_down failed: %d\n", ret);
}

static void sc_return_all_buffers(struct sc_dev *sc,
				  enum vb2_buffer_state state)
{
	struct sc_buffer *buf;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&sc->qlock, flags);
		if (list_empty(&sc->queued)) {
			spin_unlock_irqrestore(&sc->qlock, flags);
			break;
		}
		buf = list_first_entry(&sc->queued, struct sc_buffer, list);
		list_del(&buf->list);
		spin_unlock_irqrestore(&sc->qlock, flags);

		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static struct sc_buffer *sc_pop_buffer(struct sc_dev *sc)
{
	struct sc_buffer *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&sc->qlock, flags);
	if (!list_empty(&sc->queued)) {
		buf = list_first_entry(&sc->queued, struct sc_buffer, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&sc->qlock, flags);

	return buf;
}

static int sc_capture_thread(void *data)
{
	struct sc_dev *sc = data;

	while (!kthread_should_stop()) {
		struct sc_buffer *buf;
		void *dst;
		unsigned int offset;
		int actual = 0;
		int ret;

		if (READ_ONCE(sc->disconnected))
			break;

		ret = usb_bulk_msg(sc->udev,
				   usb_rcvbulkpipe(sc->udev, SC_EP_IN & 0x0f),
				   sc->xfer_buf, SC_READ_SIZE, &actual,
				   SC_USB_TIMEOUT_MS);

		if (kthread_should_stop())
			break;

		if (ret == -ETIMEDOUT)
			continue;

		if (ret == -EPIPE) {
			usb_clear_halt(sc->udev,
				       usb_rcvbulkpipe(sc->udev, SC_EP_IN & 0x0f));
			continue;
		}

		if (ret < 0) {
			if (!READ_ONCE(sc->disconnected))
				dev_warn(&sc->intf->dev, "bulk read failed: %d\n", ret);
			continue;
		}

		offset = sc->first_block ? SC_PREAMBLE_SIZE : 0;
		sc->first_block = false;

		if (actual < offset + SC_FRAME_SIZE) {
			dev_dbg(&sc->intf->dev,
				"short video block: %d bytes, need %u\n",
				actual, offset + SC_FRAME_SIZE);
			continue;
		}

		buf = sc_pop_buffer(sc);
		if (!buf)
			continue;

		dst = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
		if (!dst) {
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
			continue;
		}

		memcpy(dst, sc->xfer_buf + offset, SC_FRAME_SIZE);
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, SC_FRAME_SIZE);
		buf->vb.sequence = sc->sequence++;
		buf->vb.field = V4L2_FIELD_NONE;
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}

	return 0;
}

/* videobuf2 */

static int sc_queue_setup(struct vb2_queue *q,
			  unsigned int *num_buffers,
			  unsigned int *num_planes,
			  unsigned int sizes[],
			  struct device *alloc_devs[])
{
	if (*num_planes) {
		if (sizes[0] < SC_FRAME_SIZE)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = SC_FRAME_SIZE;
	return 0;
}

static int sc_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < SC_FRAME_SIZE)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, SC_FRAME_SIZE);
	return 0;
}

static void sc_buf_queue(struct vb2_buffer *vb)
{
	struct sc_dev *sc = vb2_get_drv_priv(vb->vb2_queue);
	struct sc_buffer *buf = to_sc_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&sc->qlock, flags);
	list_add_tail(&buf->list, &sc->queued);
	spin_unlock_irqrestore(&sc->qlock, flags);
}

static int sc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sc_dev *sc = vb2_get_drv_priv(q);
	int ret;

	if (READ_ONCE(sc->disconnected)) {
		ret = -ENODEV;
		goto err_buffers;
	}

	usb_clear_halt(sc->udev, usb_rcvbulkpipe(sc->udev, SC_EP_IN & 0x0f));
	usb_clear_halt(sc->udev, usb_sndbulkpipe(sc->udev, SC_EP_OUT & 0x0f));

	ret = sc_getinfo(sc);
	if (ret)
		dev_dbg(&sc->intf->dev, "getinfo failed: %d\n", ret);

	ret = sc_camera_up(sc);
	if (ret) {
		dev_err(&sc->intf->dev, "camera_up failed: %d\n", ret);
		goto err_buffers;
	}

	sc->first_block = true;
	sc->sequence = 0;

	sc->thread = kthread_run(sc_capture_thread, sc, DRIVER_NAME);
	if (IS_ERR(sc->thread)) {
		ret = PTR_ERR(sc->thread);
		sc->thread = NULL;
		sc_camera_down(sc);
		goto err_buffers;
	}

	return 0;

err_buffers:
	sc_return_all_buffers(sc, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void sc_stop_streaming(struct vb2_queue *q)
{
	struct sc_dev *sc = vb2_get_drv_priv(q);

	if (sc->thread) {
		kthread_stop(sc->thread);
		sc->thread = NULL;
	}

	sc_camera_down(sc);
	sc_return_all_buffers(sc, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops sc_vb2_ops = {
	.queue_setup     = sc_queue_setup,
	.buf_prepare     = sc_buf_prepare,
	.buf_queue       = sc_buf_queue,
	.start_streaming = sc_start_streaming,
	.stop_streaming  = sc_stop_streaming,
};

/* V4L2 ioctls */

static int sc_querycap(struct file *file, void *priv,
		       struct v4l2_capability *cap)
{
	struct sc_dev *sc = video_drvdata(file);

	strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Geek/Szitman SuperCamera YUYV", sizeof(cap->card));
	usb_make_path(sc->udev, cap->bus_info, sizeof(cap->bus_info));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE |
			   V4L2_CAP_STREAMING |
			   V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int sc_enum_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	strscpy(f->description, "YUYV 4:2:2", sizeof(f->description));
	return 0;
}

static int sc_g_fmt_vid_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	sc_fill_pix(&f->fmt.pix);
	return 0;
}

static int sc_try_fmt_vid_cap(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	sc_fill_pix(&f->fmt.pix);
	return 0;
}

static int sc_s_fmt_vid_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct sc_dev *sc = video_drvdata(file);

	if (vb2_is_busy(&sc->vbq))
		return -EBUSY;

	sc_fill_pix(&f->fmt.pix);
	return 0;
}

static int sc_enum_framesizes(struct file *file, void *priv,
			      struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index != 0 || fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = SC_WIDTH;
	fsize->discrete.height = SC_HEIGHT;
	return 0;
}

static int sc_enum_input(struct file *file, void *priv,
			 struct v4l2_input *input)
{
	if (input->index != 0)
		return -EINVAL;

	strscpy(input->name, "Camera", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int sc_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int sc_s_input(struct file *file, void *priv, unsigned int i)
{
	return i == 0 ? 0 : -EINVAL;
}

static const struct v4l2_ioctl_ops sc_ioctl_ops = {
	.vidioc_querycap          = sc_querycap,
	.vidioc_enum_fmt_vid_cap  = sc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap     = sc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap   = sc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap     = sc_s_fmt_vid_cap,
	.vidioc_enum_framesizes   = sc_enum_framesizes,
	.vidioc_enum_input        = sc_enum_input,
	.vidioc_g_input           = sc_g_input,
	.vidioc_s_input           = sc_s_input,
	.vidioc_reqbufs           = vb2_ioctl_reqbufs,
	.vidioc_create_bufs       = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf       = vb2_ioctl_prepare_buf,
	.vidioc_querybuf          = vb2_ioctl_querybuf,
	.vidioc_qbuf              = vb2_ioctl_qbuf,
	.vidioc_dqbuf             = vb2_ioctl_dqbuf,
	.vidioc_expbuf            = vb2_ioctl_expbuf,
	.vidioc_streamon          = vb2_ioctl_streamon,
	.vidioc_streamoff         = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations sc_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.mmap           = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = video_ioctl2,
#endif
};

/* USB */

static int sc_check_interface(struct usb_interface *intf)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	bool have_in = false;
	bool have_out = false;
	int i;

	if (alt->desc.bInterfaceNumber != SC_INTERFACE)
		return -ENODEV;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		const struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(ep) && ep->bEndpointAddress == SC_EP_IN)
			have_in = true;
		if (usb_endpoint_is_bulk_out(ep) && ep->bEndpointAddress == SC_EP_OUT)
			have_out = true;
	}

	return have_in && have_out ? 0 : -ENODEV;
}

static void sc_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct sc_dev *sc = container_of(v4l2_dev, struct sc_dev, v4l2_dev);

	/*
	 * video_register_device() holds a reference on v4l2_dev.  For a
	 * hot-pluggable USB device the final close can happen after disconnect(),
	 * so keep the containing sc_dev alive until that last reference is gone.
	 */
	v4l2_device_unregister(v4l2_dev);
	kfree(sc->xfer_buf);
	usb_put_dev(sc->udev);
	kfree(sc);
}

static int sc_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct sc_dev *sc;
	int ret;

	ret = sc_check_interface(intf);
	if (ret)
		return ret;

	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;

	sc->udev = usb_get_dev(interface_to_usbdev(intf));
	sc->intf = intf;
	mutex_init(&sc->lock);
	spin_lock_init(&sc->qlock);
	INIT_LIST_HEAD(&sc->queued);

	/* usb_bulk_msg() needs a USB-mappable (non-vmalloc) buffer. */
	sc->xfer_buf = kmalloc(SC_READ_SIZE, GFP_KERNEL);
	if (!sc->xfer_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	sc->v4l2_dev.release = sc_v4l2_release;
	ret = v4l2_device_register(&intf->dev, &sc->v4l2_dev);
	if (ret)
		goto err_buf;

	sc->vbq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	sc->vbq.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	sc->vbq.drv_priv = sc;
	sc->vbq.buf_struct_size = sizeof(struct sc_buffer);
	sc->vbq.ops = &sc_vb2_ops;
	sc->vbq.mem_ops = &vb2_vmalloc_memops;
	sc->vbq.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	sc->vbq.lock = &sc->lock;
	sc->vbq.dev = &intf->dev;

	ret = vb2_queue_init(&sc->vbq);
	if (ret)
		goto err_v4l2;

	strscpy(sc->vdev.name, "Geek/Szitman SuperCamera YUYV", sizeof(sc->vdev.name));
	sc->vdev.v4l2_dev = &sc->v4l2_dev;
	sc->vdev.fops = &sc_fops;
	sc->vdev.ioctl_ops = &sc_ioctl_ops;
	sc->vdev.release = video_device_release_empty;
	sc->vdev.lock = &sc->lock;
	sc->vdev.queue = &sc->vbq;
	sc->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
			       V4L2_CAP_STREAMING |
			       V4L2_CAP_READWRITE;
	sc->vdev.vfl_dir = VFL_DIR_RX;

	video_set_drvdata(&sc->vdev, sc);
	usb_set_intfdata(intf, sc);

	ret = video_register_device(&sc->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_intfdata;

	dev_info(&intf->dev,
		 "registered %s as /dev/video%d (320x240 YUYV)\n",
		 sc->vdev.name, sc->vdev.num);
	return 0;

err_intfdata:
	usb_set_intfdata(intf, NULL);
err_v4l2:
	v4l2_device_unregister(&sc->v4l2_dev);
err_buf:
	kfree(sc->xfer_buf);
err_put:
	usb_put_dev(sc->udev);
	kfree(sc);
	return ret;
}

static void sc_disconnect(struct usb_interface *intf)
{
	struct sc_dev *sc = usb_get_intfdata(intf);

	if (!sc)
		return;

	usb_set_intfdata(intf, NULL);

	/*
	 * Serialize against V4L2/vb2 operations while marking the USB parent as
	 * gone and removing the video node.  The v4l2_device initial reference
	 * keeps sc alive until after we drop the lock below.
	 */
	mutex_lock(&sc->lock);
	sc->disconnected = true;

	if (sc->thread) {
		kthread_stop(sc->thread);
		sc->thread = NULL;
	}

	sc_return_all_buffers(sc, VB2_BUF_STATE_ERROR);
	v4l2_device_disconnect(&sc->v4l2_dev);
	video_unregister_device(&sc->vdev);
	mutex_unlock(&sc->lock);

	/* Drop the initial v4l2_device reference.  If userspace still has the
	 * node open, final cleanup is deferred until its last close.
	 */
	v4l2_device_put(&sc->v4l2_dev);
	dev_info(&intf->dev, "disconnected\n");
}

static const struct usb_device_id sc_id_table[] = {
	{ USB_DEVICE(SC_VENDOR_ID, SC_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, sc_id_table);

static struct usb_driver sc_usb_driver = {
	.name       = DRIVER_NAME,
	.probe      = sc_probe,
	.disconnect = sc_disconnect,
	.id_table   = sc_id_table,
};

module_usb_driver(sc_usb_driver);

MODULE_AUTHOR("KlumpRasmus");
MODULE_DESCRIPTION("V4L2 driver for 2ce3:3828 SuperCamera 320x240 YUYV variant");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
