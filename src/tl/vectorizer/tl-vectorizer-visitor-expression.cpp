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

#include "tl-vectorizer-visitor-expression.hpp"
#include "tl-vectorizer-utils.hpp"
#include "tl-nodecl-utils.hpp"

namespace TL
{
    namespace Vectorization
    {
        VectorizerVisitorExpression::VectorizerVisitorExpression(
                VectorizerEnvironment& environment) :
            _environment(environment)
        {
        }

        bool VectorizerVisitorExpression::process_fmul_op(const Nodecl::NodeclBase&  n)
        {
            Nodecl::Add binary_op = n.as<Nodecl::Add>();

            Nodecl::NodeclBase lhs = binary_op.get_lhs();
            Nodecl::NodeclBase rhs = binary_op.get_rhs();
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            if (lhs.is<Nodecl::Mul>())
            {
                /*
                if (n.is<Nodecl::Add>())
                {
                    Nodecl::Mul mul = lhs.as<Nodecl::Mul>();

                    walk(rhs);
                    walk(mul.get_lhs());
                    walk(mul.get_rhs());

                    Nodecl::VectorFmadd fmadd =
                        Nodecl::VectorFmadd::make(mul.get_lhs().shallow_copy(),
                                mul.get_rhs().shallow_copy(),
                                rhs.shallow_copy(),
                                mask,
                                n.get_type(),
                                n.get_locus());
                
                    std::cerr << "FMADD detected" << std::endl;

                    n.replace(fmadd);

                    return true;
                }
                else if (n.is<Nodecl::Minus>())
                {
                    // TODO
                }
                */
            }
            else if (rhs.is<Nodecl::Mul>())
            {
                if (n.is<Nodecl::Add>())
                {
                    /*
                    Nodecl::Mul mul = rhs.as<Nodecl::Mul>();

                    walk(lhs);
                    walk(mul.get_lhs());
                    walk(mul.get_rhs());

                    Nodecl::VectorFmadd fmadd =
                        Nodecl::VectorFmadd::make(mul.get_lhs().shallow_copy(),
                                mul.get_rhs().shallow_copy(),
                                lhs.shallow_copy(),
                                mask,
                                n.get_type(),
                                n.get_locus());

                    std::cerr << "FMADD detected" << std::endl;

                    n.replace(fmadd);

                    return true;
                    */
                }
                else if (n.is<Nodecl::Minus>())
                {
                    // TODO
                }
            }

            return false;
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Add& n)
        {
            if(!process_fmul_op(n))
            {
                Nodecl::NodeclBase mask = Utils::get_proper_mask(
                        _environment._mask_list.back());

                walk(n.get_lhs());
                walk(n.get_rhs());

                const Nodecl::VectorAdd vector_add =
                    Nodecl::VectorAdd::make(
                            n.get_lhs().shallow_copy(),
                            n.get_rhs().shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(n.get_type(), 
                                _environment._unroll_factor),
                            n.get_locus());

                n.replace(vector_add);
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Minus& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            if(!process_fmul_op(n))
            {
                walk(n.get_lhs());
                walk(n.get_rhs());

                const Nodecl::VectorMinus vector_minus =
                    Nodecl::VectorMinus::make(
                            n.get_lhs().shallow_copy(),
                            n.get_rhs().shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(n.get_type(), 
                                _environment._unroll_factor),
                            n.get_locus());

                n.replace(vector_minus);
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Mul& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorMul vector_mul =
                Nodecl::VectorMul::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_mul);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Div& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            Nodecl::NodeclBase lhs = n.get_lhs();

            walk(n.get_lhs());
            walk(n.get_rhs());


            const Nodecl::VectorDiv vector_div =
                Nodecl::VectorDiv::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_div);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Mod& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());


            const Nodecl::VectorMod vector_mod =
                Nodecl::VectorMod::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_mod);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Neg& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            Nodecl::NodeclBase rhs = n.get_rhs();

            if (rhs.is<Nodecl::IntegerLiteral>() || // -1
                rhs.is<Nodecl::FloatingLiteral>())
            {
                Nodecl::VectorPromotion vector_prom =
                    Nodecl::VectorPromotion::make(
                            n.shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(n.get_type(), 
                                _environment._unroll_factor),
                            n.get_locus());

                vector_prom.set_constant(const_value_make_vector_from_scalar(
                            _environment._unroll_factor,
                            n.get_constant()));

                n.replace(vector_prom);
            }
            else // -a
            {
                walk(rhs);

                Nodecl::VectorNeg vector_neg =
                    Nodecl::VectorNeg::make(
                            n.get_rhs().shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(n.get_type(), 
                                _environment._unroll_factor),
                            n.get_locus());

                if(n.is_constant())
                    vector_neg.set_constant(const_value_make_vector_from_scalar(
                                _environment._unroll_factor,
                                n.get_constant()));

                n.replace(vector_neg);
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::LowerThan& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorLowerThan vector_lt =
                Nodecl::VectorLowerThan::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        TL::Type::get_mask_type(_environment._unroll_factor), 
                        n.get_locus());

            n.replace(vector_lt);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::LowerOrEqualThan& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorLowerOrEqualThan vector_lt =
                Nodecl::VectorLowerOrEqualThan::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        TL::Type::get_mask_type(_environment._unroll_factor), 
                        n.get_locus());

            n.replace(vector_lt);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::GreaterThan& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorGreaterThan vector_gt =
                Nodecl::VectorGreaterThan::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        TL::Type::get_mask_type(_environment._unroll_factor), 
                        n.get_locus());

            n.replace(vector_gt);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::GreaterOrEqualThan& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorGreaterOrEqualThan vector_gt =
                Nodecl::VectorGreaterOrEqualThan::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        TL::Type::get_mask_type(_environment._unroll_factor), 
                        n.get_locus());

            n.replace(vector_gt);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Equal& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorEqual vector_eq =
                Nodecl::VectorEqual::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        TL::Type::get_mask_type(_environment._unroll_factor), 
                        n.get_locus());

            n.replace(vector_eq);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Different& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorDifferent vector_dif =
                Nodecl::VectorDifferent::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        TL::Type::get_mask_type(_environment._unroll_factor), 
                        n.get_locus());

            n.replace(vector_dif);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::BitwiseAnd& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorBitwiseAnd vector_ba =
                Nodecl::VectorBitwiseAnd::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_ba);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::BitwiseOr& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorBitwiseOr vector_bo =
                Nodecl::VectorBitwiseOr::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_bo);
        }


        void VectorizerVisitorExpression::visit(const Nodecl::LogicalAnd& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorLogicalAnd vector_lo =
                Nodecl::VectorLogicalAnd::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_lo);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::LogicalOr& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_lhs());
            walk(n.get_rhs());

            const Nodecl::VectorLogicalOr vector_lo =
                Nodecl::VectorLogicalOr::make(
                        n.get_lhs().shallow_copy(),
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_lo);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::ConditionalExpression& n)
        {
            Nodecl::NodeclBase condition = n.get_condition();

            walk(condition);

            Nodecl::NodeclBase prev_mask =
                _environment._mask_list.back();

            if(Utils::is_all_one_mask(prev_mask))
            {
                _environment._mask_list.push_back(condition);
                _environment._local_scope_list.push_back(n.get_true().retrieve_context());
                walk(n.get_true());
                _environment._mask_list.pop_back();
                _environment._local_scope_list.pop_back();

                Nodecl::VectorMaskNot neg_condition =
                    Nodecl::VectorMaskNot::make(condition,
                            condition.get_type(),
                            condition.get_locus());

                _environment._mask_list.push_back(neg_condition);
                _environment._local_scope_list.push_back(n.get_false().retrieve_context());
                walk(n.get_false());
                _environment._local_scope_list.pop_back();
                _environment._mask_list.pop_back();
            }
            else
            {
                TL::Scope scope = n.retrieve_context();
                // True Mask
                TL::Symbol true_mask_sym = scope.new_symbol("__mask_" + 
                        Utils::get_var_counter());
                true_mask_sym.get_internal_symbol()->kind = SK_VARIABLE;
                true_mask_sym.get_internal_symbol()->entity_specs.is_user_declared = 1;
                true_mask_sym.set_type(TL::Type::get_mask_type(_environment._mask_size));

                Nodecl::Symbol true_mask_nodecl_sym = true_mask_sym.make_nodecl(true, n.get_locus());
                Nodecl::NodeclBase true_mask_value = 
                    Nodecl::VectorMaskAnd::make(
                            prev_mask.shallow_copy(),
                            condition.shallow_copy(),
                            true_mask_sym.get_type(),
                            n.get_locus());

                Nodecl::ExpressionStatement true_mask_exp =
                    Nodecl::ExpressionStatement::make(
                            Nodecl::VectorMaskAssignment::make(true_mask_nodecl_sym, 
                                true_mask_value,
                                true_mask_sym.get_type(),
                                n.get_locus()));

                // Visit True
                _environment._mask_list.push_back(true_mask_nodecl_sym.shallow_copy());
                _environment._local_scope_list.push_back(n.get_true().retrieve_context());
                walk(n.get_true());
                _environment._local_scope_list.pop_back();
                _environment._mask_list.pop_back();

                n.prepend_sibling(true_mask_exp);

                    
                // False Mask
                TL::Symbol false_mask_sym = scope.new_symbol("__mask_" + 
                        Utils::get_var_counter());
                false_mask_sym.get_internal_symbol()->kind = SK_VARIABLE;
                false_mask_sym.get_internal_symbol()->entity_specs.is_user_declared = 1;
                false_mask_sym.set_type(TL::Type::get_mask_type(_environment._mask_size));

                Nodecl::Symbol false_mask_nodecl_sym = false_mask_sym.make_nodecl(true, n.get_locus());

                Nodecl::NodeclBase false_mask_value =
                    Nodecl::VectorMaskAnd2Not::make(
                            prev_mask.shallow_copy(),
                            condition.shallow_copy(),
                            false_mask_sym.get_type(),
                            n.get_locus());

                Nodecl::ExpressionStatement else_mask_exp =
                    Nodecl::ExpressionStatement::make(
                            Nodecl::VectorMaskAssignment::make(false_mask_nodecl_sym.shallow_copy(), 
                                false_mask_value,
                                false_mask_sym.get_type(),
                                n.get_locus()));

                // Visit False
                _environment._mask_list.push_back(false_mask_nodecl_sym);
                _environment._local_scope_list.push_back(n.get_false().retrieve_context());
                walk(n.get_false());
                _environment._local_scope_list.pop_back();
                _environment._mask_list.pop_back();

                n.prepend_sibling(else_mask_exp);
            }

            const Nodecl::VectorConditionalExpression vector_cond =
                Nodecl::VectorConditionalExpression::make(
                        condition.shallow_copy(),
                        n.get_true().shallow_copy(),
                        n.get_false().shallow_copy(),
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(vector_cond);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Assignment& n)
        {
            TL::Type assignment_type = n.get_type().no_ref();
            Nodecl::NodeclBase lhs = n.get_lhs();
            Nodecl::NodeclBase rhs = n.get_rhs();
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            // Computing new vector type
            TL::Type vector_type = Utils::get_qualified_vector_to(assignment_type, 
                    _environment._unroll_factor);

            // IV vectorization: i = i + 3 --> i = i + (unroll_factor * 3)
            if(Vectorizer::_analysis_info->is_non_reduction_basic_induction_variable(_environment._analysis_simd_scope,
                        lhs))
            {
                std::cerr << "Vectorizing IV update: " << lhs.prettyprint() << std::endl;

                Nodecl::NodeclBase step = Vectorizer::_analysis_info->get_induction_variable_increment(
                        _environment._analysis_simd_scope, lhs);

                    //Vectorizer::_analysis_info->get_induction_variable_increment(
                    //    _environment._analysis_simd_scope, lhs);

                ObjectList<Nodecl::NodeclBase> step_list = 
                    Nodecl::Utils::get_all_nodecl_occurrences(step, lhs);

                for(ObjectList<Nodecl::NodeclBase>::iterator it = step_list.begin();
                        it != step_list.end();
                        it ++)
                {
                    Nodecl::Mul new_step =
                        Nodecl::Mul::make(
                                it->shallow_copy(),
                                Nodecl::IntegerLiteral::make(
                                    TL::Type::get_int_type(),
                                    const_value_get_signed_int(_environment._unroll_factor),
                                    it->get_locus()),
                                TL::Type::get_int_type(),
                                it->get_locus());

                    it->replace(new_step);
                }
            }
            else if(lhs.is<Nodecl::ArraySubscript>())
            {
                walk(rhs);

                // Vector Store
                // Constant ArraySubscript, nothing to do
                if (Vectorizer::_analysis_info->is_constant_access(
                            _environment._analysis_simd_scope,
                            lhs))
                {
                    std::cerr << "Constant store: " << lhs.prettyprint() << std::endl;
                    running_error("Vectorizer: Extract operation is not supported yet (%s).",
                            lhs.prettyprint().c_str());
 
                }
                // ArraySubscript indexed by nested IV, nothing to do
                else if (Vectorization::Utils::is_nested_induction_variable_dependent_access(_environment, lhs))
                {
                    std::cerr << "Nested IV dependent store: " << lhs.prettyprint() << std::endl;
                    running_error("Vectorizer: Extract operation is not supported yet (%s).",
                            lhs.prettyprint().c_str());
                }
                else if(Vectorizer::_analysis_info->is_adjacent_access(
                            _environment._analysis_simd_scope,
                            lhs))
                {
                    TL::Type basic_type = lhs.get_type();
                    if (basic_type.is_lvalue_reference())
                    {
                        basic_type = basic_type.references_to();
                    }

                    // Aligned
                    if(Vectorizer::_analysis_info->is_simd_aligned_access(
                            _environment._analysis_simd_scope,
                            lhs,
                            _environment._suitable_expr_list,
                            _environment._unroll_factor,
                            _environment._unroll_factor * assignment_type.get_size()))
                    {
                        DEBUG_CODE()
                        {
                            fprintf(stderr, "VECTORIZER: Store access '%s' is ALIGNED\n",
                                lhs.prettyprint().c_str());
                        }

                        const Nodecl::VectorStore vector_store =
                            Nodecl::VectorStore::make(
                                    Nodecl::Reference::make(
                                        lhs.shallow_copy(),
                                        basic_type.get_pointer_to(),
                                        n.get_locus()),
                                    rhs.shallow_copy(),
                                    mask,
                                    vector_type,
                                    n.get_locus());

                        n.replace(vector_store);
                    }
                    else // Unaligned
                    {
                        //DEBUG_CODE()
                        {
                            fprintf(stderr, "VECTORIZER: Store access '%s' is UNALIGNED\n",
                                    lhs.prettyprint().c_str());
                        }

                        if (_environment._prefer_gather_scatter) // Unaligned Load or Scatter
                        {
                            const Nodecl::ArraySubscript lhs_array = lhs.as<Nodecl::ArraySubscript>();

                            const Nodecl::NodeclBase base = lhs_array.get_subscripted();
                            const Nodecl::List subscripts = lhs_array.get_subscripts().as<Nodecl::List>();

                            Nodecl::NodeclBase strides = Nodecl::Utils::linearize_array_subscript(lhs.as<Nodecl::ArraySubscript>());

                            walk(strides);

                            const Nodecl::VectorScatter vector_scatter =
                                Nodecl::VectorScatter::make(
                                        base.shallow_copy(),
                                        strides,
                                        rhs.shallow_copy(),
                                        mask,
                                        vector_type,
                                        n.get_locus());

                            n.replace(vector_scatter);
                        }
                        else
                        {
                            const Nodecl::UnalignedVectorStore vector_store =
                                Nodecl::UnalignedVectorStore::make(
                                        Nodecl::Reference::make(
                                            lhs.shallow_copy(),
                                            basic_type.get_pointer_to(),
                                            n.get_locus()),
                                        rhs.shallow_copy(),
                                        mask,
                                        vector_type,
                                        n.get_locus());

                            n.replace(vector_store);
 
                        }
                    } 
                }
                else // Vector Scatter
                {
                    const Nodecl::ArraySubscript lhs_array = lhs.as<Nodecl::ArraySubscript>();

                    const Nodecl::NodeclBase base = lhs_array.get_subscripted();
                    const Nodecl::List subscripts = lhs_array.get_subscripts().as<Nodecl::List>();

                    std::cerr << "Scatter: " << lhs_array.prettyprint() << std::endl;

                    Nodecl::NodeclBase strides = Nodecl::Utils::linearize_array_subscript(lhs.as<Nodecl::ArraySubscript>());

                    //std::cerr << "Stride: " << strides.prettyprint() << std::endl;

                    walk(strides);

                    const Nodecl::VectorScatter vector_scatter =
                        Nodecl::VectorScatter::make(
                                base.shallow_copy(),
                                strides,
                                rhs.shallow_copy(),
                                mask,
                                vector_type,
                                n.get_locus());

                    n.replace(vector_scatter);
                }
            }
            else // Register
            {
                walk(lhs);
                walk(rhs);

                const Nodecl::VectorAssignment vector_assignment =
                    Nodecl::VectorAssignment::make(
                            lhs.shallow_copy(),
                            rhs.shallow_copy(),
                            mask,
                            vector_type,
                            n.get_locus());

                n.replace(vector_assignment);
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::AddAssignment& n)
        {
            const Nodecl::Assignment assignment =
                Nodecl::Assignment::make(
                        n.get_lhs().shallow_copy(),
                        Nodecl::Add::make(
                            n.get_lhs().shallow_copy(),
                            n.get_rhs().shallow_copy(),
                            n.get_type(),
                            n.get_locus()),
                        n.get_type(),
                        n.get_locus());

            n.replace(assignment);

            // Visit standard assignment
            walk(n);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::MinusAssignment& n)
        {
            const Nodecl::Assignment assignment =
                Nodecl::Assignment::make(
                        n.get_lhs().shallow_copy(),
                        Nodecl::Minus::make(
                            n.get_lhs().shallow_copy(),
                            n.get_rhs().shallow_copy(),
                            n.get_type(),
                            n.get_locus()),
                        n.get_type(),
                        n.get_locus());

            n.replace(assignment);

            // Visit standard assignment
            walk(n);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::MulAssignment& n)
        {
            const Nodecl::Assignment assignment =
                Nodecl::Assignment::make(
                        n.get_lhs().shallow_copy(),
                        Nodecl::Mul::make(
                            n.get_lhs().shallow_copy(),
                            n.get_rhs().shallow_copy(),
                            n.get_type(),
                            n.get_locus()),
                        n.get_type(),
                        n.get_locus());

            n.replace(assignment);

            // Visit standard assignment
            walk(n);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::DivAssignment& n)
        {
            const Nodecl::Assignment assignment =
                Nodecl::Assignment::make(
                        n.get_lhs().shallow_copy(),
                        Nodecl::Div::make(
                            n.get_lhs().shallow_copy(),
                            n.get_rhs().shallow_copy(),
                            n.get_type(),
                            n.get_locus()),
                        n.get_type(),
                        n.get_locus());

            n.replace(assignment);

            // Visit standard assignment
            walk(n);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::ModAssignment& n)
        {
            const Nodecl::Assignment assignment =
                Nodecl::Assignment::make(
                        n.get_lhs().shallow_copy(),
                        Nodecl::Mod::make(
                            n.get_lhs().shallow_copy(),
                            n.get_rhs().shallow_copy(),
                            n.get_type(),
                            n.get_locus()),
                        n.get_type(),
                        n.get_locus());

            n.replace(assignment);

            // Visit standard assignment
            walk(n);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Conversion& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_nest());

            // If false, someone (TL::Symbol) moves/replaces my tree.
            // Therefore do nothing, I'm no longer a Conversion!!
            if (n.is<Nodecl::Conversion>())
            {
                const Nodecl::VectorConversion vector_conv =
                    Nodecl::VectorConversion::make(
                            n.get_nest().shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(n.get_type(), _environment._unroll_factor),
                            n.get_locus());

                n.replace(vector_conv);
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Cast& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            walk(n.get_rhs());

            const Nodecl::VectorConversion vector_conv =
                Nodecl::VectorConversion::make(
                        n.get_rhs().shallow_copy(),
                        mask,
                        Utils::get_qualified_vector_to(n.get_type(), _environment._unroll_factor),
                        n.get_locus());
/*            
            printf("Casting %s %s\n",
                    Utils::get_qualified_vector_to(n.get_type(), _environment._unroll_factor).get_simple_declaration(n.retrieve_context(), "").c_str(),
                    n.get_rhs().get_type().get_simple_declaration(n.retrieve_context(), "").c_str());
*/
            n.replace(vector_conv);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::ArraySubscript& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            TL::Type basic_type = n.get_type().no_ref();

            const_value_t* const_value = NULL;
            if (n.is_constant())
            {
                std::cerr << "TODO: CONSTANT ARRAYSUBSCRIPT: " << n.prettyprint() << "\n";
            }


            // Computing new vector type
            TL::Type vector_type = Utils::get_qualified_vector_to(basic_type, 
                    _environment._unroll_factor);

            // Vector Promotion from constant ArraySubscript
            if (Vectorizer::_analysis_info->is_constant_access(
                        _environment._analysis_simd_scope,
                        n))
            {
//                std::cerr << "Constant load: " << n.prettyprint() << "\n";

                Nodecl::VectorPromotion vector_prom =
                    Nodecl::VectorPromotion::make(
                            n.shallow_copy(),
                            mask,
                            vector_type,
                            n.get_locus());

                vector_prom.set_constant(const_value);

                n.replace(vector_prom);
            }
            // Vector promotion from ArraySubscript indexed by nested IV
            else if (Vectorization::Utils::is_nested_induction_variable_dependent_access(_environment, n) &&
                    !Vectorizer::_analysis_info->is_induction_variable_dependent_access(_environment._analysis_simd_scope,
                        n))
            {
//                std::cerr << "Nested IV dependent load: " << n.prettyprint() << "\n";

                Nodecl::VectorPromotion vector_prom =
                    Nodecl::VectorPromotion::make(
                            n.shallow_copy(),
                            mask,
                            vector_type,
                            n.get_locus());

                vector_prom.set_constant(const_value);

                n.replace(vector_prom);
            }
            // Vector Load
            else if (Vectorizer::_analysis_info->is_adjacent_access(
                        _environment._analysis_simd_scope,
                        n))
            {
                // Aligned
                if(Vectorizer::_analysis_info->is_simd_aligned_access(
                            _environment._analysis_simd_scope,
                            n,
                            _environment._suitable_expr_list,
                            _environment._unroll_factor,
                            _environment._vector_length))
                {
                    DEBUG_CODE()
                    {
                        fprintf(stderr, "VECTORIZER: Load access '%s' is ALIGNED\n",
                            n.prettyprint().c_str());
                    }

                    Nodecl::VectorLoad vector_load =
                        Nodecl::VectorLoad::make(
                                Nodecl::Reference::make(
                                    n.shallow_copy(),
                                    basic_type.get_pointer_to(),
                                    n.get_locus()),
                                mask,
                                vector_type,
                                n.get_locus());

                    vector_load.set_constant(const_value);

                    n.replace(vector_load);
                }
                else // Unaligned
                {
                    //DEBUG_CODE()
                    {
                        fprintf(stderr, "VECTORIZER: Load access '%s' is UNALIGNED\n",
                            n.prettyprint().c_str());
                    }

                    if(_environment._prefer_gather_scatter) // Unaligned Load or Gather
                    {
                        const Nodecl::NodeclBase base = n.get_subscripted();
                        const Nodecl::List subscripts = n.get_subscripts().as<Nodecl::List>();

                        Nodecl::NodeclBase strides = Nodecl::Utils::linearize_array_subscript(n);

                        walk(strides);

                        Nodecl::VectorGather vector_gather =
                            Nodecl::VectorGather::make(
                                    base.shallow_copy(),
                                    strides,
                                    mask,
                                    vector_type,
                                    n.get_locus());

                        vector_gather.set_constant(const_value);

                        n.replace(vector_gather);
                    }
                    else
                    {
                        Nodecl::UnalignedVectorLoad vector_load =
                            Nodecl::UnalignedVectorLoad::make(
                                    Nodecl::Reference::make(
                                        n.shallow_copy(),
                                        basic_type.get_pointer_to(),
                                        n.get_locus()),
                                    mask,
                                    vector_type,
                                    n.get_locus());

                        vector_load.set_constant(const_value);

                        n.replace(vector_load);
                    }
                } 
            }
            else // Vector Gather
            {
                const Nodecl::NodeclBase base = n.get_subscripted();
                const Nodecl::List subscripts = n.get_subscripts().as<Nodecl::List>();

                std::cerr << "Gather: " << n.prettyprint() << "\n";

                Nodecl::NodeclBase strides = Nodecl::Utils::linearize_array_subscript(n);

                //std::cerr << "Stride: " << strides.prettyprint() << std::endl;

                walk(strides);

                Nodecl::VectorGather vector_gather =
                    Nodecl::VectorGather::make(
                            base.shallow_copy(),
                            strides,
                            mask,
                            vector_type,
                            n.get_locus());

                vector_gather.set_constant(const_value);

                n.replace(vector_gather);
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::FunctionCall& n)
        {
            Nodecl::NodeclBase mask = Utils::get_proper_mask(
                    _environment._mask_list.back());

            Nodecl::NodeclBase called = n.get_called();
            ERROR_CONDITION(!called.is<Nodecl::Symbol>(),
                    "Vectorizer: %s found. This kind of function call is not supported yet",
                    ast_print_node_type(called.get_kind()));

            Nodecl::Symbol called_sym = called.as<Nodecl::Symbol>();
            TL::Type call_type = n.get_type();

            // Vectorizing arguments
            walk(n.get_arguments());

            if (called_sym.get_symbol().get_name() == "fabsf" ||
                    called_sym.get_symbol().get_name() == "fabs")
            {
                const Nodecl::VectorFabs vector_fabs_call =
                    Nodecl::VectorFabs::make(
                            n.get_arguments().as<Nodecl::List>().front().shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(call_type, _environment._unroll_factor),
                            n.get_locus());

                n.replace(vector_fabs_call);
            }
            else if (called_sym.get_symbol().get_name() == "sincosf")
            {
                Nodecl::List::iterator args = n.get_arguments().as<Nodecl::List>().begin();

                Nodecl::NodeclBase source = *args;
                args++;
                Nodecl::NodeclBase sin_p = *args;
                args++;
                Nodecl::NodeclBase cos_p = *args;
                args++;

                const Nodecl::VectorSincos vector_sincos_call =
                    Nodecl::VectorSincos::make(
                            source.shallow_copy(),
                            sin_p.shallow_copy(),
                            cos_p.shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(call_type, _environment._unroll_factor),
                            n.get_locus());

                n.replace(vector_sincos_call);
            }
            else //Common functions
            {
                TL::Type function_target_type = _environment._target_type;

                // If _target_type and call_type have the same size, we use call_type as
                // this function should have been registered with this type
                if (_environment._target_type.get_size() == call_type.get_size())
                {
                    function_target_type = call_type;
                }

                // Get the best vector version of the function available
                Nodecl::NodeclBase best_version =
                    TL::Vectorization::Vectorizer::_function_versioning.get_best_version(
                            called_sym.get_symbol().get_name(), 
                            _environment._device, 
                            _environment._unroll_factor * call_type.get_size(), 
                            function_target_type,
                            !mask.is_null());

                ERROR_CONDITION(best_version.is_null(), "Vectorizer: the best vector function for '%s' is null",
                        called_sym.get_symbol().get_name().c_str());

                // Create new called symbol
                Nodecl::Symbol new_called;
                if (best_version.is<Nodecl::FunctionCode>())
                {
                    new_called = best_version.as<Nodecl::FunctionCode>().get_symbol().
                        make_nodecl(true, n.get_locus());
                }
                else if (best_version.is<Nodecl::Symbol>())
                {
                    new_called = best_version.as<Nodecl::Symbol>().get_symbol().
                        make_nodecl(true, n.get_locus());
                }
                else
                {
                    running_error("Vectorizer: %s found as vector function version in function versioning.",
                            ast_print_node_type(best_version.get_kind()));
                }

                const Nodecl::VectorFunctionCall vector_function_call =
                    Nodecl::VectorFunctionCall::make(
                            Nodecl::FunctionCall::make(
                                new_called,
                                n.get_arguments().shallow_copy(),
                                n.get_alternate_name().shallow_copy(),
                                n.get_function_form().shallow_copy(),
                                Utils::get_qualified_vector_to(call_type, _environment._unroll_factor),
                                n.get_locus()),
                            called_sym.shallow_copy(),
                            mask,
                            Utils::get_qualified_vector_to(call_type, _environment._unroll_factor),
                            n.get_locus());

                n.replace(vector_function_call);
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Symbol& n)
        {
            TL::Type sym_type = n.get_type().no_ref();
            TL::Symbol tl_sym = n.get_symbol();
            TL::Type tl_sym_type = tl_sym.get_type().no_ref();

            //std::cerr << "scalar_type: " << n.prettyprint() << std::endl;

            if (!sym_type.is_vector())
            {
                // Vectorize BASIC induction variable
                if (Vectorizer::_analysis_info->is_non_reduction_basic_induction_variable(
                            _environment._analysis_simd_scope,
                            n)) 
                {
                    vectorize_basic_induction_variable(n);
                }
                // Vectorize NESTED IV
                else if (Vectorization::Utils::is_nested_non_reduction_basic_induction_variable(
                            _environment,
                            n)) 
                {
                    DEBUG_CODE()
                    {
                        fprintf(stderr,"VECTORIZER: '%s' is NESTED IV and will be PROMOTED to vector\n", 
                                n.prettyprint().c_str()); 
                    }

                    const Nodecl::VectorPromotion vector_prom =
                        Nodecl::VectorPromotion::make(
                                n.shallow_copy(),
                                Utils::get_null_mask(),
                                Utils::get_qualified_vector_to(sym_type, 
                                    _environment._unroll_factor),
                                n.get_locus());

                    n.replace(vector_prom);
                }
                // Vectorize symbols declared in the SIMD scope
                else if (Utils::is_declared_in_inner_scope(
                            _environment._local_scope_list.front().get_decl_context().current_scope,
                            _environment._local_scope_list.back().get_decl_context().current_scope,
                            n.get_symbol().get_scope().get_decl_context().current_scope))
                {
                    DEBUG_CODE()
                    {
                        fprintf(stderr,"VECTORIZER: '%s' is DECLARED INSIDE OF THE SIMD SCOPE. Its TYPE will be vectorized\n", 
                                n.prettyprint().c_str()); 
                    }

                    //std::cerr << "NS scalar_type: " << n.prettyprint() << std::endl;

                    //TL::Symbol
                    if (tl_sym_type.is_scalar_type())
                    {
                        //std::cerr << "TS scalar_type: " << n.prettyprint() << std::endl;
                        tl_sym.set_type(Utils::get_qualified_vector_to(tl_sym_type, 
                                    _environment._unroll_factor));
                        tl_sym_type = tl_sym.get_type();
                    }

                    //Nodecl::Symbol
                    Nodecl::Symbol new_sym =
                        Nodecl::Symbol::make(tl_sym,
                                n.get_locus());

                    new_sym.set_type(tl_sym_type.get_lvalue_reference_to());

                    n.replace(new_sym);
                }
                // Non local Nodecl::Symbol with scalar type whose TL::Symbol has vector_type
                else if(tl_sym_type.is_vector())
                {
                    DEBUG_CODE()
                    {
                        fprintf(stderr,"VECTORIZER: '%s' is a scalar NON-LOCAL Nodecl::Symbol whose"\
                               "TL::Symbol has vector type. Its Nodecl TYPE will be vectorized\n", 
                                n.prettyprint().c_str()); 
                    }

                    //Nodecl::Symbol
                    Nodecl::Symbol new_sym =
                        Nodecl::Symbol::make(tl_sym,
                                n.get_locus());

                    new_sym.set_type(tl_sym_type.get_lvalue_reference_to());

                    n.replace(new_sym);
                }
                // Vectorize constants
                else if (Vectorizer::_analysis_info->is_constant(
                            _environment._analysis_simd_scope,
                            n))
                {
                    DEBUG_CODE()
                    {
                        fprintf(stderr,"VECTORIZER: '%s' is CONSTANT and will be PROMOTED to vector\n", 
                                n.prettyprint().c_str()); 
                    }

                    Nodecl::VectorPromotion vector_prom =
                        Nodecl::VectorPromotion::make(
                                n.shallow_copy(),
                                Utils::get_null_mask(),
                                Utils::get_qualified_vector_to(sym_type, 
                                    _environment._unroll_factor),
                                n.get_locus());

                    if(n.is_constant())
                        vector_prom.set_constant(const_value_make_vector_from_scalar(
                                    _environment._unroll_factor,
                                    n.get_constant()));

                    n.replace(vector_prom);
                }
                else if(_environment._reduction_list != NULL)
                {
                    if(_environment._reduction_list->contains(tl_sym))
                    {
                        // If symbol is in the map
                        Nodecl::Symbol new_red_symbol;

                        std::map<TL::Symbol, TL::Symbol>::iterator it =
                            _environment._new_external_vector_symbol_map->find(tl_sym);

                        new_red_symbol = it->second.make_nodecl(true, n.get_locus());

                        n.replace(new_red_symbol);
                    }
                }
                else
                {
                    //TODO: If you are from outside of the loop -> Vector local copy.
                    running_error("Vectorizer: Loop is not vectorizable. '%s' is not IV, Constant, Local, Reduction or LastPrivate.",
                            n.get_symbol().get_name().c_str());
                }
            }
        }

        void VectorizerVisitorExpression::visit(const Nodecl::IntegerLiteral& n)
        {
            Nodecl::VectorPromotion vector_prom =
                Nodecl::VectorPromotion::make(
                        n.shallow_copy(),
                        Utils::get_null_mask(),
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            vector_prom.set_constant(const_value_make_vector_from_scalar(
                        _environment._unroll_factor,
                        n.get_constant()));


            n.replace(vector_prom);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::FloatingLiteral& n)
        {
            Nodecl::VectorPromotion vector_prom =
                Nodecl::VectorPromotion::make(
                        n.shallow_copy(),
                        Utils::get_null_mask(),
                        Utils::get_qualified_vector_to(n.get_type(), 
                            _environment._unroll_factor),
                        n.get_locus());

            vector_prom.set_constant(const_value_make_vector_from_scalar(
                        _environment._unroll_factor,
                        n.get_constant()));

            n.replace(vector_prom);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::ParenthesizedExpression& n)
        {
            walk(n.get_nest());

            Nodecl::ParenthesizedExpression parent = n.shallow_copy().as<Nodecl::ParenthesizedExpression>();

            parent.set_type(n.get_nest().get_type());
            if(n.is_constant())
                parent.set_constant(const_value_make_vector_from_scalar(
                            _environment._unroll_factor,
                            n.get_constant()));

            n.replace(parent);
        }

        void VectorizerVisitorExpression::visit(const Nodecl::Reference& n)
        {
            walk(n.get_rhs());

            const Nodecl::Reference reference =
                Nodecl::Reference::make(
                        n.get_rhs().shallow_copy(),
                        n.get_rhs().get_type().no_ref().get_pointer_to(),
                        n.get_locus());

            n.replace(reference);
        } 
/*
        void VectorizerVisitorExpression::visit(const Nodecl::Dereference& n)
        {
            std::cerr << "Hola, soy una Dereference: " << n.prettyprint() << std::endl;
            walk(n.get_rhs());

            const Nodecl::Reference reference =
                Nodecl::Reference::make(
                        n.get_rhs().shallow_copy(),
                        Utils::get_qualified_vector_to(n.get_type(),
                            _environment._unroll_factor),
                        n.get_locus());

            n.replace(reference);
        } 
*/
        void VectorizerVisitorExpression::vectorize_basic_induction_variable(const Nodecl::Symbol& n)
        {
            DEBUG_CODE()
            {
                fprintf(stderr,"VECTORIZER: '%s' is IV and will be PROMOTED with OFFSET\n",
                        n.prettyprint().c_str()); 
            }

            // Computing IV offset {0, 1, 2, 3}
            TL::ObjectList<Nodecl::NodeclBase> literal_list;

            Nodecl::NodeclBase ind_var_increment = Vectorizer::_analysis_info->get_induction_variable_increment(
                    _environment._analysis_simd_scope, n);
            
            if (ind_var_increment.is_constant())
            {
                const_value_t *ind_var_increment_const = ind_var_increment.get_constant();
                const_value_t *i = const_value_get_zero(4, 0);
                for(unsigned int j = 0;
                        j < _environment._unroll_factor;
                        i = const_value_add(i, ind_var_increment_const), j++)
                {
                    literal_list.prepend(const_value_to_nodecl(i));
                }

                Nodecl::List offset = Nodecl::List::make(literal_list);

                // IV cannot be a reference
                TL::Type ind_var_type = Utils::get_qualified_vector_to(
                        n.get_type(), _environment._unroll_factor).no_ref();

                TL::Type offset_type = ind_var_type;

                // Get outter conversion node
                Nodecl::NodeclBase iv_with_conversion = n;
                while(iv_with_conversion.get_parent().is<Nodecl::Conversion>())
                    iv_with_conversion = iv_with_conversion.get_parent();

                // VectorLiteral offset
                Nodecl::VectorLiteral offset_vector_literal =
                    Nodecl::VectorLiteral::make(
                            offset,
                            Utils::get_null_mask(),
                            offset_type,
                            n.get_locus());

                offset_vector_literal.set_constant(get_vector_const_value(literal_list));

                Nodecl::VectorAdd vector_induction_var =
                    Nodecl::VectorAdd::make(
                            Nodecl::VectorPromotion::make(
                                iv_with_conversion.shallow_copy(),
                                Utils::get_null_mask(),
                                ind_var_type,
                                n.get_locus()),
                            offset_vector_literal,
                            Utils::get_null_mask(),
                            Utils::get_qualified_vector_to(n.get_type(), 
                                _environment._unroll_factor),
                            n.get_locus());

                iv_with_conversion.replace(vector_induction_var);
            }
            else
            {
                running_error("Vectorizer: IV increment is not constant.");
            }
        }

        const_value_t * VectorizerVisitorExpression::get_vector_const_value(const TL::ObjectList<Nodecl::NodeclBase>& list)
        {
            int size = list.size();
            ERROR_CONDITION(size == 0, "Invalid number of items in vector value", 0);

            const_value_t** value_set = new const_value_t*[size];

            int i = 0;
            for (TL::ObjectList<Nodecl::NodeclBase>::const_iterator it = list.begin();
                    it != list.end();
                    it++, i++)
            {
                value_set[i] = it->get_constant();
                ERROR_CONDITION(value_set[i] == NULL, "Invalid constant", 0);
            }

            const_value_t* result = const_value_make_vector(size, value_set);

            delete[] value_set;

            return result;
        }

        Nodecl::NodeclVisitor<void>::Ret VectorizerVisitorExpression::unhandled_node(
                const Nodecl::NodeclBase& n)
        {
            internal_error("Vectorizer: Unknown 'Expression' node %s at %s.\n%s",
                    ast_print_node_type(n.get_kind()),
                    n.get_locus(),
                    n.prettyprint().c_str());
/*
            std::cerr << "Vectorizer: Unknown 'Expression' node "
                << ast_print_node_type(n.get_kind())
                //<< "(" << n.prettyprint() << ")"
                << " at " << n.get_locus() << "." << std::endl
                << n.prettyprint()
                << std::endl;
*/
            return Ret();
        }
    }
}
