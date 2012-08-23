/*
 * libdict -- height-balanced (AVL) tree implementation.
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

#include "hb_tree.h"
#include "dict_private.h"

typedef struct hb_node hb_node;

struct hb_node {
    void*		    key;
    void*		    datum;
    hb_node*		    llink;
    hb_node*		    rlink;
    hb_node*		    parent;
    signed char		    bal;    /* TODO: store in unused low bits. */
};

struct hb_tree {
    hb_node*		    root;
    size_t		    count;
    dict_compare_func	    cmp_func;
    dict_delete_func	    del_func;
};

struct hb_itor {
    hb_tree*		    tree;
    hb_node*		    node;
};

static dict_vtable hb_tree_vtable = {
    (dict_inew_func)	    hb_dict_itor_new,
    (dict_dfree_func)	    hb_tree_free,
    (dict_insert_func)	    hb_tree_insert,
    (dict_probe_func)	    hb_tree_probe,
    (dict_search_func)	    hb_tree_search,
    (dict_remove_func)	    hb_tree_remove,
    (dict_clear_func)	    hb_tree_clear,
    (dict_traverse_func)    hb_tree_traverse,
    (dict_count_func)	    hb_tree_count
};

static itor_vtable hb_tree_itor_vtable = {
    (dict_ifree_func)	    hb_itor_free,
    (dict_valid_func)	    hb_itor_valid,
    (dict_invalidate_func)  hb_itor_invalidate,
    (dict_next_func)	    hb_itor_next,
    (dict_prev_func)	    hb_itor_prev,
    (dict_nextn_func)	    hb_itor_nextn,
    (dict_prevn_func)	    hb_itor_prevn,
    (dict_first_func)	    hb_itor_first,
    (dict_last_func)	    hb_itor_last,
    (dict_key_func)	    hb_itor_key,
    (dict_data_func)	    hb_itor_data,
    (dict_dataset_func)	    hb_itor_set_data,
    (dict_iremove_func)	    NULL,/* hb_itor_remove not implemented yet */
    (dict_icompare_func)    NULL /* hb_itor_compare not implemented yet */
};

static bool	rot_left(hb_tree *restrict tree, hb_node *restrict node);
static bool	rot_right(hb_tree *restrict tree, hb_node *restrict node);
static size_t	node_height(const hb_node *node);
static size_t	node_mheight(const hb_node *node);
static size_t	node_pathlen(const hb_node *node, size_t level);
static hb_node*	node_new(void *key, void *datum);
static hb_node*	node_min(hb_node *node);
static hb_node*	node_max(hb_node *node);
static hb_node*	node_next(hb_node *node);
static hb_node*	node_prev(hb_node *node);

hb_tree *
hb_tree_new(dict_compare_func cmp_func, dict_delete_func del_func)
{
    hb_tree *tree = MALLOC(sizeof(*tree));
    if (tree) {
	tree->root = NULL;
	tree->count = 0;
	tree->cmp_func = cmp_func ? cmp_func : dict_ptr_cmp;
	tree->del_func = del_func;
    }
    return tree;
}

dict *
hb_dict_new(dict_compare_func cmp_func, dict_delete_func del_func)
{
    dict *dct = MALLOC(sizeof(*dct));
    if (dct) {
	if (!(dct->_object = hb_tree_new(cmp_func, del_func))) {
	    FREE(dct);
	    return NULL;
	}
	dct->_vtable = &hb_tree_vtable;
    }
    return dct;
}

size_t
hb_tree_free(hb_tree *tree)
{
    ASSERT(tree != NULL);

    if (!tree->root)
	return 0;
    const size_t count = hb_tree_clear(tree);
    FREE(tree);
    return count;
}

size_t
hb_tree_clear(hb_tree *tree)
{
    ASSERT(tree != NULL);

    hb_node *node = tree->root;
    const size_t count = tree->count;
    while (node) {
	if (node->llink) {
	    node = node->llink;
	    continue;
	}
	if (node->rlink) {
	    node = node->rlink;
	    continue;
	}

	if (tree->del_func)
	    tree->del_func(node->key, node->datum);

	hb_node *parent = node->parent;
	FREE(node);
	tree->count--;

	if (parent) {
	    if (parent->llink == node)
		parent->llink = NULL;
	    else
		parent->rlink = NULL;
	}
	node = parent;
    }

    tree->root = NULL;
    ASSERT(tree->count == 0);

    return count;
}

void *
hb_tree_search(hb_tree *tree, const void *key)
{
    ASSERT(tree != NULL);

    hb_node *node = tree->root;
    while (node) {
	int cmp = tree->cmp_func(key, node->key);
	if (cmp < 0)
	    node = node->llink;
	else if (cmp > 0)
	    node = node->rlink;
	else
	    return node->datum;
    }

    return NULL;
}

int
hb_tree_insert(hb_tree *tree, void *key, void *datum, bool overwrite)
{
    ASSERT(tree != NULL);

    int cmp = 0;
    hb_node *node = tree->root, *parent = NULL, *q = NULL;
    while (node) {
	cmp = tree->cmp_func(key, node->key);
	if (cmp < 0)
	    parent = node, node = node->llink;
	else if (cmp > 0)
	    parent = node, node = node->rlink;
	else {
	    if (!overwrite)
		return 1;
	    if (tree->del_func)
		tree->del_func(node->key, node->datum);
	    node->key = key;
	    node->datum = datum;
	    return 0;
	}
	if (parent->bal)
	    q = parent;
    }

    if (!(node = node_new(key, datum)))
	return -1;
    if (!(node->parent = parent)) {
	tree->root = node;
	ASSERT(tree->count == 0);
	tree->count = 1;
	return 0;
    }
    if (cmp < 0)
	parent->llink = node;
    else
	parent->rlink = node;

    while (parent != q) {
	parent->bal = (parent->rlink == node) * 2 - 1;
	node = parent;
	parent = node->parent;
    }
    if (q) {
	if (q->llink == node) {
	    if (--q->bal == -2) {
		if (q->llink->bal > 0)
		    rot_left(tree, q->llink);
		rot_right(tree, q);
	    }
	} else {
	    if (++q->bal == +2) {
		if (q->rlink->bal < 0)
		    rot_right(tree, q->rlink);
		rot_left(tree, q);
	    }
	}
    }
    tree->count++;
    return 0;
}

int
hb_tree_probe(hb_tree *tree, void *key, void **datum)
{
    ASSERT(tree != NULL);

    int cmp = 0;
    hb_node *node = tree->root, *parent = NULL, *q = NULL;
    while (node) {
	cmp = tree->cmp_func(key, node->key);
	if (cmp < 0)
	    parent = node, node = node->llink;
	else if (cmp > 0)
	    parent = node, node = node->rlink;
	else {
	    *datum = node->datum;
	    return 0;
	}
	if (parent->bal)
	    q = parent;
    }

    if (!(node = node_new(key, *datum)))
	return -1;
    if (!(node->parent = parent)) {
	tree->root = node;
	ASSERT(tree->count == 0);
	tree->count = 1;
	return 1;
    }
    if (cmp < 0)
	parent->llink = node;
    else
	parent->rlink = node;

    while (parent != q) {
	parent->bal = (parent->rlink == node) * 2 - 1;
	node = parent;
	parent = parent->parent;
    }
    if (q) {
	if (q->llink == node) {
	    if (--q->bal == -2) {
		if (q->llink->bal > 0)
		    rot_left(tree, q->llink);
		rot_right(tree, q);
	    }
	} else {
	    if (++q->bal == +2) {
		if (q->rlink->bal < 0)
		    rot_right(tree, q->rlink);
		rot_left(tree, q);
	    }
	}
    }
    tree->count++;
    return 1;
}

bool
hb_tree_remove(hb_tree *tree, const void *key)
{
    ASSERT(tree != NULL);

    hb_node *node = tree->root, *parent = NULL;
    while (node) {
	int cmp = tree->cmp_func(key, node->key);
	if (cmp == 0)
	    break;
	parent = node;
	node = cmp < 0 ? node->llink : node->rlink;
    }
    if (!node)
	return false;

    if (node->llink && node->rlink) {
	hb_node *out = node->rlink;
	while (out->llink)
	    out = out->llink;
	void *tmp;
	SWAP(node->key, out->key, tmp);
	SWAP(node->datum, out->datum, tmp);
	node = out;
	parent = out->parent;
    }

    hb_node *out = node->llink ? node->llink : node->rlink;
    if (tree->del_func)
	tree->del_func(node->key, node->datum);
    FREE(node);
    if (out)
	out->parent = parent;
    if (!parent) {
	tree->root = out;
	tree->count--;
	return true;
    }

    bool left = parent->llink == node;
    if (left)
	parent->llink = out;
    else
	parent->rlink = out;

    for (;;) {
	if (left) {
	    if (++parent->bal == 0) {
		node = parent;
		goto higher;
	    }
	    if (parent->bal == +2) {
		ASSERT(parent->rlink != NULL);
		if (parent->rlink->bal < 0) {
		    rot_right(tree, parent->rlink);
		    rot_left(tree, parent);
		} else {
		    ASSERT(parent->rlink->rlink != NULL);
		    if (rot_left(tree, parent) == 0)
			break;
		}
	    } else {
		break;
	    }
	} else {
	    if (--parent->bal == 0) {
		node = parent;
		goto higher;
	    }
	    if (parent->bal == -2) {
		ASSERT(parent->llink != NULL);
		if (parent->llink->bal > 0) {
		    rot_left(tree, parent->llink);
		    rot_right(tree, parent);
		} else {
		    ASSERT(parent->llink->llink != NULL);
		    if (rot_right(tree, parent) == 0)
			break;
		}
	    } else {
		break;
	    }
	}

	/* Only get here on double rotations or single rotations that changed
	 * subtree height - in either event, `parent->parent' is positioned
	 * where `parent' was positioned before any rotations. */
	node = parent->parent;
higher:
	if (!(parent = node->parent))
	    break;
	left = (parent->llink == node);
    }
    tree->count--;
    return true;
}

const void *
hb_tree_min(const hb_tree *tree)
{
    ASSERT(tree != NULL);

    const hb_node *node = tree->root;
    if (!node)
	return NULL;
    for (; node->llink; node = node->llink)
	/* void */;
    return node->key;
}

const void *
hb_tree_max(const hb_tree *tree)
{
    ASSERT(tree != NULL);

    const hb_node *node = tree->root;
    if (!node)
	return NULL;
    for (; node->rlink; node = node->rlink)
	/* void */;
    return node->key;
}

size_t
hb_tree_traverse(hb_tree *tree, dict_visit_func visit)
{
    ASSERT(tree != NULL);

    if (!tree->root)
	return 0;

    size_t count = 0;
    for (hb_node *node = node_min(tree->root); node; node = node_next(node)) {
	++count;
	if (!visit(node->key, node->datum))
	    break;
    }
    return count;
}

size_t
hb_tree_count(const hb_tree *tree)
{
    ASSERT(tree != NULL);

    return tree->count;
}

size_t
hb_tree_height(const hb_tree *tree)
{
    ASSERT(tree != NULL);

    return tree->root ? node_height(tree->root) : 0;
}

size_t
hb_tree_mheight(const hb_tree *tree)
{
    ASSERT(tree != NULL);

    return tree->root ? node_mheight(tree->root) : 0;
}

size_t
hb_tree_pathlen(const hb_tree *tree)
{
    ASSERT(tree != NULL);

    return tree->root ? node_pathlen(tree->root, 1) : 0;
}

static hb_node *
node_new(void *key, void *datum)
{
    hb_node *node = MALLOC(sizeof(*node));
    if (node) {
	node->key = key;
	node->datum = datum;
	node->parent = NULL;
	node->llink = NULL;
	node->rlink = NULL;
	node->bal = 0;
    }
    return node;
}

static hb_node *
node_min(hb_node *node)
{
    ASSERT(node != NULL);

    while (node->llink)
	node = node->llink;
    return node;
}

static hb_node *
node_max(hb_node *node)
{
    ASSERT(node != NULL);

    while (node->rlink)
	node = node->rlink;
    return node;
}

static hb_node *
node_next(hb_node *node)
{
    ASSERT(node != NULL);

    if (node->rlink) {
	for (node = node->rlink; node->llink; node = node->llink)
	    /* void */;
	return node;
    }
    hb_node *temp = node->parent;
    while (temp && temp->rlink == node) {
	node = temp;
	temp = temp->parent;
    }
    return temp;
}

static hb_node *
node_prev(hb_node *node)
{
    ASSERT(node != NULL);

    if (node->llink) {
	for (node = node->llink; node->rlink; node = node->rlink)
	    /* void */;
	return node;
    }
    hb_node *temp = node->parent;
    while (temp && temp->llink == node) {
	node = temp;
	temp = temp->parent;
    }
    return temp;
}

static size_t
node_height(const hb_node *node)
{
    ASSERT(node != NULL);

    size_t l = node->llink ? node_height(node->llink) + 1 : 0;
    size_t r = node->rlink ? node_height(node->rlink) + 1 : 0;
    return MAX(l, r);
}

static size_t
node_mheight(const hb_node *node)
{
    ASSERT(node != NULL);

    size_t l = node->llink ? node_mheight(node->llink) + 1 : 0;
    size_t r = node->rlink ? node_mheight(node->rlink) + 1 : 0;
    return MIN(l, r);
}

static size_t
node_pathlen(const hb_node *node, size_t level)
{
    ASSERT(node != NULL);

    size_t n = 0;
    if (node->llink)
	n += level + node_pathlen(node->llink, level + 1);
    if (node->rlink)
	n += level + node_pathlen(node->rlink, level + 1);
    return n;
}

/*
 * rot_left(T, B):
 *
 *     /             /
 *    B             D
 *   / \           / \
 *  A   D   ==>   B   E
 *     / \       / \
 *    C   E     A   C
 *
 */
static bool
rot_left(hb_tree *restrict tree, hb_node *restrict node)
{
    ASSERT(tree != NULL);
    ASSERT(node != NULL);
    ASSERT(node->rlink != NULL);

    hb_node *rlink = node->rlink;
    node->rlink = rlink->llink;
    if (rlink->llink)
	rlink->llink->parent = node;
    hb_node *parent = node->parent;
    rlink->parent = parent;
    if (parent) {
	if (parent->llink == node)
	    parent->llink = rlink;
	else
	    parent->rlink = rlink;
    } else {
	tree->root = rlink;
    }
    rlink->llink = node;
    node->parent = rlink;

    bool hc = (rlink->bal != 0);
    node->bal  -= 1 + MAX(rlink->bal, 0);
    rlink->bal -= 1 - MIN(node->bal, 0);
    return hc;
}

/*
 * rot_right(T, D):
 *
 *       /           /
 *      D           B
 *     / \         / \
 *    B   E  ==>  A   D
 *   / \             / \
 *  A   C           C   E
 *
 */
static bool
rot_right(hb_tree *restrict tree, hb_node *restrict node)
{
    ASSERT(tree != NULL);
    ASSERT(node != NULL);
    ASSERT(node->llink != NULL);

    hb_node *llink = node->llink;
    node->llink = llink->rlink;
    if (llink->rlink)
	llink->rlink->parent = node;
    hb_node *parent = node->parent;
    llink->parent = parent;
    if (parent) {
	if (parent->llink == node)
	    parent->llink = llink;
	else
	    parent->rlink = llink;
    } else {
	tree->root = llink;
    }
    llink->rlink = node;
    node->parent = llink;

    bool hc = (llink->bal != 0);
    node->bal  += 1 - MIN(llink->bal, 0);
    llink->bal += 1 + MAX(node->bal, 0);
    return hc;
}

hb_itor *
hb_itor_new(hb_tree *tree)
{
    ASSERT(tree != NULL);

    hb_itor *itor = MALLOC(sizeof(*itor));
    if (itor) {
	itor->tree = tree;
	hb_itor_first(itor);
    }
    return itor;
}

dict_itor *
hb_dict_itor_new(hb_tree *tree)
{
    ASSERT(tree != NULL);

    dict_itor *itor = MALLOC(sizeof(*itor));
    if (itor) {
	if (!(itor->_itor = hb_itor_new(tree))) {
	    FREE(itor);
	    return NULL;
	}
	itor->_vtable = &hb_tree_itor_vtable;
    }
    return itor;
}

void
hb_itor_free(hb_itor *itor)
{
    ASSERT(itor != NULL);

    FREE(itor);
}

bool
hb_itor_valid(const hb_itor *itor)
{
    ASSERT(itor != NULL);

    return itor->node != NULL;
}

void
hb_itor_invalidate(hb_itor *itor)
{
    ASSERT(itor != NULL);

    itor->node = NULL;
}

bool
hb_itor_next(hb_itor *itor)
{
    ASSERT(itor != NULL);

    if (!itor->node)
	hb_itor_first(itor);
    else
	itor->node = node_next(itor->node);
    return itor->node != NULL;
}

bool
hb_itor_prev(hb_itor *itor)
{
    ASSERT(itor != NULL);

    if (!itor->node)
	hb_itor_last(itor);
    else
	itor->node = node_prev(itor->node);
    return itor->node != NULL;
}

bool
hb_itor_nextn(hb_itor *itor, size_t count)
{
    ASSERT(itor != NULL);

    while (count--)
	if (!hb_itor_prev(itor))
	    return false;
    return itor->node != NULL;
}

bool
hb_itor_prevn(hb_itor *itor, size_t count)
{
    ASSERT(itor != NULL);

    while (count--)
	if (!hb_itor_prev(itor))
	    return false;
    return itor->node != NULL;
}

bool
hb_itor_first(hb_itor *itor)
{
    ASSERT(itor != NULL);

    itor->node = itor->tree->root ? node_min(itor->tree->root) : NULL;
    return itor->node != NULL;
}

bool
hb_itor_last(hb_itor *itor)
{
    ASSERT(itor != NULL);

    itor->node = itor->tree->root ? node_max(itor->tree->root) : NULL;
    return itor->node != NULL;
}

bool
hb_itor_search(hb_itor *itor, const void *key)
{
    ASSERT(itor != NULL);

    hb_node *node = itor->tree->root;
    while (node) {
	int cmp = itor->tree->cmp_func(key, node->key);
	if (cmp == 0)
	    break;
	node = cmp < 0 ? node->llink : node->rlink;
    }
    itor->node = node;
    return itor->node != NULL;
}

const void *
hb_itor_key(const hb_itor *itor)
{
    ASSERT(itor != NULL);

    return itor->node ? itor->node->key : NULL;
}

void *
hb_itor_data(hb_itor *itor)
{
    ASSERT(itor != NULL);

    return itor->node ? itor->node->datum : NULL;
}

bool
hb_itor_set_data(hb_itor *itor, void *datum, void **old_datum)
{
    ASSERT(itor != NULL);

    if (!itor->node)
	return false;

    if (old_datum)
	*old_datum = itor->node->datum;
    itor->node->datum = datum;
    return true;
}
