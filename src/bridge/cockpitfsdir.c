/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitfsdir.h"
#include "cockpitfswatch.h"

#include "common/cockpitjson.h"

#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

/**
 * CockpitFsdir:
 *
 * A #CockpitChannel that watches a directory.
 *
 * The payload type for this channel is 'fsdir1'.
 */

#define COCKPIT_FSDIR(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_FSDIR, CockpitFsdir))

typedef struct {
  CockpitChannel parent;
  const gchar *path;
  GFileMonitor *monitor;
  guint sig_changed;
  GCancellable *cancellable;
} CockpitFsdir;

typedef struct {
  CockpitChannelClass parent_class;
} CockpitFsdirClass;

G_DEFINE_TYPE (CockpitFsdir, cockpit_fsdir, COCKPIT_TYPE_CHANNEL);

static void
cockpit_fsdir_recv (CockpitChannel *channel,
                    GBytes *message)
{
  g_warning ("received unexpected message in fsdir channel");
  cockpit_channel_close (channel, "protocol-error");
}

static void
cockpit_fsdir_init (CockpitFsdir *self)
{
}

static void
on_files_listed (GObject *source_object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  GError *error = NULL;
  GList *files;

  files = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (source_object), res, &error);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          CockpitFsdir *self = COCKPIT_FSDIR (user_data);
          g_message ("%s: %s", COCKPIT_FSDIR(user_data)->path, error->message);
          cockpit_channel_close_option (COCKPIT_CHANNEL (self), "message", error->message);
          cockpit_channel_close (COCKPIT_CHANNEL (self), "internal-error");
        }
      g_clear_error (&error);
      return;
    }

  CockpitFsdir *self = COCKPIT_FSDIR (user_data);

  if (files == NULL)
    {
      JsonObject *msg;
      GBytes *msg_bytes;

      msg = json_object_new ();
      json_object_set_string_member (msg, "event", "present-done");
      msg_bytes = cockpit_json_write_bytes (msg);
      json_object_unref (msg);
      cockpit_channel_send (COCKPIT_CHANNEL(self), msg_bytes, FALSE);
      g_bytes_unref (msg_bytes);

      g_clear_object (&self->cancellable);
      g_object_unref (source_object);
      return;
    }

  for (GList *l = files; l; l = l->next)
    {
      GFileInfo *info = G_FILE_INFO (l->data);
      JsonObject *msg;
      GBytes *msg_bytes;

      msg = json_object_new ();
      json_object_set_string_member (msg, "event", "present");
      json_object_set_string_member
        (msg, "path", g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_STANDARD_NAME));
      msg_bytes = cockpit_json_write_bytes (msg);
      json_object_unref (msg);
      cockpit_channel_send (COCKPIT_CHANNEL(self), msg_bytes, FALSE);
      g_bytes_unref (msg_bytes);
    }

  g_list_free_full (files, g_object_unref);

  g_file_enumerator_next_files_async (G_FILE_ENUMERATOR (source_object),
                                      10,
                                      G_PRIORITY_DEFAULT,
                                      self->cancellable,
                                      on_files_listed,
                                      self);
}

static void
on_enumerator_ready (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
  GError *error = NULL;
  GFileEnumerator *enumerator;

  enumerator = g_file_enumerate_children_finish (G_FILE (source_object), res, &error);
  if (enumerator == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          CockpitFsdir *self = COCKPIT_FSDIR (user_data);
          g_message ("%s: %s", self->path, error->message);
          cockpit_channel_close_option (COCKPIT_CHANNEL (self), "message", error->message);
          cockpit_channel_close (COCKPIT_CHANNEL (self), "internal-error");
        }
      g_clear_error (&error);
      return;
    }

  CockpitFsdir *self = COCKPIT_FSDIR (user_data);

  g_file_enumerator_next_files_async (enumerator,
                                      10,
                                      G_PRIORITY_DEFAULT,
                                      self->cancellable,
                                      on_files_listed,
                                      self);
}

static void
on_changed (GFileMonitor      *monitor,
            GFile             *file,
            GFile             *other_file,
            GFileMonitorEvent  event_type,
            gpointer           user_data)
{
  CockpitFsdir *self = COCKPIT_FSDIR(user_data);
  cockpit_fswatch_emit_event (COCKPIT_CHANNEL(self), file, other_file, event_type);
}

static void
cockpit_fsdir_prepare (CockpitChannel *channel)
{
  CockpitFsdir *self = COCKPIT_FSDIR (channel);
  GError *error;

  COCKPIT_CHANNEL_CLASS (cockpit_fsdir_parent_class)->prepare (channel);

  self->path = cockpit_channel_get_option (channel, "path");
  if (self->path == NULL || *(self->path) == 0)
    {
      g_warning ("missing 'path' option for fsdir channel");
      cockpit_channel_close (channel, "protocol-error");
      return;
    }

  self->cancellable = g_cancellable_new ();

  GFile *file = g_file_new_for_path (self->path);
  g_file_enumerate_children_async (file,
                                   G_FILE_ATTRIBUTE_STANDARD_NAME,
                                   G_FILE_QUERY_INFO_NONE,
                                   G_PRIORITY_DEFAULT,
                                   self->cancellable,
                                   on_enumerator_ready,
                                   self);
  self->monitor = g_file_monitor_directory (file, 0, NULL, &error);
  g_object_unref (file);

  if (self->monitor == NULL)
    {
      g_message ("%s: %s", self->path, error->message);
      cockpit_channel_close_option (channel, "message", error->message);
      cockpit_channel_close (channel, "internal-error");
      return;
    }

  self->sig_changed = g_signal_connect (self->monitor, "changed", G_CALLBACK (on_changed), self);

  cockpit_channel_ready (channel);
}

static void
cockpit_fsdir_dispose (GObject *object)
{
  CockpitFsdir *self = COCKPIT_FSDIR (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  if (self->monitor)
    {
      if (self->sig_changed)
        g_signal_handler_disconnect (self->monitor, self->sig_changed);
      self->sig_changed = 0;

      // HACK - It is not generally safe to just unref a GFileMonitor.
      // Some events might be on their way to the main loop from its
      // worker thread and if they arrive after the GFileMonitor has
      // been destroyed, bad things will happen.
      //
      // As a workaround, we cancel the monitor and then spin the main
      // loop a bit until nothing is pending anymore.
      //
      // https://bugzilla.gnome.org/show_bug.cgi?id=740491

      g_file_monitor_cancel (self->monitor);
      for (int tries = 0; tries < 10; tries ++)
        {
          if (!g_main_context_iteration (NULL, FALSE))
            break;
        }
    }

  G_OBJECT_CLASS (cockpit_fsdir_parent_class)->dispose (object);
}

static void
cockpit_fsdir_finalize (GObject *object)
{
  CockpitFsdir *self = COCKPIT_FSDIR (object);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (cockpit_fsdir_parent_class)->finalize (object);
}

static void
cockpit_fsdir_class_init (CockpitFsdirClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);

  gobject_class->dispose = cockpit_fsdir_dispose;
  gobject_class->finalize = cockpit_fsdir_finalize;

  channel_class->prepare = cockpit_fsdir_prepare;
  channel_class->recv = cockpit_fsdir_recv;
}

/**
 * cockpit_fsdir_open:
 * @transport: the transport to send/receive messages on
 * @channel_id: the channel id
 * @path: the path name of the file to read
 *
 * This function is mainly used by tests. The usual way
 * to get a #CockpitFsdir is via cockpit_channel_open()
 *
 * Returns: (transfer full): the new channel
 */
CockpitChannel *
cockpit_fsdir_open (CockpitTransport *transport,
                    const gchar *channel_id,
                    const gchar *path)
{
  CockpitChannel *channel;
  JsonObject *options;

  g_return_val_if_fail (channel_id != NULL, NULL);

  options = json_object_new ();
  json_object_set_string_member (options, "path", path);
  json_object_set_string_member (options, "payload", "fsdir1");

  channel = g_object_new (COCKPIT_TYPE_FSDIR,
                          "transport", transport,
                          "id", channel_id,
                          "options", options,
                          NULL);

  json_object_unref (options);
  return channel;
}