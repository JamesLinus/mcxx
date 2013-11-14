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

#include "tl-vector-backend-knc.hpp"
#include "tl-source.hpp"

#define KNC_INTRIN_PREFIX "_mm512"

namespace TL 
{
    namespace Vectorization
    {
        KNCVectorLowering::KNCVectorLowering() 
            : _vectorizer(TL::Vectorization::Vectorizer::get_vectorizer()), 
            _vector_length(64) 
        {
            std::cerr << "--- KNC lowering phase ---" << std::endl;
        }

        std::string KNCVectorLowering::get_undef_intrinsic(const TL::Type& type)
        {
            if (type.is_float()) 
            { 
                return "_mm512_undefined()";
            } 
            else if (type.is_double()) 
            { 
                return "_mm512_castps_pd(_mm512_undefined())";
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                return "_mm512_castps_si512(_mm512_undefined())";
            }

            return "undefined_error()"; 
        }

        void KNCVectorLowering::process_mask_component(const Nodecl::NodeclBase& mask,
                TL::Source& mask_prefix, TL::Source& mask_args, const TL::Type& type,
                ConfigMaskProcessing conf)
        {
            if(!mask.is_null())
            {
                TL::Source old;

                mask_prefix << "_mask";

                if (_old_m512.empty())
                {
                    old << get_undef_intrinsic(type);
                }
                else
                {
                    old << "("
                        << print_type_str(
                                type.get_vector_to(_vector_length).get_internal_type(),
                                mask.retrieve_context().get_decl_context())
                        << ")"
                        << as_expression(_old_m512.back());

                    if ((conf & ConfigMaskProcessing::KEEP_OLD) !=
                            ConfigMaskProcessing::KEEP_OLD)
                    { // DEFAULT
                        _old_m512.pop_back();
                    }
                }

                walk(mask);

                if((conf & ConfigMaskProcessing::ONLY_MASK) ==
                       ConfigMaskProcessing::ONLY_MASK)
                {
                    mask_args << as_expression(mask);
                }
                else // DEFAULT
                {
                    mask_args << old.get_source()
                        << ", "
                        << as_expression(mask)
                        ;
                }
                
                if((conf & ConfigMaskProcessing::NO_FINAL_COMMA) !=
                        ConfigMaskProcessing::NO_FINAL_COMMA)
                {
                    mask_args << ", ";
                }
            }
            else if((conf & ConfigMaskProcessing::ALWAYS_OLD) ==
                    ConfigMaskProcessing::ALWAYS_OLD)
            {
                if (!_old_m512.empty())
                {
                    internal_error("KNC Lowering: mask is null but old is not null. Old '%s'. At %s", 
                            _old_m512.back().prettyprint().c_str(),
                            locus_to_str(mask.get_locus()));
                }

                mask_args << get_undef_intrinsic(type) << ", ";
            }
        }

        void KNCVectorLowering::visit(const Nodecl::ObjectInit& node) 
        {
            TL::Source intrin_src;
            
            if(node.has_symbol())
            {
                TL::Symbol sym = node.get_symbol();

                // Vectorizing initialization
                Nodecl::NodeclBase init = sym.get_value();
                if(!init.is_null())
                {
                    walk(init);
                }
            }
        }

        void KNCVectorLowering::common_binary_op_lowering(const Nodecl::NodeclBase& node,
                const std::string& intrin_op_name)
        {
            const Nodecl::VectorAdd& binary_node = node.as<Nodecl::VectorAdd>();

            const Nodecl::NodeclBase lhs = binary_node.get_lhs();
            const Nodecl::NodeclBase rhs = binary_node.get_rhs();
            const Nodecl::NodeclBase mask = binary_node.get_mask();

            TL::Type type = binary_node.get_type().basic_type();

            TL::Source intrin_src, intrin_name, intrin_type_suffix,
                mask_prefix, args, mask_args;

            intrin_src << intrin_name
                << "("
                << args
                << ")"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type);

            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps"; 
            } 
            else if (type.is_double()) 
            { 
                intrin_type_suffix << "pd"; 
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_type_suffix << "epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type: %s.", 
                        ast_print_node_type(binary_node.get_kind()),
                        locus_to_str(binary_node.get_locus()),
                        type.get_simple_declaration(node.retrieve_context(), "").c_str());
            }      

            walk(lhs);
            walk(rhs);

            args << mask_args
                << as_expression(lhs)
                << ", "
                << as_expression(rhs)
                ;

            Nodecl::NodeclBase function_call = 
                    intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::bitwise_binary_op_lowering(const Nodecl::NodeclBase& node,
                const std::string& intrin_op_name)
        {
            const Nodecl::VectorBitwiseAnd& binary_node = node.as<Nodecl::VectorBitwiseAnd>();

            const Nodecl::NodeclBase lhs = binary_node.get_lhs();
            const Nodecl::NodeclBase rhs = binary_node.get_rhs();
            const Nodecl::NodeclBase mask = binary_node.get_mask();

            TL::Type type = binary_node.get_type().basic_type();

            TL::Source intrin_src, casting_intrin, intrin_name, intrin_type_suffix,
                mask_prefix, casting_args, args, mask_args;

            intrin_src << casting_intrin
                << "("
                << intrin_name
                << "("
                << args
                << "))"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type);

            if (type.is_float()) 
            { 
                casting_intrin << "_mm512_castsi512i_ps";
                casting_args << "_mm512_castps_si512i";

                intrin_type_suffix << "epi32"; 
            } 
            else if (type.is_double()) 
            { 
                casting_intrin << "_mm512_castsi512i_pd";
                casting_args << "_mm512_castpd_si512i";
 
                intrin_type_suffix << "epi64"; 
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_type_suffix << "epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(binary_node.get_kind()),
                        locus_to_str(binary_node.get_locus()));
            }      

            walk(lhs);
            walk(rhs);
 
            args << mask_args
                << casting_args << "(" << as_expression(lhs) << ")"
                << ", "
                << casting_args << "(" << as_expression(rhs) << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorAdd& node) 
        {
            common_binary_op_lowering(node, "add"); 
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorMinus& node) 
        {
            common_binary_op_lowering(node, "sub");
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorMul& node) 
        {
            TL::Type type = node.get_type().basic_type();
            
            if (type.is_integral_type())
                common_binary_op_lowering(node, "mullo");
            else
                common_binary_op_lowering(node, "mul");
        }    

        void KNCVectorLowering::visit(const Nodecl::VectorDiv& node) 
        { 
            common_binary_op_lowering(node, "div");
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorMod& node) 
        { 
            common_binary_op_lowering(node, "rem");
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorFmadd& node)
        {
            const Nodecl::NodeclBase first_op = node.get_first_op();
            const Nodecl::NodeclBase second_op = node.get_second_op();
            const Nodecl::NodeclBase third_op = node.get_third_op();
            const Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, intrin_name, intrin_type_suffix,
                mask_prefix, args, mask_args;

            intrin_src << intrin_name
                << "("
                << args
                << ")"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << "fmadd_round"
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type,
                    ConfigMaskProcessing::ONLY_MASK);

            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps"; 
            } 
            else if (type.is_double()) 
            { 
                intrin_type_suffix << "pd"; 
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_type_suffix << "epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(first_op);
            walk(second_op);
            walk(third_op);

            args << as_expression(first_op)
                << ", "
                << mask_args
                << as_expression(second_op)
                << ", "
                << as_expression(third_op)
                << ", "
                << _MM_FROUND_CUR_DIRECTION
                ;

            Nodecl::NodeclBase function_call = 
                    intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorLowerThan& node) 
        { 
            const TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src;
            TL::Source cmp_flavor;
            
            // Intrinsic name
            intrin_src << "_mm512_cmp";

            if (type.is_float()) 
            { 
                intrin_src << "_ps"; 
                cmp_flavor << _CMP_LT_OS;
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_src << "_epi32"; 
                cmp_flavor << "_MM_CMPINT_LT";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mask(";
            intrin_src << as_expression(node.get_lhs());
            intrin_src << ", ";
            intrin_src << as_expression(node.get_rhs());
            intrin_src << ", ";
            intrin_src << cmp_flavor;
            intrin_src << ")";

            Nodecl::NodeclBase function_call = 
                    intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorLowerOrEqualThan& node) 
        { 
            const TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src;
            TL::Source cmp_flavor;

            std::cerr << "KNC:::> " << node.prettyprint() << std::endl;

            // Intrinsic name
            intrin_src << "_mm512_cmp";

            if (type.is_float()) 
            { 
                intrin_src << "_ps"; 
                cmp_flavor << _CMP_LE_OS;
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_src << "_epi32"; 
                cmp_flavor << "_MM_CMPINT_LE";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mask(";
            intrin_src << as_expression(node.get_lhs());
            intrin_src << ", ";
            intrin_src << as_expression(node.get_rhs());
            intrin_src << ", ";
            intrin_src << cmp_flavor;
            intrin_src << ")";

            Nodecl::NodeclBase function_call = 
                    intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorGreaterThan& node) 
        { 
            const TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src;
            TL::Source cmp_flavor;

            // Intrinsic name
            intrin_src << "_mm512_cmp";

            if (type.is_float()) 
            { 
                intrin_src << "_ps"; 
                cmp_flavor << _CMP_GT_OS;
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_src << "_epi32"; 
                cmp_flavor << "_MM_CMPINT_NLE";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }     
            
            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mask(";
            intrin_src << as_expression(node.get_lhs());
            intrin_src << ", ";
            intrin_src << as_expression(node.get_rhs());
            intrin_src << ", ";
            intrin_src << cmp_flavor;
            intrin_src << ")";

            Nodecl::NodeclBase function_call = 
                    intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }   

        void KNCVectorLowering::visit(const Nodecl::VectorGreaterOrEqualThan& node) 
        { 
            const TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src;
            TL::Source cmp_flavor;

            // Intrinsic name
            intrin_src << "_mm512_cmp";
            cmp_flavor << _CMP_GE_OS;

            if (type.is_float()) 
            { 
                intrin_src << "_ps"; 
                cmp_flavor << _CMP_GE_OS;
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_src << "_epi32"; 
                cmp_flavor << "_MM_CMPINT_NLT";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }     
            
            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mask(";
            intrin_src << as_expression(node.get_lhs());
            intrin_src << ", ";
            intrin_src << as_expression(node.get_rhs());
            intrin_src << ", ";
            intrin_src << cmp_flavor;
            intrin_src << ")";

            Nodecl::NodeclBase function_call = 
                    intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorEqual& node) 
        { 
            const TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src;
            TL::Source cmp_flavor;

            // Intrinsic name
            intrin_src << "_mm512_cmp";

            if (type.is_float()) 
            { 
                intrin_src << "_ps"; 
                cmp_flavor << _CMP_EQ_OQ;
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_src << "_epi32"; 
                cmp_flavor << "_MM_CMPINT_EQ";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mask("
                << as_expression(node.get_lhs())
                << ", "
                << as_expression(node.get_rhs())
                << ", "
                << cmp_flavor
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorDifferent& node)
        { 
            const TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src;
            TL::Source cmp_flavor;

            // Intrinsic name
            intrin_src << "_mm512_cmp";

            if (type.is_float()) 
            { 
                intrin_src << "_ps"; 
                cmp_flavor << _CMP_NEQ_UQ;
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_src << "_epi32"; 
                cmp_flavor << "_MM_CMPINT_NE";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mask("
                << as_expression(node.get_lhs())
                << ", "
                << as_expression(node.get_rhs())
                << ", "
                << cmp_flavor
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorBitwiseAnd& node) 
        { 
            bitwise_binary_op_lowering(node, "and");
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorBitwiseOr& node) 
        { 
            bitwise_binary_op_lowering(node, "or");
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorBitwiseXor& node) 
        { 
            bitwise_binary_op_lowering(node, "xor");
        }

        void KNCVectorLowering::visit(const Nodecl::VectorBitwiseShl& node) 
        { 
            const Nodecl::NodeclBase lhs = node.get_lhs();
            const Nodecl::NodeclBase rhs = node.get_rhs();
            const Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, casting_intrin, intrin_name, intrin_type_suffix, intrin_op_name,
                mask_prefix, casting_args, args, mask_args, rhs_expression;

            intrin_src << casting_intrin
                << "("
                << intrin_name
                << "("
                << args
                << "))"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type);

            if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_type_suffix << "epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(lhs);

            //RHS
            if (rhs.is<Nodecl::VectorPromotion>())
            {
                intrin_op_name << "slli";
                rhs_expression << as_expression(rhs.as<Nodecl::VectorPromotion>().get_rhs());
            }
            else
            {
                intrin_op_name << "sllv";
                walk(rhs);
                rhs_expression << as_expression(rhs);
            }
 
            args << mask_args
                << casting_args << "(" << as_expression(lhs) << ")"
                << ", "
                << casting_args << "(" << rhs_expression << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorBitwiseShr& node) 
        { 
            const Nodecl::NodeclBase lhs = node.get_lhs();
            const Nodecl::NodeclBase rhs = node.get_rhs();
            const Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, casting_intrin, intrin_name, intrin_type_suffix, intrin_op_name,
                mask_prefix, casting_args, args, mask_args, rhs_expression;

            intrin_src << casting_intrin
                << "("
                << intrin_name
                << "("
                << args
                << "))"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type);

            if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_type_suffix << "epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(lhs);

            //RHS
            if (rhs.is<Nodecl::VectorPromotion>())
            {
                intrin_op_name << "srli";
                rhs_expression << as_expression(rhs.as<Nodecl::VectorPromotion>().get_rhs());
            }
            else
            {
                intrin_op_name << "srlv";
                walk(rhs);
                rhs_expression << as_expression(rhs);
            }
 
            args << mask_args
                << casting_args << "(" << as_expression(lhs) << ")"
                << ", "
                << casting_args << "(" << rhs_expression << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorLogicalOr& node) 
        { 
            running_error("KNC Lowering %s: 'logical or' operation (i.e., operator '||') is not "\
                    "supported in KNC. Try using 'bitwise or' operations (i.e., operator '|') instead if possible.",
                    locus_to_str(node.get_locus()));
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorNeg& node) 
        {
            Nodecl::NodeclBase rhs = node.get_rhs();
            Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, casting_intrin, neg_op;

            intrin_src << casting_intrin
                << "("
                << neg_op
                << ")"
                ;

            if (type.is_float()) 
            { 
                casting_intrin << "_mm512_castsi512_ps";

                TL::Type vector_int_type = 
                    TL::Type::get_int_type().get_vector_to(_vector_length);

                Nodecl::VectorBitwiseXor vector_xor =
                    Nodecl::VectorBitwiseXor::make(
                        Nodecl::VectorPromotion::make(
                            Nodecl::IntegerLiteral::make(
                                TL::Type::get_int_type(),
                                const_value_get_signed_int(0x80000000)),
                            mask.shallow_copy(),
                            vector_int_type),
                        Nodecl::VectorCast::make(
                            rhs.shallow_copy(),
                            mask.shallow_copy(),
                            vector_int_type),
                        mask.shallow_copy(),
                        vector_int_type);
                            
                walk(vector_xor);

                neg_op << as_expression(vector_xor);
            } 
            else if (type.is_double()) 
            { 
                casting_intrin << "_mm512_castsi512_pd";

                TL::Type vector_int_type = 
                    TL::Type::get_long_long_int_type().get_vector_to(_vector_length);

                Nodecl::VectorBitwiseXor vector_xor =
                    Nodecl::VectorBitwiseXor::make(
                        Nodecl::VectorPromotion::make(
                            Nodecl::IntegerLiteral::make(
                                TL::Type::get_long_long_int_type(),
                                const_value_get_signed_int(0x8000000000000000LL)),
                            mask.shallow_copy(),
                            vector_int_type),
                        Nodecl::VectorCast::make(
                            rhs.shallow_copy(),
                            mask.shallow_copy(),
                            vector_int_type),
                        mask.shallow_copy(),
                        vector_int_type);
                            
                walk(vector_xor);

                neg_op << as_expression(vector_xor);
            }
            else if (type.is_signed_int() ||
                    type.is_unsigned_int())
            {
                TL::Type vector_int_type = 
                    TL::Type::get_int_type().get_vector_to(_vector_length);

                Nodecl::VectorMinus vector_minus =
                    Nodecl::VectorMinus::make(
                        Nodecl::VectorPromotion::make(
                            Nodecl::IntegerLiteral::make(
                                TL::Type::get_int_type(),
                                const_value_get_zero(4 , 1)),
                            mask.shallow_copy(),
                            vector_int_type),
                        rhs.shallow_copy(),
                        mask.shallow_copy(),
                        vector_int_type);
                            
                walk(vector_minus);

                neg_op << as_expression(vector_minus);
            }
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            } 

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorConversion& node) 
        {
            const Nodecl::NodeclBase nest = node.get_nest();
            const Nodecl::NodeclBase mask = node.get_mask();

            const TL::Type& src_vector_type = nest.get_type().get_unqualified_type().no_ref();
            const TL::Type& dst_vector_type = node.get_type().get_unqualified_type().no_ref();
            const TL::Type& src_type = src_vector_type.basic_type().get_unqualified_type();
            const TL::Type& dst_type = dst_vector_type.basic_type().get_unqualified_type();
            const int src_type_size = src_type.get_size();
            const int dst_type_size = dst_type.get_size();
/*
            printf("Conversion from %s(%s) to %s(%s)\n",
                    src_vector_type.get_simple_declaration(node.retrieve_context(), "").c_str(),
                    src_type.get_simple_declaration(node.retrieve_context(), "").c_str(),
                    dst_vector_type.get_simple_declaration(node.retrieve_context(), "").c_str(),
                    dst_type.get_simple_declaration(node.retrieve_context(), "").c_str());
*/
            //const unsigned int src_num_elements = src_vector_type.vector_num_elements();
            const unsigned int dst_num_elements = dst_vector_type.vector_num_elements();

            TL::Source intrin_src, intrin_name, intrin_op_name,
                mask_prefix, args, mask_args, extra_args;

            intrin_src << intrin_name
                << "("
                << args
                << ")"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                ;

            process_mask_component(mask, mask_prefix, mask_args, dst_type);

            walk(nest);

            if (src_type.is_same_type(dst_type))
            {
                node.replace(nest);
                return;
            }
            else if ((src_type.is_signed_int() && dst_type.is_unsigned_int()) ||
                    (dst_type.is_signed_int() && src_type.is_unsigned_int()) ||
                    (src_type.is_signed_int() && dst_type.is_signed_long_int()) ||
                    (src_type.is_signed_int() && dst_type.is_unsigned_long_int()) ||
                    (src_type.is_unsigned_int() && dst_type.is_signed_long_int()) ||
                    (src_type.is_unsigned_int() && dst_type.is_unsigned_long_int()) ||
                    (src_type.is_signed_int() && src_type.is_signed_long_int()) ||
                    (src_type.is_signed_int() && src_type.is_unsigned_long_int()) ||
                    (src_type.is_unsigned_int() && src_type.is_signed_long_int()) ||
                    (src_type.is_unsigned_int() && src_type.is_unsigned_long_int()) ||
                    (src_type.is_signed_short_int() && dst_type.is_unsigned_short_int()) ||
                    (dst_type.is_signed_short_int() && src_type.is_unsigned_short_int()))
            {
                node.replace(nest);
                return;
            }
            // SIZE_DST == SIZE_SRC
            else if (src_type_size == dst_type_size)
            {
                extra_args << ", "
                    << "_MM_ROUND_MODE_NEAREST"
                    << ", "
                    << "_MM_EXPADJ_NONE"
                    ;

                if (src_type.is_signed_int() &&
                        dst_type.is_float()) 
                { 
                    intrin_op_name << "cvtfxpnt_round_adjustepi32_ps";
                } 
                else if (src_type.is_unsigned_int() &&
                        dst_type.is_float()) 
                { 
                    intrin_op_name << "cvtfxpnt_round_adjustepu32_ps";
                } 
                else if (src_type.is_float() &&
                        dst_type.is_signed_int()) 
                { 
                    // C/C++ requires truncated conversion
                    intrin_op_name << "cvtfxpnt_round_adjustps_epi32";
                }
                else if (src_type.is_float() &&
                        dst_type.is_unsigned_int()) 
                { 
                    // C/C++ requires truncated conversion
                    intrin_op_name << "cvtfxpnt_round_adjustps_epu32";
                }
            }
            // SIZE_DST > SIZE_SRC
            else if (src_type_size < dst_type_size)
            {
                // From float8 to double8
                if (src_type.is_float() && dst_type.is_double() && (dst_num_elements == 8))
                {
                    intrin_op_name << "cvtpslo_pd";
                }
                // From int8 to double8
                else if (src_type.is_signed_int() && dst_type.is_double() && (dst_num_elements == 8))
                {
                    intrin_op_name << "cvtepi32lo_pd";
                }
                else if (src_type.is_unsigned_int() && dst_type.is_double() && (dst_num_elements == 8))
                {
                    intrin_op_name << "cvtepu32lo_pd";
                }
            }
            // SIZE_DST < SIZE_SRC
            else if (src_type_size > dst_type_size)
            {
            }

            if (intrin_op_name.empty())
            {
                fprintf(stderr, "KNC Lowering: Masked conversion from '%s' to '%s' at '%s' is not supported yet: %s\n",
                        src_type.get_simple_declaration(node.retrieve_context(), "").c_str(),
                        dst_type.get_simple_declaration(node.retrieve_context(), "").c_str(),
                        locus_to_str(node.get_locus()),
                        nest.prettyprint().c_str());
            }   

            args << mask_args
                << as_expression(nest)
                << extra_args
                ;

            Nodecl::NodeclBase function_call = 
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorCast& node) 
        {
            const Nodecl::NodeclBase rhs = node.get_rhs();

            const TL::Type& dst_vector_type = node.get_type().get_unqualified_type().no_ref();
            const TL::Type& dst_type = dst_vector_type.basic_type().get_unqualified_type();

            TL::Source intrin_src, cast_type, args;

            intrin_src << "("
                << cast_type
                << "("
                << args
                << "))"
                ;

            walk(rhs);

            if (dst_type.is_float()) 
            { 
                cast_type << "(__m512)";
            } 
            else if (dst_type.is_double()) 
            {
                cast_type << "(__m512d)";
            }
            else if (dst_type.is_integral_type()) 
            {
                cast_type << "(__m512i)";
            }
            else 
            {
                fprintf(stderr, "KNC Lowering: Casting at '%s' is not supported yet: %s\n", 
                        locus_to_str(node.get_locus()),
                        rhs.prettyprint().c_str());
            }   

            args << as_expression(rhs);

            Nodecl::NodeclBase function_call = 
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorPromotion& node) 
        { 
            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src;

            // Intrinsic name
            intrin_src << "_mm512_set1";

            if (type.is_float()) 
            { 
                intrin_src << "_ps"; 
            } 
            else if (type.is_double()) 
            { 
                intrin_src << "_pd"; 
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_src << "_epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(node.get_rhs());
            
            intrin_src << "("; 
            intrin_src << as_expression(node.get_rhs());
            intrin_src << ")"; 

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }        

        void KNCVectorLowering::visit(const Nodecl::VectorLiteral& node) 
        {
            TL::Type vector_type = node.get_type();
            TL::Type scalar_type = vector_type.basic_type();

            TL::Source intrin_src, intrin_name, undefined_value, values;

            intrin_src << intrin_name
                << "("
                << values
                << ")"
                ;

            // Intrinsic name
            intrin_name << "_mm512_set";

            if (scalar_type.is_float()) 
            { 
                intrin_name << "_ps"; 
                undefined_value << "0.0f";
            } 
            else if (scalar_type.is_double()) 
            { 
                intrin_name << "_pd";
                undefined_value << "0.0";
            } 
            else if (scalar_type.is_signed_int() ||
                    scalar_type.is_unsigned_int()) 
            { 
                intrin_name << "_epi32"; 
                undefined_value << "0";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported scalar_type (%s).", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()),
                        scalar_type.get_simple_declaration(node.retrieve_context(), "").c_str());
            }      

            Nodecl::List scalar_values =
                node.get_scalar_values().as<Nodecl::List>();


            unsigned int num_undefined_values =
                _vector_length/scalar_type.get_size() - vector_type.vector_num_elements();
            
            for (unsigned int i=0; i<num_undefined_values; i++)
            {
                values.append_with_separator(undefined_value, ",");
            }

            for (Nodecl::List::const_iterator it = scalar_values.begin();
                    it != scalar_values.end();
                    it++)
            {
                walk((*it));
                values.append_with_separator(as_expression(*it), ",");
            }

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }        

        void KNCVectorLowering::visit(const Nodecl::VectorConditionalExpression& node) 
        { 
            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, swizzle;

            Nodecl::NodeclBase true_node = node.get_true();
            Nodecl::NodeclBase false_node = node.get_false();
            Nodecl::NodeclBase condition_node = node.get_condition();

            TL::Type true_type = true_node.get_type().basic_type();
            TL::Type false_type = false_node.get_type().basic_type();
            TL::Type condiition_type = condition_node.get_type();

            if (true_type.is_float()
                    && false_type.is_float())
            {
                intrin_src << "_mm512_mask_mov_ps";
            }
            else if (true_type.is_double()
                    && false_type.is_double())
            {
                intrin_src << "_mm512_mask_mov_pd";
            }
            else if (true_type.is_integral_type()
                    && false_type.is_integral_type())
            {
                intrin_src << "_mm512_mask_swizzle_epi32";
                swizzle << ", _MM_SWIZ_REG_NONE";
            }
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            walk(false_node);
            walk(true_node);
            walk(condition_node);

            intrin_src << "(" 
                << as_expression(false_node) // False first!
                << ", "
                << as_expression(condition_node)
                << ", "
                << as_expression(true_node)
                << swizzle
                << ")"; 

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }        

        void KNCVectorLowering::visit(const Nodecl::VectorAssignment& node) 
        {
            Nodecl::NodeclBase lhs = node.get_lhs();
            Nodecl::NodeclBase rhs = node.get_rhs();
            Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, intrin_name, intrin_type_suffix, intrin_op_name,
                mask_prefix, casting_args, args, mask_args;

            intrin_src << as_expression(lhs)
                << " = "
                << intrin_name
                << "("
                << args
                << ")"
                ;

            walk(lhs);

            if (mask.is_null())
            {
                walk(rhs);
                args << as_expression(rhs);
            }
            else
            {
                // LHS has old_value of rhs
                _old_m512.push_back(lhs);

                // Visit RHS with lhs as old
                walk(rhs);

                // Nodes that needs implicit blending
                if (!_old_m512.empty())
                {
                    if (lhs != _old_m512.back())
                    {
                        internal_error("KNC Lowering: Different old value and lhs in assignment with blend. LHS node '%s'. Old '%s'. At %s", 
                                lhs.prettyprint().c_str(),
                                _old_m512.back().prettyprint().c_str(),
                                locus_to_str(node.get_locus()));
                    }

                    process_mask_component(mask, mask_prefix, mask_args, type,
                            Vectorization::ConfigMaskProcessing::ONLY_MASK);

                    intrin_name << KNC_INTRIN_PREFIX
                        << mask_prefix
                        << "_"
                        << intrin_op_name
                        << "_"
                        << intrin_type_suffix
                        ;

                    args << mask_args
                        << as_expression(lhs)
                        << ", "
                        << as_expression(rhs)
                        ;

                    if (type.is_float()) 
                    {
                        intrin_op_name << "blend";
                        intrin_type_suffix << "ps";
                    } 
                    else if (type.is_double()) 
                    { 
                        intrin_op_name << "blend";
                        intrin_type_suffix << "pd";
                    } 
                    else if (type.is_integral_type()) 
                    { 
                        intrin_op_name << "blend";
                        intrin_type_suffix << "epi32";
                    }
                }
                else
                {
                    args << as_expression(rhs);
                }
            }

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }                                                 

        void KNCVectorLowering::visit(const Nodecl::VectorLoad& node) 
        {
            Nodecl::NodeclBase rhs = node.get_rhs();
            Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, intrin_name, intrin_type_suffix, intrin_op_name,
                mask_prefix, casting_args, args, mask_args, extra_args;

            intrin_src << intrin_name
                << "("
                << args
                << ")"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type);

            intrin_op_name << "load";

            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps"; 
            } 
            else if (type.is_double()) 
            { 
                intrin_type_suffix << "pd";
            } 
            else if (type.is_integral_type()) 
            { 
                intrin_type_suffix << "epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            walk(rhs);

            args << mask_args
                << as_expression(rhs)
                << extra_args
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());
            
            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::UnalignedVectorLoad& node) 
        { 
            Nodecl::NodeclBase rhs = node.get_rhs();
            Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, intrin_name_hi, intrin_name_lo, intrin_type_suffix,
                mask_prefix, mask_args, args_lo, args_hi, extra_args;

            intrin_src << intrin_name_hi
                << "("
                << intrin_name_lo
                << "("
                << args_lo
                << ")"
                << ", "
                << args_hi
                << ")"
                ;

            intrin_name_hi << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << "extloadunpackhi"
                << "_"
                << intrin_type_suffix
                ;

            intrin_name_lo << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << "extloadunpacklo"
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type,
                    ConfigMaskProcessing::ALWAYS_OLD);

            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps"; 
                extra_args << "_MM_UPCONV_PS_NONE";
            } 
            else if (type.is_double()) 
            { 
                intrin_type_suffix << "pd"; 
                extra_args << "_MM_UPCONV_PD_NONE";
            } 
            else if (type.is_integral_type()) 
            { 
                intrin_type_suffix << "epi32"; 
                extra_args << "_MM_UPCONV_EPI32_NONE";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            args_lo << mask_args
                << as_expression(rhs)
                << ", "
                << extra_args
                << ", "
                << _MM_HINT_NONE
                ;

            args_hi = (!mask.is_null()) ?  as_expression(mask) + ", " : "";
            args_hi
                << "("
                << "(void *)" << as_expression(rhs)
                << ") + "
                << _vector_length
                << ", "
                << extra_args
                << ", "
                << _MM_HINT_NONE
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());
            
            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorStore& node) 
        {
            Nodecl::NodeclBase lhs = node.get_lhs();
            Nodecl::NodeclBase rhs = node.get_rhs();
            Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src, intrin_name, intrin_op_name, args,
                intrin_type_suffix, mask_prefix, mask_args, casting_args;


            intrin_src << intrin_name
                << "("
                << args
                << ")"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type, 
                    ConfigMaskProcessing::ONLY_MASK );

            intrin_op_name << "store";

            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps"; 
            } 
            else if (type.is_double()) 
            { 
                intrin_type_suffix << "pd"; 
            } 
            else if (type.is_integral_type()) 
            { 
                intrin_type_suffix << "epi32";
                casting_args << "(" 
                    << print_type_str(
                            TL::Type::get_void_type().get_pointer_to().get_internal_type(),
                            node.retrieve_context().get_decl_context()) 
                    << ")";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            walk(lhs);
            walk(rhs);

            args << "(" 
                << casting_args
                << as_expression(lhs)
                << ")"
                << ", "
                << mask_args
                << as_expression(rhs)
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::UnalignedVectorStore& node) 
        { 
            Nodecl::NodeclBase lhs = node.get_lhs();
            Nodecl::NodeclBase rhs = node.get_rhs();
            Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_lhs().get_type().basic_type();

            TL::Source intrin_src, intrin_name_hi, intrin_name_lo, intrin_type_suffix,
                mask_prefix, mask_args, args_lo, args_hi, extra_args, tmp_var, 
                tmp_var_type, tmp_var_name, tmp_var_init;

            intrin_src 
                << "({"
                << tmp_var << ";"
                << intrin_name_lo << "(" << args_lo << ");"
                << intrin_name_hi << "(" << args_hi << ");"
                << "})"
                ;

            intrin_name_hi << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << "extpackstorehi"
                << "_"
                << intrin_type_suffix
                ;

            intrin_name_lo << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << "extpackstorelo"
                << "_"
                << intrin_type_suffix
                ;

            tmp_var << tmp_var_type
                << " "
                << tmp_var_name
                << tmp_var_init;

            process_mask_component(mask, mask_prefix, mask_args, type, 
                    ConfigMaskProcessing::ONLY_MASK);


            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps"; 
                extra_args << "_MM_DOWNCONV_PS_NONE";
                tmp_var_type << "__m512";
            } 
            else if (type.is_double()) 
            { 
                intrin_type_suffix << "pd"; 
                extra_args << "_MM_DOWNCONV_PD_NONE";
                tmp_var_type << "__m512d";
            } 
            else if (type.is_integral_type()) 
            { 
                intrin_type_suffix << "epi32"; 
                extra_args << "_MM_DOWNCONV_EPI32_NONE";
                tmp_var_type << "__m512i";
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            walk(lhs);
            walk(rhs);

            tmp_var_name << "__vtmp";

            tmp_var_init << " = "
                << as_expression(rhs);

            args_lo << as_expression(lhs)
                << ", "
                << mask_args
                << tmp_var_name
                << ", "
                << extra_args
                << ", "
                << _MM_HINT_NONE
                ;

            args_hi << "("
                << "(void *)" << as_expression(lhs)
                << ") + "
                << _vector_length
                << ", "
                << mask_args
                << tmp_var_name
                << ", "
                << extra_args
                << ", "
                << _MM_HINT_NONE
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());
            
            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorGather& node) 
        {
            const Nodecl::NodeclBase base = node.get_base();
            const Nodecl::NodeclBase strides = node.get_strides();
            const Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = node.get_type().basic_type();
            TL::Type index_type = strides.get_type().basic_type();

            TL::Source intrin_src, intrin_name, intrin_op_name, intrin_type_suffix,
                mask_prefix, args, mask_args, extra_args;

            intrin_src << intrin_name
                << "("
                << args
                << ")"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type);


            intrin_op_name << "i32extgather";

            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps";
                extra_args << "_MM_DOWNCONV_PS_NONE";
            } 
            else if (type.is_signed_int() || type.is_unsigned_int()) 
            { 
                intrin_type_suffix << "epi32";
                extra_args << "_MM_DOWNCONV_EPI32_NONE";
            }
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported source type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            if ((!index_type.is_signed_int()) && (!index_type.is_unsigned_int()))
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported index type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            walk(base);
            walk(strides);

            args << mask_args
                << as_expression(strides)
                << ", "
                << as_expression(base) 
                << ", " 
                << extra_args
                << ", "
                << type.get_size()
                << ", "
                << _MM_HINT_NONE
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorScatter& node) 
        { 
            const Nodecl::NodeclBase base = node.get_base();
            const Nodecl::NodeclBase strides = node.get_strides();
            const Nodecl::NodeclBase source = node.get_source();
            const Nodecl::NodeclBase mask = node.get_mask();

            TL::Type type = source.get_type().basic_type();
            TL::Type index_type = strides.get_type().basic_type();

            TL::Source intrin_src, intrin_name, intrin_op_name, intrin_type_suffix,
                mask_prefix, args, mask_args, extra_args;

            intrin_src << intrin_name
                << "("
                << args
                << ")"
                ;

            intrin_name << KNC_INTRIN_PREFIX
                << mask_prefix
                << "_"
                << intrin_op_name
                << "_"
                << intrin_type_suffix
                ;

            process_mask_component(mask, mask_prefix, mask_args, type,
                    ConfigMaskProcessing::ONLY_MASK);

            intrin_op_name << "i32extscatter";


            // Indexes
            if ((!index_type.is_signed_int()) && (!index_type.is_unsigned_int()) &&
                  (!index_type.is_signed_long_int()) && (!index_type.is_unsigned_long_int())) 
            { 
                internal_error("KNC Lowering: Node %s at %s has an unsupported index type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            // Source
            if (type.is_float()) 
            { 
                intrin_type_suffix << "ps";
                extra_args << "_MM_DOWNCONV_PS_NONE";
            } 
            else if (type.is_signed_int() || type.is_unsigned_int()) 
            { 
                intrin_type_suffix << "epi32";
                extra_args << "_MM_DOWNCONV_EPI32_NONE";
            }
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported source type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }

            walk(base);
            walk(strides);
            walk(source);

            args << as_expression(base)
                << ", "
                << mask_args
                << as_expression(strides)
                << ", "
                << as_expression(source)
                << ", "
                << extra_args
                << ", "
                << type.get_size()
                << ", "
                << _MM_HINT_NONE
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }
 
        void KNCVectorLowering::visit(const Nodecl::VectorFunctionCall& node) 
        {
            Nodecl::FunctionCall function_call =
                node.get_function_call().as<Nodecl::FunctionCall>();

            const Nodecl::NodeclBase mask = node.get_mask();
            TL::Type vector_type = node.get_type();
            TL::Type scalar_type = vector_type.basic_type();
            Nodecl::List arguments = function_call.get_arguments().as<Nodecl::List>();

            if (mask.is_null()) // UNMASKED FUNCTION CALLS
            {
                walk(arguments);
                node.replace(function_call);
            }
            else // MASKED FUNCTION CALLS
            {
                TL::Source intrin_src, intrin_name, intrin_op_name, intrin_type_suffix,
                    mask_prefix, args, mask_args, extra_args;

                intrin_src << intrin_name
                    << "("
                    << args
                    << ")"
                    ;

                // Vector function name
                intrin_name << function_call.get_called().
                    as<Nodecl::Symbol>().get_symbol().get_name();

                TL::Symbol scalar_sym =
                    node.get_scalar_symbol().as<Nodecl::Symbol>().get_symbol();

                // Use scalar symbol to look up
                if(_vectorizer.is_svml_function(scalar_sym.get_name(),
                            "knc",
                            _vector_length,
                            scalar_type,
                            /*masked*/ !mask.is_null()))
                {
                    process_mask_component(mask, mask_prefix, mask_args, scalar_type,
                            ConfigMaskProcessing::NO_FINAL_COMMA);

                    walk(arguments);

                    args << mask_args;
                    for (Nodecl::List::const_iterator it = arguments.begin();
                            it != arguments.end();
                            it++)
                    {
                        args.append_with_separator(as_expression(*it), ", ");
                    }

                    Nodecl::NodeclBase intrin_function_call =
                        intrin_src.parse_expression(node.retrieve_context());

                    node.replace(intrin_function_call);
                }
                else // Compound Expression to avoid infinite recursion
                {
                    TL::Source conditional_exp, mask_casting;

                    process_mask_component(mask, mask_prefix, mask_args, scalar_type,
                            ConfigMaskProcessing::ONLY_MASK | ConfigMaskProcessing::NO_FINAL_COMMA);

                    walk(arguments);

                    for (Nodecl::List::const_iterator it = arguments.begin();
                            it != arguments.end();
                            it++)
                    {
                        args.append_with_separator(as_expression(*it), ", ");
                    }

                    args.append_with_separator(mask_args, ", ");

                    mask_casting << "("
                        << mask.get_type().no_ref().get_simple_declaration(
                                mask.retrieve_context(), "") 
                        << ")";

                    // Conditional expression
//#warning This should work
/*
                    conditional_exp << "("
                        << "(" << as_expression(mask) << "!= " << "(" << mask_casting << "0))"
                        << " ? " <<  intrin_src << " : " << get_undef_intrinsic(scalar_type)
                        << ")"
                        ;
*/
                    conditional_exp <<  intrin_src;

                    Nodecl::NodeclBase conditional_exp_node =
                        conditional_exp.parse_expression(node.retrieve_context());

                    node.replace(conditional_exp_node);
                }
            }
        }

        void KNCVectorLowering::visit(const Nodecl::VectorFabs& node) 
        {
            const Nodecl::NodeclBase mask = node.get_mask();
            const Nodecl::NodeclBase argument = node.get_argument();

            TL::Type type = node.get_type().basic_type();
            
            TL::Source intrin_src, mask_prefix, mask_args;

            process_mask_component(mask, mask_prefix, mask_args, 
                    TL::Type::get_int_type());

            walk(argument);

            // Handcoded implementations for float and double
            if (type.is_float()) 
            { 
                intrin_src << "_mm512_castsi512_ps(" << KNC_INTRIN_PREFIX << mask_prefix << "_and_epi32("
                    << mask_args  
                    << "_mm512_castps_si512("
                    << as_expression(argument)
                    << "), _mm512_set1_epi32(0x7FFFFFFF)))"; 
            } 
            else if (type.is_double()) 
            { 
                intrin_src << "_mm512_castsi512_pd(" << KNC_INTRIN_PREFIX << mask_prefix << "_and_epi64("
                    << mask_args
                    << "_mm512_castpd_si512("
                    << as_expression(argument)
                    << "), _mm512_set1_epi64(0x7FFFFFFFFFFFFFFFLL)))"; 
            }
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }
            
            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());
            
            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorSincos& node) 
        {
            const Nodecl::NodeclBase mask = node.get_mask();
            const Nodecl::NodeclBase source = node.get_source();
            const Nodecl::NodeclBase sin_pointer = node.get_sin_pointer();
            const Nodecl::NodeclBase cos_pointer = node.get_cos_pointer();

            TL::Type type = source.get_type().basic_type();
            
            TL::Source intrin_src, mask_prefix, mask_args;

            process_mask_component(mask, mask_prefix, mask_args, TL::Type::get_int_type(),
                    ConfigMaskProcessing::ONLY_MASK);

            walk(source);

            // Handcoded implementations for float and double
            if (type.is_float()) 
            { 
                intrin_src << "(*" << as_expression(sin_pointer) << ")"
                    << " = __svml_sincosf16_ha" << mask_prefix
                    << "("
                    << as_expression(cos_pointer)
                    << " ,"
                    << mask_args  
                    << as_expression(source)
                    << ")"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }
           
            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());
            
            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::ParenthesizedExpression& node)
        {
            walk(node.get_nest());

            Nodecl::NodeclBase n(node.shallow_copy());
            n.set_type(node.get_nest().get_type());
            node.replace(n);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorReductionAdd& node) 
        { 
            TL::Type type = node.get_type().basic_type();

            TL::Source intrin_src, intrin_name;

            intrin_name << "_mm512_reduce_add";

            if (type.is_float()) 
            { 
                intrin_name << "_ps"; 
            } 
            else if (type.is_double()) 
            { 
                intrin_name << "_pd"; 
            } 
            else if (type.is_signed_int() ||
                    type.is_unsigned_int()) 
            { 
                intrin_name << "_epi32"; 
            } 
            else
            {
                internal_error("KNC Lowering: Node %s at %s has an unsupported type.", 
                        ast_print_node_type(node.get_kind()),
                        locus_to_str(node.get_locus()));
            }      

            walk(node.get_scalar_dst());
            walk(node.get_vector_src());

            intrin_src << as_expression(node.get_scalar_dst())
                << " += "
                << intrin_name 
                << "("
                << as_expression(node.get_vector_src())
                << ")";

            Nodecl::NodeclBase function_call = 
                    intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorReductionMinus& node) 
        {
            // OpenMP defines reduction(-:a) in the same way as reduction(+:a)
            visit(node.as<Nodecl::VectorReductionAdd>());
        }        

        void KNCVectorLowering::visit(const Nodecl::VectorMaskAssignment& node)
        {
            TL::Source intrin_src, mask_cast;

            TL::Type rhs_type = node.get_rhs().get_type();

            if(rhs_type.is_integral_type())
                mask_cast << "(" << node.get_lhs().get_type().no_ref().
                    get_simple_declaration(node.retrieve_context(), "") << ")";

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << as_expression(node.get_lhs())
                << " = "
                << "("
                << mask_cast
                << "("
                << as_expression(node.get_rhs())
                << "))"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        //TODO
        void KNCVectorLowering::visit(const Nodecl::VectorMaskConversion& node)
        {
            walk(node.get_nest());

            node.get_nest().set_type(node.get_type());

            node.replace(node.get_nest());
        }

        void KNCVectorLowering::visit(const Nodecl::VectorMaskNot& node)
        {
            TL::Source intrin_src;

            walk(node.get_rhs());

            intrin_src << "_mm512_knot("
                << as_expression(node.get_rhs())
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorMaskAnd& node)
        {
            TL::Source intrin_src;

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mm512_kand("
                << as_expression(node.get_lhs())
                << ", "
                << as_expression(node.get_rhs())
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorMaskOr& node)
        {
            TL::Source intrin_src;

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mm512_kor("
                << as_expression(node.get_lhs())
                << ", "
                << as_expression(node.get_rhs())
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorMaskAnd1Not& node)
        {
            TL::Source intrin_src;

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mm512_kandn("
                << as_expression(node.get_lhs())
                << ", "
                << as_expression(node.get_rhs())
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorMaskAnd2Not& node)
        {
            TL::Source intrin_src;

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mm512_kandnr("
                << as_expression(node.get_lhs())
                << ", "
                << as_expression(node.get_rhs())
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }

        void KNCVectorLowering::visit(const Nodecl::VectorMaskXor& node)
        {
            TL::Source intrin_src;

            walk(node.get_lhs());
            walk(node.get_rhs());

            intrin_src << "_mm512_kxor("
                << as_expression(node.get_lhs())
                << ", "
                << as_expression(node.get_rhs())
                << ")"
                ;

            Nodecl::NodeclBase function_call =
                intrin_src.parse_expression(node.retrieve_context());

            node.replace(function_call);
        }


        void KNCVectorLowering::visit(const Nodecl::MaskLiteral& node)
        {
            Nodecl::IntegerLiteral int_mask = 
                Nodecl::IntegerLiteral::make(
                        TL::Type::get_short_int_type(),
                        node.get_constant());

            node.replace(int_mask);
        }

        Nodecl::NodeclVisitor<void>::Ret KNCVectorLowering::unhandled_node(const Nodecl::NodeclBase& n) 
        { 
            internal_error("KNC Lowering: Unknown node %s at %s.",
                    ast_print_node_type(n.get_kind()),
                    locus_to_str(n.get_locus())); 

            return Ret(); 
        }
    }
}
