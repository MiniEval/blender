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

#include "BLI_ghash.h"

struct Editing;
struct GSet;
struct GSetIterator;
struct Sequence;

#define SEQ_ITERATOR_FOREACH(var, collection) \
  for (SeqIterator iter = {{{NULL}}}; \
       SEQ_iterator_ensure(collection, &iter, &var) && var != NULL; \
       var = SEQ_iterator_yield(&iter))

typedef struct SeqCollection {
  struct GSet *set;
} SeqCollection;

typedef struct SeqIterator {
  GSetIterator gsi;
  SeqCollection *collection;
  bool iterator_initialized;
} SeqIterator;

bool SEQ_iterator_ensure(SeqCollection *collection,
                         SeqIterator *iterator,
                         struct Sequence **r_seq);
struct Sequence *SEQ_iterator_yield(SeqIterator *iterator);

/* Callback format for the for_each function below. */
typedef bool (*SeqForEachFunc)(struct Sequence *seq, void *user_data);

void SEQ_for_each_callback(struct ListBase *seqbase, SeqForEachFunc callback, void *user_data);

SeqCollection *SEQ_collection_create(const char *name);
SeqCollection *SEQ_collection_duplicate(SeqCollection *collection);
uint SEQ_collection_len(const SeqCollection *collection);
bool SEQ_collection_has_strip(const struct Sequence *seq, const SeqCollection *collection);
bool SEQ_collection_append_strip(struct Sequence *seq, SeqCollection *data);
bool SEQ_collection_remove_strip(struct Sequence *seq, SeqCollection *data);
void SEQ_collection_free(SeqCollection *collection);
void SEQ_collection_merge(SeqCollection *collection_dst, SeqCollection *collection_src);
void SEQ_collection_exclude(SeqCollection *collection, SeqCollection *exclude_elements);
void SEQ_collection_expand(struct ListBase *seqbase,
                           SeqCollection *collection,
                           void query_func(struct Sequence *seq_reference,
                                           struct ListBase *seqbase,
                                           SeqCollection *collection));
SeqCollection *SEQ_query_by_reference(struct Sequence *seq_reference,
                                      struct ListBase *seqbase,
                                      void seq_query_func(struct Sequence *seq_reference,
                                                          struct ListBase *seqbase,
                                                          SeqCollection *collection));
SeqCollection *SEQ_query_selected_strips(struct ListBase *seqbase);
SeqCollection *SEQ_query_all_strips(struct ListBase *seqbase);
SeqCollection *SEQ_query_all_strips_recursive(struct ListBase *seqbase);
void SEQ_query_strip_effect_chain(struct Sequence *seq_reference,
                                  struct ListBase *seqbase,
                                  SeqCollection *collection);

#ifdef __cplusplus
}
#endif
