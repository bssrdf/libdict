/*
 * libdict -- skiplist implementation.
 * cf. [Pugh 1990], [Sedgewick 1998]
 *
 * Copyright (c) 2001-2011, Farooq Mela
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Farooq Mela.
 * 4. Neither the name of the Farooq Mela nor the
 *    names of contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY FAROOQ MELA ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL FAROOQ MELA BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>

#include "skiplist.h"
#include "dict_private.h"

typedef struct skip_node skip_node;

struct skip_node {
    void*		    key;
    void*		    datum;
    skip_node*		    prev;
    unsigned		    link_count;
    skip_node*		    link[0];
};

#define MAX_LINK	    32

struct skiplist {
    skip_node*		    head;
    unsigned		    max_link;
    unsigned		    top_link;
    dict_compare_func	    cmp_func;
    dict_delete_func	    del_func;
    size_t		    count;
    unsigned		    randgen;
};

#define RGEN_A		    1664525U
#define RGEN_M		    1013904223U

struct skiplist_itor {
    skiplist*		    list;
    skip_node*		    node;
};

static dict_vtable skiplist_vtable = {
    (dict_inew_func)	    skiplist_dict_itor_new,
    (dict_dfree_func)	    skiplist_free,
    (dict_insert_func)	    skiplist_insert,
    (dict_probe_func)	    skiplist_probe,
    (dict_search_func)	    skiplist_search,
    (dict_remove_func)	    skiplist_remove,
    (dict_clear_func)	    skiplist_clear,
    (dict_traverse_func)    skiplist_traverse,
    (dict_count_func)	    skiplist_count
};

static itor_vtable skiplist_itor_vtable = {
    (dict_ifree_func)	    skiplist_itor_free,
    (dict_valid_func)	    skiplist_itor_valid,
    (dict_invalidate_func)  skiplist_itor_invalidate,
    (dict_next_func)	    skiplist_itor_next,
    (dict_prev_func)	    skiplist_itor_prev,
    (dict_nextn_func)	    skiplist_itor_nextn,
    (dict_prevn_func)	    skiplist_itor_prevn,
    (dict_first_func)	    skiplist_itor_first,
    (dict_last_func)	    skiplist_itor_last,
    (dict_key_func)	    skiplist_itor_key,
    (dict_data_func)	    skiplist_itor_data,
    (dict_dataset_func)	    skiplist_itor_set_data,
    (dict_iremove_func)	    NULL,/* skiplist_itor_remove not implemented yet */
    (dict_icompare_func)    NULL/* skiplist_itor_compare not implemented yet */
};

static skip_node*   node_new(void *key, void *datum, unsigned link_count);
static skip_node*   node_insert(skiplist *list, void *key, void *datum,
				skip_node **update);
static unsigned	    rand_link_count(skiplist *list);

skiplist *
skiplist_new(dict_compare_func cmp_func, dict_delete_func del_func,
	     unsigned max_link)
{
    ASSERT(max_link > 0);

    if (max_link > MAX_LINK)
	max_link = MAX_LINK;

    skiplist *list = MALLOC(sizeof(*list));
    if (list) {
	if (!(list->head = node_new(NULL, NULL, max_link))) {
	    FREE(list);
	    return NULL;
	}

	list->max_link = max_link;
	list->top_link = 0;
	list->cmp_func = cmp_func ? cmp_func : dict_ptr_cmp;
	list->del_func = del_func;
	list->count = 0;
	list->randgen = rand();
    }
    return list;
}

dict *
skiplist_dict_new(dict_compare_func cmp_func, dict_delete_func del_func,
		  unsigned max_link) {
    dict *dct = MALLOC(sizeof(*dct));
    if (dct) {
	if (!(dct->_object = skiplist_new(cmp_func, del_func, max_link))) {
	    FREE(dct);
	    return NULL;
	}
	dct->_vtable = &skiplist_vtable;
    }
    return dct;
}

size_t
skiplist_free(skiplist *list)
{
    ASSERT(list != NULL);

    size_t count = skiplist_clear(list);
    FREE(list->head);
    FREE(list);
    return count;
}

skip_node *
node_insert(skiplist *list, void *key, void *datum, skip_node **update)
{
    const unsigned nlinks = rand_link_count(list);
    ASSERT(nlinks < list->max_link);
    skip_node *x = node_new(key, datum, nlinks);
    if (!x)
	return NULL;

    if (list->top_link < nlinks) {
	for (unsigned k = list->top_link+1; k <= nlinks; k++) {
	    ASSERT(!update[k]);
	    update[k] = list->head;
	}
	list->top_link = nlinks;
    }

    x->prev = update[0];
    if (update[0]->link[0])
	update[0]->link[0]->prev = x;
    for (unsigned k = 0; k < nlinks; k++) {
	ASSERT(update[k]->link_count > k);
	x->link[k] = update[k]->link[k];
	update[k]->link[k] = x;
    }
    list->count++;
    return x;
}

int
skiplist_insert(skiplist *list, void *key, void *datum, bool overwrite)
{
    ASSERT(list != NULL);

    skip_node *x = list->head, *update[MAX_LINK] = { 0 };
    for (unsigned k = list->top_link+1; k-->0; ) {
	ASSERT(x->link_count > k);
	while (x->link[k] && list->cmp_func(key, x->link[k]->key) > 0)
	    x = x->link[k];
	update[k] = x;
    }
    x = x->link[0];
    if (x && list->cmp_func(key, x->key) == 0) {
	if (!overwrite)
	    return 1;
	if (list->del_func)
	    list->del_func(x->key, x->datum);
	x->key = key;
	x->datum = datum;
	return 0;
    }
    return node_insert(list, key, datum, update) ? 0 : -1;
}

int
skiplist_probe(skiplist *list, void *key, void **datum)
{
    ASSERT(list != NULL);

    skip_node *x = list->head, *update[MAX_LINK] = { 0 };
    for (unsigned k = list->top_link+1; k-->0; ) {
	ASSERT(x->link_count > k);
	while (x->link[k]) {
	    int cmp = list->cmp_func(key, x->link[k]->key);
	    if (cmp < 0)
		break;
	    x = x->link[k];
	    ASSERT(x->link_count > k);
	    if (cmp == 0) {
		*datum = x->datum;
		return 0;
	    }
	}
	update[k] = x;
    }
    return node_insert(list, key, *datum, update) ? 1 : -1;
}

void *
skiplist_search(skiplist *list, const void *key)
{
    ASSERT(list != NULL);

    skip_node *x = list->head;
    for (unsigned k = list->top_link+1; k-->0;) {
	while (x->link[k]) {
	    int cmp = list->cmp_func(key, x->link[k]->key);
	    if (cmp < 0)
		break;
	    x = x->link[k];
	    if (cmp == 0)
		return x->datum;
	}
    }
    return NULL;
}

bool
skiplist_remove(skiplist *list, const void *key)
{
    ASSERT(list != NULL);

    skip_node *x = list->head, *update[MAX_LINK] = { 0 };
    for (unsigned k = list->top_link+1; k-->0;) {
	ASSERT(x->link_count > k);
	while (x->link[k] && list->cmp_func(key, x->link[k]->key) > 0)
	    x = x->link[k];
	update[k] = x;
    }
    x = x->link[0];
    if (!x || list->cmp_func(key, x->key) != 0)
	return false;
    for (unsigned k = 0; k <= list->top_link; k++) {
	ASSERT(update[k] != NULL);
	ASSERT(update[k]->link_count > k);
	if (update[k]->link[k] != x)
	    break;
	update[k]->link[k] = x->link[k];
    }
    if (list->del_func)
	list->del_func(x->key, x->datum);
    FREE(x);
    while (list->top_link > 0 && !list->head->link[list->top_link])
	list->top_link--;
    list->count--;
    return true;
}

size_t
skiplist_clear(skiplist *list)
{
    ASSERT(list != NULL);

    skip_node *node = list->head->link[0];
    while (node) {
	skip_node *next = node->link[0];
	if (list->del_func)
	    list->del_func(node->key, node->datum);
	FREE(node);
	node = next;
    }

    const size_t count = list->count;
    list->count = 0;
    list->head->link[list->top_link] = NULL;
    while (list->top_link)
	list->head->link[--list->top_link] = NULL;

    return count;
}

size_t
skiplist_traverse(skiplist *list, dict_visit_func visit)
{
    ASSERT(list != NULL);
    ASSERT(visit != NULL);

    size_t count = 0;
    for (skip_node *node = list->head->link[0]; node; node = node->link[0]) {
	++count;
	if (!visit(node->key, node->datum))
	    break;
    }
    return count;
}

size_t
skiplist_count(const skiplist *list)
{
    ASSERT(list != NULL);

    return list->count;
}

#define VALID(itor) ((itor)->node && (itor)->node != (itor)->list->head)

skiplist_itor *
skiplist_itor_new(skiplist *list)
{
    ASSERT(list != NULL);

    skiplist_itor *itor = MALLOC(sizeof(*itor));
    if (itor) {
	itor->list = list;
	itor->node = NULL;
	skiplist_itor_first(itor);
    }
    return itor;
}

dict_itor *
skiplist_dict_itor_new(skiplist *list)
{
    ASSERT(list != NULL);

    dict_itor *itor = MALLOC(sizeof(*itor));
    if (itor) {
	if (!(itor->_itor = skiplist_itor_new(list))) {
	    FREE(itor);
	    return NULL;
	}
	itor->_vtable = &skiplist_itor_vtable;
    }
    return itor;
}

void
skiplist_itor_free(skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    FREE(itor);
}

bool
skiplist_itor_valid(const skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    return VALID(itor);
}

void
skiplist_itor_invalidate(skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    itor->node = NULL;
}

bool
skiplist_itor_next(skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    if (!itor->node)
	return skiplist_itor_first(itor);

    itor->node = itor->node->link[0];
    return VALID(itor);
}

bool
skiplist_itor_prev(skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    if (!itor->node)
	return skiplist_itor_last(itor);

    itor->node = itor->node->prev;
    return VALID(itor);
}

bool
skiplist_itor_nextn(skiplist_itor *itor, size_t count)
{
    ASSERT(itor != NULL);

    while (count--)
	if (!skiplist_itor_next(itor))
	    return false;
    return VALID(itor);
}

bool
skiplist_itor_prevn(skiplist_itor *itor, size_t count)
{
    ASSERT(itor != NULL);

    while (count--)
	if (!skiplist_itor_prev(itor))
	    return false;
    return VALID(itor);
}

bool
skiplist_itor_first(skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    itor->node = itor->list->head->link[0];
    return VALID(itor);
}

bool
skiplist_itor_last(skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    skip_node *x = itor->list->head;
    for (unsigned k = itor->list->top_link; k-->0;) {
	while (x->link[k])
	    x = x->link[k];
    }
    if (x == itor->list->head) {
	itor->node = NULL;
	return false;
    } else {
	itor->node = x;
	return true;
    }
}

bool
skiplist_itor_search(skiplist_itor *itor, const void *key)
{
    ASSERT(itor != NULL);

    skiplist *list = itor->list;
    skip_node *x = list->head;
    for (unsigned k = list->top_link+1; k-->0;) {
	while (x->link[k]) {
	    int cmp = list->cmp_func(key, x->link[k]->key);
	    if (cmp < 0)
		break;
	    x = x->link[k];
	    if (cmp == 0) {
		itor->node = x;
		return true;
	    }
	}
    }
    itor->node = NULL;
    return false;
}

const void *
skiplist_itor_key(const skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    return itor->node ? itor->node->key : NULL;
}

void *
skiplist_itor_data(skiplist_itor *itor)
{
    ASSERT(itor != NULL);

    return itor->node ? itor->node->datum : NULL;
}

bool
skiplist_itor_set_data(skiplist_itor *itor, void *datum, void **prev_datum)
{
    ASSERT(itor != NULL);

    if (!itor->node)
	return false;

    if (prev_datum)
	*prev_datum = itor->node->datum;
    itor->node->datum = datum;
    return true;
}

skip_node*
node_new(void *key, void *datum, unsigned link_count)
{
    ASSERT(link_count >= 1);

    skip_node *node = MALLOC(sizeof(*node) +
			     sizeof(node->link[0]) * link_count);
    if (node) {
	node->key = key;
	node->datum = datum;
	node->prev = NULL;
	node->link_count = link_count;
	memset(node->link, 0, sizeof(node->link[0]) * link_count);
    }
    return node;
}

static unsigned
rand_link_count(skiplist *list)
{
    unsigned r = list->randgen = list->randgen * RGEN_A + RGEN_M;
    unsigned i = 1;
    for (; i+1<list->max_link; ++i)
	if (r > (1U<<(32-i)))
/*	if (r & (1U<<(32-i))) */
	    break;
    return i;
}

void
skiplist_verify(const skiplist *list)
{
    ASSERT(list != NULL);
    ASSERT(list->top_link <= list->max_link);

    const skip_node *node = list->head->link[0];
    while (node != NULL) {
	ASSERT(node->link_count >= 1);
	ASSERT(node->link_count <= list->top_link);
	for (unsigned k = 0; k < node->link_count; k++) {
	    if (node->link[k]) {
		ASSERT(node->link[k]->link_count >= k);
	    }
	}
	node = node->link[0];
    }
}
