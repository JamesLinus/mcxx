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

#include "cxx-cexpr.h"
#include "cxx-codegen.h"
#include "cxx-process.h"

#include "tl-cfg-analysis-visitor.hpp"
#include "tl-nodecl-calc.hpp"


namespace TL
{
namespace Analysis
{
    static ObjectList<Symbol> get_symbols(Nodecl::NodeclBase n)
    {
        if (n.get_symbol().is_valid())
        {
            return ObjectList<Symbol>(1, n.get_symbol());
        }
        
        ObjectList<Symbol> result;
        ObjectList<Nodecl::NodeclBase> children = n.children();
        for(ObjectList<Nodecl::NodeclBase>::iterator it = children.begin(); it != children.end(); ++it)
        {
            result.append(get_symbols(*it));
        }
        
        return result;
    }

    static Nodecl::NodeclBase compute_init_expr(Nodecl::NodeclBase n, Nodecl::NodeclBase stride, int op)
    {
        Nodecl::NodeclBase val;
        switch (op)
        {
            case 0:     val = Nodecl::Add::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 1:     val = Nodecl::Minus::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 2:     val = Nodecl::Div::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 3:     val = Nodecl::Mul::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 4:     val = Nodecl::Mod::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 5:     val = Nodecl::BitwiseAnd::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 6:     val = Nodecl::BitwiseOr::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 7:     val = Nodecl::BitwiseXor::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 8:     val = Nodecl::Shl::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            case 9:     val = Nodecl::Shr::make(n, stride, n.get_type(), n.get_filename(), n.get_line());
                        break;
            default:    internal_error("Unexpected type of operation '%d' while computing initial expression", op);
        }
        
        Nodecl::Calculator calc;
        const_value_t* const_val = calc.compute_const_value(val);
        Nodecl::NodeclBase result;
        if (const_val != NULL)
        {
            if (n.is<Nodecl::IntegerLiteral>())
            {
                result = Nodecl::IntegerLiteral::make(n.get_type(), const_val, n.get_filename(), n.get_line());
            }
            else if (n.is<Nodecl::FloatingLiteral>())
            {
                result = Nodecl::FloatingLiteral::make(n.get_type(), const_val, n.get_filename(), n.get_line());
            }
            else
            {
                internal_error("Unexpected node type '%s' while computing initial value in a constant expression", 
                               ast_print_node_type(n.get_kind()));
            }
        }
        else
        {
            result = val;
        }
        
        return result;
    }

    CfgAnalysisVisitor::CfgAnalysisVisitor(Node* n)
        : _node(n), _define(false), _actual_nodecl(Nodecl::NodeclBase::null())
    {}

    CfgAnalysisVisitor::CfgAnalysisVisitor(const CfgAnalysisVisitor& v)
        : _node(v._node), _define(v._define), _actual_nodecl(v._actual_nodecl)
    {} 

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::unhandled_node(const Nodecl::NodeclBase& n)
    {
        std::cerr << "Unhandled node during CFG Analysis'" << codegen_to_str(n.get_internal_nodecl())
                  << "' of type '" << ast_print_node_type(n.get_kind()) << "'" << std::endl;
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Symbol& n)
    {
        Nodecl::NodeclBase defined_var = n;
        
        if (_actual_nodecl.is_null())
        {   
            _node->fill_use_def_sets(n, _define);
        }
        else
        {
            defined_var = _actual_nodecl;
            _node->fill_use_def_sets(_actual_nodecl, _define);
            _actual_nodecl = Nodecl::NodeclBase::null();
        }
        
        if (!_init_expression.is_null())
        {
//             _node->set_reaching_definition(defined_var, _init_expression);
            _init_expression = Nodecl::NodeclBase::null();
        }
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::ObjectInit& n)
    {
        Symbol s = n.get_symbol();
       
        Nodecl::Symbol sym_node = Nodecl::Symbol::make(s, n.get_filename(), n.get_line());
        _node->fill_use_def_sets(sym_node, true);
       
        Nodecl::NodeclBase init = s.get_initialization();
        
        if (!init.is_null())
        {
//             _node->set_reaching_definition(sym_node, init);
            walk(init);
        }
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::ArraySubscript& n)
    {
        if (_actual_nodecl.is_null())
        {
            _actual_nodecl = n;
        }
        
        walk(n.get_subscripted());
        _define = false;        // We may come form a LHS walk and subscripts not defined!
        walk(n.get_subscripts());
    }
   
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::ClassMemberAccess& n)
    {
        if (_actual_nodecl.is_null())
        {    
            _actual_nodecl = n;
        }
        
        // walk(n.get_lhs());  // In a member access, the use/definition is always of the member, not the base
        walk(n.get_member());
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Assignment& n)
    {
        Nodecl::NodeclBase assig = n;
        walk(n.get_rhs());
        _init_expression = n.get_rhs();
        _define = true;
        walk(n.get_lhs());
        _define = false;
    }
    
    template <typename T>
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::binary_assignment(const T& n)
    {
        Nodecl::NodeclBase rhs = n.get_rhs();
        Nodecl::NodeclBase lhs = n.get_lhs();
        walk(rhs);
        walk(lhs);
        _define = true;
        if (n.template is<Nodecl::AddAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 0);
        }
        else if (n.template is<Nodecl::SubAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 1);
        }
        else if (n.template is<Nodecl::DivAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 2);
        }
        else if (n.template is<Nodecl::MulAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 3);
        }
        else if (n.template is<Nodecl::ModAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 4);
        }
        else if (n.template is<Nodecl::BitwiseAndAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 5);
        }
        else if (n.template is<Nodecl::BitwiseOrAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 6);
        }
        else if (n.template is <Nodecl::BitwiseXorAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 7);
        }
        else if (n.template is <Nodecl::ShlAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 8);
        }
        else if (n.template is <Nodecl::ShrAssignment>())
        {
            _init_expression = compute_init_expr(rhs, lhs, 9);
        }
        else
        {
            Nodecl::NodeclBase node = n;
            internal_error("Non add or sub assignment not yet implemented in CFG analysis. Founded '%s'", 
                           node.prettyprint().c_str());
        }
        walk(n.get_lhs());
        _define = false;
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::AddAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::SubAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::DivAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::MulAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::ModAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::BitwiseAndAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::BitwiseOrAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::BitwiseXorAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::ShrAssignment& n)
    {
        binary_assignment(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::ShlAssignment& n)
    {
        binary_assignment(n);
    }

    template <typename T>
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::binary_visit(const T& n)
    {
        walk(n.get_lhs());
        walk(n.get_rhs());
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Comma& n)
    {
        binary_visit(n);
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Concat& n)
    {
        binary_visit(n);
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Add& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Minus& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Mul& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Div& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Mod& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Power& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::LogicalAnd& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::LogicalOr& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::BitwiseAnd& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::BitwiseOr& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::BitwiseXor& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Shr& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Shl& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Equal& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Different& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::LowerThan& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::GreaterThan& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::LowerOrEqualThan& n)
    {
        binary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::GreaterOrEqualThan& n)
    {
        binary_visit(n);
    }
   
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Predecrement& n)
    {
        Nodecl::NodeclBase rhs = n.get_rhs();
        walk(rhs);
        _define = true;
        nodecl_t one = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed*/ 1));
        _init_expression = compute_init_expr(rhs, Nodecl::NodeclBase(one), 1);
        walk(rhs);
        _define = false;
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Postdecrement& n)
    {
        Nodecl::NodeclBase rhs = n.get_rhs();
        walk(rhs);
        _define = true;
        nodecl_t one = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed*/ 1));
        _init_expression = compute_init_expr(rhs, Nodecl::NodeclBase(one), 1);
        walk(rhs);
        _define = false;
    }

    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Preincrement& n)
    {
        Nodecl::NodeclBase rhs = n.get_rhs();
        walk(rhs);
        _define = true;
        nodecl_t one = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed*/ 1));
        _init_expression = compute_init_expr(rhs, Nodecl::NodeclBase(one), 0);
        walk(rhs);
        _define = false;
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Postincrement& n)
    {
        Nodecl::NodeclBase rhs = n.get_rhs();
        walk(rhs);
        _define = true;
        nodecl_t one = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed*/ 1));
        _init_expression = compute_init_expr(rhs, Nodecl::NodeclBase(one), 0);
        walk(rhs);
        _define = false;
    }

    template <typename T>
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::unary_visit(const T& n)
    {
        walk(n.get_rhs());
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Derreference& n)
    {
        if (_actual_nodecl.is_null())
        {    
            _actual_nodecl = n;
        }
        
        unary_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::Reference& n)
    {
        if (_actual_nodecl.is_null())
        {    
            _actual_nodecl = n;
        }
        
        unary_visit(n);
    } 
    
    template <typename T>
    void CfgAnalysisVisitor::function_visit(const T& n)
    {   // The function has its Use-def computed from a previous node.
        // We arrive here if a function call is inside other expression like "b + f(a)", "return g(b, c)", etc.
        
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::FunctionCall& n)
    {
        function_visit(n);
    }
    
    CfgAnalysisVisitor::Ret CfgAnalysisVisitor::visit(const Nodecl::VirtualFunctionCall& n)
    {
        function_visit(n);
    }
        
    
    /// *** GLOBAL VARIABLES & FUNCTION PARAMETERS VISITOR *** //
    
    CfgIPAVisitor::CfgIPAVisitor(ExtensibleGraph* cfg, ObjectList<var_usage_t*> glob_vars, ObjectList<Symbol> reference_params)
        : _cfg(cfg), _global_vars(glob_vars), _ref_params(), _usage(), _defining(false)
    {}
    
    void CfgIPAVisitor::compute_usage_rec(Node* node)
    {
        if (!node->is_visited())
        {
            node->set_visited(true);
            
            Node_type ntype = node->get_type();
            if (ntype == GRAPH_NODE)
            {
                compute_usage_rec(node->get_graph_entry_node());
            }
            else if (node->has_key(_NODE_STMTS))
            {
                ObjectList<Nodecl::NodeclBase> stmts = node->get_statements();
                for (ObjectList<Nodecl::NodeclBase>::iterator it = stmts.begin(); it != stmts.end(); ++it)
                {
                    walk(*it);
                }
            }
            
            ObjectList<Node*> children = node->get_children();
            for (ObjectList<Node*>::iterator it = children.begin(); it != children.end(); ++it)
            {
                compute_usage_rec(*it);
            }
        }
    }

    void CfgIPAVisitor::compute_usage()
    {
        Node* graph_node = _cfg->get_graph();
        
        compute_usage_rec(graph_node);
        
        ExtensibleGraph::clear_visits(graph_node);
    }
   
    
    struct var_usage_t* CfgIPAVisitor::get_var_in_list(Nodecl::Symbol n, ObjectList<struct var_usage_t*> list)
    {
        for (ObjectList<struct var_usage_t*>::iterator it = list.begin(); it != list.end(); ++it)
        {
            if (Nodecl::Utils::equal_nodecls((*it)->get_nodecl(), n))
            {
                return *it;
            }
        }
        
        internal_error("No symbol '%s' founded in global variable list", n.get_symbol().get_name().c_str());
    }
    
    ObjectList<struct var_usage_t*> CfgIPAVisitor::get_usage() const
    {
        return _usage;
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::Symbol& n)
    {
        if (_ref_params.contains(n.get_symbol()) || TL::Analysis::usage_list_contains_sym(n, _global_vars))
            if (usage_list_contains_sym(n, _global_vars))
            {
                struct var_usage_t* global_var = get_var_in_list(n, _global_vars);
                char usage = global_var->get_usage();
                if (usage == '0' || usage == '2')
                {   // nothing to do: It doesn't matters what happens with an already Killed variable
                }
                else if (usage == '1')
                {
                    if (_defining)
                    {   // Set to 2 the usage value
                        global_var->set_usage('2');
                    }
                    else {} // nothing to do, the variable was already used
                }
            }
            else
            {
                char usage;
                if (_defining) usage = '0';
                else usage = '1';
                
                struct var_usage_t* new_global_var_usage = new var_usage_t(n, usage);
                _global_vars.insert(new_global_var_usage);
            }
    }
    
    template <typename T>
    void CfgIPAVisitor::op_assignment_visit(const T& n)
    {
        walk(n.get_lhs());
        _defining = true;
        walk(n.get_lhs());
        _defining = false;
        walk(n.get_rhs());        
    }

    template <typename T>
    void CfgIPAVisitor::unary_visit(const T& n)
    {
        walk(n.get_rhs());
        _defining = true;
        walk(n.get_rhs());
        _defining = false;        
    }

    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::Assignment& n)
    {
        _defining = true;
        walk(n.get_lhs());
        _defining = false;
        walk(n.get_rhs());  
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::AddAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::SubAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::DivAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::MulAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::ModAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::BitwiseAndAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::BitwiseOrAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::BitwiseXorAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::ShrAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::ShlAssignment& n)
    {
        op_assignment_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::Predecrement& n)
    {
        unary_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::Postdecrement& n)
    {
        unary_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::Preincrement& n)
    {
        unary_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::Postincrement& n)
    {
        unary_visit(n);
    }
    
    template <typename T>
    void CfgIPAVisitor::function_visit(const T& n)
    {
        // 
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::FunctionCall& n)
    {
        function_visit(n);
    }
    
    CfgIPAVisitor::Ret CfgIPAVisitor::visit(const Nodecl::VirtualFunctionCall& n)
    {
        function_visit(n);
    }
}
}
