/* Aravis - Digital camera library
 *
 * Copyright © 2009-2025 Emmanuel Pacaud <emmanuel.pacaud@free.fr>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Emmanuel Pacaud <emmanuel.pacaud@free.fr>
 */

#include <arvgvfakecamera.h>
#include <arvfakecamera.h>
#include <arvbufferprivate.h>
#include <arvgvcpprivate.h>
#include <arvgvspprivate.h>
#include <arvmisc.h>
#include <arvmiscprivate.h>
#include <arvnetworkprivate.h>

/**
 * SECTION: arvgvfakecamera
 * @short_description: GigE Vision Simulator
 *
 * #ArvGvFakeCamera is a class that simulates a real GigEVision camera.
 */

#define ARV_GV_FAKE_CAMERA_BUFFER_SIZE	65536

/* Number of sent frames kept in memory for the packet resend requests */
#define ARV_GV_FAKE_CAMERA_N_FRAME_HISTORY	2

/* Requested size of the kernel send buffer of the GVSP socket */
#define ARV_GV_FAKE_CAMERA_GVSP_SOCKET_BUFFER_SIZE	(4 * 1024 * 1024)

/* Number of attempts and delay between them when a GVSP packet can not be sent */
#define ARV_GV_FAKE_CAMERA_GVSP_SEND_N_RETRIES		50
#define ARV_GV_FAKE_CAMERA_GVSP_SEND_RETRY_DELAY_US	200

/* Inter packet delays shorter than this one are accumulated before sleeping */
#define ARV_GV_FAKE_CAMERA_MIN_SLEEP_NS		100000

/* Number of GVSP packets sent between two checks of the incoming control packets */
#define ARV_GV_FAKE_CAMERA_CONTROL_CHECK_PERIOD	32

enum {
	ARV_GV_FAKE_CAMERA_INPUT_SOCKET_GVCP = 0,
	ARV_GV_FAKE_CAMERA_INPUT_SOCKET_GLOBAL_DISCOVERY,
	ARV_GV_FAKE_CAMERA_INPUT_SOCKET_SUBNET_DISCOVERY,
	ARV_GV_FAKE_CAMERA_N_INPUT_SOCKETS
};

enum
{
  PROP_0,
  PROP_INTERFACE_NAME,
  PROP_SERIAL_NUMBER,
  PROP_GENICAM_FILENAME,
  PROP_GVSP_LOST_PACKET_RATIO,
  PROP_PACKET_RESEND,
  PROP_CM_DOMAIN
};

typedef struct {
	ArvBuffer *buffer;
	guint32 gv_packet_size;
	guint32 n_packets;
} ArvGvFakeCameraFrame;

typedef struct {
	char *interface_name;
	char *serial_number;
	char *genicam_filename;

	ArvFakeCamera *camera;

	gboolean is_running;

	GPollFD socket_fds[ARV_GV_FAKE_CAMERA_N_INPUT_SOCKETS];
	guint n_socket_fds;

	GSocketAddress *controller_address;
	gint64 controller_time;

	GSocket *input_sockets[ARV_GV_FAKE_CAMERA_N_INPUT_SOCKETS];

	GSocket *gvsp_socket;

	GThread *thread;
	gboolean cancel;

	double gvsp_lost_packet_ratio;
	gboolean packet_resend;

	/* Streaming state, only used by the camera thread */
	GSocketAddress *stream_address;
	void *packet_buffer;
	size_t payload;
	ArvGvFakeCameraFrame frames[ARV_GV_FAKE_CAMERA_N_FRAME_HISTORY];
	unsigned int current_frame;
	gint64 next_packet_time_ns;

	guint64 n_resend_requests;
	guint64 n_resent_packets;
	guint64 n_unavailable_packets;
	guint64 n_send_failures;
} ArvGvFakeCameraPrivate;

struct _ArvGvFakeCamera {
	GObject	object;

	ArvGvFakeCameraPrivate *priv;
};

struct _ArvGvFakeCameraClass {
	GObjectClass parent_class;
};

G_DEFINE_TYPE_WITH_CODE (ArvGvFakeCamera, arv_gv_fake_camera, G_TYPE_OBJECT, G_ADD_PRIVATE (ArvGvFakeCamera))

static gboolean
_g_inet_socket_address_is_equal (GInetSocketAddress *a, GInetSocketAddress *b)
{
	if (!G_IS_INET_SOCKET_ADDRESS (a) ||
	    !G_IS_INET_SOCKET_ADDRESS (b))
		return FALSE;

	if (g_inet_socket_address_get_port (a) != g_inet_socket_address_get_port (b))
		return FALSE;

	return g_inet_address_equal (g_inet_socket_address_get_address (a),
				     g_inet_socket_address_get_address (b));
}

static guint64
_get_packet_delay_ns (ArvGvFakeCamera *gv_fake_camera)
{
	ArvFakeCamera *camera = gv_fake_camera->priv->camera;
	guint32 delay_ticks = 0;
	guint32 frequency_high = 0;
	guint32 frequency_low = 0;
	guint64 frequency;

	arv_fake_camera_read_register (camera, ARV_GVBS_STREAM_CHANNEL_0_PACKET_DELAY_OFFSET, &delay_ticks);
	if (delay_ticks == 0)
		return 0;

	arv_fake_camera_read_register (camera, ARV_GVBS_TIMESTAMP_TICK_FREQUENCY_HIGH_OFFSET, &frequency_high);
	arv_fake_camera_read_register (camera, ARV_GVBS_TIMESTAMP_TICK_FREQUENCY_LOW_OFFSET, &frequency_low);
	frequency = ((guint64) frequency_high << 32) | frequency_low;
	if (frequency == 0)
		return 0;

	return (guint64) delay_ticks * 1000000000ULL / frequency;
}

/* Wait until the next GVSP packet can be sent, according to the stream channel packet delay (GevSCPD). Small delays
 * are accumulated, in order to keep the average throughput without sleeping between each packet. */

static void
_wait_for_next_packet (ArvGvFakeCamera *gv_fake_camera, guint64 packet_delay_ns)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	gint64 now_ns;

	if (packet_delay_ns == 0)
		return;

	now_ns = g_get_monotonic_time () * 1000;

	if (priv->next_packet_time_ns - now_ns >= ARV_GV_FAKE_CAMERA_MIN_SLEEP_NS) {
		g_usleep ((priv->next_packet_time_ns - now_ns) / 1000);
		now_ns = g_get_monotonic_time () * 1000;
	}

	/* Do not try to catch up if we are late */
	priv->next_packet_time_ns = MAX (priv->next_packet_time_ns, now_ns - ARV_GV_FAKE_CAMERA_MIN_SLEEP_NS) +
		packet_delay_ns;
}

static gboolean
_send_gvsp_packet (ArvGvFakeCamera *gv_fake_camera, size_t packet_size, guint64 frame_id, guint32 packet_id,
		   gboolean is_resend)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	GError *error = NULL;
	unsigned int i;

	if (priv->stream_address == NULL || priv->gvsp_socket == NULL)
		return FALSE;

	if (priv->gvsp_lost_packet_ratio > 0.0 &&
	    g_random_double () < priv->gvsp_lost_packet_ratio) {
		arv_info_stream_thread ("[GvFakeCamera::send_gvsp_packet] Drop %sGVSP packet %u of frame %"
					G_GUINT64_FORMAT, is_resend ? "resent " : "", packet_id, frame_id);
		return TRUE;
	}

	for (i = 0; i < ARV_GV_FAKE_CAMERA_GVSP_SEND_N_RETRIES; i++) {
		if (g_socket_send_to (priv->gvsp_socket, priv->stream_address,
				      priv->packet_buffer, packet_size, NULL, &error) >= 0)
			return TRUE;

		/* The socket is blocking, but sendto can still fail if the kernel queues are full (ENOBUFS),
		 * typically on virtual interfaces. Wait a bit for them to be emptied. */
		if (i == ARV_GV_FAKE_CAMERA_GVSP_SEND_N_RETRIES - 1 ||
		    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
		    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
		    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED))
			break;

		g_clear_error (&error);
		g_usleep (ARV_GV_FAKE_CAMERA_GVSP_SEND_RETRY_DELAY_US);
	}

	priv->n_send_failures++;
	arv_warning_stream_thread ("[GvFakeCamera::send_gvsp_packet] Failed to send %sGVSP packet %u of frame %"
				   G_GUINT64_FORMAT ": %s", is_resend ? "resent " : "",
				   packet_id, frame_id, error != NULL ? error->message : "unknown error");
	g_clear_error (&error);

	return FALSE;
}

/* Build and send the GVSP packet @packet_id of @frame: 0 is the leader, then the payload packets, and the last one
 * is the trailer. */

static gboolean
_send_frame_packet (ArvGvFakeCamera *gv_fake_camera, ArvGvFakeCameraFrame *frame, guint32 packet_id,
		    guint64 packet_delay_ns, gboolean is_resend)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	ArvBuffer *buffer = frame->buffer;
	ArvGvspPacket *packet;
	size_t packet_size = 0;
	guint16 frame_id;

	g_return_val_if_fail (packet_id < frame->n_packets, FALSE);

	frame_id = buffer->priv->frame_id;

	if (packet_id == 0) {
		packet = arv_gvsp_packet_new_image_leader (frame_id, packet_id,
							   arv_buffer_get_timestamp (buffer),
							   arv_buffer_get_image_pixel_format (buffer),
							   arv_buffer_get_image_width (buffer),
							   arv_buffer_get_image_height (buffer),
							   arv_buffer_get_image_x (buffer),
							   arv_buffer_get_image_y (buffer),
							   0, 0,
							   priv->packet_buffer, ARV_GV_FAKE_CAMERA_BUFFER_SIZE,
							   &packet_size);
	} else if (packet_id == frame->n_packets - 1) {
		packet = arv_gvsp_packet_new_data_trailer (frame_id, packet_id,
							   arv_buffer_get_image_height (buffer),
							   priv->packet_buffer, ARV_GV_FAKE_CAMERA_BUFFER_SIZE,
							   &packet_size);
	} else {
		size_t block_size = frame->gv_packet_size - ARV_GVSP_PACKET_PROTOCOL_OVERHEAD (FALSE);
		size_t offset = (size_t) (packet_id - 1) * block_size;

		packet = arv_gvsp_packet_new_payload (frame_id, packet_id,
						      MIN (block_size, priv->payload - offset),
						      ((char *) buffer->priv->data) + offset,
						      priv->packet_buffer, ARV_GV_FAKE_CAMERA_BUFFER_SIZE,
						      &packet_size);
	}

	if (packet == NULL) {
		arv_warning_stream_thread ("[GvFakeCamera::send_frame_packet] Failed to build packet %u", packet_id);
		return FALSE;
	}

	_wait_for_next_packet (gv_fake_camera, packet_delay_ns);

	return _send_gvsp_packet (gv_fake_camera, packet_size, frame_id, packet_id, is_resend);
}

static void
_send_unavailable_packet (ArvGvFakeCamera *gv_fake_camera, guint16 frame_id, guint32 packet_id)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	ArvGvspPacket *packet;
	size_t packet_size = 0;
	char dummy = 0;

	packet = arv_gvsp_packet_new_payload (frame_id, packet_id, 0, &dummy,
					      priv->packet_buffer, ARV_GV_FAKE_CAMERA_BUFFER_SIZE, &packet_size);
	if (packet == NULL)
		return;

	packet->status = g_htons (ARV_GVSP_PACKET_STATUS_PACKET_UNAVAILABLE);

	_send_gvsp_packet (gv_fake_camera, packet_size, frame_id, packet_id, TRUE);
}

static void
_handle_packet_resend (ArvGvFakeCamera *gv_fake_camera, ArvGvcpPacket *packet, size_t size)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	ArvGvFakeCameraFrame *frame = NULL;
	const guint32 *data;
	gboolean extended_ids;
	guint64 frame_id;
	guint32 first_block;
	guint32 last_block;
	guint64 packet_delay_ns;
	guint32 i;

	extended_ids = (packet->header.packet_flags & ARV_GVCP_CMD_PACKET_FLAGS_EXTENDED_IDS) != 0;

	if (size < sizeof (ArvGvcpHeader) + sizeof (guint32) * (extended_ids ? 5 : 3)) {
		arv_warning_device ("[GvFakeCamera::handle_packet_resend] Invalid packet size (%" G_GSIZE_FORMAT ")",
				    size);
		return;
	}

	data = (const guint32 *) &packet->data;

	if (extended_ids) {
		frame_id = ((guint64) g_ntohl (data[3]) << 32) | g_ntohl (data[4]);
		first_block = g_ntohl (data[1]);
		last_block = g_ntohl (data[2]);
	} else {
		/* Stream channel index in the 16 MSB, block id in the 16 LSB */
		frame_id = g_ntohl (data[0]) & 0xffff;
		first_block = g_ntohl (data[1]) & ARV_GVSP_PACKET_ID_MASK;
		last_block = g_ntohl (data[2]) & ARV_GVSP_PACKET_ID_MASK;
	}

	priv->n_resend_requests++;

	arv_debug_stream_thread ("[GvFakeCamera::handle_packet_resend] Resend request for frame %" G_GUINT64_FORMAT
				 ", packets %u to %u", frame_id, first_block, last_block);

	if (priv->stream_address == NULL)
		return;

	for (i = 0; i < ARV_GV_FAKE_CAMERA_N_FRAME_HISTORY; i++) {
		if (priv->frames[i].buffer != NULL &&
		    priv->frames[i].n_packets > 0 &&
		    priv->frames[i].buffer->priv->frame_id == frame_id) {
			frame = &priv->frames[i];
			break;
		}
	}

	if (last_block < first_block) {
		arv_warning_stream_thread ("[GvFakeCamera::handle_packet_resend] Invalid resend request"
					   " for frame %" G_GUINT64_FORMAT " (%u > %u)",
					   frame_id, first_block, last_block);
		return;
	}

	if (frame == NULL) {
		arv_info_stream_thread ("[GvFakeCamera::handle_packet_resend] Frame %" G_GUINT64_FORMAT
					" not available anymore", frame_id);
		priv->n_unavailable_packets += last_block - first_block + 1;
		_send_unavailable_packet (gv_fake_camera, frame_id, first_block);
		return;
	}

	packet_delay_ns = _get_packet_delay_ns (gv_fake_camera);

	for (i = first_block; i <= last_block; i++) {
		if (i >= frame->n_packets) {
			priv->n_unavailable_packets += last_block - i + 1;
			_send_unavailable_packet (gv_fake_camera, frame_id, i);
			break;
		}

		if (_send_frame_packet (gv_fake_camera, frame, i, packet_delay_ns, TRUE))
			priv->n_resent_packets++;
	}
}

static gboolean
_handle_control_packet (ArvGvFakeCamera *gv_fake_camera, GSocket *socket,
			GSocketAddress *remote_address,
			ArvGvcpPacket *packet, size_t size)
{
	ArvGvcpPacket *ack_packet = NULL;
	size_t ack_packet_size;
	guint32 block_address;
	guint32 block_size;
	guint16 packet_id;
	guint16 packet_type;
	guint32 register_address;
	guint32 register_value;
	gboolean write_access;
	gboolean success = FALSE;

	if (gv_fake_camera->priv->controller_address != NULL) {
		gint64 time;
		guint64 elapsed_ms;

		time = g_get_real_time ();

		elapsed_ms = (time - gv_fake_camera->priv->controller_time) / 1000;

		if (elapsed_ms > arv_fake_camera_get_heartbeat_timeout (gv_fake_camera->priv->camera)) {
			g_object_unref (gv_fake_camera->priv->controller_address);
			gv_fake_camera->priv->controller_address = NULL;
			write_access = TRUE;
			arv_warning_device ("[GvFakeCamera::handle_control_packet] Heartbeat timeout");
			arv_fake_camera_set_control_channel_privilege (gv_fake_camera->priv->camera, 0);
		} else
			write_access = _g_inet_socket_address_is_equal
				(G_INET_SOCKET_ADDRESS (remote_address),
				 G_INET_SOCKET_ADDRESS (gv_fake_camera->priv->controller_address));
	} else
		write_access = TRUE;


	arv_gvcp_packet_debug (packet, ARV_DEBUG_LEVEL_DEBUG);

	packet_id = arv_gvcp_packet_get_packet_id (packet, size);
	packet_type = arv_gvcp_packet_get_packet_type (packet, size);

	if (packet_type != ARV_GVCP_PACKET_TYPE_CMD) {
		arv_warning_device ("[GvFakeCamera::handle_control_packet] Unknown packet type");
		return FALSE;
	}

	switch (g_ntohs (packet->header.command)) {
		case ARV_GVCP_COMMAND_DISCOVERY_CMD:
			ack_packet = arv_gvcp_packet_new_discovery_ack (packet_id, &ack_packet_size);
			arv_info_device ("[GvFakeCamera::handle_control_packet] Discovery command");
			arv_fake_camera_read_memory (gv_fake_camera->priv->camera, 0, ARV_GVBS_DISCOVERY_DATA_SIZE,
						     &ack_packet->data);
			break;
		case ARV_GVCP_COMMAND_READ_MEMORY_CMD:
			arv_gvcp_packet_get_read_memory_cmd_infos (packet, size, &block_address, &block_size);
			arv_info_device ("[GvFakeCamera::handle_control_packet] Read memory command %d (%d)",
					  block_address, block_size);
			ack_packet = arv_gvcp_packet_new_read_memory_ack (block_address, block_size,
									  packet_id, &ack_packet_size);
			arv_fake_camera_read_memory (gv_fake_camera->priv->camera, block_address, block_size,
						     arv_gvcp_packet_get_read_memory_ack_data (ack_packet));
			break;
		case ARV_GVCP_COMMAND_WRITE_MEMORY_CMD:
			arv_gvcp_packet_get_write_memory_cmd_infos (packet, size, &block_address, &block_size);
			if (!write_access) {
				arv_warning_device("[GvFakeCamera::handle_control_packet]"
                                                   " Ignore Write memory command %d (%d) not controller",
					block_address, block_size);
				break;
			}

			arv_info_device ("[GvFakeCamera::handle_control_packet] Write memory command %d (%d)",
					  block_address, block_size);
			arv_fake_camera_write_memory (gv_fake_camera->priv->camera, block_address, block_size,
						      arv_gvcp_packet_get_write_memory_cmd_data (packet));
			ack_packet = arv_gvcp_packet_new_write_memory_ack (block_address, packet_id,
									   &ack_packet_size);
			break;
		case ARV_GVCP_COMMAND_READ_REGISTER_CMD:
			arv_gvcp_packet_get_read_register_cmd_infos (packet, size, &register_address);
			arv_fake_camera_read_register (gv_fake_camera->priv->camera, register_address, &register_value);
			arv_info_device ("[GvFakeCamera::handle_control_packet] Read register command %d -> %d",
					  register_address, register_value);
			ack_packet = arv_gvcp_packet_new_read_register_ack (register_value, packet_id,
									    &ack_packet_size);

			if (register_address == ARV_GVBS_CONTROL_CHANNEL_PRIVILEGE_OFFSET)
				gv_fake_camera->priv->controller_time = g_get_real_time ();

			break;
		case ARV_GVCP_COMMAND_WRITE_REGISTER_CMD:
			arv_gvcp_packet_get_write_register_cmd_infos (packet, size, &register_address, &register_value);
			if (!write_access) {
				arv_warning_device("[GvFakeCamera::handle_control_packet]"
                                                   " Ignore Write register command %d (%d) not controller",
					register_address, register_value);
				break;
			}

			arv_fake_camera_write_register (gv_fake_camera->priv->camera, register_address, register_value);
			arv_info_device ("[GvFakeCamera::handle_control_packet] Write register command %d -> %d",
					  register_address, register_value);
			ack_packet = arv_gvcp_packet_new_write_register_ack (1, packet_id,
									     &ack_packet_size);
			break;
		case ARV_GVCP_COMMAND_PACKET_RESEND_CMD:
			/* No acknowledge is sent for the packet resend command */
			if (gv_fake_camera->priv->packet_resend)
				_handle_packet_resend (gv_fake_camera, packet, size);
			else
				arv_warning_device ("[GvFakeCamera::handle_control_packet]"
						    " Ignore packet resend command, not supported");
			success = TRUE;
			break;
		default:
			arv_warning_device ("[GvFakeCamera::handle_control_packet] Unknown command");
	}

	if (ack_packet != NULL) {
		g_socket_send_to (socket, remote_address, (char *) ack_packet, ack_packet_size, NULL, NULL);
		arv_gvcp_packet_debug (ack_packet, ARV_DEBUG_LEVEL_DEBUG);
		g_free (ack_packet);

		success = TRUE;
	}

	if (gv_fake_camera->priv->controller_address == NULL &&
	    arv_fake_camera_get_control_channel_privilege (gv_fake_camera->priv->camera) != 0) {
		g_object_ref (remote_address);
		arv_info_device("[GvFakeCamera::handle_control_packet] New controller");
		gv_fake_camera->priv->controller_address = remote_address;
		gv_fake_camera->priv->controller_time = g_get_real_time ();
	}
	else if (gv_fake_camera->priv->controller_address != NULL &&
	    arv_fake_camera_get_control_channel_privilege (gv_fake_camera->priv->camera) == 0) {
		g_object_unref (gv_fake_camera->priv->controller_address);
		arv_info_device("[GvFakeCamera::handle_control_packet] Controller releases");
		gv_fake_camera->priv->controller_address = NULL;
		gv_fake_camera->priv->controller_time = g_get_real_time ();
	}

	return success;
}

/* Receive and handle the pending control packets, waiting at most @timeout_ms for them */

static void
_process_control_packets (ArvGvFakeCamera *gv_fake_camera, GInputVector *input_vector, gint timeout_ms)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	unsigned int i;

	if (g_poll (priv->socket_fds, priv->n_socket_fds, timeout_ms) <= 0)
		return;

	for (i = 0; i < ARV_GV_FAKE_CAMERA_N_INPUT_SOCKETS; i++) {
		GSocket *socket = priv->input_sockets[i];

		if (G_IS_SOCKET (socket)) {
			GSocketAddress *remote_address = NULL;
			int count;

			arv_gpollfd_clear_one (&priv->socket_fds[i], socket);

			/* Drain the socket, as resend requests may come in bursts */
			do {
				count = g_socket_receive_message (socket, &remote_address,
								  input_vector, 1, NULL, NULL,
								  NULL, NULL, NULL);
				if (count > 0) {
					if (_handle_control_packet (gv_fake_camera, socket,
								    remote_address, input_vector->buffer,
								    count))
						arv_debug_device ("[GvFakeCamera::thread] Control packet received");
				}
				g_clear_object (&remote_address);
			} while (count > 0);
		}
	}
}

static void
_stop_stream (ArvGvFakeCamera *gv_fake_camera)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	unsigned int i;

	if (priv->stream_address == NULL)
		return;

	g_clear_object (&priv->stream_address);
	for (i = 0; i < ARV_GV_FAKE_CAMERA_N_FRAME_HISTORY; i++) {
		g_clear_object (&priv->frames[i].buffer);
		priv->frames[i].n_packets = 0;
	}

	arv_info_stream_thread ("[GvFakeCamera::thread] Stop stream");
}

static void
_start_stream (ArvGvFakeCamera *gv_fake_camera)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	GInetAddress *inet_address;
	char *inet_address_string;
	unsigned int i;

	priv->stream_address = arv_fake_camera_get_stream_address (priv->camera);
	inet_address = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (priv->stream_address));
	inet_address_string = g_inet_address_to_string (inet_address);
	arv_info_stream_thread ("[GvFakeCamera::thread] Start stream to %s (%d)",
				inet_address_string,
				g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (priv->stream_address)));
	g_free (inet_address_string);

	priv->payload = arv_fake_camera_get_payload (priv->camera);
	for (i = 0; i < ARV_GV_FAKE_CAMERA_N_FRAME_HISTORY; i++) {
		priv->frames[i].buffer = arv_buffer_new (priv->payload, NULL);
		priv->frames[i].n_packets = 0;
	}
	priv->current_frame = 0;
}

static void
_send_frame (ArvGvFakeCamera *gv_fake_camera, GInputVector *input_vector)
{
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	ArvGvFakeCameraFrame *frame;
	guint64 packet_delay_ns;
	size_t block_size;
	guint32 packet_id;

	/* Use the oldest frame of the history */
	priv->current_frame = (priv->current_frame + 1) % ARV_GV_FAKE_CAMERA_N_FRAME_HISTORY;
	frame = &priv->frames[priv->current_frame];

	/* Make the frame unavailable for resend requests while it is filled */
	frame->n_packets = 0;

	arv_fake_camera_fill_buffer (priv->camera, frame->buffer, &frame->gv_packet_size);

	if (frame->gv_packet_size <= ARV_GVSP_PACKET_PROTOCOL_OVERHEAD (FALSE) ||
	    frame->gv_packet_size > ARV_GV_FAKE_CAMERA_BUFFER_SIZE) {
		arv_warning_stream_thread ("[GvFakeCamera::thread] Invalid packet size (%u)", frame->gv_packet_size);
		return;
	}

	block_size = frame->gv_packet_size - ARV_GVSP_PACKET_PROTOCOL_OVERHEAD (FALSE);
	/* Leader + payload + trailer */
	frame->n_packets = 2 + (priv->payload + block_size - 1) / block_size;

	arv_info_stream_thread ("[GvFakeCamera::thread] Send frame %" G_GUINT64_FORMAT " (%u packets)",
				frame->buffer->priv->frame_id, frame->n_packets);

	packet_delay_ns = _get_packet_delay_ns (gv_fake_camera);
	priv->next_packet_time_ns = g_get_monotonic_time () * 1000;

	for (packet_id = 0; packet_id < frame->n_packets; packet_id++) {
		_send_frame_packet (gv_fake_camera, frame, packet_id, packet_delay_ns, FALSE);

		/* Keep answering the control packets while sending large frames, for the resend requests and the
		 * heartbeat. This is also where the resend requests for the current frame are honored. */
		if (packet_id % ARV_GV_FAKE_CAMERA_CONTROL_CHECK_PERIOD == ARV_GV_FAKE_CAMERA_CONTROL_CHECK_PERIOD - 1)
			_process_control_packets (gv_fake_camera, input_vector, 0);

		if (g_atomic_int_get (&priv->cancel))
			break;
	}
}

static void *
_thread (void *user_data)
{
	ArvGvFakeCamera *gv_fake_camera = user_data;
	ArvGvFakeCameraPrivate *priv = gv_fake_camera->priv;
	GInputVector input_vector;
	gboolean is_streaming = FALSE;

	input_vector.buffer = g_malloc0 (ARV_GV_FAKE_CAMERA_BUFFER_SIZE);
	input_vector.size = ARV_GV_FAKE_CAMERA_BUFFER_SIZE;

	priv->packet_buffer = g_malloc (ARV_GV_FAKE_CAMERA_BUFFER_SIZE);

	do {
		guint64 next_timestamp_us;

		if (is_streaming) {
			arv_fake_camera_get_sleep_time_for_next_frame (priv->camera, &next_timestamp_us);
		} else {
			next_timestamp_us = g_get_real_time () + 100000;
		}

		do {
			gint timeout_ms;

			timeout_ms =  (next_timestamp_us - g_get_real_time ()) / 1000LL;
			if (timeout_ms < 0)
				timeout_ms = 0;
			else if (timeout_ms > 100)
				timeout_ms = 100;

			_process_control_packets (gv_fake_camera, &input_vector, timeout_ms);

			if (arv_fake_camera_get_control_channel_privilege (priv->camera) == 0 ||
			    arv_fake_camera_get_acquisition_status (priv->camera) == 0) {
				_stop_stream (gv_fake_camera);
				is_streaming = FALSE;
			}
		} while (!g_atomic_int_get (&priv->cancel) && g_get_real_time () < next_timestamp_us);

		if (arv_fake_camera_get_control_channel_privilege (priv->camera) != 0 &&
		    arv_fake_camera_get_acquisition_status (priv->camera) != 0) {
			if (priv->stream_address == NULL)
				_start_stream (gv_fake_camera);

			if (arv_fake_camera_is_in_free_running_mode (priv->camera) ||
			    (arv_fake_camera_is_in_software_trigger_mode (priv->camera) &&
			     arv_fake_camera_check_and_acknowledge_software_trigger (priv->camera))) {
				_send_frame (gv_fake_camera, &input_vector);

				is_streaming = TRUE;
			}
		}

	} while (!g_atomic_int_get (&priv->cancel));

	_stop_stream (gv_fake_camera);

	arv_info_device ("[GvFakeCamera::thread] Resend requests = %" G_GUINT64_FORMAT
			 ", resent packets = %" G_GUINT64_FORMAT
			 ", unavailable packets = %" G_GUINT64_FORMAT
			 ", send failures = %" G_GUINT64_FORMAT,
			 priv->n_resend_requests, priv->n_resent_packets,
			 priv->n_unavailable_packets, priv->n_send_failures);

	g_clear_pointer (&priv->packet_buffer, g_free);
	g_free (input_vector.buffer);

	return NULL;
}

static gboolean
_create_and_bind_input_socket (GSocket **socket_out, const char *socket_name,
			       GInetAddress *inet_address, unsigned int port,
			       gboolean allow_reuse, gboolean blocking)
{
	GSocket *socket;
	GSocketAddress *socket_address;
	GError *error = NULL;
	gboolean success;
	char *address_string;

	address_string = g_inet_address_to_string (inet_address);
	if (port > 0)
		arv_info_device ("%s address = %s:%d",socket_name,  address_string, port);
	else
		arv_info_device ("%s address = %s",socket_name,  address_string);
	g_clear_pointer (&address_string, g_free);

	socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
	if (!G_IS_SOCKET (socket)) {
		*socket_out = NULL;
		return FALSE;
	}

	socket_address = arv_socket_bind_with_range (socket, inet_address, port, allow_reuse, &error);
        success = G_IS_INET_SOCKET_ADDRESS (socket_address);

	if (error != NULL) {
		arv_warning_device ("Failed to bind %s socket: %s", socket_name, error->message);
		g_clear_error (&error);
	}

	g_clear_object (&socket_address);

	if (success)
		g_socket_set_blocking (socket, blocking);
	else
		g_clear_object (&socket);

	*socket_out = socket;

	return G_IS_SOCKET (socket);
}

static gboolean
arv_gv_fake_camera_start (ArvGvFakeCamera *gv_fake_camera)
{
	ArvNetworkInterface *iface;
	GSocketAddress *socket_address;
	GInetAddress *inet_address;
	GInetAddress *gvcp_inet_address;
	unsigned int i;
	unsigned int n_socket_fds;

	g_return_val_if_fail (ARV_IS_GV_FAKE_CAMERA (gv_fake_camera), FALSE);

	/* get the interface by name or address */
	iface = arv_network_get_interface_by_address(gv_fake_camera->priv->interface_name);
	if (iface == NULL) iface = arv_network_get_interface_by_name(gv_fake_camera->priv->interface_name);
	#ifdef G_OS_WIN32
		/* fake the default loopback interface in windows, in case not found */
		if(iface == NULL && g_strcmp0 (gv_fake_camera->priv->interface_name,ARV_GV_FAKE_CAMERA_DEFAULT_INTERFACE) == 0)
			iface = arv_network_get_fake_ipv4_loopback();
	#endif
	if (iface == NULL) {
		arv_warning_device ("[GvFakeCamera::start] No network interface with address or name '%s' found.",gv_fake_camera->priv->interface_name);
		return FALSE;
	}

	socket_address = g_socket_address_new_from_native (arv_network_interface_get_addr(iface),
								sizeof (struct sockaddr));
	gvcp_inet_address = g_object_ref (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (socket_address)));
	arv_fake_camera_set_inet_address (gv_fake_camera->priv->camera, gvcp_inet_address);

	_create_and_bind_input_socket (&gv_fake_camera->priv->gvsp_socket,
								 "GVSP", gvcp_inet_address, 0, FALSE, TRUE);
	if (G_IS_SOCKET (gv_fake_camera->priv->gvsp_socket)) {
		GError *error = NULL;
		int buffer_size = 0;

		/* A large send buffer avoids the send failures when the frames are sent in bursts. The actual size
		 * is limited by net.core.wmem_max. */
		if (!g_socket_set_option (gv_fake_camera->priv->gvsp_socket, SOL_SOCKET, SO_SNDBUF,
					  ARV_GV_FAKE_CAMERA_GVSP_SOCKET_BUFFER_SIZE, &error)) {
			arv_warning_device ("[GvFakeCamera::start] Failed to set GVSP socket buffer size: %s",
					    error->message);
			g_clear_error (&error);
		}
		if (g_socket_get_option (gv_fake_camera->priv->gvsp_socket, SOL_SOCKET, SO_SNDBUF,
					 &buffer_size, NULL))
			arv_info_device ("[GvFakeCamera::start] GVSP socket buffer size = %d", buffer_size);
	}
	_create_and_bind_input_socket
		(&gv_fake_camera->priv->input_sockets[ARV_GV_FAKE_CAMERA_INPUT_SOCKET_GVCP],
		 "GVCP", gvcp_inet_address, ARV_GVCP_PORT, FALSE, FALSE);

	inet_address = g_inet_address_new_from_string ("255.255.255.255");
	if (!g_inet_address_equal (gvcp_inet_address, inet_address))
		_create_and_bind_input_socket
			(&gv_fake_camera->priv->input_sockets[ARV_GV_FAKE_CAMERA_INPUT_SOCKET_GLOBAL_DISCOVERY],
			 "Global discovery", inet_address, ARV_GVCP_PORT, TRUE, FALSE);
	g_clear_object (&inet_address);
	g_clear_object (&socket_address);

	socket_address = g_socket_address_new_from_native (arv_network_interface_get_broadaddr(iface),
								sizeof (struct sockaddr));
	inet_address = g_object_ref (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (socket_address)));
	if (!g_inet_address_equal (gvcp_inet_address, inet_address))
		_create_and_bind_input_socket
			(&gv_fake_camera->priv->input_sockets[ARV_GV_FAKE_CAMERA_INPUT_SOCKET_SUBNET_DISCOVERY],
			 "Subnet discovery", inet_address, ARV_GVCP_PORT, FALSE, FALSE);
	g_clear_object (&inet_address);
	g_clear_object (&socket_address);

	g_clear_object (&gvcp_inet_address);

	arv_network_interface_free(iface);

	n_socket_fds = 0;
	for (i = 0; i < ARV_GV_FAKE_CAMERA_N_INPUT_SOCKETS; i++) {
		GSocket *socket = gv_fake_camera->priv->input_sockets[i];

		if (G_IS_SOCKET (socket)) {
			gv_fake_camera->priv->socket_fds[n_socket_fds].fd = g_socket_get_fd (socket);
			gv_fake_camera->priv->socket_fds[n_socket_fds].events = G_IO_IN;
			gv_fake_camera->priv->socket_fds[n_socket_fds].revents = 0;

			n_socket_fds++;
		}
	}
	arv_info_device ("Listening to %d sockets", n_socket_fds);

	gv_fake_camera->priv->n_socket_fds = n_socket_fds;

	arv_gpollfd_prepare_all (gv_fake_camera->priv->socket_fds, n_socket_fds);

	gv_fake_camera->priv->cancel = FALSE;
	gv_fake_camera->priv->thread = g_thread_new ("arv_fake_gv_fake_camera", _thread, gv_fake_camera);

	return TRUE;
}


static void
arv_gv_fake_camera_stop (ArvGvFakeCamera *gv_fake_camera)
{
	unsigned int i;

	g_return_if_fail (ARV_IS_GV_FAKE_CAMERA (gv_fake_camera));

	if (gv_fake_camera->priv->thread != NULL) {

		g_atomic_int_set (&gv_fake_camera->priv->cancel, TRUE);
		g_thread_join (gv_fake_camera->priv->thread);
		gv_fake_camera->priv->thread = NULL;
	}

	arv_gpollfd_finish_all (gv_fake_camera->priv->socket_fds, gv_fake_camera->priv->n_socket_fds);

	for (i = 0; i < ARV_GV_FAKE_CAMERA_N_INPUT_SOCKETS; i++) {
		g_clear_object (&gv_fake_camera->priv->input_sockets[i]);
	}
	g_clear_object (&gv_fake_camera->priv->gvsp_socket);

	g_clear_object (&gv_fake_camera->priv->controller_address);
}

/**
 * arv_gv_fake_camera_new_full:
 * @interface_name: (nullable): listening network interface, by name or IP address, default is 127.0.0.1
 * @serial_number: (nullable): fake device serial number, default is GV01
 * @genicam_filename: (nullable): path to alternative genicam data
 *
 * Returns: a new #ArvGvFakeCamera
 *
 * Since: 0.8.0
 */

ArvGvFakeCamera *
arv_gv_fake_camera_new_full (const char *interface_name, const char *serial_number, const char *genicam_filename)
{
	return g_object_new (ARV_TYPE_GV_FAKE_CAMERA,
			     "interface-name", interface_name != NULL ?
			     interface_name : ARV_GV_FAKE_CAMERA_DEFAULT_INTERFACE,
			     "serial-number", serial_number != NULL ?
			     serial_number : ARV_GV_FAKE_CAMERA_DEFAULT_SERIAL_NUMBER,
			     "genicam-filename", genicam_filename,
			     NULL);
}

/**
 * arv_gv_fake_camera_new:
 * @interface_name: (nullable): listening network interface ('lo' by default)
 * @serial_number: (nullable): fake device serial number ('GV01' by default)
 *
 * Returns: a new #ArvGvFakeCamera
 *
 * Since: 0.8.0
 */

ArvGvFakeCamera *
arv_gv_fake_camera_new (const char *interface_name, const char *serial_number)
{
	return arv_gv_fake_camera_new_full (interface_name, serial_number, NULL);
}

static void
_update_gvcp_capability (ArvGvFakeCamera *gv_fake_camera)
{
	guint32 capability = 0;

	if (gv_fake_camera->priv->camera == NULL)
		return;

	arv_fake_camera_read_register (gv_fake_camera->priv->camera, ARV_GVBS_GVCP_CAPABILITY_OFFSET, &capability);
	if (gv_fake_camera->priv->packet_resend)
		capability |= ARV_GVBS_GVCP_CAPABILITY_PACKET_RESEND;
	else
		capability &= ~ARV_GVBS_GVCP_CAPABILITY_PACKET_RESEND;
	arv_fake_camera_write_register (gv_fake_camera->priv->camera, ARV_GVBS_GVCP_CAPABILITY_OFFSET, capability);
}

static void
_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	ArvGvFakeCamera *gv_fake_camera = ARV_GV_FAKE_CAMERA (object);

	switch (prop_id)
	{
		case PROP_INTERFACE_NAME:
			g_free (gv_fake_camera->priv->interface_name);
			gv_fake_camera->priv->interface_name = g_value_dup_string (value);
			break;
		case PROP_SERIAL_NUMBER:
			g_free (gv_fake_camera->priv->serial_number);
			gv_fake_camera->priv->serial_number = g_value_dup_string (value);
			break;
		case PROP_GENICAM_FILENAME:
			g_free (gv_fake_camera->priv->genicam_filename);
			gv_fake_camera->priv->genicam_filename = g_value_dup_string (value);
			break;
		case PROP_GVSP_LOST_PACKET_RATIO:
			gv_fake_camera->priv->gvsp_lost_packet_ratio = g_value_get_double (value);
			break;
		case PROP_PACKET_RESEND:
			gv_fake_camera->priv->packet_resend = g_value_get_boolean (value);
			_update_gvcp_capability (gv_fake_camera);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

/**
 * arv_gv_fake_camera_get_fake_camera:
 * @gv_fake_camera: a #ArvGvFakeCamera
 *
 * Retrieves the underlying #ArvFakeCamera object owned by @gv_fake_camera.
 *
 * Returns: (transfer none): underlying fake camera object.
 *
 * Since: 0.8.0
 */

ArvFakeCamera *
arv_gv_fake_camera_get_fake_camera (ArvGvFakeCamera *gv_fake_camera)
{
	g_return_val_if_fail (ARV_IS_GV_FAKE_CAMERA (gv_fake_camera), NULL);

        return gv_fake_camera->priv->camera;
}

/**
 * arv_gv_fake_camera_is_running:
 * @gv_fake_camera: a #ArvGvFakeCamera
 *
 * Returns: %TRUE if the fake camera is correctly listening on the GVCP port
 *
 * Since: 0.8.0
 */

gboolean
arv_gv_fake_camera_is_running (ArvGvFakeCamera *gv_fake_camera)
{
	g_return_val_if_fail (ARV_IS_GV_FAKE_CAMERA (gv_fake_camera), FALSE);

	return gv_fake_camera->priv->is_running;
}

static void
arv_gv_fake_camera_init (ArvGvFakeCamera *gv_fake_camera)
{
	gv_fake_camera->priv = arv_gv_fake_camera_get_instance_private (gv_fake_camera);
}

static void
_constructed (GObject *gobject)
{
	ArvGvFakeCamera *gv_fake_camera = ARV_GV_FAKE_CAMERA (gobject);

	G_OBJECT_CLASS (arv_gv_fake_camera_parent_class)->constructed (gobject);

	gv_fake_camera->priv->camera = arv_fake_camera_new_full (gv_fake_camera->priv->serial_number, gv_fake_camera->priv->genicam_filename);
	_update_gvcp_capability (gv_fake_camera);
	gv_fake_camera->priv->is_running = arv_gv_fake_camera_start (gv_fake_camera);
}

static void
_finalize (GObject *object)
{
	ArvGvFakeCamera *gv_fake_camera = ARV_GV_FAKE_CAMERA (object);

	gv_fake_camera->priv->is_running = FALSE;

	arv_gv_fake_camera_stop (gv_fake_camera);

	g_object_unref (gv_fake_camera->priv->camera);

	g_clear_pointer (&gv_fake_camera->priv->interface_name, g_free);
	g_clear_pointer (&gv_fake_camera->priv->serial_number, g_free);
	g_clear_pointer (&gv_fake_camera->priv->genicam_filename, g_free);

	G_OBJECT_CLASS (arv_gv_fake_camera_parent_class)->finalize (object);
}

static void
arv_gv_fake_camera_class_init (ArvGvFakeCameraClass *this_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (this_class);

	object_class->set_property = _set_property;
	object_class->constructed = _constructed;
	object_class->finalize = _finalize;

	g_object_class_install_property (object_class,
					 PROP_INTERFACE_NAME,
					 g_param_spec_string ("interface-name",
							      "Interface name",
							      "Interface name",
							      ARV_GV_FAKE_CAMERA_DEFAULT_INTERFACE,
							      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
							      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK |
							      G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_SERIAL_NUMBER,
					 g_param_spec_string ("serial-number",
							      "Serial number",
							      "Serial number",
							      ARV_GV_FAKE_CAMERA_DEFAULT_SERIAL_NUMBER,
							      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
							      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK |
							      G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_GENICAM_FILENAME,
					 g_param_spec_string ("genicam-filename",
							      "Genicam filename",
							      "Genicam filename",
							      NULL,
							      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
							      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK |
							      G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_GVSP_LOST_PACKET_RATIO,
					 g_param_spec_double ("gvsp-lost-ratio",
							      "GVSP lost packet ratio",
							      "GVSP lost packet ratio",
							      0.0, 1.0, 0.0,
							      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT |
							      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK |
							      G_PARAM_STATIC_BLURB));
	/**
	 * ArvGvFakeCamera:packet-resend:
	 *
	 * Advertise and honor the GVCP packet resend requests. The last frames are kept in memory for this purpose.
	 *
	 * Since: 0.10.0
	 */
	g_object_class_install_property (object_class,
					 PROP_PACKET_RESEND,
					 g_param_spec_boolean ("packet-resend",
							       "Packet resend",
							       "Packet resend support",
							       TRUE,
							       G_PARAM_WRITABLE | G_PARAM_CONSTRUCT |
							       G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK |
							       G_PARAM_STATIC_BLURB));
}
