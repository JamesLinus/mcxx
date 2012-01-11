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


#ifndef STRUCTURES_HPP
#define STRUCTURES_HPP

namespace TL
{
    namespace Analysis
    {
        //! Enumeration of the different node types
        enum Node_type {
            UNCLASSIFIED_NODE,
            // BASIC
            BASIC_ENTRY_NODE,               //! Entry point of a composite node
            BASIC_EXIT_NODE,                //! Exit point of a composite node
            BASIC_NORMAL_NODE,              //! Node representing a Basic Bloc
            BASIC_LABELED_NODE,             //! Node containing an only Labeled Statement
            BASIC_GOTO_NODE,                //! Node containing a GotoStatement
            BASIC_BREAK_NODE,               //! Node containing a BreakStatement
            BASIC_CONTINUE_NODE,            //! Node containing a ContinueStatement
            BASIC_FUNCTION_CALL_NODE,       //! Node containing a Function Call
            FLUSH_NODE,                     //! Node containing an OMP Flush directive
            BARRIER_NODE,                   //! Node containing an OMP Barrier directive
            TASKWAIT_NODE,                  //! Node containing a Taskwait directive
            BASIC_PRAGMA_DIRECTIVE_NODE,    //! Node containing an OMP Pragma directive (not permanent)
            // COMPOSITE
            GRAPH_NODE                      //! Composite node
        };
        
        enum Graph_type {
            EXTENSIBLE_GRAPH,               //! Special graph: this is the most outer graph node in an Extensible Graph
            SPLIT_STMT,                     //! Expression being split because it contains a sub-expression with a separated node
            FUNC_CALL,                      //! Function Call
            COND_EXPR,                      //! Conditional expression
            LOOP,                           //! Set of nodes of a for loop
            OMP_PRAGMA,                     //! Pragma Custom Definition / Statement
            TASK                            //! Pragma Task
        };
        
        enum Loop_type {
            FOR,
            WHILE,
            DO_WHILE,
            GOTO
        };
        
        //! Enumeration of the different edge types
        enum Edge_type
        {
            UNCLASSIFIED_EDGE,
            TRUE_EDGE,              //! Taken when a previous condition is evaluated true
            FALSE_EDGE,             //! Taken when a previous condition is evaluated false
            ALWAYS_EDGE,            //! Always taken edge
            CASE_EDGE,              //! Edge within a Switch statement representing a case/default stmt
            CATCH_EDGE,             //! Handler edge for a Try/Catch statement
            GOTO_EDGE               //! Edge between a GotoNode and a LabeledNode containing the label
        };
        
        //! Definitions of the different node attributes
        /*! \def _NODE_TYPE
        * Type of a node. This will be a value of the enumeration Node_type.
        * Mandatory in all nodes.
        */
        #define _NODE_TYPE      "node_type"
        
        /*! \def _OUTER_NODE
        * Pointer to the node which contains a node. 
        * Only the nodes that are within other nodes has this value set.
        * Mandatory in all nodes.
        */
        #define _OUTER_NODE    "outer_node"
        
        /*! \def _SCOPE
         * Scope where the block code contained in the node is created 
         * Only graph nodes that contain a block of code (all but SPLIT_STMT)
         */
        #define _SCOPE         "scope"
        
        /*! \def _NODE_LABEL
        * String containing the label of a node.
        * It may have different meanings depending on the node type:
        *   - Graph: is the Statement that defines the composition.
        *   - Goto / Label: label that identifies the source or target of the Statement contained.
        * Mandatory and only available in 'Composite', 'Labeled' or 'Goto' nodes.
        */    
        #define _NODE_LABEL     "node_label"
        
        /*! \def _NODE_STMTS
        * List of Statements within the Basic Bloc contained in a node.
        * Mandatory and only available in basic normal nodes.
        */
        #define _NODE_STMTS     "statements"
        
        /*!  \def _ENTRY_NODE
        * Node which is the entry point of a composite node. 
        * All paths will cross this point when traversing the outer composite node.
        * Mandatory and only available in composite nodes.
        */
        #define _ENTRY_NODE     "entry"
        
        /*! \def _EXIT_NODE
        * Node which is the exit point of a composite node. 
        * All paths will cross this point when traversing the outer composite node.
        * Mandatory and only available in composite nodes.
        */
        #define _EXIT_NODE      "exit"

        /*! \def _STRIDE_NODE
        * Node containing the loop stride statement within a loop graph node.
        * Mandatory and only available in Loop Graph nodes.
        */
        #define _STRIDE_NODE      "stride"

        /*! \def _GRAPH_TYPE
        * Type of the graph node. This will be a value of the enumeration Graph_type.
        * Mandatory in all graph nodes.
        */    
        #define _GRAPH_TYPE     "graph_type"
        
        /*! \def _LOOP_TYPE
        * Type of the loop in a graph node. This will be a value of the enumeration Loop_type.
        * Mandatory in all loop graph nodes.
        */    
        #define _LOOP_TYPE      "loop_type"    
        
        /*!
        * Nodecl containing the context associated to a task
        * Mandatory and only available in composite nodes with _GRAPH_TYPE "task"
        */
        #define _TASK_CONTEXT   "task_context"
        
        /*!
        * Nodecl containing the symbol of the function contained contained in task
        * Mandatory and only available in composite nodes with _GRAPH_TYPE "task" of declaration level tasks
        */
        #define _TASK_FUNCTION  "task_function"
        
        /*! \def _LIVE_IN
        * Set of variables that are alive at the entry point of a node.
        * Available in all nodes (Mandatory once the Liveness analysis is performed).
        */   
        #define _LIVE_IN        "live_in_vars"
        
        /*! \def _LIVE_OUT
        * Set of variables that are alive at the exit point of a node.
        * Available in all nodes (Mandatory once the Liveness analysis is performed).
        */       
        #define _LIVE_OUT       "live_out_vars"
        
        /*! \def _UPPER_EXPOSED
        * Set of upper exposed variables within a node.
        * Available in all nodes (Mandatory once the Liveness analysis is performed).
        */ 
        #define _UPPER_EXPOSED  "ue_vars"
        
        /*! \def _KILLED
        * Set of killed variables within a node.
        * Available in all nodes (Mandatory once the Liveness analysis is performed).
        */ 
        #define _KILLED         "killed_vars"

        /*! \def _UNDEF
        * Set of variables within a node that we cannot define the behaviour.
        * Available in all nodes (Mandatory once the Liveness analysis is performed).
        */ 
        #define _UNDEF          "undefined_behaviour_vars"

        /*! \def _SHARED
        * Set of symbols with shared auto-scoping in a task
        * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once Auto-scoping is performed).
        */ 
        #define _SHARED         "shared"

        /*! \def _PRIVATE
        * Set of symbols with private auto-scoping in a task
        * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once Auto-scoping is performed).
        */ 
        #define _PRIVATE        "private"
        
        /*! \def _LASTPRIVATE
        * Set of symbols with lastprivate auto-scoping in a task
        * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once Auto-scoping is performed).
        */ 
        #define _LASTPRIVATE    "lastprivate"
        
        /*! \def _UNDEF_SC
        * Set of symbols with non-computable auto-scoping in a task
        * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once Auto-scoping is performed).
        */ 
        #define _UNDEF_SC       "undef_scope"
        
        /*! \def _IN_DEPS
        * Set of symbols with input dependence in a task
        * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once the Liveness analysis is performed).
        */ 
        #define _IN_DEPS        "input_deps"
        
        /*! \def _OUT_DEPS
        * Set of symbols with output dependence in a task
        * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once the Liveness analysis is performed).
        */ 
        #define _OUT_DEPS       "output_deps"    

        /*! \def _INOUT_DEPS
        * Set of symbols with inout dependence in a task
        * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once the Liveness analysis is performed).
        */ 
        #define _INOUT_DEPS     "inout_deps"

        /*! \def _UNDEF_DEPS
         * Set of symbols with which we are unable to compute the proper dependencies in a task
         * Available Graph nodes with 'task' _GRAPH_TYPE (Mandatory once the Liveness analysis is performed).
         */ 
        #define _UNDEF_DEPS     "undef_deps"
        
        /*! \def _INDUCTION_VARS
        * Map containing the induction variables associated with a Loop Node
        * Available only in Loop (Graph) nodes (Mandatory once the Loop analysis is performed).
        */ 
        #define _INDUCTION_VARS "induction_vars"

        /*! \def _REACH_DEFS
        * Map containing the reaching definitions in a given point
        * Available in all nodes (Mandatory once the Liveness analysis is performed).
        */ 
        #define _REACH_DEFS      "reaching_defs"   

        /*! \def _AUX_REACH_DEFS
        * Map containing the propagated reaching definitions in a given point
        * This varaible is used while propagating the reaching definitions among the nodes to differentiate 
        * those definitions performed within the node and those that has been propagated
        * At the end of the propagation, the reaching definitions stored in this value are copied in the _REACH_DEFS varaible
        * an this is deleted
        * Available in all nodes (Mandatory once the Liveness analysis is performed).
        */ 
        #define _AUX_REACH_DEFS      "aux_reaching_defs"   

        /*! \def _CLAUSES
        * Set of clauses associated to a pragma
        * Available in Graph nodes of type 'omp_pragma' and 'task' but not mandat
        */ 
        #define _CLAUSES        "clauses"
        
        /*! \def _ARGS
        * Empty clause associated to a pragma
        * Available in Graph nodes of type 'omp_pragma' and 'task'.
        */ 
        #define _ARGS           "args"
        
        //! Definitions of the different edge attributes
        /*! \def _EDGE_TYPE
        * Type of the edge. This will be a value of the enumeration Edge_type.
        * Mandatory in all edges.
        */
        #define _EDGE_TYPE      "edge_type"
        
        /*! \def _EDGE_LABEL
        * String containing the label of an edge.
        * Available and mandatory in all edges but those with 'Always' type.
        */
        #define _EDGE_LABEL     "edge_label"
        
        /*! \def _IS_BACK_EDGE
        * Boolean indicating whether an edge connects a source with a target with a back edge (loop)
        * Available and mandatory in all edges.
        */
        #define _IS_BACK_EDGE     "is_back_edge"
        
        /*! \def _IS_TASK_EDGE
        * Boolean indicating whether an edge connects a target being a Task
        * Available and mandatory in all edges.
        */
        #define _IS_TASK_EDGE     "is_task_edge"
    }
}

#endif // STRUCTURES_HPP