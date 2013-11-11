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

#ifndef TL_ANALYSIS_STATIC_INFO_HPP
#define TL_ANALYSIS_STATIC_INFO_HPP

#include "tl-analysis-singleton.hpp"
#include "tl-induction-variables-data.hpp"
#include "tl-nodecl-visitor.hpp"
#include "tl-objectlist.hpp"

namespace TL {
namespace Analysis {

    // ********************************************************************************************* //
    // ********************** Class to define which analysis are to be done ************************ //

    struct WhichAnalysis
    {
        // Macros defining the analysis to be computed
        enum Analysis_tag
        {
            PCFG_ANALYSIS           = 1u << 1,
            USAGE_ANALYSIS          = 1u << 2,
            LIVENESS_ANALYSIS       = 1u << 3,
            REACHING_DEFS_ANALYSIS  = 1u << 4,
            INDUCTION_VARS_ANALYSIS = 1u << 5,
            CONSTANTS_ANALYSIS      = 1u << 6,
            AUTO_SCOPING            = 1u << 7
        } _which_analysis;

        WhichAnalysis( Analysis_tag a );
        WhichAnalysis( int a );
        WhichAnalysis operator|( WhichAnalysis a );
    };

    struct WhereAnalysis
    {
        // Macros defining whether the Static Info must be computed in nested block
        enum Nested_analysis_tag
        {
            NESTED_NONE_STATIC_INFO         = 1u << 0,
            NESTED_IF_STATIC_INFO           = 1u << 2,
            NESTED_DO_STATIC_INFO           = 1u << 3,
            NESTED_WHILE_STATIC_INFO        = 1u << 4,
            NESTED_FOR_STATIC_INFO          = 1u << 5,
            NESTED_OPENMP_TASK_STATIC_INFO  = 1u << 6,
            NESTED_ALL_STATIC_INFO          = 0xFF
        } _where_analysis;

        WhereAnalysis( Nested_analysis_tag a );
        WhereAnalysis( int a );
        WhereAnalysis operator|( WhereAnalysis a );
    };

    // ******************** END class to define which analysis are to be done ********************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // **************** Class to retrieve analysis info about one specific nodecl ****************** //

    class NodeclStaticInfo
    {
        private:
            ObjectList<Utils::InductionVariableData*> _induction_variables;
            ObjectList<Symbol> _reductions;
            Utils::ext_sym_set _killed;
            ObjectList<ExtensibleGraph*> _pcfgs;
            Node* _autoscoped_task;

            Node* find_node_from_nodecl( const Nodecl::NodeclBase& n ) const;
            
        public:
            NodeclStaticInfo( ObjectList<Utils::InductionVariableData*> induction_variables,
                              ObjectList<Symbol> reductions,
                              Utils::ext_sym_set killed, ObjectList<ExtensibleGraph*> pcfgs, 
                              Node* autoscoped_task );

            
            // *** Queries about Use-Def analysis *** //

            bool is_constant( const Nodecl::NodeclBase& n ) const;

            bool has_been_defined( const Nodecl::NodeclBase& n, 
                                   const Nodecl::NodeclBase& s, 
                                   const Nodecl::NodeclBase& scope ) const;

            // *** Queries about induction variables *** //

            bool is_induction_variable( const Nodecl::NodeclBase& n ) const;

            bool is_basic_induction_variable( const Nodecl::NodeclBase& n ) const;

            bool is_non_reduction_basic_induction_variable( const Nodecl::NodeclBase& n ) const;
            
            Nodecl::NodeclBase get_induction_variable_increment( const Nodecl::NodeclBase& n ) const;

            ObjectList<Nodecl::NodeclBase> get_induction_variable_increment_list( const Nodecl::NodeclBase& n ) const;
            
            bool is_induction_variable_increment_one( const Nodecl::NodeclBase& n ) const;

            //! Returns the induction variable containing the given nodecl
            //! If the nodecl is not an induction variable, then returns NULL
            Utils::InductionVariableData* get_induction_variable( const Nodecl::NodeclBase& n ) const;

            ObjectList<Utils::InductionVariableData*> get_induction_variables( const Nodecl::NodeclBase& n ) const;
            
            // *** Queries for Vectorization *** //
            
            bool is_adjacent_access( const Nodecl::NodeclBase& n ) const;
            
            bool contains_induction_variable( const Nodecl::NodeclBase& n ) const;

            bool is_constant_access( const Nodecl::NodeclBase& n ) const;

            bool is_simd_aligned_access( const Nodecl::NodeclBase& n, const Nodecl::List* suitable_expressions, 
                                         int unroll_factor, int alignment ) const;

            
            // *** Queries about Auto-Scoping *** //

            void print_auto_scoping_results( ) const;

            Utils::AutoScopedVariables get_auto_scoped_variables( );
    };

    // ************** END class to retrieve analysis info about one specific nodecl **************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // **************************** User interface for static analysis ***************************** //

    typedef std::map<Nodecl::NodeclBase, NodeclStaticInfo> static_info_map_t;
    typedef std::pair<Nodecl::NodeclBase, NodeclStaticInfo> static_info_pair_t;

    class AnalysisStaticInfo
    {
        private:
            static_info_map_t _static_info_map;

            Nodecl::NodeclBase _node;

        public:
            AnalysisStaticInfo( const Nodecl::NodeclBase& n, WhichAnalysis analysis_mask,
                                WhereAnalysis nested_analysis_mask, int nesting_level );


            // *** Getters and Setters *** //

            static_info_map_t get_static_info_map( ) const;

            //! Returns the nodecl that originated the analysis
            Nodecl::NodeclBase get_nodecl_origin( ) const;


            // *** Queries about Use-Def analysis *** //

            //! Returns true when an object is constant in a given scope
            bool is_constant( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;

            bool has_been_defined( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n, 
                                   const Nodecl::NodeclBase& s ) const;

            // *** Queries about induction variables *** //

            //! Returns true when an object is an induction variable in a given scope
            bool is_induction_variable( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;

            //! Returns true when an object is an induction variable in a given scope
            bool is_basic_induction_variable( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;

            bool is_non_reduction_basic_induction_variable( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;
            
            //! Returns the const_value corresponding to the increment of an induction variable in a given scope
            Nodecl::NodeclBase get_induction_variable_increment( const Nodecl::NodeclBase& scope,
                                                             const Nodecl::NodeclBase& n ) const;

            //! Returns the list of const_values containing the increments of an induction variable in a given scope
            ObjectList<Nodecl::NodeclBase> get_induction_variable_increment_list( const Nodecl::NodeclBase& scope,
                                                                                  const Nodecl::NodeclBase& n ) const;
                                                             
            //! Returns true when the increment of a given induction variable is constant and equal to 1
            bool is_induction_variable_increment_one( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;

            //! Returns a list with the induction variables of a given scope
            ObjectList<Utils::InductionVariableData*> get_induction_variables( const Nodecl::NodeclBase& scope,
                                                                               const Nodecl::NodeclBase& n ) const;

            //! Returns true if the given nodecl is an array accessed by adjacent positions
            bool is_adjacent_access( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;

            //! Returns true if the given nodecl is an array accessed by an IV
            bool is_induction_variable_dependent_access( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;

            //! Returns true if the given nodecl is an array accessed by a constant expression
            bool is_constant_access( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const;

            //! Returns true if the given nodecl is aligned to a given value
            bool is_simd_aligned_access( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n, 
                                         const Nodecl::List* suitable_expressions, int unroll_factor, int alignment ) const;
            
            // *** Queries about Auto-Scoping *** //

            void print_auto_scoping_results( const Nodecl::NodeclBase& scope );

            Utils::AutoScopedVariables get_auto_scoped_variables( const Nodecl::NodeclBase scope );
    };

    // ************************** END User interface for static analysis *************************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // ********************* Visitor retrieving the analysis of a given Nodecl ********************* //

    class LIBTL_CLASS NestedBlocksStaticInfoVisitor : public Nodecl::ExhaustiveVisitor<void>
    {
    private:
        //! State of the analysis for the given nodecl
        PCFGAnalysis_memento _state;

        //! Mask containing the analysis to be performed
        WhichAnalysis _analysis_mask;

        //! Mask containing the nested constructs to be parsed
        WhereAnalysis _nested_analysis_mask;

        //! Level of nesting of blocks inside the visited nodecl to be parsed
        int _nesting_level;

        //! Temporary member used during computation. Contains the current level of nesting
        int _current_level;

        //! Member where the analysis info is stored during the visit
        static_info_map_t _analysis_info;

        void retrieve_current_node_static_info( Nodecl::NodeclBase n );

    public:
        // *** Constructor *** //
        NestedBlocksStaticInfoVisitor( WhichAnalysis analysis_mask, WhereAnalysis nested_analysis_mask,
                                       PCFGAnalysis_memento state, int nesting_level );

        // *** Getters and Setters *** //
        static_info_map_t get_analysis_info( );

        // *** Visiting methods *** //
        void join_list( ObjectList<static_info_map_t>& list );

        void visit( const Nodecl::DoStatement& n );
        void visit( const Nodecl::ForStatement& n );
        void visit( const Nodecl::FunctionCode& n );
        void visit( const Nodecl::IfElseStatement& n );
        void visit( const Nodecl::OpenMP::Task& n );
        void visit( const Nodecl::WhileStatement& n );
    };

    // ******************* END Visitor retrieving the analysis of a given Nodecl ******************* //
    // ********************************************************************************************* //


    
    // ********************************************************************************************* //
    // ************************ Visitor retrieving suitable simd alignment ************************* //
    
    class LIBTL_CLASS SuitableAlignmentVisitor : public Nodecl::NodeclVisitor<int>
    {
    private:
        const ObjectList<Utils::InductionVariableData*> _induction_variables;
        const Nodecl::List* _suitable_expressions;
        const int _unroll_factor;
        const int _type_size;
        const int _alignment;
        int _nesting_level;
        
        bool is_suitable_expression( Nodecl::NodeclBase n );
        bool is_suitable_constant( int n );
        
    public:
        // *** Constructor *** //
        SuitableAlignmentVisitor( ObjectList<Utils::InductionVariableData*> induction_variables,
                                  const Nodecl::List* suitable_expressions, 
                                  int unroll_factor, int type_size, int alignment );
        
        // *** Visiting methods *** //
        Ret join_list( ObjectList<int>& list );
        
        Ret visit( const Nodecl::Add& n );
        Ret visit( const Nodecl::ArraySubscript& n );
        Ret visit( const Nodecl::BitwiseShl& n );
        Ret visit( const Nodecl::BitwiseShr& n );
        Ret visit( const Nodecl::Conversion& n );
        Ret visit( const Nodecl::IntegerLiteral& n );
        Ret visit( const Nodecl::Minus& n );
        Ret visit( const Nodecl::Mul& n );
        Ret visit( const Nodecl::ParenthesizedExpression& n );
        Ret visit( const Nodecl::Symbol& n );
        
        Ret unhandled_node(const Nodecl::NodeclBase& n);
    };
    
    // ********************** END visitor retrieving suitable simd alignment *********************** //
    // ********************************************************************************************* //
    
    
    
    // ********************************************************************************************* //
    // ***************** Visitor retrieving adjacent array accesses within a loop ****************** //
    
    // The return value indicates whether the visit returns a constant value
    class LIBTL_CLASS ArrayAccessInfoVisitor : public Nodecl::NodeclVisitor<bool>
    {
    private:
        ObjectList<Utils::InductionVariableData*> _induction_variables; /* All IVs in the containing loop */
        Utils::ext_sym_set _killed;                                     /* All killed variables in the containing loop */
        ObjectList<Utils::InductionVariableData*> _ivs;                 /* IVs found during traversal */
        bool _is_adjacent_access;
        
        bool variable_is_iv( const Nodecl::NodeclBase& n );
        bool visit_binary_node( const Nodecl::NodeclBase& lhs, const Nodecl::NodeclBase& rhs );
        bool visit_unary_node( const Nodecl::NodeclBase& rhs );
        
    public:
        // *** Constructor *** //
        ArrayAccessInfoVisitor( ObjectList<Utils::InductionVariableData*> ivs, 
                               Utils::ext_sym_set killed );
        
        // *** Consultants *** //
        bool is_adjacent_access( );
        bool depends_on_induction_vars( );
        
        // *** Visiting methods *** //
        Ret unhandled_node( const Nodecl::NodeclBase& n );
        Ret join_list( ObjectList<bool>& list );
        
        Ret visit( const Nodecl::Add& n );
        Ret visit( const Nodecl::ArraySubscript& n );
        Ret visit( const Nodecl::BitwiseShl& n );
        Ret visit( const Nodecl::BitwiseShr& n );
        Ret visit( const Nodecl::BooleanLiteral& n );
        Ret visit( const Nodecl::Cast& n );
        Ret visit( const Nodecl::ComplexLiteral& n );
        Ret visit( const Nodecl::Conversion& n );
        Ret visit( const Nodecl::Div& n );
        Ret visit( const Nodecl::FloatingLiteral& n );
        Ret visit( const Nodecl::FunctionCall& n );
        Ret visit( const Nodecl::IntegerLiteral& n );
        Ret visit( const Nodecl::Minus& n );
        Ret visit( const Nodecl::Mul& n );
        Ret visit( const Nodecl::Neg& n );
        Ret visit( const Nodecl::PointerToMember& n );
        Ret visit( const Nodecl::Postdecrement& n );
        Ret visit( const Nodecl::Postincrement& n );
        Ret visit( const Nodecl::Power& n );
        Ret visit( const Nodecl::Predecrement& n );
        Ret visit( const Nodecl::Preincrement& n );
        Ret visit( const Nodecl::Reference& n );
        Ret visit( const Nodecl::Sizeof& n );
        Ret visit( const Nodecl::StringLiteral& n );
        Ret visit( const Nodecl::Symbol& n );
    };
    
    // *************** END visitor retrieving adjacent array accesses within a loop **************** //
    // ********************************************************************************************* //
}
}

#endif // TL_ANALYSIS_STATIC_INFO_HPP
