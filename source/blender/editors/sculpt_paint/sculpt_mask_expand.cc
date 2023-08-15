/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.h"
#include "BLI_task.h"

#include "BLT_translation.h"

#include "DNA_brush_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_ccg.h"
#include "BKE_context.h"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"

#include "DEG_depsgraph.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "ED_screen.hh"
#include "sculpt_intern.hh"

#include "bmesh.h"

#include <cmath>
#include <cstdlib>

static void sculpt_mask_expand_cancel(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  const bool create_face_set = RNA_boolean_get(op->ptr, "create_face_set");

  MEM_freeN(op->customdata);

  for (int n = 0; n < ss->filter_cache->nodes.size(); n++) {
    PBVHNode *node = ss->filter_cache->nodes[n];
    if (create_face_set) {
      for (int i = 0; i < ss->totfaces; i++) {
        ss->face_sets[i] = ss->filter_cache->prev_face_set[i];
      }
    }
    else {
      PBVHVertexIter vd;
      BKE_pbvh_vertex_iter_begin (ss->pbvh, node, vd, PBVH_ITER_UNIQUE) {
        *vd.mask = ss->filter_cache->prev_mask[vd.index];
      }
      BKE_pbvh_vertex_iter_end;
    }

    BKE_pbvh_node_mark_redraw(node);
  }

  if (!create_face_set) {
    SCULPT_flush_update_step(C, SCULPT_UPDATE_MASK);
  }
  SCULPT_filter_cache_free(ss);
  SCULPT_undo_push_end(ob);
  SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);
  ED_workspace_status_text(C, nullptr);
}

static void sculpt_expand_task_cb(void *__restrict userdata,
                                  const int i,
                                  const TaskParallelTLS *__restrict /*tls*/)
{
  SculptThreadedTaskData *data = static_cast<SculptThreadedTaskData *>(userdata);
  SculptSession *ss = data->ob->sculpt;
  PBVHNode *node = data->nodes[i];
  PBVHVertexIter vd;
  int update_it = data->mask_expand_update_it;

  PBVHVertRef active_vertex = SCULPT_active_vertex_get(ss);
  int active_vertex_i = BKE_pbvh_vertex_to_index(ss->pbvh, active_vertex);

  bool face_sets_changed = false;

  BKE_pbvh_vertex_iter_begin (ss->pbvh, node, vd, PBVH_ITER_ALL) {
    int vi = vd.index;
    float final_mask = *vd.mask;
    if (data->mask_expand_use_normals) {
      if (ss->filter_cache->normal_factor[active_vertex_i] <
          ss->filter_cache->normal_factor[vd.index]) {
        final_mask = 1.0f;
      }
      else {
        final_mask = 0.0f;
      }
    }
    else {
      if (ss->filter_cache->mask_update_it[vi] <= update_it &&
          ss->filter_cache->mask_update_it[vi] != 0) {
        final_mask = 1.0f;
      }
      else {
        final_mask = 0.0f;
      }
    }

    if (data->mask_expand_create_face_set) {
      if (final_mask == 1.0f) {
        SCULPT_vertex_face_set_set(ss, vd.vertex, ss->filter_cache->new_face_set);
        face_sets_changed = true;
      }
      BKE_pbvh_node_mark_redraw(node);
    }
    else {

      if (data->mask_expand_keep_prev_mask) {
        final_mask = MAX2(ss->filter_cache->prev_mask[vd.index], final_mask);
      }

      if (data->mask_expand_invert_mask) {
        final_mask = 1.0f - final_mask;
      }

      if (*vd.mask != final_mask) {
        *vd.mask = final_mask;
        BKE_pbvh_node_mark_update_mask(node);
      }
    }
  }
  BKE_pbvh_vertex_iter_end;

  if (face_sets_changed) {
    SCULPT_undo_push_node(data->ob, node, SCULPT_UNDO_FACE_SETS);
  }
}

static int sculpt_mask_expand_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  ARegion *region = CTX_wm_region(C);
  float prev_click_f[2];
  copy_v2_v2(prev_click_f, static_cast<float *>(op->customdata));
  const int prev_click[2] = {int(prev_click_f[0]), int(prev_click_f[1])};
  int len = int(len_v2v2_int(prev_click, event->mval));
  len = abs(len);
  int mask_speed = RNA_int_get(op->ptr, "mask_speed");
  int mask_expand_update_it = len / mask_speed;
  mask_expand_update_it = mask_expand_update_it + 1;

  const bool create_face_set = RNA_boolean_get(op->ptr, "create_face_set");

  if (RNA_boolean_get(op->ptr, "use_cursor")) {
    SculptCursorGeometryInfo sgi;

    const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
    if (SCULPT_cursor_geometry_info_update(C, &sgi, mval_fl, false)) {
      int active_vertex_i = BKE_pbvh_vertex_to_index(ss->pbvh, SCULPT_active_vertex_get(ss));

      /* The cursor is over the mesh, get the update iteration from the updated active vertex. */
      mask_expand_update_it = ss->filter_cache->mask_update_it[active_vertex_i];
    }
    else {
      /* When the cursor is outside the mesh, affect the entire connected component. */
      mask_expand_update_it = ss->filter_cache->mask_update_last_it - 1;
    }
  }

  if ((event->type == EVT_ESCKEY && event->val == KM_PRESS) ||
      (event->type == RIGHTMOUSE && event->val == KM_PRESS))
  {
    /* Returning OPERATOR_CANCELLED will leak memory due to not finishing
     * undo. Better solution could be to make paint_mesh_restore_co work
     * for this case. */
    sculpt_mask_expand_cancel(C, op);
    return OPERATOR_FINISHED;
  }

  if ((event->type == LEFTMOUSE && event->val == KM_RELEASE) ||
      (event->type == EVT_RETKEY && event->val == KM_PRESS) ||
      (event->type == EVT_PADENTER && event->val == KM_PRESS))
  {

    /* Smooth iterations. */
    BKE_sculpt_update_object_for_edit(depsgraph, ob, true, false, false);
    const int smooth_iterations = RNA_int_get(op->ptr, "smooth_iterations");
    SCULPT_mask_filter_smooth_apply(sd, ob, ss->filter_cache->nodes, smooth_iterations);

    /* Pivot position. */
    if (RNA_boolean_get(op->ptr, "update_pivot")) {
      const char symm = SCULPT_mesh_symmetry_xyz_get(ob);
      const float threshold = 0.2f;
      float avg[3];
      int total = 0;
      zero_v3(avg);

      for (PBVHNode *node : ss->filter_cache->nodes) {
        PBVHVertexIter vd;
        BKE_pbvh_vertex_iter_begin (ss->pbvh, node, vd, PBVH_ITER_UNIQUE) {
          const float mask = (vd.mask) ? *vd.mask : 0.0f;
          if (mask < (0.5f + threshold) && mask > (0.5f - threshold)) {
            if (SCULPT_check_vertex_pivot_symmetry(
                    vd.co, ss->filter_cache->mask_expand_initial_co, symm)) {
              add_v3_v3(avg, vd.co);
              total++;
            }
          }
        }
        BKE_pbvh_vertex_iter_end;
      }

      if (total > 0) {
        mul_v3_fl(avg, 1.0f / total);
        copy_v3_v3(ss->pivot_pos, avg);
      }
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob->data);
    }

    MEM_freeN(op->customdata);

    for (PBVHNode *node : ss->filter_cache->nodes) {
      BKE_pbvh_node_mark_redraw(node);
    }

    SCULPT_filter_cache_free(ss);

    SCULPT_undo_push_end(ob);
    SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);
    ED_workspace_status_text(C, nullptr);
    return OPERATOR_FINISHED;
  }

  /* When pressing Ctrl, expand directly to the max number of iterations. This allows to flood fill
   * mask and face sets by connectivity directly. */
  if (event->modifier & KM_CTRL) {
    mask_expand_update_it = ss->filter_cache->mask_update_last_it - 1;
  }

  if (!ELEM(event->type, MOUSEMOVE, EVT_LEFTCTRLKEY, EVT_RIGHTCTRLKEY)) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (mask_expand_update_it == ss->filter_cache->mask_update_current_it) {
    ED_region_tag_redraw(region);
    return OPERATOR_RUNNING_MODAL;
  }

  if (mask_expand_update_it < ss->filter_cache->mask_update_last_it) {

    if (create_face_set) {
      for (int i = 0; i < ss->totfaces; i++) {
        ss->face_sets[i] = ss->filter_cache->prev_face_set[i];
      }
    }
    SculptThreadedTaskData data{};
    data.sd = sd;
    data.ob = ob;
    data.nodes = ss->filter_cache->nodes;
    data.mask_expand_update_it = mask_expand_update_it;
    data.mask_expand_use_normals = RNA_boolean_get(op->ptr, "use_normals");
    data.mask_expand_invert_mask = RNA_boolean_get(op->ptr, "invert");
    data.mask_expand_keep_prev_mask = RNA_boolean_get(op->ptr, "keep_previous_mask");
    data.mask_expand_create_face_set = RNA_boolean_get(op->ptr, "create_face_set");

    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, ss->filter_cache->nodes.size());
    BLI_task_parallel_range(
        0, ss->filter_cache->nodes.size(), &data, sculpt_expand_task_cb, &settings);
    ss->filter_cache->mask_update_current_it = mask_expand_update_it;
  }

  SCULPT_flush_update_step(C, SCULPT_UPDATE_MASK);

  return OPERATOR_RUNNING_MODAL;
}

struct MaskExpandFloodFillData {
  float original_normal[3];
  float edge_sensitivity;
  bool use_normals;
};

static bool mask_expand_floodfill_cb(
    SculptSession *ss, PBVHVertRef from_v, PBVHVertRef to_v, bool is_duplicate, void *userdata)
{
  MaskExpandFloodFillData *data = static_cast<MaskExpandFloodFillData *>(userdata);

  int from_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, from_v);
  int to_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, to_v);

  if (!is_duplicate) {
    int to_it = ss->filter_cache->mask_update_it[from_v_i] + 1;
    ss->filter_cache->mask_update_it[to_v_i] = to_it;
    if (to_it > ss->filter_cache->mask_update_last_it) {
      ss->filter_cache->mask_update_last_it = to_it;
    }

    if (data->use_normals) {
      float current_normal[3], prev_normal[3];
      SCULPT_vertex_normal_get(ss, to_v, current_normal);
      SCULPT_vertex_normal_get(ss, from_v, prev_normal);
      const float from_edge_factor = ss->filter_cache->edge_factor[from_v_i];
      ss->filter_cache->edge_factor[to_v_i] = dot_v3v3(current_normal, prev_normal) *
                                              from_edge_factor;
      ss->filter_cache->normal_factor[to_v_i] = dot_v3v3(data->original_normal, current_normal) *
                                                powf(from_edge_factor, data->edge_sensitivity);
      CLAMP(ss->filter_cache->normal_factor[to_v_i], 0.0f, 1.0f);
    }
  }
  else {
    /* PBVH_GRIDS duplicate handling. */
    ss->filter_cache->mask_update_it[to_v_i] = ss->filter_cache->mask_update_it[from_v_i];
    if (data->use_normals) {
      ss->filter_cache->edge_factor[to_v_i] = ss->filter_cache->edge_factor[from_v_i];
      ss->filter_cache->normal_factor[to_v_i] = ss->filter_cache->normal_factor[from_v_i];
    }
  }

  return true;
}

static int sculpt_mask_expand_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  PBVH *pbvh = ob->sculpt->pbvh;

  const bool use_normals = RNA_boolean_get(op->ptr, "use_normals");
  const bool create_face_set = RNA_boolean_get(op->ptr, "create_face_set");

  SculptCursorGeometryInfo sgi;
  const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};

  MultiresModifierData *mmd = BKE_sculpt_multires_active(CTX_data_scene(C), ob);
  BKE_sculpt_mask_layers_ensure(depsgraph, CTX_data_main(C), ob, mmd);

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);

  SCULPT_vertex_random_access_ensure(ss);

  op->customdata = MEM_mallocN(sizeof(float[2]), "initial mouse position");
  copy_v2_v2(static_cast<float *>(op->customdata), mval_fl);

  SCULPT_cursor_geometry_info_update(C, &sgi, mval_fl, false);

  int vertex_count = SCULPT_vertex_count_get(ss);

  ss->filter_cache = MEM_new<FilterCache>(__func__);

  ss->filter_cache->nodes = blender::bke::pbvh::search_gather(pbvh, nullptr, nullptr);

  SCULPT_undo_push_begin(ob, op);

  if (create_face_set) {
    for (PBVHNode *node : ss->filter_cache->nodes) {
      BKE_pbvh_node_mark_redraw(node);
      SCULPT_undo_push_node(ob, node, SCULPT_UNDO_FACE_SETS);
    }
  }
  else {
    for (PBVHNode *node : ss->filter_cache->nodes) {
      SCULPT_undo_push_node(ob, node, SCULPT_UNDO_MASK);
      BKE_pbvh_node_mark_redraw(node);
    }
  }

  ss->filter_cache->mask_update_it = MEM_cnew_array<int>(vertex_count, __func__);
  if (use_normals) {
    ss->filter_cache->normal_factor = MEM_cnew_array<float>(vertex_count, __func__);
    ss->filter_cache->edge_factor = MEM_cnew_array<float>(vertex_count, __func__);
    for (int i = 0; i < vertex_count; i++) {
      ss->filter_cache->edge_factor[i] = 1.0f;
    }
  }

  if (create_face_set) {
    ss->filter_cache->prev_face_set = MEM_cnew_array<int>(ss->totfaces, __func__);
    for (int i = 0; i < ss->totfaces; i++) {
      ss->filter_cache->prev_face_set[i] = ss->face_sets ? ss->face_sets[i] : 0;
    }
    ss->filter_cache->new_face_set = SCULPT_face_set_next_available_get(ss);
  }
  else {
    ss->filter_cache->prev_mask = MEM_cnew_array<float>(vertex_count, __func__);
    for (int i = 0; i < vertex_count; i++) {
      PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

      ss->filter_cache->prev_mask[i] = SCULPT_vertex_mask_get(ss, vertex);
    }
  }

  int active_vertex_i = BKE_pbvh_vertex_to_index(ss->pbvh, SCULPT_active_vertex_get(ss));

  ss->filter_cache->mask_update_last_it = 1;
  ss->filter_cache->mask_update_current_it = 1;
  ss->filter_cache->mask_update_it[active_vertex_i] = 0;

  copy_v3_v3(ss->filter_cache->mask_expand_initial_co, SCULPT_active_vertex_co_get(ss));

  SculptFloodFill flood;
  SCULPT_floodfill_init(ss, &flood);
  SCULPT_floodfill_add_active(sd, ob, ss, &flood, FLT_MAX);

  MaskExpandFloodFillData fdata{};
  fdata.use_normals = use_normals;
  fdata.edge_sensitivity = RNA_int_get(op->ptr, "edge_sensitivity");

  SCULPT_active_vertex_normal_get(ss, fdata.original_normal);
  SCULPT_floodfill_execute(ss, &flood, mask_expand_floodfill_cb, &fdata);
  SCULPT_floodfill_free(&flood);

  if (use_normals) {
    for (int repeat = 0; repeat < 2; repeat++) {
      for (int i = 0; i < vertex_count; i++) {
        PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

        float avg = 0.0f;
        SculptVertexNeighborIter ni;
        SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
          avg += ss->filter_cache->normal_factor[ni.index];
        }
        SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
        ss->filter_cache->normal_factor[i] = avg / ni.size;
      }
    }

    MEM_SAFE_FREE(ss->filter_cache->edge_factor);
  }

  SculptThreadedTaskData data{};
  data.sd = sd;
  data.ob = ob;
  data.nodes = ss->filter_cache->nodes;
  data.mask_expand_update_it = 0;
  data.mask_expand_use_normals = RNA_boolean_get(op->ptr, "use_normals");
  data.mask_expand_invert_mask = RNA_boolean_get(op->ptr, "invert");
  data.mask_expand_keep_prev_mask = RNA_boolean_get(op->ptr, "keep_previous_mask");
  data.mask_expand_create_face_set = RNA_boolean_get(op->ptr, "create_face_set");

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, ss->filter_cache->nodes.size());
  BLI_task_parallel_range(
      0, ss->filter_cache->nodes.size(), &data, sculpt_expand_task_cb, &settings);

  const char *status_str = TIP_(
      "Move the mouse to expand the mask from the active vertex. LMB: confirm mask, ESC/RMB: "
      "cancel");
  ED_workspace_status_text(C, status_str);

  SCULPT_flush_update_step(C, SCULPT_UPDATE_MASK);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

void SCULPT_OT_mask_expand(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Mask Expand";
  ot->idname = "SCULPT_OT_mask_expand";
  ot->description = "Expands a mask from the initial active vertex under the cursor";

  /* API callbacks. */
  ot->invoke = sculpt_mask_expand_invoke;
  ot->modal = sculpt_mask_expand_modal;
  ot->cancel = sculpt_mask_expand_cancel;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->prop = RNA_def_boolean(ot->srna, "invert", true, "Invert", "Invert the new mask");
  ot->prop = RNA_def_boolean(
      ot->srna, "use_cursor", true, "Use Cursor", "Expand the mask to the cursor position");
  ot->prop = RNA_def_boolean(ot->srna,
                             "update_pivot",
                             true,
                             "Update Pivot Position",
                             "Set the pivot position to the mask border after creating the mask");
  ot->prop = RNA_def_int(ot->srna, "smooth_iterations", 2, 0, 10, "Smooth Iterations", "", 0, 10);
  ot->prop = RNA_def_int(ot->srna, "mask_speed", 5, 1, 10, "Mask Speed", "", 1, 10);

  ot->prop = RNA_def_boolean(ot->srna,
                             "use_normals",
                             true,
                             "Use Normals",
                             "Generate the mask using the normals and curvature of the model");
  ot->prop = RNA_def_boolean(ot->srna,
                             "keep_previous_mask",
                             false,
                             "Keep Previous Mask",
                             "Generate the new mask on top of the current one");
  ot->prop = RNA_def_int(ot->srna,
                         "edge_sensitivity",
                         300,
                         0,
                         2000,
                         "Edge Detection Sensitivity",
                         "Sensitivity for expanding the mask across sculpted sharp edges when "
                         "using normals to generate the mask",
                         0,
                         2000);
  ot->prop = RNA_def_boolean(ot->srna,
                             "create_face_set",
                             false,
                             "Expand Face Mask",
                             "Expand a new Face Mask instead of the sculpt mask");
}
