/*
 * Copyright (C) 2010-2017 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#ifndef __GUM_ARM_RELOCATOR_H__
#define __GUM_ARM_RELOCATOR_H__

#include "gumarmwriter.h"

#include <capstone.h>

G_BEGIN_DECLS

typedef struct _GumArmRelocator GumArmRelocator;

struct _GumArmRelocator
{
  volatile gint ref_count;

  csh capstone;

  const guint8 * input_start;
  const guint8 * input_cur;
  GumAddress input_pc;
  cs_insn ** input_insns;
  GumArmWriter * output;

  guint inpos;
  guint outpos;

  gboolean eob;
  gboolean eoi;
};

GumArmRelocator * gum_arm_relocator_new (gconstpointer input_code,
    GumArmWriter * output);
GumArmRelocator * gum_arm_relocator_ref (GumArmRelocator * relocator);
void gum_arm_relocator_unref (GumArmRelocator * relocator);

void gum_arm_relocator_init (GumArmRelocator * relocator,
    gconstpointer input_code, GumArmWriter * output);
void gum_arm_relocator_clear (GumArmRelocator * relocator);

void gum_arm_relocator_reset (GumArmRelocator * relocator,
    gconstpointer input_code, GumArmWriter * output);

guint gum_arm_relocator_read_one (GumArmRelocator * self,
    const cs_insn ** instruction);

cs_insn * gum_arm_relocator_peek_next_write_insn (GumArmRelocator * self);
gpointer gum_arm_relocator_peek_next_write_source (GumArmRelocator * self);
void gum_arm_relocator_skip_one (GumArmRelocator * self);
gboolean gum_arm_relocator_write_one (GumArmRelocator * self);
void gum_arm_relocator_write_all (GumArmRelocator * self);

gboolean gum_arm_relocator_eob (GumArmRelocator * self);
gboolean gum_arm_relocator_eoi (GumArmRelocator * self);

gboolean gum_arm_relocator_can_relocate (gpointer address, guint min_bytes,
    guint * maximum);
guint gum_arm_relocator_relocate (gpointer from, guint min_bytes, gpointer to);

G_END_DECLS

#endif
