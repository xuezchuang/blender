/* SPDX-FileCopyrightText: 2014 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 *
 * Implementation of tools for debugging the depsgraph
 */

#include "DNA_scene_types.h"

#include "BKE_global.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_debug.hh"
#include "DEG_depsgraph_query.hh"

#include "intern/debug/deg_debug.h"
#include "intern/depsgraph.hh"
#include "intern/depsgraph_relation.hh"
#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_id.hh"
#include "intern/node/deg_node_time.hh"

#include "BLI_math_bits.h"

namespace deg = blender::deg;

void DEG_debug_flags_set(Depsgraph *depsgraph, int flags)
{
  deg::Depsgraph *deg_graph = reinterpret_cast<deg::Depsgraph *>(depsgraph);
  deg_graph->debug.flags = flags;
}

int DEG_debug_flags_get(const Depsgraph *depsgraph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(depsgraph);
  return deg_graph->debug.flags;
}

void DEG_debug_name_set(Depsgraph *depsgraph, const char *name)
{
  deg::Depsgraph *deg_graph = reinterpret_cast<deg::Depsgraph *>(depsgraph);
  deg_graph->debug.name = name;
}

const char *DEG_debug_name_get(Depsgraph *depsgraph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(depsgraph);
  return deg_graph->debug.name.c_str();
}

bool DEG_debug_compare(const Depsgraph *graph1, const Depsgraph *graph2)
{
  BLI_assert(graph1 != nullptr);
  BLI_assert(graph2 != nullptr);
  const deg::Depsgraph *deg_graph1 = reinterpret_cast<const deg::Depsgraph *>(graph1);
  const deg::Depsgraph *deg_graph2 = reinterpret_cast<const deg::Depsgraph *>(graph2);
  if (deg_graph1->operations.size() != deg_graph2->operations.size()) {
    return false;
  }
  /* TODO(sergey): Currently we only do real stupid check,
   * which is fast but which isn't 100% reliable.
   *
   * Would be cool to make it more robust, but it's good enough
   * for now. Also, proper graph check is actually NP-complex
   * problem. */
  return true;
}

bool DEG_debug_graph_relations_validate(Depsgraph *graph,
                                        Main *bmain,
                                        Scene *scene,
                                        ViewLayer *view_layer)
{
  Depsgraph *temp_depsgraph = DEG_graph_new(bmain, scene, view_layer, DEG_get_mode(graph));
  bool valid = true;
  DEG_graph_build_from_view_layer(temp_depsgraph);
  if (!DEG_debug_compare(temp_depsgraph, graph)) {
    fprintf(stderr, "ERROR! Depsgraph wasn't tagged for update when it should have!\n");
    BLI_assert_msg(0, "This should not happen!");
    valid = false;
  }
  DEG_graph_free(temp_depsgraph);
  return valid;
}

bool DEG_debug_consistency_check(Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  /* Validate links exists in both directions. */
  for (deg::OperationNode *node : deg_graph->operations) {
    for (deg::Relation *rel : node->outlinks) {
      int counter1 = 0;
      for (deg::Relation *tmp_rel : node->outlinks) {
        if (tmp_rel == rel) {
          counter1++;
        }
      }
      int counter2 = 0;
      for (deg::Relation *tmp_rel : rel->to->inlinks) {
        if (tmp_rel == rel) {
          counter2++;
        }
      }
      if (counter1 != counter2) {
        printf(
            "Relation exists in outgoing direction but not in "
            "incoming (%d vs. %d).\n",
            counter1,
            counter2);
        return false;
      }
    }
  }

  for (deg::OperationNode *node : deg_graph->operations) {
    for (deg::Relation *rel : node->inlinks) {
      int counter1 = 0;
      for (deg::Relation *tmp_rel : node->inlinks) {
        if (tmp_rel == rel) {
          counter1++;
        }
      }
      int counter2 = 0;
      for (deg::Relation *tmp_rel : rel->from->outlinks) {
        if (tmp_rel == rel) {
          counter2++;
        }
      }
      if (counter1 != counter2) {
        printf("Relation exists in incoming direction but not in outcoming (%d vs. %d).\n",
               counter1,
               counter2);
      }
    }
  }

  /* Validate node valency calculated in both directions. */
  for (deg::OperationNode *node : deg_graph->operations) {
    node->num_links_pending = 0;
    node->custom_flags = 0;
  }

  for (deg::OperationNode *node : deg_graph->operations) {
    if (node->custom_flags) {
      printf("Node %s is twice in the operations!\n", node->identifier().c_str());
      return false;
    }
    for (deg::Relation *rel : node->outlinks) {
      if (rel->to->type == deg::NodeType::OPERATION) {
        deg::OperationNode *to = (deg::OperationNode *)rel->to;
        BLI_assert(to->num_links_pending < to->inlinks.size());
        ++to->num_links_pending;
      }
    }
    node->custom_flags = 1;
  }

  for (deg::OperationNode *node : deg_graph->operations) {
    int num_links_pending = 0;
    for (deg::Relation *rel : node->inlinks) {
      if (rel->from->type == deg::NodeType::OPERATION) {
        num_links_pending++;
      }
    }
    if (node->num_links_pending != num_links_pending) {
      printf("Valency mismatch: %s, %u != %d\n",
             node->identifier().c_str(),
             node->num_links_pending,
             num_links_pending);
      printf("Number of inlinks: %d\n", int(node->inlinks.size()));
      return false;
    }
  }
  return true;
}

/* ------------------------------------------------ */

void DEG_stats_simple(const Depsgraph *graph,
                      size_t *r_outer,
                      size_t *r_operations,
                      size_t *r_relations)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);

  /* number of operations */
  if (r_operations) {
    /* All operations should be in this list, allowing us to count the total
     * number of nodes. */
    *r_operations = deg_graph->operations.size();
  }

  /* Count number of outer nodes and/or relations between these. */
  if (r_outer || r_relations) {
    size_t tot_outer = 0;
    size_t tot_rels = 0;

    for (deg::IDNode *id_node : deg_graph->id_nodes) {
      tot_outer++;
      for (deg::ComponentNode *comp_node : id_node->components.values()) {
        tot_outer++;
        for (deg::OperationNode *op_node : comp_node->operations) {
          tot_rels += op_node->inlinks.size();
        }
      }
    }

    deg::TimeSourceNode *time_source = deg_graph->find_time_source();
    if (time_source != nullptr) {
      tot_rels += time_source->inlinks.size();
    }

    if (r_relations) {
      *r_relations = tot_rels;
    }
    if (r_outer) {
      *r_outer = tot_outer;
    }
  }
}

static std::string depsgraph_name_for_logging(Depsgraph *depsgraph)
{
  const char *name = DEG_debug_name_get(depsgraph);
  if (name[0] == '\0') {
    return "";
  }
  return "[" + std::string(name) + "]: ";
}

void DEG_debug_print_begin(Depsgraph *depsgraph)
{
  fprintf(stdout, "%s", depsgraph_name_for_logging(depsgraph).c_str());
}

void DEG_debug_print_eval(Depsgraph *depsgraph,
                          const char *function_name,
                          const char *object_name,
                          const void *object_address)
{
  if ((DEG_debug_flags_get(depsgraph) & G_DEBUG_DEPSGRAPH_EVAL) == 0) {
    return;
  }
  fprintf(stdout,
          "%s%s on %s %s(%p)%s\n",
          depsgraph_name_for_logging(depsgraph).c_str(),
          function_name,
          object_name,
          deg::color_for_pointer(object_address).c_str(),
          object_address,
          deg::color_end().c_str());
  fflush(stdout);
}

void DEG_debug_print_eval_subdata(Depsgraph *depsgraph,
                                  const char *function_name,
                                  const char *object_name,
                                  const void *object_address,
                                  const char *subdata_comment,
                                  const char *subdata_name,
                                  const void *subdata_address)
{
  if ((DEG_debug_flags_get(depsgraph) & G_DEBUG_DEPSGRAPH_EVAL) == 0) {
    return;
  }
  fprintf(stdout,
          "%s%s on %s %s(%p)%s %s %s %s(%p)%s\n",
          depsgraph_name_for_logging(depsgraph).c_str(),
          function_name,
          object_name,
          deg::color_for_pointer(object_address).c_str(),
          object_address,
          deg::color_end().c_str(),
          subdata_comment,
          subdata_name,
          deg::color_for_pointer(subdata_address).c_str(),
          subdata_address,
          deg::color_end().c_str());
  fflush(stdout);
}

void DEG_debug_print_eval_subdata_index(Depsgraph *depsgraph,
                                        const char *function_name,
                                        const char *object_name,
                                        const void *object_address,
                                        const char *subdata_comment,
                                        const char *subdata_name,
                                        const void *subdata_address,
                                        const int subdata_index)
{
  if ((DEG_debug_flags_get(depsgraph) & G_DEBUG_DEPSGRAPH_EVAL) == 0) {
    return;
  }
  fprintf(stdout,
          "%s%s on %s %s(%p)%s %s %s[%d] %s(%p)%s\n",
          depsgraph_name_for_logging(depsgraph).c_str(),
          function_name,
          object_name,
          deg::color_for_pointer(object_address).c_str(),
          object_address,
          deg::color_end().c_str(),
          subdata_comment,
          subdata_name,
          subdata_index,
          deg::color_for_pointer(subdata_address).c_str(),
          subdata_address,
          deg::color_end().c_str());
  fflush(stdout);
}

void DEG_debug_print_eval_parent_typed(Depsgraph *depsgraph,
                                       const char *function_name,
                                       const char *object_name,
                                       const void *object_address,
                                       const char *parent_comment,
                                       const char *parent_name,
                                       const void *parent_address)
{
  if ((DEG_debug_flags_get(depsgraph) & G_DEBUG_DEPSGRAPH_EVAL) == 0) {
    return;
  }
  fprintf(stdout,
          "%s%s on %s %s(%p) [%s] %s %s %s(%p)%s\n",
          depsgraph_name_for_logging(depsgraph).c_str(),
          function_name,
          object_name,
          deg::color_for_pointer(object_address).c_str(),
          object_address,
          deg::color_end().c_str(),
          parent_comment,
          parent_name,
          deg::color_for_pointer(parent_address).c_str(),
          parent_address,
          deg::color_end().c_str());
  fflush(stdout);
}

void DEG_debug_print_eval_time(Depsgraph *depsgraph,
                               const char *function_name,
                               const char *object_name,
                               const void *object_address,
                               float time)
{
  if ((DEG_debug_flags_get(depsgraph) & G_DEBUG_DEPSGRAPH_EVAL) == 0) {
    return;
  }
  fprintf(stdout,
          "%s%s on %s %s(%p)%s at time %f\n",
          depsgraph_name_for_logging(depsgraph).c_str(),
          function_name,
          object_name,
          deg::color_for_pointer(object_address).c_str(),
          object_address,
          deg::color_end().c_str(),
          time);
  fflush(stdout);
}

static std::string stringify_append_bit(const std::string &str, IDRecalcFlag tag)
{
  const char *tag_name = DEG_update_tag_as_string(tag);
  if (tag_name == nullptr) {
    return str;
  }
  std::string result = str;
  if (!result.empty()) {
    result += ", ";
  }
  result += tag_name;
  return result;
}

std::string DEG_stringify_recalc_flags(uint flags)
{
  if (flags == 0) {
    return "NONE";
  }
  std::string result;
  uint current_flag = flags;
  /* Special cases to avoid ALL flags from being split into individual bits. */
  if ((current_flag & ID_RECALC_PSYS_ALL) == ID_RECALC_PSYS_ALL) {
    result = stringify_append_bit(result, ID_RECALC_PSYS_ALL);
  }
  /* Handle all the rest of the flags. */
  while (current_flag != 0) {
    IDRecalcFlag tag = (IDRecalcFlag)(1 << bitscan_forward_clear_uint(&current_flag));
    result = stringify_append_bit(result, tag);
  }
  return result;
}
