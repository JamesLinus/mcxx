/*--------------------------------------------------------------------
  (C) Copyright 2006-2012 Barcelona Supercomputing Center
                          Centro Nacional de Supercomputacion

  This file is part of Mercurium C/C++ source-to-source compiler.

  See AUTHORS file in the top level directory for information
  regarding developers and contributors.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.

  Mercurium C/C++ source-to-source compiler is distributed in the hope
  that it will be useful, but WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the GNU Lesser General Public License for more
  details.

  You should have received a copy of the GNU Lesser General Public
  License along with Mercurium C/C++ source-to-source compiler; if
  not, write to the Free Software Foundation, Inc., 675 Mass Ave,
  Cambridge, MA 02139, USA.
--------------------------------------------------------------------*/

#include "cxx-cexpr.h"
#include "cxx-codegen.h"
#include "cxx-process.h"
#include "cxx-utils.h"

#include "tl-renaming-visitor.hpp"
#include "tl-loop-analysis.hpp"

namespace TL {
namespace Analysis {

    LoopAnalysis::LoopAnalysis( ExtensibleGraph* graph, Utils::InductionVarsPerNode ivs )
            : _graph( graph ), _induction_vars( ivs )
    {}

    void LoopAnalysis::compute_loop_ranges( )
    {
        Node* graph = _graph->get_graph( );
        compute_loop_ranges_rec( graph );
        ExtensibleGraph::clear_visits( graph );
    }

    void LoopAnalysis::compute_loop_ranges_rec( Node* current )
    {
        if( !current->is_visited( ) )
        {
            current->set_visited( true );

            if( current->is_graph_node( ) )
            {
                // First compute recursively the inner nodes
                compute_loop_ranges_rec( current->get_graph_entry_node( ) );

                // If the graph is a loop, compute the current ranges
                if( current->is_loop_node( ) )
                {
                    ObjectList<Utils::InductionVariableData*> ivs = current->get_induction_variables( );

                    for( ObjectList<Utils::InductionVariableData*>::iterator it = ivs.begin( ); it != ivs.end( ); ++it )
                    {
                        // The lower bound must be in the Reaching Definitions In set
                        Utils::ext_sym_map rdi = current->get_reaching_definitions_in( );
                        if( rdi.find( ( *it )->get_variable( ) ) != rdi.end( ) )
                        {
                            ( *it )->set_lb( rdi.find( ( *it )->get_variable( ) )->second );
                        }
                        else
                        {
                            nodecl_t iv = ( *it )->get_variable( ).get_nodecl( ).get_internal_nodecl( );
//                             for( Utils::ext_sym_map::iiterator itrd = rdi.begin( ); itrd != rdi.end( ); ++itrd )
//                             {
//
//                             }
                            WARNING_MESSAGE( "Induction Variable '%s' not found in the RD_IN set of loop '%d'",
                                             codegen_to_str( iv, nodecl_retrieve_context( iv ) ), current->get_id( ) );
                        }

                        // The upper bound


                        // The stride

                    }

                }
            }

            // Compute ranges for the following loops
            ObjectList<Node*> children = current->get_children( );
            for( ObjectList<Node*>::iterator it = children.begin( ); it != children.end( ); ++it )
            {
                compute_loop_ranges_rec( *it );
            }
        }
    }









//     void LoopAnalysis::traverse_loop_init(Node* loop_node, Nodecl::NodeclBase init)
//     {
//         if (init.is<Nodecl::Comma>())
//         {
//             Nodecl::Comma init_ = init.as<Nodecl::Comma>();
//             traverse_loop_init(loop_node, init_.get_rhs());
//             traverse_loop_init(loop_node, init_.get_lhs());
//         }
//         else if (init.is<Nodecl::ObjectInit>())
//         {
//             Nodecl::ObjectInit init_ = init.as<Nodecl::ObjectInit>();
//             Symbol def_var = init_.get_symbol();
//             Nodecl::NodeclBase def_expr = def_var.get_value();
//
//             InductionVarInfo* ind = new InductionVarInfo(def_var, def_expr);
//             _induction_vars.insert(induc_vars_map::value_type(loop_node->get_id(), ind));
//         }
//         else if (init.is<Nodecl::Assignment>())
//         {
//             Nodecl::Assignment init_ = init.as<Nodecl::Assignment>();
//             Symbol def_var = init_.get_lhs().get_symbol();
//             Nodecl::NodeclBase def_expr = init_.get_rhs();
//
//             InductionVarInfo* ind = new InductionVarInfo(def_var, def_expr);
//             _induction_vars.insert(induc_vars_map::value_type(loop_node->get_id(), ind));
//         }
//         else if (init.is_null())
//         {}  // Nothing to do, no init expression
//         else
//         {
//             internal_error("Node kind '%s' while analysing the induction variables in loop init expression not yet implemented",
//                 ast_print_node_type(init.get_kind()));
//         }
//     }
//
//     InductionVarInfo* LoopAnalysis::induction_vars_l_contains_symbol(Node* node, Symbol s) const
//     {
//         std::pair<induc_vars_map::const_iterator, induc_vars_map::const_iterator> actual_ind_vars = _induction_vars.equal_range(node->get_id());
//         for (induc_vars_map::const_iterator it = actual_ind_vars.first; it != actual_ind_vars.second; ++it)
//         {
//             if (it->second->get_symbol() == s)
//             {
//                 return it->second;
//             }
//         }
//         return NULL;
//     }
//
//     // FIXME LHS of comparisons can be not symbols, e.g.: (b = p[0]) > a
//     void LoopAnalysis::traverse_loop_cond(Node* loop_node, Nodecl::NodeclBase cond)
//     {
//         Nodecl::LoopControl loop_control = loop_node->get_graph_label().as<Nodecl::LoopControl>();
//
//         // Logical Operators
//         if (cond.is<Nodecl::Symbol>())
//         {   // No limits to be computed
//         }
//         else if (cond.is<Nodecl::LogicalAnd>())
//         {
//             // Traverse left and right parts
//             Nodecl::LogicalAnd cond_ = cond.as<Nodecl::LogicalAnd>();
//             traverse_loop_cond(loop_node, cond_.get_lhs());
//             traverse_loop_cond(loop_node, cond_.get_rhs());
//
//             std::cerr << "warning: Combined && expressions as loop condition not properly implemented" << std::endl;
//         }
//         else if (cond.is<Nodecl::LogicalOr>())
//         {
//             internal_error("Combined || expressions as loop condition not yet implemented", 0);
//         }
//         else if (cond.is<Nodecl::LogicalNot>())
//         {
//             internal_error("Combined ! expressions as loop condition not yet implemented", 0);
//         }
//         // Relational Operators
//         else if (cond.is<Nodecl::LowerThan>())
//         {
//             Nodecl::LowerThan cond_ = cond.as<Nodecl::LowerThan>();
//             Symbol def_var = cond_.get_lhs().get_symbol();
//             Nodecl::NodeclBase def_expr = cond_.get_rhs();
//
//             // The upper bound will be the rhs minus 1
//             nodecl_t one = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed*/ 1));
//             Nodecl::NodeclBase ub = Nodecl::Minus::make(def_expr, Nodecl::IntegerLiteral(one), def_var.get_type(),
//                                                         cond.get_filename(), cond.get_line());
//             ub = Nodecl::Utils::reduce_expression(ub);
//
//             InductionVarInfo* loop_info_var;
//             if ( (loop_info_var = induction_vars_l_contains_symbol(loop_node, def_var)) != NULL )
//             {
//                 loop_info_var->set_ub(ub);
//             }
//             else
//             {
//                 loop_info_var = new InductionVarInfo(def_var, def_expr);
//                 loop_info_var->set_ub(ub);
//                 _induction_vars.insert(induc_vars_map::value_type(loop_node->get_id(), loop_info_var));
//             }
//         }
//         else if (cond.is<Nodecl::LowerOrEqualThan>())
//         {
//             Nodecl::LowerThan cond_ = cond.as<Nodecl::LowerThan>();
//             Symbol def_var = cond_.get_lhs().get_symbol();
//             Nodecl::NodeclBase def_expr = cond_.get_rhs();
//
//             InductionVarInfo* loop_info_var;
//             if ( (loop_info_var = induction_vars_l_contains_symbol(loop_node, def_var)) != NULL )
//             {
//                 loop_info_var->set_ub(def_expr);
//             }
//             else
//             {
//                 loop_info_var = new InductionVarInfo(def_var, def_expr);
//                 loop_info_var->set_ub(def_expr);
//                 _induction_vars.insert(induc_vars_map::value_type(loop_node->get_id(), loop_info_var));
//             }
//
//         }
//         else if (cond.is<Nodecl::GreaterThan>())
//         {
//             Nodecl::GreaterThan cond_ = cond.as<Nodecl::GreaterThan>();
//             Symbol def_var = cond_.get_lhs().get_symbol();
//             Nodecl::NodeclBase def_expr = cond_.get_rhs();
//
//             // This is not the UB, is the LB: the lower bound will be the rhs plus 1
//             nodecl_t one = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed*/ 1));
//             Nodecl::NodeclBase lb = Nodecl::Add::make(def_expr, Nodecl::IntegerLiteral(one), def_var.get_type(),
//                                                     cond.get_filename(), cond.get_line());
//             lb = Nodecl::Utils::reduce_expression(lb);
//
//             InductionVarInfo* loop_info_var;
//             if ( (loop_info_var = induction_vars_l_contains_symbol(loop_node, def_var)) != NULL )
//             {
//                 loop_info_var->set_ub(loop_info_var->get_lb());
//                 loop_info_var->set_lb(lb);
//             }
//             else
//             {
//                 loop_info_var = new InductionVarInfo(def_var, def_expr);
//                 loop_info_var->set_lb(lb);
//                 _induction_vars.insert(induc_vars_map::value_type(loop_node->get_id(), loop_info_var));
//             }
//         }
//         else if (cond.is<Nodecl::GreaterOrEqualThan>())
//         {
//             Nodecl::GreaterThan cond_ = cond.as<Nodecl::GreaterThan>();
//             Symbol def_var = cond_.get_lhs().get_symbol();
//             Nodecl::NodeclBase def_expr = cond_.get_rhs();
//
//             InductionVarInfo* loop_info_var;
//             if ( (loop_info_var = induction_vars_l_contains_symbol(loop_node, def_var)) != NULL )
//             {
//                 loop_info_var->set_ub(loop_info_var->get_lb());
//                 loop_info_var->set_lb(def_expr);
//             }
//             else
//             {
//                 loop_info_var = new InductionVarInfo(def_var, def_expr);
//                 loop_info_var->set_lb(def_expr);
//                 _induction_vars.insert(induc_vars_map::value_type(loop_node->get_id(), loop_info_var));
//             }
//         }
//         else if (cond.is<Nodecl::Different>())
//         {
//             internal_error("Analysis of loops with DIFFERENT condition expression not yet implemented", 0);
//         }
//         else if (cond.is<Nodecl::Equal>())
//         {
//             internal_error("Analysis of loops with EQUAL condition expression not yet implemented", 0);
//         }
//         else if (cond.is_null())
//         {}  // Nothing to do, no init expression
//         else
//         {   // TODO Complex expression in the condition node may contain an UB or LB of the induction variable
//         }
//     }
//
//     void LoopAnalysis::traverse_loop_stride(Node* loop_node, Nodecl::NodeclBase stride)
//     {
//         if (stride.is<Nodecl::Preincrement>() || stride.is<Nodecl::Postincrement>())
//         {
//             Nodecl::NodeclBase rhs;
//             if (stride.is<Nodecl::Preincrement>())
//             {
//                 Nodecl::Preincrement stride_ = stride.as<Nodecl::Preincrement>();
//                 rhs = stride_.get_rhs();
//             }
//             else
//             {   // Post-increment
//                 Nodecl::Postincrement stride_ = stride.as<Nodecl::Postincrement>();
//                 rhs = stride_.get_rhs();
//             }
//
//             InductionVarInfo* loop_info_var;
//             if ( (loop_info_var = induction_vars_l_contains_symbol(loop_node, rhs.get_symbol())) != NULL )
//             {
//                 nodecl_t one = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed*/ 1));
//                 loop_info_var->set_stride(Nodecl::NodeclBase(one));
//                 loop_info_var->set_stride_is_positive(1);
//             }
//             else
//             {
//                 internal_error("The stride of loop '%s' do not correspond to the variable used in the initial and codition expressions. This is not yet supported", loop_node->get_graph_label().prettyprint().c_str());
//             }
//         }
//         else if (stride.is<Nodecl::Predecrement>() || stride.is<Nodecl::Postdecrement>())
//         {
//             Nodecl::NodeclBase rhs;
//             if (stride.is<Nodecl::Predecrement>())
//             {
//                 Nodecl::Predecrement stride_ = stride.as<Nodecl::Predecrement>();
//                 rhs = stride_.get_rhs();
//             }
//             else
//             {   // Post-decrement
//                 Nodecl::Postdecrement stride_ = stride.as<Nodecl::Postdecrement>();
//                 rhs = stride_.get_rhs();
//             }
//
//             InductionVarInfo* loop_info_var;
//             if ( (loop_info_var = induction_vars_l_contains_symbol(loop_node, rhs.get_symbol())) != NULL )
//             {
//                 nodecl_t minus_one = const_value_to_nodecl(const_value_get_minus_one(/* bytes */ 4, /* signed*/ 1));
//                 loop_info_var->set_stride(Nodecl::NodeclBase(minus_one));
//                 loop_info_var->set_stride_is_positive(0);
//             }
//             else
//             {
//                 internal_error("The stride of loop '%s' do not correspond to the variable used in the initial and codition expressions. This is not yet supported", loop_node->get_graph_label().prettyprint().c_str());
//             }
//         }
//         else if (stride.is<Nodecl::AddAssignment>())
//         {
//             Nodecl::AddAssignment stride_ = stride.as<Nodecl::AddAssignment>();
//
//             InductionVarInfo* loop_info_var;
//             if ( (loop_info_var = induction_vars_l_contains_symbol(loop_node, stride_.get_lhs().get_symbol())) != NULL )
//             {
//                 loop_info_var->set_stride(stride_.get_rhs());
//                 // FIXME May be we can figure out the sign of the stride
//                 loop_info_var->set_stride_is_positive(2);
//             }
//             else
//             {
//                 internal_error("The stride of loop '%s' do not correspond to the variable used in the initial and codition expressions. This is not yet supported", loop_node->get_graph_label().prettyprint().c_str());
//             }
//         }
//         else if (stride.is_null())
//         {}  // Nothing to do, no init expression
//         else
//         {
//             internal_error("Node kind '%s' while analysing the induction variables in loop stride expression not yet implemented",
//                 ast_print_node_type(stride.get_kind()));
//         }
//     }
//
//     void LoopAnalysis::compute_loop_induction_vars(Node* loop_node)
//     {
//         // Propagate induction variables of the outer loop to the actual loop
//         Node* outer_node = loop_node->get_outer_node();
//         induc_vars_map new_induction_vars;
//         while (outer_node != NULL)
//         {
//             if (outer_node->is_loop_node())
//             {
//                 std::pair<induc_vars_map::const_iterator, induc_vars_map::const_iterator> outer_ind_vars =
//                         _induction_vars.equal_range(outer_node->get_id());
//
//                 for (induc_vars_map::const_iterator it = outer_ind_vars.first; it != outer_ind_vars.second; ++it)
//                 {
//                     new_induction_vars.insert(induc_vars_map::value_type(loop_node->get_id(), it->second));
//                 }
//                 break;  // If there are more outer loops analysed, their info has been already propagated to the nearest outer node
//             }
//             outer_node = outer_node->get_outer_node();
//         }
//         for (induc_vars_map::const_iterator it = new_induction_vars.begin(); it != new_induction_vars.end(); ++it)
//         {
//             _induction_vars.insert(induc_vars_map::value_type(it->first, it->second));
//         }
//
//         // Compute actual loop control info
//         Nodecl::LoopControl loop_control = loop_node->get_graph_label().as<Nodecl::LoopControl>();
//         traverse_loop_init(loop_node, loop_control.get_init());
//         traverse_loop_cond(loop_node, loop_control.get_cond());
//         traverse_loop_stride(loop_node, loop_control.get_next());
//
//         // Check whether the statements within the loop modify the induction variables founded in the loop control
//         Node* entry = loop_node->get_graph_entry_node();
//         delete_false_induction_vars(entry, loop_node);
//
//         ExtensibleGraph::clear_visits(entry);
//     }
//
//     void LoopAnalysis::compute_induction_variables_info(Node* node)
//     {
//         if ( !node->is_visited( ) )
//         {
//             node->set_visited( true );
//
//             if( !node->is_graph_exit_node( ) )
//             {
//                 if( node->is_loop_node( ) )
//                 {
//                     if( node->get_data<Graph_type>( _GRAPH_TYPE ) == LOOP_FOR )
//                     {
//                         // Get the info about induction variables in the loop control
//                         compute_loop_induction_vars(node);
//                     }
//                     compute_induction_variables_info( node->get_graph_entry_node( ) );
//                 }
//
//                 ObjectList<Node*> children = node->get_children( );
//                 for( ObjectList<Node*>::iterator it = children.begin( ); it != children.end( ); ++it )
//                 {
//                     compute_induction_variables_info( *it );
//                 }
//             }
//         }
//     }
//
//     Nodecl::NodeclBase LoopAnalysis::set_access_range(Node* node, Node* loop_node,
//                                                     const char use_type, Nodecl::NodeclBase nodecl,
//                                                     std::map<Symbol, Nodecl::NodeclBase> ind_var_map,
//                                                     Nodecl::NodeclBase reach_def_var)
//     {
//         Nodecl::NodeclBase renamed_nodecl;
//         RenamingVisitor renaming_v(ind_var_map, nodecl.get_filename().c_str(), nodecl.get_line());
//         ObjectList<Nodecl::NodeclBase> renamed = renaming_v.walk(nodecl);
//
//         if (!renamed.empty())
//         {
//             if (renamed.size() == 1)
//             {
// //                     std::cerr << "Renaming performed: " << nodecl.prettyprint() << " --> " << renamed[0].prettyprint() << std::endl;
//                 if (use_type == '0')
//                 {
//                     node->unset_ue_var(ExtendedSymbol(nodecl));
//                     node->set_ue_var(ExtendedSymbol(renamed[0]));
//                     renamed_nodecl = renamed[0];
//                 }
//                 else if (use_type == '1')
//                 {
//                     node->unset_killed_var(ExtendedSymbol(nodecl));
//                     node->set_killed_var(ExtendedSymbol(renamed[0]));
//                     renamed_nodecl = renamed[0];
//                 }
//                 else if (use_type == '2' || use_type == '3')
//                 {
//                     InductionVarInfo* ind_var = induction_vars_l_contains_symbol(loop_node, renaming_v.get_matching_symbol());
//                     int is_positive = ind_var->stride_is_positive();
//
//                     if (nodecl.is<Nodecl::ArraySubscript>())
//                     {
//                         if (use_type == '2')
//                         {   // We are renaming the key, this is the defined variable
//                             node->rename_reaching_defintion_var(nodecl, renamed[0]);
//                             renamed_nodecl = renamed[0];
//                         }
//                         else
//                         {   //  We are renaming the init expression of a reaching definition
//                             node->set_reaching_definition(reach_def_var, renamed[0]);
//                             renamed_nodecl = reach_def_var;
//                         }
//                     }
//                     else
//                     {
//                         if (use_type == '3')
//                         {   // In a reaching definition, we cannot modify the defined variable, just the init expression
//                             Nodecl::NodeclBase range = renamed[0];
//                             node->set_reaching_definition(reach_def_var, range);
//                             renamed_nodecl = reach_def_var;
//                         }
//                         else
//                         {
//                             renamed_nodecl = nodecl;
//                         }
//                     }
//                 }
//                 else if (use_type == '4')
//                 {
//                     node->unset_undefined_behaviour_var(ExtendedSymbol(nodecl));
//                     node->set_undefined_behaviour_var(ExtendedSymbol(renamed[0]));
//                     renamed_nodecl = renamed[0];
//                 }
//                 else
//                 {
//                     internal_error("Unexpected type of variable use '%s' in node '%d'", use_type, node->get_id());
//                 }
//             }
//             else
//             {
//                 internal_error("More than one nodecl returned while renaming variables to ranges within a loop [%d] '%s'",
//                             loop_node->get_id(), loop_node->get_graph_label().prettyprint().c_str());
//             }
//         }
//         else
//         {
//             renamed_nodecl = nodecl;
//         }
//
//         return renamed_nodecl;
//     }
//
//     void LoopAnalysis::set_access_range_in_ext_sym_set(Node* node, Node* loop_node, Utils::ext_sym_set nodecl_l, const char use_type)
//     {
//         std::map<Symbol, Nodecl::NodeclBase> ind_var_map = get_induction_vars_mapping(loop_node);
//         for(Utils::ext_sym_set::iterator it = nodecl_l.begin(); it != nodecl_l.end(); ++it)
//         {
//             if (it->is_array())
//             {
//                 set_access_range(node, loop_node, use_type, it->get_nodecl(), ind_var_map);
//             }
//         }
//     }
//
//     void LoopAnalysis::set_access_range_in_nodecl_map(Node* node, Node* loop_node, nodecl_map nodecl_m)
//     {
//         std::map<Symbol, Nodecl::NodeclBase> ind_var_map = get_induction_vars_mapping(loop_node);
//         for(nodecl_map::iterator it = nodecl_m.begin(); it != nodecl_m.end(); ++it)
//         {
//             Nodecl::NodeclBase first = it->first, second = it->second;
//             Nodecl::NodeclBase renamed_reach_def = set_access_range(node, loop_node, '2', it->first, ind_var_map);
//             set_access_range(node, loop_node, '3', it->second, ind_var_map, renamed_reach_def);
//         }
//     }
//
//     void LoopAnalysis::compute_ranges_for_variables_in_loop(Node* node, Node* loop_node)
//     {
//         if (!node->is_visited())
//         {
//             node->set_visited(true);
//
//             Node_type ntype = node->get_type();
//             if (ntype != EXIT)
//             {
//                 if (ntype == GRAPH)
//                 {
//                     Node* next_loop = loop_node;
//                     if (node->get_graph_type() == LOOP)
//                         next_loop = node;
//                     compute_ranges_for_variables_in_loop(node->get_data<Node*>(_ENTRY_NODE), next_loop);
//                     ExtensibleGraph::clear_visits_in_level(node->get_data<Node*>(_ENTRY_NODE), node);
//                     node->set_visited(false);
//                     node->set_graph_node_use_def();
//                 }
//                 else if (ntype == NORMAL || ntype == LABELED || ntype == FUNCTION_CALL)
//                 {   // Check for arrays in that are used in some way within the BB statements
//                     set_access_range_in_ext_sym_set(node, loop_node, node->get_ue_vars(), /* use type */ '0');
//                     set_access_range_in_ext_sym_set(node, loop_node, node->get_killed_vars(), /* use type */ '1');
//                     set_access_range_in_ext_sym_set(node, loop_node, node->get_undefined_behaviour_vars(), /* use type */ '4');
//                     set_access_range_in_nodecl_map(node, loop_node, node->get_reaching_definitions());
//                 }
//
//                 ObjectList<Node*> children = node->get_children();
//                 for (ObjectList<Node*>::iterator it = children.begin(); it != children.end(); ++it)
//                 {
//                     compute_ranges_for_variables_in_loop(*it, loop_node);
//                 }
//             }
//         }
//     }
//
//     void LoopAnalysis::compute_ranges_for_variables( Node* node )
//     {
//         if ( !node->is_visited( ) )
//         {
//             node->set_visited( true );
//
//             if ( !node->is_graph_exit_node( ) )
//             {
//                 if ( node->is_graph_node( ) )
//                 {
//                     Node* entry = node->get_graph_entry_node( );
//                     if ( node->is_loop_node( ) )
//                     {
//                         compute_ranges_for_variables_in_loop( entry, node );
//                     }
//                     else
//                     {
//                         compute_ranges_for_variables( entry );
//                     }
//                     ExtensibleGraph::clear_visits( node );
//                     node->set_graph_node_use_def( );
//                 }
//
//                 ObjectList<Node*> children = node->get_children( );
//                 for ( ObjectList<Node*>::iterator it = children.begin( ); it != children.end( ); ++it )
//                 {
//                     compute_ranges_for_variables( *it );
//                 }
//             }
//         }
//     }

}
}