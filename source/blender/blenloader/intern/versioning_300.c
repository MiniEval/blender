/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup blenloader
 */
/* allow readfile to use deprecated functionality */
#define DNA_DEPRECATED_ALLOW

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_brush_types.h"
#include "DNA_collection_types.h"
#include "DNA_constraint_types.h"
#include "DNA_curve_types.h"
#include "DNA_genfile.h"
#include "DNA_listBase.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_text_types.h"
#include "DNA_workspace_types.h"

#include "BKE_action.h"
#include "BKE_animsys.h"
#include "BKE_asset.h"
#include "BKE_collection.h"
#include "BKE_deform.h"
#include "BKE_fcurve.h"
#include "BKE_fcurve_driver.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_node.h"

#include "BLO_readfile.h"
#include "MEM_guardedalloc.h"
#include "readfile.h"

#include "SEQ_sequencer.h"

#include "RNA_access.h"

#include "versioning_common.h"

static void sort_linked_ids(Main *bmain)
{
  ListBase *lb;
  FOREACH_MAIN_LISTBASE_BEGIN (bmain, lb) {
    ListBase temp_list;
    BLI_listbase_clear(&temp_list);
    LISTBASE_FOREACH_MUTABLE (ID *, id, lb) {
      if (ID_IS_LINKED(id)) {
        BLI_remlink(lb, id);
        BLI_addtail(&temp_list, id);
        id_sort_by_name(&temp_list, id, NULL);
      }
    }
    BLI_movelisttolist(lb, &temp_list);
  }
  FOREACH_MAIN_LISTBASE_END;
}

static void assert_sorted_ids(Main *bmain)
{
#ifndef NDEBUG
  ListBase *lb;
  FOREACH_MAIN_LISTBASE_BEGIN (bmain, lb) {
    ID *id_prev = NULL;
    LISTBASE_FOREACH (ID *, id, lb) {
      if (id_prev == NULL) {
        continue;
      }
      BLI_assert(id_prev->lib != id->lib || BLI_strcasecmp(id_prev->name, id->name) < 0);
    }
  }
  FOREACH_MAIN_LISTBASE_END;
#else
  UNUSED_VARS_NDEBUG(bmain);
#endif
}

static void move_vertex_group_names_to_object_data(Main *bmain)
{
  LISTBASE_FOREACH (Object *, object, &bmain->objects) {
    if (ELEM(object->type, OB_MESH, OB_LATTICE, OB_GPENCIL)) {
      ListBase *new_defbase = BKE_object_defgroup_list_mutable(object);

      /* Choose the longest vertex group name list among all linked duplicates. */
      if (BLI_listbase_count(&object->defbase) < BLI_listbase_count(new_defbase)) {
        BLI_freelistN(&object->defbase);
      }
      else {
        /* Clear the list in case the it was already assigned from another object. */
        BLI_freelistN(new_defbase);
        *new_defbase = object->defbase;
      }
    }
  }
}

static void do_versions_sequencer_speed_effect_recursive(Scene *scene, const ListBase *seqbase)
{
  /* Old SpeedControlVars->flags. */
#define SEQ_SPEED_INTEGRATE (1 << 0)
#define SEQ_SPEED_COMPRESS_IPO_Y (1 << 2)

  LISTBASE_FOREACH (Sequence *, seq, seqbase) {
    if (seq->type == SEQ_TYPE_SPEED) {
      SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
      const char *substr = NULL;
      float globalSpeed = v->globalSpeed;
      if (seq->flag & SEQ_USE_EFFECT_DEFAULT_FADE) {
        if (globalSpeed == 1.0f) {
          v->speed_control_type = SEQ_SPEED_STRETCH;
        }
        else {
          v->speed_control_type = SEQ_SPEED_MULTIPLY;
          v->speed_fader = globalSpeed *
                           ((float)seq->seq1->len /
                            max_ff((float)(seq->seq1->enddisp - seq->seq1->start), 1.0f));
        }
      }
      else if (v->flags & SEQ_SPEED_INTEGRATE) {
        v->speed_control_type = SEQ_SPEED_MULTIPLY;
        v->speed_fader = seq->speed_fader * globalSpeed;
      }
      else if (v->flags & SEQ_SPEED_COMPRESS_IPO_Y) {
        globalSpeed *= 100.0f;
        v->speed_control_type = SEQ_SPEED_LENGTH;
        v->speed_fader_length = seq->speed_fader * globalSpeed;
        substr = "speed_length";
      }
      else {
        v->speed_control_type = SEQ_SPEED_FRAME_NUMBER;
        v->speed_fader_frame_number = (int)(seq->speed_fader * globalSpeed);
        substr = "speed_frame_number";
      }

      v->flags &= ~(SEQ_SPEED_INTEGRATE | SEQ_SPEED_COMPRESS_IPO_Y);

      if (substr || globalSpeed != 1.0f) {
        FCurve *fcu = id_data_find_fcurve(&scene->id, seq, &RNA_Sequence, "speed_factor", 0, NULL);
        if (fcu) {
          if (globalSpeed != 1.0f) {
            for (int i = 0; i < fcu->totvert; i++) {
              BezTriple *bezt = &fcu->bezt[i];
              bezt->vec[0][1] *= globalSpeed;
              bezt->vec[1][1] *= globalSpeed;
              bezt->vec[2][1] *= globalSpeed;
            }
          }
          if (substr) {
            char *new_path = BLI_str_replaceN(fcu->rna_path, "speed_factor", substr);
            MEM_freeN(fcu->rna_path);
            fcu->rna_path = new_path;
          }
        }
      }
    }
    else if (seq->type == SEQ_TYPE_META) {
      do_versions_sequencer_speed_effect_recursive(scene, &seq->seqbase);
    }
  }

#undef SEQ_SPEED_INTEGRATE
#undef SEQ_SPEED_COMPRESS_IPO_Y
}

void do_versions_after_linking_300(Main *bmain, ReportList *UNUSED(reports))
{
  if (MAIN_VERSION_ATLEAST(bmain, 300, 0) && !MAIN_VERSION_ATLEAST(bmain, 300, 1)) {
    /* Set zero user text objects to have a fake user. */
    LISTBASE_FOREACH (Text *, text, &bmain->texts) {
      if (text->id.us == 0) {
        id_fake_user_set(&text->id);
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 3)) {
    /* Use new texture socket in Attribute Sample Texture node. */
    LISTBASE_FOREACH (bNodeTree *, ntree, &bmain->nodetrees) {
      if (ntree->type != NTREE_GEOMETRY) {
        continue;
      }
      LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
        if (node->type != GEO_NODE_ATTRIBUTE_SAMPLE_TEXTURE) {
          continue;
        }
        if (node->id == NULL) {
          continue;
        }
        LISTBASE_FOREACH (bNodeSocket *, socket, &node->inputs) {
          if (socket->type == SOCK_TEXTURE) {
            bNodeSocketValueTexture *socket_value = (bNodeSocketValueTexture *)
                                                        socket->default_value;
            socket_value->value = (Tex *)node->id;
            break;
          }
        }
        node->id = NULL;
      }
    }

    sort_linked_ids(bmain);
    assert_sorted_ids(bmain);
  }

  if (MAIN_VERSION_ATLEAST(bmain, 300, 3)) {
    assert_sorted_ids(bmain);
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 11)) {
    move_vertex_group_names_to_object_data(bmain);
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 13)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      if (scene->ed != NULL) {
        do_versions_sequencer_speed_effect_recursive(scene, &scene->ed->seqbase);
      }
    }
  }

  /**
   * Versioning code until next subversion bump goes here.
   *
   * \note Be sure to check when bumping the version:
   * - #blo_do_versions_300 in this file.
   * - "versioning_userdef.c", #blo_do_versions_userdef
   * - "versioning_userdef.c", #do_versions_theme
   *
   * \note Keep this message at the bottom of the function.
   */
  {
    /* Keep this block, even when empty. */
  }
}

static void version_switch_node_input_prefix(Main *bmain)
{
  FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
    if (ntree->type == NTREE_GEOMETRY) {
      LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
        if (node->type == GEO_NODE_SWITCH) {
          LISTBASE_FOREACH (bNodeSocket *, socket, &node->inputs) {
            /* Skip the "switch" socket. */
            if (socket == node->inputs.first) {
              continue;
            }
            strcpy(socket->name, socket->name[0] == 'A' ? "False" : "True");

            /* Replace "A" and "B", but keep the unique number suffix at the end. */
            char number_suffix[8];
            BLI_strncpy(number_suffix, socket->identifier + 1, sizeof(number_suffix));
            strcpy(socket->identifier, socket->name);
            strcat(socket->identifier, number_suffix);
          }
        }
      }
    }
  }
  FOREACH_NODETREE_END;
}

static void version_node_socket_name(bNodeTree *ntree,
                                     const int node_type,
                                     const char *old_name,
                                     const char *new_name)
{
  LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
    if (node->type == node_type) {
      LISTBASE_FOREACH (bNodeSocket *, socket, &node->inputs) {
        if (STREQ(socket->name, old_name)) {
          strcpy(socket->name, new_name);
        }
        if (STREQ(socket->identifier, old_name)) {
          strcpy(socket->identifier, new_name);
        }
      }
      LISTBASE_FOREACH (bNodeSocket *, socket, &node->outputs) {
        if (STREQ(socket->name, old_name)) {
          strcpy(socket->name, new_name);
        }
        if (STREQ(socket->identifier, old_name)) {
          strcpy(socket->identifier, new_name);
        }
      }
    }
  }
}

static bool replace_bbone_len_scale_rnapath(char **p_old_path, int *p_index)
{
  char *old_path = *p_old_path;

  if (old_path == NULL) {
    return false;
  }

  int len = strlen(old_path);

  if (BLI_str_endswith(old_path, ".bbone_curveiny") ||
      BLI_str_endswith(old_path, ".bbone_curveouty")) {
    old_path[len - 1] = 'z';
    return true;
  }

  if (BLI_str_endswith(old_path, ".bbone_scaleinx") ||
      BLI_str_endswith(old_path, ".bbone_scaleiny") ||
      BLI_str_endswith(old_path, ".bbone_scaleoutx") ||
      BLI_str_endswith(old_path, ".bbone_scaleouty")) {
    int index = (old_path[len - 1] == 'y' ? 2 : 0);

    old_path[len - 1] = 0;

    if (p_index) {
      *p_index = index;
    }
    else {
      *p_old_path = BLI_sprintfN("%s[%d]", old_path, index);
      MEM_freeN(old_path);
    }

    return true;
  }

  return false;
}

static void do_version_bbone_len_scale_fcurve_fix(FCurve *fcu)
{
  /* Update driver variable paths. */
  if (fcu->driver) {
    LISTBASE_FOREACH (DriverVar *, dvar, &fcu->driver->variables) {
      DRIVER_TARGETS_LOOPER_BEGIN (dvar) {
        replace_bbone_len_scale_rnapath(&dtar->rna_path, NULL);
      }
      DRIVER_TARGETS_LOOPER_END;
    }
  }

  /* Update F-Curve's path. */
  replace_bbone_len_scale_rnapath(&fcu->rna_path, &fcu->array_index);
}

static void do_version_bbone_len_scale_animdata_cb(ID *UNUSED(id),
                                                   AnimData *adt,
                                                   void *UNUSED(wrapper_data))
{
  LISTBASE_FOREACH_MUTABLE (FCurve *, fcu, &adt->drivers) {
    do_version_bbone_len_scale_fcurve_fix(fcu);
  }
}

static void do_version_bones_bbone_len_scale(ListBase *lb)
{
  LISTBASE_FOREACH (Bone *, bone, lb) {
    if (bone->flag & BONE_ADD_PARENT_END_ROLL) {
      bone->bbone_flag |= BBONE_ADD_PARENT_END_ROLL;
    }

    copy_v3_fl3(bone->scale_in, bone->scale_in_x, 1.0f, bone->scale_in_z);
    copy_v3_fl3(bone->scale_out, bone->scale_out_x, 1.0f, bone->scale_out_z);

    do_version_bones_bbone_len_scale(&bone->childbase);
  }
}

static void do_version_constraints_spline_ik_joint_bindings(ListBase *lb)
{
  /* Binding array data could be freed without properly resetting its size data. */
  LISTBASE_FOREACH (bConstraint *, con, lb) {
    if (con->type == CONSTRAINT_TYPE_SPLINEIK) {
      bSplineIKConstraint *data = (bSplineIKConstraint *)con->data;
      if (data->points == NULL) {
        data->numpoints = 0;
      }
    }
  }
}

/* NOLINTNEXTLINE: readability-function-size */
void blo_do_versions_300(FileData *fd, Library *UNUSED(lib), Main *bmain)
{
  if (!MAIN_VERSION_ATLEAST(bmain, 300, 1)) {
    /* Set default value for the new bisect_threshold parameter in the mirror modifier. */
    if (!DNA_struct_elem_find(fd->filesdna, "MirrorModifierData", "float", "bisect_threshold")) {
      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
          if (md->type == eModifierType_Mirror) {
            MirrorModifierData *mmd = (MirrorModifierData *)md;
            /* This was the previous hard-coded value. */
            mmd->bisect_threshold = 0.001f;
          }
        }
      }
    }
    /* Grease Pencil: Set default value for dilate pixels. */
    if (!DNA_struct_elem_find(fd->filesdna, "BrushGpencilSettings", "int", "dilate_pixels")) {
      LISTBASE_FOREACH (Brush *, brush, &bmain->brushes) {
        if (brush->gpencil_settings) {
          brush->gpencil_settings->dilate_pixels = 1;
        }
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 2)) {
    version_switch_node_input_prefix(bmain);

    if (!DNA_struct_elem_find(fd->filesdna, "bPoseChannel", "float", "custom_scale_xyz[3]")) {
      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        if (ob->pose == NULL) {
          continue;
        }
        LISTBASE_FOREACH (bPoseChannel *, pchan, &ob->pose->chanbase) {
          copy_v3_fl(pchan->custom_scale_xyz, pchan->custom_scale);
        }
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 4)) {
    /* Add a properties sidebar to the spreadsheet editor. */
    LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
          if (sl->spacetype == SPACE_SPREADSHEET) {
            ListBase *regionbase = (sl == area->spacedata.first) ? &area->regionbase :
                                                                   &sl->regionbase;
            ARegion *new_sidebar = do_versions_add_region_if_not_found(
                regionbase, RGN_TYPE_UI, "sidebar for spreadsheet", RGN_TYPE_FOOTER);
            if (new_sidebar != NULL) {
              new_sidebar->alignment = RGN_ALIGN_RIGHT;
              new_sidebar->flag |= RGN_FLAG_HIDDEN;
            }
          }
        }
      }
    }

    /* Enable spreadsheet filtering in old files without row filters. */
    LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
          if (sl->spacetype == SPACE_SPREADSHEET) {
            SpaceSpreadsheet *sspreadsheet = (SpaceSpreadsheet *)sl;
            sspreadsheet->filter_flag |= SPREADSHEET_FILTER_ENABLE;
          }
        }
      }
    }

    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      if (ntree->type == NTREE_GEOMETRY) {
        version_node_socket_name(ntree, GEO_NODE_BOUNDING_BOX, "Mesh", "Bounding Box");
      }
    }
    FOREACH_NODETREE_END;

    if (!DNA_struct_elem_find(fd->filesdna, "FileAssetSelectParams", "int", "import_type")) {
      LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
        LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
          LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
            if (sl->spacetype == SPACE_FILE) {
              SpaceFile *sfile = (SpaceFile *)sl;
              if (sfile->asset_params) {
                sfile->asset_params->import_type = FILE_ASSET_IMPORT_APPEND;
              }
            }
          }
        }
      }
    }

    /* Initialize length-wise scale B-Bone settings. */
    if (!DNA_struct_elem_find(fd->filesdna, "Bone", "int", "bbone_flag")) {
      /* Update armature data and pose channels. */
      LISTBASE_FOREACH (bArmature *, arm, &bmain->armatures) {
        do_version_bones_bbone_len_scale(&arm->bonebase);
      }

      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        if (ob->pose) {
          LISTBASE_FOREACH (bPoseChannel *, pchan, &ob->pose->chanbase) {
            copy_v3_fl3(pchan->scale_in, pchan->scale_in_x, 1.0f, pchan->scale_in_z);
            copy_v3_fl3(pchan->scale_out, pchan->scale_out_x, 1.0f, pchan->scale_out_z);
          }
        }
      }

      /* Update action curves and drivers. */
      LISTBASE_FOREACH (bAction *, act, &bmain->actions) {
        LISTBASE_FOREACH_MUTABLE (FCurve *, fcu, &act->curves) {
          do_version_bbone_len_scale_fcurve_fix(fcu);
        }
      }

      BKE_animdata_main_cb(bmain, do_version_bbone_len_scale_animdata_cb, NULL);
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 5)) {
    /* Add a dataset sidebar to the spreadsheet editor. */
    LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
          if (sl->spacetype == SPACE_SPREADSHEET) {
            ListBase *regionbase = (sl == area->spacedata.first) ? &area->regionbase :
                                                                   &sl->regionbase;
            ARegion *spreadsheet_dataset_region = do_versions_add_region_if_not_found(
                regionbase, RGN_TYPE_CHANNELS, "spreadsheet dataset region", RGN_TYPE_FOOTER);

            if (spreadsheet_dataset_region) {
              spreadsheet_dataset_region->alignment = RGN_ALIGN_LEFT;
              spreadsheet_dataset_region->v2d.scroll = (V2D_SCROLL_RIGHT | V2D_SCROLL_BOTTOM);
            }
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 6)) {
    LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        LISTBASE_FOREACH (SpaceLink *, space, &area->spacedata) {
          /* Disable View Layers filter. */
          if (space->spacetype == SPACE_OUTLINER) {
            SpaceOutliner *space_outliner = (SpaceOutliner *)space;
            space_outliner->filter |= SO_FILTER_NO_VIEW_LAYERS;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 7)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      ToolSettings *tool_settings = scene->toolsettings;
      tool_settings->snap_flag |= SCE_SNAP_SEQ;
      short snap_mode = tool_settings->snap_mode;
      short snap_node_mode = tool_settings->snap_node_mode;
      short snap_uv_mode = tool_settings->snap_uv_mode;
      tool_settings->snap_mode &= ~((1 << 4) | (1 << 5) | (1 << 6));
      tool_settings->snap_node_mode &= ~((1 << 5) | (1 << 6));
      tool_settings->snap_uv_mode &= ~(1 << 4);
      if (snap_mode & (1 << 4)) {
        tool_settings->snap_mode |= (1 << 6); /* SCE_SNAP_MODE_INCREMENT */
      }
      if (snap_mode & (1 << 5)) {
        tool_settings->snap_mode |= (1 << 4); /* SCE_SNAP_MODE_EDGE_MIDPOINT */
      }
      if (snap_mode & (1 << 6)) {
        tool_settings->snap_mode |= (1 << 5); /* SCE_SNAP_MODE_EDGE_PERPENDICULAR */
      }
      if (snap_node_mode & (1 << 5)) {
        tool_settings->snap_node_mode |= (1 << 0); /* SCE_SNAP_MODE_NODE_X */
      }
      if (snap_node_mode & (1 << 6)) {
        tool_settings->snap_node_mode |= (1 << 1); /* SCE_SNAP_MODE_NODE_Y */
      }
      if (snap_uv_mode & (1 << 4)) {
        tool_settings->snap_uv_mode |= (1 << 6); /* SCE_SNAP_MODE_INCREMENT */
      }

      SequencerToolSettings *sequencer_tool_settings = SEQ_tool_settings_ensure(scene);
      sequencer_tool_settings->snap_mode = SEQ_SNAP_TO_STRIPS | SEQ_SNAP_TO_CURRENT_FRAME |
                                           SEQ_SNAP_TO_STRIP_HOLD;
      sequencer_tool_settings->snap_distance = 15;
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 8)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      if (scene->master_collection != NULL) {
        BLI_strncpy(scene->master_collection->id.name + 2,
                    BKE_SCENE_COLLECTION_NAME,
                    sizeof(scene->master_collection->id.name) - 2);
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 9)) {
    /* Fix a bug where reordering FCurves and bActionGroups could cause some corruption. Just
     * reconstruct all the action groups & ensure that the FCurves of a group are continuously
     * stored (i.e. not mixed with other groups) to be sure. See T89435. */
    LISTBASE_FOREACH (bAction *, act, &bmain->actions) {
      BKE_action_groups_reconstruct(act);
    }

    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      if (ntree->type == NTREE_GEOMETRY) {
        LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
          if (node->type == GEO_NODE_MESH_SUBDIVIDE) {
            strcpy(node->idname, "GeometryNodeMeshSubdivide");
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 10)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      ToolSettings *tool_settings = scene->toolsettings;
      if (tool_settings->snap_uv_mode & (1 << 4)) {
        tool_settings->snap_uv_mode |= (1 << 6); /* SCE_SNAP_MODE_INCREMENT */
        tool_settings->snap_uv_mode &= ~(1 << 4);
      }
    }
    LISTBASE_FOREACH (Material *, mat, &bmain->materials) {
      if (!(mat->lineart.flags & LRT_MATERIAL_CUSTOM_OCCLUSION_EFFECTIVENESS)) {
        mat->lineart.mat_occlusion = 1;
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 13)) {
    /* Convert Surface Deform to sparse-capable bind structure. */
    if (!DNA_struct_elem_find(
            fd->filesdna, "SurfaceDeformModifierData", "int", "num_mesh_verts")) {
      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
          if (md->type == eModifierType_SurfaceDeform) {
            SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;
            if (smd->num_bind_verts && smd->verts) {
              smd->num_mesh_verts = smd->num_bind_verts;

              for (unsigned int i = 0; i < smd->num_bind_verts; i++) {
                smd->verts[i].vertex_idx = i;
              }
            }
          }
        }
      }
    }

    if (!DNA_struct_elem_find(
            fd->filesdna, "WorkSpace", "AssetLibraryReference", "asset_library")) {
      LISTBASE_FOREACH (WorkSpace *, workspace, &bmain->workspaces) {
        BKE_asset_library_reference_init_default(&workspace->asset_library_ref);
      }
    }

    if (!DNA_struct_elem_find(
            fd->filesdna, "FileAssetSelectParams", "AssetLibraryReference", "asset_library_ref")) {
      LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
        LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
          LISTBASE_FOREACH (SpaceLink *, space, &area->spacedata) {
            if (space->spacetype == SPACE_FILE) {
              SpaceFile *sfile = (SpaceFile *)space;
              if (sfile->browse_mode != FILE_BROWSE_MODE_ASSETS) {
                continue;
              }
              BKE_asset_library_reference_init_default(&sfile->asset_params->asset_library_ref);
            }
          }
        }
      }
    }

    /* Set default 2D annotation placement. */
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      ToolSettings *ts = scene->toolsettings;
      ts->gpencil_v2d_align = GP_PROJECT_VIEWSPACE | GP_PROJECT_CURSOR;
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 14)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      ToolSettings *tool_settings = scene->toolsettings;
      tool_settings->snap_flag &= ~SCE_SNAP_SEQ;
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 15)) {
    LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
          if (sl->spacetype == SPACE_SEQ) {
            SpaceSeq *sseq = (SpaceSeq *)sl;
            sseq->flag |= SEQ_SHOW_GRID;
          }
        }
      }
    }
  }

  /* Font names were copied directly into ID names, see: T90417. */
  if (!MAIN_VERSION_ATLEAST(bmain, 300, 16)) {
    ListBase *lb = which_libbase(bmain, ID_VF);
    BKE_main_id_repair_duplicate_names_listbase(lb);
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 17)) {
    if (!DNA_struct_elem_find(
            fd->filesdna, "View3DOverlay", "float", "normals_constant_screen_size")) {
      LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
        LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
          LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
            if (sl->spacetype == SPACE_VIEW3D) {
              View3D *v3d = (View3D *)sl;
              v3d->overlay.normals_constant_screen_size = 7.0f;
            }
          }
        }
      }
    }

    /* Fix SplineIK constraint's inconsistency between binding points array and its stored size. */
    LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
      /* NOTE: Objects should never have SplineIK constraint, so no need to apply this fix on
       * their constraints. */
      if (ob->pose) {
        LISTBASE_FOREACH (bPoseChannel *, pchan, &ob->pose->chanbase) {
          do_version_constraints_spline_ik_joint_bindings(&pchan->constraints);
        }
      }
    }
  }

  if (!MAIN_VERSION_ATLEAST(bmain, 300, 18)) {
    if (!DNA_struct_elem_find(
            fd->filesdna, "WorkSpace", "AssetLibraryReference", "asset_library_ref")) {
      LISTBASE_FOREACH (WorkSpace *, workspace, &bmain->workspaces) {
        BKE_asset_library_reference_init_default(&workspace->asset_library_ref);
      }
    }

    if (!DNA_struct_elem_find(
            fd->filesdna, "FileAssetSelectParams", "AssetLibraryReference", "asset_library_ref")) {
      LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
        LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
          LISTBASE_FOREACH (SpaceLink *, space, &area->spacedata) {
            if (space->spacetype != SPACE_FILE) {
              continue;
            }

            SpaceFile *sfile = (SpaceFile *)space;
            if (sfile->browse_mode != FILE_BROWSE_MODE_ASSETS) {
              continue;
            }
            BKE_asset_library_reference_init_default(&sfile->asset_params->asset_library_ref);
          }
        }
      }
    }

    /* Previously, only text ending with `.py` would run, apply this logic
     * to existing files so text that happens to have the "Register" enabled
     * doesn't suddenly start running code on startup that was previously ignored. */
    LISTBASE_FOREACH (Text *, text, &bmain->texts) {
      if ((text->flags & TXT_ISSCRIPT) && !BLI_path_extension_check(text->id.name + 2, ".py")) {
        text->flags &= ~TXT_ISSCRIPT;
      }
    }
  }

  /**
   * Versioning code until next subversion bump goes here.
   *
   * \note Be sure to check when bumping the version:
   * - "versioning_userdef.c", #blo_do_versions_userdef
   * - "versioning_userdef.c", #do_versions_theme
   *
   * \note Keep this message at the bottom of the function.
   */
  {
    /* Keep this block, even when empty. */

    /* Add node storage for subdivision surface node. */
    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      if (ntree->type == NTREE_GEOMETRY) {
        LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
          if (node->type == GEO_NODE_SUBDIVISION_SURFACE) {
            if (node->storage == NULL) {
              NodeGeometrySubdivisionSurface *data = MEM_callocN(
                  sizeof(NodeGeometrySubdivisionSurface), __func__);
              data->uv_smooth = SUBSURF_UV_SMOOTH_PRESERVE_BOUNDARIES;
              data->boundary_smooth = SUBSURF_BOUNDARY_SMOOTH_ALL;
              node->storage = data;
            }
          }
        }
      }
    }
    FOREACH_NODETREE_END;

    /* Disable Fade Inactive Overlay by default as it is redundant after introducing flash on mode
     * transfer. */
    for (bScreen *screen = bmain->screens.first; screen; screen = screen->id.next) {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
          if (sl->spacetype == SPACE_VIEW3D) {
            View3D *v3d = (View3D *)sl;
            v3d->overlay.flag &= ~V3D_OVERLAY_FADE_INACTIVE;
          }
        }
      }
    }
  }
}
