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
#include <arvmisc.h>

struct _ArvV4l2Interface {
	ArvInterface	interface;
};

struct _ArvV4l2InterfaceClass {
	ArvInterfaceClass parent_class;
};

G_DEFINE_TYPE (ArvV4l2Interface, arv_v4l2_interface, ARV_TYPE_INTERFACE)

static void
arv_v4l2_interface_update_device_list (ArvInterface *interface, GArray *device_ids)
{
	g_assert (device_ids->len == 0);
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
