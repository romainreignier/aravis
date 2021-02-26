/* Aravis - Digital camera library
 *
 * Copyright © 2009-2021 Emmanuel Pacaud
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Emmanuel Pacaud <emmanuel@gnome.org>
 */

/**
 * SECTION: arvv4l2interface
 * @short_description: V4l2 interface
 */

#include <arvv4l2interfaceprivate.h>
#include <arvv4l2deviceprivate.h>
#include <arvinterfaceprivate.h>
#include <arvv4l2device.h>
#include <arvdebug.h>
#include <gudev/gudev.h>
#include <libv4l2.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <errno.h>
#include <arvmisc.h>

struct _ArvV4l2Interface {
	ArvInterface	interface;

	GUdevClient *udev;
};

struct _ArvV4l2InterfaceClass {
	ArvInterfaceClass parent_class;
};

G_DEFINE_TYPE (ArvV4l2Interface, arv_v4l2_interface, ARV_TYPE_INTERFACE)

static void
arv_v4l2_interface_update_device_list (ArvInterface *interface, GArray *device_ids)
{
	ArvV4l2Interface *v4l2_interface = ARV_V4L2_INTERFACE (interface);
	GList *devices, *elem;

	g_assert (device_ids->len == 0);

	devices = g_udev_client_query_by_subsystem (v4l2_interface->udev, "video4linux");

	for (elem = g_list_first (devices); elem; elem = g_list_next (elem)) {
		const char *device_file;

		device_file = g_udev_device_get_device_file (elem->data);

		if (strncmp ("/dev/vbi", device_file,  8) != 0) {
			ArvInterfaceDeviceIds *ids;
			int fd;

			fd = v4l2_open (device_file, O_RDWR);
			if (fd != -1) {
				struct v4l2_capability cap;

				if (v4l2_ioctl (fd, VIDIOC_QUERYCAP, &cap) != -1) {
					ids = g_new0 (ArvInterfaceDeviceIds, 1);

					ids->device = g_strdup ((char *) cap.card);
					ids->physical = g_strdup ((char *) cap.bus_info);

					g_array_append_val (device_ids, ids);
				}
				v4l2_close (fd);
			}
		}

		g_object_unref (elem->data);
	}

	g_list_free (devices);
}

static ArvDevice *
arv_v4l2_interface_open_device (ArvInterface *interface, const char *device_id, GError **error)
{
	return NULL;
}

static ArvInterface *v4l2_interface = NULL;
static GMutex v4l2_interface_mutex;

/**
 * arv_v4l2_interface_get_instance:
 *
 * Gets the unique instance of the v4l2 interface.
 *
 * Returns: (transfer none): a #ArvInterface singleton.
 */

ArvInterface *
arv_v4l2_interface_get_instance (void)
{
	g_mutex_lock (&v4l2_interface_mutex);

	if (v4l2_interface == NULL)
		v4l2_interface = g_object_new (ARV_TYPE_V4L2_INTERFACE, NULL);

	g_mutex_unlock (&v4l2_interface_mutex);

	return ARV_INTERFACE (v4l2_interface);
}

void
arv_v4l2_interface_destroy_instance (void)
{
	g_mutex_lock (&v4l2_interface_mutex);

	if (v4l2_interface != NULL) {
		g_object_unref (v4l2_interface);
		v4l2_interface = NULL;
	}

	g_mutex_unlock (&v4l2_interface_mutex);
}

static void
arv_v4l2_interface_init (ArvV4l2Interface *v4l2_interface)
{
	const gchar *const subsystems[] = {"video4linux", NULL};

	v4l2_interface->udev = g_udev_client_new (subsystems);
}

static void
arv_v4l2_interface_finalize (GObject *object)
{
	G_OBJECT_CLASS (arv_v4l2_interface_parent_class)->finalize (object);
}

static void
arv_v4l2_interface_class_init (ArvV4l2InterfaceClass *v4l2_interface_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (v4l2_interface_class);
	ArvInterfaceClass *interface_class = ARV_INTERFACE_CLASS (v4l2_interface_class);

	object_class->finalize = arv_v4l2_interface_finalize;

	interface_class->update_device_list = arv_v4l2_interface_update_device_list;
	interface_class->open_device = arv_v4l2_interface_open_device;

	interface_class->protocol = "V4L2";
}
