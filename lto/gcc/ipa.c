/* Basic IPA optimizations and utilities.
   Copyright (C) 2003, 2004, 2005, 2007, 2008 Free Software Foundation,
   Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "cgraph.h"
#include "tree-pass.h"
#include "timevar.h"
#include "gimple.h"
#include "ggc.h"

/* Fill array order with all nodes with output flag set in the reverse
   topological order.  */

int
cgraph_postorder (struct cgraph_node **order)
{
  struct cgraph_node *node, *node2;
  int stack_size = 0;
  int order_pos = 0;
  struct cgraph_edge *edge, last;
  int pass;

  struct cgraph_node **stack =
    XCNEWVEC (struct cgraph_node *, cgraph_n_nodes);

  /* We have to deal with cycles nicely, so use a depth first traversal
     output algorithm.  Ignore the fact that some functions won't need
     to be output and put them into order as well, so we get dependencies
     right through inline functions.  */
  for (node = cgraph_nodes; node; node = node->next)
    node->aux = NULL;
  for (pass = 0; pass < 2; pass++)
    for (node = cgraph_nodes; node; node = node->next)
      if (!node->aux
	  && (pass || (node->needed && !node->address_taken)))
	{
	  node2 = node;
	  if (!node->callers)
	    node->aux = &last;
	  else
	    node->aux = node->callers;
	  while (node2)
	    {
	      while (node2->aux != &last)
		{
		  edge = (struct cgraph_edge *) node2->aux;
		  if (edge->next_caller)
		    node2->aux = edge->next_caller;
		  else
		    node2->aux = &last;
		  if (!edge->caller->aux)
		    {
		      if (!edge->caller->callers)
			edge->caller->aux = &last;
		      else
			edge->caller->aux = edge->caller->callers;
		      stack[stack_size++] = node2;
		      node2 = edge->caller;
		      break;
		    }
		}
	      if (node2->aux == &last)
		{
		  order[order_pos++] = node2;
		  if (stack_size)
		    node2 = stack[--stack_size];
		  else
		    node2 = NULL;
		}
	    }
	}
  free (stack);
  for (node = cgraph_nodes; node; node = node->next)
    node->aux = NULL;
  return order_pos;
}

/* Look for all functions inlined to NODE and update their inlined_to pointers
   to INLINED_TO.  */

static void
update_inlined_to_pointer (struct cgraph_node *node, struct cgraph_node *inlined_to)
{
  struct cgraph_edge *e;
  for (e = node->callees; e; e = e->next_callee)
    if (e->callee->global.inlined_to)
      {
        e->callee->global.inlined_to = inlined_to;
	update_inlined_to_pointer (e->callee, inlined_to);
      }
}

/* Perform reachability analysis and reclaim all unreachable nodes.
   If BEFORE_INLINING_P is true this function is called before inlining
   decisions has been made.  If BEFORE_INLINING_P is false this function also 
   removes unneeded bodies of extern inline functions.  */

bool
cgraph_remove_unreachable_nodes (bool before_inlining_p, FILE *file)
{
  struct cgraph_node *first = (struct cgraph_node *) (void *) 1;
  struct cgraph_node *node, *next;
  bool changed = false;

#ifdef ENABLE_CHECKING
  verify_cgraph ();
#endif
  if (file)
    fprintf (file, "\nReclaiming functions:");
#ifdef ENABLE_CHECKING
  for (node = cgraph_nodes; node; node = node->next)
    gcc_assert (!node->aux);
#endif
  for (node = cgraph_nodes; node; node = node->next)
    if (node->needed && !node->global.inlined_to
	&& ((!DECL_EXTERNAL (node->decl)) 
            || !node->analyzed
            || before_inlining_p))
      {
	node->aux = first;
	first = node;
      }
    else
      gcc_assert (!node->aux);

  /* Perform reachability analysis.  As a special case do not consider
     extern inline functions not inlined as live because we won't output
     them at all.  */
  while (first != (void *) 1)
    {
      struct cgraph_edge *e;
      node = first;
      first = (struct cgraph_node *) first->aux;

      for (e = node->callees; e; e = e->next_callee)
	if (!e->callee->aux
	    && node->analyzed
	    && (!e->inline_failed || !e->callee->analyzed
		|| (!DECL_EXTERNAL (e->callee->decl))
                || before_inlining_p))
	  {
	    e->callee->aux = first;
	    first = e->callee;
	  }
      while (node->clone_of && !node->clone_of->aux && !gimple_has_body_p (node->decl))
        {
	  node = node->clone_of;
	  node->aux = first;
	  first = node;
	}
    }

  /* Remove unreachable nodes.  Extern inline functions need special care;
     Unreachable extern inline functions shall be removed.
     Reachable extern inline functions we never inlined shall get their bodies
     eliminated.
     Reachable extern inline functions we sometimes inlined will be turned into
     unanalyzed nodes so they look like for true extern functions to the rest
     of code.  Body of such functions is released via remove_node once the
     inline clones are eliminated.  */
  for (node = cgraph_nodes; node; node = next)
    {
      next = node->next;
      if (!node->aux)
	{
          node->global.inlined_to = NULL;
	  if (file)
	    fprintf (file, " %s", cgraph_node_name (node));
	  if (!node->analyzed || !DECL_EXTERNAL (node->decl)
	      || before_inlining_p)
	    cgraph_remove_node (node);
	  else
	    {
	      struct cgraph_edge *e;

	      /* See if there is reachable caller.  */
	      for (e = node->callers; e; e = e->next_caller)
		if (e->caller->aux)
		  break;

	      /* If so, we need to keep node in the callgraph.  */
	      if (e || node->needed)
		{
		  struct cgraph_node *clone;

		  /* If there are still clones, we must keep body around.
		     Otherwise we can just remove the body but keep the clone.  */
		  for (clone = node->clones; clone;
		       clone = clone->next_sibling_clone)
		    if (clone->aux)
		      break;
		  if (!clone)
		    {
		      cgraph_release_function_body (node);
		      cgraph_node_remove_callees (node);
		      node->analyzed = false;
		      node->local.inlinable = false;
		    }
		}
	      else
		cgraph_remove_node (node);
	    }
	  changed = true;
	}
    }
  for (node = cgraph_nodes; node; node = node->next)
    {
      /* Inline clones might be kept around so their materializing allows further
         cloning.  If the function the clone is inlined into is removed, we need
         to turn it into normal cone.  */
      if (node->global.inlined_to
	  && !node->callers)
	{
	  gcc_assert (node->clones);
	  node->global.inlined_to = NULL;
	  update_inlined_to_pointer (node, node);
	}
      node->aux = NULL;
    }
#ifdef ENABLE_CHECKING
  verify_cgraph ();
#endif

  /* Reclaim alias pairs for functions that have disappeared from the
     call graph.  */
  remove_unreachable_alias_pairs ();

  return changed;
}

/* Mark visibility of all functions.

   A local function is one whose calls can occur only in the current
   compilation unit and all its calls are explicit, so we can change
   its calling convention.  We simply mark all static functions whose
   address is not taken as local.

   We also change the TREE_PUBLIC flag of all declarations that are public
   in language point of view but we want to overwrite this default
   via visibilities for the backend point of view.  */

static unsigned int
function_and_variable_visibility (void)
{
  struct cgraph_node *node;
  struct varpool_node *vnode;

  for (node = cgraph_nodes; node; node = node->next)
    {
      if (node->reachable
	  && (DECL_COMDAT (node->decl)
	      || (!flag_whole_program
		  && TREE_PUBLIC (node->decl) && !DECL_EXTERNAL (node->decl))))
	node->local.externally_visible = true;
      if (!node->local.externally_visible && node->analyzed
	  && !DECL_EXTERNAL (node->decl))
	{
	  gcc_assert (flag_whole_program || !TREE_PUBLIC (node->decl));
	  TREE_PUBLIC (node->decl) = 0;
	}
      node->local.local = (!node->needed
			   && node->analyzed
			   && !DECL_EXTERNAL (node->decl)
			   && !node->local.externally_visible);
    }
  for (vnode = varpool_nodes_queue; vnode; vnode = vnode->next_needed)
    {
      if (vnode->needed
	  && !flag_whole_program
	  && (DECL_COMDAT (vnode->decl) || TREE_PUBLIC (vnode->decl)))
	vnode->externally_visible = 1;
      if (!vnode->externally_visible)
	{
	  gcc_assert (flag_whole_program || !TREE_PUBLIC (vnode->decl));
	  TREE_PUBLIC (vnode->decl) = 0;
	}
     gcc_assert (TREE_STATIC (vnode->decl));
    }

  if (dump_file)
    {
      fprintf (dump_file, "\nMarking local functions:");
      for (node = cgraph_nodes; node; node = node->next)
	if (node->local.local)
	  fprintf (dump_file, " %s", cgraph_node_name (node));
      fprintf (dump_file, "\n\n");
      fprintf (dump_file, "\nMarking externally visible functions:");
      for (node = cgraph_nodes; node; node = node->next)
	if (node->local.externally_visible)
	  fprintf (dump_file, " %s", cgraph_node_name (node));
      fprintf (dump_file, "\n\n");
    }
  cgraph_function_flags_ready = true;
  return 0;
}

struct simple_ipa_opt_pass pass_ipa_function_and_variable_visibility = 
{
 {
  SIMPLE_IPA_PASS,
  "visibility",				/* name */
  NULL,					/* gate */
  function_and_variable_visibility,	/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_CGRAPHOPT,				/* tv_id */
  0,	                                /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_remove_functions | TODO_dump_cgraph/* todo_flags_finish */
 }
};


/* Hash a cgraph node set element.  */

static hashval_t
hash_cgraph_node_set_element (const void *p)
{
  const_cgraph_node_set_element element = (const_cgraph_node_set_element) p;
  return htab_hash_pointer (element->node);
}

/* Compare two cgraph node set elements.  */

static int
eq_cgraph_node_set_element (const void *p1, const void *p2)
{
  const_cgraph_node_set_element e1 = (const_cgraph_node_set_element) p1;
  const_cgraph_node_set_element e2 = (const_cgraph_node_set_element) p2;

  return e1->node == e2->node;
}

/* Create a new cgraph node set.  */

cgraph_node_set
cgraph_node_set_new (void)
{
  cgraph_node_set new_node_set;

  new_node_set = GGC_NEW (struct cgraph_node_set_def);
  new_node_set->hashtab = htab_create_ggc (10,
					   hash_cgraph_node_set_element,
					   eq_cgraph_node_set_element,
					   NULL);
  new_node_set->nodes = NULL;
  return new_node_set;
}

/* Add cgraph_node NODE to cgraph_node_set SET.  */

void
cgraph_node_set_add (cgraph_node_set set, struct cgraph_node *node)
{
  void **slot;
  cgraph_node_set_element element;
  struct cgraph_node_set_element_def dummy;

  dummy.node = node;
  slot = htab_find_slot (set->hashtab, &dummy, INSERT);

  if (*slot != HTAB_EMPTY_ENTRY)
    {
      element = (cgraph_node_set_element) *slot;
      gcc_assert (node == element->node
		  && (VEC_index (cgraph_node_ptr, set->nodes, element->index)
		      == node));
      return;
    }

  /* Insert node into hash table.  */
  element =
    (cgraph_node_set_element) GGC_NEW (struct cgraph_node_set_element_def);
  element->node = node;
  element->index = VEC_length (cgraph_node_ptr, set->nodes);
  *slot = element;

  /* Insert into node vector.  */
  VEC_safe_push (cgraph_node_ptr, gc, set->nodes, node);
}

/* Remove cgraph_node NODE from cgraph_node_set SET.  */

void
cgraph_node_set_remove (cgraph_node_set set, struct cgraph_node *node)
{
  void **slot, **last_slot;
  cgraph_node_set_element element, last_element;
  struct cgraph_node *last_node;
  struct cgraph_node_set_element_def dummy;

  dummy.node = node;
  slot = htab_find_slot (set->hashtab, &dummy, NO_INSERT);
  if (slot == NULL)
    return;

  element = (cgraph_node_set_element) *slot;
  gcc_assert (VEC_index (cgraph_node_ptr, set->nodes, element->index)
	      == node);

  /* Remove from vector. We do this by swapping node with the last element
     of the vector.  */
  last_node = VEC_pop (cgraph_node_ptr, set->nodes);
  if (last_node != node)
    {
      dummy.node = last_node;
      last_slot = htab_find_slot (set->hashtab, &dummy, NO_INSERT);
      last_element = (cgraph_node_set_element) *last_slot;
      gcc_assert (last_element);

      /* Move the last element to the original spot of NODE.  */
      last_element->index = element->index;
      VEC_replace (cgraph_node_ptr, set->nodes, last_element->index,
		   last_node);
    }
  
  /* Remove element from hash table.  */
  htab_clear_slot (set->hashtab, slot);
  ggc_free (element);
}

/* Find NODE in SET and return an iterator to it if found.  A null iterator
   is returned if NODE is not in SET.  */

cgraph_node_set_iterator
cgraph_node_set_find (cgraph_node_set set, struct cgraph_node *node)
{
  void **slot;
  struct cgraph_node_set_element_def dummy;
  cgraph_node_set_element element;
  cgraph_node_set_iterator csi;

  dummy.node = node;
  slot = htab_find_slot (set->hashtab, &dummy, NO_INSERT);
  if (slot == NULL)
    csi.index = (unsigned) ~0;
  else
    {
      element = (cgraph_node_set_element) *slot;
      gcc_assert (VEC_index (cgraph_node_ptr, set->nodes, element->index)
		  == node);
      csi.index = element->index;
    }
  csi.set = set;

  return csi;
}

/* Dump content of SET to file F.  */

void
dump_cgraph_node_set (FILE *f, cgraph_node_set set)
{
  cgraph_node_set_iterator iter;

  for (iter = csi_start (set); !csi_end_p (iter); csi_next (&iter))
    {
      struct cgraph_node *node = csi_node (iter);
      dump_cgraph_node (f, node);
    }
}

/* Dump content of SET to stderr.  */

void
debug_cgraph_node_set (cgraph_node_set set)
{
  dump_cgraph_node_set (stderr, set);
}

