/*--------------------------------------------------------------------
(C) Copyright 2006-2009 Barcelona Supercomputing Center 
Centro Nacional de Supercomputacion

This file is part of Mercurium C/C++ source-to-source compiler.

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


#ifndef TL_LOOP_ANALYSIS_HPP
#define TL_LOOP_ANALYSIS_HPP


#include "tl-nodecl.hpp"
#include "tl-node.hpp"
#include "tl-symbol.hpp"


namespace TL
{
    namespace Analysis
    {
        struct InductionVarInfo {
            Symbol _s;
            Nodecl::NodeclBase _lb;
            Nodecl::NodeclBase _ub;     // value included in the range
            Nodecl::NodeclBase _stride;
            bool _stride_is_one;
            int _stride_is_positive;
            
            InductionVarInfo(Symbol s, Nodecl::NodeclBase lb);
            
            // *** Getters and setters *** //
            Symbol get_symbol() const;
            Type get_type() const;
            Nodecl::NodeclBase get_lb() const;
            void set_lb(Nodecl::NodeclBase lb);
            Nodecl::NodeclBase get_ub() const;
            void set_ub(Nodecl::NodeclBase ub);
            Nodecl::NodeclBase get_stride() const;
            void set_stride(Nodecl::NodeclBase stride);
            /*!\return 0 if negative, 1 if positive, 2 if we cannot compute it
            */        
            int stride_is_positive() const;
            void set_stride_is_positive(int stride_is_positive);
            
            bool operator==(const InductionVarInfo &v) const;
            bool operator<(const InductionVarInfo &v) const;
        };
        
        struct Node_hash {
                size_t operator() (const int& n) const;
        };
        
        struct Node_comp {
                bool operator() (const int& n1, const int& n2) const;
        };
        
        typedef std::tr1::unordered_multimap<int, InductionVarInfo*, Node_hash, Node_comp> induc_vars_map;
        
        class LIBTL_CLASS LoopAnalysis {    
            
        private:
            induc_vars_map _induction_vars;
            
            // *** Private methods *** //
            void compute_loop_induction_vars(Node* loop_node);
            void traverse_loop_init(Node* loop_node, Nodecl::NodeclBase init);
            void traverse_loop_cond(Node* loop_node, Nodecl::NodeclBase cond);
            void traverse_loop_stride(Node* loop_node, Nodecl::NodeclBase stride);
            
            void compute_ranges_for_variables_in_loop(Node* node, Node* loop_node);
            
            Nodecl::NodeclBase set_access_range(Node* node, Node* loop_node, const char use_type, Nodecl::NodeclBase nodecl, 
                                                std::map<Symbol, Nodecl::NodeclBase> ind_var_map,
                                                Nodecl::NodeclBase reach_def_var = Nodecl::NodeclBase::null());
            
            /*!
            * Looks for any induction variable contained in a list of nodecls and
            * substitutes its scalar access by the corresponding ranged access computed from the loop boundaries
            * \param node Node of the graph we are analysing right now
            * \param nodecl_l list containing the potential nodecls where we want to substitute an scalar by a range
            * \param use_type kind of the list we are analysing; it can be a UpperExposed list, a Killed list, a Undefined_behaviour list 
            *                 or a ReachingDefintions list
            */
            void set_access_range_in_ext_sym_set(Node* node, Node* loop_node, ext_sym_set nodecl_l, const char use_type);
            /*!
            * Wrapping method for #set_access_range_in_ext_sym_list in the case we traverse a nodecl map container
            */
            void set_access_range_in_nodecl_map(Node* node, Node* loop_node, nodecl_map nodecl_m);
            /*!
            * Deletes those induction variables included in the list during a previous traverse through the loop control 
            * that are redefined within the loop
            * \param node Node in the graph we are analysing
            * \param loop_node Outer loop node where is contained the node we are checking
            */
            void delete_false_induction_vars(Node* node, Node* loop_node);
            
            void prettyprint_induction_var_info(InductionVarInfo* var_info);
            
            void print_induction_vars_in_loop_info(Node* loop_node);
            
            
        public:
            
            // *** Constructors *** //
            LoopAnalysis();
        
            
            // *** Modifiers *** //
            /*!
            * Calculates the induction variables from a loop control
            * \param loop_node node of the graph containing a ForStatement graph node
            */
            void compute_induction_variables_info(Node* loop_node);
            
            /*!
            * Looks for loops in the graph hanging up from a node and 
            * computes the ranges for induction variables of each loop
            * \param node Node form the graph used to start the up-bottom analysis
            */
            void compute_ranges_for_variables(Node* node);
            
            /*!
            * The method recomputes use-def and reaching definitions in a graph taking into account loop information
            * \param node Node containing the graph to be analysed
            */
            void analyse_loops(Node* node);
            
            
            // *** Getters and setters *** //
            InductionVarInfo* induction_vars_l_contains_symbol(Node*, Symbol s) const;
        
            std::map<Symbol, Nodecl::NodeclBase> get_induction_vars_mapping(Node* loop_node) const;
            
            std::map<Symbol, int> get_induction_vars_direction(Node* loop_node) const;
            
            
            // *** Utils *** //
            void print_induction_vars_info();
            
            friend class StaticAnalysis;
        };
    }
}

#endif      // TL_LOOP_ANALYSIS_HPP