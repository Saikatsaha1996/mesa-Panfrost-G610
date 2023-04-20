/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include "agx_compiler.h"
#include "compiler/nir/nir_builder.h"
#include "agx_internal_formats.h"

static bool
pass(struct nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_ubo)
      return false;

   b->cursor = nir_before_instr(instr);

   nir_ssa_def *ubo_index = nir_ssa_for_src(b, intr->src[0], 1);
   nir_ssa_def *offset = nir_ssa_for_src(b, *nir_get_io_offset_src(intr), 1);

   unsigned dest_size = nir_dest_bit_size(intr->dest);
   assert((dest_size == 16 || dest_size == 32) && "other sizes lowered");

   nir_ssa_def *value =
      nir_load_constant_agx(b, intr->num_components, dest_size,
                            nir_load_ubo_base_agx(b, ubo_index),
                            nir_udiv_imm(b, offset, (dest_size / 8)),
                            .format = (dest_size == 32) ?
                                      AGX_INTERNAL_FORMAT_I32 :
                                      AGX_INTERNAL_FORMAT_I16);

   nir_ssa_def_rewrite_uses(&intr->dest.ssa, value);
   return true;
}

bool
agx_nir_lower_ubo(nir_shader *shader)
{
   return nir_shader_instructions_pass(shader, pass,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance,
                                       NULL);
}
