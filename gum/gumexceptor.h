/*
 * Copyright (C) 2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#ifndef __GUM_EXCEPTOR_H__
#define __GUM_EXCEPTOR_H__

#include <glib-object.h>
#include <gum/gummemory.h>

#define GUM_TYPE_EXCEPTOR (gum_exceptor_get_type ())
#define GUM_EXCEPTOR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj),\
    GUM_TYPE_EXCEPTOR, GumExceptor))
#define GUM_EXCEPTOR_CAST(obj) ((GumExceptor *) (obj))
#define GUM_EXCEPTOR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass),\
    GUM_TYPE_EXCEPTOR, GumExceptorClass))
#define GUM_IS_EXCEPTOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj),\
    GUM_TYPE_EXCEPTOR))
#define GUM_IS_EXCEPTOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE (\
    (klass), GUM_TYPE_EXCEPTOR))
#define GUM_EXCEPTOR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS (\
    (obj), GUM_TYPE_EXCEPTOR, GumExceptorClass))

G_BEGIN_DECLS

typedef struct _GumExceptor GumExceptor;
typedef struct _GumExceptorClass GumExceptorClass;
typedef struct _GumExceptorPrivate GumExceptorPrivate;

typedef struct _GumExceptionDetails GumExceptionDetails;
typedef guint GumExceptionType;
typedef struct _GumExceptionMemoryAccessDetails GumExceptionMemoryAccessDetails;
typedef gboolean (* GumExceptionHandler) (GumExceptionDetails * details,
    gpointer user_data);

typedef struct _GumExceptorScope GumExceptorScope;
typedef struct _GumExceptorScopeImpl GumExceptorScopeImpl;
typedef gpointer GumExceptorJmpBuf;
typedef gint (* GumExceptorSetJmp) (GumExceptorJmpBuf buf, gboolean save_mask);

struct _GumExceptor
{
  GObject parent;

  GumExceptorPrivate * priv;
};

struct _GumExceptorClass
{
  GObjectClass parent_class;
};

enum _GumExceptionType
{
  GUM_EXCEPTION_ACCESS_VIOLATION,
  GUM_EXCEPTION_GUARD_VIOLATION,
  GUM_EXCEPTION_ILLEGAL_INSTRUCTION,
  GUM_EXCEPTION_STACK_OVERFLOW,
  GUM_EXCEPTION_ARITHMETIC,
  GUM_EXCEPTION_BREAKPOINT,
  GUM_EXCEPTION_SINGLE_STEP,
};

struct _GumExceptionMemoryAccessDetails
{
  GumMemoryOperation operation;
  gpointer address;
};

struct _GumExceptionDetails
{
  GumExceptionType type;
  gpointer address;
  GumExceptionMemoryAccessDetails memory_access;
  GumCpuContext cpu_context;
};

struct _GumExceptorScope
{
  GumExceptionDetails exception;

  GumExceptorScopeImpl * impl;
};

GUM_API GType gum_exceptor_get_type (void) G_GNUC_CONST;

GUM_API GumExceptor * gum_exceptor_obtain (void);

GUM_API void gum_exceptor_add (GumExceptor * self, GumExceptionHandler func,
    gpointer user_data);
GUM_API void gum_exceptor_remove (GumExceptor * self, GumExceptionHandler func,
    gpointer user_data);

/*
 * The setjmp() API does not allow longjmp() to be called after the function
 * that called setjmp() returns. That's why we cannot hide all the gory details
 * behind our API and need this hack...
 */
#define gum_exceptor_try(self, scope) \
    _gum_exceptor_get_setjmp () ( \
        _gum_exceptor_prepare_try (self, scope), TRUE) == 0
GUM_API gboolean gum_exceptor_catch (GumExceptor * self,
    GumExceptorScope * scope);

GUM_API GumExceptorSetJmp _gum_exceptor_get_setjmp (void);
GUM_API GumExceptorJmpBuf _gum_exceptor_prepare_try (GumExceptor * self,
    GumExceptorScope * scope);

G_END_DECLS

#endif
