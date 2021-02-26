/* Aravis - Digital camera library
 *
 * Copyright © 2009-2019 Emmanuel Pacaud
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
 * SECTION: arvv4l2device
 * @short_description: V4l2 device
 */

#include <arvdeviceprivate.h>
#include <arvv4l2deviceprivate.h>
#include <arvv4l2streamprivate.h>
#include <arvdebug.h>

enum
{
	PROP_0,
};

typedef struct {
	ArvGc *genicam;
} ArvV4l2DevicePrivate;

struct _ArvV4l2Device {
	ArvDevice device;
};

struct _ArvV4l2DeviceClass {
	ArvDeviceClass parent_class;
};

G_DEFINE_TYPE_WITH_CODE (ArvV4l2Device, arv_v4l2_device, ARV_TYPE_DEVICE, G_ADD_PRIVATE (ArvV4l2Device))

/* ArvV4l2Device implemenation */

/* ArvDevice implemenation */

static ArvStream *
arv_v4l2_device_create_stream (ArvDevice *device, ArvStreamCallback callback, void *user_data, GError **error)
{
	return arv_v4l2_stream_new (ARV_V4L2_DEVICE (device), callback, user_data, error);
}

static const char *
arv_v4l2_device_get_genicam_xml (ArvDevice *device, size_t *size)
{
	/* FIXME */
	return NULL;
}

static ArvGc *
arv_v4l2_device_get_genicam (ArvDevice *device)
{
	ArvV4l2DevicePrivate *priv = arv_v4l2_device_get_instance_private (ARV_V4L2_DEVICE (device));

	return priv->genicam;
}

static gboolean
arv_v4l2_device_read_memory (ArvDevice *device, guint64 address, guint32 size, void *buffer, GError **error)
{
	/* FIXME */
	return FALSE;
}

static gboolean
arv_v4l2_device_write_memory (ArvDevice *device, guint64 address, guint32 size, void *buffer, GError **error)
{
	/* FIXME */
	return FALSE;
}

static gboolean
arv_v4l2_device_read_register (ArvDevice *device, guint64 address, guint32 *value, GError **error)
{
	/* FIXME */
	return FALSE;
}

static gboolean
arv_v4l2_device_write_register (ArvDevice *device, guint64 address, guint32 value, GError **error)
{
	/* FIXME */
	return FALSE;
}

/**
 * arv_v4l2_device_new:
 * @error: a #GError placeholder, %NULL to ignore
 *
 * Returns: a newly created #ArvDevice connected to a v4l2 device
 *
 * Since: 0.8.7
 */

ArvDevice *
arv_v4l2_device_new (GError **error)
{
	return g_initable_new (ARV_TYPE_V4L2_DEVICE, NULL, error, NULL);
}

static void
arv_v4l2_device_init (ArvV4l2Device *v4l2_device)
{
}

static void
arv_v4l2_device_finalize (GObject *object)
{
	ArvV4l2DevicePrivate *priv = arv_v4l2_device_get_instance_private (ARV_V4L2_DEVICE (object));

	g_clear_object (&priv->genicam);

	G_OBJECT_CLASS (arv_v4l2_device_parent_class)->finalize (object);
}

static void
arv_v4l2_device_constructed (GObject *self)
{
}

static void
arv_v4l2_device_set_property (GObject *self, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	switch (prop_id)
	{
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
			break;
	}
}

static void
arv_v4l2_device_class_init (ArvV4l2DeviceClass *v4l2_device_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (v4l2_device_class);
	ArvDeviceClass *device_class = ARV_DEVICE_CLASS (v4l2_device_class);

	object_class->finalize = arv_v4l2_device_finalize;
	object_class->constructed = arv_v4l2_device_constructed;
	object_class->set_property = arv_v4l2_device_set_property;

	device_class->create_stream = arv_v4l2_device_create_stream;
	device_class->get_genicam_xml = arv_v4l2_device_get_genicam_xml;
	device_class->get_genicam = arv_v4l2_device_get_genicam;
	device_class->read_memory = arv_v4l2_device_read_memory;
	device_class->write_memory = arv_v4l2_device_write_memory;
	device_class->read_register = arv_v4l2_device_read_register;
	device_class->write_register = arv_v4l2_device_write_register;
}
