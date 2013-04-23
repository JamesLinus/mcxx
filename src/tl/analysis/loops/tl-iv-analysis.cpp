/*--------------------------------------------------------------------
  (C) Copyright 2006-2013 Barcelona Supercomputing Center
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

#include <utility>

#include "cxx-cexpr.h"
#include "codegen-common.hpp"
#include "tl-iv-analysis.hpp"
#include "tl-node.hpp"

namespace TL {
namespace Analysis {


    bool is_accepted_induction_variable_syntax( Nodecl::NodeclBase stmt,
                                                Nodecl::NodeclBase& iv, Nodecl::NodeclBase& incr )
    {
        bool is_iv = false;

        Nodecl::Utils::ReduceExpressionVisitor v;
        Nodecl::NodeclBase st = stmt.shallow_copy( );
        v.walk( st );

        if( st.is<Nodecl::Assignment>( ) )
        {
            Nodecl::Assignment st_ = st.as<Nodecl::Assignment>( );
            Nodecl::NodeclBase lhs = st_.get_lhs( );
            Nodecl::NodeclBase rhs = st_.get_rhs( );

            Nodecl::NodeclBase lhs_rhs, rhs_rhs;
            if( rhs.is<Nodecl::Add>( ) )
            {   // Expression accepted: iv = x + iv;
                Nodecl::Add _rhs = rhs.as<Nodecl::Add>( );
                lhs_rhs = _rhs.get_lhs( );
                rhs_rhs = _rhs.get_rhs( );

                if( Nodecl::Utils::equal_nodecls( lhs, rhs_rhs, /* Skip Conversion node */ true )
                    && lhs_rhs.is_constant( ) )
                {
                    iv = lhs;
                    incr = lhs_rhs;
                    is_iv = true;
                }
            }
            else if( rhs.is<Nodecl::Minus>( ) )
            {   // Expression accepted: iv = -x + iv;
                Nodecl::Minus _rhs = rhs.as<Nodecl::Minus>( );
                lhs_rhs = _rhs.get_lhs( );
                rhs_rhs = _rhs.get_rhs( );

                if( Nodecl::Utils::equal_nodecls( lhs, rhs_rhs, /* Skip Conversion node */ true )
                    && lhs_rhs.is_constant( ) )
                {
                    iv = lhs;
                    incr = lhs_rhs;
                    is_iv = true;
                }
            }
        }
        else if( st.is<Nodecl::AddAssignment>( ) )
        {   // Expression accepted: iv += x;
            Nodecl::AddAssignment st_ = st.as<Nodecl::AddAssignment>( );
            if( st_.get_rhs( ).is_constant( ) )
            {
                iv = st_.get_lhs( );
                incr = st_.get_rhs( );
                is_iv = true;
            }
        }
        else if( st.is<Nodecl::MinusAssignment>( ) )
        {   // Expression accepted: iv -= x;
            Nodecl::MinusAssignment st_ = st.as<Nodecl::MinusAssignment>( );
            Nodecl::NodeclBase rhs = st_.get_rhs( );
            if( st_.get_rhs( ).is_constant( ) )
            {
                Nodecl::NodeclBase new_rhs = Nodecl::Neg::make( rhs, rhs.get_type( ),
                                                                rhs.get_filename( ), rhs.get_line( ) );
                iv = st_.get_lhs( );
                incr = new_rhs;
                is_iv = true;
            }
        }
        else if( st.is<Nodecl::Preincrement>( ) )
        {
            Nodecl::NodeclBase rhs = st.as<Nodecl::Preincrement>( ).get_rhs( );
            iv = rhs;
            incr = Nodecl::IntegerLiteral::make( rhs.get_type( ), const_value_get_one( /* bytes */ 4, /* signed */ 1 ) );
            is_iv = true;
        }
        else if( st.is<Nodecl::Postincrement>( ) )
        {
            Nodecl::NodeclBase rhs = st.as<Nodecl::Postincrement>( ).get_rhs( );
            iv = rhs;
            incr = Nodecl::IntegerLiteral::make( rhs.get_type( ), const_value_get_one( /* bytes */ 4, /* signed */ 1 ) );
            is_iv = true;
        }
        else if( st.is<Nodecl::Predecrement>( ) )
        {
            Nodecl::NodeclBase rhs = st.as<Nodecl::Predecrement>( ).get_rhs( );
            iv = rhs;
            incr = Nodecl::IntegerLiteral::make( rhs.get_type( ), const_value_get_minus_one( /* bytes */ 4, /* signed */ 1 ) );
            is_iv = true;
        }
        else if( st.is<Nodecl::Postdecrement>( ) )
        {
            Nodecl::NodeclBase rhs = st.as<Nodecl::Postdecrement>( ).get_rhs( );
            iv = rhs;
            incr = Nodecl::IntegerLiteral::make( rhs.get_type( ), const_value_get_minus_one( /* bytes */ 4, /* signed */ 1 ) );
            is_iv = true;
        }

        return is_iv;
    }

    // ***************************************** END Utils ***************************************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // ************************** Class for induction variables analysis *************************** //

    InductionVariableAnalysis::InductionVariableAnalysis( ExtensibleGraph* graph )
            : _induction_vars( ), _graph( graph )
    {}

    void InductionVariableAnalysis::compute_induction_variables( )
    {
        Node* graph = _graph->get_graph( );
        compute_induction_variables_rec( graph );
        ExtensibleGraph::clear_visits( graph );
    }

    void InductionVariableAnalysis::compute_induction_variables_rec( Node* current )
    {
        if( !current->is_visited( ) )
        {
            current->set_visited(true);

            if( current->is_graph_node( ) )
            {
                // IV is computed from inner to outer loops
                Node* entry = current->get_graph_entry_node( );
                compute_induction_variables_rec( entry );

                if( current->is_loop_node( ) )
                {   // Treat current loop
                    ExtensibleGraph::clear_visits_in_level( entry, current );
                    detect_basic_induction_variables( entry, current );
//                     ExtensibleGraph::clear_visits_in_level( entry, current );
//                     detect_derived_induction_variables( entry, current );
                }
            }

            ObjectList<Node*> children = current->get_children( );
            for( ObjectList<Node*>::iterator it = children.begin( ); it != children.end( ); ++it )
            {
                compute_induction_variables_rec( *it );
            }
        }
    }

    void InductionVariableAnalysis::detect_basic_induction_variables( Node* current, Node* loop )
    {
        if( !current->is_visited( ) && !current->is_graph_exit_node( loop ) )
        {
            current->set_visited( true );

            // Look for IVs in the current node
            ObjectList<Nodecl::NodeclBase> stmts = current->get_statements( );
            for( ObjectList<Nodecl::NodeclBase>::iterator it = stmts.begin( ); it != stmts.end( ); ++it )
            {
                Nodecl::NodeclBase incr;
                Nodecl::NodeclBase iv = is_basic_induction_variable( *it, loop, incr );
                if( !iv.is_null( ) )
                {
                    Utils::InductionVariableData* ivd = new Utils::InductionVariableData( Utils::ExtendedSymbol( iv ),
                                                                                            Utils::BASIC_IV, iv );
                    ivd->set_increment( incr );
                    loop->set_induction_variable( ivd );
                    _induction_vars.insert( std::pair<int, Utils::InductionVariableData*>( loop->get_id( ), ivd ) );
                }
            }

            // Look for IVs in current's children
            ObjectList<Node*> children = current->get_children( );
            for( ObjectList<Node*>::iterator it = children.begin( ); it != children.end( ); ++it )
            {
                detect_basic_induction_variables( *it, loop );
            }
        }
    }

    // FIXME This method does not cover all kind induction variable.
    // F.i., 'st': iv = 1 + iv + z, where 'z' is loop invariant, will return false
    Nodecl::NodeclBase InductionVariableAnalysis::is_basic_induction_variable( Nodecl::NodeclBase st, Node* loop,
                                                                               Nodecl::NodeclBase& incr )
    {
        Nodecl::NodeclBase iv = Nodecl::NodeclBase::null( );

        Node* loop_entry = loop->get_graph_entry_node( );
        int id_end = loop->get_graph_exit_node( )->get_id( );

        if( is_accepted_induction_variable_syntax( st, iv, incr ) )
        {
            // Get a list from the IVs in the map corresponding to the current loop
            ObjectList<Utils::InductionVariableData*> ivs;
            std::pair<Utils::InductionVarsPerNode::iterator, Utils::InductionVarsPerNode::iterator> loop_ivs =
                    _induction_vars.equal_range( loop->get_id( ) );
            for( Utils::InductionVarsPerNode::iterator it = loop_ivs.first; it != loop_ivs.second; ++it )
            {
                ivs.append( it->second );
            }

            if( !Utils::induction_variable_list_contains_variable( ivs, iv ) )
            {
                if( !check_potential_induction_variable( iv, incr, st, loop_entry, id_end ) )
                {
                    iv = Nodecl::NodeclBase::null( );
                }
            }
            else
            {
                iv = Nodecl::NodeclBase::null( );
            }
        }

        return iv;
    }

    void InductionVariableAnalysis::detect_derived_induction_variables( Node* current, Node* loop )
    {
        if( !current->is_visited( ) && current->is_graph_exit_node( loop ) )
        {
            current->set_visited( true );

            // Look for IVs in the current node
            ObjectList<Nodecl::NodeclBase> stmts = current->get_statements( );
            for( ObjectList<Nodecl::NodeclBase>::iterator it = stmts.begin( ); it != stmts.end( ); ++it )
            {
                Nodecl::NodeclBase iv_family;
                Nodecl::NodeclBase n = is_derived_induction_variable( *it, current, loop, iv_family );
                if( !n.is_null( ) ){
                    Utils::InductionVariableData* iv = new Utils::InductionVariableData( Utils::ExtendedSymbol( n ),
                                                                                         Utils::DERIVED_IV, n );
                    loop->set_induction_variable( iv );
                    _induction_vars.insert( std::pair<int, Utils::InductionVariableData*>( loop->get_id( ), iv ) );
                }

            }
        }
    }

    Nodecl::NodeclBase InductionVariableAnalysis::is_derived_induction_variable( Nodecl::NodeclBase st, Node* current,
                                                                                 Node* loop, Nodecl::NodeclBase& family )
    {
        Nodecl::NodeclBase res = Nodecl::NodeclBase::null( );

//         Node* loop_entry = loop->get_graph_entry_node( );
//         int id_end = loop->get_graph_exit_node( )->get_id( );
//         if( st.is<Nodecl::Assignment>( ) )
//         {   /*! Expressions accepted
//                 * . iv_1 = iv_2 + x;
//                 * . iv_1 = x + iv_2;
//                 * . iv_1 = iv_2 * x;
//                 * . iv_1 = x * iv_2; */
//             Nodecl::Assignment _st = st.as<Nodecl::Assignment>( );
//             Nodecl::NodeclBase lhs = _st.get_lhs( );
//             Nodecl::NodeclBase rhs = _st.get_rhs( );
//
//             Nodecl::NodeclBase lhs_rhs, rhs_rhs;
//             if( rhs.is<Nodecl::Add>( ) )
//             {
//                 Nodecl::Add rhs_ = rhs.as<Nodecl::Add>( );
//                 lhs_rhs = rhs_.get_lhs( );
//                 rhs_rhs = rhs_.get_rhs( );
//             }
//             else if( rhs.is<Nodecl::Mul>( ) )
//             {
//                 Nodecl::Mul rhs_ = rhs.as<Nodecl::Mul>( );
//                 lhs_rhs = rhs_.get_lhs( );
//                 rhs_rhs = rhs_.get_rhs( );
//             }
//
//             ObjectList<Utils::InductionVariableData*> loop_ivs = loop->get_induction_variables( );
//             _constant = Nodecl::NodeclBase::null( );
//             if( Utils::induction_variable_list_contains_variable( loop_ivs, lhs_rhs ) )
//             {
//                 _constant = rhs_rhs;
//                 family = lhs_rhs;
//             }
//             else if( Utils::induction_variable_list_contains_variable( loop_ivs, rhs_rhs ) )
//             {
//                 _constant= lhs_rhs;
//                 family = rhs_rhs;
//             }
//             if( !_constant.is_null( ) )
//             {
//                 if( is_loop_invariant( loop_entry, id_end ) )
//                 {   //! expression of type: "lhs = family (+,*) _constant"
//                     if( /*!loop_ivs.at(family).is_basic( )*/ true )
//                     {
//                         // The only definition of \family that reaches \lhs is within the loop
//                         _constant = family;
//                         if(only_definition_is_in_loop(st, current, loop))
//                         // The family of \family must not be defined between the definition of \family and \lhs
//                         // TODO
//                         if( true )
//                         {
//                             res = lhs;
//                         }
//                     }
//                     else
//                     {
//                         res = lhs;
//                     }
//                 }
//             }
//         }
//
//         if( check_potential_induction_variable( res, st, loop_entry, id_end ) )
//         {
//             res = Nodecl::NodeclBase::null( );
//         }

        return res;
    }

    bool InductionVariableAnalysis::check_potential_induction_variable( Nodecl::NodeclBase iv, Nodecl::NodeclBase& incr,
                                                                        Nodecl::NodeclBase stmt, Node* node, int id_end )
    {
        bool res = check_potential_induction_variable_rec( iv, incr, stmt, node, id_end );
        ExtensibleGraph::clear_visits_aux( node );
        return res;
    }

    bool InductionVariableAnalysis::check_potential_induction_variable_rec( Nodecl::NodeclBase iv, Nodecl::NodeclBase& incr,
                                                                            Nodecl::NodeclBase stmt, Node* node, int id_end )
    {
        bool result = true;

        if( ( node->get_id( ) != id_end ) && !node->is_visited_aux( ) )
        {
            node->set_visited_aux( true );

            // Check the current node
            ObjectList<Nodecl::NodeclBase> stmts = node->get_statements( );
            for( ObjectList<Nodecl::NodeclBase>::iterator it = stmts.begin( ); it != stmts.end( ); ++it )
            {
                // Check the statement only if it is not the statement where the potential IV was found
                if( !Nodecl::Utils::equal_nodecls( stmt, *it, /* skip conversion nodes */ true ) )
                {
                    FalseInductionVariablesVisitor v( iv, &incr );
                    v.walk( *it );
                    if( !v.get_is_induction_variable( ) )
                    {
                        result = false;
                        break;
                    }
                }
            }

            // If IV still locks like an IV, check for false positives in the children nodes
            if( result )
            {
                ObjectList<Node*> children = node->get_children( );
                for( ObjectList<Node*>::iterator it = children.begin( ); it != children.end( ); ++it )
                {
                    if( !check_potential_induction_variable_rec( iv, incr, stmt, *it, id_end ) )
                    {
                        result = false;
                        break;
                    }
                }
            }
        }

        return result;
    }

    Utils::InductionVarsPerNode InductionVariableAnalysis::get_all_induction_vars( ) const
    {
        return _induction_vars;
    }

    // ************************ END class for induction variables analysis ************************* //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // ****************** Visitor that checks whether a potential IV is a real IV ****************** //

    FalseInductionVariablesVisitor::FalseInductionVariablesVisitor( Nodecl::NodeclBase iv, Nodecl::NodeclBase* incr )
            : _iv( iv ), _incr( incr ), _is_induction_var( true ), _n_nested_conditionals( 0 ), _calc( )
    {}

    bool FalseInductionVariablesVisitor::get_is_induction_variable( ) const
    {
        return _is_induction_var;
    }

    void FalseInductionVariablesVisitor::undefine_induction_variable( )
    {
        _is_induction_var = false;
        _incr = NULL;
    }

    void FalseInductionVariablesVisitor::join_list( TL::ObjectList<bool>& list )
    {   // nothing to be done
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::AddAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            if( _n_nested_conditionals > 0 )
            {
                // An induction variable may never be modified inside a conditional block
                undefine_induction_variable( );
            }
            else
            {
                Nodecl::NodeclBase new_incr;
                // Check whether the statement has the accepted syntax of an induction variable
                if( is_accepted_induction_variable_syntax( n, _iv, new_incr ) )
                {
                    // Check if the increments are linear and can be combined
                    if( const_value_is_positive( _incr->get_constant( ) ) &&
                        const_value_is_positive( new_incr.get_constant( ) ) )
                    {
                        Nodecl::NodeclBase c = Nodecl::Add::make( *_incr, new_incr, _incr->get_type( ) );
                        const_value_t* c_value = _calc.compute_const_value( c );
                        *_incr = const_value_to_nodecl( c_value );
                    }
                    else if( const_value_is_negative( _incr->get_constant( ) ) &&
                        const_value_is_negative( new_incr.get_constant( ) ) )
                    {
                        Nodecl::NodeclBase c = Nodecl::Minus::make( *_incr, new_incr, _incr->get_type( ) );
                        const_value_t* c_value = _calc.compute_const_value( c );
                        *_incr = const_value_to_nodecl( c_value );
                    }
                    else
                    {
                        undefine_induction_variable( );
                    }
                }
            }
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::ArithmeticShrAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::Assignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            if( _n_nested_conditionals > 0 )
            {
                // An induction variable may never be modified inside a conditional block
                undefine_induction_variable( );
            }
            else
            {
                Nodecl::NodeclBase new_incr;
                // Check whether the statement has the accepted syntax of an induction variable
                if( is_accepted_induction_variable_syntax( n, _iv, new_incr ) )
                {
                    // Check if the increments are linear and can be combined
                    if( const_value_is_positive( _incr->get_constant( ) ) &&
                        const_value_is_positive( new_incr.get_constant( ) ) )
                    {
                        Nodecl::NodeclBase c = Nodecl::Add::make( *_incr, new_incr, _incr->get_type( ) );
                        const_value_t* c_value = _calc.compute_const_value( c );
                        *_incr = const_value_to_nodecl( c_value );

                    }
                    else if( const_value_is_negative( _incr->get_constant( ) ) &&
                        const_value_is_negative( new_incr.get_constant( ) ) )
                    {
                        Nodecl::NodeclBase c = Nodecl::Minus::make( *_incr, new_incr, _incr->get_type( ) );
                        const_value_t* c_value = _calc.compute_const_value( c );
                        *_incr = const_value_to_nodecl( c_value );
                    }
                    else
                    {
                        undefine_induction_variable( );
                    }
                }
            }
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::BitwiseAndAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::BitwiseOrAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::BitwiseShlAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::BitwiseShrAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::BitwiseXorAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::ConditionalExpression& n )
    {
        walk( n.get_condition() );
        _n_nested_conditionals++;
        walk( n.get_true() );
        walk( n.get_false() );
        _n_nested_conditionals--;
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::DivAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::IfElseStatement& n )
    {
        walk( n.get_condition() );
        _n_nested_conditionals++;
        walk( n.get_then() );
        walk( n.get_else() );
        _n_nested_conditionals--;
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::MinusAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            if( _n_nested_conditionals > 0 )
            {
                // An induction variable may never be modified inside a conditional block
                undefine_induction_variable( );
            }
            else
            {
                Nodecl::NodeclBase new_incr;
                // Check whether the statement has the accepted syntax of an induction variable
                if( is_accepted_induction_variable_syntax( n, _iv, new_incr ) )
                {
                    // Check if the increments are linear and can be combined
                    if( const_value_is_positive( _incr->get_constant( ) ) &&
                        const_value_is_positive( new_incr.get_constant( ) ) )
                    {
                        Nodecl::NodeclBase c = Nodecl::Add::make( *_incr, new_incr, _incr->get_type( ) );
                        const_value_t* c_value = _calc.compute_const_value( c );
                        *_incr = const_value_to_nodecl( c_value );
                    }
                    else if( const_value_is_negative( _incr->get_constant( ) ) &&
                        const_value_is_negative( new_incr.get_constant( ) ) )
                    {
                        Nodecl::NodeclBase c = Nodecl::Minus::make( *_incr, new_incr, _incr->get_type( ) );
                        const_value_t* c_value = _calc.compute_const_value( c );
                        *_incr = const_value_to_nodecl( c_value );
                    }
                    else
                    {
                        undefine_induction_variable( );
                    }
                }
            }
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::ModAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::MulAssignment& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_lhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_lhs( ) ) )
        {
            undefine_induction_variable( );
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::Postdecrement& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_rhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_rhs( ) ) )
        {
            if( _n_nested_conditionals > 0 )
            {
                // An induction variable may never be modified inside a conditional block
                undefine_induction_variable( );
            }
            else
            {
                // Check if the increments are linear and can be combined
                if( const_value_is_negative( _incr->get_constant( ) ) )
                {
                    Nodecl::NodeclBase new_incr = const_value_to_nodecl( const_value_get_one( /* bytes */ 4, /* signed */ 1 ) );
                    Nodecl::NodeclBase c = Nodecl::Minus::make( *_incr, new_incr, _incr->get_type( ) );
                    const_value_t* c_value = _calc.compute_const_value( c );
                    *_incr = const_value_to_nodecl( c_value );
                }
                else
                {
                    undefine_induction_variable( );
                }
            }
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::Postincrement& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_rhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_rhs( ) ) )
        {
            if( _n_nested_conditionals > 0 )
            {
                // An induction variable may never be modified inside a conditional block
                undefine_induction_variable( );
            }
            else
            {
                // Check if the increments are linear and can be combined
                if( const_value_is_positive( _incr->get_constant( ) ) )
                {
                    Nodecl::NodeclBase new_incr = const_value_to_nodecl( const_value_get_one( /* bytes */ 4, /* signed */ 1 ) );
                    Nodecl::NodeclBase c = Nodecl::Add::make( *_incr, new_incr, _incr->get_type( ) );
                    const_value_t* c_value = _calc.compute_const_value( c );
                    *_incr = const_value_to_nodecl( c_value );
                }
                else
                {
                    undefine_induction_variable( );
                }
            }
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::Predecrement& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_rhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_rhs( ) ) )
        {
            if( _n_nested_conditionals > 0 )
            {
                // An induction variable may never be modified inside a conditional block
                undefine_induction_variable( );
            }
            else
            {
                // Check if the increments are linear and can be combined
                if( const_value_is_negative( _incr->get_constant( ) ) )
                {
                    Nodecl::NodeclBase new_incr = const_value_to_nodecl( const_value_get_one( /* bytes */ 4, /* signed */ 1 ) );
                    Nodecl::NodeclBase c = Nodecl::Minus::make( *_incr, new_incr, _incr->get_type( ) );
                    const_value_t* c_value = _calc.compute_const_value( c );
                    *_incr = const_value_to_nodecl( c_value );
                }
                else
                {
                    undefine_induction_variable( );
                }
            }
        }
    }

    void FalseInductionVariablesVisitor::visit( const Nodecl::Preincrement& n )
    {
        if( Nodecl::Utils::nodecl_contains_nodecl( n.get_rhs( ), _iv ) ||
            Nodecl::Utils::nodecl_contains_nodecl( _iv, n.get_rhs( ) ) )
        {
            if( _n_nested_conditionals > 0 )
            {
                // An induction variable may never be modified inside a conditional block
                undefine_induction_variable( );
            }
            else
            {
                // Check if the increments are linear and can be combined
                if( const_value_is_positive( _incr->get_constant( ) ) )
                {
                    Nodecl::NodeclBase new_incr = const_value_to_nodecl( const_value_get_one( /* bytes */ 4, /* signed */ 1 ) );
                    Nodecl::NodeclBase c = Nodecl::Add::make( *_incr, new_incr, _incr->get_type( ) );
                    const_value_t* c_value = _calc.compute_const_value( c );
                    *_incr = const_value_to_nodecl( c_value );
                }
                else
                {
                    undefine_induction_variable( );
                }
            }
        }
    }

    // ************************ END class for induction variables analysis ************************* //
    // ********************************************************************************************* //
}
}