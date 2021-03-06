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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#define _GNU_SOURCE 1

#include "config.h"

#include "appchooser.h"
#include "request.h"
#include "utils.h"

#include <string.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include <glib/gi18n.h>

#include "xdg-desktop-portal-dbus.h"

#include "appchooserdialog.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

typedef struct {
  XdpImplAppChooser *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GtkWidget *dialog;

  char *chosen;
  int response;

} AppDialogHandle;

static void
app_dialog_handle_free (gpointer data)
{
  AppDialogHandle *handle = data;

  g_object_unref (handle->request);
  g_object_unref (handle->dialog);
  g_free (handle->chosen);

  g_free (handle);
}

static void
app_dialog_handle_close (AppDialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  app_dialog_handle_free (handle);
}

static void
send_response (AppDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "choice", g_variant_new_string (handle->chosen ? handle->chosen : ""));

  if (handle->request->exported)
    request_unexport (handle->request);

  xdp_impl_app_chooser_complete_choose_application (handle->impl,
                                                    handle->invocation,
                                                    handle->response,
                                                    g_variant_builder_end (&opt_builder));

  app_dialog_handle_close (handle);
}

static void
handle_app_chooser_done (GtkDialog *dialog,
                         GAppInfo *info,
                         gpointer data)
{
  AppDialogHandle *handle = data;

  if (info != NULL)
    {
      const char *desktop_id = g_app_info_get_id (info);
      handle->response = 0;
      handle->chosen = g_strndup (desktop_id, strlen (desktop_id) - strlen (".desktop"));
    }
  else
    {
      handle->response = 1;
      handle->chosen = NULL;
    }

  send_response (handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              AppDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_app_chooser_complete_choose_application (handle->impl,
                                                    handle->invocation,
                                                    2,
                                                    g_variant_builder_end (&opt_builder));
  app_dialog_handle_close (handle);

  if (handle->request->exported)
    request_unexport (handle->request);

  xdp_impl_request_complete_close (object, invocation);

  return TRUE;
}

static gboolean
handle_choose_application (XdpImplAppChooser *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_handle,
                           const char *arg_app_id,
                           const char *arg_parent_window,
                           const char **choices,
                           GVariant *arg_options)
{
  g_autoptr(Request) request = NULL;
  GtkWidget *dialog;
  GdkWindow *foreign_parent = NULL;
  AppDialogHandle *handle;
  const char *sender;
  const char *cancel_label;
  const char *accept_label;
  const char *title;
  const char *heading;
  const char *latest_chosen_id;
  gboolean modal;

  sender = g_dbus_method_invocation_get_sender (invocation);

  request = request_new (sender, arg_app_id, arg_handle);

  if (!g_variant_lookup (arg_options, "accept_label", "&s", &accept_label))
    accept_label = _("_Select");
  if (!g_variant_lookup (arg_options, "title", "&s", &title))
    title = _("Open With");
  if (!g_variant_lookup (arg_options, "heading", "&s", &heading))
    heading = _("Select application");
  if (!g_variant_lookup (arg_options, "last_choice", "&s", &latest_chosen_id))
    latest_chosen_id = NULL;
  if (!g_variant_lookup (arg_options, "modal", "b", &modal))
    modal = TRUE;

  cancel_label = _("_Cancel");

  dialog = GTK_WIDGET (app_chooser_dialog_new (choices, latest_chosen_id, cancel_label, accept_label, title, heading));

  gtk_window_set_modal (GTK_WINDOW (dialog), modal);

  handle = g_new0 (AppDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->dialog = g_object_ref (dialog);

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (handle_close), handle);

  g_signal_connect (dialog, "done",
                    G_CALLBACK (handle_app_chooser_done), handle);

#ifdef GDK_WINDOWING_X11
  if (g_str_has_prefix (arg_parent_window, "x11:"))
    {
      int xid;

      if (sscanf (arg_parent_window, "x11:%x", &xid) != 1)
        g_warning ("invalid xid");
      else
        foreign_parent = gdk_x11_window_foreign_new_for_display (gtk_widget_get_display (dialog), xid);
    }
#endif
  else
    g_warning ("Unhandled parent window type %s", arg_parent_window);

  gtk_widget_realize (dialog);

  if (foreign_parent)
    gdk_window_set_transient_for (gtk_widget_get_window (dialog), foreign_parent);

  gtk_window_present (GTK_WINDOW (dialog));

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  return TRUE;
}

gboolean
app_chooser_init (GDBusConnection *bus,
                  GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_app_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-choose-application", G_CALLBACK (handle_choose_application), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
