/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "file-chooser.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _FileChooser FileChooser;
typedef struct _FileChooserClass FileChooserClass;

struct _FileChooser
{
  XdpFileChooserSkeleton parent_instance;
};

struct _FileChooserClass
{
  XdpFileChooserSkeletonClass parent_class;
};

static XdpImplFileChooser *impl;
static FileChooser *file_chooser;

GType file_chooser_get_type (void) G_GNUC_CONST;
static void file_chooser_iface_init (XdpFileChooserIface *iface);

G_DEFINE_TYPE_WITH_CODE (FileChooser, file_chooser, XDP_TYPE_FILE_CHOOSER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_FILE_CHOOSER, file_chooser_iface_init));

static void
open_file_done (GObject *source,
                GAsyncResult *result,
                gpointer data)
{
  g_autoptr(Request) request = data;
  guint response;
  GVariant *options;
  GVariantBuilder results;
  GVariantBuilder ruris;
  g_autofree char *ruri = NULL;
  gboolean writable = TRUE;
  g_autoptr(GError) error = NULL;
  const char **uris;
  GVariant *choices;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&ruris, G_VARIANT_TYPE_STRING_ARRAY);

  if (!xdp_impl_file_chooser_call_open_file_finish (XDP_IMPL_FILE_CHOOSER (source),
                                                    &response,
                                                    &options,
                                                    result,
                                                    &error))
    response = 2;

  if (response != 0)
    goto out;

  if (!g_variant_lookup (options, "b", "writable", &writable))
    writable = FALSE;

  choices = g_variant_lookup_value (options, "choices", G_VARIANT_TYPE ("a(ss)"));
  if (choices)
    g_variant_builder_add (&results, "{sv}", "choices", choices);

  if (g_variant_lookup (options, "uris", "^a&s", &uris))
    {
      ruri = register_document (uris[0], request->app_id, FALSE, writable, &error);
      if (ruri == NULL)
        {
          g_warning ("Failed to register %s: %s\n", uris[0], error->message);
          g_clear_error (&error);
          goto out;
        }
      g_debug ("convert uri %s -> %s\n", uris[0], ruri);
      g_variant_builder_add (&ruris, "s", ruri);
    }

out:
  g_variant_builder_add (&results, "{sv}", "uris", g_variant_builder_end (&ruris));

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

typedef struct {
  const char *key;
  const GVariantType *type;
} OptionKey;

static OptionKey open_file_options[] = {
  { "accept_label", G_VARIANT_TYPE_STRING },
  { "filters", (const GVariantType *)"a(sa(us))" },
  { "choices", (const GVariantType *)"a(ssa(ss)s)" }
};

void
copy_options (GVariant *arg_options,
              GVariantBuilder *options,
              OptionKey *supported_options,
              int n_supported_options)
{
  GVariant *value;
  int i;

  for (i = 0; i < n_supported_options; i++)
    {
      value = g_variant_lookup_value (arg_options,
                                      supported_options[i].key,
                                      supported_options[i].type);
      if (value)
         g_variant_builder_add (options, "{sv}", supported_options[i].key, value);
    }
}

static gboolean
handle_open_file (XdpFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_parent_window,
                  const gchar *arg_title,
                  GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  copy_options (arg_options, &options, open_file_options, G_N_ELEMENTS (open_file_options));

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_impl_file_chooser_call_open_file (impl,
                                        request->id,
                                        app_id,
                                        arg_parent_window,
                                        arg_title,
                                        g_variant_builder_end (&options),
                                        NULL,
                                        open_file_done,
                                        g_object_ref (request));

  xdp_file_chooser_complete_open_file (object, invocation, request->id);

  return TRUE;
}

static void
open_files_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  g_autoptr(Request) request = data;
  guint response;
  GVariant *options;
  GVariantBuilder ruris;
  GVariantBuilder results;
  g_autofree char *ruri = NULL;
  gboolean writable = TRUE;
  g_autoptr(GError) error = NULL;
  int i;
  const char **uris;
  GVariant *choices;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&ruris, G_VARIANT_TYPE_STRING_ARRAY);

  if (!xdp_impl_file_chooser_call_open_files_finish (XDP_IMPL_FILE_CHOOSER (source),
                                                     &response,
                                                     &options,
                                                     result,
                                                     &error))
    response = 2;

  if (response != 0)
    goto out;

  if (!g_variant_lookup (options, "b", "writable", &writable))
    writable = FALSE;

  choices = g_variant_lookup_value (options, "choices", G_VARIANT_TYPE ("a(ss)"));
  if (choices)
    g_variant_builder_add (&results, "{sv}", "choices", choices);

  if (g_variant_lookup (options, "uris", "^a&s", &uris))
    {
      for (i = 0; uris && uris[i]; i++)
        {
          ruri = register_document (uris[i], request->app_id, FALSE, writable, &error);
          if (ruri == NULL)
            {
              g_warning ("Failed to register %s: %s\n", uris[i], error->message);
              g_clear_error (&error);
              continue;
            }
          g_debug ("convert uri %s -> %s\n", uris[i], ruri);
          g_variant_builder_add (&ruris, "s", ruri);
        }
    }

out:
  g_variant_builder_add (&results, "{&sv}", "uris", g_variant_builder_end (&ruris));

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_open_files (XdpFileChooser *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   const gchar *arg_title,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  XdpImplRequest *impl_request;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  copy_options (arg_options, &options, open_file_options, G_N_ELEMENTS (open_file_options));

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_impl_file_chooser_call_open_files (impl,
                                         request->id,
                                         app_id,
                                         arg_parent_window,
                                         arg_title,
                                         g_variant_builder_end (&options),
                                         NULL,
                                         open_files_done,
                                         g_object_ref (request));

  xdp_file_chooser_complete_open_files (object, invocation, request->id);

  return TRUE;
}

static OptionKey save_file_options[] = {
  { "accept_label", G_VARIANT_TYPE_STRING },
  { "filters", (const GVariantType *)"a(sa(us))" },
  { "current_name", G_VARIANT_TYPE_STRING },
  { "current_folder", G_VARIANT_TYPE_BYTESTRING },
  { "current_file", G_VARIANT_TYPE_BYTESTRING },
  { "choices", (const GVariantType *)"a(ssa(ss)s)" }
};

static void
save_file_done (GObject *source,
                GAsyncResult *result,
                gpointer data)
{
  g_autoptr(Request) request = data;
  guint response;
  GVariant *options;
  GVariantBuilder results;
  GVariantBuilder ruris;
  g_autofree char *ruri = NULL;
  g_autoptr(GError) error = NULL;
  const char **uris;
  GVariant *choices;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&ruris, G_VARIANT_TYPE_STRING_ARRAY);

  if (!xdp_impl_file_chooser_call_save_file_finish (XDP_IMPL_FILE_CHOOSER (source),
                                                    &response,
                                                    &options,
                                                    result,
                                                    &error))
    response = 2;

  if (response != 0)
    goto out;

  choices = g_variant_lookup_value (options, "choices", G_VARIANT_TYPE ("a(ss)"));
  if (choices)
    g_variant_builder_add (&results, "{sv}", "choices", choices);

  if (g_variant_lookup (options, "uris", "^a&s", &uris))
    {
      ruri = register_document (uris[0], request->app_id, TRUE, TRUE, &error);
      if (ruri == NULL)
        {
          g_warning ("Failed to register %s: %s\n", uris[0], error->message);
          g_clear_error (&error);
          goto out;
        }
      g_debug ("convert uri %s -> %s\n", uris[0], ruri);
      g_variant_builder_add (&ruris, "s", ruri);
    }

out:
  g_variant_builder_add (&results, "{&sv}", "uris", g_variant_builder_end (&ruris));

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
handle_save_file (XdpFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_parent_window,
                  const gchar *arg_title,
                  GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  XdpImplRequest *impl_request;
  GVariantBuilder options;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  copy_options (arg_options, &options, save_file_options, G_N_ELEMENTS (save_file_options));

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_impl_file_chooser_call_save_file (impl,
                                        request->id,
                                        app_id,
                                        arg_parent_window,
                                        arg_title,
                                        g_variant_builder_end (&options),
                                        NULL,
                                        save_file_done,
                                        g_object_ref (request));

  xdp_file_chooser_complete_open_file (object, invocation, request->id);

  return TRUE;
}

static void
file_chooser_iface_init (XdpFileChooserIface *iface)
{
  iface->handle_open_file = handle_open_file;
  iface->handle_open_files = handle_open_files;
  iface->handle_save_file = handle_save_file;
}

static void
file_chooser_init (FileChooser *fc)
{
}

static void
file_chooser_class_init (FileChooserClass *klass)
{
}

GDBusInterfaceSkeleton *
file_chooser_create (GDBusConnection *connection,
                     const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_file_chooser_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               dbus_name,
                                               "/org/freedesktop/portal/desktop",
                                               NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create file chooser proxy: %s\n", error->message);
      return NULL;
    }

  file_chooser = g_object_new (file_chooser_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (file_chooser);
}
