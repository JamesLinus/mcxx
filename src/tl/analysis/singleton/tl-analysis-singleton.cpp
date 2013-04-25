/*--------------------------------------------------------------------
 ( C) Copyright 2006-2013 Barcelona Supercomputing Center             *
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
#include "cxx-process.h"

#include "tl-analysis-singleton.hpp"
#include "tl-analysis-utils.hpp"
#include "tl-pcfg-visitor.hpp"
#include "tl-use-def.hpp"
#include "tl-liveness.hpp"
#include "tl-reaching-definitions.hpp"
#include "tl-iv-analysis.hpp"
#include "tl-loop-analysis.hpp"

namespace TL {
namespace Analysis {

    // ************************************************************************ //
    // ************************ Analysis Memento class ************************ //

    PCFGAnalysis_memento::PCFGAnalysis_memento( )
        : _pcfgs( ), _constants_propagation( false ), _canonical( false ), _use_def( false ), _liveness( false ),
          _loops( false ), _reaching_definitions( false ), _induction_variables( false ),
          _auto_scoping( false ), _auto_deps( false )
    {}

    ExtensibleGraph* PCFGAnalysis_memento::get_pcfg( std::string name )
    {
        ExtensibleGraph* pcfg = NULL;
        Name_to_pcfg_map::iterator pcfgs_it = _pcfgs.find( name );

        if( pcfgs_it != _pcfgs.end( ) )
            pcfg = _pcfgs[name];

        return pcfg;
    }

    void PCFGAnalysis_memento::set_pcfg( std::string name, ExtensibleGraph* pcfg )
    {
        _pcfgs[name] = pcfg;
    }

    bool PCFGAnalysis_memento::is_constants_propagation_computed( ) const
    {
        return _constants_propagation;
    }

    void PCFGAnalysis_memento::set_constants_propagation_computed( )
    {
        _constants_propagation = true;
    }

    bool PCFGAnalysis_memento::is_canonical_computed( ) const
    {
        return _canonical;
    }

    void PCFGAnalysis_memento::set_canonical_computed( )
    {
        _canonical = true;
    }

    bool PCFGAnalysis_memento::is_usage_computed( ) const
    {
        return _use_def;
    }

    void PCFGAnalysis_memento::set_usage_computed( )
    {
        _use_def = true;
    }

    bool PCFGAnalysis_memento::is_liveness_computed( ) const
    {
        return _liveness;
    }

    void PCFGAnalysis_memento::set_liveness_computed( )
    {
        _liveness = true;
    }

    bool PCFGAnalysis_memento::is_loops_computed( ) const
    {
        return _loops;
    }

    void PCFGAnalysis_memento::set_loops_computed( )
    {
        _loops = true;
    }

    bool PCFGAnalysis_memento::is_reaching_definitions_computed( ) const
    {
        return _reaching_definitions;
    }

    void PCFGAnalysis_memento::set_reaching_definitions_computed( )
    {
        _reaching_definitions = true;
    }

    bool PCFGAnalysis_memento::is_induction_variables_computed( ) const
    {
        return _induction_variables;
    }

    void PCFGAnalysis_memento::set_induction_variables_computed( )
    {
        _induction_variables = true;
    }

    bool PCFGAnalysis_memento::is_auto_scoping_computed( ) const
    {
        return _auto_scoping;
    }

    void PCFGAnalysis_memento::set_auto_scoping_computed( )
    {
        _auto_scoping = true;
    }

    bool PCFGAnalysis_memento::is_auto_deps_computed( ) const
    {
        return _auto_deps;
    }

    void PCFGAnalysis_memento::set_auto_deps_computed( )
    {
        _auto_deps = true;
    }

    Node* PCFGAnalysis_memento::pcfg_node_enclosing_nodecl( Node* current, const Nodecl::NodeclBase& n )
    {
        Node* result = NULL;
        if( !current->is_visited( ) )
        {
            current->set_visited( true );
            if( current->is_exit_node( ) )
            {
                return NULL;
            }
            else if( current->is_graph_node( ) )
            {
                // We a node is found as label of a PCFGNode, i might be confusing:
                // When calling to analysis with a non-TopLevel node, both the EXTENSIBLE_GRAPH and
                // the non-TopLevel node, have the same label, so we have to look for the one that
                // is not the EXTENSIBLE_GRAPH node.
                if( Nodecl::Utils::equal_nodecls( current->get_graph_label( ), n )
                    && ( n.is<Nodecl::FunctionCode>( )
                         || n.is<Nodecl::OpenMP::SimdFunction>( )
                         || ( !n.is<Nodecl::FunctionCode>() && !current->is_extended_graph_node( ) ) ) )
                {
                    result = current;
                }
                else
                {
                    result = pcfg_node_enclosing_nodecl( current->get_graph_entry_node( ), n );
                }
            }
            else if( !current->is_entry_node( ) )
            {
                ObjectList<Nodecl::NodeclBase> stmts = current->get_statements( );
                for( ObjectList<Nodecl::NodeclBase>::iterator it = stmts.begin( );
                     it != stmts.end( ); ++it )
                {
                    if( Nodecl::Utils::equal_nodecls( *it, n ) )
                    {
                        result = current;
                        break;
                    }
                }
            }

            if( result == NULL )
            {
                ObjectList<Node*> children = current->get_children( );
                for( ObjectList<Node*>::iterator it = children.begin( );
                     it != children.end( ); ++it )
                {
                    result = pcfg_node_enclosing_nodecl( *it, n );
                    if( result != NULL )
                    {
                        break;
                    }
                }
            }
        }
        return result;
    }

    Node* PCFGAnalysis_memento::node_enclosing_nodecl( const Nodecl::NodeclBase& n )
    {
        Node* result;
        for( Name_to_pcfg_map::iterator it = _pcfgs.begin( ); it != _pcfgs.end( ); ++it )
        {
            Node* current = it->second->get_graph( );
            result = pcfg_node_enclosing_nodecl( current, n );
            ExtensibleGraph::clear_visits( current );

            if( result != NULL )
            {
                break;
            }
        }

        if( result == NULL )
        {
            nodecl_t internal_n = n.get_internal_nodecl( );
            WARNING_MESSAGE( "Nodecl '%s' do not found in current memento. "\
                             "You probably misstepped during the analysis.",
                             codegen_to_str( internal_n, nodecl_retrieve_context( internal_n ) ) );
        }

        return result;
    }

    static void get_inner_loops_induction_variables( Node* pcfg_node,
                                                     ObjectList<Utils::InductionVariableData*>& result )
    {
        if( !pcfg_node->is_visited( ) )
        {
            pcfg_node->set_visited( true );

            if( pcfg_node->is_exit_node( ) )
            {
                return;
            }
            else
            {
                if( pcfg_node->is_graph_node( ) )
                {
                    // Look for inner loops
                    get_inner_loops_induction_variables( pcfg_node->get_graph_entry_node( ), result );

                    // Add current induction variables
                    if( pcfg_node->is_loop_node( ) )
                    {
                        result.append( pcfg_node->get_induction_variables( ) );
                    }
                }

                ObjectList<Node*> children = pcfg_node->get_children( );
                for( ObjectList<Node*>::iterator it = children.begin( ); it != children.end( ); ++it )
                {
                    get_inner_loops_induction_variables( *it, result );
                }
            }
        }
    }

    ObjectList<Utils::InductionVariableData*> PCFGAnalysis_memento::get_induction_variables(
            const Nodecl::NodeclBase& n )
    {
        ObjectList<Utils::InductionVariableData*> result;
        if( _induction_variables )
        {
            Node* pcfg_node = node_enclosing_nodecl( n );
            if( pcfg_node != NULL )
            {
                get_inner_loops_induction_variables( pcfg_node, result );
                ExtensibleGraph::clear_visits( pcfg_node );
            }
        }
        return result;
    }

    Utils::ext_sym_set PCFGAnalysis_memento::get_killed( const Nodecl::NodeclBase& n )
    {
        Utils::ext_sym_set result;

        Node* n_pcfg_node = node_enclosing_nodecl( n );
        if( n_pcfg_node != NULL )
        {
            result = n_pcfg_node->get_killed_vars( );
        }

        return result;
    }

    void PCFGAnalysis_memento::reset_state( )
    {
        _constants_propagation = false;
        _canonical = false;
        _use_def = false;
        _liveness = false;
        _loops = false;
        _reaching_definitions = false;
        _induction_variables = false;
        _auto_scoping = false;
        _auto_deps = false;
    }

    // ********************** END Analysis Memento class ********************** //
    // ************************************************************************ //



    // ************************************************************************ //
    // *********** Analysis Singleton class ( Memento originator ) ************ //

    // Private constructor
    AnalysisSingleton::AnalysisSingleton( )
    {}

    // Single instance constructor
    AnalysisSingleton& AnalysisSingleton::get_analysis( )
    {
        static AnalysisSingleton analysis;
        return analysis;
    }

    ObjectList<ExtensibleGraph*> AnalysisSingleton::parallel_control_flow_graph( PCFGAnalysis_memento& memento,
                                                                                 Nodecl::NodeclBase ast )
    {
        ObjectList<ExtensibleGraph*> result;
        ObjectList<Nodecl::NodeclBase> unique_asts;

        // Get all unique ASTs embedded in 'ast'
        if( !ast.is<Nodecl::TopLevel>( ) )
        {
            unique_asts.append( ast );
        }
        else
        {
            // Get all functions in \ast
            Utils::TopLevelVisitor tlv;
            tlv.walk_functions( ast );
            unique_asts = tlv.get_functions( );
        }

        // Compute the PCFG corresponding to each AST
        for( ObjectList<Nodecl::NodeclBase>::iterator it = unique_asts.begin( ); it != unique_asts.end( ); ++it )
        {
            // Generate the hashed name corresponding to the AST of the function
            std::string pcfg_name = Utils::generate_hashed_name( *it );

            // Create the PCFG only if it has not been created previously
            if( memento.get_pcfg( pcfg_name ) == NULL )
            {
                if( VERBOSE )
                    std::cerr << "- Generating PCFG '" << pcfg_name << "'" << std::endl;

                // Create the PCFG
                PCFGVisitor v( pcfg_name, *it );
                ExtensibleGraph* pcfg = v.parallel_control_flow_graph( *it );

                // Store the pcfg in the singleton
                memento.set_pcfg( pcfg_name, pcfg );
                result.append( pcfg );
            }
            else
            {
                result.append( memento.get_pcfg( pcfg_name ) );
            }
        }

        return result;
    }

    // TODO
    void AnalysisSingleton::conditional_constant_propagation( PCFGAnalysis_memento& memento,
                                                              Nodecl::NodeclBase ast )
    {
        if( !memento.is_constants_propagation_computed( ) )
        {
            memento.set_constants_propagation_computed( );

            ObjectList<ExtensibleGraph*> pcfgs = parallel_control_flow_graph( memento, ast );

            for( ObjectList<ExtensibleGraph*>::iterator it = pcfgs.begin( ); it != pcfgs.end( ); ++it )
            {
                if( VERBOSE )
                    std::cerr << "- Constants propagation of PCFG '" << (*it )->get_name( ) << "'" << std::endl;
                std::cerr << "Constants Propagation is not yet implemented" << std::endl;
                // ConditionalConstantAnalysis ca( ipa );
                // ca.conditional_constant_propagation( pcfg );
            }
        }
    }

    // TODO
    void AnalysisSingleton::expression_canonicalization( PCFGAnalysis_memento& memento,
                                                         Nodecl::NodeclBase ast )
    {

    }

    ObjectList<ExtensibleGraph*> AnalysisSingleton::use_def( PCFGAnalysis_memento& memento, Nodecl::NodeclBase ast )
    {
        ObjectList<ExtensibleGraph*> pcfgs = parallel_control_flow_graph( memento, ast );

        if( !memento.is_usage_computed( ) )
        {
            memento.set_usage_computed( );

            for( ObjectList<ExtensibleGraph*>::iterator it = pcfgs.begin( ); it != pcfgs.end( ); ++it )
            {
                if( VERBOSE )
                    std::cerr << "- Use-Definition of PCFG '" << ( *it )->get_name( ) << "'" << std::endl;
                UseDef ud( *it );

                ObjectList<TL::Symbol> visited_functions;
                if( (*it)->get_function_symbol( ).is_valid( ) )
                    visited_functions.insert( (*it)->get_function_symbol( ) );
                ObjectList<Utils::ExtendedSymbolUsage> visited_global_vars =
                    ObjectList<Utils::ExtendedSymbolUsage>( ( *it )->get_global_variables( ) );
                ud.compute_usage( visited_functions, visited_global_vars );
            }
        }

        return pcfgs;
    }

    ObjectList<ExtensibleGraph*> AnalysisSingleton::liveness( PCFGAnalysis_memento& memento, Nodecl::NodeclBase ast )
    {
        ObjectList<ExtensibleGraph*> pcfgs = use_def( memento, ast );

        if( !memento.is_liveness_computed( ) )
        {
            memento.set_liveness_computed( );

            for( ObjectList<ExtensibleGraph*>::iterator it = pcfgs.begin( ); it != pcfgs.end( ); ++it )
            {
                if( VERBOSE )
                    std::cerr << "- Liveness of PCFG '" << ( *it )->get_name( ) << "'" << std::endl;
                Liveness l( *it );
                l.compute_liveness( );
            }
        }

        return pcfgs;
    }

    ObjectList<ExtensibleGraph*> AnalysisSingleton::reaching_definitions( PCFGAnalysis_memento& memento, Nodecl::NodeclBase ast )
    {
        ObjectList<ExtensibleGraph*> pcfgs = liveness( memento, ast );

        if( !memento.is_reaching_definitions_computed( ) )
        {
            memento.set_reaching_definitions_computed( );

            for( ObjectList<ExtensibleGraph*>::iterator it = pcfgs.begin( ); it != pcfgs.end( ); ++it )
            {
                if( VERBOSE )
                    std::cerr << "- Reaching Definitions of PCFG '" << (*it )->get_name( ) << "'" << std::endl;
                ReachingDefinitions rd( *it );
                rd.compute_reaching_definitions( );
            }
        }

        return pcfgs;
    }

    ObjectList<ExtensibleGraph*> AnalysisSingleton::induction_variables( PCFGAnalysis_memento& memento, Nodecl::NodeclBase ast )
    {
        ObjectList<ExtensibleGraph*> pcfgs = reaching_definitions( memento, ast );

        if( !memento.is_induction_variables_computed( ) )
        {
            memento.set_induction_variables_computed( );

            for( ObjectList<ExtensibleGraph*>::iterator it = pcfgs.begin( ); it != pcfgs.end( ); ++it )
            {
                if( VERBOSE )
                    std::cerr << "- Induction Variables of PCFG '" << (*it )->get_name( ) << "'" << std::endl;

                // Compute the induction variables of all loops of each PCFG
                InductionVariableAnalysis iva( *it );
                iva.compute_induction_variables( );

                // Compute the limits of the induction variables
                Utils::InductionVarsPerNode ivs = iva.get_all_induction_vars( );
                LoopAnalysis la( *it, ivs );
                la.compute_loop_ranges( );

                if( VERBOSE )
                    print_induction_vars( ivs );
            }
        }

        return pcfgs;
    }

    void AnalysisSingleton::print_pcfg( PCFGAnalysis_memento& memento, std::string pcfg_name )
    {
        ExtensibleGraph* pcfg = memento.get_pcfg( pcfg_name );
        pcfg->print_graph_to_dot( memento.is_usage_computed( ), memento.is_liveness_computed( ),
                                  memento.is_reaching_definitions_computed( ),
                                  memento.is_auto_scoping_computed( ), memento.is_auto_deps_computed( ) );
    }
}
}
