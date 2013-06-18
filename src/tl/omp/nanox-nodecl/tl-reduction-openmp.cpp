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


#include "tl-lowering-visitor.hpp"
#include "tl-outline-info.hpp"
#include "tl-predicateutils.hpp"
#include "tl-nanos.hpp"
#include "cxx-diagnostic.h"
#include "fortran03-typeutils.h"

namespace TL { namespace Nanox {

    struct ExpandVisitor : public Nodecl::ExhaustiveVisitor<void>
    {
        private:
            TL::Symbol _orig_omp_in, _new_omp_in, _orig_omp_out, _new_omp_out;
            TL::Symbol _index;
        public:
        ExpandVisitor(
                TL::Symbol orig_omp_in, 
                TL::Symbol new_omp_in,
                TL::Symbol orig_omp_out, 
                TL::Symbol new_omp_out,
                TL::Symbol index)
            : _orig_omp_in(orig_omp_in),
            _new_omp_in(new_omp_in),
            _orig_omp_out(orig_omp_out),
            _new_omp_out(new_omp_out),
            _index(index)
        {
        }

        void visit(const Nodecl::Symbol &node)
        {
            bool must_expand = false;
            TL::Symbol sym = node.get_symbol();
            TL::Symbol new_sym;
            if (sym == _orig_omp_in)
            {
                must_expand = true;
                new_sym = _new_omp_in;
            }
            else if (sym == _orig_omp_out)
            {
                must_expand = true;
                new_sym = _new_omp_out;
            }

            if (!must_expand)
                return;

            Nodecl::NodeclBase new_sym_ref = Nodecl::Symbol::make(new_sym);
            new_sym_ref.set_type(new_sym.get_type());

            Nodecl::NodeclBase index_ref = Nodecl::Symbol::make(_index);
            index_ref.set_type(_index.get_type().get_lvalue_reference_to());

            TL::Type new_type = new_sym.get_type().no_ref();
            if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
            {
                new_type = new_type.points_to().get_lvalue_reference_to();
            }
            else if (IS_FORTRAN_LANGUAGE)
            {
                new_type = new_type.array_element().get_lvalue_reference_to();
            }
            else
            {
                internal_error("Code unreachable", 0);
            }

            node.replace(
                    Nodecl::ArraySubscript::make(
                        new_sym_ref,
                        Nodecl::List::make(
                            index_ref),
                        new_type
                        )
                    );
        }
    };

    TL::Symbol LoweringVisitor::create_basic_reduction_function_c(OpenMP::Reduction* red, Nodecl::NodeclBase construct)
    {
        reduction_map_t::iterator it = _reduction_map_openmp.find(red);
        if (it != _reduction_map_openmp.end())
        {
            return it->second;
        }

        std::string fun_name;
        {
            std::stringstream ss;
            ss << "nanos_red_" << red << "_" << simple_hash_str(construct.get_filename().c_str());
            fun_name = ss.str();
        }

        Nodecl::NodeclBase function_body;
        Source src;
        src << "void " << fun_name << "(" 
            << as_type(red->get_type()) << "* omp_out, "
            << as_type(red->get_type()) << "* omp_in, "
            << "int num_scalars )"
            << "{"
            <<    "int i;"
            <<    "for (i = 0; i < num_scalars; i++)"
            <<    "{"
            <<    statement_placeholder(function_body)
            <<    "}"
            << "}"
            ;

        FORTRAN_LANGUAGE()
        {
            Source::source_language = SourceLanguage::C;
        }
        Nodecl::NodeclBase function_code = src.parse_global(construct);
        FORTRAN_LANGUAGE()
        {
            Source::source_language = SourceLanguage::Current;
        }

        TL::Scope inside_function = ReferenceScope(function_body).get_scope();
        TL::Symbol param_omp_in = inside_function.get_symbol_from_name("omp_in");
        ERROR_CONDITION(!param_omp_in.is_valid(), "Symbol omp_in not found", 0);
        TL::Symbol param_omp_out = inside_function.get_symbol_from_name("omp_out");
        ERROR_CONDITION(!param_omp_out.is_valid(), "Symbol omp_out not found", 0);

        TL::Symbol function_sym = inside_function.get_symbol_from_name(fun_name);
        ERROR_CONDITION(!function_sym.is_valid(), "Symbol %s not found", fun_name.c_str());

        TL::Symbol index = inside_function.get_symbol_from_name("i");
        ERROR_CONDITION(!index.is_valid(), "Symbol %s not found", "i");
        TL::Symbol num_scalars = inside_function.get_symbol_from_name("num_scalars");
        ERROR_CONDITION(!num_scalars.is_valid(), "Symbol %s not found", "num_scalars");

        Nodecl::NodeclBase expanded_combiner =
            red->get_combiner().shallow_copy();
        ExpandVisitor expander_visitor(
                red->get_omp_in(),
                param_omp_in,
                red->get_omp_out(),
                param_omp_out,
                index);
        expander_visitor.walk(expanded_combiner);

        function_body.replace(
                Nodecl::List::make(Nodecl::ExpressionStatement::make(expanded_combiner)));

        _reduction_map_openmp[red] = function_sym;

        Nodecl::Utils::append_to_enclosing_top_level_location(construct, function_code);

        return function_sym;
    }

    TL::Symbol LoweringVisitor::create_vector_reduction_function_c(OpenMP::Reduction* red, Nodecl::NodeclBase construct)
    {
        reduction_map_t::iterator it = _reduction_map_openmp.find(red);
        if (it != _reduction_map_openmp.end())
        {
            return it->second;
        }

        std::string fun_name;
        {
            std::stringstream ss;
            ss << "nanos_v_red_" << red << "_" << simple_hash_str(construct.get_filename().c_str());
            fun_name = ss.str();
        }

        Nodecl::NodeclBase function_body;
        Source src;
        src << "void " << fun_name << "(" 
            <<    "int size,"
            <<    "void *v_original,"
            <<    "void *v_privates)"
            << "{"
            <<    as_type(red->get_type().get_pointer_to())
            <<      " original = (" << as_type(red->get_type().get_pointer_to()) << ")v_original;"
            <<    as_type(red->get_type().get_pointer_to())
            <<      " privates = (" << as_type(red->get_type().get_pointer_to()) << ")v_privates;"
            <<    "int num_elements = size / sizeof(" << as_type(red->get_type()) << ");"
            <<    "int i;"
            <<    "for (i = 0; i < num_elements; i++)"
            <<    "{"
            <<           statement_placeholder(function_body)
            <<    "}"
            << "}"
            ;

        // FORTRAN_LANGUAGE()
        // {
        //     Source::source_language = SourceLanguage::C;
        // }
        // Nodecl::NodeclBase function_code = src.parse_global(construct);
        // FORTRAN_LANGUAGE()
        // {
        //     Source::source_language = SourceLanguage::Current;
        // }

        // TL::Scope inside_function = ReferenceScope(function_body).get_scope();
        // TL::Symbol param_omp_in = inside_function.get_symbol_from_name("omp_in");
        // ERROR_CONDITION(!param_omp_in.is_valid(), "Symbol omp_in not found", 0);
        // TL::Symbol param_omp_out = inside_function.get_symbol_from_name("omp_out");
        // ERROR_CONDITION(!param_omp_out.is_valid(), "Symbol omp_out not found", 0);

        // TL::Symbol function_sym = inside_function.get_symbol_from_name(fun_name);
        // ERROR_CONDITION(!function_sym.is_valid(), "Symbol %s not found", fun_name.c_str());

        // TL::Symbol index = inside_function.get_symbol_from_name("i");
        // ERROR_CONDITION(!index.is_valid(), "Symbol %s not found", "i");
        // TL::Symbol num_scalars = inside_function.get_symbol_from_name("num_scalars");
        // ERROR_CONDITION(!num_scalars.is_valid(), "Symbol %s not found", "num_scalars");

        // Nodecl::NodeclBase expanded_combiner =
        //     red->get_combiner().shallow_copy();
        // ExpandVisitor expander_visitor(
        //         red->get_omp_in(),
        //         param_omp_in,
        //         red->get_omp_out(),
        //         param_omp_out,
        //         index);
        // expander_visitor.walk(expanded_combiner);

        // function_body.replace(
        //         Nodecl::List::make(Nodecl::ExpressionStatement::make(expanded_combiner)));

        // _reduction_map_openmp[red] = function_sym;

        // Nodecl::Utils::append_to_enclosing_top_level_location(construct, function_code);
        TL::Symbol function_sym;

        return function_sym;
    }

    TL::Symbol LoweringVisitor::create_basic_reduction_function_fortran(OpenMP::Reduction* red, Nodecl::NodeclBase construct)
    {
        reduction_map_t::iterator it = _reduction_map_openmp.find(red);
        if (it != _reduction_map_openmp.end())
        {
            return it->second;
        }

        std::string fun_name;
        {
            std::stringstream ss;
            ss << "nanos_red_" << red << "_" << simple_hash_str(construct.get_filename().c_str());
            fun_name = ss.str();
        }

        Nodecl::NodeclBase function_body;
        Source src;

        src << "SUBROUTINE " << fun_name << "(omp_out, omp_in, num_scalars)\n"
            <<    "IMPLICIT NONE\n"
            <<    as_type(red->get_type()) << " :: omp_out(num_scalars)\n" 
            <<    as_type(red->get_type()) << " :: omp_in(num_scalars)\n"
            <<    "INTEGER, VALUE :: num_scalars\n"
            <<    "INTEGER :: I\n"
            <<    statement_placeholder(function_body) << "\n"
            << "END SUBROUTINE " << fun_name << "\n";
        ;

        Nodecl::NodeclBase function_code = src.parse_global(construct);

        TL::Scope inside_function = ReferenceScope(function_body).get_scope();
        TL::Symbol param_omp_in = inside_function.get_symbol_from_name("omp_in");
        ERROR_CONDITION(!param_omp_in.is_valid(), "Symbol omp_in not found", 0);
        TL::Symbol param_omp_out = inside_function.get_symbol_from_name("omp_out");
        ERROR_CONDITION(!param_omp_out.is_valid(), "Symbol omp_out not found", 0);

        TL::Symbol function_sym = inside_function.get_symbol_from_name(fun_name);
        ERROR_CONDITION(!function_sym.is_valid(), "Symbol %s not found", fun_name.c_str());

        TL::Symbol index = inside_function.get_symbol_from_name("i");
        ERROR_CONDITION(!index.is_valid(), "Symbol %s not found", "i");
        TL::Symbol num_scalars = inside_function.get_symbol_from_name("num_scalars");
        ERROR_CONDITION(!num_scalars.is_valid(), "Symbol %s not found", "num_scalars");

        Nodecl::NodeclBase num_scalars_ref = Nodecl::Symbol::make(num_scalars);

        num_scalars_ref.set_type(num_scalars.get_type().no_ref().get_lvalue_reference_to());

        Nodecl::Symbol nodecl_index = Nodecl::Symbol::make(index);
        nodecl_index.set_type(index.get_type().get_lvalue_reference_to());

        Nodecl::NodeclBase loop_header = Nodecl::RangeLoopControl::make(
                nodecl_index,
                const_value_to_nodecl(const_value_get_signed_int(1)),
                num_scalars_ref,
                Nodecl::NodeclBase::null());

        Nodecl::NodeclBase expanded_combiner =
            red->get_combiner().shallow_copy();
        ExpandVisitor expander_visitor(
                red->get_omp_in(),
                param_omp_in,
                red->get_omp_out(),
                param_omp_out,
                index);
        expander_visitor.walk(expanded_combiner);

        function_body.replace(
                Nodecl::ForStatement::make(loop_header,
                    Nodecl::List::make(
                        Nodecl::ExpressionStatement::make(
                            expanded_combiner)),
                    Nodecl::NodeclBase::null()));

        _reduction_map_openmp[red] = function_sym;

        Nodecl::Utils::append_to_enclosing_top_level_location(construct, function_code);

        return function_sym;
    }

    void LoweringVisitor::create_reduction_function(OpenMP::Reduction* red,
            Nodecl::NodeclBase construct,
            TL::Symbol& basic_reduction_function,
            TL::Symbol& vector_reduction_function)
    {
        if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
        {
            basic_reduction_function = create_basic_reduction_function_c(red, construct);
        }
        else if (IS_FORTRAN_LANGUAGE)
        {
            basic_reduction_function = create_basic_reduction_function_fortran(red, construct);
        }
        else
        {
            internal_error("Code unreachable", 0);
        }
    }

    TL::Symbol LoweringVisitor::create_reduction_cleanup_function(OpenMP::Reduction* red, Nodecl::NodeclBase construct)
    {
        if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
        {
            internal_error("Currently only valid in Fortran", 0);
        }
        reduction_map_t::iterator it = _reduction_cleanup_map.find(red);
        if (it != _reduction_cleanup_map.end())
        {
            return it->second;
        }

        std::string fun_name;
        {
            std::stringstream ss;
            ss << "nanos_cleanup_" << red << "_" << simple_hash_str(construct.get_filename().c_str());
            fun_name = ss.str();
        }


        Source src;
        src << "SUBROUTINE " << fun_name << "(X)\n"
            <<     as_type(red->get_type()) << ", POINTER ::  X(:)\n"
            <<     "DEALLOCATE(X)\n"
            << "END SUBROUTINE\n"
            ;

        Nodecl::NodeclBase function_code = src.parse_global(construct);

        TL::Symbol function_sym = ReferenceScope(construct).get_scope().get_symbol_from_name(fun_name);
        ERROR_CONDITION(!function_sym.is_valid(), "Symbol %s not found", fun_name.c_str());

        _reduction_cleanup_map[red] = function_sym;

        Nodecl::Utils::append_to_enclosing_top_level_location(construct, function_code);

        return function_sym;
    }

    bool LoweringVisitor::there_are_reductions(OutlineInfo& outline_info)
    {
        TL::ObjectList<OutlineDataItem*> reduction_items = outline_info.get_data_items().filter(
                predicate(lift_pointer(functor(&OutlineDataItem::is_reduction))));
        return !reduction_items.empty();
    }

    void LoweringVisitor::reduction_initialization_code(
            OutlineInfo& outline_info,
            Nodecl::NodeclBase ref_tree,
            Nodecl::NodeclBase construct)
    {
        ERROR_CONDITION(ref_tree.is_null(), "Invalid tree", 0);

        if (!Nanos::Version::interface_is_at_least("master", 5023))
        {
            running_error("%s: error: a newer version of Nanos++ (>=5023) is required for reductions support\n",
                    construct.get_locus_str().c_str());
        }

        TL::ObjectList<OutlineDataItem*> reduction_items = outline_info.get_data_items().filter(
                predicate(lift_pointer(functor(&OutlineDataItem::is_reduction))));
        ERROR_CONDITION (reduction_items.empty(), "No reductions to process", 0);

        Source result;

        Source reduction_declaration,
               thread_initializing_reduction_info,
               thread_fetching_reduction_info;

        result
            << reduction_declaration
            << "{"
            << as_type(get_bool_type()) << " red_single_guard;"
            << "nanos_err_t err;"
            << "err = nanos_enter_sync_init(&red_single_guard);"
            << "if (err != NANOS_OK)"
            <<     "nanos_handle_error(err);"
            << "if (red_single_guard)"
            << "{"
            <<    "int nanos_num_threads = nanos_omp_get_num_threads();"
            <<    thread_initializing_reduction_info
            <<    "err = nanos_release_sync_init();"
            <<    "if (err != NANOS_OK)"
            <<        "nanos_handle_error(err);"
            << "}"
            << "else"
            << "{"
            <<    "err = nanos_wait_sync_init();"
            <<    "if (err != NANOS_OK)"
            <<        "nanos_handle_error(err);"
            <<    thread_fetching_reduction_info
            << "}"
            << "}"
            ;

        for (TL::ObjectList<OutlineDataItem*>::iterator it = reduction_items.begin();
                it != reduction_items.end();
                it++)
        {
            std::string nanos_red_name = "nanos_red_" + (*it)->get_symbol().get_name();

            OpenMP::Reduction *reduction = (*it)->get_reduction_info();
            ERROR_CONDITION(reduction == NULL, "Invalid reduction info", 0);

            TL::Type reduction_type = (*it)->get_symbol().get_type();
            if (reduction_type.is_any_reference())
                reduction_type = reduction_type.references_to();
            Source element_size;
            if (IS_FORTRAN_LANGUAGE
                    && reduction_type.is_fortran_array())
            {
                while (reduction_type.is_fortran_array())
                    reduction_type = reduction_type.array_element();

                // We need to parse this bit in Fortran
                Source number_of_bytes;
                number_of_bytes << "SIZE(" << (*it)->get_symbol().get_name() << ") * " << reduction_type.get_size();

                element_size << as_expression(number_of_bytes.parse_expression(construct));
            }
            else
            {
                element_size << "sizeof(" << as_type(reduction_type) << ")";
            }

            reduction_declaration
                << "nanos_reduction_t* " << nanos_red_name << ";"
                ;

            Source allocate_private_buffer, cleanup_code;

            Source num_scalars;

            TL::Symbol basic_reduction_function, vector_reduction_function;
            create_reduction_function(reduction, construct, basic_reduction_function, vector_reduction_function);
            (*it)->reduction_set_basic_function(basic_reduction_function);

            thread_initializing_reduction_info
                << "err = nanos_malloc((void**)&" << nanos_red_name << ", sizeof(nanos_reduction_t), " 
                << "\"" << construct.get_filename() << "\", " << construct.get_line() << ");"
                << "if (err != NANOS_OK)"
                <<     "nanos_handle_error(err);"
                << nanos_red_name << "->original = (void*)&" << (*it)->get_symbol().get_name() << ";"
                << allocate_private_buffer
                << nanos_red_name << "->vop = 0;"
                << nanos_red_name << "->bop = (void(*)(void*,void*,int))" << as_symbol(basic_reduction_function) << ";"
                << nanos_red_name << "->element_size = " << element_size << ";"
                << nanos_red_name << "->num_scalars = " << num_scalars << ";"
                << cleanup_code
                << "err = nanos_register_reduction(" << nanos_red_name << ");"
                << "if (err != NANOS_OK)"
                <<     "nanos_handle_error(err);"
                ;

            if (IS_C_LANGUAGE
                    || IS_CXX_LANGUAGE)
            {
                // No array reductions are possible in C/C++
                num_scalars << "1";
                allocate_private_buffer
                    << "err = nanos_malloc(&" << nanos_red_name << "->privates, sizeof(" << as_type(reduction_type) << ") * nanos_num_threads, "
                    << "\"" << construct.get_filename() << "\", " << construct.get_line() << ");"
                    << "if (err != NANOS_OK)"
                    <<     "nanos_handle_error(err);"
                    << nanos_red_name << "->descriptor = " << nanos_red_name << "->privates;"
                    << "rdv_" << (*it)->get_field_name() << " = (" <<  as_type( (*it)->get_field_type() ) << ")" << nanos_red_name << "->privates;"
                    ;


                thread_fetching_reduction_info
                    << "err = nanos_reduction_get(&" << nanos_red_name << ", &" << (*it)->get_symbol().get_name() << ");"
                    << "if (err != NANOS_OK)"
                    <<     "nanos_handle_error(err);"
                    << "rdv_" << (*it)->get_field_name() << " = (" <<  as_type( (*it)->get_field_type() ) << ")" << nanos_red_name << "->privates;"
                    ;
                cleanup_code
                    << nanos_red_name << "->cleanup = nanos_free0;"
                    ;
            }
            else if (IS_FORTRAN_LANGUAGE)
            {

                Type private_reduction_vector_type;

                Source extra_dims;
                {
                    TL::Type t = (*it)->get_symbol().get_type().no_ref();
                    int rank = 0;
                    if (t.is_fortran_array())
                    {
                        rank = t.fortran_rank();
                    }

                    if (rank != 0)
                    {
                        // We need to parse this bit in Fortran
                        Source size_call;
                        size_call << "SIZE(" << (*it)->get_symbol().get_name() << ")";

                        num_scalars << as_expression(size_call.parse_expression(construct));
                    }
                    else
                    {
                        num_scalars << "1";
                    }
                    private_reduction_vector_type = fortran_get_n_ranked_type_with_descriptor(
                            get_void_type(), rank + 1, construct.retrieve_context().get_decl_context());

                    int i;
                    for (i = 0; i < rank; i++)
                    {
                        Source lbound_src;
                        lbound_src << "LBOUND(" << (*it)->get_symbol().get_name() << ", DIM = " << (rank - i) << ")";
                        Source ubound_src;
                        ubound_src << "UBOUND(" << (*it)->get_symbol().get_name() << ", DIM = " << (rank - i) << ")";

                        extra_dims 
                            << "["
                            << as_expression(lbound_src.parse_expression(construct))
                            << ":"
                            << as_expression(ubound_src.parse_expression(construct))
                            << "]";

                        t = t.array_element();
                    }
                }

                allocate_private_buffer
                    << "@FORTRAN_ALLOCATE@((*rdv_" << (*it)->get_field_name() << ")[0:(nanos_num_threads-1)]" << extra_dims <<");"
                    << nanos_red_name << "->privates = &(*rdv_" << (*it)->get_field_name() << ");"
                    << "err = nanos_malloc(&" << nanos_red_name << "->descriptor, sizeof(" << as_type(private_reduction_vector_type) << "), "
                    << "\"" << construct.get_filename() << "\", " << construct.get_line() << ");"
                    << "if (err != NANOS_OK)"
                    <<     "nanos_handle_error(err);"
                    << "err = nanos_memcpy(" << nanos_red_name << "->descriptor, "
                    "&rdv_" << (*it)->get_field_name() << ", sizeof(" << as_type(private_reduction_vector_type) << "));"
                    << "if (err != NANOS_OK)"
                    <<     "nanos_handle_error(err);"
                    ;

                thread_fetching_reduction_info
                    << "err = nanos_reduction_get(&" << nanos_red_name << ", &" << (*it)->get_symbol().get_name() << ");"
                    << "if (err != NANOS_OK)"
                    <<     "nanos_handle_error(err);"
                    << "err = nanos_memcpy("
                    << "&rdv_" << (*it)->get_field_name() << ","
                    << nanos_red_name << "->descriptor, "
                    << "sizeof(" << as_type(private_reduction_vector_type) << "));"
                    << "if (err != NANOS_OK)"
                    <<     "nanos_handle_error(err);"
                    ;

                TL::Symbol reduction_cleanup = create_reduction_cleanup_function(reduction, construct);
                cleanup_code
                    << nanos_red_name << "->cleanup = " << as_symbol(reduction_cleanup) << ";"
                    ;
            }
            else
            {
                internal_error("Code unreachable", 0);
            }
        }

        FORTRAN_LANGUAGE()
        {
            Source::source_language = SourceLanguage::C;
        }
        ref_tree.replace(result.parse_statement(ref_tree));
        FORTRAN_LANGUAGE()
        {
            Source::source_language = SourceLanguage::Current;
        }
    }

    void LoweringVisitor::perform_partial_reduction(OutlineInfo& outline_info, Nodecl::NodeclBase ref_tree)
    {
        ERROR_CONDITION(ref_tree.is_null(), "Invalid tree", 0);

        Source reduction_code;

        TL::ObjectList<OutlineDataItem*> reduction_items = outline_info.get_data_items().filter(
                predicate(lift_pointer(functor(&OutlineDataItem::is_reduction))));
        if (!reduction_items.empty())
        {
            for (TL::ObjectList<OutlineDataItem*>::iterator it = reduction_items.begin();
                    it != reduction_items.end();
                    it++)
            {
                if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
                {
                    reduction_code
                        << "rdv_" << (*it)->get_field_name() << "[nanos_omp_get_thread_num()] = rdp_" << (*it)->get_symbol().get_name() << ";"
                        ;
                }
                else if (IS_FORTRAN_LANGUAGE)
                {
                    Source extra_dims;
                    {
                        TL::Type t = (*it)->get_symbol().get_type().no_ref();
                        int rank = 0;
                        if (t.is_fortran_array())
                        {
                            rank = t.fortran_rank();
                        }

                        int i;
                        for (i = 0; i < rank; i++)
                        {
                            extra_dims << ":,";
                        }
                    }

                    reduction_code
                        << "rdv_" << (*it)->get_field_name() << "( " << extra_dims << "nanos_omp_get_thread_num() ) = rdp_" << (*it)->get_symbol().get_name() << "\n"
                        ;
                }
                else
                {
                    internal_error("Code unreachable", 0);
                }
            }
        }

        ref_tree.replace(reduction_code.parse_statement(ref_tree));
    }

} }
