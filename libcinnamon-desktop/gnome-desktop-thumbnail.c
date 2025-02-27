/*
 * gnome-thumbnail.c: Utilities for handling thumbnails
 *
 * Copyright (C) 2002 Red Hat, Inc.
 * Copyright (C) 2010 Carlos Garcia Campos <carlosgc@gnome.org>
 *
 * This file is part of the Gnome Library.
 *
 * The Gnome Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * The Gnome Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <glib.h>
#include <stdio.h>
#include <errno.h>

#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include "gnome-desktop-thumbnail.h"
#include "gnome-desktop-utils.h"
#include <glib/gstdio.h>

#define SECONDS_BETWEEN_STATS 10

struct _GnomeDesktopThumbnailFactoryPrivate {
  GnomeDesktopThumbnailSize size;

  GMutex lock;

  GList *thumbnailers;
  GHashTable *mime_types_map;
  GList *monitors;

  GSettings *settings;
  gboolean loaded : 1;
  gboolean disabled : 1;
  gchar **disabled_types;

  gboolean permissions_problem;
  gboolean needs_chown;
  uid_t real_uid;
  gid_t real_gid;
};

static const char *appname = "gnome-thumbnail-factory";

static void gnome_desktop_thumbnail_factory_init          (GnomeDesktopThumbnailFactory      *factory);
static void gnome_desktop_thumbnail_factory_class_init    (GnomeDesktopThumbnailFactoryClass *class);

G_DEFINE_TYPE_WITH_PRIVATE (GnomeDesktopThumbnailFactory, gnome_desktop_thumbnail_factory, G_TYPE_OBJECT)
#define parent_class gnome_desktop_thumbnail_factory_parent_class

typedef struct {
    gint width;
    gint height;
    gint input_width;
    gint input_height;
    gboolean preserve_aspect_ratio;
} SizePrepareContext;

#define LOAD_BUFFER_SIZE 4096

#define THUMBNAILER_ENTRY_GROUP "Thumbnailer Entry"
#define THUMBNAILER_EXTENSION   ".thumbnailer"

typedef struct {
    volatile gint ref_count;

    gchar *path;

    gchar  *try_exec;
    gchar  *command;
    gchar **mime_types;
} Thumbnailer;

static Thumbnailer *
thumbnailer_ref (Thumbnailer *thumb)
{
  g_return_val_if_fail (thumb != NULL, NULL);
  g_return_val_if_fail (thumb->ref_count > 0, NULL);

  g_atomic_int_inc (&thumb->ref_count);
  return thumb;
}

static void
thumbnailer_unref (Thumbnailer *thumb)
{
  g_return_if_fail (thumb != NULL);
  g_return_if_fail (thumb->ref_count > 0);

  if (g_atomic_int_dec_and_test (&thumb->ref_count))
    {
      g_free (thumb->path);
      g_free (thumb->try_exec);
      g_free (thumb->command);
      g_strfreev (thumb->mime_types);

      g_slice_free (Thumbnailer, thumb);
    }
}

static Thumbnailer *
thumbnailer_load (Thumbnailer *thumb)
{
  GKeyFile *key_file;
  GError *error = NULL;

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file, thumb->path, 0, &error))
    {
      g_warning ("Failed to load thumbnailer from \"%s\": %s\n", thumb->path, error->message);
      g_error_free (error);
      thumbnailer_unref (thumb);
      g_key_file_free (key_file);

      return NULL;
    }

  if (!g_key_file_has_group (key_file, THUMBNAILER_ENTRY_GROUP))
    {
      g_warning ("Invalid thumbnailer: missing group \"%s\"\n", THUMBNAILER_ENTRY_GROUP);
      thumbnailer_unref (thumb);
      g_key_file_free (key_file);

      return NULL;
    }

  thumb->command = g_key_file_get_string (key_file, THUMBNAILER_ENTRY_GROUP, "Exec", NULL);
  if (!thumb->command)
    {
      g_warning ("Invalid thumbnailer: missing Exec key\n");
      thumbnailer_unref (thumb);
      g_key_file_free (key_file);

      return NULL;
    }

  thumb->mime_types = g_key_file_get_string_list (key_file, THUMBNAILER_ENTRY_GROUP, "MimeType", NULL, NULL);
  if (!thumb->mime_types)
    {
      g_warning ("Invalid thumbnailer: missing MimeType key\n");
      thumbnailer_unref (thumb);
      g_key_file_free (key_file);

      return NULL;
    }

  thumb->try_exec = g_key_file_get_string (key_file, THUMBNAILER_ENTRY_GROUP, "TryExec", NULL);
  if (thumb->try_exec != NULL)
    {
      gchar *path_to_exec = g_find_program_in_path (thumb->try_exec);

      if (path_to_exec == NULL)
        {
          g_message ("Ignoring thumbnailer with missing binary: '%s'", thumb->try_exec);
          thumbnailer_unref (thumb);
          g_key_file_free (key_file);
          return NULL;
        }

      g_free (path_to_exec);
    }

  g_key_file_free (key_file);

  return thumb;
}

static Thumbnailer *
thumbnailer_reload (Thumbnailer *thumb)
{
  g_return_val_if_fail (thumb != NULL, NULL);

  g_free (thumb->command);
  thumb->command = NULL;
  g_strfreev (thumb->mime_types);
  thumb->mime_types = NULL;
  g_free (thumb->try_exec);
  thumb->try_exec = NULL;

  return thumbnailer_load (thumb);
}

static Thumbnailer *
thumbnailer_new (const gchar *path)
{
  Thumbnailer *thumb;

  thumb = g_slice_new0 (Thumbnailer);
  thumb->ref_count = 1;
  thumb->path = g_strdup (path);

  return thumbnailer_load (thumb);
}

static gpointer
init_thumbnailers_dirs (gpointer data)
{
  const gchar * const *data_dirs;
  gchar **thumbs_dirs;
  guint i, length;

  data_dirs = g_get_system_data_dirs ();
  length = g_strv_length ((char **) data_dirs);

  thumbs_dirs = g_new (gchar *, length + 2);
  thumbs_dirs[0] = g_build_filename (g_get_user_data_dir (), "thumbnailers", NULL);
  for (i = 0; i < length; i++)
    thumbs_dirs[i + 1] = g_build_filename (data_dirs[i], "thumbnailers", NULL);
  thumbs_dirs[length + 1] = NULL;

  return thumbs_dirs;
}

static const gchar * const *
get_thumbnailers_dirs (void)
{
  static GOnce once_init = G_ONCE_INIT;
  return g_once (&once_init, init_thumbnailers_dirs, NULL);
}

static void
size_prepared_cb (GdkPixbufLoader *loader, 
		  int              width,
		  int              height,
		  gpointer         data)
{
  SizePrepareContext *info = data;
  
  g_return_if_fail (width > 0 && height > 0);
  
  info->input_width = width;
  info->input_height = height;
  
  if (width < info->width && height < info->height) return;
  
  if (info->preserve_aspect_ratio && 
      (info->width > 0 || info->height > 0)) {
    if (info->width < 0)
      {
	width = width * (double)info->height/(double)height;
	height = info->height;
      }
    else if (info->height < 0)
      {
	height = height * (double)info->width/(double)width;
	width = info->width;
      }
    else if ((double)height * (double)info->width >
	     (double)width * (double)info->height) {
      width = 0.5 + (double)width * (double)info->height / (double)height;
      height = info->height;
    } else {
      height = 0.5 + (double)height * (double)info->width / (double)width;
      width = info->width;
    }
  } else {
    if (info->width > 0)
      width = info->width;
    if (info->height > 0)
      height = info->height;
  }
  
  gdk_pixbuf_loader_set_size (loader, width, height);
}

static GdkPixbufLoader *
create_loader (GFile        *file,
               const guchar *data,
               gsize         size)
{
  GdkPixbufLoader *loader;
  GError *error = NULL;
  char *mime_type;
  char *filename;

  loader = NULL;

  /* need to specify the type here because the gdk_pixbuf_loader_write
     doesn't have access to the filename in order to correct detect
     the image type. */
  filename = g_file_get_basename (file);
  mime_type = g_content_type_guess (filename, data, size, NULL);
  g_free (filename);

  if (mime_type != NULL) {
    loader = gdk_pixbuf_loader_new_with_mime_type (mime_type, &error);
  }

  if (loader == NULL && error != NULL) {
    g_warning ("Unable to create loader for mime type %s: %s", mime_type, error->message);
    g_clear_error (&error);
    loader = gdk_pixbuf_loader_new ();
  }
  g_free (mime_type);

  return loader;
}

static GdkPixbuf *
_gdk_pixbuf_new_from_uri_at_scale (const char *uri,
				   gint        width,
				   gint        height,
				   gboolean    preserve_aspect_ratio)
{
    gboolean result;
    guchar buffer[LOAD_BUFFER_SIZE];
    gsize bytes_read;
    GdkPixbufLoader *loader = NULL;
    GdkPixbuf *pixbuf;	
    GdkPixbufAnimation *animation;
    GdkPixbufAnimationIter *iter;
    gboolean has_frame;
    SizePrepareContext info;
    GFile *file;
    GFileInfo *file_info;
    GInputStream *input_stream;
    GError *error = NULL;

    g_return_val_if_fail (uri != NULL, NULL);

    input_stream = NULL;

    file = g_file_new_for_uri (uri);

    /* First see if we can get an input stream via preview::icon  */
    file_info = g_file_query_info (file,
                                   G_FILE_ATTRIBUTE_PREVIEW_ICON,
                                   G_FILE_QUERY_INFO_NONE,
                                   NULL,  /* GCancellable */
                                   NULL); /* return location for GError */
    if (file_info != NULL) {
        GObject *object;

        object = g_file_info_get_attribute_object (file_info,
                                                   G_FILE_ATTRIBUTE_PREVIEW_ICON);
        if (object != NULL && G_IS_LOADABLE_ICON (object)) {
            input_stream = g_loadable_icon_load (G_LOADABLE_ICON (object),
                                                 0,     /* size */
                                                 NULL,  /* return location for type */
                                                 NULL,  /* GCancellable */
                                                 NULL); /* return location for GError */
        }
        g_object_unref (file_info);
    }

    if (input_stream == NULL) {
        input_stream = G_INPUT_STREAM (g_file_read (file, NULL, &error));
        if (input_stream == NULL) {
            if (error != NULL) {
                g_warning ("Unable to create an input stream for %s: %s", uri, error->message);
                g_clear_error (&error);
            }
	    g_object_unref (file);
            return NULL;
        }
    }

    has_frame = FALSE;

    result = FALSE;
    while (!has_frame) {

	bytes_read = g_input_stream_read (input_stream,
					  buffer,
					  sizeof (buffer),
					  NULL,
					  &error);
        if (error != NULL) {
            g_warning ("Error reading from %s: %s", uri, error->message);
            g_clear_error (&error);
        }
	if (bytes_read == -1) {
	    break;
	}
	result = TRUE;
	if (bytes_read == 0) {
	    break;
	}

        if (loader == NULL) {
            loader = create_loader (file, buffer, bytes_read);
            if (1 <= width || 1 <= height) {
              info.width = width;
              info.height = height;
              info.input_width = info.input_height = 0;
              info.preserve_aspect_ratio = preserve_aspect_ratio;
              g_signal_connect (loader, "size-prepared", G_CALLBACK (size_prepared_cb), &info);
            }
            g_assert (loader != NULL);
        }

	if (!gdk_pixbuf_loader_write (loader,
				      (unsigned char *)buffer,
				      bytes_read,
				      &error)) {
            g_warning ("Error creating thumbnail for %s: %s", uri, error->message);
            g_clear_error (&error);
	    result = FALSE;
	    break;
	}

	animation = gdk_pixbuf_loader_get_animation (loader);
	if (animation) {
		iter = gdk_pixbuf_animation_get_iter (animation, NULL);
		if (!gdk_pixbuf_animation_iter_on_currently_loading_frame (iter)) {
			has_frame = TRUE;
		}
		g_object_unref (iter);
	}
    }

    if (!GDK_IS_PIXBUF_LOADER (loader)) {
        g_input_stream_close (input_stream, NULL, NULL);
        g_object_unref (input_stream);
        g_object_unref (file);
        return NULL;
    }

    gdk_pixbuf_loader_close (loader, NULL);

    if (!result) {
	g_object_unref (G_OBJECT (loader));
	g_input_stream_close (input_stream, NULL, NULL);
	g_object_unref (input_stream);
	g_object_unref (file);
	return NULL;
    }

    g_input_stream_close (input_stream, NULL, NULL);
    g_object_unref (input_stream);
    g_object_unref (file);

    pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
    if (pixbuf != NULL) {
	g_object_ref (G_OBJECT (pixbuf));
	g_object_set_data (G_OBJECT (pixbuf), "gnome-original-width",
			   GINT_TO_POINTER (info.input_width));
	g_object_set_data (G_OBJECT (pixbuf), "gnome-original-height",
			   GINT_TO_POINTER (info.input_height));
    }
    g_object_unref (G_OBJECT (loader));

    return pixbuf;
}

static void
gnome_desktop_thumbnail_factory_finalize (GObject *object)
{
  GnomeDesktopThumbnailFactory *factory;
  GnomeDesktopThumbnailFactoryPrivate *priv;
  
  factory = GNOME_DESKTOP_THUMBNAIL_FACTORY (object);

  priv = factory->priv;

  if (priv->thumbnailers)
    {
      g_list_free_full (priv->thumbnailers, (GDestroyNotify)thumbnailer_unref);
      priv->thumbnailers = NULL;
    }

  g_clear_pointer (&priv->mime_types_map, g_hash_table_destroy);

  if (priv->monitors)
    {
      g_list_free_full (priv->monitors, (GDestroyNotify)g_object_unref);
      priv->monitors = NULL;
    }

  g_mutex_clear (&priv->lock);

  g_clear_pointer (&priv->disabled_types, g_strfreev);
  g_clear_object (&priv->settings);

  if (G_OBJECT_CLASS (parent_class)->finalize)
    (* G_OBJECT_CLASS (parent_class)->finalize) (object);
}

/* These should be called with the lock held */
static void
gnome_desktop_thumbnail_factory_register_mime_types (GnomeDesktopThumbnailFactory *factory,
                                                     Thumbnailer                  *thumb)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;
  gint i;

  for (i = 0; thumb->mime_types[i]; i++)
    {
      if (!g_hash_table_lookup (priv->mime_types_map, thumb->mime_types[i]))
        g_hash_table_insert (priv->mime_types_map,
                             g_strdup (thumb->mime_types[i]),
                             thumbnailer_ref (thumb));
    }
}

static void
gnome_desktop_thumbnail_factory_add_thumbnailer (GnomeDesktopThumbnailFactory *factory,
                                                 Thumbnailer                  *thumb)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;

  gnome_desktop_thumbnail_factory_register_mime_types (factory, thumb);
  priv->thumbnailers = g_list_prepend (priv->thumbnailers, thumb);
}

static gboolean
gnome_desktop_thumbnail_factory_is_disabled (GnomeDesktopThumbnailFactory *factory,
                                             const gchar                  *mime_type)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;
  guint i;

  if (priv->disabled)
    return TRUE;

  if (!priv->disabled_types)
    return FALSE;

  for (i = 0; priv->disabled_types[i]; i++)
    {
      if (g_strcmp0 (priv->disabled_types[i], mime_type) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
remove_thumbnailer_from_mime_type_map (gchar       *key,
                                       Thumbnailer *value,
                                       gchar       *path)
{
  return (strcmp (value->path, path) == 0);
}


static void
update_or_create_thumbnailer (GnomeDesktopThumbnailFactory *factory,
                              const gchar                  *path)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;
  GList *l;
  Thumbnailer *thumb;
  gboolean found = FALSE;

  g_mutex_lock (&priv->lock);

  for (l = priv->thumbnailers; l && !found; l = g_list_next (l))
    {
      thumb = (Thumbnailer *)l->data;

      if (strcmp (thumb->path, path) == 0)
        {
          found = TRUE;

          /* First remove the mime_types associated to this thumbnailer */
          g_hash_table_foreach_remove (priv->mime_types_map,
                                       (GHRFunc)remove_thumbnailer_from_mime_type_map,
                                       (gpointer)path);
          if (!thumbnailer_reload (thumb))
              priv->thumbnailers = g_list_delete_link (priv->thumbnailers, l);
          else
              gnome_desktop_thumbnail_factory_register_mime_types (factory, thumb);
        }
    }

  if (!found)
    {
      thumb = thumbnailer_new (path);
      if (thumb)
        gnome_desktop_thumbnail_factory_add_thumbnailer (factory, thumb);
    }

  g_mutex_unlock (&priv->lock);
}

static void
remove_thumbnailer (GnomeDesktopThumbnailFactory *factory,
                    const gchar                  *path)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;
  GList *l;
  Thumbnailer *thumb;

  g_mutex_lock (&priv->lock);

  for (l = priv->thumbnailers; l; l = g_list_next (l))
    {
      thumb = (Thumbnailer *)l->data;

      if (strcmp (thumb->path, path) == 0)
        {
          priv->thumbnailers = g_list_delete_link (priv->thumbnailers, l);
          g_hash_table_foreach_remove (priv->mime_types_map,
                                       (GHRFunc)remove_thumbnailer_from_mime_type_map,
                                       (gpointer)path);
          thumbnailer_unref (thumb);

          break;
        }
    }

  g_mutex_unlock (&priv->lock);
}

static void
thumbnailers_directory_changed (GFileMonitor                 *monitor,
                                GFile                        *file,
                                GFile                        *other_file,
                                GFileMonitorEvent             event_type,
                                GnomeDesktopThumbnailFactory *factory)
{
  gchar *path;

  switch (event_type)
    {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_CHANGED:
    case G_FILE_MONITOR_EVENT_DELETED:
      path = g_file_get_path (file);
      if (!g_str_has_suffix (path, THUMBNAILER_EXTENSION))
        {
          g_free (path);
          return;
        }

      if (event_type == G_FILE_MONITOR_EVENT_DELETED)
        remove_thumbnailer (factory, path);
      else
        update_or_create_thumbnailer (factory, path);

      g_free (path);
      break;
    default:
      break;
    }
}

static void
gnome_desktop_thumbnail_factory_load_thumbnailers (GnomeDesktopThumbnailFactory *factory)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;
  const gchar * const *dirs;
  guint i;

  if (priv->loaded)
    return;

  dirs = get_thumbnailers_dirs ();
  for (i = 0; dirs[i]; i++)
    {
      const gchar *path = dirs[i];
      GDir *dir;
      GFile *dir_file;
      GFileMonitor *monitor;
      const gchar *dirent;

      dir = g_dir_open (path, 0, NULL);
      if (!dir)
        continue;

      /* Monitor dir */
      dir_file = g_file_new_for_path (path);
      monitor = g_file_monitor_directory (dir_file,
                                          G_FILE_MONITOR_NONE,
                                          NULL, NULL);
      if (monitor)
        {
          g_signal_connect (monitor, "changed",
                            G_CALLBACK (thumbnailers_directory_changed),
                            factory);
          priv->monitors = g_list_prepend (priv->monitors, monitor);
        }
      g_object_unref (dir_file);

      while ((dirent = g_dir_read_name (dir)))
        {
          Thumbnailer *thumb;
          gchar       *filename;

          if (!g_str_has_suffix (dirent, THUMBNAILER_EXTENSION))
            continue;

          filename = g_build_filename (path, dirent, NULL);
          thumb = thumbnailer_new (filename);
          g_free (filename);

          if (thumb)
            gnome_desktop_thumbnail_factory_add_thumbnailer (factory, thumb);
        }

      g_dir_close (dir);
    }

  priv->loaded = TRUE;
}

static void
external_thumbnailers_disabled_all_changed_cb (GSettings                    *settings,
                                               const gchar                  *key,
                                               GnomeDesktopThumbnailFactory *factory)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;

  g_mutex_lock (&priv->lock);

  priv->disabled = g_settings_get_boolean (priv->settings, "disable-all");
  if (priv->disabled)
    {
      g_strfreev (priv->disabled_types);
      priv->disabled_types = NULL;
    }
  else
    {
      priv->disabled_types = g_settings_get_strv (priv->settings, "disable");
      gnome_desktop_thumbnail_factory_load_thumbnailers (factory);
    }

  g_mutex_unlock (&priv->lock);
}

static void
external_thumbnailers_disabled_changed_cb (GSettings                    *settings,
                                           const gchar                  *key,
                                           GnomeDesktopThumbnailFactory *factory)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;

  g_mutex_lock (&priv->lock);

  if (!priv->disabled)
    {
      g_strfreev (priv->disabled_types);
      priv->disabled_types = g_settings_get_strv (priv->settings, "disable");
    }

  g_mutex_unlock (&priv->lock);
}

/* There are various different behavior depending on whether the application
   ran with sudo, pkexec, under the root user, and 'sudo su'...

   - sudo (program) keeps the user's home folder (and their .cache folder)
   - pkexec (program) uses root's .cache folder
   - root terminal, running (program) uses root's .cache folder
   - sudo su, then running (program), uses root's .cache folder

   Using sudo, or sudo su, SUDO_UID and friends are set in the environment
   Using pkexec, PKEXEC_UID is set

   root terminal and pkexec cases don't need any extra work - they're thumbnailing
   with the correct permissions (root-owned in root's .cache folder)

   sudo and sudo su need help, and each in a different way:
       - sudo su gives a false positive, since SUDO_UID is set, *but* the program
         is using root's .cache folder, so we really don't want to fix these
       - sudo (program) wants to use the user's cache folder, but will end up writing
         them as root:root files, instead of user:user.  So, in this case, we make sure
         to chmod those files to be owned by the original user, and not root.
*/

static void
get_user_info (GnomeDesktopThumbnailFactory *factory,
                                   gboolean *adjust,
                                      uid_t *uid,
                                      gid_t *gid)
{
    struct passwd *pwent;

    pwent = gnome_desktop_get_session_user_pwent ();

    *uid = pwent->pw_uid;
    *gid = pwent->pw_gid;

    /* Only can (and need to) adjust if we're root, but
       this process will be writing to the session user's
       home folder */

    *adjust = geteuid () == 0 &&
              g_strcmp0 (pwent->pw_dir, g_get_home_dir ()) == 0;
}

static void
gnome_desktop_thumbnail_factory_init (GnomeDesktopThumbnailFactory *factory)
{
  GnomeDesktopThumbnailFactoryPrivate *priv;
  
  factory->priv = gnome_desktop_thumbnail_factory_get_instance_private (factory);

  priv = factory->priv;
  priv->size = GNOME_DESKTOP_THUMBNAIL_SIZE_NORMAL;
 
  priv->mime_types_map = g_hash_table_new_full (g_str_hash,
                                                g_str_equal,
                                                (GDestroyNotify)g_free,
                                                (GDestroyNotify)thumbnailer_unref);

  get_user_info (factory, &priv->needs_chown, &priv->real_uid, &priv->real_gid);

  priv->permissions_problem = !gnome_desktop_thumbnail_cache_check_permissions (NULL, TRUE);

  g_mutex_init (&priv->lock);

  priv->settings = g_settings_new ("org.cinnamon.desktop.thumbnailers");
  priv->disabled = g_settings_get_boolean (priv->settings, "disable-all");
  if (!priv->disabled)
    priv->disabled_types = g_settings_get_strv (priv->settings, "disable");
  g_signal_connect (priv->settings, "changed::disable-all",
                    G_CALLBACK (external_thumbnailers_disabled_all_changed_cb),
                    factory);
  g_signal_connect (priv->settings, "changed::disable",
                    G_CALLBACK (external_thumbnailers_disabled_changed_cb),
                    factory);

  if (!priv->disabled)
    gnome_desktop_thumbnail_factory_load_thumbnailers (factory);
}

static void
gnome_desktop_thumbnail_factory_class_init (GnomeDesktopThumbnailFactoryClass *class)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (class);
	
  gobject_class->finalize = gnome_desktop_thumbnail_factory_finalize;
}

/**
 * gnome_desktop_thumbnail_factory_new:
 * @size: The thumbnail size to use
 *
 * Creates a new #GnomeDesktopThumbnailFactory.
 *
 * This function must be called on the main thread.
 * 
 * Return value: a new #GnomeDesktopThumbnailFactory
 *
 * Since: 2.2
 **/
GnomeDesktopThumbnailFactory *
gnome_desktop_thumbnail_factory_new (GnomeDesktopThumbnailSize size)
{
  GnomeDesktopThumbnailFactory *factory;
  
  factory = g_object_new (GNOME_DESKTOP_TYPE_THUMBNAIL_FACTORY, NULL);
  
  factory->priv->size = size;
  
  return factory;
}

/**
 * gnome_desktop_thumbnail_factory_lookup:
 * @factory: a #GnomeDesktopThumbnailFactory
 * @uri: the uri of a file
 * @mtime: the mtime of the file
 *
 * Tries to locate an existing thumbnail for the file specified.
 *
 * Usage of this function is threadsafe.
 *
 * Return value: The absolute path of the thumbnail, or %NULL if none exist.
 *
 * Since: 2.2
 **/
char *
gnome_desktop_thumbnail_factory_lookup (GnomeDesktopThumbnailFactory *factory,
					const char            *uri,
					time_t                 mtime)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;
  char *path, *file;
  GChecksum *checksum;
  guint8 digest[16];
  gsize digest_len = sizeof (digest);
  GdkPixbuf *pixbuf;
  gboolean res;

  g_return_val_if_fail (uri != NULL, NULL);

  res = FALSE;

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, (const guchar *) uri, strlen (uri));

  g_checksum_get_digest (checksum, digest, &digest_len);
  g_assert (digest_len == 16);

  file = g_strconcat (g_checksum_get_string (checksum), ".png", NULL);
  
  path = g_build_filename (g_get_user_cache_dir (),
			   "thumbnails",
			   (priv->size == GNOME_DESKTOP_THUMBNAIL_SIZE_NORMAL)?"normal":"large",
			   file,
			   NULL);
  g_free (file);

  pixbuf = gdk_pixbuf_new_from_file (path, NULL);
  if (pixbuf != NULL)
    {
      res = gnome_desktop_thumbnail_is_valid (pixbuf, uri, mtime);
      g_object_unref (pixbuf);
    }

  g_checksum_free (checksum);

  if (res)
    return path;

  g_free (path);
  return FALSE;
}

/**
 * gnome_desktop_thumbnail_factory_has_valid_failed_thumbnail:
 * @factory: a #GnomeDesktopThumbnailFactory
 * @uri: the uri of a file
 * @mtime: the mtime of the file
 *
 * Tries to locate an failed thumbnail for the file specified. Writing
 * and looking for failed thumbnails is important to avoid to try to
 * thumbnail e.g. broken images several times.
 *
 * Usage of this function is threadsafe.
 *
 * Return value: TRUE if there is a failed thumbnail for the file.
 *
 * Since: 2.2
 **/
gboolean
gnome_desktop_thumbnail_factory_has_valid_failed_thumbnail (GnomeDesktopThumbnailFactory *factory,
							    const char            *uri,
							    time_t                 mtime)
{
  char *path, *file;
  GdkPixbuf *pixbuf;
  gboolean res;
  GChecksum *checksum;
  guint8 digest[16];
  gsize digest_len = sizeof (digest);

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, (const guchar *) uri, strlen (uri));

  g_checksum_get_digest (checksum, digest, &digest_len);
  g_assert (digest_len == 16);

  res = FALSE;

  file = g_strconcat (g_checksum_get_string (checksum), ".png", NULL);

  path = g_build_filename (g_get_user_cache_dir (),
			   "thumbnails/fail",
			   appname,
			   file,
			   NULL);
  g_free (file);

  pixbuf = gdk_pixbuf_new_from_file (path, NULL);
  g_free (path);

  if (pixbuf)
    {
      res = gnome_desktop_thumbnail_is_valid (pixbuf, uri, mtime);
      g_object_unref (pixbuf);
    }

  g_checksum_free (checksum);

  return res;
}

static gboolean
mimetype_supported_by_gdk_pixbuf (const char *mime_type)
{
    guint i;
    static gsize formats_hash = 0;
    gchar *key;
    gboolean result;

    if (g_once_init_enter (&formats_hash)) {
        GSList *formats, *list;
        GHashTable *hash;

        hash = g_hash_table_new_full (g_str_hash,
                                      (GEqualFunc) g_content_type_equals,
                                      g_free, NULL);

        formats = gdk_pixbuf_get_formats ();
        list = formats;

        while (list) {
            GdkPixbufFormat *format = list->data;
            gchar **mime_types;

            mime_types = gdk_pixbuf_format_get_mime_types (format);

            for (i = 0; mime_types[i] != NULL; i++)
              {
                    g_hash_table_insert (hash,
                                         (gpointer) g_content_type_from_mime_type (mime_types[i]),
                                         GUINT_TO_POINTER (1));
              }

            g_strfreev (mime_types);
            list = list->next;
        }

        g_slist_free (formats);

        g_once_init_leave (&formats_hash, (gsize) hash);
    }

    key = g_content_type_from_mime_type (mime_type);
    if (g_hash_table_lookup ((void*)formats_hash, key))
            result = TRUE;
    else
            result = FALSE;
    g_free (key);

    return result;
}

/**
 * gnome_desktop_thumbnail_factory_can_thumbnail:
 * @factory: a #GnomeDesktopThumbnailFactory
 * @uri: the uri of a file
 * @mime_type: the mime type of the file
 * @mtime: the mtime of the file
 *
 * Returns TRUE if this GnomeIconFactory can (at least try) to thumbnail
 * this file. Thumbnails or files with failed thumbnails won't be thumbnailed.
 *
 * Usage of this function is threadsafe.
 *
 * Return value: TRUE if the file can be thumbnailed.
 *
 * Since: 2.2
 **/
gboolean
gnome_desktop_thumbnail_factory_can_thumbnail (GnomeDesktopThumbnailFactory *factory,
					       const char            *uri,
					       const char            *mime_type,
					       time_t                 mtime)
{
  if (factory->priv->permissions_problem)
    return FALSE;

  /* Don't thumbnail thumbnails */
  if (uri &&
      strncmp (uri, "file:/", 6) == 0 &&
      strstr (uri, "/thumbnails/") != NULL)
    return FALSE;
  
  if (!mime_type)
    return FALSE;

  if (gnome_desktop_thumbnail_factory_is_disabled (factory, mime_type))
    {
      return FALSE;
    }

  g_mutex_lock (&factory->priv->lock);

  Thumbnailer *thumb;
  thumb = g_hash_table_lookup (factory->priv->mime_types_map, mime_type);

  g_mutex_unlock (&factory->priv->lock);

  if (thumb || mimetype_supported_by_gdk_pixbuf (mime_type))
    {
      return !gnome_desktop_thumbnail_factory_has_valid_failed_thumbnail (factory,
                                                                          uri,
                                                                          mtime);
    }
  
  return FALSE;
}

static char *
expand_thumbnailing_script (const char *script,
			    const int   size, 
			    const char *inuri,
			    const char *outfile)
{
  GString *str;
  const char *p, *last;
  char *localfile, *quoted;
  gboolean got_in;

  str = g_string_new (NULL);
  
  got_in = FALSE;
  last = script;
  while ((p = strchr (last, '%')) != NULL)
    {
      g_string_append_len (str, last, p - last);
      p++;

      switch (*p) {
      case 'u':
	quoted = g_shell_quote (inuri);
	g_string_append (str, quoted);
	g_free (quoted);
	got_in = TRUE;
	p++;
	break;
      case 'i':
	localfile = g_filename_from_uri (inuri, NULL, NULL);
	if (localfile)
	  {
	    quoted = g_shell_quote (localfile);
	    g_string_append (str, quoted);
	    got_in = TRUE;
	    g_free (quoted);
	    g_free (localfile);
	  }
	p++;
	break;
      case 'o':
	quoted = g_shell_quote (outfile);
	g_string_append (str, quoted);
	g_free (quoted);
	p++;
	break;
      case 's':
	g_string_append_printf (str, "%d", size);
	p++;
	break;
      case '%':
	g_string_append_c (str, '%');
	p++;
	break;
      case 0:
      default:
	break;
      }
      last = p;
    }
  g_string_append (str, last);

  if (got_in)
    return g_string_free (str, FALSE);

  g_string_free (str, TRUE);
  return NULL;
}

/**
 * gnome_desktop_thumbnail_factory_generate_thumbnail:
 * @factory: a #GnomeDesktopThumbnailFactory
 * @uri: the uri of a file
 * @mime_type: the mime type of the file
 *
 * Tries to generate a thumbnail for the specified file. If it succeeds
 * it returns a pixbuf that can be used as a thumbnail.
 *
 * Usage of this function is threadsafe.
 *
 * Return value: (transfer full): thumbnail pixbuf if thumbnailing succeeded, %NULL otherwise.
 *
 * Since: 2.2
 **/
GdkPixbuf *
gnome_desktop_thumbnail_factory_generate_thumbnail (GnomeDesktopThumbnailFactory *factory,
						    const char            *uri,
						    const char            *mime_type)
{
  GdkPixbuf *pixbuf, *scaled, *tmp_pixbuf;
  char *script, *expanded_script;
  int width, height, size;
  int original_width = 0;
  int original_height = 0;
  char dimension[12];
  double scale;
  int exit_status;
  char *tmpname;
  gboolean disabled = FALSE;

  g_return_val_if_fail (uri != NULL, NULL);
  g_return_val_if_fail (mime_type != NULL, NULL);

  /* Doesn't access any volatile fields in factory, so it's threadsafe */
  
  size = 128;
  if (factory->priv->size == GNOME_DESKTOP_THUMBNAIL_SIZE_LARGE)
    size = 256;

  pixbuf = NULL;

  script = NULL;
  g_mutex_lock (&factory->priv->lock);

  disabled = gnome_desktop_thumbnail_factory_is_disabled (factory, mime_type);

  if (!disabled)
    {
      Thumbnailer *thumb;

      thumb = g_hash_table_lookup (factory->priv->mime_types_map, mime_type);
      if (thumb)
        script = g_strdup (thumb->command);
    }
  g_mutex_unlock (&factory->priv->lock);
  
  if (script)
    {
      int fd;

      fd = g_file_open_tmp (".gnome_desktop_thumbnail.XXXXXX", &tmpname, NULL);

      if (fd != -1)
	{
	  close (fd);

	  expanded_script = expand_thumbnailing_script (script, size, uri, tmpname);
	  if (expanded_script != NULL &&
	      g_spawn_command_line_sync (expanded_script,
					 NULL, NULL, &exit_status, NULL) &&
	      exit_status == 0)
	    {
	      pixbuf = gdk_pixbuf_new_from_file (tmpname, NULL);
	    }

	  g_free (expanded_script);
	  g_unlink (tmpname);
	  g_free (tmpname);
	}

      g_free (script);
    }

  /* Fall back to gdk-pixbuf */
  if (!disabled && pixbuf == NULL && mimetype_supported_by_gdk_pixbuf (mime_type))
    {
      pixbuf = _gdk_pixbuf_new_from_uri_at_scale (uri, size, size, TRUE);

      if (pixbuf != NULL)
        {
          original_width = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (pixbuf),
                                                               "gnome-original-width"));
          original_height = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (pixbuf),
                                                                "gnome-original-height"));
        }
    }
      
  if (pixbuf == NULL)
    return NULL;

  /* The pixbuf loader may attach an "orientation" option to the pixbuf,
     if the tiff or exif jpeg file had an orientation tag. Rotate/flip
     the pixbuf as specified by this tag, if present. */
  tmp_pixbuf = gdk_pixbuf_apply_embedded_orientation (pixbuf);
  g_object_unref (pixbuf);
  pixbuf = tmp_pixbuf;

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  
  if (width > size || height > size)
    {
      const gchar *orig_width, *orig_height;
      scale = (double)size / MAX (width, height);

      scaled = gnome_desktop_thumbnail_scale_down_pixbuf (pixbuf,
						  floor (width * scale + 0.5),
						  floor (height * scale + 0.5));

      orig_width = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::Image::Width");
      orig_height = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::Image::Height");

      if (orig_width != NULL) {
	      gdk_pixbuf_set_option (scaled, "tEXt::Thumb::Image::Width", orig_width);
      }
      if (orig_height != NULL) {
	      gdk_pixbuf_set_option (scaled, "tEXt::Thumb::Image::Height", orig_height);
      }
      
      g_object_unref (pixbuf);
      pixbuf = scaled;
    }
  
  if (original_width > 0) {
	  g_snprintf (dimension, sizeof (dimension), "%i", original_width);
	  gdk_pixbuf_set_option (pixbuf, "tEXt::Thumb::Image::Width", dimension);
  }
  if (original_height > 0) {
	  g_snprintf (dimension, sizeof (dimension), "%i", original_height);
	  gdk_pixbuf_set_option (pixbuf, "tEXt::Thumb::Image::Height", dimension);
  }

  return pixbuf;
}

static void
maybe_fix_ownership (GnomeDesktopThumbnailFactory *factory, const gchar *path)
{
    if (factory->priv->needs_chown) {
        G_GNUC_UNUSED int res;

        res = chown (path,
                     factory->priv->real_uid,
                     factory->priv->real_gid);
    }
}

static gboolean
make_thumbnail_dirs (GnomeDesktopThumbnailFactory *factory)
{
  char *thumbnail_dir;
  char *image_dir;
  gboolean res;

  res = FALSE;

  thumbnail_dir = g_build_filename (g_get_user_cache_dir (),
				    "thumbnails",
				    NULL);
  if (!g_file_test (thumbnail_dir, G_FILE_TEST_IS_DIR))
    {
      g_mkdir (thumbnail_dir, 0700);
      maybe_fix_ownership (factory, thumbnail_dir);
      res = TRUE;
    }

  image_dir = g_build_filename (thumbnail_dir,
				(factory->priv->size == GNOME_DESKTOP_THUMBNAIL_SIZE_NORMAL)?"normal":"large",
				NULL);
  if (!g_file_test (image_dir, G_FILE_TEST_IS_DIR))
    {
      g_mkdir (image_dir, 0700);
      maybe_fix_ownership (factory, image_dir);
      res = TRUE;
    }

  g_free (thumbnail_dir);
  g_free (image_dir);
  
  return res;
}

static gboolean
make_thumbnail_fail_dirs (GnomeDesktopThumbnailFactory *factory)
{
  char *thumbnail_dir;
  char *fail_dir;
  char *app_dir;
  gboolean res;

  res = FALSE;

  thumbnail_dir = g_build_filename (g_get_user_cache_dir (),
				    "thumbnails",
				    NULL);
  if (!g_file_test (thumbnail_dir, G_FILE_TEST_IS_DIR))
    {
      g_mkdir (thumbnail_dir, 0700);
      maybe_fix_ownership (factory, thumbnail_dir);
      res = TRUE;
    }

  fail_dir = g_build_filename (thumbnail_dir,
			       "fail",
			       NULL);
  if (!g_file_test (fail_dir, G_FILE_TEST_IS_DIR))
    {
      g_mkdir (fail_dir, 0700);
      maybe_fix_ownership (factory, fail_dir);
      res = TRUE;
    }

  app_dir = g_build_filename (fail_dir,
			      appname,
			      NULL);
  if (!g_file_test (app_dir, G_FILE_TEST_IS_DIR))
    {
      g_mkdir (app_dir, 0700);
      maybe_fix_ownership (factory, app_dir);
      res = TRUE;
    }

  g_free (thumbnail_dir);
  g_free (fail_dir);
  g_free (app_dir);
  
  return res;
}


/**
 * gnome_desktop_thumbnail_factory_save_thumbnail:
 * @factory: a #GnomeDesktopThumbnailFactory
 * @thumbnail: the thumbnail as a pixbuf 
 * @uri: the uri of a file
 * @original_mtime: the modification time of the original file 
 *
 * Saves @thumbnail at the right place. If the save fails a
 * failed thumbnail is written.
 *
 * Usage of this function is threadsafe.
 *
 * Since: 2.2
 **/
void
gnome_desktop_thumbnail_factory_save_thumbnail (GnomeDesktopThumbnailFactory *factory,
						GdkPixbuf             *thumbnail,
						const char            *uri,
						time_t                 original_mtime)
{
  GnomeDesktopThumbnailFactoryPrivate *priv = factory->priv;
  char *path, *file;
  char *tmp_path;
  const char *width, *height;
  int tmp_fd;
  char mtime_str[21];
  gboolean saved_ok;
  GChecksum *checksum;
  guint8 digest[16];
  gsize digest_len = sizeof (digest);
  GError *error;

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, (const guchar *) uri, strlen (uri));

  g_checksum_get_digest (checksum, digest, &digest_len);
  g_assert (digest_len == 16);

  file = g_strconcat (g_checksum_get_string (checksum), ".png", NULL);

  path = g_build_filename (g_get_user_cache_dir (),
			   "thumbnails",
			   (priv->size == GNOME_DESKTOP_THUMBNAIL_SIZE_NORMAL)?"normal":"large",
			   file,
			   NULL);

  g_free (file);

  g_checksum_free (checksum);

  tmp_path = g_strconcat (path, ".XXXXXX", NULL);

  tmp_fd = g_mkstemp (tmp_path);
  if (tmp_fd == -1 &&
      make_thumbnail_dirs (factory))
    {
      g_free (tmp_path);
      tmp_path = g_strconcat (path, ".XXXXXX", NULL);
      tmp_fd = g_mkstemp (tmp_path);
    }

  if (tmp_fd == -1)
    {
      gnome_desktop_thumbnail_factory_create_failed_thumbnail (factory, uri, original_mtime);
      g_free (tmp_path);
      g_free (path);
      return;
    }
  close (tmp_fd);
  
  g_snprintf (mtime_str, 21, "%ld",  original_mtime);
  width = gdk_pixbuf_get_option (thumbnail, "tEXt::Thumb::Image::Width");
  height = gdk_pixbuf_get_option (thumbnail, "tEXt::Thumb::Image::Height");

  error = NULL;
  if (width != NULL && height != NULL) 
    saved_ok  = gdk_pixbuf_save (thumbnail,
				 tmp_path,
				 "png", &error,
				 "tEXt::Thumb::Image::Width", width,
				 "tEXt::Thumb::Image::Height", height,
				 "tEXt::Thumb::URI", uri,
				 "tEXt::Thumb::MTime", mtime_str,
				 "tEXt::Software", "GNOME::ThumbnailFactory",
				 NULL);
  else
    saved_ok  = gdk_pixbuf_save (thumbnail,
				 tmp_path,
				 "png", &error,
				 "tEXt::Thumb::URI", uri,
				 "tEXt::Thumb::MTime", mtime_str,
				 "tEXt::Software", "GNOME::ThumbnailFactory",
				 NULL);
    

  if (saved_ok)
    {
      g_chmod (tmp_path, 0600);
      g_rename (tmp_path, path);
      maybe_fix_ownership (factory, path);
    }
  else
    {
      g_warning ("Failed to create thumbnail %s: %s", tmp_path, error->message);
      gnome_desktop_thumbnail_factory_create_failed_thumbnail (factory, uri, original_mtime);
      g_unlink (tmp_path);
      g_clear_error (&error);
    }

  g_free (path);
  g_free (tmp_path);
}

/**
 * gnome_desktop_thumbnail_factory_create_failed_thumbnail:
 * @factory: a #GnomeDesktopThumbnailFactory
 * @uri: the uri of a file
 * @mtime: the modification time of the file
 *
 * Creates a failed thumbnail for the file so that we don't try
 * to re-thumbnail the file later.
 *
 * Usage of this function is threadsafe.
 *
 * Since: 2.2
 **/
void
gnome_desktop_thumbnail_factory_create_failed_thumbnail (GnomeDesktopThumbnailFactory *factory,
							 const char            *uri,
							 time_t                 mtime)
{
  char *path, *file;
  char *tmp_path;
  int tmp_fd;
  char mtime_str[21];
  gboolean saved_ok;
  GdkPixbuf *pixbuf;
  GChecksum *checksum;
  guint8 digest[16];
  gsize digest_len = sizeof (digest);

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, (const guchar *) uri, strlen (uri));

  g_checksum_get_digest (checksum, digest, &digest_len);
  g_assert (digest_len == 16);

  file = g_strconcat (g_checksum_get_string (checksum), ".png", NULL);

  path = g_build_filename (g_get_user_cache_dir (),
			   "thumbnails/fail",
			   appname,
			   file,
			   NULL);
  g_free (file);

  g_checksum_free (checksum);

  tmp_path = g_strconcat (path, ".XXXXXX", NULL);

  tmp_fd = g_mkstemp (tmp_path);
  if (tmp_fd == -1 &&
      make_thumbnail_fail_dirs (factory))
    {
      g_free (tmp_path);
      tmp_path = g_strconcat (path, ".XXXXXX", NULL);
      tmp_fd = g_mkstemp (tmp_path);
    }

  if (tmp_fd == -1)
    {
      g_free (tmp_path);
      g_free (path);
      return;
    }
  close (tmp_fd);
  
  g_snprintf (mtime_str, 21, "%ld",  mtime);
  pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
  saved_ok  = gdk_pixbuf_save (pixbuf,
			       tmp_path,
			       "png", NULL, 
			       "tEXt::Thumb::URI", uri,
			       "tEXt::Thumb::MTime", mtime_str,
			       "tEXt::Software", "GNOME::ThumbnailFactory",
			       NULL);
  g_object_unref (pixbuf);
  if (saved_ok)
    {
      g_chmod (tmp_path, 0600);
      g_rename(tmp_path, path);
      maybe_fix_ownership (factory, path);
    }

  g_free (path);
  g_free (tmp_path);
}

/**
 * gnome_desktop_thumbnail_md5:
 * @uri: an uri
 *
 * Calculates the MD5 checksum of the uri. This can be useful
 * if you want to manually handle thumbnail files.
 *
 * Return value: A string with the MD5 digest of the uri string.
 *
 * Since: 2.2
 * Deprecated: 2.22: Use #GChecksum instead
 **/
char *
gnome_desktop_thumbnail_md5 (const char *uri)
{
  return g_compute_checksum_for_data (G_CHECKSUM_MD5,
                                      (const guchar *) uri,
                                      strlen (uri));
}

/**
 * gnome_desktop_thumbnail_path_for_uri:
 * @uri: an uri
 * @size: a thumbnail size
 *
 * Returns the filename that a thumbnail of size @size for @uri would have.
 *
 * Return value: an absolute filename
 *
 * Since: 2.2
 **/
char *
gnome_desktop_thumbnail_path_for_uri (const char         *uri,
				      GnomeDesktopThumbnailSize  size)
{
  char *md5;
  char *file;
  char *path;

  md5 = gnome_desktop_thumbnail_md5 (uri);
  file = g_strconcat (md5, ".png", NULL);
  g_free (md5);
  
  path = g_build_filename (g_get_user_cache_dir (),
			   "thumbnails",
			   (size == GNOME_DESKTOP_THUMBNAIL_SIZE_NORMAL)?"normal":"large",
			   file,
			   NULL);
    
  g_free (file);

  return path;
}

/**
 * gnome_desktop_thumbnail_has_uri:
 * @pixbuf: an loaded thumbnail pixbuf
 * @uri: a uri
 *
 * Returns whether the thumbnail has the correct uri embedded in the
 * Thumb::URI option in the png.
 *
 * Return value: TRUE if the thumbnail is for @uri
 *
 * Since: 2.2
 **/
gboolean
gnome_desktop_thumbnail_has_uri (GdkPixbuf          *pixbuf,
				 const char         *uri)
{
  const char *thumb_uri;
  
  thumb_uri = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::URI");
  if (!thumb_uri)
    return FALSE;

  return strcmp (uri, thumb_uri) == 0;
}

/**
 * gnome_desktop_thumbnail_is_valid:
 * @pixbuf: an loaded thumbnail #GdkPixbuf
 * @uri: a uri
 * @mtime: the mtime
 *
 * Returns whether the thumbnail has the correct uri and mtime embedded in the
 * png options.
 *
 * Return value: TRUE if the thumbnail has the right @uri and @mtime
 *
 * Since: 2.2
 **/
gboolean
gnome_desktop_thumbnail_is_valid (GdkPixbuf          *pixbuf,
				  const char         *uri,
				  time_t              mtime)
{
  const char *thumb_uri, *thumb_mtime_str;
  time_t thumb_mtime;
  
  thumb_uri = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::URI");
  if (!thumb_uri)
    return FALSE;
  if (strcmp (uri, thumb_uri) != 0)
    return FALSE;
  
  thumb_mtime_str = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::MTime");
  if (!thumb_mtime_str)
    return FALSE;
  thumb_mtime = atol (thumb_mtime_str);
  if (mtime != thumb_mtime)
    return FALSE;
  
  return TRUE;
}

static void
fix_owner (const gchar *path, uid_t uid, gid_t gid)
{
    G_GNUC_UNUSED int res;

    res = chown (path, uid, gid);
}

static gboolean
access_ok (const gchar *path, uid_t uid, gid_t gid)
{
    /* user mode will always trip on this */
    if (g_access (path, R_OK|W_OK) != 0) {
        if (errno != ENOENT && errno != EFAULT)
            return FALSE;
        else
            return TRUE;
    }

    /* root will need to check against the real user */

    GStatBuf buf;

    gint ret = g_stat (path, &buf);

    if (ret == 0) {
        if (buf.st_uid != uid || buf.st_gid != gid || (buf.st_mode & (S_IRUSR|S_IWUSR)) == 0)
            return FALSE;
    }

    return TRUE;
}

static void
recursively_fix_file (const gchar *path, uid_t uid, gid_t gid)
{
    if (!access_ok (path, uid, gid))
        fix_owner (path, uid, gid);

    if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
        GDir *dir = g_dir_open (path, 0, NULL);

        if (dir) {
            const char *name;

            while ((name = g_dir_read_name (dir))) {
                gchar *filename;

                filename = g_build_filename (path, name, NULL);
                recursively_fix_file (filename, uid, gid);
                g_free (filename);
            }

            g_dir_close (dir);
        }
    }
}

static gboolean
recursively_check_file (const gchar *path, uid_t uid, gid_t gid)
{
    if (!access_ok (path, uid, gid))
        return FALSE;

    gboolean ret = TRUE;

    if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
        GDir *dir = g_dir_open (path, 0, NULL);

        if (dir) {
            const char *name;

            while ((name = g_dir_read_name (dir))) {
                gchar *filename;
                filename = g_build_filename (path, name, NULL);

                if (!recursively_check_file (filename, uid, gid)) {
                    ret = FALSE;
                }

                g_free (filename);

                if (!ret)
                    break;
            }

            g_dir_close (dir);
        }
    }

    return ret;
}

static gboolean
check_subfolder_permissions_only (const gchar *path, uid_t uid, gid_t gid)
{
    gboolean ret = TRUE;

    GDir *dir = g_dir_open (path, 0, NULL);

    if (dir) {
        const char *name;

        while ((name = g_dir_read_name (dir))) {
            gchar *filename;
            filename = g_build_filename (path, name, NULL);

            if (!access_ok (filename, uid, gid)) {
                ret = FALSE;
            }

            g_free (filename);

            if (!ret)
                break;
        }

        g_dir_close (dir);
    }

    return ret;
}

/**
 * gnome_desktop_cache_fix_permissions:
 *
 * Fixes any file or folder ownership issues for the *currently
 * logged-in* user.  Note - this may not be the same as the uid
 * of the user running this operation.
 *
 **/

void
gnome_desktop_thumbnail_cache_fix_permissions (void)
{
    struct passwd *pwent;

    pwent = gnome_desktop_get_session_user_pwent ();

    gchar *cache_dir = g_build_filename (g_get_user_cache_dir (), "thumbnails", NULL);

    if (!access_ok (cache_dir, pwent->pw_uid, pwent->pw_gid))
        fix_owner (cache_dir, pwent->pw_uid, pwent->pw_gid);

    recursively_fix_file (cache_dir, pwent->pw_uid, pwent->pw_gid);

    g_free (cache_dir);
}

/**
 * gnome_desktop_cache_check_permissions:
 * @factory: (allow-none): an optional GnomeDesktopThumbnailFactory
 * @quick: if TRUE, only do a quick check of directory ownersip
 * This is more serious than thumbnail ownership issues, and is faster.
 *
 * Returns whether there are any ownership issues with the thumbnail
 * folders and existing thumbnails for the *currently logged-in* user.
 * Note - this may not be the same as the uid of the user running this
 * check.
 *
 * if factory is not NULL, factory->priv->permissions_problem will be
 * set to the result of the check.
 * 
 * Return value: TRUE if everything checks out in these folders for the
 * logged in user.
 *
 **/

gboolean
gnome_desktop_thumbnail_cache_check_permissions (GnomeDesktopThumbnailFactory *factory, gboolean quick)
{
    gboolean checks_out = TRUE;

    struct passwd *pwent;
    pwent = gnome_desktop_get_session_user_pwent ();
    gchar *cache_dir = g_build_filename (g_get_user_cache_dir(), "thumbnails", NULL);

    if (!access_ok (cache_dir, pwent->pw_uid, pwent->pw_gid)) {
        checks_out = FALSE;
        goto out;
    }

    if (quick) {
        checks_out = check_subfolder_permissions_only (cache_dir, pwent->pw_uid, pwent->pw_gid);
    } else {
        checks_out = recursively_check_file (cache_dir, pwent->pw_uid, pwent->pw_gid);
    }

out:
    g_free (cache_dir);

    if (factory)
        factory->priv->permissions_problem = !checks_out;

    return checks_out;
}
