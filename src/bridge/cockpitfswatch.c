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

#include "cockpitfswatch.h"
#include "cockpitfsread.h"

#include "common/cockpitjson.h"

#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

/**
 * CockpitFswatch:
 *
 * A #CockpitChannel that watches a file or directory.
 *
 * The payload type for this channel is 'fswatch1'.
 */

#define COCKPIT_FSWATCH(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_FSWATCH, CockpitFswatch))

typedef struct {
  CockpitChannel parent;
  const gchar *path;
  GFileMonitor *monitor;
  guint sig_changed;
} CockpitFswatch;

typedef struct {
  CockpitChannelClass parent_class;
} CockpitFswatchClass;

G_DEFINE_TYPE (CockpitFswatch, cockpit_fswatch, COCKPIT_TYPE_CHANNEL);

static void
cockpit_fswatch_recv (CockpitChannel *channel,
                      GBytes *message)
{
  g_warning ("received unexpected message in fswatch channel");
  cockpit_channel_close (channel, "protocol-error");
}

static void
cockpit_fswatch_init (CockpitFswatch *self)
{
}

static gchar *
event_type_to_string (GFileMonitorEvent event_type)
{
  switch (event_type) {
  case G_FILE_MONITOR_EVENT_CHANGED:
    return "changed";
  case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    return "done-hint";
  case G_FILE_MONITOR_EVENT_DELETED:
    return "deleted";
  case G_FILE_MONITOR_EVENT_CREATED:
    return "created";
  case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    return "attribute-changed";
  case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    return "pre-unmount";
  case G_FILE_MONITOR_EVENT_UNMOUNTED:
    return "unmounted";
  case G_FILE_MONITOR_EVENT_MOVED:
    return "moved";
  default:
    return "unknown";
  }
}

void
cockpit_fswatch_emit_event (CockpitChannel    *channel,
                            GFile             *file,
                            GFile             *other_file,
                            GFileMonitorEvent  event_type)
{
  JsonObject *msg;
  GBytes *msg_bytes;

  msg = json_object_new ();
  json_object_set_string_member (msg, "event", event_type_to_string (event_type));
  if (file)
    {
      char *p = g_file_get_path (file);
      char *t = cockpit_get_file_tag (p);
      json_object_set_string_member (msg, "path", p);
      json_object_set_string_member (msg, "tag", t);
      g_free (p);
      g_free (t);
    }
  if (other_file)
    {
      char *p = g_file_get_path (other_file);
      json_object_set_string_member (msg, "other", p);
      g_free (p);
    }
  msg_bytes = cockpit_json_write_bytes (msg);
  json_object_unref (msg);
  cockpit_channel_send (channel, msg_bytes, TRUE);
  g_bytes_unref (msg_bytes);
}

static void
on_changed (GFileMonitor      *monitor,
            GFile             *file,
            GFile             *other_file,
            GFileMonitorEvent  event_type,
            gpointer           user_data)
{
  CockpitFswatch *self = COCKPIT_FSWATCH (user_data);
  cockpit_fswatch_emit_event (COCKPIT_CHANNEL(self), file, other_file, event_type);
}

static void
cockpit_fswatch_prepare (CockpitChannel *channel)
{
  CockpitFswatch *self = COCKPIT_FSWATCH (channel);
  GError *error = NULL;
  const gchar *path;

  COCKPIT_CHANNEL_CLASS (cockpit_fswatch_parent_class)->prepare (channel);

  path = cockpit_channel_get_option (channel, "path");
  if (path == NULL || *path == 0)
    {
      g_warning ("missing 'path' option for fswatch channel");
      cockpit_channel_close (channel, "protocol-error");
      return;
    }

  GFile *file = g_file_new_for_path (path);
  GFileMonitor *monitor = g_file_monitor (file, 0, NULL, &error);
  g_object_unref (file);

  if (monitor == NULL)
    {
      g_message ("%s: %s", self->path, error->message);
      cockpit_channel_close_option (channel, "message", error->message);
      cockpit_channel_close (channel, "internal-error");
      return;
    }

  self->monitor = monitor;
  self->sig_changed = g_signal_connect (self->monitor, "changed", G_CALLBACK (on_changed), self);

  cockpit_channel_ready (channel);
}

static void
cockpit_fswatch_dispose (GObject *object)
{
  CockpitFswatch *self = COCKPIT_FSWATCH (object);

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

  G_OBJECT_CLASS (cockpit_fswatch_parent_class)->dispose (object);
}

static void
cockpit_fswatch_finalize (GObject *object)
{
  CockpitFswatch *self = COCKPIT_FSWATCH (object);

  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (cockpit_fswatch_parent_class)->finalize (object);
}

static void
cockpit_fswatch_class_init (CockpitFswatchClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);

  gobject_class->dispose = cockpit_fswatch_dispose;
  gobject_class->finalize = cockpit_fswatch_finalize;

  channel_class->prepare = cockpit_fswatch_prepare;
  channel_class->recv = cockpit_fswatch_recv;
}

/**
 * cockpit_fswatch_open:
 * @transport: the transport to send/receive messages on
 * @channel_id: the channel id
 * @path: the path name of the file to read
 *
 * This function is mainly used by tests. The usual way
 * to get a #CockpitFswatch is via cockpit_channel_open()
 *
 * Returns: (transfer full): the new channel
 */
CockpitChannel *
cockpit_fswatch_open (CockpitTransport *transport,
                      const gchar *channel_id,
                      const gchar *path)
{
  CockpitChannel *channel;
  JsonObject *options;

  g_return_val_if_fail (channel_id != NULL, NULL);

  options = json_object_new ();
  json_object_set_string_member (options, "path", path);
  json_object_set_string_member (options, "payload", "fswatch1");

  channel = g_object_new (COCKPIT_TYPE_FSWATCH,
                          "transport", transport,
                          "id", channel_id,
                          "options", options,
                          NULL);

  json_object_unref (options);
  return channel;
}