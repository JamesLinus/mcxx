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

#ifndef CXX_EXPRTYPE_H
#define CXX_EXPRTYPE_H

#include "libmcxx-common.h"
#include "cxx-ast-decls.h"
#include "cxx-scope-decls.h"
#include "cxx-buildscope-decls.h"
#include "cxx-typeutils.h"

MCXX_BEGIN_DECLS

/*
 * Computes the type of an expression
 *
 * Only implemented for C
 *
 */
LIBMCXX_EXTERN struct type_tag* decimal_literal_type(AST expr);
LIBMCXX_EXTERN struct type_tag* character_literal_type(AST expr);
LIBMCXX_EXTERN struct type_tag* floating_literal_type(AST expr);
LIBMCXX_EXTERN struct type_tag* string_literal_type(AST expr);

LIBMCXX_EXTERN struct type_tag *compute_expression_type(AST expr, decl_context_t decl_context, 
        char *is_lvalue) __attribute__((deprecated));

LIBMCXX_EXTERN AST advance_expression_nest(AST expr);
LIBMCXX_EXTERN AST advance_expression_nest_flags(AST expr, char advance_parentheses);

LIBMCXX_EXTERN char can_be_called_with_number_of_arguments(scope_entry_t *entry, int num_arguments);

LIBMCXX_EXTERN char check_for_expression(AST a, decl_context_t decl_context);

LIBMCXX_EXTERN char check_for_expression_list(AST expression_list, decl_context_t decl_context);

LIBMCXX_EXTERN char check_for_initialization(AST initializer, decl_context_t decl_context, type_t* declared_type);
LIBMCXX_EXTERN char check_for_initializer_clause(AST initializer, decl_context_t decl_context, type_t* declared_type);

LIBMCXX_EXTERN char check_zero_args_constructor(type_t* class_type, decl_context_t decl_context, AST declarator);

LIBMCXX_EXTERN unsigned long long exprtype_used_memory(void);

LIBMCXX_EXTERN scope_entry_list_t* unfold_and_mix_candidate_functions(
        scope_entry_list_t* result_from_lookup,
        scope_entry_list_t* builtin_list,
        type_t** argument_types,
        int num_arguments,
        decl_context_t decl_context,
        const char *filename,
        int line,
        template_argument_list_t *explicit_template_arguments
        );

LIBMCXX_EXTERN void set_as_template_parameter_name(AST a, scope_entry_t* template_param_sym);
LIBMCXX_EXTERN char is_template_parameter_name(AST a);
LIBMCXX_EXTERN scope_entry_t* lookup_template_parameter_name(decl_context_t decl_context, AST a);

MCXX_END_DECLS

#endif
