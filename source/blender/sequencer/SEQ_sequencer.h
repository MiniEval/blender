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
 *
 * The Original Code is Copyright (C) 2004 Blender Foundation.
 * All rights reserved.
 */

#pragma once

/** \file
 * \ingroup sequencer
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "DNA_scene_types.h"

struct BlendDataReader;
struct BlendExpander;
struct BlendLibReader;
struct BlendWriter;
struct Depsgraph;
struct Editing;
struct Scene;
struct Sequence;
struct SequenceLookup;
struct SequencerToolSettings;

/* RNA enums, just to be more readable */
enum {
  SEQ_SIDE_MOUSE = -1,
  SEQ_SIDE_NONE = 0,
  SEQ_SIDE_LEFT,
  SEQ_SIDE_RIGHT,
  SEQ_SIDE_BOTH,
  SEQ_SIDE_NO_CHANGE,
};

/* seq_dupli' flags */
#define SEQ_DUPE_UNIQUE_NAME (1 << 0)
#define SEQ_DUPE_ALL (1 << 3) /* otherwise only selected are copied */
#define SEQ_DUPE_IS_RECURSIVE_CALL (1 << 4)

struct SequencerToolSettings *SEQ_tool_settings_init(void);
struct SequencerToolSettings *SEQ_tool_settings_ensure(struct Scene *scene);
void SEQ_tool_settings_free(struct SequencerToolSettings *tool_settings);
eSeqImageFitMethod SEQ_tool_settings_fit_method_get(struct Scene *scene);
void SEQ_tool_settings_fit_method_set(struct Scene *scene, eSeqImageFitMethod fit_method);
short SEQ_tool_settings_snap_flag_get(struct Scene *scene);
short SEQ_tool_settings_snap_mode_get(struct Scene *scene);
int SEQ_tool_settings_snap_distance_get(struct Scene *scene);
struct SequencerToolSettings *SEQ_tool_settings_copy(struct SequencerToolSettings *tool_settings);
struct Editing *SEQ_editing_get(struct Scene *scene, bool alloc);
struct Editing *SEQ_editing_ensure(struct Scene *scene);
void SEQ_editing_free(struct Scene *scene, const bool do_id_user);
struct ListBase *SEQ_active_seqbase_get(const struct Editing *ed);
void SEQ_seqbase_active_set(struct Editing *ed, struct ListBase *seqbase);
struct Sequence *SEQ_sequence_alloc(ListBase *lb, int timeline_frame, int machine, int type);
void SEQ_sequence_free(struct Scene *scene, struct Sequence *seq, const bool do_clean_animdata);
struct MetaStack *SEQ_meta_stack_alloc(struct Editing *ed, struct Sequence *seq_meta);
struct MetaStack *SEQ_meta_stack_active_get(const struct Editing *ed);
void SEQ_meta_stack_free(struct Editing *ed, struct MetaStack *ms);
void SEQ_offset_animdata(struct Scene *scene, struct Sequence *seq, int ofs);
void SEQ_dupe_animdata(struct Scene *scene, const char *name_src, const char *name_dst);
struct Sequence *SEQ_sequence_dupli_recursive(const struct Scene *scene_src,
                                              struct Scene *scene_dst,
                                              struct ListBase *new_seq_list,
                                              struct Sequence *seq,
                                              int dupe_flag);
void SEQ_sequence_base_dupli_recursive(const struct Scene *scene_src,
                                       struct Scene *scene_dst,
                                       struct ListBase *nseqbase,
                                       const struct ListBase *seqbase,
                                       int dupe_flag,
                                       const int flag);

/* Read and Write functions for .blend file data */
void SEQ_blend_write(struct BlendWriter *writer, struct ListBase *seqbase);
void SEQ_blend_read(struct BlendDataReader *reader, struct ListBase *seqbase);

void SEQ_blend_read_lib(struct BlendLibReader *reader,
                        struct Scene *scene,
                        struct ListBase *seqbase);

void SEQ_blend_read_expand(struct BlendExpander *expander, struct ListBase *seqbase);

/* Depsgraph update function */
void SEQ_eval_sequences(struct Depsgraph *depsgraph,
                        struct Scene *scene,
                        struct ListBase *seqbase);

/* Defined in sequence_lookup.c */

typedef enum eSequenceLookupTag {
  SEQ_LOOKUP_TAG_INVALID = (1 << 0),
} eSequenceLookupTag;

struct Sequence *SEQ_sequence_lookup_by_name(const struct Scene *scene, const char *key);
void SEQ_sequence_lookup_free(const struct Scene *scene);
void SEQ_sequence_lookup_tag(const struct Scene *scene, eSequenceLookupTag tag);

#ifdef __cplusplus
}
#endif
