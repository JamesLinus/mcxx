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


#ifndef TL_CFG_ANALYSIS_VISITOR_HPP
#define TL_CFG_ANALYSIS_VISITOR_HPP

#include "tl-analysis-common.hpp"
#include "tl-extensible-graph.hpp"
#include "tl-node.hpp"
#include "tl-nodecl-visitor.hpp"

namespace TL
{
    namespace Analysis
    {
        class LIBTL_CLASS CfgAnalysisVisitor : public Nodecl::ExhaustiveVisitor<void>
        {
        protected:
            //! Pointer to the Node in a CFG of type ExtensibleGraph where the Nodecl is contained
            //! The results of the analysis performed during the visit will be attached to the node
            Node* _node;
            
            //! State of the traversal
            /*! 
            * This value will be true when the actual expression is a defined value
            * Otherwise, when the value is just used, the value will be false
            * By default this value will be false. Each time we change the value to true for any definition, 
            * at the end of the recursion, we turn back the value to false
            */
            bool _define;
            
            //! Nodecl we are traversing actually
            /*!
            * This attribute stores the actual nodecl when we are traversing class member access, reference/dereference or array subscript
            */
            Nodecl::NodeclBase _actual_nodecl;
            
            //! Nodecl containing the initializing expression of the symbol which is being defined in the actual statement
            /*!
            * For example, within an assignment, this Nodecl contain the rhs of the assignment
            */
            Nodecl::NodeclBase _init_expression;
            
        private:
            
            //! This method implements the visitor for any Binary operation
            /*!
            * \param n Nodecl containing the Binary operation
            */
            template <typename T>
            Ret binary_visit(const T& n);

            //! This method implements the visitor for any Binary Assignment operation
            /*!
            * \param n Nodecl containing the Binary Assignment operation
            */        
            template <typename T>
            Ret binary_assignment(const T& n);
            
            template <typename T>
            Ret unary_visit(const T& n);
            
            template <typename T>
            void function_visit(const T& n);
        
            
        public:
            // *** Constructors *** //
            CfgAnalysisVisitor(Node* n);
            CfgAnalysisVisitor(const CfgAnalysisVisitor& v);
            
            // *** Visitors *** //
            Ret unhandled_node(const Nodecl::NodeclBase& n);
            // Basic nodes
            Ret visit(const Nodecl::Symbol& n);  
            Ret visit(const Nodecl::ObjectInit& n);
            // Composed nodes
            Ret visit(const Nodecl::ArraySubscript& n);
            Ret visit(const Nodecl::ClassMemberAccess& n);
            // Assignments
            Ret visit(const Nodecl::Assignment& n);
            Ret visit(const Nodecl::AddAssignment& n);
            Ret visit(const Nodecl::SubAssignment& n);
            Ret visit(const Nodecl::DivAssignment& n);
            Ret visit(const Nodecl::MulAssignment& n);
            Ret visit(const Nodecl::ModAssignment& n);
            Ret visit(const Nodecl::BitwiseAndAssignment& n);
            Ret visit(const Nodecl::BitwiseOrAssignment& n);
            Ret visit(const Nodecl::BitwiseXorAssignment& n);
            Ret visit(const Nodecl::ShrAssignment& n);
            Ret visit(const Nodecl::ShlAssignment& n);
            // Binary nodes
            Ret visit(const Nodecl::Comma& n);
            Ret visit(const Nodecl::Concat& n);
            Ret visit(const Nodecl::Add& n);
            Ret visit(const Nodecl::Minus& n);
            Ret visit(const Nodecl::Mul& n);
            Ret visit(const Nodecl::Div& n);
            Ret visit(const Nodecl::Mod& n);
            Ret visit(const Nodecl::Power& n);
            Ret visit(const Nodecl::LogicalAnd& n);
            Ret visit(const Nodecl::LogicalOr& n);
            Ret visit(const Nodecl::BitwiseAnd& n);
            Ret visit(const Nodecl::BitwiseOr& n);
            Ret visit(const Nodecl::BitwiseXor& n);
            Ret visit(const Nodecl::Shr& n);
            Ret visit(const Nodecl::Shl& n);
            Ret visit(const Nodecl::Equal& n);
            Ret visit(const Nodecl::Different& n);
            Ret visit(const Nodecl::LowerThan& n);
            Ret visit(const Nodecl::GreaterThan& n);
            Ret visit(const Nodecl::LowerOrEqualThan& n);
            Ret visit(const Nodecl::GreaterOrEqualThan& n);        
            // Pre-post increments-decrements
            Ret visit(const Nodecl::Predecrement& n);
            Ret visit(const Nodecl::Postdecrement& n);
            Ret visit(const Nodecl::Preincrement& n);
            Ret visit(const Nodecl::Postincrement& n);
            // Unary nodes
            Ret visit(const Nodecl::Derreference& n);
            Ret visit(const Nodecl::Reference& n);
            // FunctionCall (We just do not want this visit to be done)
            Ret visit(const Nodecl::FunctionCall& n);
            Ret visit(const Nodecl::VirtualFunctionCall& n);
        };
        
        //! This visitor parses a nodecl searching global variables and the use
        /*! By default, the usage of the variable is USE. Visitor only marks DEFINITION usages
        */
        class LIBTL_CLASS CfgIPAVisitor : public Nodecl::ExhaustiveVisitor<void>
        {
        private:
            ExtensibleGraph* _cfg;
            ObjectList<ExtensibleGraph*> _cfgs;
            ObjectList<struct var_usage_t*> _global_vars;           // Global variables appearing in the code we are analysing
            ObjectList<Symbol> _params;                             // Parameters of the function we are analysing
            ObjectList<struct var_usage_t*> _usage;                 // Variable where the usage computation is stored
            char _defining;                                         // Temporary value used during the visit
            std::map<Symbol, Nodecl::NodeclBase> _params_to_args;    // Mapping between the parameters of a function and 
                                                                    // the arguments of an specific function call
                                                                    // This value is needed when we have more than one level of IPA
            
            ObjectList<Symbol> _visited_functions;                  // List containing the functions' symbols of all visited functions
                                                                    // This list avoids repeat the same computation over and over
            
            template <typename T>
            void op_assignment_visit(const T& n);
            
            template <typename T>
            void unary_visit(const T& n);
            
            template <typename T>
            void function_visit(const T& n);
            
            /*!
             * Computes the usage of #n depending on the previous uses of this nodecl
             */
            void set_up_symbol_usage(Nodecl::Symbol n);
            /*!
             * Special usage computation when we are dealing with arguments of a function call
             */
            void set_up_argument_usage(Nodecl::Symbol arg);
            
            /*!
             * Once IPA is performed over a graph, the information computed in #_usage is propagated to the proper attributes of the graph
             * This is necessary in the case of recursive calls with parameters between them.
             */
            void fill_graph_usage_info();
            
            /*!
             * Maps a mapping between parameters and arguments in the current mapping (nested IPA analysis)
             */
            std::map<Symbol, Nodecl::NodeclBase> compute_nested_param_args(Nodecl::NodeclBase n, ExtensibleGraph* called_func_graph);
            
            void compute_usage_rec(Node* node);
            
        public:
            
            // *** Constructors *** //
            CfgIPAVisitor(ExtensibleGraph* cfg, ObjectList<ExtensibleGraph*> cfgs, 
                          ObjectList<var_usage_t*> glob_vars, ObjectList<Symbol> parameters,
                          std::map<Symbol, Nodecl::NodeclBase> params_to_args);
            
            // *** Modifiers *** //
            void compute_usage();
            
            // *** Getters and setters *** //
            ObjectList<struct var_usage_t*> get_usage() const;
            static struct var_usage_t* get_var_in_list(Nodecl::Symbol n, ObjectList<struct var_usage_t*> list);
            static struct var_usage_t* get_var_in_list(Symbol n, ObjectList<struct var_usage_t*> list);
            
            // *** Visitors *** //
            Ret visit(const Nodecl::Symbol& n);
            Ret visit(const Nodecl::Assignment& n);
            Ret visit(const Nodecl::AddAssignment& n);
            Ret visit(const Nodecl::SubAssignment& n);
            Ret visit(const Nodecl::DivAssignment& n);
            Ret visit(const Nodecl::MulAssignment& n);
            Ret visit(const Nodecl::ModAssignment& n);
            Ret visit(const Nodecl::BitwiseAndAssignment& n);
            Ret visit(const Nodecl::BitwiseOrAssignment& n);
            Ret visit(const Nodecl::BitwiseXorAssignment& n);
            Ret visit(const Nodecl::ShrAssignment& n);
            Ret visit(const Nodecl::ShlAssignment& n);
            Ret visit(const Nodecl::Predecrement& n);
            Ret visit(const Nodecl::Postdecrement& n);
            Ret visit(const Nodecl::Preincrement& n);
            Ret visit(const Nodecl::Postincrement& n);
            Ret visit(const Nodecl::FunctionCall& n);
            Ret visit(const Nodecl::VirtualFunctionCall& n);
            
            friend class CfgVisitor;
        };       
        
        class LIBTL_CLASS SymbolVisitor : public Nodecl::ExhaustiveVisitor<void>
        {
            // FIXME Incomplete: think about pointers, references and function calls
            //       We store a list of symbols because it is used to compute the usage and at that moment
            //       only valid nodecl symbols can be stored in the usage list
            //       We should change that!
        private:
            ObjectList<Nodecl::Symbol> _symbols;
            
        public:
            SymbolVisitor();
            
            Ret visit(const Nodecl::Symbol& n);
            
            ObjectList<Nodecl::Symbol> get_symbols();
        };
    }
}
    
#endif      // TL_CFG_ANALYSIS_VISITOR_HPP