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

#include "cxx-utils.h"
#include "fortran03-scope.h"

#include "tl-symbol-utils.hpp"
#include "tl-nodecl-visitor.hpp"
#include "tl-scope.hpp"

namespace SymbolUtils
{

    TL::Symbol new_function_symbol(
            TL::Symbol current_function,
            const std::string& name,
            TL::Type return_type,
            TL::ObjectList<std::string> parameter_names,
            TL::ObjectList<TL::Type> parameter_types)
    {
        if (IS_FORTRAN_LANGUAGE && current_function.is_nested_function())
        {
            // Get the enclosing function
            current_function = current_function.get_scope().get_related_symbol();
        }

        decl_context_t decl_context = current_function.get_scope().get_decl_context();

        ERROR_CONDITION(parameter_names.size() != parameter_types.size(), "Mismatch between names and types", 0);

        decl_context_t function_context;
        if (IS_FORTRAN_LANGUAGE)
        {
            function_context = new_program_unit_context(decl_context);
        }
        else
        {
            function_context = new_function_context(decl_context);
            function_context = new_block_context(function_context);
        }

        // Build the function type
        int num_parameters = 0;
        scope_entry_t** parameter_list = NULL;

        parameter_info_t* p_types = new parameter_info_t[parameter_types.size()];
        parameter_info_t* it_ptypes = &(p_types[0]);
        TL::ObjectList<TL::Type>::iterator type_it = parameter_types.begin();
        for (TL::ObjectList<std::string>::iterator it = parameter_names.begin();
                it != parameter_names.end();
                it++, it_ptypes++, type_it++)
        {
            scope_entry_t* param = new_symbol(function_context, function_context.current_scope, it->c_str());
            param->entity_specs.is_user_declared = 1;
            param->kind = SK_VARIABLE;
            param->locus = make_locus("", 0, 0);

            param->defined = 1;

            param->type_information = get_unqualified_type(type_it->get_internal_type());

            P_LIST_ADD(parameter_list, num_parameters, param);

            it_ptypes->is_ellipsis = 0;
            it_ptypes->nonadjusted_type_info = NULL;
            it_ptypes->type_info = get_indirect_type(param);
        }

        type_t *function_type = get_new_function_type(
                return_type.get_internal_type(),
                p_types,
                parameter_types.size());

        delete[] p_types;

        // Now, we can create the new function symbol
        scope_entry_t* new_function_sym = NULL;
        if (!current_function.get_type().is_template_specialized_type())
        {
            new_function_sym = new_symbol(decl_context, decl_context.current_scope, name.c_str());
            new_function_sym->entity_specs.is_user_declared = 1;
            new_function_sym->kind = SK_FUNCTION;
            new_function_sym->locus = make_locus("", 0, 0);
            new_function_sym->type_information = function_type;
        }
        else
        {
            scope_entry_t* new_template_sym = new_symbol(
                    decl_context, decl_context.current_scope, name.c_str());
            new_template_sym->kind = SK_TEMPLATE;
            new_template_sym->locus = make_locus("", 0, 0);

            new_template_sym->type_information = get_new_template_type(
                    decl_context.template_parameters,
                    function_type,
                    uniquestr(name.c_str()),
                    decl_context, make_locus("", 0, 0));

            template_type_set_related_symbol(new_template_sym->type_information, new_template_sym);

            // The new function is the primary template specialization
            new_function_sym = named_type_get_symbol(
                    template_type_get_primary_type(
                        new_template_sym->type_information));
        }

        function_context.function_scope->related_entry = new_function_sym;
        function_context.block_scope->related_entry = new_function_sym;

        new_function_sym->related_decl_context = function_context;

        new_function_sym->entity_specs.related_symbols = parameter_list;
        new_function_sym->entity_specs.num_related_symbols = num_parameters;
        for (int i = 0; i < new_function_sym->entity_specs.num_related_symbols; ++i)
        {
            symbol_set_as_parameter_of_function(
                    new_function_sym->entity_specs.related_symbols[i], new_function_sym, /* parameter position */ i);
        }

        // Make it static
        new_function_sym->entity_specs.is_static = 1;

        // Make it member if the enclosing function is member
        if (current_function.is_member())
        {
            new_function_sym->entity_specs.is_member = 1;
            new_function_sym->entity_specs.class_type = current_function.get_class_type().get_internal_type();

            new_function_sym->entity_specs.access = AS_PUBLIC;

            ::class_type_add_member(new_function_sym->entity_specs.class_type, new_function_sym);
        }
        if (IS_FORTRAN_LANGUAGE && current_function.is_in_module())
        {
            scope_entry_t* module_sym = current_function.in_module().get_internal_symbol();
            new_function_sym->entity_specs.in_module = module_sym;
            P_LIST_ADD(
                    module_sym->entity_specs.related_symbols,
                    module_sym->entity_specs.num_related_symbols,
                    new_function_sym);
            new_function_sym->entity_specs.is_module_procedure = 1;
        }

        return new_function_sym;
    }

    void build_empty_body_for_function(
            TL::Symbol function_symbol,
            Nodecl::NodeclBase &function_code,
            Nodecl::NodeclBase &empty_stmt)
    {
        empty_stmt = Nodecl::EmptyStatement::make(make_locus("", 0, 0));
        Nodecl::List stmt_list = Nodecl::List::make(empty_stmt);

        if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
        {
            Nodecl::CompoundStatement compound_statement =
                Nodecl::CompoundStatement::make(stmt_list,
                        /* destructors */ Nodecl::NodeclBase::null(),
                        make_locus("", 0, 0));
            stmt_list = Nodecl::List::make(compound_statement);
        }

        Nodecl::NodeclBase context = Nodecl::Context::make(
                stmt_list,
                function_symbol.get_related_scope(), make_locus("", 0, 0));

        function_symbol.get_internal_symbol()->defined = 1;

        if (function_symbol.is_dependent_function())
        {
            function_code = Nodecl::TemplateFunctionCode::make(context,
                    // Initializers
                    Nodecl::NodeclBase::null(),
                    function_symbol,
                    make_locus("", 0, 0));
        }
        else
        {
            function_code = Nodecl::FunctionCode::make(context,
                    // Initializers
                    Nodecl::NodeclBase::null(),
                    function_symbol,
                    make_locus("", 0, 0));
        }
    }
}
