/*
 * Copyright (C) 2009-2013 Ole Andr� Vadla Ravn�s <ole.andre.ravnas@tillitech.com>
 * Copyright (C) 2010-2013 Karl Trygve Kalleberg <karltk@boblycat.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#define ENABLE_DEBUG 0

#include "gumstalker.h"

#include "gumx86writer.h"
#include "gummemory.h"
#include "gumx86relocator.h"
#include "gumspinlock.h"
#include "gumudis86.h"

#include <stdlib.h>
#include <string.h>
#ifdef G_OS_WIN32
#include "backend-windows/gumwinexceptionhook.h"

#define VC_EXTRALEAN
#include <windows.h>
#include <psapi.h>
#include <tchar.h>
#endif

#define GUM_STACK_SIZE_IN_PAGES              128
#define GUM_CODE_ALIGNMENT                     8
#define GUM_DATA_ALIGNMENT                     8
#define GUM_CODE_SLAB_SIZE_IN_PAGES        20000
#define GUM_MAPPING_SLAB_SIZE_IN_PAGES       200
#define GUM_EXEC_BLOCK_MIN_SIZE             1024
#define GUM_RED_ZONE_MAX_SIZE                128

typedef struct _GumInfectContext GumInfectContext;

typedef struct _GumCallProbe GumCallProbe;
typedef struct _GumSlab GumSlab;

typedef struct _GumExecFrame GumExecFrame;
typedef struct _GumExecCtx GumExecCtx;
typedef struct _GumExecBlock GumExecBlock;

typedef guint GumPrologType;
typedef struct _GumGeneratorContext GumGeneratorContext;
typedef struct _GumAddressMapping GumAddressMapping;
typedef struct _GumInstruction GumInstruction;
typedef struct _GumBranchTarget GumBranchTarget;

typedef guint GumVirtualizationRequirements;

struct _GumStalkerPrivate
{
  guint page_size;

  GMutex * mutex;
  GSList * contexts;
  GPrivate * exec_ctx;

  GArray * exclusions;
  gint trust_threshold;
  volatile gboolean any_probes_attached;
  volatile gint last_probe_id;
  GumSpinlock probe_lock;
  GHashTable * probe_target_by_id;
  GHashTable * probe_array_by_address;

#ifdef G_OS_WIN32
  gpointer user32_start, user32_end;
  gpointer ki_user_callback_dispatcher_impl;
#endif
};

struct _GumInfectContext
{
  GumStalker * stalker;
  GumEventSink * sink;
};

struct _GumCallProbe
{
  GumProbeId id;
  GumCallProbeCallback callback;
  gpointer user_data;
  GDestroyNotify user_notify;
};

struct _GumSlab
{
  guint8 * data;
  guint offset;
  guint size;
  GumSlab * next; /* TODO: implement worker adding a new slab when needed */
};

struct _GumExecFrame
{
  gpointer real_address;
  gpointer code_address;
};

enum _GumExecCtxState
{
  GUM_EXEC_CTX_ACTIVE,
  GUM_EXEC_CTX_UNFOLLOW_PENDING,
  GUM_EXEC_CTX_DESTROY_PENDING
};

struct _GumExecCtx
{
  volatile guint state;
  volatile gboolean invalidate_pending;

  GumStalker * stalker;
  GumThreadId thread_id;

  GumX86Writer code_writer;
  GumX86Relocator relocator;

  GumEventSink * sink;
  GumEventType sink_mask;
  gpointer sink_process_impl; /* cached */
  GumEvent tmp_event;

  gboolean unfollow_called_while_still_following;
  GumExecBlock * current_block;
  GumExecFrame * current_frame;
  GumExecFrame * first_frame;
  GumExecFrame * frames;

  gpointer resume_at;
  gpointer return_at;
  gpointer app_stack;

  gpointer thunks;
  gpointer infect_thunk;

  GumSlab code_slab;
  GumSlab mapping_slab;
};

struct _GumAddressMapping
{
  gpointer real_address;
  gpointer code_address;
  GumExecBlock * block;
};

struct _GumExecBlock
{
  GumExecCtx * ctx;
  GumSlab * slab;

  guint8 * real_begin;
  guint8 * real_end;
  guint8 * real_snapshot;
  guint8 * code_begin;
  guint8 * code_end;

  guint8 state;
  guint recycle_count;
  gboolean has_call_to_excluded_range;

#ifdef G_OS_WIN32
  DWORD previous_dr0;
  DWORD previous_dr1;
  DWORD previous_dr2;
  DWORD previous_dr7;
#endif
};

enum _GumExecState
{
  GUM_EXEC_NORMAL,
  GUM_EXEC_SINGLE_STEPPING_ON_CALL,
  GUM_EXEC_SINGLE_STEPPING_THROUGH_CALL
};

enum _GumPrologType
{
  GUM_PROLOG_NONE,
  GUM_PROLOG_MINIMAL,
  GUM_PROLOG_FULL
};

struct _GumGeneratorContext
{
  GumInstruction * instruction;
  GumX86Relocator * relocator;
  GumX86Writer * code_writer;
  gpointer continuation_real_address;
  GumPrologType opened_prolog;
  guint state_preserve_stack_offset;
  guint accumulated_stack_delta;
};

struct _GumInstruction
{
  ud_t * ud;
  guint8 * begin;
  guint8 * end;
};

struct _GumBranchTarget
{
  gpointer origin_ip;

  gpointer absolute_address;
  gssize relative_offset;

  gboolean is_indirect;
  uint8_t pfx_seg;
  enum ud_type base;
  enum ud_type index;
  guint8 scale;
};

enum _GumVirtualizationRequirements
{
  GUM_REQUIRE_NOTHING         = 0,

  GUM_REQUIRE_MAPPING         = 1 << 0,
  GUM_REQUIRE_RELOCATION      = 1 << 1,
  GUM_REQUIRE_SINGLE_STEP     = 1 << 2
};

#define GUM_STALKER_LOCK(o) g_mutex_lock ((o)->priv->mutex)
#define GUM_STALKER_UNLOCK(o) g_mutex_unlock ((o)->priv->mutex)

#if GLIB_SIZEOF_VOID_P == 4
#define STATE_PRESERVE_SIZE (5 * sizeof (gpointer))
#define STATE_PRESERVE_TOPMOST_REGISTER_INDEX (3)
#else
#define STATE_PRESERVE_SIZE (11 * sizeof (gpointer))
#define STATE_PRESERVE_TOPMOST_REGISTER_INDEX (9)
#endif
#define GUM_THUNK_ARGLIST_STACK_RESERVE 64 /* x64 ABI compatibility */

static void gum_stalker_finalize (GObject * object);

void _gum_stalker_do_follow_me (GumStalker * self, GumEventSink * sink,
    volatile gpointer * ret_addr_ptr);
static void gum_stalker_infect (GumThreadId thread_id,
    GumCpuContext * cpu_context, gpointer user_data);

static void gum_stalker_free_probe_array (gpointer data);

static GumExecCtx * gum_stalker_create_exec_ctx (GumStalker * self,
    GumThreadId thread_id, GumEventSink * sink);
static GumExecCtx * gum_stalker_get_exec_ctx (GumStalker * self);
static void gum_stalker_invalidate_caches (GumStalker * self);

static void gum_exec_ctx_free (GumExecCtx * ctx);
static void gum_exec_ctx_add_code_slab_if_neeed (GumExecCtx * ctx);
static gpointer GUM_THUNK gum_exec_ctx_replace_current_block_with (
    GumExecCtx * ctx, gpointer start_address);
static void gum_exec_ctx_create_thunks (GumExecCtx * ctx);
static void gum_exec_ctx_destroy_thunks (GumExecCtx * ctx);

static GumExecBlock * gum_exec_ctx_obtain_block_for (GumExecCtx * ctx,
    gpointer real_address, gpointer * code_address);
static void gum_exec_ctx_clear_address_mappings (GumExecCtx * ctx);
static void gum_exec_ctx_add_address_mapping (GumExecCtx * ctx,
    gpointer real_address, gpointer code_address, GumExecBlock * block);
static void gum_exec_ctx_remove_address_mapping (GumExecCtx * ctx,
    gpointer real_address);
static void gum_exec_ctx_write_prolog (GumExecCtx * ctx, GumPrologType type,
    gpointer ip, GumX86Writer * cw);
static void gum_exec_ctx_write_epilog (GumExecCtx * ctx, GumPrologType type,
    GumX86Writer * cw);
static void gum_exec_ctx_write_push_branch_target_address (GumExecCtx * ctx,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_into (GumExecCtx * ctx,
    GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);

static int gum_address_mapping_compare (const void * a, const void * b);

static GumExecBlock * gum_exec_block_new (GumExecCtx * ctx);
static GumExecBlock * gum_exec_block_obtain (GumExecCtx * ctx,
    gpointer real_address, gpointer * code_address);
static gboolean gum_exec_block_is_full (GumExecBlock * block);
static void gum_exec_block_commit (GumExecBlock * block);

static void gum_exec_block_backpatch_call (GumExecBlock * block,
    gpointer code_start, GumPrologType opened_prolog, gpointer target_address,
    gpointer ret_real_address, gpointer ret_code_address);
static void gum_exec_block_backpatch_jmp (GumExecBlock * block,
    gpointer code_start, GumPrologType opened_prolog, gpointer target_address);
static void gum_exec_block_backpatch_ret (GumExecBlock * block,
    gpointer code_start, gpointer target_address);

static GumVirtualizationRequirements gum_exec_block_virtualize_branch_insn (
    GumExecBlock * block, GumGeneratorContext * gc);
static GumVirtualizationRequirements gum_exec_block_virtualize_ret_insn (
    GumExecBlock * block, GumGeneratorContext * gc);
static GumVirtualizationRequirements gum_exec_block_virtualize_sysenter_insn (
    GumExecBlock * block, GumGeneratorContext * gc);

static void gum_exec_block_write_call_invoke_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_block_write_jmp_transfer_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_block_write_ret_transfer_code (GumExecBlock * block,
    GumGeneratorContext * gc);
static void gum_exec_block_write_single_step_transfer_code (
    GumExecBlock * block, GumGeneratorContext * gc);

static void gum_exec_block_write_call_event_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_block_write_ret_event_code (GumExecBlock * block,
    GumGeneratorContext * gc);
static void gum_exec_block_write_exec_event_code (GumExecBlock * block,
    GumGeneratorContext * gc);
static void gum_exec_block_write_event_init_code (GumExecBlock * block,
    GumEventType type, GumGeneratorContext * gc);
static void gum_exec_block_write_event_submit_code (GumExecBlock * block,
    GumGeneratorContext * gc);

static void gum_exec_block_write_call_probe_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc);

static void gum_exec_block_open_prolog (GumExecBlock * block,
    GumPrologType type, GumGeneratorContext * gc);
static void gum_exec_block_close_prolog (GumExecBlock * block,
    GumGeneratorContext * gc);

static void gum_write_segment_prefix (uint8_t segment, GumX86Writer * cw);

static GumCpuReg gum_cpu_meta_reg_from_real_reg (GumCpuReg reg);
static GumCpuReg gum_cpu_reg_from_ud (enum ud_type reg);

static void gum_private_set (GPrivate * private_key, gpointer data);

#ifdef G_OS_WIN32
static gboolean gum_stalker_handle_exception (
    EXCEPTION_RECORD * exception_record, CONTEXT * context,
    gpointer user_data);
#endif

G_DEFINE_TYPE (GumStalker, gum_stalker, G_TYPE_OBJECT);

static void
gum_stalker_class_init (GumStalkerClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GumStalkerPrivate));

  object_class->finalize = gum_stalker_finalize;
}

static void
gum_stalker_init (GumStalker * self)
{
  GumStalkerPrivate * priv;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GUM_TYPE_STALKER, GumStalkerPrivate);
  priv = self->priv;

  priv->exclusions = g_array_new (FALSE, FALSE, sizeof (GumMemoryRange));
  priv->trust_threshold = 1;

  gum_spinlock_init (&priv->probe_lock);
  priv->probe_target_by_id =
      g_hash_table_new_full (NULL, NULL, NULL, NULL);
  priv->probe_array_by_address =
      g_hash_table_new_full (NULL, NULL, NULL, gum_stalker_free_probe_array);

#if defined (G_OS_WIN32) && GLIB_SIZEOF_VOID_P == 4
  gum_win_exception_hook_add (gum_stalker_handle_exception, self);

  {
    HMODULE ntmod, usermod;
    MODULEINFO mi;
    BOOL success;
    gboolean found_user32_code = FALSE;
    guint8 * p;

    ntmod = GetModuleHandle (_T ("ntdll.dll"));
    usermod = GetModuleHandle (_T ("user32.dll"));
    g_assert (ntmod != NULL && usermod != NULL);

    success = GetModuleInformation (GetCurrentProcess (), usermod,
        &mi, sizeof (mi));
    g_assert (success);
    priv->user32_start = mi.lpBaseOfDll;
    priv->user32_end = (guint8 *) mi.lpBaseOfDll + mi.SizeOfImage;

    for (p = (guint8 *) priv->user32_start; p < (guint8 *) priv->user32_end;)
    {
      MEMORY_BASIC_INFORMATION mbi;

      success = VirtualQuery (p, &mbi, sizeof (mbi)) == sizeof (mbi);
      g_assert (success);

      if (mbi.Protect == PAGE_EXECUTE_READ ||
          mbi.Protect == PAGE_EXECUTE_READWRITE ||
          mbi.Protect == PAGE_EXECUTE_WRITECOPY)
      {
        priv->user32_start = mbi.BaseAddress;
        priv->user32_end = (guint8 *) mbi.BaseAddress + mbi.RegionSize;

        found_user32_code = TRUE;
      }

      p = (guint8 *) mbi.BaseAddress + mbi.RegionSize;
    }

    g_assert (found_user32_code);

    priv->ki_user_callback_dispatcher_impl = GUM_FUNCPTR_TO_POINTER (
        GetProcAddress (ntmod, "KiUserCallbackDispatcher"));
    g_assert (priv->ki_user_callback_dispatcher_impl != NULL);
  }
#endif

  priv->page_size = gum_query_page_size ();
  priv->mutex = g_mutex_new ();
  priv->contexts = NULL;
  priv->exec_ctx = g_private_new (NULL);
}

static void
gum_stalker_finalize (GObject * object)
{
  GumStalker * self = GUM_STALKER (object);
  GumStalkerPrivate * priv = self->priv;

  gum_stalker_garbage_collect (self);

#if defined (G_OS_WIN32) && GLIB_SIZEOF_VOID_P == 4
  gum_win_exception_hook_remove (gum_stalker_handle_exception);
#endif

  g_hash_table_unref (priv->probe_array_by_address);
  g_hash_table_unref (priv->probe_target_by_id);

  gum_spinlock_free (&priv->probe_lock);

  g_array_free (priv->exclusions, TRUE);

  g_assert (priv->contexts == NULL);
  g_mutex_free (priv->mutex);

  G_OBJECT_CLASS (gum_stalker_parent_class)->finalize (object);
}

GumStalker *
gum_stalker_new (void)
{
  return GUM_STALKER (g_object_new (GUM_TYPE_STALKER, NULL));
}

void
gum_stalker_exclude (GumStalker * self,
                     const GumMemoryRange * range)
{
  g_array_append_val (self->priv->exclusions, *range);
}

gint
gum_stalker_get_trust_threshold (GumStalker * self)
{
  return self->priv->trust_threshold;
}

void
gum_stalker_set_trust_threshold (GumStalker * self,
                                 gint trust_threshold)
{
  self->priv->trust_threshold = trust_threshold;
}

void
gum_stalker_garbage_collect (GumStalker * self)
{
  GSList * keep = NULL, * cur;

  GUM_STALKER_LOCK (self);

  for (cur = self->priv->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * ctx = (GumExecCtx *) cur->data;
    if (ctx->state == GUM_EXEC_CTX_DESTROY_PENDING)
      gum_exec_ctx_free (ctx);
    else
      keep = g_slist_prepend (keep, ctx);
  }

  g_slist_free (self->priv->contexts);
  self->priv->contexts = keep;

  GUM_STALKER_UNLOCK (self);
}

#ifdef _MSC_VER

#define RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT(arg)   \
    ((gpointer *) ((volatile guint8 *) &arg - sizeof (gpointer)))

void
gum_stalker_follow_me (GumStalker * self,
                       GumEventSink * sink)
{
  volatile gpointer * ret_addr_ptr;

  ret_addr_ptr = RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT (self);

  _gum_stalker_do_follow_me (self, sink, ret_addr_ptr);
}

#endif

void
_gum_stalker_do_follow_me (GumStalker * self,
                           GumEventSink * sink,
                           volatile gpointer * ret_addr_ptr)
{
  GumExecCtx * ctx;
  gpointer code_address;

  ctx = gum_stalker_create_exec_ctx (self,
      gum_process_get_current_thread_id (), sink);
  g_private_set (self->priv->exec_ctx, ctx);
  ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, *ret_addr_ptr,
      &code_address);
  *ret_addr_ptr = code_address;
}

void
gum_stalker_unfollow_me (GumStalker * self)
{
  GumExecCtx * ctx;

  ctx = gum_stalker_get_exec_ctx (self);
  g_assert (ctx != NULL);

  if (ctx->current_block != NULL &&
      ctx->current_block->has_call_to_excluded_range)
  {
    ctx->state = GUM_EXEC_CTX_UNFOLLOW_PENDING;
  }
  else
  {
    g_assert (ctx->unfollow_called_while_still_following);

    g_private_set (self->priv->exec_ctx, NULL);

    GUM_STALKER_LOCK (self);
    self->priv->contexts = g_slist_remove (self->priv->contexts, ctx);
    GUM_STALKER_UNLOCK (self);

    gum_exec_ctx_free (ctx);
  }
}

gboolean
gum_stalker_is_following_me (GumStalker * self)
{
  return gum_stalker_get_exec_ctx (self) != NULL;
}

void
gum_stalker_follow (GumStalker * self,
                    GumThreadId thread_id,
                    GumEventSink * sink)
{
  if (thread_id == gum_process_get_current_thread_id ())
  {
    gum_stalker_follow_me (self, sink);
  }
  else
  {
    GumInfectContext ctx;
    ctx.stalker = self;
    ctx.sink = sink;
    gum_process_modify_thread (thread_id, gum_stalker_infect, &ctx);
  }
}

void
gum_stalker_unfollow (GumStalker * self,
                      GumThreadId thread_id)
{
  if (thread_id == gum_process_get_current_thread_id ())
  {
    gum_stalker_unfollow_me (self);
  }
  else
  {
    GSList * cur;

    GUM_STALKER_LOCK (self);

    for (cur = self->priv->contexts; cur != NULL; cur = cur->next)
    {
      GumExecCtx * ctx = (GumExecCtx *) cur->data;
      if (ctx->thread_id == thread_id && ctx->state == GUM_EXEC_CTX_ACTIVE)
      {
        ctx->state = GUM_EXEC_CTX_UNFOLLOW_PENDING;
        break;
      }
    }

    GUM_STALKER_UNLOCK (self);
  }
}

static void
gum_stalker_infect (GumThreadId thread_id,
                    GumCpuContext * cpu_context,
                    gpointer user_data)
{
  GumInfectContext * infect_context = (GumInfectContext *) user_data;
  GumStalker * self = infect_context->stalker;
  GumExecCtx * ctx;
  gpointer code_address;
  GumX86Writer cw;
#if GLIB_SIZEOF_VOID_P == 4
  guint align_correction = 8;
#else
  guint align_correction = 0;
#endif

  ctx = gum_stalker_create_exec_ctx (self, thread_id, infect_context->sink);

  ctx->current_block = gum_exec_ctx_obtain_block_for (ctx,
      GSIZE_TO_POINTER (GUM_CPU_CONTEXT_XIP (cpu_context)), &code_address);
  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (ctx->infect_thunk);

  gum_x86_writer_init (&cw, ctx->infect_thunk);
  gum_exec_ctx_write_prolog (ctx, GUM_PROLOG_MINIMAL,
      ctx->current_block->real_begin, &cw);
  gum_x86_writer_put_sub_reg_imm (&cw, GUM_REG_XSP, align_correction);
  gum_x86_writer_put_call_with_arguments (&cw,
      GUM_FUNCPTR_TO_POINTER (gum_private_set), 2,
      GUM_ARG_POINTER, self->priv->exec_ctx,
      GUM_ARG_POINTER, ctx);
  gum_x86_writer_put_add_reg_imm (&cw, GUM_REG_XSP, align_correction);
  gum_exec_ctx_write_epilog (ctx, GUM_PROLOG_MINIMAL, &cw);
  gum_x86_writer_put_jmp (&cw, code_address);
  gum_x86_writer_free (&cw);
}

GumProbeId
gum_stalker_add_call_probe (GumStalker * self,
                            gpointer target_address,
                            GumCallProbeCallback callback,
                            gpointer data,
                            GDestroyNotify notify)
{
  GumStalkerPrivate * priv = self->priv;
  GumCallProbe probe;
  GArray * probes;

  probe.id = g_atomic_int_exchange_and_add (&priv->last_probe_id, 1) + 1;
  probe.callback = callback;
  probe.user_data = data;
  probe.user_notify = notify;

  gum_spinlock_acquire (&priv->probe_lock);

  g_hash_table_insert (priv->probe_target_by_id, GSIZE_TO_POINTER (probe.id),
      target_address);

  probes = (GArray *)
      g_hash_table_lookup (priv->probe_array_by_address, target_address);
  if (probes == NULL)
  {
    probes = g_array_sized_new (FALSE, FALSE, sizeof (GumCallProbe), 4);
    g_hash_table_insert (priv->probe_array_by_address, target_address, probes);
  }

  g_array_append_val (probes, probe);

  priv->any_probes_attached = TRUE;

  gum_spinlock_release (&priv->probe_lock);

  gum_stalker_invalidate_caches (self);

  return probe.id;
}

void
gum_stalker_remove_call_probe (GumStalker * self,
                               GumProbeId id)
{
  GumStalkerPrivate * priv = self->priv;
  gpointer target_address;

  gum_spinlock_acquire (&priv->probe_lock);

  target_address =
      g_hash_table_lookup (priv->probe_target_by_id, GSIZE_TO_POINTER (id));
  if (target_address != NULL)
  {
    GArray * probes;
    gint match_index = -1;
    guint i;
    GumCallProbe * probe;

    g_hash_table_remove (priv->probe_target_by_id, GSIZE_TO_POINTER (id));

    probes = (GArray *)
        g_hash_table_lookup (priv->probe_array_by_address, target_address);
    g_assert (probes != NULL);

    for (i = 0; i != probes->len; i++)
    {
      if (g_array_index (probes, GumCallProbe, i).id == id)
      {
        match_index = i;
        break;
      }
    }
    g_assert_cmpint (match_index, !=, -1);

    probe = &g_array_index (probes, GumCallProbe, match_index);
    if (probe->user_notify != NULL)
      probe->user_notify (probe->user_data);
    g_array_remove_index (probes, match_index);

    if (probes->len == 0)
      g_hash_table_remove (priv->probe_array_by_address, target_address);

    priv->any_probes_attached =
        g_hash_table_size (priv->probe_array_by_address) != 0;
  }

  gum_spinlock_release (&priv->probe_lock);

  gum_stalker_invalidate_caches (self);
}

static void
gum_stalker_free_probe_array (gpointer data)
{
  GArray * probes = (GArray *) data;
  guint i;

  for (i = 0; i != probes->len; i++)
  {
    GumCallProbe * probe = &g_array_index (probes, GumCallProbe, i);
    if (probe->user_notify != NULL)
      probe->user_notify (probe->user_data);
  }

  g_array_free (probes, TRUE);
}

static GumExecCtx *
gum_stalker_create_exec_ctx (GumStalker * self,
                             GumThreadId thread_id,
                             GumEventSink * sink)
{
  GumStalkerPrivate * priv = self->priv;
  guint base_size;
  GumExecCtx * ctx;

  base_size = sizeof (GumExecCtx) / priv->page_size;
  if (sizeof (GumExecCtx) % priv->page_size != 0)
    base_size++;

  ctx = (GumExecCtx *)
      gum_alloc_n_pages (base_size + GUM_CODE_SLAB_SIZE_IN_PAGES +
          GUM_MAPPING_SLAB_SIZE_IN_PAGES + 1, GUM_PAGE_RWX);
  ctx->state = GUM_EXEC_CTX_ACTIVE;
  ctx->invalidate_pending = FALSE;

  ctx->code_slab.data = ((guint8 *) ctx) + (base_size * priv->page_size);
  ctx->code_slab.offset = 0;
  ctx->code_slab.size = GUM_CODE_SLAB_SIZE_IN_PAGES * priv->page_size;
  ctx->code_slab.next = NULL;

  ctx->mapping_slab.data = ctx->code_slab.data + ctx->code_slab.size;
  ctx->mapping_slab.offset = 0;
  ctx->mapping_slab.size = GUM_MAPPING_SLAB_SIZE_IN_PAGES * priv->page_size;
  ctx->mapping_slab.next = NULL;

  ctx->frames = (GumExecFrame *)
      ctx->mapping_slab.data + ctx->mapping_slab.size;
  ctx->first_frame = (GumExecFrame *) (ctx->mapping_slab.data +
      ctx->mapping_slab.size + priv->page_size - sizeof (GumExecFrame));
  ctx->current_frame = ctx->first_frame;

  ctx->stalker = g_object_ref (self);
  ctx->thread_id = thread_id;

  gum_x86_writer_init (&ctx->code_writer, NULL);
  gum_x86_relocator_init (&ctx->relocator, NULL, &ctx->code_writer);

  ctx->sink = (GumEventSink *) g_object_ref (sink);
  ctx->sink_mask = gum_event_sink_query_mask (sink);
  ctx->sink_process_impl = GUM_FUNCPTR_TO_POINTER (
      GUM_EVENT_SINK_GET_INTERFACE (sink)->process);

  gum_exec_ctx_create_thunks (ctx);

  GUM_STALKER_LOCK (self);
  self->priv->contexts = g_slist_prepend (self->priv->contexts, ctx);
  GUM_STALKER_UNLOCK (self);

  gum_event_sink_start (sink);

  return ctx;
}

static GumExecCtx *
gum_stalker_get_exec_ctx (GumStalker * self)
{
  return (GumExecCtx *) g_private_get (self->priv->exec_ctx);
}

static void
gum_stalker_invalidate_caches (GumStalker * self)
{
  GSList * cur;

  GUM_STALKER_LOCK (self);

  for (cur = self->priv->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * ctx = (GumExecCtx *) cur->data;

    gum_exec_ctx_add_code_slab_if_neeed (ctx);

    ctx->invalidate_pending = TRUE;
  }

  GUM_STALKER_UNLOCK (self);
}

static void
gum_exec_ctx_free (GumExecCtx * ctx)
{
  GumSlab * slab;

  gum_event_sink_stop (ctx->sink);

  slab = ctx->code_slab.next;
  while (slab != NULL)
  {
    GumSlab * next = slab->next;
    gum_free_pages (slab);
    slab = next;
  }

  gum_exec_ctx_destroy_thunks (ctx);

  g_object_unref (ctx->sink);

  gum_x86_relocator_free (&ctx->relocator);
  gum_x86_writer_free (&ctx->code_writer);

  if (ctx->stalker != NULL)
    g_object_unref (ctx->stalker);

  gum_free_pages (ctx);
}

static void
gum_exec_ctx_add_code_slab_if_neeed (GumExecCtx * ctx)
{
  GumSlab * slab;

  slab = &ctx->code_slab;
  while (slab->next != NULL)
    slab = slab->next;
  if (slab->size - slab->offset > slab->size / 2)
  {
    GumSlab * s;

    s = gum_alloc_n_pages (GUM_CODE_SLAB_SIZE_IN_PAGES, GUM_PAGE_RWX);
    s->data = (guint8 *) (s + 1);
    s->offset = 0;
    s->size = (GUM_CODE_SLAB_SIZE_IN_PAGES * ctx->stalker->priv->page_size)
        - sizeof (GumSlab);
    s->next = NULL;

    slab->next = s;
  }
}

static gpointer GUM_THUNK
gum_exec_ctx_replace_current_block_with (GumExecCtx * ctx,
                                         gpointer start_address)
{
  if (ctx->invalidate_pending)
  {
    gum_exec_ctx_clear_address_mappings (ctx);

    ctx->invalidate_pending = FALSE;
  }

  ctx->current_block = NULL;

  if (start_address == gum_stalker_unfollow_me)
  {
    ctx->unfollow_called_while_still_following = TRUE;
    ctx->resume_at = start_address;
  }
  else if (ctx->state == GUM_EXEC_CTX_UNFOLLOW_PENDING)
  {
    GumStalker * stalker;

    ctx->resume_at = start_address;

    g_private_set (ctx->stalker->priv->exec_ctx, NULL);
    stalker = ctx->stalker;
    ctx->stalker = NULL;
    ctx->state = GUM_EXEC_CTX_DESTROY_PENDING;

    g_object_unref (stalker);
  }
  else
  {
    ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, start_address,
        &ctx->resume_at);
  }

  return ctx->resume_at;
}

static void
gum_exec_ctx_create_thunks (GumExecCtx * ctx)
{
  GumX86Writer cw;

  g_assert (ctx->thunks == NULL);

  ctx->thunks = gum_alloc_n_pages (1, GUM_PAGE_RWX);
  gum_x86_writer_init (&cw, ctx->thunks);

  ctx->infect_thunk = gum_x86_writer_cur (&cw);

  gum_x86_writer_free (&cw);
}

static void
gum_exec_ctx_destroy_thunks (GumExecCtx * ctx)
{
  gum_free_pages (ctx->thunks);
}

#if ENABLE_DEBUG

static void
gum_disasm (guint8 * code, guint size, const gchar * prefix)
{
  ud_t ud_obj;

  ud_init (&ud_obj);
  ud_set_input_buffer (&ud_obj, code, size);
  ud_set_mode (&ud_obj, GUM_CPU_MODE);
  ud_set_pc (&ud_obj, GPOINTER_TO_SIZE (code));
  ud_set_syntax (&ud_obj, UD_SYN_INTEL);

  while (ud_disassemble (&ud_obj))
  {
    printf ("%s0x%" G_GINT64_MODIFIER "x\t%s\n",
        prefix, (guint64) ud_insn_off (&ud_obj), ud_insn_asm (&ud_obj));
  }
}

static void
gum_hexdump (guint8 * data, guint size, const gchar * prefix)
{
  guint i, line_offset;

  line_offset = 0;
  for (i = 0; i != size; i++)
  {
    if (line_offset == 0)
      printf ("%s0x%" G_GINT64_MODIFIER "x\t%02x", prefix, (guint64) data + i,
          data[i]);
    else
      printf (" %02x", data[i]);

    line_offset++;
    if (line_offset == 16 && i != size - 1)
    {
      printf ("\n");
      line_offset = 0;
    }
  }

  if (line_offset != 0)
    printf ("\n");
}

#endif

static GumExecBlock *
gum_exec_ctx_obtain_block_for (GumExecCtx * ctx,
                               gpointer real_address,
                               gpointer * code_address)
{
  GumExecBlock * block;
  GumX86Writer * cw = &ctx->code_writer;
  GumX86Relocator * rl = &ctx->relocator;
  GumGeneratorContext gc;

  if (ctx->stalker->priv->trust_threshold >= 0)
  {
    block = gum_exec_block_obtain (ctx, real_address, code_address);
    if (block != NULL)
    {
      if (block->recycle_count >= ctx->stalker->priv->trust_threshold ||
          memcmp (real_address, block->real_snapshot,
            block->real_end - block->real_begin) == 0)
      {
        block->recycle_count++;
        return block;
      }
      else
      {
        gum_exec_ctx_remove_address_mapping (ctx, real_address);
      }
    }
  }

  block = gum_exec_block_new (ctx);
  *code_address = block->code_begin;
  gum_exec_ctx_add_address_mapping (ctx, real_address, block->code_begin,
      block);
  gum_x86_writer_reset (cw, block->code_begin);
  gum_x86_relocator_reset (rl, real_address, cw);

  gc.instruction = NULL;
  gc.relocator = rl;
  gc.code_writer = cw;
  gc.continuation_real_address = NULL;
  gc.opened_prolog = GUM_PROLOG_NONE;
  gc.state_preserve_stack_offset = 0;
  gc.accumulated_stack_delta = 0;

#if ENABLE_DEBUG
  printf ("\n\n***\n\nCreating block for %p:\n", real_address);
#endif

  while (TRUE)
  {
    guint n_read;
    GumInstruction insn;
    GumVirtualizationRequirements requirements = GUM_REQUIRE_NOTHING;

    n_read = gum_x86_relocator_read_one (rl, NULL);
    g_assert_cmpuint (n_read, !=, 0);

    insn.ud = gum_x86_relocator_peek_next_write_insn (rl);
    insn.begin = gum_x86_relocator_peek_next_write_source (rl);
    insn.end = insn.begin + ud_insn_len (insn.ud);

    g_assert (insn.ud != NULL && insn.begin != NULL);

#if ENABLE_DEBUG
    gum_disasm (insn.begin, insn.end - insn.begin, "");
    gum_hexdump (insn.begin, insn.end - insn.begin, "; ");
#endif

    gc.instruction = &insn;

    if ((ctx->sink_mask & GUM_EXEC) != 0)
      gum_exec_block_write_exec_event_code (block, &gc);

    switch (insn.ud->mnemonic)
    {
      case UD_Icall:
      case UD_Ijmp:
        requirements = gum_exec_block_virtualize_branch_insn (block, &gc);
        break;
      case UD_Iret:
        requirements = gum_exec_block_virtualize_ret_insn (block, &gc);
        break;
      case UD_Isysenter:
        requirements = gum_exec_block_virtualize_sysenter_insn (block, &gc);
        break;
      default:
        if (gum_mnemonic_is_jcc (insn.ud->mnemonic))
          requirements = gum_exec_block_virtualize_branch_insn (block, &gc);
        else
          requirements = GUM_REQUIRE_RELOCATION;
        break;
    }

    gum_exec_block_close_prolog (block, &gc);

    if ((requirements & GUM_REQUIRE_RELOCATION) != 0)
    {
      if ((requirements & GUM_REQUIRE_MAPPING) != 0)
      {
        gum_exec_ctx_add_address_mapping (ctx, insn.begin,
            gum_x86_writer_cur (cw), block);
      }

      gum_x86_relocator_write_one_no_label (rl);

      if ((requirements & GUM_REQUIRE_MAPPING) != 0)
      {
        gum_exec_ctx_add_address_mapping (ctx, insn.end,
            gum_x86_writer_cur (cw), block);
      }
    }
    else if ((requirements & GUM_REQUIRE_SINGLE_STEP) != 0)
    {
      gum_x86_relocator_skip_one_no_label (rl);
      gum_exec_block_write_single_step_transfer_code (block, &gc);
    }

#if ENABLE_DEBUG
    {
      guint8 * begin = block->code_end;
      block->code_end = gum_x86_writer_cur (cw);
      gum_disasm (begin, block->code_end - begin, "\t");
      gum_hexdump (begin, block->code_end - begin, "\t; ");
    }
#else
    block->code_end = gum_x86_writer_cur (cw);
#endif

    if (gum_exec_block_is_full (block))
    {
      gc.continuation_real_address = insn.end;
      break;
    }
    else if (insn.ud->mnemonic == UD_Icall)
    {
      /* We always stop on a call unless it's to an excluded range */
      if ((requirements & GUM_REQUIRE_RELOCATION) != 0)
      {
        rl->eob = FALSE;
      }
      else
      {
        break;
      }
    }
    else if (gum_x86_relocator_eob (rl))
    {
      break;
    }
  }

  if (gc.continuation_real_address != NULL)
  {
    GumBranchTarget continue_target = { 0, };

    continue_target.is_indirect = FALSE;
    continue_target.absolute_address = gc.continuation_real_address;

    gum_exec_block_write_jmp_transfer_code (block, &continue_target, &gc);
  }

  gum_x86_writer_put_int3 (cw); /* should never get here */

  gum_x86_writer_flush (cw);

  block->code_end = (guint8 *) gum_x86_writer_cur (cw);

  block->real_begin = (guint8 *) rl->input_start;
  block->real_end = (guint8 *) rl->input_cur;

  gum_exec_block_commit (block);

  return block;
}

static void
gum_exec_ctx_clear_address_mappings (GumExecCtx * ctx)
{
  GumSlab * slab;

  for (slab = &ctx->mapping_slab; slab != NULL; slab = slab->next)
  {
    slab->offset = 0;
  }
}

static void
gum_exec_ctx_add_address_mapping (GumExecCtx * ctx,
                                  gpointer real_address,
                                  gpointer code_address,
                                  GumExecBlock * block)
{
  GumSlab * slab;

  if (ctx->stalker->priv->trust_threshold < 0)
  {
    return;
  }

  for (slab = &ctx->mapping_slab; slab != NULL; slab = slab->next)
  {
    if (slab->size - slab->offset >= sizeof (GumAddressMapping))
    {
      GumAddressMapping * mapping, * cur;
      guint offset_from_end;

      mapping = NULL;

      for (cur = (GumAddressMapping *) (slab->data + slab->offset -
            sizeof (GumAddressMapping)), offset_from_end = 0;
          cur >= (GumAddressMapping *) slab->data;
          cur--, offset_from_end++)
      {
        if (real_address >= cur->real_address)
        {
          if (offset_from_end > 0)
          {
            mapping = cur + 1;
            memmove (cur + 2, cur + 1, offset_from_end * sizeof (GumAddressMapping));
          }
          break;
        }
        else if (cur == (GumAddressMapping *) slab->data)
        {
          mapping = cur;
          memmove (cur + 1, cur, (offset_from_end + 1) * sizeof (GumAddressMapping));
        }
      }

      if (mapping == NULL)
        mapping = (GumAddressMapping *) (slab->data + slab->offset);

      mapping->real_address = real_address;
      mapping->code_address = code_address;
      mapping->block = block;

      slab->offset += sizeof (GumAddressMapping);

      break;
    }
  }
}

static void
gum_exec_ctx_remove_address_mapping (GumExecCtx * ctx,
                                     gpointer real_address)
{
  GumAddressMapping mapping;
  GumSlab * slab;

  mapping.real_address = real_address;
  mapping.code_address = NULL;
  mapping.block = NULL;

  for (slab = &ctx->mapping_slab; slab != NULL; slab = slab->next)
  {
    GumAddressMapping * match;

    match = bsearch (&mapping, slab->data,
        slab->offset / sizeof (GumAddressMapping), sizeof (GumAddressMapping),
        gum_address_mapping_compare);
    if (match != NULL)
    {
      GumAddressMapping * last_mapping;

      last_mapping = (GumAddressMapping *) (slab->data + slab->offset -
          sizeof (GumAddressMapping));
      if (match != last_mapping)
      {
        memmove (match, match + 1, (slab->data + slab->offset) -
            ((guint8 *) (match + 1)));
      }
      slab->offset -= sizeof (GumAddressMapping);

      return;
    }
  }
}

static void
gum_exec_ctx_write_prolog (GumExecCtx * ctx,
                           GumPrologType type,
                           gpointer ip,
                           GumX86Writer * cw)
{
  guint8 fxsave[] = {
    0x0f, 0xae, 0x04, 0x24 /* fxsave [esp] */
  };

  gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (&ctx->app_stack),
      GUM_REG_XSP);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
      GUM_REG_XSP, -GUM_RED_ZONE_MAX_SIZE);

  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_cld (cw); /* C ABI mandates this */

  if (type == GUM_PROLOG_MINIMAL)
  {
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XBX);

#if GLIB_SIZEOF_VOID_P == 8
    gum_x86_writer_put_push_reg (cw, GUM_REG_XSI);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDI);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R8);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R9);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R10);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R11);
#endif
  }
  else /* GUM_PROLOG_FULL */
  {
    gum_x86_writer_put_pushax (cw); /* all of GumCpuContext except for xip */
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX, GUM_ADDRESS (ip));
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX); /* GumCpuContext.xip */

    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
        GUM_REG_XSP, GUM_CPU_CONTEXT_OFFSET_XSP,
        GUM_REG_XAX);
  }

  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XBX, GUM_REG_XSP);
  gum_x86_writer_put_and_reg_u32 (cw, GUM_REG_XSP, (guint32) ~(16 - 1));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP, 512);
  gum_x86_writer_put_bytes (cw, fxsave, sizeof (fxsave));
}

static void
gum_exec_ctx_write_epilog (GumExecCtx * ctx,
                           GumPrologType type,
                           GumX86Writer * cw)
{
  guint8 fxrstor[] = {
    0x0f, 0xae, 0x0c, 0x24 /* fxrstor [esp] */
  };

  gum_x86_writer_put_bytes (cw, fxrstor, sizeof (fxrstor));
  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XSP, GUM_REG_XBX);

  if (type == GUM_PROLOG_MINIMAL)
  {
#if GLIB_SIZEOF_VOID_P == 8
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R11);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R10);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R9);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R8);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDI);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XSI);
#endif

    gum_x86_writer_put_pop_reg (cw, GUM_REG_XBX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  }
  else /* GUM_PROLOG_FULL */
  {
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX); /* discard
                                                     GumCpuContext.xip */
    gum_x86_writer_put_popax (cw);
  }

  gum_x86_writer_put_popfx (cw);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XSP,
      GUM_ADDRESS (&ctx->app_stack));
}

static void
gum_exec_ctx_write_push_branch_target_address (GumExecCtx * ctx,
                                               const GumBranchTarget * target,
                                               GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  if (!target->is_indirect)
  {
    if (target->base == UD_NONE)
    {
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
          GUM_ADDRESS (target->absolute_address));
      gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);
    }
    else
    {
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XAX,
          gum_cpu_reg_from_ud (target->base),
          target->origin_ip,
          gc);
      gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);
    }
  }
  else if (target->base == UD_NONE && target->index == UD_NONE)
  {
    g_assert (target->scale == 0);
    g_assert (target->absolute_address != NULL);
    g_assert (target->relative_offset == 0);

    gum_write_segment_prefix (target->pfx_seg, cw);
    gum_x86_writer_put_byte (cw, 0xff);
    gum_x86_writer_put_byte (cw, 0x35);
    gum_x86_writer_put_bytes (cw, (guint8 *) &target->absolute_address,
        sizeof (target->absolute_address));
  }
  else
  {
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX); /* placeholder */

    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);

    gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XAX,
        gum_cpu_reg_from_ud (target->base),
        target->origin_ip,
        gc);
    gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XDX,
        gum_cpu_reg_from_ud (target->index),
        target->origin_ip,
        gc);
    gum_x86_writer_put_mov_reg_base_index_scale_offset_ptr (cw, GUM_REG_XAX,
        GUM_REG_XAX, GUM_REG_XDX, target->scale ? target->scale : 1,
        target->relative_offset);
    gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
        GUM_REG_XSP, 2 * sizeof (gpointer),
        GUM_REG_XAX);

    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  }
}

static void
gum_exec_ctx_load_real_register_into (GumExecCtx * ctx,
                                      GumCpuReg target_register,
                                      GumCpuReg source_register,
                                      gpointer ip,
                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  GumCpuReg source_meta;

  source_meta = gum_cpu_meta_reg_from_real_reg (source_register);

  if (source_meta >= GUM_REG_XAX && source_meta <= GUM_REG_XBX)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, gc->state_preserve_stack_offset +
        STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - GUM_REG_XAX) * sizeof (gpointer)));
  }
#if GLIB_SIZEOF_VOID_P == 8
  else if (source_meta >= GUM_REG_XSI && source_meta <= GUM_REG_XDI)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, gc->state_preserve_stack_offset +
        STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - 2 - GUM_REG_XAX) * sizeof (gpointer)));
  }
  else if (source_meta >= GUM_REG_R8 && source_meta <= GUM_REG_R11)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, gc->state_preserve_stack_offset +
        STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - 2 - GUM_REG_RAX) * sizeof (gpointer)));
  }
#endif
  else if (source_meta == GUM_REG_XSP)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, target_register,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_lea_reg_reg_offset (cw, target_register,
        target_register, gc->accumulated_stack_delta);
  }
  else if (source_meta == GUM_REG_XIP)
  {
    gum_x86_writer_put_mov_reg_address (cw, target_register,
        GUM_ADDRESS (ip));
  }
  else if (source_meta == GUM_REG_NONE)
  {
    gum_x86_writer_put_xor_reg_reg (cw, target_register, target_register);
  }
  else
  {
    gum_x86_writer_put_mov_reg_reg (cw, target_register, source_register);
  }
}

static int
gum_address_mapping_compare (const void * a,
                             const void * b)
{
  const GumAddressMapping * mapping_a = (const GumAddressMapping *) a;
  const GumAddressMapping * mapping_b = (const GumAddressMapping *) b;

  if (mapping_a->real_address < mapping_b->real_address)
    return -1;
  else if (mapping_a->real_address > mapping_b->real_address)
    return 1;
  else
    return 0;
}

static GumExecBlock *
gum_exec_block_new (GumExecCtx * ctx)
{
  GumSlab * slab;

  for (slab = &ctx->code_slab; slab != NULL; slab = slab->next)
  {
    if (slab->size - slab->offset >= GUM_EXEC_BLOCK_MIN_SIZE)
    {
      GumExecBlock * block = (GumExecBlock *) (slab->data + slab->offset);

      block->ctx = ctx;
      block->slab = slab;

      block->code_begin = GSIZE_TO_POINTER (GPOINTER_TO_SIZE (slab->data +
            slab->offset + sizeof (GumExecBlock) + GUM_CODE_ALIGNMENT - 1)
          & ~(GUM_CODE_ALIGNMENT - 1));
      block->code_end = block->code_begin;

      block->state = GUM_EXEC_NORMAL;
      block->recycle_count = 0;
      block->has_call_to_excluded_range = FALSE;

      slab->offset += block->code_begin - (slab->data + slab->offset);

      return block;
    }
  }

  if (ctx->stalker->priv->trust_threshold < 0)
  {
    ctx->code_slab.offset = 0;

    return gum_exec_block_new (ctx);
  }

  return NULL;
}

static GumExecBlock *
gum_exec_block_obtain (GumExecCtx * ctx,
                       gpointer real_address,
                       gpointer * code_address)
{
  GumAddressMapping mapping;
  GumSlab * slab;

  mapping.real_address = real_address;
  mapping.code_address = NULL;
  mapping.block = NULL;

  for (slab = &ctx->mapping_slab; slab != NULL; slab = slab->next)
  {
    GumAddressMapping * match;

    match = bsearch (&mapping, slab->data,
        slab->offset / sizeof (GumAddressMapping), sizeof (GumAddressMapping),
        gum_address_mapping_compare);
    if (match != NULL)
    {
      *code_address = match->code_address;
      return match->block;
    }
  }

  return NULL;
}

static gboolean
gum_exec_block_is_full (GumExecBlock * block)
{
  guint8 * slab_end = block->slab->data + block->slab->size;
  return slab_end - block->code_end < GUM_EXEC_BLOCK_MIN_SIZE;
}

static void
gum_exec_block_commit (GumExecBlock * block)
{
  guint real_size;
  guint8 * aligned_end;

  real_size = block->real_end - block->real_begin;
  block->real_snapshot = block->code_end;
  memcpy (block->real_snapshot, block->real_begin, real_size);
  block->slab->offset += real_size;

  aligned_end = GSIZE_TO_POINTER (GPOINTER_TO_SIZE (block->real_snapshot +
        real_size + GUM_DATA_ALIGNMENT - 1) & ~(GUM_DATA_ALIGNMENT - 1));
  block->slab->offset += aligned_end - block->code_begin;
}

static void
gum_exec_block_backpatch_call (GumExecBlock * block,
                               gpointer code_start,
                               GumPrologType opened_prolog,
                               gpointer target_address,
                               gpointer ret_real_address,
                               gpointer ret_code_address)
{
  GumExecCtx * ctx = block->ctx;

  if (ctx->state == GUM_EXEC_CTX_ACTIVE &&
      block->recycle_count >= ctx->stalker->priv->trust_threshold)
  {
    GumX86Writer * cw = &ctx->code_writer;
    gconstpointer beach_label = cw->code + 1;

    gum_x86_writer_reset (cw, code_start);

    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_x86_writer_put_pushfx (cw);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
    }

    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XCX,
        GUM_ADDRESS (&block->ctx->current_frame));
    gum_x86_writer_put_test_reg_u32 (cw, GUM_REG_XCX,
        block->ctx->stalker->priv->page_size - 1);
    gum_x86_writer_put_jcc_short_label (cw, GUM_X86_JZ, beach_label,
        GUM_UNLIKELY);

    gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XCX, sizeof (GumExecFrame));
    gum_x86_writer_put_mov_near_ptr_reg (cw,
        GUM_ADDRESS (&block->ctx->current_frame), GUM_REG_XCX);

    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
        GUM_ADDRESS (ret_real_address));
    gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
        GUM_ADDRESS (ret_code_address));
    gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
        GUM_REG_XCX, G_STRUCT_OFFSET (GumExecFrame, code_address), GUM_REG_XAX);

    gum_x86_writer_put_label (cw, beach_label);
    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_popfx (cw);
    }
    else
    {
      gum_exec_ctx_write_epilog (block->ctx, opened_prolog, cw);
    }

    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
        GUM_ADDRESS (ret_real_address));
    gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);

    gum_x86_writer_put_jmp (cw, target_address);

    gum_x86_writer_flush (cw);
  }
}

static void
gum_exec_block_backpatch_jmp (GumExecBlock * block,
                              gpointer code_start,
                              GumPrologType opened_prolog,
                              gpointer target_address)
{
  GumExecCtx * ctx = block->ctx;

  if (ctx->state == GUM_EXEC_CTX_ACTIVE &&
      block->recycle_count >= ctx->stalker->priv->trust_threshold)
  {
    GumX86Writer * cw = &ctx->code_writer;

    if (opened_prolog != GUM_PROLOG_NONE)
    {
      gum_exec_ctx_write_epilog (block->ctx, opened_prolog, cw);
    }

    gum_x86_writer_reset (cw, code_start);
    gum_x86_writer_put_jmp (cw, target_address);
    gum_x86_writer_flush (cw);
  }
}

static void
gum_exec_block_backpatch_ret (GumExecBlock * block,
                              gpointer code_start,
                              gpointer target_address)
{
  if (block != NULL) /* when we just unfollowed */
  {
    GumExecCtx * ctx = block->ctx;

    if (ctx->state == GUM_EXEC_CTX_ACTIVE &&
        block->recycle_count >= ctx->stalker->priv->trust_threshold)
    {
      GumX86Writer * cw = &ctx->code_writer;

      gum_x86_writer_reset (cw, code_start);
      gum_x86_writer_put_jmp (cw, target_address);
      gum_x86_writer_flush (cw);
    }
  }
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_branch_insn (GumExecBlock * block,
                                       GumGeneratorContext * gc)
{
  GumInstruction * insn = gc->instruction;
  GumX86Writer * cw = gc->code_writer;
  gboolean is_conditional;
  ud_operand_t * op = &insn->ud->operand[0];
  GumBranchTarget target = { 0, };

  is_conditional =
      (insn->ud->mnemonic != UD_Icall && insn->ud->mnemonic != UD_Ijmp);

  target.origin_ip = insn->end;

  target.pfx_seg = UD_NONE;
  target.base = op->base;
  target.index = op->index;
  target.scale = op->scale;

  if (op->type == UD_OP_JIMM && op->base == UD_NONE)
  {
    if (op->size == 8)
      target.absolute_address = insn->end + op->lval.sbyte;
    else if (op->size == 32)
      target.absolute_address = insn->end + op->lval.sdword;
    else
      g_assert_not_reached ();
    target.is_indirect = FALSE;
  }
  else if (op->type == UD_OP_MEM)
  {
    g_assert (op->size == GLIB_SIZEOF_VOID_P * 8);
#if GLIB_SIZEOF_VOID_P == 4
    g_assert (op->base == UD_NONE ||
        (op->base >= UD_R_EAX && op->base <= UD_R_EDI));
#else
    g_assert (op->base == UD_NONE || op->base == UD_R_RIP ||
        (op->base >= UD_R_RAX && op->base <= UD_R_R15));
#endif
    g_assert (op->offset == 8 || op->offset == 32 || op->offset == 0);

#ifdef G_OS_WIN32
    /* Can't follow WoW64 */
    if (insn->ud->pfx_seg == UD_R_FS && op->lval.udword == 0xc0)
      return GUM_REQUIRE_SINGLE_STEP;
#endif

    if (op->base == UD_NONE && op->index == UD_NONE)
    {
      g_assert (op->offset == 32);
      target.absolute_address = GSIZE_TO_POINTER (op->lval.udword);
    }
    else
    {
      if (op->offset == 8)
        target.relative_offset = op->lval.sbyte;
      else if (op->offset == 32)
        target.relative_offset = op->lval.sdword;
      else
        target.relative_offset = 0;
    }

    target.is_indirect = TRUE;
    target.pfx_seg = insn->ud->pfx_seg;
  }
  else if (op->type == UD_OP_REG)
  {
    target.is_indirect = FALSE;
  }
  else
  {
    g_assert_not_reached ();
  }

  if (insn->ud->mnemonic == UD_Icall)
  {
    gboolean target_is_excluded = FALSE;

    if ((block->ctx->sink_mask & GUM_CALL) != 0)
      gum_exec_block_write_call_event_code (block, &target, gc);

    if (block->ctx->stalker->priv->any_probes_attached)
      gum_exec_block_write_call_probe_code (block, &target, gc);

    if (!target.is_indirect && target.base == UD_NONE)
    {
      GArray * exclusions = block->ctx->stalker->priv->exclusions;
      guint i;

      for (i = 0; i != exclusions->len; i++)
      {
        GumMemoryRange * r = &g_array_index (exclusions, GumMemoryRange, i);
        if (GUM_MEMORY_RANGE_INCLUDES (r,
            GUM_ADDRESS (target.absolute_address)))
        {
          target_is_excluded = TRUE;
          break;
        }
      }
    }

    if (target_is_excluded)
    {
      block->has_call_to_excluded_range = TRUE;
      return GUM_REQUIRE_RELOCATION;
    }

    gum_x86_relocator_skip_one_no_label (gc->relocator);
    gum_exec_block_write_call_invoke_code (block, &target, gc);
  }
  else
  {
    gpointer cond_false_lbl_id;

    gum_x86_relocator_skip_one_no_label (gc->relocator);

    cond_false_lbl_id =
        GUINT_TO_POINTER ((GPOINTER_TO_UINT (insn->begin) << 16) | 0xbeef);

    if (is_conditional)
    {
      g_assert (!target.is_indirect);

      gum_exec_block_close_prolog (block, gc);

#if GLIB_SIZEOF_VOID_P == 4
      gum_x86_writer_put_jcc_short_label (cw,
#else
      gum_x86_writer_put_jcc_near_label (cw,
#endif
          gum_jcc_opcode_negate (gum_jcc_insn_to_short_opcode (insn->begin)),
          cond_false_lbl_id, GUM_NO_HINT);
    }

    gum_exec_block_write_jmp_transfer_code (block, &target, gc);

    if (is_conditional)
    {
      GumBranchTarget cond_target = { 0, };

      cond_target.is_indirect = FALSE;
      cond_target.absolute_address = insn->end;

      gum_x86_writer_put_label (cw, cond_false_lbl_id);
      gum_exec_block_write_jmp_transfer_code (block, &cond_target, gc);
    }
  }

  return GUM_REQUIRE_NOTHING;
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_ret_insn (GumExecBlock * block,
                                    GumGeneratorContext * gc)
{
  if ((block->ctx->sink_mask & GUM_RET) != 0)
    gum_exec_block_write_ret_event_code (block, gc);

  gum_x86_relocator_skip_one_no_label (gc->relocator);

  gum_exec_block_write_ret_transfer_code (block, gc);

  return GUM_REQUIRE_NOTHING;
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_sysenter_insn (GumExecBlock * block,
                                         GumGeneratorContext * gc)
{
#if defined (HAVE_MAC) && GLIB_SIZEOF_VOID_P == 4
  GumX86Writer * cw = gc->code_writer;
  guint8 code[] = {
    /* 00 */ 0x89, 0x15, 0xaa, 0xaa, 0xaa, 0xaa, /* mov [0xaaaaaaaa], edx */
    /* 06 */ 0xba, 0xbb, 0xbb, 0xbb, 0xbb,       /* mov edx, 0xbbbbbbbb   */
    /* 0b */ 0x0f, 0x34,                         /* sysenter              */
    /* 0d */ 0xcc, 0xcc, 0xcc, 0xcc              /* <saved edx goes here> */
  };
  gpointer * saved_edx;
  gpointer continuation;
  gconstpointer resolve_dynamically_label = cw->code;

  gum_exec_block_close_prolog (block, gc);

  saved_edx = (gpointer *) (cw->code + 0xd); /* address of 0xcccccccc */
  continuation = cw->code + 0xd + 4; /* after 0xcccccccc */
  *((gpointer *) (code + 2)) = saved_edx; /* fill in 0xaaaaaaaa */
  *((gpointer *) (code + 7)) = continuation; /* fill in 0xbbbbbbbb */

  gum_x86_writer_put_bytes (cw, code, sizeof (code));

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EDX,
      GUM_ADDRESS (saved_edx));

  if ((block->ctx->sink_mask & GUM_RET) != 0)
  {
    gum_exec_block_write_ret_event_code (block, gc);
    gum_exec_block_close_prolog (block, gc);
  }

  /*
   * Fast path (try the stack)
   */
  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_push_reg (cw, GUM_REG_EAX);

  /* check frame at the top of the stack */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EAX,
      GUM_ADDRESS (&block->ctx->current_frame));
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw,
      GUM_REG_EAX, G_STRUCT_OFFSET (GumExecFrame, real_address),
      GUM_REG_EDX);
  gum_x86_writer_put_jcc_short_label (cw, GUM_X86_JNZ,
      resolve_dynamically_label, GUM_UNLIKELY);

  /* replace return address */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_EDX,
      GUM_REG_EAX, G_STRUCT_OFFSET (GumExecFrame, code_address));

  /* pop from our stack */
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_EAX, sizeof (GumExecFrame));
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->current_frame), GUM_REG_EAX);

  /* proceeed to block */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_EAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_jmp_reg (cw, GUM_REG_EDX);

  gum_x86_writer_put_label (cw, resolve_dynamically_label);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_EAX);
  gum_x86_writer_put_popfx (cw);

  /*
   * Slow path (resolve dynamically)
   */
  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_THUNK_REG_ARG1,
      GUM_ADDRESS (saved_edx));
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_ESP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (gum_exec_ctx_replace_current_block_with));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  gum_exec_block_close_prolog (block, gc);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));

  gum_x86_relocator_skip_one_no_label (gc->relocator);

  return GUM_REQUIRE_NOTHING;
#else
  (void) block;
  (void) gc;

  return GUM_REQUIRE_RELOCATION;
#endif
}

static void
gum_exec_block_write_call_invoke_code (GumExecBlock * block,
                                       const GumBranchTarget * target,
                                       GumGeneratorContext * gc)
{
  gboolean can_backpatch;
  GumX86Writer * cw = gc->code_writer;
  gpointer call_code_start;
  GumPrologType opened_prolog;
  gconstpointer perform_stack_push = cw->code + 1;
  gconstpointer skip_stack_push = cw->code + 2;
  gpointer ret_real_address;
  gpointer ret_code_address;

  call_code_start = cw->code;
  opened_prolog = gc->opened_prolog;

  /* We can backpatch if we have some trust and the call's target is static */
  can_backpatch = (block->ctx->stalker->priv->trust_threshold >= 0 &&
      !target->is_indirect &&
      target->base == UD_NONE);

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  /* fill in placeholder with application's retaddr */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->app_stack));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XAX, sizeof (gpointer));
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->end));
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->app_stack), GUM_REG_XAX);
  gc->accumulated_stack_delta += sizeof (gpointer);

  /* generate code for the target */
  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_THUNK_REG_ARG1);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (gum_exec_ctx_replace_current_block_with));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XDX, GUM_REG_XAX);
  gum_x86_writer_put_jmp_near_label (cw, perform_stack_push);

  if (can_backpatch)
  {
    /*
     * We need some padding so the backpatching doesn't overwrite the return
     * handling logic below
     */
    gum_x86_writer_put_padding (cw, 50);
  }

  /* generate code for handling the return */
  ret_real_address = gc->instruction->end;
  ret_code_address = cw->code;

  gum_exec_ctx_write_prolog (block->ctx, GUM_PROLOG_MINIMAL,
      ret_real_address, cw);

  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG1,
      GUM_ADDRESS (ret_real_address));
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (gum_exec_ctx_replace_current_block_with));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XDX, GUM_REG_XAX);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->current_block));
  gum_x86_writer_put_call_with_arguments (cw,
      GUM_FUNCPTR_TO_POINTER (gum_exec_block_backpatch_ret), 3,
      GUM_ARG_REGISTER, GUM_REG_XAX,
      GUM_ARG_POINTER, ret_code_address,
      GUM_ARG_REGISTER, GUM_REG_XDX);

  gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_MINIMAL, cw);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));

  /* push frame on stack */
  gum_x86_writer_put_label (cw, perform_stack_push);
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XCX,
      GUM_ADDRESS (&block->ctx->current_frame));
  gum_x86_writer_put_test_reg_u32 (cw, GUM_REG_XCX,
      block->ctx->stalker->priv->page_size - 1);
  gum_x86_writer_put_jcc_short_label (cw, GUM_X86_JZ, skip_stack_push,
      GUM_UNLIKELY);

  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XCX, sizeof (GumExecFrame));
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->current_frame), GUM_REG_XCX);

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (ret_real_address));
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (ret_code_address));
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XCX, G_STRUCT_OFFSET (GumExecFrame, code_address), GUM_REG_XAX);

  gum_x86_writer_put_label (cw, skip_stack_push);

  if (can_backpatch)
  {
    gum_x86_writer_put_call_with_arguments (cw,
        GUM_FUNCPTR_TO_POINTER (gum_exec_block_backpatch_call), 6,
        GUM_ARG_POINTER, block,
        GUM_ARG_POINTER, call_code_start,
        GUM_ARG_POINTER, GSIZE_TO_POINTER (opened_prolog),
        GUM_ARG_REGISTER, GUM_REG_XDX,
        GUM_ARG_POINTER, ret_real_address,
        GUM_ARG_POINTER, ret_code_address);
  }

  /* execute the generated code */
  gum_exec_block_close_prolog (block, gc);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));
}

static void
gum_exec_block_write_jmp_transfer_code (GumExecBlock * block,
                                        const GumBranchTarget * target,
                                        GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  guint8 * code_start;
  GumPrologType opened_prolog;

  code_start = cw->code;
  opened_prolog = gc->opened_prolog;

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_THUNK_REG_ARG1);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (gum_exec_ctx_replace_current_block_with));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  if (block->ctx->stalker->priv->trust_threshold >= 0 &&
      !target->is_indirect &&
      target->base == UD_NONE)
  {
    gum_x86_writer_put_call_with_arguments (cw,
        GUM_FUNCPTR_TO_POINTER (gum_exec_block_backpatch_jmp), 4,
        GUM_ARG_POINTER, block,
        GUM_ARG_POINTER, code_start,
        GUM_ARG_POINTER, GSIZE_TO_POINTER (opened_prolog),
        GUM_ARG_REGISTER, GUM_REG_XAX);
  }

  gum_exec_block_close_prolog (block, gc);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));
}

static void
gum_exec_block_write_ret_transfer_code (GumExecBlock * block,
                                        GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  gconstpointer resolve_dynamically_label = cw->code;

  gum_exec_block_close_prolog (block, gc);

  /*
   * Fast path (try the stack)
   */
  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);

  /* we want to jump to the origin ret instruction after modifying the
   * return address on the stack */
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (gc->instruction->begin));
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->return_at), GUM_REG_XAX);

  /* check frame at the top of the stack */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XDX,
      GUM_ADDRESS (&block->ctx->current_frame));
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XDX);
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw,
      GUM_REG_XSP, 3 * sizeof (gpointer),
      GUM_REG_XAX);
  gum_x86_writer_put_jcc_short_label (cw, GUM_X86_JNZ,
      resolve_dynamically_label, GUM_UNLIKELY);

  /* replace return address */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XAX,
      GUM_REG_XDX, G_STRUCT_OFFSET (GumExecFrame, code_address));
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XSP, 3 * sizeof (gpointer),
      GUM_REG_XAX);

  /* pop from our stack */
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XDX, sizeof (GumExecFrame));
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->current_frame), GUM_REG_XDX);

  /* proceeed to block */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->return_at));

  gum_x86_writer_put_label (cw, resolve_dynamically_label);
  /* clear our stack so we might resync later */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XDX,
      GUM_ADDRESS (&block->ctx->first_frame));
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->current_frame), GUM_REG_XDX);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_popfx (cw);

  /*
   * Slow path (resolve dynamically)
   */
  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->app_stack));
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_THUNK_REG_ARG1, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (gum_exec_ctx_replace_current_block_with));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);

  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->app_stack));
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XCX,
      GUM_ADDRESS (&block->ctx->resume_at));
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);
  gum_exec_block_close_prolog (block, gc);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->return_at));
}

static void
gum_exec_block_write_single_step_transfer_code (GumExecBlock * block,
                                                GumGeneratorContext * gc)
{
  guint8 code[] = {
    0xc6, 0x05, 0x78, 0x56, 0x34, 0x12,       /* mov byte [X], state */
          GUM_EXEC_SINGLE_STEPPING_ON_CALL,
    0x9c,                                     /* pushfd              */
    0x81, 0x0c, 0x24, 0x00, 0x01, 0x00, 0x00, /* or [esp], 0x100     */
    0x9d                                      /* popfd               */
  };

  *((guint8 **) (code + 2)) = &block->state;
  gum_x86_writer_put_bytes (gc->code_writer, code, sizeof (code));
  gum_x86_writer_put_jmp (gc->code_writer, gc->instruction->begin);
}

static void
gum_exec_block_write_call_event_code (GumExecBlock * block,
                                      const GumBranchTarget * target,
                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_exec_block_write_event_init_code (block, GUM_CALL, gc);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->begin));
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumCallEvent, location),
      GUM_REG_XCX);

  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumCallEvent, target),
      GUM_REG_XCX);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XCX,
      GUM_ADDRESS (&block->ctx->first_frame));
  gum_x86_writer_put_sub_reg_near_ptr (cw, GUM_REG_XCX,
      GUM_ADDRESS (&block->ctx->current_frame));
#if GLIB_SIZEOF_VOID_P == 4
  gum_x86_writer_put_shr_reg_u8 (cw, GUM_REG_XCX, 3);
#else
  gum_x86_writer_put_shr_reg_u8 (cw, GUM_REG_XCX, 4);
#endif
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumCallEvent, depth),
      GUM_REG_XCX);

  gum_exec_block_write_event_submit_code (block, gc);
}

static void
gum_exec_block_write_ret_event_code (GumExecBlock * block,
                                     GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_exec_block_write_event_init_code (block, GUM_RET, gc);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->begin));
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumRetEvent, location),
      GUM_REG_XCX);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XDX,
      GUM_ADDRESS (&block->ctx->app_stack));
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XDX, GUM_REG_XDX);
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumRetEvent, target),
      GUM_REG_XDX);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XCX,
      GUM_ADDRESS (&block->ctx->first_frame));
  gum_x86_writer_put_sub_reg_near_ptr (cw, GUM_REG_XCX,
      GUM_ADDRESS (&block->ctx->current_frame));
#if GLIB_SIZEOF_VOID_P == 4
  gum_x86_writer_put_shr_reg_u8 (cw, GUM_REG_XCX, 3);
#else
  gum_x86_writer_put_shr_reg_u8 (cw, GUM_REG_XCX, 4);
#endif
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumCallEvent, depth),
      GUM_REG_ECX);

  gum_exec_block_write_event_submit_code (block, gc);
}

static void
gum_exec_block_write_exec_event_code (GumExecBlock * block,
                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_exec_block_write_event_init_code (block, GUM_EXEC, gc);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->begin));
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumExecEvent, location),
      GUM_REG_XCX);

  gum_exec_block_write_event_submit_code (block, gc);
}

static void
gum_exec_block_write_event_init_code (GumExecBlock * block,
                                      GumEventType type,
                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->tmp_event));
  gum_x86_writer_put_mov_reg_offset_ptr_u32 (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumAnyEvent, type),
      type);
}

static void
gum_exec_block_write_event_submit_code (GumExecBlock * block,
                                        GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
#if GLIB_SIZEOF_VOID_P == 4
  guint align_correction = 8;
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP, align_correction);
#endif
  gum_x86_writer_put_call_with_arguments (cw,
      block->ctx->sink_process_impl, 2,
      GUM_ARG_POINTER, block->ctx->sink,
      GUM_ARG_REGISTER, GUM_REG_XAX);
#if GLIB_SIZEOF_VOID_P == 4
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP, align_correction);
#endif
}

static void
gum_exec_block_invoke_call_probes_for_target (GumExecBlock * block,
                                              gpointer target_address,
                                              GumCpuContext * cpu_context)
{
  GumStalkerPrivate * priv = block->ctx->stalker->priv;
  GArray * probes;

  gum_spinlock_acquire (&priv->probe_lock);

  probes = (GArray *)
      g_hash_table_lookup (priv->probe_array_by_address, target_address);
  if (probes != NULL)
  {
    GumCallSite call_site;
    guint i;

    call_site.block_address = block->real_begin;
    call_site.stack_data = block->ctx->app_stack;
    call_site.cpu_context = cpu_context;

    for (i = 0; i != probes->len; i++)
    {
      GumCallProbe * probe = &g_array_index (probes, GumCallProbe, i);

      probe->callback (&call_site, probe->user_data);
    }
  }

  gum_spinlock_release (&priv->probe_lock);
}

static void
gum_exec_block_write_call_probe_code (GumExecBlock * block,
                                      const GumBranchTarget * target,
                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  gboolean skip_probing = FALSE;

  if (!target->is_indirect && target->base == UD_NONE)
  {
    GumStalkerPrivate * priv = block->ctx->stalker->priv;

    gum_spinlock_acquire (&priv->probe_lock);
    skip_probing = g_hash_table_lookup (priv->probe_array_by_address,
        target->absolute_address) == NULL;
    gum_spinlock_release (&priv->probe_lock);
  }

  if (!skip_probing)
  {
#if GLIB_SIZEOF_VOID_P == 4
    guint align_correction = 4;
#endif

    if (gc->opened_prolog != GUM_PROLOG_NONE)
      gum_exec_block_close_prolog (block, gc);
    gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

    gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);

#if GLIB_SIZEOF_VOID_P == 4
    gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP, align_correction);
#endif
    gum_x86_writer_put_call_with_arguments (cw,
        GUM_FUNCPTR_TO_POINTER (gum_exec_block_invoke_call_probes_for_target), 3,
        GUM_ARG_POINTER, block,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_REGISTER, GUM_REG_XBX);
#if GLIB_SIZEOF_VOID_P == 4
    gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP, align_correction);
#endif
  }
}

static void
gum_exec_block_open_prolog (GumExecBlock * block,
                            GumPrologType type,
                            GumGeneratorContext * gc)
{
  if (gc->opened_prolog >= type)
    return;

  /* We don't want to handle this case for performance reasons */
  g_assert (gc->opened_prolog == GUM_PROLOG_NONE);

  gc->opened_prolog = type;
  if (type == GUM_PROLOG_MINIMAL)
  {
    gc->state_preserve_stack_offset = 0;
  }
  else /* GUM_PROLOG_FULL */
  {
#if GLIB_SIZEOF_VOID_P == 4
    gc->state_preserve_stack_offset = G_STRUCT_OFFSET (GumCpuContext, ebx);
#else
    gc->state_preserve_stack_offset = G_STRUCT_OFFSET (GumCpuContext, r9);
#endif
  }
  gc->accumulated_stack_delta = 0;

  gum_exec_ctx_write_prolog (block->ctx, type, gc->instruction->begin,
      gc->code_writer);
}

static void
gum_exec_block_close_prolog (GumExecBlock * block,
                             GumGeneratorContext * gc)
{
  if (gc->opened_prolog == GUM_PROLOG_NONE)
    return;

  gum_exec_ctx_write_epilog (block->ctx, gc->opened_prolog, gc->code_writer);

  gc->accumulated_stack_delta = 0;
  gc->state_preserve_stack_offset = 0;
  gc->opened_prolog = GUM_PROLOG_NONE;
}

static void
gum_write_segment_prefix (uint8_t segment,
                          GumX86Writer * cw)
{
  switch (segment)
  {
    case UD_NONE: break;

    case UD_R_CS: gum_x86_writer_put_byte (cw, 0x2e); break;
    case UD_R_SS: gum_x86_writer_put_byte (cw, 0x36); break;
    case UD_R_DS: gum_x86_writer_put_byte (cw, 0x3e); break;
    case UD_R_ES: gum_x86_writer_put_byte (cw, 0x26); break;
    case UD_R_FS: gum_x86_writer_put_byte (cw, 0x64); break;
    case UD_R_GS: gum_x86_writer_put_byte (cw, 0x65); break;

    default:
      g_assert_not_reached ();
      break;
  }
}

static GumCpuReg
gum_cpu_meta_reg_from_real_reg (GumCpuReg reg)
{
  if (reg >= GUM_REG_EAX && reg <= GUM_REG_R15D)
    return (GumCpuReg) (GUM_REG_XAX + reg - GUM_REG_EAX);
  else if (reg >= GUM_REG_RAX && reg <= GUM_REG_R15)
    return (GumCpuReg) (GUM_REG_XAX + reg - GUM_REG_RAX);
  else if (reg == GUM_REG_RIP)
    return GUM_REG_XIP;
  else if (reg == GUM_REG_NONE)
    return GUM_REG_NONE;

  g_assert_not_reached ();
  return GUM_REG_NONE;
}

static GumCpuReg
gum_cpu_reg_from_ud (enum ud_type reg)
{
  if (reg >= UD_R_EAX && reg <= UD_R_EDI)
    return (GumCpuReg) (GUM_REG_EAX + reg - UD_R_EAX);
  else if (reg >= UD_R_RAX && reg <= UD_R_R15)
    return (GumCpuReg) (GUM_REG_RAX + reg - UD_R_RAX);
  else if (reg == UD_R_RIP)
    return GUM_REG_RIP;
  else if (reg == UD_NONE)
    return GUM_REG_NONE;
  else
    g_assert_not_reached ();
}

static void
gum_private_set (GPrivate * private_key,
                 gpointer data)
{
  g_private_set (private_key, data);
}

#ifdef G_OS_WIN32

#ifdef _M_IX86

static void
enable_hardware_breakpoint (DWORD * dr7_reg, guint index)
{
  /* set both RWn and LENn to 00 */
  *dr7_reg &= ~(0xf << (16 + (2 * index)));

  /* set LE bit */
  *dr7_reg |= 1 << (2 * index);
}

static gpointer
find_system_call_above_us (GumStalker * stalker, gpointer * start_esp)
{
  GumStalkerPrivate * priv = stalker->priv;
  gpointer * top_esp, * cur_esp;
  guint8 call_fs_c0_code[] = { 0x64, 0xff, 0x15, 0xc0, 0x00, 0x00, 0x00 };
  guint8 * minimum_address, * maximum_address;

  __asm
  {
    mov eax, fs:[4];
    mov [top_esp], eax;
  }

  if ((guint) ABS (top_esp - start_esp) > priv->page_size)
  {
    top_esp = (gpointer *) ((GPOINTER_TO_SIZE (start_esp) +
        (priv->page_size - 1)) & ~(priv->page_size - 1));
  }

  /* These boundaries are quite artificial... */
  minimum_address = (guint8 *) priv->user32_start + sizeof (call_fs_c0_code);
  maximum_address = (guint8 *) priv->user32_end - 1;

  for (cur_esp = start_esp + 1; cur_esp < top_esp; cur_esp++)
  {
    guint8 * address = (guint8 *) *cur_esp;

    if (address >= minimum_address && address <= maximum_address &&
        memcmp (address - sizeof (call_fs_c0_code), call_fs_c0_code,
        sizeof (call_fs_c0_code)) == 0)
    {
      return address;
    }
  }

  return NULL;
}

#endif

static gboolean
gum_stalker_handle_exception (EXCEPTION_RECORD * exception_record,
    CONTEXT * context, gpointer user_data)
{
  GumStalker * self = GUM_STALKER_CAST (user_data);
  GumExecCtx * ctx;
  GumExecBlock * block;

  if (exception_record->ExceptionCode != STATUS_SINGLE_STEP)
    return FALSE;

  ctx = gum_stalker_get_exec_ctx (self);
  if (ctx == NULL)
    return FALSE;

  block = ctx->current_block;

#ifdef _M_IX86
  /*printf ("gum_stalker_handle_exception state=%u %p %08x\n",
      block->state, context->Eip, exception_record->ExceptionCode);*/

  switch (block->state)
  {
    case GUM_EXEC_SINGLE_STEPPING_ON_CALL:
    {
      DWORD instruction_after_call_here;
      DWORD instruction_after_call_above_us;

      block->previous_dr0 = context->Dr0;
      block->previous_dr1 = context->Dr1;
      block->previous_dr2 = context->Dr2;
      block->previous_dr7 = context->Dr7;

      instruction_after_call_here = context->Eip +
          gum_find_instruction_length ((guint8 *) context->Eip);
      context->Dr0 = instruction_after_call_here;
      enable_hardware_breakpoint (&context->Dr7, 0);

      context->Dr1 = (DWORD) self->priv->ki_user_callback_dispatcher_impl;
      enable_hardware_breakpoint (&context->Dr7, 1);

      instruction_after_call_above_us = (DWORD)
          find_system_call_above_us (self, (gpointer *) context->Esp);
      if (instruction_after_call_above_us != 0)
      {
        context->Dr2 = instruction_after_call_above_us;
        enable_hardware_breakpoint (&context->Dr7, 2);
      }

      block->state = GUM_EXEC_SINGLE_STEPPING_THROUGH_CALL;

      break;
    }

    case GUM_EXEC_SINGLE_STEPPING_THROUGH_CALL:
    {
      context->Dr0 = block->previous_dr0;
      context->Dr1 = block->previous_dr1;
      context->Dr2 = block->previous_dr2;
      context->Dr7 = block->previous_dr7;

      gum_exec_ctx_replace_current_block_with (ctx,
          GSIZE_TO_POINTER (context->Eip));
      context->Eip = (DWORD) ctx->resume_at;

      block->state = GUM_EXEC_NORMAL;

      break;
    }

    case GUM_EXEC_NORMAL:
      return FALSE;

    default:
      g_assert_not_reached ();
  }
#else
  (void) context;
#endif

  return TRUE;
}

#endif
