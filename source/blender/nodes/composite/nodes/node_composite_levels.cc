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
 * The Original Code is Copyright (C) 2006 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup cmpnodes
 */

#include "node_composite_util.hh"

/* **************** LEVELS ******************** */

namespace blender::nodes {

static void cmp_node_levels_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Image").default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_output<decl::Float>("Mean");
  b.add_output<decl::Float>("Std Dev");
}

}  // namespace blender::nodes

static void node_composit_init_view_levels(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 = 1; /* All channels. */
}

void register_node_type_cmp_view_levels(void)
{
  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_VIEW_LEVELS, "Levels", NODE_CLASS_OUTPUT, NODE_PREVIEW);
  ntype.declare = blender::nodes::cmp_node_levels_declare;
  node_type_init(&ntype, node_composit_init_view_levels);

  nodeRegisterType(&ntype);
}
