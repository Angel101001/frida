/*
 * Copyright (C) 2010-2014 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#ifndef __GUM_SCRIPT_H__
#define __GUM_SCRIPT_H__

#include <glib-object.h>
#include <gum/gumdefs.h>
#include <gum/guminvocationcontext.h>
#include <gum/gumstalker.h>

#define GUM_TYPE_SCRIPT (gum_script_get_type ())
#define GUM_SCRIPT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj),\
    GUM_TYPE_SCRIPT, GumScript))
#define GUM_SCRIPT_CAST(obj) ((GumScript *) (obj))
#define GUM_SCRIPT_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass),\
    GUM_TYPE_SCRIPT, GumScriptClass))
#define GUM_IS_SCRIPT(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj),\
    GUM_TYPE_SCRIPT))
#define GUM_IS_SCRIPT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE (\
    (klass), GUM_TYPE_SCRIPT))
#define GUM_SCRIPT_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS (\
    (obj), GUM_TYPE_SCRIPT, GumScriptClass))

G_BEGIN_DECLS

typedef struct _GumScript           GumScript;
typedef struct _GumScriptClass      GumScriptClass;
typedef struct _GumScriptPrivate    GumScriptPrivate;

typedef void (* GumScriptMessageHandler) (GumScript * script,
    const gchar * message, const guint8 * data, gint data_length,
    gpointer user_data);
typedef void (* GumScriptDebugMessageHandler) (const gchar * message,
    gpointer user_data);

struct _GumScript
{
  GObject parent;

  GumScriptPrivate * priv;
};

struct _GumScriptClass
{
  GObjectClass parent_class;
};

GUM_API GType gum_script_get_type (void) G_GNUC_CONST;

GUM_API GumScript * gum_script_from_string (const gchar * source,
    GError ** error);

GUM_API GumStalker * gum_script_get_stalker (GumScript * self);

GUM_API void gum_script_set_message_handler (GumScript * self,
    GumScriptMessageHandler func, gpointer data, GDestroyNotify notify);

GUM_API void gum_script_load (GumScript * self);
GUM_API void gum_script_unload (GumScript * self);

GUM_API void gum_script_post_message (GumScript * self, const gchar * message);

GUM_API void gum_script_set_debug_message_handler (
    GumScriptDebugMessageHandler func, gpointer data, GDestroyNotify notify);
GUM_API void gum_script_post_debug_message (const gchar * message);

GUM_API void gum_script_ignore (GumThreadId thread_id);
GUM_API void gum_script_unignore (GumThreadId thread_id);
GUM_API gboolean gum_script_is_ignoring (GumThreadId thread_id);

G_END_DECLS

#endif
