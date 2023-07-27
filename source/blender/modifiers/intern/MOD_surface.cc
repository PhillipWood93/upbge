/* SPDX-FileCopyrightText: 2005 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_bvhutils.h"
#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.hh"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

#include "BLO_read_write.h"

#include "MEM_guardedalloc.h"

static void init_data(ModifierData *md)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(surmd, modifier));

  MEMCPY_STRUCT_AFTER(surmd, DNA_struct_default_get(SurfaceModifierData), modifier);
}

static void copy_data(const ModifierData *md_src, ModifierData *md_dst, const int flag)
{
  SurfaceModifierData *surmd_dst = (SurfaceModifierData *)md_dst;

  BKE_modifier_copydata_generic(md_src, md_dst, flag);

  memset(&surmd_dst->runtime, 0, sizeof(surmd_dst->runtime));
}

static void free_data(ModifierData *md)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;

  if (surmd) {
    if (surmd->runtime.bvhtree) {
      free_bvhtree_from_mesh(surmd->runtime.bvhtree);
      MEM_SAFE_FREE(surmd->runtime.bvhtree);
    }

    if (surmd->runtime.mesh) {
      BKE_id_free(nullptr, surmd->runtime.mesh);
      surmd->runtime.mesh = nullptr;
    }

    MEM_SAFE_FREE(surmd->runtime.vert_positions_prev);

    MEM_SAFE_FREE(surmd->runtime.vert_velocities);
  }
}

static bool depends_on_time(Scene * /*scene*/, ModifierData * /*md*/)
{
  return true;
}

static void deform_verts(ModifierData *md,
                         const ModifierEvalContext *ctx,
                         Mesh *mesh,
                         float (*vertexCos)[3],
                         int /*verts_num*/)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;
  const int cfra = int(DEG_get_ctime(ctx->depsgraph));

  /* Free mesh and BVH cache. */
  if (surmd->runtime.bvhtree) {
    free_bvhtree_from_mesh(surmd->runtime.bvhtree);
    MEM_SAFE_FREE(surmd->runtime.bvhtree);
  }

  if (surmd->runtime.mesh) {
    BKE_id_free(nullptr, surmd->runtime.mesh);
    surmd->runtime.mesh = nullptr;
  }

  if (mesh) {
    surmd->runtime.mesh = BKE_mesh_copy_for_eval(mesh);
  }

  if (!ctx->object->pd) {
    printf("SurfaceModifier deform_verts: Should not happen!\n");
    return;
  }

  if (surmd->runtime.mesh) {
    uint mesh_verts_num = 0, i = 0;
    int init = 0;

    BKE_mesh_vert_coords_apply(surmd->runtime.mesh, vertexCos);

    mesh_verts_num = surmd->runtime.mesh->totvert;

    if ((mesh_verts_num != surmd->runtime.verts_num) ||
        (surmd->runtime.vert_positions_prev == nullptr) ||
        (surmd->runtime.vert_velocities == nullptr) || (cfra != surmd->runtime.cfra_prev + 1))
    {

      MEM_SAFE_FREE(surmd->runtime.vert_positions_prev);
      MEM_SAFE_FREE(surmd->runtime.vert_velocities);

      surmd->runtime.vert_positions_prev = static_cast<float(*)[3]>(
          MEM_calloc_arrayN(mesh_verts_num, sizeof(float[3]), __func__));
      surmd->runtime.vert_velocities = static_cast<float(*)[3]>(
          MEM_calloc_arrayN(mesh_verts_num, sizeof(float[3]), __func__));

      surmd->runtime.verts_num = mesh_verts_num;

      init = 1;
    }

    /* convert to global coordinates and calculate velocity */
    blender::MutableSpan<blender::float3> positions =
        surmd->runtime.mesh->vert_positions_for_write();
    for (i = 0; i < mesh_verts_num; i++) {
      float *vec = positions[i];
      mul_m4_v3(ctx->object->object_to_world, vec);

      if (init) {
        zero_v3(surmd->runtime.vert_velocities[i]);
      }
      else {
        sub_v3_v3v3(surmd->runtime.vert_velocities[i], vec, surmd->runtime.vert_positions_prev[i]);
      }

      copy_v3_v3(surmd->runtime.vert_positions_prev[i], vec);
    }

    surmd->runtime.cfra_prev = cfra;

    const bool has_face = surmd->runtime.mesh->faces_num > 0;
    const bool has_edge = surmd->runtime.mesh->totedge > 0;
    if (has_face || has_edge) {
      surmd->runtime.bvhtree = static_cast<BVHTreeFromMesh *>(
          MEM_callocN(sizeof(BVHTreeFromMesh), __func__));

      if (has_face) {
        BKE_bvhtree_from_mesh_get(
            surmd->runtime.bvhtree, surmd->runtime.mesh, BVHTREE_FROM_LOOPTRI, 2);
      }
      else if (has_edge) {
        BKE_bvhtree_from_mesh_get(
            surmd->runtime.bvhtree, surmd->runtime.mesh, BVHTREE_FROM_EDGES, 2);
      }
    }
  }
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  uiItemL(layout, TIP_("Settings are inside the Physics tab"), ICON_NONE);

  modifier_panel_end(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Surface, panel_draw);
}

static void blend_read(BlendDataReader * /*reader*/, ModifierData *md)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;

  memset(&surmd->runtime, 0, sizeof(surmd->runtime));
}

ModifierTypeInfo modifierType_Surface = {
    /*idname*/ "Surface",
    /*name*/ N_("Surface"),
    /*struct_name*/ "SurfaceModifierData",
    /*struct_size*/ sizeof(SurfaceModifierData),
    /*srna*/ &RNA_SurfaceModifier,
    /*type*/ eModifierTypeType_OnlyDeform,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_AcceptsCVs |
        eModifierTypeFlag_NoUserAdd,
    /*icon*/ ICON_MOD_PHYSICS,

    /*copy_data*/ copy_data,

    /*deform_verts*/ deform_verts,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ free_data,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ depends_on_time,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ blend_read,
};
