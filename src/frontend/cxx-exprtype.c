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




#include "cxx-exprtype.h"
#include "cxx-ambiguity.h"
#include "cxx-utils.h"
#include "cxx-typeutils.h"
#include "cxx-typededuc.h"
#include "cxx-koenig.h"
#include "cxx-tltype.h"
#include "cxx-ambiguity.h"
#include "cxx-overload.h"
#include "cxx-prettyprint.h"
#include "cxx-instantiation.h"
#include "cxx-buildscope.h"
#include "cxx-cexpr.h"
#include "cxx-typeenviron.h"
#include "cxx-gccsupport.h"
#include "cxx-gccbuiltins.h"
#include "cxx-cuda.h"
#include "cxx-entrylist.h"
#include "cxx-limits.h"
#include "cxx-diagnostic.h"
#include "cxx-codegen.h"
#include "cxx-instantiation.h"
#include <ctype.h>
#include <string.h>

#include <math.h>
#include <errno.h>

#include <iconv.h>


#include "fortran/fortran03-exprtype.h"

typedef
struct builtin_operators_set_tag
{
    scope_entry_list_t *entry_list;
    scope_entry_t entry[MCXX_MAX_BUILTINS_IN_OVERLOAD];
    int num_builtins;
} builtin_operators_set_t;

enum must_be_constant_t
{
    MUST_NOT_BE_CONSTANT = 0,
    MUST_BE_CONSTANT,
    MUST_BE_NONTYPE_TEMPLATE_PARAMETER,
};

// This structure contains information about the context of the current expression
typedef
struct check_expr_flags_tag
{
    /*
     * Diagnoses if the expression is not constant
     * according to rules in §5.19
     */
    enum must_be_constant_t must_be_constant:2;
    /*
     * We do not evaluate constexpr calls
     * for nonstrict operators like ?, && and ||
     */
    char do_not_call_constexpr:1;
    /* It will be true if the expression is non_executable.
     * Examples of non-executable expressions contains:
     * sizeof, alignof, typeof, decltype
     *
     * Otherwise it will be false.
     */
    char is_non_executable:1;
} check_expr_flags_t;

static check_expr_flags_t check_expr_flags =
{
    .must_be_constant = 0,
    .do_not_call_constexpr = 0,
    .is_non_executable = 0,
};

char builtin_needs_contextual_conversion(scope_entry_t* candidate,
        int num_arg, type_t* parameter_type)
{
    if (!symbol_entity_specs_get_is_builtin(candidate))
        return 0;

    const char *operator_or = UNIQUESTR_LITERAL(STR_OPERATOR_LOGIC_OR);
    const char *operator_and = UNIQUESTR_LITERAL(STR_OPERATOR_LOGIC_AND);
    const char *operator_not = UNIQUESTR_LITERAL(STR_OPERATOR_LOGIC_NOT);
    const char *operator_ternary = UNIQUESTR_LITERAL("operator ?");

    if (candidate->symbol_name == operator_or
            || candidate->symbol_name == operator_and)
    {
        if (num_arg != 0 && num_arg != 1)
            return 0;
    }
    else if (candidate->symbol_name == operator_not
            || candidate->symbol_name == operator_ternary)
    {
        if (num_arg != 0)
            return 0;
    }

    // Sanity check
    if (!is_bool_type(parameter_type))
        return 0;

    return 1;
}

static
void build_unary_builtin_operators(type_t* t1,
        builtin_operators_set_t *result,
        decl_context_t decl_context, AST operator, 
        char (*property)(type_t*, const locus_t*),
        type_t* (*result_type)(type_t**, const locus_t*),
        const locus_t* locus);

static
void build_binary_builtin_operators(type_t* t1, 
        type_t* t2, 
        builtin_operators_set_t *result,
        decl_context_t decl_context, AST operator, 
        char (*property)(type_t*, type_t*, const locus_t*),
        type_t* (*result_type)(type_t**, type_t**, const locus_t*),
        const locus_t* locus);

static
void build_ternary_builtin_operators(type_t* t1, 
        type_t* t2, 
        type_t* t3, 
        builtin_operators_set_t *result,
        decl_context_t decl_context,
        const char* operator_name, 
        char (*property)(type_t*, type_t*, type_t*, const locus_t*),
        type_t* (*result_type)(type_t**, type_t**, type_t**, const locus_t*),
        const locus_t* locus);

scope_entry_list_t* get_entry_list_from_builtin_operator_set(builtin_operators_set_t* builtin_operators)
{
    if (builtin_operators->num_builtins == 0)
        return NULL;
    else 
        return (builtin_operators->entry_list);
}


// declared_type is used only to cv-qualify constructors
static type_t* actual_type_of_conversor(scope_entry_t* conv)
{
    conv = entry_advance_aliases(conv);

    if (symbol_entity_specs_get_is_constructor(conv))
    {
        return symbol_entity_specs_get_class_type(conv);
    }
    else if (symbol_entity_specs_get_is_conversion(conv))
    {
        return function_type_get_return_type(conv->type_information);
    }
    else
    {
        internal_error("Invalid conversion function!", 0);
    }
}

static
scope_entry_t* expand_template_given_arguments(scope_entry_t* template_sym,
        type_t** argument_types, int num_arguments, 
        decl_context_t decl_context,
        const locus_t* locus,
        template_parameter_list_t* explicit_template_arguments)
{
    // We have to expand the template
    template_parameter_list_t* type_template_parameters 
        = template_type_get_template_parameters(template_sym->type_information);
    type_t* primary_type = template_type_get_primary_type(template_sym->type_information);
    scope_entry_t* primary_symbol = named_type_get_symbol(primary_type);
    type_t* specialized_function_type = primary_symbol->type_information;

    template_parameter_list_t* template_parameters = 
        template_specialized_type_get_template_arguments(specialized_function_type);

    template_parameter_list_t* argument_list = NULL;

    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Attempting to deduce template arguments for '%s' (declared in '%s')\n",
                print_decl_type_str(primary_symbol->type_information, primary_symbol->decl_context,
                    get_qualified_symbol_name(primary_symbol, primary_symbol->decl_context)),
                locus_to_str(primary_symbol->locus));
    }

    if (deduce_template_arguments_from_function_call(
                argument_types,
                num_arguments,
                primary_type,
                template_parameters,
                type_template_parameters,
                explicit_template_arguments,
                decl_context,
                locus,
                // out
                &argument_list) == DEDUCTION_OK)
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Deduction succeeded for '%s' (declared in '%s')\n",
                    print_decl_type_str(primary_symbol->type_information, primary_symbol->decl_context,
                        get_qualified_symbol_name(primary_symbol, primary_symbol->decl_context)),
                    locus_to_str(primary_symbol->locus));
        }
        // Now get a specialized template type for this
        // function (this will sign it in if it does not exist)
        type_t* named_specialization_type = template_type_get_specialized_type(template_sym->type_information,
                argument_list, decl_context, locus);
        free_template_parameter_list(argument_list);

        if (named_specialization_type == NULL)
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Substitution failed for '%s' (declared in '%s')\n",
                        print_decl_type_str(primary_symbol->type_information, primary_symbol->decl_context,
                            get_qualified_symbol_name(primary_symbol, primary_symbol->decl_context)),
                        locus_to_str(primary_symbol->locus));
            }
            return NULL;
        }

        scope_entry_t* specialized_symbol = named_type_get_symbol(named_specialization_type);

        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Got specialization '%s' at '%s'\n", 
                    print_decl_type_str(specialized_symbol->type_information, specialized_symbol->decl_context,
                        get_qualified_symbol_name(specialized_symbol, specialized_symbol->decl_context)),
                    locus_to_str(specialized_symbol->locus));
        }

        return specialized_symbol;
    }
    else
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Discarding symbol '%s' declared at '%s' as its arguments could not be deduced.\n",
                    print_decl_type_str(primary_symbol->type_information, primary_symbol->decl_context,
                        get_qualified_symbol_name(primary_symbol, primary_symbol->decl_context)),
                    locus_to_str(primary_symbol->locus));
        }
    }
    return NULL;
}

scope_entry_t* expand_template_function_given_template_arguments(
        scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus,
        template_parameter_list_t* explicit_template_arguments)
{
    return expand_template_given_arguments(
            entry,
            NULL, 0,
            decl_context,
            locus,
            explicit_template_arguments);
}


type_t* compute_type_for_type_id_tree(AST type_id,
        decl_context_t decl_context,
        // Out
        type_t** out_simple_type,
        gather_decl_spec_t *out_gather_info)
{
    if (out_simple_type != NULL)
        *out_simple_type = NULL;

    AST type_specifier = ASTSon0(type_id);
    AST abstract_declarator = ASTSon1(type_id);

    gather_decl_spec_t gather_info;
    memset(&gather_info, 0, sizeof(gather_info));

    nodecl_t dummy_nodecl_output = nodecl_null();

    type_t* simple_type_info = NULL;
    build_scope_decl_specifier_seq(type_specifier, &gather_info, &simple_type_info, decl_context,
            &dummy_nodecl_output);

    type_t* declarator_type = simple_type_info;

    if (!is_error_type(declarator_type))
    {
        if (out_simple_type != NULL)
            *out_simple_type = simple_type_info;

        compute_declarator_type(abstract_declarator,
                &gather_info, simple_type_info,
                &declarator_type, decl_context,
                &dummy_nodecl_output);
    }

    if (out_gather_info != NULL)
        *out_gather_info = gather_info;

    return declarator_type;
}


scope_entry_list_t* unfold_and_mix_candidate_functions(
        scope_entry_list_t* result_from_lookup,
        scope_entry_list_t* builtin_list,
        type_t** argument_types,
        int num_arguments,
        decl_context_t decl_context,
        const locus_t* locus,
        template_parameter_list_t *explicit_template_arguments
        )
{
    scope_entry_list_t* overload_set = NULL;

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(result_from_lookup);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* entry = entry_list_iterator_current(it);
        scope_entry_t *orig_entry = entry;

        entry = entry_advance_aliases(entry);

        if (entry->kind == SK_TEMPLATE)
        {
            scope_entry_t* specialized_symbol = expand_template_given_arguments(entry,
                    argument_types, num_arguments, decl_context, locus,
                    explicit_template_arguments);

            if (specialized_symbol != NULL)
            {
                overload_set = entry_list_add(overload_set, specialized_symbol);
            }
        }
        else if (entry->kind == SK_FUNCTION)
        {
            overload_set = entry_list_add(overload_set, orig_entry);
        }
    }
    entry_list_iterator_free(it);
    
    // Add builtins but only if their signature is not already in the overload
    // set
    for (it = entry_list_iterator_begin(builtin_list);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* builtin = entry_list_iterator_current(it);
        scope_entry_list_iterator_t* it2 = NULL;
        char found = 0;
        for (it2 = entry_list_iterator_begin(overload_set);
                !entry_list_iterator_end(it2) && !found;
                entry_list_iterator_next(it2))
        {
            scope_entry_t* ovl = entry_advance_aliases(entry_list_iterator_current(it2));

            found = equivalent_types(ovl->type_information, builtin->type_information);
        }
        entry_list_iterator_free(it2);

        if (!found)
        {
            overload_set = entry_list_add(overload_set, builtin);
        }
    }
    entry_list_iterator_free(it);

    return overload_set;
}

static void print_field_path(field_path_t* field_path)
{
    if (field_path != NULL)
    {
        fprintf(stderr, "EXPRTYPE: Field path: ");
        if (field_path->length == 0)
        {
            fprintf(stderr, "<<empty>>");
        }
        else
        {
            int i;
            for (i = 0; i < field_path->length; i++)
            {
                if (i > 0)
                    fprintf(stderr, " => ");
                fprintf(stderr, "%s", field_path->path[i]->symbol_name);
            }
        }
        fprintf(stderr, "\n");
    }
}

static
scope_entry_list_t* get_member_of_class_type_nodecl(
        decl_context_t decl_context,
        type_t* class_type,
        nodecl_t nodecl_name,
        field_path_t* field_path)
{
    scope_entry_list_t *entry_list = query_nodecl_name_in_class(
            decl_context,
            named_type_get_symbol(advance_over_typedefs(class_type)), 
            nodecl_name,
            field_path);

    DEBUG_CODE()
    {
        print_field_path(field_path);
    }

    return entry_list;
}

// Remove this function in a future
static scope_entry_list_t* get_member_of_class_type(type_t* class_type,
        AST id_expression, decl_context_t decl_context,
        field_path_t* field_path)
{
    nodecl_t nodecl_name = nodecl_null();
    compute_nodecl_name_from_id_expression(id_expression, decl_context, &nodecl_name);

    return get_member_of_class_type_nodecl(decl_context,
            class_type,
            nodecl_name,
            field_path);
}

static void decimal_literal_type(AST expr, nodecl_t* nodecl_output);
static void character_literal_type(AST expr, nodecl_t* nodecl_output);
static void floating_literal_type(AST expr, nodecl_t* nodecl_output);
static void string_literal_type(AST expr, nodecl_t* nodecl_output);
static void pointer_literal_type(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);

// Typechecking functions
static void check_qualified_id(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_symbol(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_array_subscript_expr(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_function_call(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_explicit_type_conversion(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_explicit_typename_type_conversion(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_member_access(AST member_access, decl_context_t decl_context, char is_arrow, char has_template_tag, nodecl_t* nodecl_output);
static void check_typeid_expr(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_typeid_type(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_sizeof_expr(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_sizeof_typeid(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_sizeof_pack(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_cast_expr(AST expression, 
        AST type_id, AST casted_expression_list, 
        decl_context_t decl_context, 
        const char* cast_kind,
        nodecl_t* nodecl_output);
static void check_new_expression(AST new_expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_new_type_id_expr(AST new_expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_delete_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
// static void check_initializer_list(AST initializer_list, decl_context_t decl_context, type_t* declared_type);
static void check_binary_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_unary_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_throw_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_templated_member_access(AST templated_member_access, decl_context_t decl_context, 
        char is_arrow, nodecl_t* nodecl_output);
static void check_postincrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_postdecrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_preincrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_predecrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_conditional_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_comma_operand(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_pointer_to_member(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_pointer_to_pointer_to_member(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_unqualified_conversion_function_id(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_noexcept_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_lambda_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_initializer_clause_pack_expansion(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_vla_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_array_section_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_shaping_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_gcc_builtin_offsetof(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_builtin_choose_expr(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_builtin_types_compatible_p(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_label_addr(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_real_or_imag_part(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_alignof_expr(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_alignof_typeid(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_postfix_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_builtin_va_arg(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);
static void check_gcc_parenthesized_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void compute_nodecl_braced_initializer(AST braced_initializer, decl_context_t decl_context, nodecl_t* nodecl_output);
static void compute_nodecl_designated_initializer(AST braced_initializer, decl_context_t decl_context, nodecl_t* nodecl_output);
static void compute_nodecl_gcc_initializer(AST braced_initializer, decl_context_t decl_context, nodecl_t* nodecl_output);

static void resolve_symbol_this_nodecl(decl_context_t decl_context, const locus_t* locus, nodecl_t* nodecl_output);

static void solve_literal_symbol(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_mcc_debug_array_subscript(AST a,
        decl_context_t decl_context,
        nodecl_t* nodecl_output);
static void check_mcc_debug_constant_value_check(AST a,
        decl_context_t decl_context,
        nodecl_t* nodecl_output);

// Returns if the function is ok
//
// Do not return within this function, set result to 0 or 1 and let it
// reach the end, by default result == 0

static void check_expression_impl_(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static char c_check_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

char check_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
    {
        return c_check_expression(expression, decl_context, nodecl_output);
    }
    else if (IS_FORTRAN_LANGUAGE)
    {
        return fortran_check_expression(expression, decl_context, nodecl_output);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

char check_expression_must_be_constant(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Save the value of 'must_be_constant' of the last expression expression
    enum must_be_constant_t must_be_constant = check_expr_flags.must_be_constant;

    // The current expression is non executable
    check_expr_flags.must_be_constant = MUST_BE_CONSTANT;

    // Check_expression the current expression
    char output = check_expression(a, decl_context, nodecl_output);

    // Restore the right value
    check_expr_flags.must_be_constant = must_be_constant;

    return output;
}

char check_expression_non_executable(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Save the value of 'is_non_executable' of the last expression expression
    char was_non_executable = check_expr_flags.is_non_executable;

    enum must_be_constant_t must_be_constant = check_expr_flags.must_be_constant;
    // The current expression can be nonconstant
    check_expr_flags.must_be_constant = MUST_NOT_BE_CONSTANT;

    // The current expression is non executable
    check_expr_flags.is_non_executable = 1;

    // Check_expression the current expression
    char output = check_expression(a, decl_context, nodecl_output);

    // Restore the right value
    check_expr_flags.must_be_constant = must_be_constant;
    check_expr_flags.is_non_executable = was_non_executable;

    return output;
}

char check_expression_non_executable_must_be_constant(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Save the value of 'is_non_executable' of the last expression expression
    char was_non_executable = check_expr_flags.is_non_executable;
    // Save the value of 'must_be_constant' of the last expression expression
    enum must_be_constant_t must_be_constant = check_expr_flags.must_be_constant;

    // The current expression is non executable
    check_expr_flags.is_non_executable = 1;
    check_expr_flags.must_be_constant = MUST_BE_CONSTANT;

    // Check_expression the current expression
    char output = check_expression(a, decl_context, nodecl_output);

    // Restore the right values
    check_expr_flags.must_be_constant = must_be_constant;
    check_expr_flags.is_non_executable = was_non_executable;

    return output;
}

nodecl_t cxx_nodecl_wrap_in_parentheses(nodecl_t n)
{
    nodecl_t result = nodecl_make_parenthesized_expression(
            n,
            nodecl_get_type(n),
            nodecl_get_locus(n));

    nodecl_set_constant(result, nodecl_get_constant(n));
    nodecl_expr_set_is_type_dependent(result, nodecl_expr_is_type_dependent(n));
    nodecl_expr_set_is_value_dependent(result, nodecl_expr_is_value_dependent(n));

    return result;
}

static char check_list_of_expressions_aux(AST expression_list,
        decl_context_t decl_context,
        char preserve_top_level_parentheses,
        nodecl_t* nodecl_output)
{
    *nodecl_output = nodecl_null();
    if (expression_list == NULL)
    {
        // An empty list is OK
        return 1;
    }

    if (ASTType(expression_list) == AST_AMBIGUITY)
    {
        return solve_ambiguous_list_of_expressions(expression_list, decl_context, nodecl_output);
    }
    else
    {
        // Check the beginning of the list
        nodecl_t nodecl_prev_list = nodecl_null();
        check_list_of_expressions(ASTSon0(expression_list), decl_context, &nodecl_prev_list);

        if (!nodecl_is_null(nodecl_prev_list)
                && nodecl_is_err_expr(nodecl_prev_list))
        {
            *nodecl_output = nodecl_prev_list;
            return 0;
        }

        nodecl_t nodecl_current = nodecl_null();
        check_expression_impl_(ASTSon1(expression_list), decl_context, &nodecl_current);

        if (nodecl_is_err_expr(nodecl_current))
        {
            *nodecl_output = nodecl_current;
            return 0;
        }

        if (preserve_top_level_parentheses
                && ast_get_type(ASTSon1(expression_list)) == AST_PARENTHESIZED_EXPRESSION)
        {
            nodecl_current = cxx_nodecl_wrap_in_parentheses(nodecl_current);
        }

        *nodecl_output = nodecl_append_to_list(nodecl_prev_list, nodecl_current);

        return 1;
    }

    internal_error("Code unreachable", 0);
}

// Note that a list of expressions is NOT an expression
char check_list_of_expressions(AST expression_list,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    return check_list_of_expressions_aux(expression_list,
            decl_context,
            /* preserve_top_level_parentheses */ 0,
            nodecl_output);
}


void ensure_function_is_emitted(scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus)
{
    if (check_expr_flags.is_non_executable)
        return;

    if (entry != NULL
            && entry->kind == SK_FUNCTION)
    {
        if (decl_context.current_scope->kind == NAMESPACE_SCOPE
                || (decl_context.current_scope->kind == CLASS_SCOPE
                    && !is_dependent_type(decl_context.current_scope->related_entry->type_information))
                || (decl_context.current_scope->kind == BLOCK_SCOPE
                    /* && decl_context->current_scope->related_entry != NULL */
                    && decl_context.current_scope->related_entry != NULL
                    && (!is_dependent_type(decl_context.current_scope->related_entry->type_information)
                        && (!symbol_entity_specs_get_is_member(decl_context.current_scope->related_entry)
                            || !is_dependent_type(symbol_entity_specs_get_class_type(decl_context.current_scope->related_entry))))))
        {
            if (function_may_be_instantiated(entry))
            {
                DEBUG_CODE()
                {
                    fprintf(stderr, "EXPRTYPE: %s: Ensuring function '%s' will be emitted\n",
                            locus_to_str(locus),
                            get_qualified_symbol_name(entry, entry->decl_context));
                }
                instantiation_add_symbol_to_instantiate(entry, locus);
                push_instantiated_entity(entry);
            }
        }
    }
}

static char c_check_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    check_expression_impl_(expression, decl_context, nodecl_output);
    char is_ok = !nodecl_is_err_expr(*nodecl_output);
    return is_ok;
}

static void check_expression_impl_(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    ERROR_CONDITION(nodecl_output == NULL, "This cannot be NULL\n", 0);
    switch (ASTType(expression))
    {
        case AST_EXPRESSION :
        case AST_CONSTANT_EXPRESSION :
            // GCC extensions
        case AST_GCC_EXTENSION_EXPR : 
            {
                check_expression_impl_(ASTSon0(expression), decl_context, nodecl_output);
                break;
            }
        case AST_PARENTHESIZED_EXPRESSION :
            {
                check_expression_impl_(ASTSon0(expression), decl_context, nodecl_output);
                if (CURRENT_CONFIGURATION->preserve_parentheses
                        && !nodecl_is_err_expr(*nodecl_output))
                {
                    *nodecl_output = cxx_nodecl_wrap_in_parentheses(*nodecl_output);
                }
                break;
            }
            // Primaries
        case AST_DECIMAL_LITERAL :
        case AST_OCTAL_LITERAL :
        case AST_BINARY_LITERAL :
        case AST_HEXADECIMAL_LITERAL :
            {

                decimal_literal_type(expression, nodecl_output);
                break;
            }
        case AST_FLOATING_LITERAL :
        case AST_HEXADECIMAL_FLOAT :
            {

                floating_literal_type(expression, nodecl_output);
                break;
            }
        case AST_BOOLEAN_LITERAL :
            {

                type_t* t = get_bool_type();

                const char* literal = ASTText(expression);

                const_value_t* val = NULL;
                if (strcmp(literal, "true") == 0)
                {
                    val = const_value_get_one(type_get_size(t), 1);
                }
                else if (strcmp(literal, "false") == 0)
                {
                    val = const_value_get_zero(type_get_size(t), 1);
                    t = get_bool_false_type();
                }
                else
                {
                    internal_error("Code unreachable", 0);
                }

                *nodecl_output = nodecl_make_boolean_literal(t, val, ast_get_locus(expression));
                break;
            }
        case AST_CHARACTER_LITERAL :
            {
                character_literal_type(expression, nodecl_output);
                break;
            }
        case AST_STRING_LITERAL :
            {
                string_literal_type(expression, nodecl_output);
                break;
            }
        case AST_POINTER_LITERAL:
            {
                // nullptr
                pointer_literal_type(expression, decl_context, nodecl_output);
                break;
            }
        case AST_THIS_VARIABLE :
            {
                resolve_symbol_this_nodecl(decl_context, ast_get_locus(expression), nodecl_output);
                break;
            }
        case AST_SYMBOL :
        case AST_OPERATOR_FUNCTION_ID :
            {
                check_symbol(expression, decl_context, nodecl_output);
                break;
            }
        case AST_QUALIFIED_ID :
            {
                check_qualified_id(expression, decl_context, nodecl_output);
                break;
            }
        case AST_OPERATOR_FUNCTION_ID_TEMPLATE :
            {
                check_template_id_expr(expression, decl_context, nodecl_output);
                break;
            }
        case AST_CONVERSION_FUNCTION_ID :
            {
                // This case is only triggered in syntax like
                //
                // return operator bool();
                //
                // When operator bool appears as the id-expression of a
                // class-member access  (e.g. "a.operator bool()") we check
                // it in check_member_access
                check_unqualified_conversion_function_id(expression, decl_context, nodecl_output);
                break;
            }
        case AST_TEMPLATE_ID :
            {
                check_template_id_expr(expression, decl_context, nodecl_output);
                break;
            }
            // Postfix expressions
        case AST_ARRAY_SUBSCRIPT :
            {
                check_array_subscript_expr(expression, decl_context, nodecl_output);
                break;
            }
        case AST_FUNCTION_CALL :
            {
                check_function_call(expression, decl_context, nodecl_output);
                break;
            }
        case AST_EXPLICIT_TYPE_CONVERSION :
            {
                check_explicit_type_conversion(expression, decl_context, nodecl_output);
                break;
            }
        case AST_TYPENAME_EXPLICIT_TYPE_CONV :
            {
                check_explicit_typename_type_conversion(expression, decl_context, nodecl_output);
                break;
            }
        case AST_POINTER_CLASS_MEMBER_ACCESS :
        case AST_CLASS_MEMBER_ACCESS :
            {
                char is_arrow = (ASTType(expression) == AST_POINTER_CLASS_MEMBER_ACCESS);
                check_member_access(expression, decl_context, is_arrow, /*has template tag*/ 0, nodecl_output);
                break;
            }
        case AST_CLASS_TEMPLATE_MEMBER_ACCESS :
        case AST_POINTER_CLASS_TEMPLATE_MEMBER_ACCESS :
            {
                char is_arrow = (ASTType(expression) == AST_POINTER_CLASS_TEMPLATE_MEMBER_ACCESS);
                check_templated_member_access(expression, decl_context, is_arrow, nodecl_output);
                break;
            }
        case AST_POINTER_TO_MEMBER :
            {
                check_pointer_to_member(expression, decl_context, nodecl_output);
                break;
            }
        case AST_POINTER_TO_POINTER_MEMBER :
            {
                check_pointer_to_pointer_to_member(expression, decl_context, nodecl_output);
                break;
            }
        case AST_POSTINCREMENT :
            {
                check_postincrement(expression, decl_context, nodecl_output);
                break;
            }
        case AST_POSTDECREMENT :
            {
                check_postdecrement(expression, decl_context, nodecl_output);
                break;
            }
        case AST_DYNAMIC_CAST :
        case AST_STATIC_CAST :
        case AST_REINTERPRET_CAST :
        case AST_CONST_CAST :
            {
                AST type_id = ASTSon0(expression);
                AST casted_expr = ASTSon1(expression);

                const char* cast_kind = NULL;
                switch (ASTType(expression))
                {
                    case AST_DYNAMIC_CAST : cast_kind = "dynamic_cast"; break;
                    case AST_STATIC_CAST : cast_kind = "static_cast"; break;
                    case AST_REINTERPRET_CAST : cast_kind = "reinterpret_cast"; break;
                    case AST_CONST_CAST : cast_kind = "const_cast"; break;
                    default:
                          internal_error("Code unreachable", 0);
                }

                check_cast_expr(expression, type_id, casted_expr, decl_context, cast_kind, nodecl_output);
                break;
            }
        case AST_TYPEID_TYPE :
            {
                check_typeid_type(expression, decl_context, nodecl_output);
                break;
            }
        case AST_TYPEID_EXPR :
            {
                check_typeid_expr(expression, decl_context, nodecl_output);
                break;
            }
            // Unary expressions
        case AST_PREINCREMENT :
            {
                check_preincrement(expression, decl_context, nodecl_output);
                break;
            }
        case AST_PREDECREMENT :
            {
                check_predecrement(expression, decl_context, nodecl_output);
                break;
            }
        case AST_SIZEOF :
            {
                check_sizeof_expr(expression, decl_context, nodecl_output);
                break;
            }
        case AST_SIZEOF_TYPEID :
            {
                check_sizeof_typeid(expression, decl_context, nodecl_output);
                break;
            }
        case AST_SIZEOF_PACK:
            {
                check_sizeof_pack(expression, decl_context, nodecl_output);
                break;
            }
            /* UPC has upc_{local,block,elem}sizeof that are identical to the normal one */
        case AST_UPC_BLOCKSIZEOF :
        case AST_UPC_ELEMSIZEOF :
        case AST_UPC_LOCALSIZEOF :
            {
                error_printf("%s: sorry: UPC constructs not supported yet\n",
                        ast_location(expression));
                break;
            }
            /* UPC has upc_{local,block,elem}sizeof that are identical to the normal one */
        case AST_UPC_BLOCKSIZEOF_TYPEID :
        case AST_UPC_ELEMSIZEOF_TYPEID :
        case AST_UPC_LOCALSIZEOF_TYPEID :
            {
                error_printf("%s: sorry: UPC constructs not supported yet\n",
                        ast_location(expression));
                break;
            }
        case AST_DERREFERENCE :
        case AST_REFERENCE :
        case AST_PLUS :
        case AST_NEG :
        case AST_LOGICAL_NOT :
        case AST_BITWISE_NOT :
            {
                check_unary_expression(expression, decl_context, nodecl_output);
                break;
            }
            // Cast expression
        case AST_CAST :
            {
                AST type_id = ASTSon0(expression);
                AST casted_expr = ASTSon1(expression);

                check_cast_expr(expression, type_id, casted_expr, decl_context, "C", nodecl_output);
                break;
            }
        case AST_MUL :
        case AST_DIV :
        case AST_MOD :
        case AST_ADD :
        case AST_MINUS :
        case AST_BITWISE_SHL :
        case AST_SHR :
        case AST_LOWER_THAN :
        case AST_GREATER_THAN :
        case AST_GREATER_OR_EQUAL_THAN :
        case AST_LOWER_OR_EQUAL_THAN :
        case AST_EQUAL :
        case AST_DIFFERENT :
        case AST_BITWISE_AND :
        case AST_BITWISE_XOR :
        case AST_BITWISE_OR :
        case AST_LOGICAL_AND :
        case AST_LOGICAL_OR :
        case AST_POWER:
            {
                check_binary_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_CONDITIONAL_EXPRESSION :
        case AST_GCC_CONDITIONAL_EXPRESSION :
            {
                check_conditional_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_ASSIGNMENT :
        case AST_MUL_ASSIGNMENT :
        case AST_DIV_ASSIGNMENT :
        case AST_ADD_ASSIGNMENT :
        case AST_SUB_ASSIGNMENT :
        case AST_BITWISE_SHL_ASSIGNMENT :
        case AST_SHR_ASSIGNMENT :
        case AST_BITWISE_AND_ASSIGNMENT :
        case AST_BITWISE_OR_ASSIGNMENT :
        case AST_BITWISE_XOR_ASSIGNMENT :
        case AST_MOD_ASSIGNMENT :
            {
                check_binary_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_THROW_EXPRESSION :
            {
                check_throw_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_COMMA :
            {
                check_comma_operand(expression, decl_context, nodecl_output);
                break;
            }
        case AST_INITIALIZER_CLAUSE_PACK_EXPANSION:
            {
                check_initializer_clause_pack_expansion(expression, decl_context, nodecl_output);
                break;
            }
        case AST_NOEXCEPT_EXPRESSION:
            {
                check_noexcept_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_LAMBDA_EXPRESSION:
            {
                check_lambda_expression(expression, decl_context, nodecl_output);
                break;
            }
            // GCC Extension
        case AST_GCC_LABEL_ADDR :
            {
                check_gcc_label_addr(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_REAL_PART :
        case AST_GCC_IMAG_PART :
            {
                check_gcc_real_or_imag_part(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_ALIGNOF :
            {
                check_gcc_alignof_expr(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_ALIGNOF_TYPE :
        case /* C++11 */ AST_ALIGNOF_TYPE :
            {
                check_gcc_alignof_typeid(expression, decl_context, nodecl_output);
                break;
            }
        case AST_NEW_EXPRESSION :
            {
                check_new_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_NEW_TYPE_ID_EXPR :
            {
                check_new_type_id_expr(expression, decl_context, nodecl_output);
                break;
            }
        case AST_DELETE_EXPR :
        case AST_DELETE_ARRAY_EXPR :
            {
                check_delete_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_INITIALIZER_BRACES:
            {
                compute_nodecl_braced_initializer(expression, decl_context, nodecl_output);
                break;
            }
        case AST_VLA_EXPRESSION :
            {
                check_vla_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_POSTFIX_EXPRESSION :
            {
                check_gcc_postfix_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_BUILTIN_VA_ARG :
            {
                check_gcc_builtin_va_arg(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_BUILTIN_OFFSETOF :
            {
                check_gcc_builtin_offsetof(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_BUILTIN_CHOOSE_EXPR :
            {
                check_gcc_builtin_choose_expr(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_BUILTIN_TYPES_COMPATIBLE_P :
            {
                check_gcc_builtin_types_compatible_p(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_PARENTHESIZED_EXPRESSION :
            {
                check_gcc_parenthesized_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_GXX_TYPE_TRAITS :
            {
                check_gxx_type_traits(expression, decl_context, nodecl_output);
                break;
            }
            // This is a mcxx extension
            // that brings the power of Fortran 90 array-sections into C/C++ :-)
        case AST_ARRAY_SECTION :
            {
                check_array_section_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_ARRAY_SECTION_SIZE :
            {
                check_array_section_expression(expression, decl_context, nodecl_output);
                break;
            }
            // This is a mcxx extension
            // that gives an array shape to pointer expressions
        case AST_SHAPING_EXPRESSION:
            {
                check_shaping_expression(expression, decl_context, nodecl_output);
                break;
            }
            // Special nodes
        case AST_NODECL_LITERAL:
            {
                // Make sure we copy it, otherwise under ambiguity
                // the same trees would be wrongly handled
                *nodecl_output = nodecl_shallow_copy(nodecl_make_from_ast_nodecl_literal(expression));
                break;
            }
        case AST_DIMENSION_STR:
            {
                internal_error("Not supported", 0);
                break;
            }
            // CUDA
        case AST_CUDA_KERNEL_CALL:
            {
                check_cuda_kernel_call(expression, decl_context, nodecl_output);
                break;
            }
        case AST_AMBIGUITY :
            {
                solve_ambiguous_expression(expression, decl_context, nodecl_output);
                break;
            }
        case AST_SYMBOL_LITERAL_REF:
            {
                solve_literal_symbol(expression, decl_context, nodecl_output);
                break;
            }
            // This node is for debugging purposes of the compiler itself
        case AST_MCC_CONSTANT_VALUE_CHECK:
            {
                check_mcc_debug_constant_value_check(expression, decl_context, nodecl_output);
                break;
            }
        case AST_MCC_ARRAY_SUBSCRIPT_CHECK:
            {
                check_mcc_debug_array_subscript(expression, decl_context, nodecl_output);
                break;
            }
        default :
            {
                internal_error("Unexpected node '%s' %s", ast_print_node_type(ASTType(expression)), 
                        ast_location(expression));
                break;
            }
    }

    if (nodecl_is_null(*nodecl_output))
    {
        internal_error("Expression '%s' at '%s' lacks a nodecl\n",
                prettyprint_in_buffer(expression),
                ast_location(expression));
    }

    DEBUG_CODE()
    {
        if (!nodecl_is_err_expr(*nodecl_output))
        {
            type_t* t = nodecl_get_type(*nodecl_output);

            fprintf(stderr, "EXPRTYPE: Expression '%s' at '%s' has as computed type '%s'",
                    prettyprint_in_buffer(expression),
                    ast_location(expression),
                    t != NULL ? print_declarator(t) : "<<NO TYPE>>");

            if (nodecl_is_constant(*nodecl_output))
            {
                const_value_t* v = nodecl_get_constant(*nodecl_output);
                fprintf(stderr, " with a constant value of '%s'",
                        const_value_to_str(v));
            }

            if (nodecl_expr_is_type_dependent(*nodecl_output))
            {
                fprintf(stderr, " [TYPE DEPENDENT]");
            }

            if (nodecl_expr_is_value_dependent(*nodecl_output))
            {
                fprintf(stderr, " [VALUE DEPENDENT]");
            }

            fprintf(stderr, "\n");
        }
    }

    // if (CURRENT_CONFIGURATION->strict_typecheck
    //         && nodecl_is_err_expr(*nodecl_output))
    // {
    //     internal_error("Invalid expression '%s' at '%s'\n", prettyprint_in_buffer(expression), ast_location(expression));
    // }
}

// This function removes the base prefix (if any) and the quotes, if any
static const char* process_integer_literal(const char* literal,
        int *base,
        const locus_t* locus)
{
    ERROR_CONDITION(literal == NULL
            || literal[0] == '\0', "Invalid literal\n", literal);

    if (literal[0] == '0')
    {
        *base = 8;

        if (literal[1] == 'x'
                || literal[1] == 'X')
        {
            *base = 16;
            literal += 2; // Skip 0x
        }
        else if (literal[1] == 'b'
                || literal[1] == 'B')
        {
            *base = 2;
            literal += 2; // Skip 0b

            CXX03_LANGUAGE()
            {
                fprintf(stderr, "%s: warning: binary-integer-literals are a C++11 feature\n",
                        locus_to_str(locus));
            }
        }
    }
    else
    {
        *base = 10;
    }

    int length = strlen(literal);
    char tmp[length + 1];

    int i, j;
    for (i = 0, j = 0; i < length; i++)
    {
        if (literal[i] != '\'')
        {
            tmp[j] = literal[i];
            j++;
        }
        else
        {
            CXX03_LANGUAGE()
            {
                fprintf(stderr, "%s: warning: quotes interspersed in integer-literal digits are a C++14 feature\n",
                        locus_to_str(locus));
            }
        }
    }
    tmp[j] = '\0';

    return uniquestr(tmp);
}

// Given a decimal literal computes the type due to its lexic form
static void decimal_literal_type(AST expr, nodecl_t* nodecl_output)
{
    const char *literal = ASTText(expr);
    const char *last = literal + strlen(literal) - 1;

    char is_unsigned = 0;
    char is_long = 0;
    char is_complex = 0;

    const_value_t* val = NULL;

    // This loop goes backwards until the literal figures are found again
    while (toupper(*last) == 'L' 
            || toupper(*last) == 'U'
            // This is a GNU extension for complex
            || toupper(*last) == 'I'
            || toupper(*last) == 'J')
    {
        switch (toupper(*last))
        {
            case 'L' :
                is_long++;
                break;
            case 'U' :
                is_unsigned = 1;
                break;
            case 'J':
            case 'I':
                is_complex = 1;
                break;
            default:
                internal_error("Code unreachable", 0);
                break;
        }
        last--;
    }

    char is_decimal = (ASTType(expr) == AST_DECIMAL_LITERAL);

    type_t** eligible_types = NULL;
    int num_eligible_types = 0;

    // decimal no suffix -> int, long, long long
    type_t* decimal_no_suffix[] = { 
        get_signed_int_type(), 
        get_signed_long_int_type(), 
        get_signed_long_long_int_type() 
    };

    if (is_decimal
            && !is_unsigned
            && !is_long)
    {
        eligible_types = decimal_no_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(decimal_no_suffix);
    }

    // oct/hex no suffix -> int, unsigned int, long, unsigned long, long long, unsigned long long
    type_t* nondecimal_no_suffix[] = { 
        get_signed_int_type(),
        get_unsigned_int_type(),
        get_signed_long_int_type(),
        get_unsigned_long_int_type(),
        get_signed_long_long_int_type(),
        get_unsigned_long_long_int_type() 
    };

    if (!is_decimal
            && !is_unsigned
            && is_long == 0)
    {
        eligible_types = nondecimal_no_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(nondecimal_no_suffix);
    }
    
    // U suffix  -> unsigned int, unsigned long, unsigned long long
    type_t* U_suffix[] =  {
        get_unsigned_int_type(),
        get_unsigned_long_int_type(),
        get_unsigned_long_long_int_type(),
    };

    if (is_unsigned
            && is_long == 0)
    {
        eligible_types = U_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(U_suffix);
    }

    // decimal L suffix  -> long, long long
    type_t* decimal_L_suffix[] = {
        get_signed_long_int_type(),
        get_signed_long_long_int_type(),
    };

    if (is_decimal
            && !is_unsigned
            && is_long == 1)
    {
        eligible_types = decimal_L_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(decimal_L_suffix);
    }

    // bin/oct/hex L suffix -> long, unsigned long, long long, unsigned long long
    type_t* nondecimal_L_suffix[] = {
        get_signed_long_int_type(),
        get_unsigned_long_int_type(),
        get_signed_long_long_int_type(),
        get_unsigned_long_long_int_type(),
    };

    if (!is_decimal
            && !is_unsigned
            && is_long == 1)
    {
        eligible_types = nondecimal_L_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(nondecimal_L_suffix);
    }

    // UL suffix -> unsigned long, unsigned long long
    type_t* UL_suffix[] = { 
        get_unsigned_long_int_type(),
        get_unsigned_long_long_int_type(),
    };

    if (is_unsigned
            && is_long == 1)
    {
        eligible_types = UL_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(UL_suffix);
    }

    // LL suffix -> long long, unsigned long logn
    type_t* decimal_LL_suffix[] = {
        get_signed_long_long_int_type(),
    };

    if (is_decimal
            && !is_unsigned
            && is_long == 2)
    {
        eligible_types = decimal_LL_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(decimal_LL_suffix);
    }

    type_t* nondecimal_LL_suffix[] = {
        get_signed_long_long_int_type(),
        get_unsigned_long_long_int_type(),
    };

    if (!is_decimal
            && !is_unsigned
            && is_long == 2)
    {
        eligible_types = nondecimal_LL_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(nondecimal_LL_suffix);
    }

    // ULL suffix -> unsigned long long
    type_t* ULL_suffix[] = {
        get_unsigned_long_long_int_type(),
    };

    if (is_unsigned
            && is_long == 2)
    {
        eligible_types = ULL_suffix;
        num_eligible_types = STATIC_ARRAY_LENGTH(ULL_suffix);
    }

    ERROR_CONDITION(eligible_types == NULL, "No set of eligible types has been computed", 0);

    int base = 0;
    const char* processed_literal = process_integer_literal(literal, &base, ast_get_locus(expr));
    ERROR_CONDITION(base == 0, "Invalid base", 0);

    uint64_t parsed_value = (uint64_t)strtoull(processed_literal, NULL, base);

    type_t* result =
        const_value_get_minimal_integer_type_from_list_of_types(
                parsed_value,
                num_eligible_types,
                eligible_types);

    if (result == NULL)
    {
        error_printf("%s: error: there is not any appropiate integer type for constant '%s', assuming 'unsigned long long'\n",
                ast_location(expr),
                ASTText(expr));
        result = get_unsigned_long_long_int_type();
    }

    val = const_value_get_integer(parsed_value, type_get_size(result), is_signed_integral_type(result));

    // Zero is a null pointer constant requiring a distinguishable 'int' type
    if (const_value_is_zero(val))
    {
        // Get the appropiate zero value
        result = get_zero_type(result);
    }

    if (is_complex)
    {
        result = get_complex_type(result);
        const_value_t* imag_val = val;
        val = const_value_make_complex(const_value_get_zero(type_get_size(result), !is_unsigned), imag_val);

        *nodecl_output = nodecl_make_complex_literal(
                result,
                val,
                ast_get_locus(expr));
    }
    else
    {
        *nodecl_output = nodecl_make_integer_literal(result, val, ast_get_locus(expr));
    }
}

#define IS_OCTA_CHAR(_c) \
(((_c) >= '0') \
  && ((_c) <= '7'))

#define IS_HEXA_CHAR(_c) \
((((_c) >= '0') \
  && ((_c) <= '9')) \
 || (((_c) >= 'a') \
     && ((_c) <= 'f')) \
 || (((_c) >= 'A') \
     && ((_c) <= 'F')))

// Given a character literal computes the type due to its lexic form
static void character_literal_type(AST expr, nodecl_t* nodecl_output)
{
    const char *literal = ASTText(expr);

    type_t* result = NULL;
    if (*literal == 'L')
    {
        result = get_wchar_t_type();
        literal++;
    }
    else if (*literal == 'u')
    {
        if (IS_CXX03_LANGUAGE)
        {
            warn_printf("%s: warning: char16_t literals are a C++11 feature\n",
                    ast_location(expr));
        }
        result = get_char16_t_type();
        literal++;
    }
    else if (*literal == 'U')
    {
        if (IS_CXX03_LANGUAGE)
        {
            warn_printf("%s: warning: char32_t literals are a C++11 feature\n",
                    ast_location(expr));
        }
        result = get_char32_t_type();
        literal++;
    }
    else
    {
        result = get_char_type();
    }

    // literal[0] is the quote
    uint64_t value = 0;
    if (literal[1] != '\\')
    {
        // Usual case: not an escape of any kind
        value = literal[1];
    }
    else
    {
        switch (literal[2])
        {
            case '\'': { value = literal[2]; break; }
            case 'a' : { value = '\a'; break; }
            case 'b' : { value = '\b'; break; }
            case 'e' : { value = '\e'; break; } // This is a gcc extension
            case 'f' : { value = '\f'; break; }
            case 'n' : { value = '\n'; break; }
            case 'r' : { value = '\r'; break; }
            case 't' : { value = '\t'; break; }
            case 'v' : { value = '\v'; break; }
            case '\\': { value = '\\'; break; }
            case '\"': { value = '\"'; break; }
            case '\?': { value = '\?'; break; }
            case '0': 
            case '1': 
            case '2': 
            case '3': 
            case '4': 
            case '5': 
            case '6': 
            case '7': 
                       {
                           int i;
                           char c[32] = { 0 };
                           // Copy until the quote
                           for (i = 2; literal[i] != '\'' && literal[i] != '\0' ; i++)
                           {
                               c[i - 2] = literal[i];
                           }

                           char *err = NULL;
                           value = strtol(c, &err, 8);

                           if (!(*c != '\0'
                                       && *err == '\0'))
                           {
                               error_printf("%s: error: %s does not seem a valid character literal\n", 
                                       ast_location(expr),
                                       prettyprint_in_buffer(expr));
                               *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
                               return;
                           }
                           break;
                       }
            case 'x':
                       {
                           int i;
                           char c[32] = { 0 };
                           // Copy until the quote
                           // Note literal_value is '\x000' so the number
                           // starts at literal_value[3]
                           for (i = 3; literal[i] != '\'' && literal[i] != '\0' ; i++)
                           {
                               c[i - 3] = literal[i];
                           }

                           char * err = NULL;
                           value = strtol(c, &err, 16);

                           if (!(*c != '\0'
                                       && *err == '\0'))
                           {
                               error_printf("%s: error: %s does not seem a valid character literal\n", 
                                       ast_location(expr),
                                       prettyprint_in_buffer(expr));
                               *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
                               return;
                           }
                           break;
                       }
            case 'u' :
            case 'U' :
                       {
                           // Universal names are followed by 4 hexa digits
                           // or 8 depending on 'u' or 'U' respectively
                           char remaining_hexa_digits = 8;
                           if (literal[2] == 'u')
                           {
                               remaining_hexa_digits = 4;
                           }

                           unsigned int current_value = 0;

                           int i = 3;
                           while (remaining_hexa_digits > 0)
                           {
                               if (!IS_HEXA_CHAR(literal[i]))
                               {
                                   char ill_literal[11];
                                   strncpy(ill_literal, &literal[1], /* hexa */ 8 + /* escape */ 1 + /* null*/ 1 );
                                   error_printf("%s: error: invalid universal literal character name '%s'\n",
                                           ast_location(expr),
                                           ill_literal);
                                   *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
                                   return;
                               }

                               current_value *= 16;
                               char current_literal = tolower(literal[i]);
                               if (('0' <= tolower(current_literal))
                                       && (tolower(current_literal) <= '9'))
                               {
                                   current_value += current_literal - '0';
                               }
                               else if (('a' <= tolower(current_literal))
                                       && (tolower(current_literal) <= 'f'))
                               {
                                   current_value += 10 + (tolower(current_literal) - 'a');
                               }
                               else
                               {
                                   internal_error("Code unreachable", 0);
                               }

                               remaining_hexa_digits--;
                               i++;
                           }

                           break;
                       }
            default: {
                         error_printf("%s: error: %s does not seem a valid escape character\n", 
                                 ast_location(expr),
                                 prettyprint_in_buffer(expr));
                         *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
                         return;
                     }
        }
    }

    if (value == 0)
    {
        result = get_zero_type(result);
    }

    *nodecl_output = nodecl_make_integer_literal(result, 
            const_value_get_integer(value, type_get_size(result), is_signed_integral_type(result)), 
            ast_get_locus(expr));
}

#define check_range_of_floating(expr, text, value, typename, huge) \
    do { \
        if (value == 0 && errno == ERANGE) \
        { \
            warn_printf("%s: warning: value '%s' underflows '%s'\n", \
                    ast_location(expr), text, typename); \
            value = 0.0; \
            generate_text_node = 1; \
        } \
        else if (isinf(value)) \
        { \
            warn_printf("%s: warning: value '%s' overflows '%s'\n", \
                    ast_location(expr), text, typename); \
            value = huge; \
            generate_text_node = 1; \
        } \
    } while (0)

#define check_range_of_floating_extended(expr, text, value, typename, isinf_fun, huge) \
    do { \
        if (value == 0 && errno == ERANGE) \
        { \
            warn_printf("%s: warning: value '%s' underflows '%s'\n", \
                    ast_location(expr), text, typename); \
            value = 0.0; \
            generate_text_node = 1; \
        } \
        else if (isinf_fun(value)) \
        { \
            warn_printf("%s: warning: value '%s' overflows '%s'\n", \
                    ast_location(expr), text, typename); \
            value = huge; \
            generate_text_node = 1; \
        } \
    } while (0)

static void floating_literal_type(AST expr, nodecl_t* nodecl_output)
{
    const_value_t* value = NULL;
    const char *literal = ASTText(expr);
    const char *last = literal + strlen(literal) - 1;

    char is_float = 0;
    char is_long_double = 0;
    char is_float128 = 0;
    char is_complex = 0;

    while (toupper(*last) == 'F' 
            || toupper(*last) == 'L'
            // This is a GNU extension for float128
            || toupper(*last) == 'Q'
            // This is a GNU extension for complex
            || toupper(*last) == 'I'
            || toupper(*last) == 'J')
    {
        switch (toupper(*last))
        {
            case 'L' :
                is_long_double++;
                break;
            case 'F' :
                is_float = 1;
                break;
            case 'Q' :
                is_float128 = 1;
                break;
            case 'I':
            case 'J':
                is_complex = 1;
                break;
            default:
                break;
        }
        last--;
    }

    type_t* result = NULL;
    const_value_t* zero = NULL;

    // Sometimes we cannot express a meaningful literal from what we got,
    // in this case we generate a text node (with a constant value)
    //
    // This variable is set to 1 in the overflow/underflow branches of
    // check_range_of_floating and check_range_of_floating_extended macros
    char generate_text_node = 0;

    if (is_float128)
    {
#ifdef HAVE_QUADMATH_H
        {
            result = get_float128_type();

            errno = 0;
            __float128 f128 = strtoflt128(literal, NULL);
            check_range_of_floating_extended(expr, literal, f128, "__float128", isinfq, HUGE_VALQ);

            value = const_value_get_float128(f128);

            if (is_complex)
                zero = const_value_get_float128(0.0);
        }
#else
        {
            error_printf("%s: error: __float128 literals not supported\n", ast_location(expr));
            *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
            return;
        }
#endif
    }
    else if (is_long_double)
    {
        result = get_long_double_type();

        errno = 0;
        long double ld = strtold(literal, NULL);
        check_range_of_floating(expr, literal, ld, "long double", HUGE_VALL);

        value = const_value_get_long_double(ld);

        if (is_complex)
            zero = const_value_get_long_double(0.0L);
    }
    else if (is_float)
    {
        result = get_float_type();

        errno = 0;
        float f = strtof(literal, NULL);
        check_range_of_floating(expr, literal, f, "float", HUGE_VALF);

        value = const_value_get_float(f);

        if (is_complex)
            zero = const_value_get_float(0.0f);
    }
    else
    {
        result = get_double_type();

        errno = 0;
        double d = strtod(literal, NULL);
        check_range_of_floating(expr, literal, d, "double", HUGE_VAL);

        value = const_value_get_double(d);

        if (is_complex)
            zero = const_value_get_double(0.0);
    }

    if (is_complex)
    {
        result = get_complex_type(result);
        const_value_t* imag_value = value;
        value = const_value_make_complex(zero, imag_value);
    }

    if (!generate_text_node)
    {
        if (is_complex)
        {
            *nodecl_output =
                nodecl_make_complex_literal(
                        result,
                        value,
                        ast_get_locus(expr));
        }
        else
        {
            *nodecl_output = nodecl_make_floating_literal(result, value, ast_get_locus(expr));
        }
    }
    else
    {
        // Unusual path where we keep the literal text because we will not
        // be able to express this floating point otherwise
        *nodecl_output = nodecl_make_text(literal, ast_get_locus(expr));
        nodecl_set_type(*nodecl_output, result);
        nodecl_set_constant(*nodecl_output, value);
    }
}

static void compute_length_of_literal_string(
        const char* literal,
        int *num_codepoints,
        int **codepoints,
        type_t** base_type,
        const locus_t* locus)
{
    const char* orig_literal = literal;
    int capacity_codepoints = 16;
    *num_codepoints = 0;

    (*codepoints) = xcalloc(capacity_codepoints, sizeof(**codepoints));

    *base_type = NULL;

    int num_of_strings_seen = 0;

#define ADD_CODEPOINT(value) \
        do { \
            int v_ = (value); \
            if (*num_codepoints == capacity_codepoints) \
            { \
                capacity_codepoints *= 2; \
                (*codepoints) = xrealloc((*codepoints), capacity_codepoints * sizeof(**codepoints)); \
            } \
            (*codepoints)[*num_codepoints] = v_; \
            (*num_codepoints)++; \
        } while (0);

    // Beginning of a string
    while (*literal != '\0')
    {
        // Check the prefix
        type_t* current_base_type = NULL;
        if ((*literal) == 'L')
        {
            current_base_type = get_wchar_t_type();
            CXX_LANGUAGE()
            {
                current_base_type = get_const_qualified_type(current_base_type);
            }
            literal++;
        }
        else if ((*literal) == 'u' && (*(literal + 1) == '8'))
        {
            current_base_type = get_const_qualified_type(get_char_type());
            literal += 2;
        }
        else if ((*literal) == 'u')
        {
            current_base_type = get_const_qualified_type(get_char16_t_type());
            literal++;
        }
        else if ((*literal) == 'U')
        {
            current_base_type = get_const_qualified_type(get_char32_t_type());
            literal++;
        }
        else
        {
            current_base_type = get_const_qualified_type(get_char_type());
        }

        if (*base_type == NULL)
        {
            *base_type = current_base_type;
        }
        else
        {
            // "a" U"b" -> U"a" U"b"
            if (is_char_type(get_unqualified_type(*base_type))
                    && !is_char_type(get_unqualified_type(current_base_type)))
            {
                *base_type = current_base_type;
            }
        }

        char is_raw_string;
        if ((*literal) == 'R')
        {
            // This is a raw_string, do not interpret escape sequences
            CXX03_LANGUAGE()
            {
                warn_printf("%s: warning: raw-string-literals are a C++11 feature\n", 
                        locus_to_str(locus));
            }
            is_raw_string = 1;
            literal++;
        }
        else
        {
            is_raw_string = 0;
        }

        ERROR_CONDITION(*literal != '"',
                "Lexical problem in the literal '%s'\n", orig_literal);

        // Advance the "
        literal++;

        // Advance till we find a '"'
        while (*literal != '"')
        {
            if (!is_raw_string
                    && *literal == '\\')
            {
                // This is used for diagnostics
                const char *beginning_of_escape = literal;

                // A scape sequence
                literal++;
                switch (*literal)
                {
                    case '\'' : { ADD_CODEPOINT('\''); break; }
                    case '"' : { ADD_CODEPOINT('"'); break; }
                    case '?' : { ADD_CODEPOINT('\?'); break; }
                    case '\\' : { ADD_CODEPOINT('\\'); break; }
                    case 'a' : { ADD_CODEPOINT('\a'); break; }
                    case 'b' : { ADD_CODEPOINT('\b'); break; }
                    case 'f' : { ADD_CODEPOINT('\f'); break; }
                    case 'n' : { ADD_CODEPOINT('\n'); break; }
                    case 'r' : { ADD_CODEPOINT('\r'); break; }
                    case 't' : { ADD_CODEPOINT('\t'); break; }
                    case 'v' : { ADD_CODEPOINT('\v'); break; }
                    case 'e' : { ADD_CODEPOINT('\033'); break; } // GNU Extension: A synonim for \033
                    case '0' :
                    case '1' :
                    case '2' :
                    case '3' :
                    case '4' :
                    case '5' :
                    case '6' :
                    case '7' :
                               // This is an octal
                               // Advance up to three octals
                               {
                                   // Advance this octal, so the remaining figures are 2
                                   unsigned int current_value = (*literal) - '0';

                                   literal++;
                                   int remaining_figures = 2;

                                   while (IS_OCTA_CHAR(*literal)
                                           && (remaining_figures > 0))
                                   {
                                       current_value *= 8;
                                       current_value += ((*literal) - '0');
                                       remaining_figures--;
                                       literal++;
                                   }
                                   // Go backwards because we have already
                                   // advanced the last element of this
                                   // escaped entity
                                   literal--;

                                   ADD_CODEPOINT(current_value);
                                   break;
                               }
                    case 'x' :
                               // This is an hexadecimal
                               {
                                   // Jump 'x' itself
                                   literal++;

                                   unsigned int current_value = 0;

                                   while (IS_HEXA_CHAR(*literal))
                                   {
                                       current_value *= 16;
                                       char current_literal = tolower(*literal);
                                       if (('0' <= tolower(current_literal))
                                               && (tolower(current_literal) <= '9'))
                                       {
                                           current_value += current_literal - '0';
                                       }
                                       else if (('a' <= tolower(current_literal))
                                               && (tolower(current_literal) <= 'f'))
                                       {
                                           current_value += 10 + (tolower(current_literal) - 'a');
                                       }
                                       else
                                       {
                                           internal_error("Code unreachable", 0);
                                       }
                                       literal++;
                                   }

                                   // Go backwards because we have already
                                   // advanced the last element of this
                                   // escaped entity
                                   literal--;

                                   ADD_CODEPOINT(current_value);
                                   break;
                               }
                    case 'u' :
                    case 'U' :
                               {
                                   // Universal names are followed by 4 hexa digits
                                   // or 8 depending on 'u' or 'U' respectively
                                   char remaining_hexa_digits = 8;
                                   if (*literal == 'u')
                                   {
                                       remaining_hexa_digits = 4;
                                   }

                                   // Advance 'u'/'U'
                                   literal++;

                                   unsigned int current_value = 0;

                                   while (remaining_hexa_digits > 0)
                                   {
                                       if (!IS_HEXA_CHAR(*literal))
                                       {
                                           char ill_literal[11];
                                           strncpy(ill_literal, beginning_of_escape, /* hexa */ 8 + /* escape */ 1 + /* null*/ 1 );
                                           error_printf("%s: error: invalid universal literal name '%s'\n", 
                                                   locus_to_str(locus),
                                                   ill_literal);
                                           *num_codepoints = -1;
                                           xfree(*codepoints);
                                           return;
                                       }

                                       current_value *= 16;
                                       char current_literal = tolower(*literal);
                                       if (('0' <= tolower(current_literal))
                                               && (tolower(current_literal) <= '9'))
                                       {
                                           current_value += current_literal - '0';
                                       }
                                       else if (('a' <= tolower(current_literal))
                                               && (tolower(current_literal) <= 'f'))
                                       {
                                           current_value += 10 + (tolower(current_literal) - 'a');
                                       }
                                       else
                                       {
                                           internal_error("Code unreachable", 0);
                                       }

                                       literal++;
                                       remaining_hexa_digits--;
                                   }

                                   // Go backwards one
                                   literal--;

                                   ADD_CODEPOINT(current_value);
                                   break;
                               }
                    default:
                               {
                                   char c[3];

                                   strncpy(c, beginning_of_escape, 3);
                                   error_printf("%s: error: invalid escape sequence '%s'\n",
                                           locus_to_str(locus), c);
                                   *num_codepoints = -1;
                                   xfree(*codepoints);
                                   return;
                               }
                }
            }
            else
            {
                // A plain codepoint
                ADD_CODEPOINT(*literal);
            }

            // Make 'literal' always point to the last thing that represents one codepoint
            //
            // For instance, for "\n", "\002", "\uabcd" and "\U98abcdef" (*literal) should
            // be 'n', '2', 'd' and 'f' respectively.
            literal++;
        }

        // Advance the "
        literal++;

        num_of_strings_seen++;
    }

    // Final NULL value
    ADD_CODEPOINT(0);

    ERROR_CONDITION(num_of_strings_seen == 0, "Empty string literal '%s'\n", orig_literal);
}

// This is used by the lexer. It returns a new string to be deallocated
// by the caller
char* interpret_schar(const char* schar, const locus_t* locus)
{
    int num_codepoints = 0;
    int *codepoints = NULL;

    type_t* base_type = NULL;

    compute_length_of_literal_string(schar,
            &num_codepoints,
            &codepoints,
            &base_type,
            locus);

    if (num_codepoints < 0)
        return NULL;

    if (is_char_type(get_unqualified_type(base_type)))
    {
        int length = 0;
        length = num_codepoints;

        char c[length];
        int i;
        for (i = 0; i < length; i++)
            c[i] = codepoints[i];

        return xstrdup(c);
    }
    else
    {
        error_printf("%s: error: invalid non-narrow char string literal\n",
                locus_to_str(locus));
        return NULL;
    }
}

static void string_literal_type(AST expr, nodecl_t* nodecl_output)
{
    int num_codepoints = 0;
    int *codepoints = NULL;

    type_t* base_type = NULL;

    compute_length_of_literal_string(ASTText(expr),
            &num_codepoints,
            &codepoints,
            &base_type,
            ast_get_locus(expr));
    if (num_codepoints < 0)
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: String literal %s base type is '%s'. Number of codepoints %d'\n",
                ASTText(expr),
                print_declarator(base_type),
                num_codepoints);
    }

    // Note that the length represents the length of the array in memory and depending on the encoding it may
    // be different to the number of codepoints
    int length = 0;
    const_value_t* value = NULL;

    if (is_char_type(get_unqualified_type(base_type)))
    {
        length = num_codepoints;

        char c[length];
        int i;
        for (i = 0; i < length; i++)
            c[i] = codepoints[i];

        value = const_value_make_string_null_ended(c, length - 1);
    }
    else if (is_wchar_t_type(get_unqualified_type(base_type)))
    {
        // FIXME - Should we perform a conversion as well?
        length = num_codepoints;
        value = const_value_make_wstring_null_ended(codepoints, length - 1);
    }
    else if (is_char16_t_type(get_unqualified_type(base_type)))
    {
        // Note that we should not care if UTF16LE or UTF16BE, we only want
        // iconv not to emit a BOM
        iconv_t cd = iconv_open("UTF16LE", "UTF32");

        ERROR_CONDITION(cd == (iconv_t)-1,
            "Cannot convert to UTF16LE from UTF32\n", 0);

        length = num_codepoints;
        value = const_value_make_wstring_null_ended(codepoints, length - 1);

        // Usually there are no surrogates, so initially allocate the same number of codepoints
        int capacity_out = num_codepoints;
        uint16_t *output_buffer = xcalloc(capacity_out, sizeof(*output_buffer));

        char done = 0;
        while (!done)
        {
            // POSIX dictates that the 2nd parameter of iconv be const char** but
			// GNU/Linux and other systems defined it as char**. AM_ICONV in configure.ac
            // defines the ICONV_CONST macro accordingly
            ICONV_CONST char* inbuff = (ICONV_CONST char*)codepoints;
            size_t inbyteslefts = num_codepoints * sizeof(*codepoints);

            size_t outbytesleft = capacity_out * sizeof(*output_buffer), outbytesbeginning = outbytesleft;
            char *outbuff = (char*)output_buffer;

            size_t conv_result = iconv(cd, &inbuff, &inbyteslefts, &outbuff, &outbytesleft);
            if (conv_result == (size_t)-1)
            {
                // Our output buffer is too small, reallocate and try again
                if (errno == E2BIG)
                {
                    capacity_out *= 2;
                    xfree(output_buffer);
                    output_buffer = xcalloc(capacity_out, sizeof(*output_buffer));

                    // Reset the state of iconv
                    iconv(cd, NULL, NULL, NULL, NULL);

                    // Start again
                    continue;
                }
                else if (errno == EINVAL || errno == EILSEQ)
                {
                    error_printf("%s: error: discarding invalid UTF-16 literal\n",
                            ast_location(expr));
                    *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
                    xfree(output_buffer);
                }
                else
                {
                    internal_error("Unexpected error '%s' from 'iconv'\n",
                            strerror(errno));
                }
            }
            done = 1;

            ERROR_CONDITION(inbyteslefts != 0, "There are still bytes left", 0);
            ERROR_CONDITION(((outbytesleft - outbytesbeginning) % sizeof(uint16_t)) != 0,
                    "The conversion has not generated an even number of bytes", 0);
            ERROR_CONDITION(outbytesbeginning < outbytesleft,
                    "There were less bytes at the beginning", 0);

            length = (outbytesbeginning - outbytesleft) / sizeof(uint16_t);
        }

        xfree(output_buffer);
        iconv_close(cd);
    }
    else if (is_char32_t_type(get_unqualified_type(base_type)))
    {
        length = num_codepoints;
        value = const_value_make_wstring_null_ended(codepoints, length - 1);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }

    type_t* result = get_literal_string_type(length, base_type);

    *nodecl_output = nodecl_make_string_literal(result, value, ast_get_locus(expr));
}

static scope_entry_t* get_nullptr_symbol(decl_context_t decl_context)
{
    decl_context_t global_context = decl_context;
    global_context.current_scope = global_context.global_scope;
    scope_entry_list_t* entry_list = query_in_scope_str(global_context, UNIQUESTR_LITERAL(".nullptr"), NULL);

    if (entry_list == NULL)
    {
        scope_entry_t* nullptr_sym = new_symbol(global_context, global_context.current_scope, ".nullptr");

        // Change the name of the symbol
        nullptr_sym->symbol_name = UNIQUESTR_LITERAL("nullptr");
        nullptr_sym->kind = SK_VARIABLE;
        symbol_entity_specs_set_is_builtin(nullptr_sym, 1);
        nullptr_sym->type_information = get_nullptr_type();

        return nullptr_sym;
    }
    else
    {
        scope_entry_t* result = entry_list_head(entry_list);
        return result;
    }
}

static void pointer_literal_type(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    scope_entry_t* entry = get_nullptr_symbol(decl_context);
    ERROR_CONDITION(entry == NULL, "This should not happen, nullptr should always exist", 0);

    *nodecl_output = nodecl_make_symbol(entry,
            ast_get_locus(expr));

    // Note that this is not an lvalue
    nodecl_set_type(*nodecl_output, entry->type_information);

    // This is a constant
    nodecl_set_constant(*nodecl_output,
            const_value_get_zero(
                type_get_size(get_pointer_type(get_void_type())),
                /* sign */ 0));
}

scope_entry_t* resolve_symbol_this(decl_context_t decl_context)
{
    scope_entry_t *this_symbol = NULL;
    if (decl_context.current_scope->kind == BLOCK_SCOPE)
    {
        // Lookup a symbol 'this' as usual
        scope_entry_list_t* entry_list = query_name_str(decl_context, UNIQUESTR_LITERAL("this"), NULL);
        if (entry_list != NULL)
        {
            this_symbol = entry_list_head(entry_list);
        }
        entry_list_free(entry_list);

        if (this_symbol != NULL
                || !check_expr_flags.is_non_executable)
            return this_symbol;
    }

    // - we are not in block scope but lexically nested in a class scope, use
    // the 'this' of the class, or if no that
    // - we are in block scope (nested in a class scope) but it does not have
    // 'this' and we are in a non executable context
    if (decl_context.class_scope != NULL)
    {
        scope_entry_t* class_symbol = decl_context.class_scope->related_entry;
        ERROR_CONDITION(class_symbol == NULL, "Invalid symbol", 0);

        if (symbol_entity_specs_get_num_related_symbols(class_symbol) != 0
                && symbol_entity_specs_get_related_symbols_num(class_symbol, 0) != NULL
                && (strcmp(symbol_entity_specs_get_related_symbols_num(class_symbol, 0)->symbol_name, "this") == 0))
        {
            this_symbol = symbol_entity_specs_get_related_symbols_num(class_symbol, 0);
        }
    }

    return this_symbol;
}


static void resolve_symbol_this_nodecl(decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    scope_entry_t* this_symbol = resolve_symbol_this(decl_context);

    if (this_symbol == NULL)
    {
        error_printf("%s: error: 'this' cannot be used in this context\n",
                locus_to_str(locus));
        *nodecl_output = nodecl_make_err_expr(locus);
    }
    else
    {
        *nodecl_output = nodecl_make_symbol(this_symbol, locus);
        // Note that 'this' is an rvalue!
        nodecl_set_type(*nodecl_output, this_symbol->type_information);
        if (is_dependent_type(this_symbol->type_information))
        {
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        }
        else if (check_expr_flags.must_be_constant)
        {
            if (decl_context.current_scope->kind == BLOCK_SCOPE
                    && !symbol_entity_specs_get_is_constexpr(decl_context.current_scope->related_entry))
            {
                error_printf("%s: error: 'this' referenced inside a non-constexpr member function or constructor\n",
                        locus_to_str(locus));
            }
        }
    }
}

static 
char operand_is_class_or_enum(type_t* op_type)
{
    return (is_enum_type(op_type)
            || is_class_type(op_type));
}

static 
char any_operand_is_class_or_enum(type_t* lhs_type, type_t* rhs_type)
{
    return operand_is_class_or_enum(lhs_type)
        || operand_is_class_or_enum(rhs_type);
}

static
char operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member(type_t* op_type, char allow_pointer_to_member)
{
    return (is_pointer_type(no_ref(op_type))
            || is_array_type(no_ref(op_type))
            || is_function_type(no_ref(op_type))
            || is_unresolved_overloaded_type(no_ref(op_type))
            || (allow_pointer_to_member && is_pointer_to_member_type(no_ref(op_type))));
}

static
char any_operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member_flags(type_t* lhs_type,
        type_t* rhs_type,
        char allow_pointer_to_member)
{
    return any_operand_is_class_or_enum(lhs_type, rhs_type)
        || operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member(lhs_type, allow_pointer_to_member)
        || operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member(rhs_type, allow_pointer_to_member);
}

static char any_operand_is_class_or_enum_or_pointer_or_array_or_function(type_t* lhs_type, type_t* rhs_type)
{
    return any_operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member_flags(lhs_type,
            rhs_type,
            /* allow_pointer_to_member */ 0);
}

static char any_operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member(type_t* lhs_type, type_t* rhs_type)
{
    return any_operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member_flags(lhs_type,
            rhs_type,
            /* allow_pointer_to_member */ 1);
}

static char is_promoteable_integral_type(type_t* t)
{
    return (is_signed_char_type(t)
            || is_unsigned_char_type(t)
            || is_signed_short_int_type(t)
            || is_unsigned_short_int_type(t)
            || is_bool_type(t)
            || is_unscoped_enum_type(t)
            || is_wchar_t_type(t)
            || is_char16_t_type(t)
            || is_char32_t_type(t));
}

static type_t* promote_integral_type(type_t* t)
{
    ERROR_CONDITION(!is_promoteable_integral_type(t), 
            "This type (%s) cannot be promoted!", print_declarator(t));

    if (is_unscoped_enum_type(t))
    {
        return enum_type_get_underlying_type(t);
    }
    else if (is_wchar_t_type(t))
    {
        C_LANGUAGE()
        {
            return CURRENT_CONFIGURATION->type_environment->int_type_of_wchar_t();
        }
        return get_signed_int_type();
    }
    else // char, bool or short
    {
        return get_signed_int_type();
    }
}

static char operand_is_arithmetic_or_unscoped_enum_type_noref(type_t* t, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_arithmetic_type(no_ref(t))
            || is_unscoped_enum_type(no_ref(t)));
}

static char operand_is_integral_or_enum_type(type_t* t, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_integral_type(t)
            || is_unscoped_enum_type(t));
}

static char operand_is_integral_or_bool_or_unscoped_enum_type_noref(type_t* t, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_integral_type(t)
            || is_unscoped_enum_type(t));
}

static
char both_operands_are_integral(type_t* lhs_type, type_t* rhs_type, const locus_t* locus)
{
    return (operand_is_integral_or_enum_type(rhs_type, locus)
            && operand_is_integral_or_enum_type(lhs_type, locus));
}

static 
char both_operands_are_integral_noref(type_t* lhs_type, type_t* rhs_type, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_integral_type(no_ref(lhs_type)) || is_unscoped_enum_type(no_ref(lhs_type)))
        && (is_integral_type(no_ref(rhs_type)) || is_unscoped_enum_type(no_ref(rhs_type)));
};

static 
char both_operands_are_arithmetic(type_t* lhs_type, type_t* rhs_type, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_arithmetic_type(lhs_type) || is_unscoped_enum_type(lhs_type))
        && (is_arithmetic_type(rhs_type) || is_unscoped_enum_type(rhs_type));
}

static 
char both_operands_are_arithmetic_noref(type_t* lhs_type, type_t* rhs_type, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_arithmetic_type(no_ref(lhs_type)) || is_unscoped_enum_type(no_ref(lhs_type)))
        && (is_arithmetic_type(no_ref(rhs_type)) || is_unscoped_enum_type(no_ref(rhs_type)));
}

static 
char both_operands_are_arithmetic_or_enum_noref(type_t* lhs_type, type_t* rhs_type, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_arithmetic_type(no_ref(lhs_type)) || is_enum_type(no_ref(lhs_type)))
        && (is_arithmetic_type(no_ref(rhs_type)) || is_enum_type(no_ref(rhs_type)));
}

static char both_operands_are_vector_types(type_t* lhs_type, type_t* rhs_type)
{
    return is_vector_type(lhs_type)
        && is_vector_type(rhs_type)
        && equivalent_types(get_unqualified_type(lhs_type), get_unqualified_type(rhs_type));
}

static char one_scalar_operand_and_one_vector_operand(type_t* lhs_type, type_t* rhs_type)
{
    return (is_vector_type(lhs_type) && is_arithmetic_type(rhs_type)) ||
           (is_vector_type(rhs_type) && is_arithmetic_type(lhs_type));
}

static char left_operand_is_vector_and_right_operand_is_scalar(type_t* lhs_type, type_t* rhs_type)
{
    return (is_vector_type(lhs_type) && is_arithmetic_type(rhs_type));
}

static char is_pointer_and_integral_type(type_t* lhs_type, type_t* rhs_type)
{
    if (is_array_type(lhs_type))
    {
        // Convert to a pointer
        lhs_type = get_pointer_type(array_type_get_element_type(lhs_type));
    }

    return (is_pointer_type(lhs_type)
            && (is_integral_type(rhs_type) || is_unscoped_enum_type(rhs_type)));
}

static 
char is_pointer_arithmetic(type_t* lhs_type, type_t* rhs_type)
{
    return is_pointer_and_integral_type(lhs_type, rhs_type)
        || is_pointer_and_integral_type(rhs_type, lhs_type);
}

static char long_int_can_represent_unsigned_int(void)
{
    static char result = 2;

    if (result == 2)
    {
        const_value_t* min_signed_long_int = integer_type_get_minimum(get_signed_long_int_type());
        const_value_t* max_signed_long_int = integer_type_get_maximum(get_signed_long_int_type());

        const_value_t* min_unsigned_int = integer_type_get_minimum(get_unsigned_int_type());
        const_value_t* max_unsigned_int = integer_type_get_maximum(get_unsigned_int_type());

#define B_(x) const_value_is_nonzero(x)
        if (B_(const_value_lte(min_signed_long_int, min_unsigned_int))
                && B_(const_value_lte(max_unsigned_int, max_signed_long_int)))
        {
            result = 1;
        }
        else
        {
            result = 0;
        }
#undef B_
    }

    return result;
}

static char long_long_int_can_represent_unsigned_long_int(void)
{
    static char result = 2;

    if (result == 2)
    {
        const_value_t* min_signed_long_long_int = integer_type_get_minimum(get_signed_long_long_int_type());
        const_value_t* max_signed_long_long_int = integer_type_get_maximum(get_signed_long_long_int_type());

        const_value_t* min_unsigned_long_int = integer_type_get_minimum(get_unsigned_long_int_type());
        const_value_t* max_unsigned_long_int = integer_type_get_maximum(get_unsigned_long_int_type());

#define B_(x) const_value_is_nonzero(x)
        if (B_(const_value_lte(min_signed_long_long_int, min_unsigned_long_int))
                && B_(const_value_lte(max_unsigned_long_int, max_signed_long_long_int)))
        {
            result = 1;
        }
        else
        {
            result = 0;
        }
#undef B_
    }

    return result;
}

static type_t* usual_arithmetic_conversions(type_t* lhs_type, type_t* rhs_type, const locus_t* locus)
{
    ERROR_CONDITION (!both_operands_are_arithmetic_noref(lhs_type, rhs_type, locus),
            "Both should be arithmetic types", 0);

    if (is_unscoped_enum_type(lhs_type))
    {
        lhs_type = enum_type_get_underlying_type(lhs_type);
    }
    if (is_unscoped_enum_type(rhs_type))
    {
        rhs_type = enum_type_get_underlying_type(rhs_type);
    }

    char is_mask = is_mask_type(lhs_type)
        || is_mask_type(rhs_type);
    if (is_mask_type(lhs_type))
    {
        lhs_type = mask_type_get_underlying_type(lhs_type);
    }
    if (is_mask_type(rhs_type))
    {
        rhs_type = mask_type_get_underlying_type(rhs_type);
    }

    char is_complex = is_complex_type(lhs_type)
        || is_complex_type(rhs_type); 

    if (is_complex_type(lhs_type))
    {
        lhs_type = complex_type_get_base_type(lhs_type);
    }
    if (is_complex_type(rhs_type))
    {
        rhs_type = complex_type_get_base_type(rhs_type);
    }

    // Floating point case is easy
    if (is_floating_type(lhs_type)
            || is_floating_type(rhs_type))
    {
        type_t* result = NULL;
        if (is_long_double_type(lhs_type)
                || is_long_double_type(rhs_type))
        {
            result = get_long_double_type();
        }
        else if (is_double_type(lhs_type)
                || is_double_type(rhs_type))
        {
            result = get_double_type();
        }
        else
        {
            result = get_float_type();
        }

        if (is_complex)
        {
            result = get_complex_type(result);
        }

        return result;
    }

    // char16_t -> uint_least16_t
    if (is_char16_t_type(lhs_type))
    {
        // FIXME - Should be uint_least16_t
        lhs_type = get_unsigned_short_int_type();
    }
    if (is_char16_t_type(rhs_type))
    {
        // FIXME - Should be uint_least16_t
        rhs_type = get_unsigned_short_int_type();
    }

    // char32_t -> uint_least32_t
    if (is_char32_t_type(lhs_type))
    {
        // FIXME - Should be uint_least32_t
        lhs_type = get_unsigned_int_type();
    }
    if (is_char32_t_type(rhs_type))
    {
        // FIXME - Should be uint_least32_t
        rhs_type = get_unsigned_int_type();
    }

    // Perform integral promotions
    {
        type_t** types[2] = {&lhs_type, &rhs_type};
        int i;
        for (i = 0; i < 2; i++)
        {
            type_t** curr_type = types[i];

            if (is_promoteable_integral_type(*curr_type))
                (*curr_type) = promote_integral_type((*curr_type));
        }
    }

    ERROR_CONDITION(!is_any_int_type(lhs_type) || !is_any_int_type(rhs_type),
            "Error, the types are wrong, they should be either of integer nature at this point", 0);

    type_t* result = NULL;
    // If either is unsigned long long, convert to unsigned long long
    if (is_unsigned_long_long_int_type(lhs_type)
            || is_unsigned_long_long_int_type(rhs_type))
    {
        result = get_unsigned_long_long_int_type();
    }
    // If one operand is a long long int and the other unsigned long int, then
    // if a long long int can represent all the values of an unsigned long int,
    // the unsigned long int shall be converted to a long long int; otherwise
    // both operands shall be converted to unsigned long long int.
    else if ((is_signed_long_long_int_type(lhs_type)
                && is_unsigned_long_int_type(rhs_type))
            || (is_signed_long_long_int_type(rhs_type)
                && is_unsigned_long_int_type(lhs_type)))
    {
        if (long_long_int_can_represent_unsigned_long_int())
        {
            result = get_signed_long_long_int_type();
        }
        else
        {
            result = get_unsigned_long_long_int_type();
        }
    }
    // If either is signed long long, convert to signed long long
    else if (is_signed_long_long_int_type(lhs_type)
            || is_signed_long_long_int_type(rhs_type))
    {
        result = get_signed_long_long_int_type();
    }
    // If either is unsigned long convert to unsigned long
    else if (is_unsigned_long_int_type(lhs_type)
            || is_unsigned_long_int_type(rhs_type))
    {
        result = get_unsigned_long_int_type();
    }
    // If one operand is a long int and the other unsigned int, then if a long
    // int can represent all the values of an unsigned int, the unsigned int
    // shall be converted to a long int; otherwise both operands shall be
    // converted to unsigned long int.
    else if ((is_signed_long_int_type(lhs_type)
                && is_unsigned_int_type(rhs_type))
            || (is_signed_long_int_type(rhs_type)
                && is_unsigned_int_type(lhs_type)))
    {
        if (long_int_can_represent_unsigned_int())
        {
            result = get_signed_long_int_type();
        }
        else
        {
            result = get_unsigned_long_int_type();
        }
    }
    // If either is signed long, convert to signed long
    else if (is_signed_long_int_type(lhs_type)
            || is_signed_long_int_type(rhs_type))
    {
        result = get_signed_long_int_type();
    }
    // If either is unsigned int the the other should be
    else if (is_unsigned_int_type(lhs_type)
            || is_unsigned_int_type(rhs_type))
    {
        result = get_unsigned_int_type();
    }
#if HAVE_INT128
    // If either is signed __int128, convert to signed __int128
    else if (is_signed_int128_type(lhs_type)
            || is_signed_int128_type(rhs_type))
    {
        result = get_signed_int128_type();
    }
    // If either is unsigned __int128, convert to unsigned __int128
    else if (is_unsigned_int128_type(lhs_type)
            || is_unsigned_int128_type(rhs_type))
    {
        result = get_unsigned_int128_type();
    }
#endif
    // both should be int here
    else if (!is_signed_int_type(lhs_type)
            || !is_signed_int_type(rhs_type))
    {
        internal_error("Unreachable code", 0);
    }
    else 
    {
        result = get_signed_int_type();
    }

    if (is_complex)
    {
        result = get_complex_type(result);
    }

    if (is_mask)
    {
        result = get_mask_type(type_get_size(result) * 8);
    }

    return result;
}

static
type_t* compute_arithmetic_builtin_bin_op(type_t* lhs_type, type_t* rhs_type, const locus_t* locus)
{
    return usual_arithmetic_conversions(lhs_type, rhs_type, locus);
}

static type_t* compute_pointer_arithmetic_type(type_t* lhs_type, type_t* rhs_type)
{
    type_t* return_type = NULL;

    type_t* types[2] = {lhs_type, rhs_type};

    int i;
    for (i = 0; i < 2; i++)
    {
        type_t* current_type = types[i];

        if (is_pointer_type(current_type) || is_array_type(current_type))
        {
            if (is_pointer_type(current_type))
            {
                return_type = current_type;
            }
            else if (is_array_type(current_type))
            {
                return_type = get_pointer_type(array_type_get_element_type(current_type));
            }
        }
    }

    return return_type;
}

static type_t * compute_scalar_vector_type(type_t* lhs_type, type_t* rhs_type)
{
    if (is_vector_type(lhs_type))
        return lhs_type;
    else
        return rhs_type;
}

static char filter_only_nonmembers(scope_entry_t* e, void* p UNUSED_PARAMETER)
{
    if (e->kind != SK_FUNCTION 
            && e->kind != SK_TEMPLATE)
        return 0;

    if (e->kind == SK_TEMPLATE)
    {
        e = named_type_get_symbol(template_type_get_primary_type(e->type_information));
        if (e->kind != SK_FUNCTION)
            return 0;
    }

    if (!symbol_entity_specs_get_is_member(e))
        return 1;

    return 0;
}

static void error_message_delete_call(decl_context_t decl_context, scope_entry_t* entry, const locus_t* locus)
{
    error_printf("%s: error: call to deleted function '%s'\n",
            locus_to_str(locus),
            print_decl_type_str(entry->type_information, decl_context,
                get_qualified_symbol_name(entry, decl_context)));
}

char function_has_been_deleted(decl_context_t decl_context, scope_entry_t* entry, const locus_t* locus)
{
    char c = symbol_entity_specs_get_is_deleted(entry);
    if (c)
    {
        error_message_delete_call(decl_context, entry, locus);
    }
    return c;
}

static void error_message_overload_failed(candidate_t* candidates, 
        const char* name,
        decl_context_t decl_context,
        int num_arguments, type_t** arguments,
        type_t* this_type,
        const locus_t* locus);

static void update_unresolved_overload_argument(type_t* arg_type,
        type_t* param_type,
        decl_context_t decl_context,
        const locus_t* locus,

        nodecl_t* nodecl_output)
{
    scope_entry_list_t* unresolved_set = unresolved_overloaded_type_get_overload_set(arg_type);
    scope_entry_t* solved_function = address_of_overloaded_function(
            unresolved_set,
            unresolved_overloaded_type_get_explicit_template_arguments(arg_type),
            no_ref(param_type),
            decl_context,
            locus);

    ERROR_CONDITION(solved_function == NULL, "Code unreachable", 0);

    if (!symbol_entity_specs_get_is_member(solved_function)
            || symbol_entity_specs_get_is_static(solved_function))
    {
        *nodecl_output = nodecl_make_symbol(solved_function, locus);
        nodecl_set_type(*nodecl_output, lvalue_ref(solved_function->type_information));
    }
    else
    {
        *nodecl_output = nodecl_make_pointer_to_member(solved_function,
                get_pointer_to_member_type(solved_function->type_information,
                    symbol_entity_specs_get_class_type(solved_function)),
                locus);
    }
}

static void check_nodecl_function_argument_initialization_(
        nodecl_t nodecl_expr,
        decl_context_t decl_context,
        type_t* declared_type,
        enum initialization_kind initialization_kind,
        char disallow_narrowing,
        nodecl_t* nodecl_output);

static type_t* compute_user_defined_bin_operator_type(AST operator_name, 
        nodecl_t *lhs, nodecl_t *rhs, 
        scope_entry_list_t* builtins,
        decl_context_t decl_context,
        const locus_t* locus,
        scope_entry_t** selected_operator)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    type_t* argument_types[2] = { lhs_type, rhs_type };
    int num_arguments = 2;

    candidate_t* candidate_set = NULL;

    scope_entry_list_t* operator_overload_set = NULL;
    if (is_class_type(no_ref(lhs_type)))
    {
        scope_entry_list_t* operator_entry_list = get_member_of_class_type(no_ref(lhs_type), 
                operator_name, decl_context, NULL);

        operator_overload_set = unfold_and_mix_candidate_functions(operator_entry_list,
                NULL, argument_types + 1, num_arguments - 1,
                decl_context,
                locus,
                /* explicit template arguments */ NULL);
        entry_list_free(operator_entry_list);
    }

    // This uses Koenig, otherwise some operators might not be found
    nodecl_t nodecl_op_name = 
        nodecl_make_cxx_dep_name_simple(get_operator_function_name(operator_name),
                locus);
    scope_entry_list_t *entry_list = koenig_lookup(num_arguments,
            argument_types, decl_context, nodecl_op_name, locus);

    nodecl_free(nodecl_op_name); // Not used anymore

    // Normal lookup might find member functions at this point, filter them
    scope_entry_list_t* nonmember_entry_list = filter_symbol_using_predicate(entry_list, filter_only_nonmembers, NULL);
    entry_list_free(entry_list);
    
    scope_entry_list_t* overload_set = unfold_and_mix_candidate_functions(nonmember_entry_list,
            builtins, argument_types, num_arguments,
            decl_context,
            locus,
            /* explicit template arguments */ NULL);
    entry_list_free(nonmember_entry_list);

    scope_entry_list_t* old_overload_set = overload_set;
    overload_set = entry_list_merge(old_overload_set, operator_overload_set);
    entry_list_free(old_overload_set);
    entry_list_free(operator_overload_set);

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(overload_set);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        candidate_set = candidate_set_add(candidate_set,
                entry_list_iterator_current(it),
                num_arguments,
                argument_types);
    }
    entry_list_iterator_free(it);
    entry_list_free(overload_set);

    scope_entry_t *orig_overloaded_call = solve_overload(candidate_set,
            decl_context,
            locus);
    scope_entry_t* overloaded_call = entry_advance_aliases(orig_overloaded_call);

    type_t* overloaded_type = NULL;
    if (overloaded_call != NULL)
    {
        candidate_set_free(&candidate_set);
        if (function_has_been_deleted(decl_context, overloaded_call, locus))
        {
            return get_error_type();
        }

        if (!symbol_entity_specs_get_is_member(overloaded_call))
        {
            type_t* param_type_0 = function_type_get_parameter_type_num(overloaded_call->type_information, 0);

            nodecl_t old_lhs = *lhs;

            check_nodecl_function_argument_initialization_(*lhs,
                    decl_context,
                    param_type_0,
                    builtin_needs_contextual_conversion(
                        overloaded_call,
                        0,
                        param_type_0)
                    ? IK_DIRECT_INITIALIZATION : IK_COPY_INITIALIZATION,
                    /* disallow_narrowing */ 0,
                    lhs);
            if (nodecl_is_err_expr(*lhs))
            {
                nodecl_free(old_lhs);
                return get_error_type();
            }
        }

        type_t* param_type_1 = NULL;
        
        if (!symbol_entity_specs_get_is_member(overloaded_call))
        {
            param_type_1 = function_type_get_parameter_type_num(overloaded_call->type_information, 1);
        }
        else
        {
            param_type_1 = function_type_get_parameter_type_num(overloaded_call->type_information, 0);
        }

        nodecl_t old_rhs = *rhs;
        check_nodecl_function_argument_initialization_(*rhs,
                decl_context,
                param_type_1,
                builtin_needs_contextual_conversion(
                    overloaded_call,
                    1,
                    param_type_1)
                ? IK_DIRECT_INITIALIZATION : IK_COPY_INITIALIZATION,
                /* disallow_narrowing */ 0,
                rhs);
        if (nodecl_is_err_expr(*rhs))
        {
            nodecl_free(old_rhs);
            return get_error_type();
        }

        *selected_operator = overloaded_call;

        overloaded_type = function_type_get_return_type(overloaded_call->type_information);
    }
    else 
    {
        error_message_overload_failed(candidate_set, 
                prettyprint_in_buffer(operator_name),
                decl_context,
                num_arguments, argument_types,
                /* implicit_argument */ NULL,
                locus);
        candidate_set_free(&candidate_set);
        overloaded_type = get_error_type();
    }
    return overloaded_type;
}


static type_t* compute_user_defined_unary_operator_type(AST operator_name, 
        nodecl_t* op,
        scope_entry_list_t* builtins,
        decl_context_t decl_context,
        const locus_t* locus,
        scope_entry_t** selected_operator)

{
    type_t* op_type = nodecl_get_type(*op);

    type_t* argument_types[1] = { op_type };
    int num_arguments = 1;

    candidate_t* candidate_set = NULL;

    if (is_class_type(no_ref(op_type)))
    {
        scope_entry_list_t* operator_entry_list = get_member_of_class_type(no_ref(op_type), 
                operator_name, decl_context, NULL);

        scope_entry_list_iterator_t *it = NULL;
        for (it = entry_list_iterator_begin(operator_entry_list);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it))
        {
            scope_entry_t* orig_entry = entry_list_iterator_current(it);
            scope_entry_t* entry = entry_advance_aliases(orig_entry);
            // It is impossible to deduce anything since a unary overloaded
            // operator has zero parameters, so discard templates at this point
            if (entry->kind != SK_TEMPLATE)
            {
                candidate_set = candidate_set_add(candidate_set,
                        orig_entry,
                        num_arguments,
                        argument_types);
            }
        }
        entry_list_iterator_free(it);
        entry_list_free(operator_entry_list);
    }

    nodecl_t nodecl_op_name = 
        nodecl_make_cxx_dep_name_simple(get_operator_function_name(operator_name),
                locus);

    scope_entry_list_t *entry_list = koenig_lookup(num_arguments,
            argument_types, decl_context, nodecl_op_name, locus);

    nodecl_free(nodecl_op_name); // Not used anymore

    // Remove any member that might have slip in because of plain lookup
    scope_entry_list_t* nonmember_entry_list = filter_symbol_using_predicate(entry_list, filter_only_nonmembers, NULL);
    entry_list_free(entry_list);
    
    scope_entry_list_t* overload_set = unfold_and_mix_candidate_functions(
            nonmember_entry_list, builtins, argument_types, num_arguments,
            decl_context,
            locus,
            /* explicit_template_arguments */ NULL);
    entry_list_free(nonmember_entry_list);

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(overload_set);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        candidate_set = candidate_set_add(candidate_set,
                entry_list_iterator_current(it),
                num_arguments,
                argument_types);
    }
    entry_list_iterator_free(it);
    entry_list_free(overload_set);

    scope_entry_t *orig_overloaded_call = solve_overload(candidate_set,
            decl_context, 
            locus);
    scope_entry_t* overloaded_call = entry_advance_aliases(orig_overloaded_call);

    type_t* overloaded_type = NULL;
    if (overloaded_call != NULL)
    {
        candidate_set_free(&candidate_set);
        if (function_has_been_deleted(decl_context, overloaded_call, locus))
        {
            return get_error_type();
        }

        if (!symbol_entity_specs_get_is_member(overloaded_call))
        {
            type_t* param_type = function_type_get_parameter_type_num(overloaded_call->type_information, 0);

            nodecl_t old_op = *op;
            check_nodecl_function_argument_initialization_ (
                    *op,
                    decl_context,
                    param_type,
                    builtin_needs_contextual_conversion(
                        overloaded_call,
                        0,
                        param_type)
                    ? IK_DIRECT_INITIALIZATION : IK_COPY_INITIALIZATION,
                    /* disallow_narrowing */ 0,
                    op);
            if (nodecl_is_err_expr(*op))
            {
                nodecl_free(old_op);
                return get_error_type();
            }
        }

        *selected_operator = overloaded_call;

        overloaded_type = function_type_get_return_type(overloaded_call->type_information);
    }
    else 
    {
        error_message_overload_failed(candidate_set, 
                prettyprint_in_buffer(operator_name),
                decl_context,
                num_arguments, argument_types,
                /* implicit_argument */ NULL,
                locus);
        candidate_set_free(&candidate_set);
        overloaded_type = get_error_type();
    }
    return overloaded_type;
}

static char operator_bin_plus_builtin_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    // <arithmetic> + <arithmetic>
    return ((is_arithmetic_type(no_ref(lhs))
                && is_arithmetic_type(no_ref(rhs)))
            // T* + <arithmetic>
            || ((is_pointer_type(no_ref(lhs)) || is_array_type(no_ref(lhs)))
                && is_arithmetic_type(no_ref(rhs)))
            // <arithmetic> + T*
            || (is_arithmetic_type(no_ref(lhs))
                && (is_pointer_type(no_ref(rhs)) || is_array_type(no_ref(rhs)))));
}

static type_t* operator_bin_plus_builtin_result(type_t** lhs, type_t** rhs, const locus_t* locus)
{
    if (is_arithmetic_type(no_ref(*lhs))
        && is_arithmetic_type(no_ref(*rhs)))
    {
        *lhs = get_unqualified_type(no_ref(*lhs));
        *rhs = get_unqualified_type(no_ref(*rhs));

        if (is_promoteable_integral_type(*lhs))
            *lhs = promote_integral_type(*lhs);

        if (is_promoteable_integral_type(*rhs))
            *rhs = promote_integral_type(*rhs);

        return usual_arithmetic_conversions(*lhs, *rhs, locus);
    }
    else if (is_pointer_arithmetic(no_ref(*lhs), no_ref(*rhs)))
    {
        *lhs = no_ref(*lhs);
        *rhs = no_ref(*rhs);

        type_t** pointer_type = NULL;
        type_t** index_type = NULL;
        if (is_pointer_type(*lhs)
                || is_array_type(*lhs))
        {
            pointer_type = lhs;
            index_type = rhs;
        }
        else
        {
            pointer_type = rhs;
            index_type = lhs;
        }

        if (is_array_type(*pointer_type))
        {
            *pointer_type = get_pointer_type(array_type_get_element_type(*pointer_type));
        }

        *index_type = get_ptrdiff_t_type();

        return *pointer_type;
    }

    return get_error_type();
}

static void unary_record_conversion_to_result(type_t* result, nodecl_t* op)
{
    type_t* op_type = nodecl_get_type(*op);

    if (!equivalent_types(result, op_type))
    {
        *op = cxx_nodecl_make_conversion(*op, result,
                nodecl_get_locus(*op));
    }
}

static void binary_record_conversion_to_result(type_t* result, nodecl_t* lhs, nodecl_t* rhs)
{
    unary_record_conversion_to_result(result, lhs);
    unary_record_conversion_to_result(result, rhs);
}

static
type_t* compute_type_no_overload_add_operation(nodecl_t *lhs, nodecl_t *rhs,
        const locus_t* locus)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (both_operands_are_arithmetic(no_ref(lhs_type), no_ref(rhs_type), locus))
    {
        type_t* result = compute_arithmetic_builtin_bin_op(no_ref(lhs_type), no_ref(rhs_type), locus);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else if (is_pointer_arithmetic(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = compute_pointer_arithmetic_type(no_ref(lhs_type), no_ref(rhs_type));

        if (is_pointer_and_integral_type(
                    no_ref(lhs_type),
                    no_ref(rhs_type)))
        {
            unary_record_conversion_to_result(result, lhs);
            unary_record_conversion_to_result(no_ref(rhs_type), rhs);
        }
        else if (is_pointer_and_integral_type(
                    no_ref(rhs_type),
                    no_ref(lhs_type)))
        {
            unary_record_conversion_to_result(result, rhs);
            unary_record_conversion_to_result(no_ref(lhs_type), lhs);
        }

        return result;
    }
    // Vector case
    else if (both_operands_are_vector_types(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = no_ref(lhs_type);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else if (one_scalar_operand_and_one_vector_operand(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = compute_scalar_vector_type(no_ref(lhs_type), no_ref(rhs_type));

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else
    {
        return get_error_type();
    }
}

static char operator_bin_only_arithmetic_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_arithmetic_type(no_ref(lhs))
            && is_arithmetic_type(no_ref(rhs)));
}

static type_t* operator_bin_only_arithmetic_result(type_t** lhs, type_t** rhs, const locus_t* locus)
{
    *lhs = get_unqualified_type(no_ref(*lhs));
    if (is_promoteable_integral_type(*lhs))
        *lhs = promote_integral_type(*lhs);

    *rhs = get_unqualified_type(no_ref(*rhs));
    if (is_promoteable_integral_type(*rhs))
        *rhs = promote_integral_type(*rhs);

    return usual_arithmetic_conversions(*lhs, *rhs, locus);
}

static char is_valid_reference_to_nonstatic_member_function(nodecl_t n, decl_context_t decl_context)
{
    char result = (nodecl_get_kind(n) == NODECL_REFERENCE
            && ((nodecl_get_kind(nodecl_get_child(n, 0)) == NODECL_CXX_DEP_NAME_NESTED)
                || (nodecl_get_kind(nodecl_get_child(n, 0)) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED)));

    if (!result)
    {
        error_printf("%s: error: invalid reference to nonstatic member function '%s'\n",
                nodecl_locus_to_str(n),
                codegen_to_str(n, decl_context));
    }

    return result;
}

static char update_simplified_unresolved_overloaded_type(scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t *nodecl_output)
{
    function_has_been_deleted(decl_context, entry, locus);
    if (!symbol_entity_specs_get_is_member(entry)
            || symbol_entity_specs_get_is_static(entry))
    {
        *nodecl_output = 
            nodecl_make_symbol(entry, locus);
        nodecl_set_type(*nodecl_output, lvalue_ref(entry->type_information));
    }
    else
    {
        if (!is_valid_reference_to_nonstatic_member_function(*nodecl_output, decl_context))
        {
            return 0;
        }
        else
        {
            nodecl_set_type(*nodecl_output, 
                    get_pointer_to_member_type(
                        entry->type_information,
                        symbol_entity_specs_get_class_type(entry)));
        }
    }

    return 1;
}


// Generic function for binary typechecking in C and C++
static
void compute_bin_operator_generic(
        nodecl_t* lhs, nodecl_t* rhs, 
        // Operator name and context
        AST operator, 
        decl_context_t decl_context,
        // Functions
        char (*will_require_overload)(type_t*, type_t*),
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t* locus),
        const_value_t* (*const_value_bin_fun)(const_value_t*, const_value_t*),
        type_t* (*compute_type_no_overload)(nodecl_t*, nodecl_t*, const locus_t* locus),
        char types_allow_constant_evaluation(type_t*, type_t*, const locus_t* locus),
        char (*overload_operator_predicate)(type_t*, type_t*, const locus_t* locus),
        type_t* (*overload_operator_result_types)(type_t**, type_t**, const locus_t* locus),
        // Locus
        const locus_t* locus,

        nodecl_t* nodecl_output)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);
    
    if (nodecl_expr_is_type_dependent(*lhs)
            || nodecl_expr_is_type_dependent(*rhs))
    {
        *nodecl_output = nodecl_bin_fun(*lhs, *rhs,  
                get_unknown_dependent_type(),
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);

        if (// If the expression can be constant and any of the operands is value dependent, so it is
                const_value_bin_fun != NULL
                && (nodecl_expr_is_value_dependent(*lhs)
                    || nodecl_expr_is_value_dependent(*rhs)))
        {
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }
        return;
    }

    const_value_t* val = NULL;

    char requires_overload = 0;
    CXX_LANGUAGE()
    {
        // Try to simplify unresolved overloads
        struct 
        {
            nodecl_t* op;
            type_t** op_type;
        } info[] = 
        { 
            { lhs, &lhs_type},
            { rhs, &rhs_type},
            //Sentinel
            { NULL, NULL}
        };
        int i;
        for (i = 0; info[i].op != NULL; i++)
        {
            type_t* current_type = no_ref(*info[i].op_type);

            if (is_unresolved_overloaded_type(current_type))
            {
                scope_entry_t* function = unresolved_overloaded_type_simplify(current_type,
                        decl_context, locus);
                if (function != NULL)
                {
                    if (!update_simplified_unresolved_overloaded_type(
                                function,
                                decl_context,
                                locus,
                                info[i].op))
                    {
                        *nodecl_output = nodecl_make_err_expr(locus);
                        return;
                    }
                    *(info[i].op_type) = nodecl_get_type(*(info[i].op));
                }
            }
        }
        requires_overload = will_require_overload(no_ref(lhs_type), no_ref(rhs_type));
    }

    if (!requires_overload)
    {
        type_t* computed_type = compute_type_no_overload(lhs, rhs, locus);

        if (is_error_type(computed_type))
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        *nodecl_output = nodecl_bin_fun(*lhs, *rhs,
                computed_type, locus);

        if (const_value_bin_fun != NULL
                && types_allow_constant_evaluation(lhs_type, rhs_type, locus)
                && nodecl_is_constant(*lhs)
                && nodecl_is_constant(*rhs))
        {
            val = const_value_bin_fun(nodecl_get_constant(*lhs), 
                    nodecl_get_constant(*rhs));
        }

        nodecl_set_constant(*nodecl_output, val);

        if (nodecl_expr_is_value_dependent(*lhs)
                || nodecl_expr_is_value_dependent(*rhs))
        {
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }
        return;
    }

    builtin_operators_set_t builtin_set;
    build_binary_builtin_operators(
            lhs_type, rhs_type, 
            &builtin_set,
            decl_context, operator, 
            overload_operator_predicate,
            overload_operator_result_types,
            locus);

    scope_entry_list_t* builtins = get_entry_list_from_builtin_operator_set(&builtin_set);

    scope_entry_t* selected_operator = NULL;

    // Now in C++ we have to rely on overloading for operators
    type_t* result = compute_user_defined_bin_operator_type(operator, 
            lhs, rhs, builtins, decl_context, locus, &selected_operator);

    ERROR_CONDITION(result == NULL, "Invalid type", 0);

    entry_list_free(builtins);

    if (selected_operator != NULL)
    {
        if (symbol_entity_specs_get_is_builtin(selected_operator))
        {
            if (const_value_bin_fun != NULL
                    && types_allow_constant_evaluation(lhs_type, rhs_type, locus)
                    && nodecl_is_constant(*lhs)
                    && nodecl_is_constant(*rhs))
            {
                val = const_value_bin_fun(nodecl_get_constant(*lhs), 
                        nodecl_get_constant(*rhs));
            }

            type_t* computed_type = compute_type_no_overload(lhs, rhs, locus);

            ERROR_CONDITION(is_error_type(computed_type), 
                    "Compute type no overload cannot deduce a type for a builtin solved overload lhs=%s rhs=%s\n",
                    print_declarator(nodecl_get_type(*lhs)),
                    print_declarator(nodecl_get_type(*rhs)));

            ERROR_CONDITION(!equivalent_types(result, computed_type), 
                "Mismatch between the types of builtin functions (%s) and result of no overload type (%s) at %s\n",
                print_declarator(result),
                print_declarator(computed_type),
                locus_to_str(locus));

            *nodecl_output = 
                nodecl_bin_fun(
                        *lhs,
                        *rhs,
                        computed_type, 
                        locus);

            nodecl_set_constant(*nodecl_output, val);

            if (nodecl_expr_is_value_dependent(*lhs)
                    || nodecl_expr_is_value_dependent(*rhs))
            {
                nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
            }
        }
        else
        {
            nodecl_t nodecl_selected_op =
                        nodecl_make_symbol(selected_operator, locus);
            nodecl_set_type(nodecl_selected_op, selected_operator->type_information);

            *nodecl_output = cxx_nodecl_make_function_call(
                    nodecl_selected_op,
                    /* called name */ nodecl_null(),
                    nodecl_make_list_2(*lhs, *rhs),
                    nodecl_make_cxx_function_form_binary_infix(locus),
                    result,
                    decl_context,
                    locus);
        }
    }
    else
    {
        *nodecl_output = nodecl_make_err_expr(locus);
    }
}

static
type_t* compute_type_no_overload_bin_arithmetic(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (both_operands_are_arithmetic(no_ref(lhs_type), no_ref(rhs_type), locus))
    {
        type_t* result = compute_arithmetic_builtin_bin_op(no_ref(lhs_type), no_ref(rhs_type), locus);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    // Vector case
    else if (both_operands_are_vector_types(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = no_ref(lhs_type);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else if (one_scalar_operand_and_one_vector_operand(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = compute_scalar_vector_type(no_ref(lhs_type), no_ref(rhs_type));

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else
    {
        return get_error_type();
    }
}

static
void compute_bin_operator_add_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    // Now in C++ we have to rely on overloading for operators
    static AST operation_add_tree = NULL;
    if (operation_add_tree == NULL)
    {
        operation_add_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_ADD_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_generic(lhs, rhs, 
            operation_add_tree, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_make_add,
            const_value_add,
            compute_type_no_overload_add_operation,
            both_operands_are_arithmetic_noref,
            operator_bin_plus_builtin_pred,
            operator_bin_plus_builtin_result,
            locus,
            nodecl_output);
}

static
void compute_bin_operator_only_arithmetic_types(nodecl_t* lhs, nodecl_t* rhs, 
        AST operator, 
        decl_context_t decl_context,
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t* locus),
        const_value_t* (*const_value_bin_fun)(const_value_t*, const_value_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_bin_fun,
            const_value_bin_fun,
            compute_type_no_overload_bin_arithmetic,
            both_operands_are_arithmetic_noref,
            operator_bin_only_arithmetic_pred,
            operator_bin_only_arithmetic_result,
            locus,
            nodecl_output);
}

static
void compute_bin_operator_mul_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_MUL_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_only_arithmetic_types(lhs, rhs, operation_tree, 
            decl_context, 
            nodecl_make_mul,
            const_value_mul,
            locus,
            nodecl_output);
}

static
void compute_bin_operator_pow_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    // No operation_tree for Fortran's **
    AST operation_tree = NULL;

    compute_bin_operator_only_arithmetic_types(lhs, rhs, operation_tree, 
            decl_context, 
            nodecl_make_power,
            const_value_pow,
            locus,
            nodecl_output);
}

static
char value_not_valid_for_divisor(const_value_t* v)
{
    if (const_value_is_zero(v))
        return 1;

    if (const_value_is_vector(v))
    {
        int num_elems = const_value_get_num_elements(v);
        int i;
        for (i = 0; i < num_elems; i++)
        {
            if (const_value_is_zero(const_value_get_element_num(v, i)))
                return 1;
        }
    }

    return 0;
}

static
void compute_bin_operator_div_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_DIV_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    const_value_t* (*const_value_div_safe)(const_value_t*, const_value_t*) = const_value_div;

    // We warn here because const_value_div_safe will not have enough
    // information
    if (both_operands_are_arithmetic_noref(
                nodecl_get_type(*lhs),
                nodecl_get_type(*rhs),
                locus)
            && nodecl_is_constant(*rhs)
            && value_not_valid_for_divisor(nodecl_get_constant(*rhs)))
    {
        warn_printf("%s: warning: division by zero\n",
                nodecl_locus_to_str(*rhs));
        // Disable constant evaluation
        const_value_div_safe = NULL;
    }

    compute_bin_operator_only_arithmetic_types(lhs, rhs, operation_tree,
            decl_context,
            nodecl_make_div,
            const_value_div_safe,
            locus,
            nodecl_output);
}

static char operator_bin_only_integer_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_integer_type(no_ref(lhs)) 
            && is_integer_type(no_ref(rhs)));
}

static type_t* operator_bin_only_integer_result(type_t** lhs, type_t** rhs, const locus_t* locus)
{
    *lhs = get_unqualified_type(no_ref(*lhs));
    if (is_promoteable_integral_type(*lhs))
        *lhs = promote_integral_type(*lhs);

    *rhs = get_unqualified_type(no_ref(*rhs));
    if (is_promoteable_integral_type(*rhs))
        *rhs = promote_integral_type(*rhs);

    return usual_arithmetic_conversions(*lhs, *rhs, locus);
}

static 
type_t* compute_type_no_overload_bin_only_integer(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (both_operands_are_integral(no_ref(lhs_type), no_ref(rhs_type), locus))
    {
        type_t* result = compute_arithmetic_builtin_bin_op(no_ref(lhs_type), no_ref(rhs_type), locus);

        binary_record_conversion_to_result(result, lhs, rhs);
        
        return result;
    }
    // Vector case
    else if (both_operands_are_vector_types(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = no_ref(lhs_type);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else if (one_scalar_operand_and_one_vector_operand(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = compute_scalar_vector_type(no_ref(lhs_type), no_ref(rhs_type));

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else
    {
        return get_error_type();
    }
}

static 
void compute_bin_operator_only_integer_types(nodecl_t* lhs, nodecl_t* rhs, 
        AST operator, 
        decl_context_t decl_context,
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t* locus),
        const_value_t* (*const_value_bin_fun)(const_value_t*, const_value_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_bin_fun,
            const_value_bin_fun,
            compute_type_no_overload_bin_only_integer,
            both_operands_are_arithmetic_noref,
            operator_bin_only_integer_pred,
            operator_bin_only_integer_result,
            locus,
            nodecl_output);
}

static
void compute_bin_operator_mod_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_MOD_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_only_integer_types(lhs, rhs, operation_tree, decl_context, 
            nodecl_make_mod, const_value_mod, 
            locus, nodecl_output);
}

static char operator_bin_sub_builtin_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    // <arithmetic> - <arithmetic>
    return ((is_arithmetic_type(no_ref(lhs))
                && is_arithmetic_type(no_ref(rhs)))
            // T* - <arithmetic>
            || ((is_pointer_type(no_ref(lhs)) || is_array_type(no_ref(lhs)))
                && is_arithmetic_type(no_ref(rhs))));
}

static type_t* operator_bin_sub_builtin_result(type_t** lhs, type_t** rhs, const locus_t* locus)
{
    if (is_arithmetic_type(no_ref(*lhs))
        && is_arithmetic_type(no_ref(*rhs)))
    {
        *lhs = get_unqualified_type(no_ref(*lhs));
        *rhs = get_unqualified_type(no_ref(*rhs));

        if (is_promoteable_integral_type(*lhs))
            *lhs = promote_integral_type(*lhs);

        if (is_promoteable_integral_type(*rhs))
            *rhs = promote_integral_type(*rhs);

        return usual_arithmetic_conversions(*lhs, *rhs, locus);
    }
    else if ((is_pointer_type(no_ref(*lhs)) || is_array_type(no_ref(*lhs)))
            || (is_pointer_type(no_ref(*rhs)) || is_array_type(no_ref(*rhs))))
    {
        *lhs = get_unqualified_type(no_ref(*lhs));
        if (is_array_type(*lhs))
            *lhs = get_pointer_type(array_type_get_element_type(*lhs));

        *rhs = get_unqualified_type(no_ref(*rhs));
        if (is_array_type(*rhs))
            *rhs = get_pointer_type(array_type_get_element_type(*rhs));

        return get_ptrdiff_t_type();
    }
    else if (is_pointer_type(no_ref(*lhs))
                && is_arithmetic_type(no_ref(*rhs)))
    {
        *lhs = no_ref(*lhs);
        *rhs = get_ptrdiff_t_type();

        type_t** pointer_type = lhs;

        if (is_array_type(*pointer_type))
        {
            *pointer_type = get_pointer_type(array_type_get_element_type(*pointer_type));
        }

        return *pointer_type;
    }

    return get_error_type();
}

static type_t* compute_type_no_overload_sub(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (both_operands_are_arithmetic(no_ref(lhs_type), no_ref(rhs_type), locus))
    {
        type_t* result = compute_arithmetic_builtin_bin_op(no_ref(lhs_type), no_ref(rhs_type), locus);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else if (is_pointer_and_integral_type(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = compute_pointer_arithmetic_type(no_ref(lhs_type), no_ref(rhs_type));

        unary_record_conversion_to_result(result, lhs);
        unary_record_conversion_to_result(no_ref(rhs_type), rhs);

        return result;
    }
    else if (pointer_types_are_similar(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = get_ptrdiff_t_type();
        return result;
    }
    // Vector case
    else if (both_operands_are_vector_types(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = no_ref(lhs_type);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else if (one_scalar_operand_and_one_vector_operand(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = compute_scalar_vector_type(no_ref(lhs_type), no_ref(rhs_type));

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else
    {
        return get_error_type();
    }
}

static 
void compute_bin_operator_sub_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operator = NULL;
    if (operator == NULL)
    {
        operator = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_MINUS_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_make_minus,
            const_value_sub,
            compute_type_no_overload_sub,
            both_operands_are_arithmetic_noref,
            operator_bin_sub_builtin_pred,
            operator_bin_sub_builtin_result,
            locus,
            nodecl_output);
}

static char operator_bin_left_integral_right_integral_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_integral_type(no_ref(lhs))
            && is_integral_type(no_ref(rhs)));
}

static type_t* operator_bin_left_integral_result(type_t** lhs, type_t** rhs, const locus_t* locus UNUSED_PARAMETER)
{
    *lhs = get_unqualified_type(no_ref(*lhs));
    if (is_promoteable_integral_type(*lhs))
    {
        *lhs = promote_integral_type(*lhs);
    }

    *rhs = get_unqualified_type(no_ref(*rhs));
    if (is_promoteable_integral_type(*rhs))
    {
        *rhs = promote_integral_type(*rhs);
    }

    return (*lhs);
}

static type_t* compute_type_no_overload_only_integral_lhs_type(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus UNUSED_PARAMETER)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (both_operands_are_integral(no_ref(lhs_type), no_ref(rhs_type), locus))
    {
        // Always the left one in this case
        type_t* result = get_unqualified_type(no_ref(lhs_type));
        if (is_unscoped_enum_type(result))
        {
            result = enum_type_get_underlying_type(result);
        }

        if (is_promoteable_integral_type(result))
            result = promote_integral_type(result);

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else if (left_operand_is_vector_and_right_operand_is_scalar(no_ref(lhs_type), no_ref(rhs_type)))
    {
        type_t* result = vector_type_get_element_type(no_ref(lhs_type));

        binary_record_conversion_to_result(result, lhs, rhs);

        return result;
    }
    else
    {
        return get_error_type();
    }
}

static
void compute_bin_operator_only_integral_lhs_type(nodecl_t* lhs, nodecl_t* rhs, 
        AST operator, 
        decl_context_t decl_context, 
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        const_value_t* (const_value_bin_fun)(const_value_t*, const_value_t*),
        const locus_t* locus, nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_bin_fun,
            const_value_bin_fun,
            compute_type_no_overload_only_integral_lhs_type,
            both_operands_are_integral_noref,
            operator_bin_left_integral_right_integral_pred,
            operator_bin_left_integral_result,
            locus,
            nodecl_output);
}

void compute_bin_operator_bitwise_shl_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_LEFT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_only_integral_lhs_type(lhs, rhs, 
            operation_tree, 
            decl_context, 
            nodecl_make_bitwise_shl,
            const_value_bitshl,
            locus, nodecl_output);
}

static nodecl_t nodecl_make_shr_common(nodecl_t lhs, nodecl_t rhs, type_t* t, const locus_t* locus,
        nodecl_t (*arithmetic_shr)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        nodecl_t (*bitwise_shr)(nodecl_t, nodecl_t, type_t*, const locus_t*))
{
    type_t* lhs_type = no_ref(nodecl_get_type(lhs));

    if (is_unscoped_enum_type(lhs_type))
    {
        lhs_type = enum_type_get_underlying_type(lhs_type);
    }

    if (is_signed_integral_type(lhs_type))
    {
        return arithmetic_shr(lhs, rhs, t, locus);
    }
    else
    {
        return bitwise_shr(lhs, rhs, t, locus);
    }
}

static nodecl_t nodecl_make_shr(nodecl_t lhs, nodecl_t rhs, type_t* t, const locus_t* locus)
{
    return nodecl_make_shr_common(lhs, rhs, t, locus,
            nodecl_make_arithmetic_shr,
            nodecl_make_bitwise_shr);
}

static nodecl_t nodecl_make_shr_assignment(nodecl_t lhs, nodecl_t rhs, type_t* t, const locus_t* locus)
{
    return nodecl_make_shr_common(lhs, rhs, t, locus,
            nodecl_make_arithmetic_shr_assignment,
            nodecl_make_bitwise_shr_assignment);
}

void compute_bin_operator_shr_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_RIGHT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_only_integral_lhs_type(lhs, rhs, 
            operation_tree, 
            decl_context, 
            nodecl_make_shr,
            const_value_shr,
            locus, nodecl_output);
}

static char operator_bin_arithmetic_pointer_or_enum_pred_flags(type_t* lhs,
        type_t* rhs,
        const locus_t* locus,
        char allow_pointer_to_member)
{
    // Two arithmetics or enum or pointer
    standard_conversion_t dummy;
    char lhs_is_ptr_like = (is_pointer_type(no_ref(lhs))
            || is_array_type(no_ref(lhs))
            || is_function_type(no_ref(lhs))
            || is_unresolved_overloaded_type(no_ref(lhs))
            || (allow_pointer_to_member
                && is_pointer_to_member_type(no_ref(lhs))));
    char rhs_is_ptr_like = (is_pointer_type(no_ref(rhs))
            || is_array_type(no_ref(rhs))
            || is_function_type(no_ref(rhs))
            || is_unresolved_overloaded_type(no_ref(rhs))
            || (allow_pointer_to_member
                && is_pointer_to_member_type(no_ref(rhs))));

    return ((is_arithmetic_type(no_ref(lhs))
                && is_arithmetic_type(no_ref(rhs)))
            // T* < T*
            || (lhs_is_ptr_like
                && rhs_is_ptr_like
                // We make a special check for void* because const T* becomes const void*
                // which cannot be converted to void* although the comparison is legal
                && (is_pointer_to_void_type(get_unqualified_type(no_ref(lhs)))
                    || is_pointer_to_void_type(get_unqualified_type(no_ref(rhs)))
                    || standard_conversion_between_types(&dummy,
                        get_unqualified_type(get_unqualified_type(no_ref(lhs))),
                        get_unqualified_type(get_unqualified_type(no_ref(rhs))),
                        locus)
                    || standard_conversion_between_types(&dummy,
                        get_unqualified_type(get_unqualified_type(no_ref(rhs))),
                        get_unqualified_type(get_unqualified_type(no_ref(lhs))),
                        locus)
                   )
               )
            || (is_zero_type_or_nullptr_type(no_ref(lhs)) && rhs_is_ptr_like)
            || (lhs_is_ptr_like && is_zero_type_or_nullptr_type(no_ref(rhs)))
            // enum E < enum E
            || (is_enum_type(no_ref(lhs))
                && is_enum_type(no_ref(rhs))
                && equivalent_types(get_unqualified_type(no_ref(lhs)), get_unqualified_type(no_ref(rhs)))));
}

static char operator_bin_arithmetic_pointer_or_enum_pred(type_t* lhs, type_t* rhs, const locus_t* locus)
{
    return operator_bin_arithmetic_pointer_or_enum_pred_flags(lhs, rhs, locus, /* allow pointer to member */0);
}

static char operator_bin_arithmetic_pointer_or_pointer_to_member_or_enum_pred(type_t* lhs, type_t* rhs, const locus_t* locus)
{
    return operator_bin_arithmetic_pointer_or_enum_pred_flags(lhs, rhs, locus, /* allow pointer to member */1);
}

static type_t* operator_bin_arithmetic_pointer_or_pointer_to_member_or_enum_result_flags(
        type_t** lhs,
        type_t** rhs,
        const locus_t* locus,
        char allow_pointer_to_member)
{
    standard_conversion_t scs;

    // x == y
    if (is_arithmetic_type(no_ref(*lhs))
            && is_arithmetic_type(no_ref(*rhs)))
    {
        if (is_promoteable_integral_type(no_ref(*lhs)))
            *lhs = promote_integral_type(no_ref(*lhs));
        if (is_promoteable_integral_type(no_ref(*rhs)))
            *rhs = promote_integral_type(no_ref(*rhs));

        *lhs = get_unqualified_type(no_ref(*lhs));
        *rhs = get_unqualified_type(no_ref(*rhs));

        return get_bool_type();
    }
    // p1 == 0
    // 0 == p1
    // a1 == 0
    // 0 == a1
    else if ((is_zero_type_or_nullptr_type(no_ref(*lhs))
                && (is_pointer_type(no_ref(*rhs))
                    || is_array_type(no_ref(*rhs))
                    || is_function_type(no_ref(*rhs))
                    || (allow_pointer_to_member && is_pointer_to_member_type(no_ref(*rhs)))))
            || ((is_pointer_type(no_ref(*lhs))
                    || is_array_type(no_ref(*lhs))
                    || is_function_type(no_ref(*lhs))
                    || (allow_pointer_to_member && is_pointer_to_member_type(no_ref(*lhs))))
                && is_zero_type_or_nullptr_type(no_ref(*rhs))))
    {
        if (is_array_type(no_ref(*lhs)))
        {
            *lhs = get_pointer_type(array_type_get_element_type(no_ref(*lhs)));
        }

        if (is_function_type(no_ref(*lhs)))
        {
            *lhs = get_pointer_type(no_ref(*lhs));
        }

        if (is_array_type(no_ref(*rhs)))
        {
            *rhs = get_pointer_type(array_type_get_element_type(no_ref(*rhs)));
        }

        if (is_function_type(no_ref(*rhs)))
        {
            *rhs = get_pointer_type(no_ref(*rhs));
        }

        // Convert the zero type to the other pointer type
        if (is_zero_type_or_nullptr_type(no_ref(*lhs)))
        {
            *lhs = get_unqualified_type(no_ref(*rhs));
            *rhs = get_unqualified_type(no_ref(*rhs));
        }
        if (is_zero_type_or_nullptr_type(no_ref(*rhs)))
        {
            *lhs = get_unqualified_type(no_ref(*lhs));
            *rhs = get_unqualified_type(no_ref(*lhs));
        }

        *lhs = get_unqualified_type(no_ref(*lhs));
        *rhs = get_unqualified_type(no_ref(*rhs));

        return get_bool_type();
    }
    // p1 == p2
    // a1 == a2
    // a1 == p2
    // p1 == a2
    else if ((is_pointer_type(no_ref(*lhs)) || is_array_type(no_ref(*lhs)) || is_function_type(no_ref(*lhs)) )
            && (is_pointer_type(no_ref(*rhs)) || is_array_type(no_ref(*rhs)) || is_function_type(no_ref(*rhs))))
    {
        if (is_array_type(no_ref(*lhs)))
        {
            *lhs = get_pointer_type(array_type_get_element_type(no_ref(*lhs)));
        }

        if (is_function_type(no_ref(*lhs)))
        {
            *lhs = get_pointer_type(no_ref(*lhs));
        }

        if (is_array_type(no_ref(*rhs)))
        {
            *rhs = get_pointer_type(array_type_get_element_type(no_ref(*rhs)));
        }

        if (is_function_type(no_ref(*rhs)))
        {
            *rhs = get_pointer_type(no_ref(*rhs));
        }

        if (equivalent_types(get_unqualified_type(no_ref(*lhs)), get_unqualified_type(no_ref(*rhs))))
        {
            *lhs = get_unqualified_type(no_ref(*lhs));
            *rhs = get_unqualified_type(no_ref(*rhs));
        }
        else if (is_pointer_to_void_type(no_ref(*lhs))
                || is_pointer_to_void_type(no_ref(*rhs)))
        {
            cv_qualifier_t cv_qualif_mixed = CV_NONE;

            if (is_pointer_type(no_ref(*lhs)))
                cv_qualif_mixed |= get_cv_qualifier(pointer_type_get_pointee_type(no_ref(*lhs)));

            if (is_pointer_type(no_ref(*rhs)))
                cv_qualif_mixed |= get_cv_qualifier(pointer_type_get_pointee_type(no_ref(*rhs)));

            *lhs = get_pointer_type(get_cv_qualified_type(get_void_type(), cv_qualif_mixed));
            *rhs = get_pointer_type(get_cv_qualified_type(get_void_type(), cv_qualif_mixed));
        }
        else if (standard_conversion_between_types(&scs,
                    get_unqualified_type(no_ref(*lhs)),
                    get_unqualified_type(no_ref(*rhs)),
                    locus))
        {
            *lhs = get_unqualified_type(no_ref(*rhs));
            *rhs = get_unqualified_type(no_ref(*rhs));
        }
        else if (standard_conversion_between_types(&scs,
                    get_unqualified_type(no_ref(*rhs)),
                    get_unqualified_type(no_ref(*lhs)),
                    locus))
        {
            *lhs = get_unqualified_type(no_ref(*lhs));
            *rhs = get_unqualified_type(no_ref(*lhs));
        }
        else
        {
            // Should not happen
            return get_error_type();
        }

        *lhs = get_unqualified_type(no_ref(*lhs));
        *rhs = get_unqualified_type(no_ref(*rhs));

        return get_bool_type();
    }
    // pm1 == pm2
    else if (allow_pointer_to_member
            && is_pointer_to_member_type(no_ref(*lhs))
            && is_pointer_to_member_type(no_ref(*rhs)))
    {
        if ( equivalent_types(get_unqualified_type(no_ref(*lhs)), get_unqualified_type(no_ref(*rhs))))
        {
            // Do nothing
            *lhs = get_unqualified_type(no_ref(*lhs));
            *rhs = get_unqualified_type(no_ref(*rhs));
        }
        else if (standard_conversion_between_types(&scs,
                    get_unqualified_type(no_ref(*lhs)),
                    get_unqualified_type(no_ref(*rhs)),
                    locus))
        {
            *lhs = get_unqualified_type(no_ref(*rhs));
            *rhs = get_unqualified_type(no_ref(*rhs));
        }
        else if (standard_conversion_between_types(&scs,
                    get_unqualified_type(no_ref(*rhs)),
                    get_unqualified_type(no_ref(*lhs)),
                    locus))
        {
            *rhs = get_unqualified_type(no_ref(*lhs));
            *lhs = get_unqualified_type(no_ref(*lhs));
        }
        else
        {
            // Should not happen
            return get_error_type();
        }

        return get_bool_type();
    }
    // e1 == e2
    else if (is_enum_type(no_ref(no_ref(*lhs)))
            && is_enum_type(no_ref(no_ref(*rhs)))
            && equivalent_types(get_unqualified_type(no_ref(no_ref(*lhs))), get_unqualified_type(no_ref(no_ref(*rhs)))))
    {
        *lhs = get_unqualified_type(no_ref(*lhs));
        *rhs = get_unqualified_type(no_ref(*rhs));

        return get_bool_type();
    }
    return get_error_type();
}

static type_t* operator_bin_arithmetic_pointer_or_enum_result(type_t** lhs, type_t** rhs, const locus_t* locus)
{
    return operator_bin_arithmetic_pointer_or_pointer_to_member_or_enum_result_flags(lhs, rhs, locus, /* allow_pointer_to_member */ 0);
}

static type_t* operator_bin_arithmetic_pointer_or_pointer_to_member_or_enum_result(type_t** lhs, type_t** rhs, const locus_t* locus)
{
    return operator_bin_arithmetic_pointer_or_pointer_to_member_or_enum_result_flags(lhs, rhs, locus, /* allow_pointer_to_member */ 1);
}

static
type_t* compute_type_no_overload_relational_operator_flags(nodecl_t *lhs, nodecl_t *rhs,
        const locus_t* locus,
        char allow_pointer_to_member)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    type_t* no_ref_lhs_type = no_ref(lhs_type);
    type_t* no_ref_rhs_type = no_ref(rhs_type);

    standard_conversion_t scs;

    if (both_operands_are_arithmetic(no_ref_lhs_type, no_ref_rhs_type, locus)
            || (is_scoped_enum_type(no_ref_lhs_type) && is_scoped_enum_type(no_ref_rhs_type))
            || ((is_pointer_type(no_ref_lhs_type)
                    || is_array_type(no_ref_lhs_type)
                    || is_function_type(no_ref_lhs_type)
                    || is_zero_type_or_nullptr_type(no_ref_lhs_type)
                    || (allow_pointer_to_member && is_pointer_to_member_type(no_ref_lhs_type)))
                && (is_pointer_type(no_ref_rhs_type)
                    || is_array_type(no_ref_rhs_type)
                    || is_function_type(no_ref_rhs_type)
                    || is_zero_type_or_nullptr_type(no_ref_rhs_type)
                    || (allow_pointer_to_member && is_pointer_to_member_type(no_ref_rhs_type)))
                && (is_zero_type_or_nullptr_type(no_ref_lhs_type)
                    || is_zero_type_or_nullptr_type(no_ref_rhs_type)
                    || is_pointer_to_void_type(no_ref_lhs_type)
                    || is_pointer_to_void_type(no_ref_rhs_type)
                    || standard_conversion_between_types(&scs,
                        get_unqualified_type(no_ref_lhs_type),
                        get_unqualified_type(no_ref_rhs_type),
                        locus)
                    || standard_conversion_between_types(&scs,
                        get_unqualified_type(no_ref_rhs_type),
                        get_unqualified_type(no_ref_lhs_type),
                        locus))))
    {
        type_t* result_type = NULL;
        C_LANGUAGE()
        {
            result_type = get_signed_int_type();
        }
        CXX_LANGUAGE()
        {
            result_type = get_bool_type();
        }
        ERROR_CONDITION(result_type == NULL, "Code unreachable", 0);

        // Lvalue conversions
        if (is_function_type(no_ref_lhs_type))
        {
            no_ref_lhs_type = get_pointer_type(no_ref_lhs_type);
        }
        else if (is_array_type(no_ref_lhs_type))
        {
            no_ref_lhs_type = get_pointer_type(array_type_get_element_type(no_ref_lhs_type));
        }

        if (is_function_type(no_ref_rhs_type))
        {
            no_ref_rhs_type = get_pointer_type(no_ref_rhs_type);
        }
        else if (is_array_type(no_ref_rhs_type))
        {
            no_ref_rhs_type = get_pointer_type(array_type_get_element_type(no_ref_rhs_type));
        }

        unary_record_conversion_to_result(no_ref_lhs_type, lhs);
        unary_record_conversion_to_result(no_ref_rhs_type, rhs);

        return result_type;
    }
    else if (both_operands_are_vector_types(no_ref_lhs_type, no_ref_rhs_type)
            || one_scalar_operand_and_one_vector_operand(no_ref_lhs_type, no_ref_rhs_type))
    {
        // return compute_scalar_vector_type(no_ref(lhs_type), no_ref(rhs_type));
        type_t* common_vec_type = NULL;
        if (one_scalar_operand_and_one_vector_operand(no_ref_lhs_type, no_ref_rhs_type))
        {
            common_vec_type = compute_scalar_vector_type(no_ref(lhs_type), no_ref(rhs_type));
        }
        else
        {
            if (vector_type_get_vector_size(no_ref_lhs_type) == 0)
            {
                common_vec_type = no_ref_rhs_type;
            }
            else if (vector_type_get_vector_size(no_ref_rhs_type) == 0)
            {
                common_vec_type = no_ref_lhs_type;
            }
            else if (vector_type_get_vector_size(no_ref_rhs_type) 
                    != vector_type_get_vector_size(no_ref_rhs_type))
            {
                // Vectors do not match their size
                return get_error_type();
            }
            else
            {
                common_vec_type = no_ref_lhs_type;
            }

            type_t* elem_lhs_type = vector_type_get_element_type(no_ref_lhs_type);
            type_t* elem_rhs_type = vector_type_get_element_type(no_ref_rhs_type);
            if (!both_operands_are_arithmetic(elem_lhs_type, elem_rhs_type, locus))
            {
                // We cannot do a binary relational op on them
                return get_error_type();
            }
        }

        type_t* ret_bool_type = NULL;
        C_LANGUAGE()
        {
            ret_bool_type = get_signed_int_type();
        }
        CXX_LANGUAGE()
        {
            ret_bool_type = get_bool_type();
        }
        type_t* result_type = get_vector_type(ret_bool_type, vector_type_get_vector_size(common_vec_type));

        unary_record_conversion_to_result(no_ref_lhs_type, lhs);
        unary_record_conversion_to_result(no_ref_rhs_type, rhs);

        return result_type;
    }

    return get_error_type();
}

static
type_t* compute_type_no_overload_relational_operator(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus)
{
    return compute_type_no_overload_relational_operator_flags(lhs, rhs, locus, /* allow_pointer_to_member */ 0);
}

static
type_t* compute_type_no_overload_relational_operator_eq_or_neq(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus)
{
    return compute_type_no_overload_relational_operator_flags(lhs, rhs, locus, /* allow_pointer_to_member */ 1);
}

static void compute_bin_operator_relational(nodecl_t* lhs, nodecl_t* rhs, AST operator, decl_context_t decl_context,
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        const_value_t* (const_value_bin_fun)(const_value_t*, const_value_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum_or_pointer_or_array_or_function,
            nodecl_bin_fun,
            const_value_bin_fun,
            compute_type_no_overload_relational_operator,
            both_operands_are_arithmetic_or_enum_noref,
            operator_bin_arithmetic_pointer_or_enum_pred,
            operator_bin_arithmetic_pointer_or_enum_result,
            locus,
            nodecl_output);
}

static void compute_bin_operator_relational_eq_or_neq(nodecl_t* lhs, nodecl_t* rhs, AST operator, decl_context_t decl_context,
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        const_value_t* (const_value_bin_fun)(const_value_t*, const_value_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs,
            operator,
            decl_context,
            any_operand_is_class_or_enum_or_pointer_or_array_or_function_or_pointer_to_member,
            nodecl_bin_fun,
            const_value_bin_fun,
            compute_type_no_overload_relational_operator_eq_or_neq,
            both_operands_are_arithmetic_or_enum_noref,
            operator_bin_arithmetic_pointer_or_pointer_to_member_or_enum_pred,
            operator_bin_arithmetic_pointer_or_pointer_to_member_or_enum_result,
            locus,
            nodecl_output);
}

static
void compute_bin_operator_lower_equal_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_LESS_OR_EQUAL_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_relational(lhs, rhs, 
            operation_tree, 
            decl_context, 
            nodecl_make_lower_or_equal_than,
            const_value_lte,
            locus,
            nodecl_output);
}

void compute_bin_operator_lower_than_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_LOWER_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_relational(lhs, rhs, 
            operation_tree, 
            decl_context, 
            nodecl_make_lower_than,
            const_value_lt,
            locus,
            nodecl_output);
}

static void compute_bin_operator_greater_equal_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_GREATER_OR_EQUAL_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_relational(lhs, rhs, 
            operation_tree, 
            decl_context, 
            nodecl_make_greater_or_equal_than,
            const_value_gte,
            locus,
            nodecl_output);
}

static void compute_bin_operator_greater_than_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_GREATER_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_relational(lhs, rhs, 
            operation_tree, 
            decl_context, 
            nodecl_make_greater_than,
            const_value_gt,
            locus,
            nodecl_output);
}

static void compute_bin_operator_different_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_DIFFERENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_relational_eq_or_neq(lhs, rhs,
            operation_tree,
            decl_context,
            nodecl_make_different,
            const_value_neq,
            locus,
            nodecl_output);
}

static void compute_bin_operator_equal_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_EQUAL_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_relational_eq_or_neq(lhs, rhs,
            operation_tree,
            decl_context,
            nodecl_make_equal,
            const_value_eq,
            locus,
            nodecl_output);
}

static char operator_bin_logical_types_pred(type_t* lhs, type_t* rhs, const locus_t* locus)
{
    standard_conversion_t dummy;
    return (standard_conversion_between_types(&dummy, no_ref(lhs), get_bool_type(), locus)
            && standard_conversion_between_types(&dummy, no_ref(rhs), get_bool_type(), locus));
}

static type_t* operator_bin_logical_types_result(type_t** lhs, type_t** rhs, const locus_t* locus UNUSED_PARAMETER)
{
    // We want the prototype of the builtin operation as 'bool operator#(bool, bool)' not
    // 'bool operator#(L, R)' with L and R convertible to bool
    *lhs = get_bool_type();
    *rhs = get_bool_type();

    return get_bool_type();
}

static type_t* compute_type_no_overload_logical_op(nodecl_t* lhs, nodecl_t* rhs, const locus_t* locus)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    type_t* conversion_type = NULL;
    C_LANGUAGE()
    {
        conversion_type = get_signed_int_type();
    }
    CXX_LANGUAGE()
    {
        conversion_type = get_bool_type();
    }

    char is_vector_op = 0;
    int vector_size = 0;
    if (both_operands_are_vector_types(no_ref(lhs_type),
                no_ref(rhs_type)))
    {
        is_vector_op = 1;
        vector_size = vector_type_get_vector_size(lhs_type);

        if (vector_size != vector_type_get_vector_size(rhs_type))
        {
            return get_error_type();
        }

        lhs_type = vector_type_get_element_type(lhs_type);
        rhs_type = vector_type_get_element_type(rhs_type);
    }

    standard_conversion_t lhs_to_bool;
    standard_conversion_t rhs_to_bool;
    if (standard_conversion_between_types(&lhs_to_bool, no_ref(lhs_type), conversion_type, locus)
            && standard_conversion_between_types(&rhs_to_bool, no_ref(rhs_type), conversion_type, locus))
    {
        if (is_vector_op)
        {
            type_t* result = get_vector_type(conversion_type, vector_size);

            binary_record_conversion_to_result(result, lhs, rhs);

            return result;
        }
        else
        {
            binary_record_conversion_to_result(conversion_type, lhs, rhs);

            return conversion_type;
        }
    }

    return get_error_type();
}

static void compute_bin_logical_op_type(nodecl_t* lhs, nodecl_t* rhs, AST operator, 
        decl_context_t decl_context,
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        const_value_t* (const_value_bin_fun)(const_value_t*, const_value_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_bin_fun,
            const_value_bin_fun,
            compute_type_no_overload_logical_op,
            both_operands_are_arithmetic_noref,
            operator_bin_logical_types_pred,
            operator_bin_logical_types_result,
            locus,
            nodecl_output);
}

static void compute_bin_operator_logical_or_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_LOGICAL_OR_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_logical_op_type(lhs, rhs, 
            operation_tree, decl_context, 
            nodecl_make_logical_or,
            const_value_or,
            locus, 
            nodecl_output);
}

static void compute_bin_operator_logical_and_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_LOGICAL_AND_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_logical_op_type(lhs, rhs, 
            operation_tree, decl_context, 
            nodecl_make_logical_and,
            const_value_and,
            locus, 
            nodecl_output);
}

static void compute_bin_operator_bitwise_and_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)

{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_AND_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_only_integer_types(
            lhs, rhs, 
            operation_tree, decl_context, 
            nodecl_make_bitwise_and,
            const_value_bitand,
            locus,
            nodecl_output);
}

static void compute_bin_operator_bitwise_or_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_OR_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_only_integer_types(
            lhs, rhs, 
            operation_tree, decl_context, 
            nodecl_make_bitwise_or,
            const_value_bitor,
            locus,
            nodecl_output);
}

static void compute_bin_operator_bitwise_xor_type(nodecl_t* lhs, nodecl_t* rhs, decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_XOR_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_only_integer_types(
            lhs, rhs, 
            operation_tree, decl_context, 
            nodecl_make_bitwise_xor,
            const_value_bitxor,
            locus,
            nodecl_output);
}

static char operator_bin_assign_only_integer_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_lvalue_reference_type(lhs)
            && is_integral_type(reference_type_get_referenced_type(lhs))
            && !is_const_qualified_type(reference_type_get_referenced_type(lhs))
            && is_integral_type(no_ref(rhs)));
}

static type_t* operator_bin_assign_only_integer_result(type_t** lhs, type_t** rhs, const locus_t* locus UNUSED_PARAMETER)
{
    type_t* ref_type = reference_type_get_referenced_type(*lhs);

    cv_qualifier_t cv_qualif = CV_NONE;
    advance_over_typedefs_with_cv_qualif(ref_type, &cv_qualif);

    *rhs = get_unqualified_type(no_ref(*rhs));
    if (is_promoteable_integral_type(*rhs))
        *rhs = promote_integral_type(*rhs);

    type_t* result = get_lvalue_reference_type(
            get_cv_qualified_type(ref_type, cv_qualif));

    return result;
}

static type_t* compute_type_no_overload_assig_only_integral_type(nodecl_t* lhs, nodecl_t* rhs, const locus_t* locus)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (!is_lvalue_reference_type(lhs_type)
            || is_const_qualified_type(reference_type_get_referenced_type(lhs_type))
            || is_array_type(no_ref(lhs_type)))
        return get_error_type();

    if (both_operands_are_integral(no_ref(lhs_type), no_ref(rhs_type), locus))
    {
        type_t* common_type = compute_arithmetic_builtin_bin_op(no_ref(lhs_type), no_ref(rhs_type), locus);

        unary_record_conversion_to_result(common_type, rhs);

        return lhs_type;
    }
    else if (left_operand_is_vector_and_right_operand_is_scalar(no_ref(lhs_type), no_ref(rhs_type)))
    { 
        type_t* result = vector_type_get_element_type(no_ref(lhs_type));

        unary_record_conversion_to_result(result, rhs);

        return result;
    }
    else
    {
        return get_error_type();
    }
}


static void compute_bin_operator_assig_only_integral_type(nodecl_t* lhs, nodecl_t* rhs, AST operator,
        decl_context_t decl_context, nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_bin_fun,
            NULL, // No constants
            compute_type_no_overload_assig_only_integral_type,
            NULL,
            operator_bin_assign_only_integer_pred,
            operator_bin_assign_only_integer_result,
            locus,
            nodecl_output);
}

static char operator_bin_assign_arithmetic_or_pointer_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_lvalue_reference_type(lhs)
            && !is_const_qualified_type(reference_type_get_referenced_type(lhs))
            && (both_operands_are_arithmetic(reference_type_get_referenced_type(lhs), no_ref(rhs), locus)
                || is_pointer_arithmetic(reference_type_get_referenced_type(lhs), no_ref(rhs))));
}

static type_t* operator_bin_assign_arithmetic_or_pointer_result(type_t** lhs, type_t** rhs, const locus_t* locus UNUSED_PARAMETER)
{
    type_t* ref_type = reference_type_get_referenced_type(*lhs);

    cv_qualifier_t cv_qualif = CV_NONE;
    advance_over_typedefs_with_cv_qualif(ref_type, &cv_qualif);

    if (both_operands_are_arithmetic(ref_type, no_ref(*rhs), locus))
    {
        *rhs = get_unqualified_type(no_ref(*rhs));

        if (is_promoteable_integral_type(*rhs))
            *rhs = promote_integral_type(*rhs);

        return *lhs;
    }
    else if (is_pointer_arithmetic(no_ref(*lhs), no_ref(*rhs)))
    {
        *rhs = get_unqualified_type(no_ref(*rhs));

        return *lhs;
    }

    return get_error_type();
}

static type_t* compute_type_no_overload_assig_arithmetic_or_pointer_type(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus UNUSED_PARAMETER)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (!is_lvalue_reference_type(lhs_type)
            || is_const_qualified_type(reference_type_get_referenced_type(lhs_type))
            || is_array_type(no_ref(lhs_type)))
        return get_error_type();

    if (both_operands_are_arithmetic(no_ref(lhs_type), no_ref(rhs_type), locus))
    {
        unary_record_conversion_to_result(no_ref(lhs_type), rhs);

        return lhs_type;
    }
    else if (is_pointer_and_integral_type(no_ref(lhs_type), no_ref(rhs_type)))
    {
        unary_record_conversion_to_result(no_ref(rhs_type), rhs);

        return lhs_type;
    }
    else if (both_operands_are_vector_types(no_ref(lhs_type), 
                no_ref(rhs_type)))
    {
        unary_record_conversion_to_result(no_ref(lhs_type), rhs);

        return lhs_type;
    }
    else if (left_operand_is_vector_and_right_operand_is_scalar(no_ref(lhs_type), no_ref(rhs_type)))
    {
        unary_record_conversion_to_result(no_ref(lhs_type), rhs);

        return lhs_type;
    }


    return get_error_type();
}

static void compute_bin_operator_assig_arithmetic_or_pointer_type(nodecl_t* lhs, nodecl_t* rhs, 
        AST operator,
        decl_context_t decl_context,
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_bin_fun,
            NULL, // No constants
            compute_type_no_overload_assig_arithmetic_or_pointer_type,
            NULL,
            operator_bin_assign_arithmetic_or_pointer_pred,
            operator_bin_assign_arithmetic_or_pointer_result,
            locus,
            nodecl_output);
}

static char operator_bin_assign_only_arithmetic_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_lvalue_reference_type(lhs)
            && is_arithmetic_type(reference_type_get_referenced_type(lhs))
            && !is_const_qualified_type(reference_type_get_referenced_type(lhs))
            && is_arithmetic_type(rhs));
}

static type_t* operator_bin_assign_only_arithmetic_result(type_t** lhs, type_t** rhs, const locus_t* locus UNUSED_PARAMETER)
{
    *rhs = get_unqualified_type(no_ref(*rhs));

    if (is_promoteable_integral_type(*rhs))
        *rhs = promote_integral_type(*rhs);

    return *lhs;
}

static
void generate_nonop_assign_builtin(
        builtin_operators_set_t *result,
        AST operator,
        type_t* lhs_type,
        type_t* rhs_type,
        decl_context_t decl_context)
{
    parameter_info_t parameters[2] =
    {
        {
            .is_ellipsis = 0,
            .type_info = lhs_type,
            .nonadjusted_type_info = NULL
        },
        {
            .is_ellipsis = 0,
            .type_info = rhs_type,
            .nonadjusted_type_info = NULL
        }
    };

    type_t* function_type = get_new_function_type(lhs_type, parameters, 2, REF_QUALIFIER_NONE);

    // Fill the minimum needed for this 'faked' function symbol
    (*result).entry[(*result).num_builtins].kind = SK_FUNCTION;
    (*result).entry[(*result).num_builtins].symbol_name = get_operator_function_name(operator);
    symbol_entity_specs_set_is_builtin(&(*result).entry[(*result).num_builtins], 1);
    (*result).entry[(*result).num_builtins].type_information = function_type;
    (*result).entry[(*result).num_builtins].decl_context = decl_context;

    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Generated builtin '%s' for '%s'\n",
                print_declarator((*result).entry[(*result).num_builtins].type_information),
                (*result).entry[(*result).num_builtins].symbol_name);
    }

    // Add to the results and properly chain things
    (*result).entry_list = entry_list_add((*result).entry_list, &((*result).entry[(*result).num_builtins]));
    (*result).num_builtins++;
}


static
void build_binary_nonop_assign_builtin(type_t* lhs_type, 
        builtin_operators_set_t *result,
        AST operator, decl_context_t decl_context)
{
    memset(result, 0, sizeof(*result));

    int vector_size = 0; // Used below in is_intel_vector_struct_type

    if (!is_lvalue_reference_type(lhs_type)
            || is_const_qualified_type(reference_type_get_referenced_type(lhs_type)))
        return;

    if (is_promoteable_integral_type(no_ref(lhs_type)))
    {
        generate_nonop_assign_builtin(result, operator, lhs_type, promote_integral_type(no_ref(lhs_type)), decl_context);
    }
    else if (is_integral_type(no_ref(lhs_type))
            || is_floating_type(no_ref(lhs_type)))
    {
        generate_nonop_assign_builtin(result, operator, lhs_type, get_unqualified_type(no_ref(lhs_type)), decl_context);
    }
    else if (is_enum_type(no_ref(lhs_type))
            || is_pointer_to_member_type(no_ref(lhs_type)))
    {
        generate_nonop_assign_builtin(result, operator, lhs_type, get_unqualified_type(no_ref(lhs_type)), decl_context);
    }
    else if (is_pointer_type(no_ref(lhs_type)))
    {
        generate_nonop_assign_builtin(result, operator, lhs_type, get_unqualified_type(no_ref(lhs_type)), decl_context);
    }
    else if (is_vector_type(no_ref(lhs_type)))
    {
        generate_nonop_assign_builtin(result, operator, lhs_type, get_unqualified_type(no_ref(lhs_type)), decl_context);

        if (CURRENT_CONFIGURATION->enable_intel_vector_types)
        {
            // Allow this case as a 'builtin'
            // __attribute__((vector_size(16))) float v1;
            // __m128 v2;
            // v1 = v2;
            type_t* intel_struct_vector = vector_type_get_intel_vector_struct_type(no_ref(lhs_type));
            if (intel_struct_vector != NULL)
            {
                generate_nonop_assign_builtin(result, operator, lhs_type, intel_struct_vector, decl_context);
            }
        }
    }
    else if (is_intel_vector_struct_type(no_ref(lhs_type), &vector_size))
    {
        // // Let overload choose the member copy operator assignment of the class
        // // __m128 v1, v2;
        // // v1 = v2;
        // type_t* vector_type = intel_vector_struct_type_get_vector_type(no_ref(lhs_type));
        // if (vector_type != NULL)
        // {
        //     // Allow this case as a 'builtin'
        //     // __m128 v1;
        //     // __attribute__((vector_size(16))) float v2;
        //     // v1 = v2;
        //     generate_nonop_assign_builtin(result, operator, lhs_type, vector_type, decl_context);
        // }
    }
    // No other builtin is possible
}

static void compute_bin_nonoperator_assig_only_arithmetic_type(nodecl_t *lhs, nodecl_t *rhs, 
        AST operator, decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)

{
    // (Non-operator) Assignment is so special that it cannot use the generic procedure for
    // solving binary operations
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (nodecl_expr_is_type_dependent(*lhs)
            || nodecl_expr_is_type_dependent(*rhs))
    {
        *nodecl_output = nodecl_make_assignment(*lhs, *rhs,
                get_unknown_dependent_type(),
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    char requires_overload = 0;
    CXX_LANGUAGE()
    {
        // Enums are not considered for overloading in operator= because any overload
        // of operator= must be member, so classes are only eligible here for overload.
        requires_overload = is_class_type(no_ref(lhs_type)) || is_class_type(no_ref(rhs_type));
    }

    if (!requires_overload)
    {
        if (!is_lvalue_reference_type(lhs_type)
                || is_const_qualified_type(no_ref(lhs_type))
                || is_array_type(no_ref(lhs_type)))
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        // If the rhs is an unresolved overloaded type we have
        // to solve it here using lhs_type
        if (is_unresolved_overloaded_type(no_ref(rhs_type)))
        {
            update_unresolved_overload_argument(rhs_type,
                    lhs_type,
                    decl_context,
                    nodecl_get_locus(*rhs),
                    rhs);
            rhs_type = nodecl_get_type(*rhs);
        }

        standard_conversion_t sc;
        if (rhs_type == NULL
                || is_error_type(rhs_type)
                || !standard_conversion_between_types(&sc, no_ref(rhs_type), no_ref(lhs_type), locus))
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        type_t* result_type = lvalue_ref(lhs_type);
        unary_record_conversion_to_result(no_ref(result_type), rhs);

        *nodecl_output = nodecl_make_assignment(
                *lhs,
                *rhs,
                result_type, 
                locus);
        return;
    }

    builtin_operators_set_t builtin_set; 
    build_binary_nonop_assign_builtin(
            lhs_type, &builtin_set, operator, decl_context);

    scope_entry_list_t* builtins = get_entry_list_from_builtin_operator_set(&builtin_set);

    scope_entry_t* selected_operator = NULL;

    // We need to do overload
    type_t* result = compute_user_defined_bin_operator_type(operator, 
            lhs, rhs, builtins, decl_context, locus, &selected_operator);

    entry_list_free(builtins);

    if (result != NULL
            && selected_operator != NULL)
    {
        if (symbol_entity_specs_get_is_builtin(selected_operator))
        {
            // Keep conversions
            if (!equivalent_types(
                        get_unqualified_type(no_ref(nodecl_get_type(*lhs))),
                        get_unqualified_type(no_ref(result))))
            {
                *lhs = cxx_nodecl_make_conversion(*lhs, result,
                        nodecl_get_locus(*lhs));
            }
            if (!equivalent_types(
                        get_unqualified_type(no_ref(nodecl_get_type(*rhs))),
                        get_unqualified_type(no_ref(result))))
            {
                *rhs = cxx_nodecl_make_conversion(*rhs,
                        no_ref(result),
                        nodecl_get_locus(*rhs));
            }

            *nodecl_output = nodecl_make_assignment(
                    *lhs,
                    *rhs,
                    result, 
                    locus);
        }
        else
        {
            nodecl_t nodecl_selected_op =
                        nodecl_make_symbol(selected_operator, locus);
            nodecl_set_type(nodecl_selected_op, selected_operator->type_information);

            *nodecl_output = 
                cxx_nodecl_make_function_call(
                        nodecl_selected_op,
                        /* called name */ nodecl_null(),
                        nodecl_make_list_2(*lhs, *rhs),
                        nodecl_make_cxx_function_form_binary_infix(nodecl_get_locus(*lhs)),
                        result,
                        decl_context,
                        locus);
        }
    }
    else
    {
        *nodecl_output = nodecl_make_err_expr(locus);
    }
}

static type_t* compute_type_no_overload_assig_only_arithmetic_type(nodecl_t *lhs, nodecl_t *rhs, const locus_t* locus UNUSED_PARAMETER)
{
    type_t* lhs_type = nodecl_get_type(*lhs);
    type_t* rhs_type = nodecl_get_type(*rhs);

    if (!is_lvalue_reference_type(lhs_type)
            || is_const_qualified_type(no_ref(lhs_type))
            || is_array_type(no_ref(rhs_type)))
        return get_error_type();

    if (both_operands_are_arithmetic(no_ref(rhs_type), no_ref(lhs_type), locus))
    {
        unary_record_conversion_to_result(no_ref(lhs_type), rhs);

        return lhs_type;
    }
    else if (both_operands_are_vector_types(no_ref(lhs_type),
                no_ref(rhs_type)))
    {
        return lhs_type;
    }
    else if (left_operand_is_vector_and_right_operand_is_scalar(no_ref(lhs_type), no_ref(rhs_type)))
    {
        unary_record_conversion_to_result(lhs_type, rhs);

        return lhs_type;
    }

    return get_error_type();
}

static void compute_bin_operator_assig_only_arithmetic_type(nodecl_t* lhs, nodecl_t* rhs, AST operator,
        decl_context_t decl_context,
        nodecl_t (*nodecl_bin_fun)(nodecl_t, nodecl_t, type_t*, const locus_t*),
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    compute_bin_operator_generic(lhs, rhs, 
            operator, 
            decl_context,
            any_operand_is_class_or_enum,
            nodecl_bin_fun,
            NULL, // No constants
            compute_type_no_overload_assig_only_arithmetic_type,
            NULL,
            operator_bin_assign_only_arithmetic_pred,
            operator_bin_assign_only_arithmetic_result,
            locus,
            nodecl_output);
}

static void compute_bin_operator_mod_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_MOD_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_integral_type(lhs, rhs, 
            operation_tree, decl_context, 
            nodecl_make_mod_assignment,
            locus,
            nodecl_output);
}

static void compute_bin_operator_bitwise_shl_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_LEFT_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_integral_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_bitwise_shl_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_shr_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_RIGHT_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_integral_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_shr_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_bitwise_and_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)

{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_AND_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_integral_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_bitwise_and_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_bitwise_or_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_OR_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_integral_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_bitwise_or_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_bitwise_xor_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_XOR_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_integral_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_bitwise_xor_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_mul_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_MUL_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_arithmetic_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_mul_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_ASSIGNMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_nonoperator_assig_only_arithmetic_type(lhs, rhs, 
            operation_tree, decl_context,
            locus,
            nodecl_output);
}

static void compute_bin_operator_div_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_DIV_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_only_arithmetic_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_div_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_add_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_ADD_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_arithmetic_or_pointer_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_add_assignment,
            locus, nodecl_output);
}

static void compute_bin_operator_sub_assig_type(nodecl_t* lhs, nodecl_t* rhs,
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_SUB_ASSIGN_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_bin_operator_assig_arithmetic_or_pointer_type(lhs, rhs, 
            operation_tree, decl_context, nodecl_make_minus_assignment,
            locus, nodecl_output);
}

static void compute_unary_operator_generic(
        nodecl_t* op, 
        // Operator name and context
        AST operator,
        decl_context_t decl_context, 
        // Functions
        char (*will_require_overload)(type_t*),
        nodecl_t (*nodecl_unary_fun)(nodecl_t, type_t*, const locus_t* locus),
        const_value_t* (*const_value_unary_fun)(const_value_t*),
        type_t* (*compute_type_no_overload)(nodecl_t*, char* is_lvalue, const locus_t* locus),
        char types_allow_constant_evaluation(type_t*, const locus_t* locus),
        char (*overload_operator_predicate)(type_t*, const locus_t* locus),
        type_t* (*overload_operator_result_types)(type_t**, const locus_t* locus),
        // Whether we have to record conversions
        char save_conversions,
        // Locus
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    type_t* op_type = nodecl_get_type(*op);

    if (nodecl_expr_is_type_dependent(*op))
    {
        *nodecl_output = nodecl_unary_fun(*op, get_unknown_dependent_type(), locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);

        nodecl_expr_set_is_value_dependent(*nodecl_output, nodecl_expr_is_value_dependent(*op));
        return;
    }

    char requires_overload = 0;
    CXX_LANGUAGE()
    {
        type_t* no_ref_op_type = no_ref(op_type);

        // Try to simplify unresolved overloads
        if (is_unresolved_overloaded_type(no_ref_op_type))
        {
            scope_entry_t* function = unresolved_overloaded_type_simplify(no_ref(op_type),
                    decl_context, locus);
            if (function != NULL)
            {
                if (!update_simplified_unresolved_overloaded_type(
                            function,
                            decl_context,
                            locus,
                            op))
                {
                    *nodecl_output = nodecl_make_err_expr(locus);
                    return;
                }
                op_type = nodecl_get_type(*op);
            }
        }

        requires_overload = will_require_overload(no_ref_op_type);
    }

    if (!requires_overload)
    {
        char is_lvalue = 0;
        type_t* computed_type = compute_type_no_overload(op, &is_lvalue, locus);

        if (is_error_type(computed_type))
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        const_value_t* val = NULL;

        if (const_value_unary_fun != NULL
                && types_allow_constant_evaluation(op_type, locus)
                && nodecl_is_constant(*op))
        {
            val = const_value_unary_fun(nodecl_get_constant(*op));
        }

        if (save_conversions
                && !equivalent_types(
                    get_unqualified_type(no_ref(nodecl_get_type(*op))), 
                    get_unqualified_type(no_ref(computed_type))))
        {
            *op = cxx_nodecl_make_conversion(*op, computed_type,
                    nodecl_get_locus(*op));
        }

        if (is_lvalue)
        {
            computed_type = lvalue_ref(computed_type);
        }

        *nodecl_output = nodecl_unary_fun(
                *op,
                computed_type, locus);

        if (nodecl_expr_is_value_dependent(*op))
        {
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }

        nodecl_set_constant(*nodecl_output, val);

        return;
    }

    builtin_operators_set_t builtin_set;
    build_unary_builtin_operators(
            op_type,
            &builtin_set,
            decl_context, operator,
            overload_operator_predicate,
            overload_operator_result_types,
            locus);

    scope_entry_list_t* builtins = get_entry_list_from_builtin_operator_set(&builtin_set);

    scope_entry_t* selected_operator = NULL;

    type_t* result = compute_user_defined_unary_operator_type(operator,
            op, builtins, decl_context, 
            locus, &selected_operator);

    ERROR_CONDITION(result == NULL, "Invalid type", 0);

    entry_list_free(builtins);

    if (selected_operator != NULL)
    {
        if (symbol_entity_specs_get_is_builtin(selected_operator))
        {
            const_value_t* val = NULL;

            if (const_value_unary_fun != NULL
                    && types_allow_constant_evaluation(op_type, locus)
                    && nodecl_is_constant(*op))
            {
                val = const_value_unary_fun(nodecl_get_constant(*op));
            }

            if (save_conversions
                    && !equivalent_types(
                        get_unqualified_type(no_ref(nodecl_get_type(*op))), 
                        get_unqualified_type(no_ref(result))))
            {
                *op = cxx_nodecl_make_conversion(*op, result,
                        nodecl_get_locus(*op));
            }

            *nodecl_output = nodecl_unary_fun(*op, result, locus);

            if (nodecl_expr_is_value_dependent(*op))
            {
                nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
            }

            nodecl_set_constant(*nodecl_output, val);
        }
        else
        {
            *nodecl_output = 
                cxx_nodecl_make_function_call(
                        nodecl_make_symbol(selected_operator, locus),
                        /* called name */ nodecl_null(),
                        nodecl_make_list_1(*op),
                        nodecl_make_cxx_function_form_unary_prefix(locus),
                        result,
                        decl_context,
                        locus);
        }
    }
    else
    {
        *nodecl_output = nodecl_make_err_expr(locus);
    }
}

char operator_unary_derref_pred(type_t* op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_pointer_type(no_ref(op_type)))
    {
        return 1;
    }
    return 0;
}

type_t* operator_unary_derref_result(type_t** op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_pointer_type(no_ref(*op_type)))
    {
        *op_type = no_ref(*op_type);

        return get_lvalue_reference_type(pointer_type_get_pointee_type(*op_type));
    }
    return get_error_type();
}

type_t* compute_type_no_overload_derref(nodecl_t *nodecl_op, char *is_lvalue, const locus_t* locus UNUSED_PARAMETER)
{
    type_t* op_type = nodecl_get_type(*nodecl_op);

    type_t* computed_type = get_error_type();
    if (is_pointer_type(no_ref(op_type)))
    {
        computed_type = lvalue_ref(pointer_type_get_pointee_type(no_ref(op_type)));
        *is_lvalue = 1;
    }
    else if (is_array_type(no_ref(op_type)))
    {
        computed_type = lvalue_ref(array_type_get_element_type(no_ref(op_type)));
        *is_lvalue = 1;
    }
    else if (is_function_type(no_ref(op_type)))
    {
        // Create a pointer type
        computed_type = lvalue_ref(get_pointer_type(no_ref(op_type)));
        *is_lvalue = 1;
    }

    return computed_type;
}

static void compute_operator_derreference_type(
        nodecl_t *op, decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t *nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_MUL_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_unary_operator_generic(op, 
            operation_tree, decl_context,
            operand_is_class_or_enum,
            nodecl_make_dereference,
            NULL, // No constants
            compute_type_no_overload_derref,
            NULL,
            operator_unary_derref_pred,
            operator_unary_derref_result,
            /* save_conversions */ 0,
            locus,
            nodecl_output);
}

static char operator_unary_plus_pred(type_t* op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_pointer_type(no_ref(op_type)))
    {
        return 1;
    }
    else if (is_arithmetic_type(no_ref(op_type)))
    {
        return 1;
    }
    return 0;
}

static type_t* operator_unary_plus_result(type_t** op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_pointer_type(no_ref(*op_type)))
    {
        *op_type = get_unqualified_type(no_ref(*op_type));
        return (*op_type);
    }
    else if (is_arithmetic_type(no_ref(*op_type)))
    {
        *op_type = get_unqualified_type(no_ref(*op_type));
        if (is_promoteable_integral_type(*op_type))
        {
            *op_type = promote_integral_type(*op_type);
        }

        return (*op_type);
    }
    return get_error_type();
}

static type_t* compute_type_no_overload_plus(nodecl_t *op, char *is_lvalue, const locus_t* locus UNUSED_PARAMETER)
{
    *is_lvalue = 0;

    type_t* op_type = nodecl_get_type(*op);

    if (is_pointer_type(no_ref(op_type)))
    {
        // Bypass
        type_t* result = no_ref(op_type);

        unary_record_conversion_to_result(result, op);

        return result;
    }
    else if (is_arithmetic_type(no_ref(op_type)))
    {
        type_t* result = NULL;
        if (is_promoteable_integral_type(no_ref(op_type)))
        {
            result = promote_integral_type(no_ref(op_type));
        }
        else
        {
            result = no_ref(op_type);
        }

        unary_record_conversion_to_result(result, op);
        return result;
    }
    else if (is_vector_type(no_ref(op_type)))
    {
        type_t* result = no_ref(op_type);

        unary_record_conversion_to_result(result, op);
        return result;
    }
    else
    {
        return get_error_type();
    }
}

static void compute_operator_plus_type(nodecl_t* op, 
        decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_ADD_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_unary_operator_generic(op, 
            operation_tree, decl_context,
            operand_is_class_or_enum,
            nodecl_make_plus,
            const_value_plus, 
            compute_type_no_overload_plus,
            operand_is_arithmetic_or_unscoped_enum_type_noref,
            operator_unary_plus_pred,
            operator_unary_plus_result,
            /* save_conversions */ 1,
            locus,
            nodecl_output);
}

static char operator_unary_minus_pred(type_t* op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_arithmetic_type(no_ref(op_type)))
    {
        return 1;
    }
    return 0;
}

static type_t* operator_unary_minus_result(type_t** op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_arithmetic_type(no_ref(*op_type)))
    {
        *op_type = get_unqualified_type(no_ref(*op_type));
        if (is_promoteable_integral_type(*op_type))
        {
            *op_type = promote_integral_type(*op_type);
        }

        return (*op_type);
    }
    return get_error_type();
}

static type_t* compute_type_no_overload_neg(nodecl_t *op, char *is_lvalue, const locus_t* locus UNUSED_PARAMETER)
{
    *is_lvalue = 0;

    type_t* op_type = nodecl_get_type(*op);

    if (is_arithmetic_type(no_ref(op_type)))
    {
        type_t* result = NULL;
        if (is_promoteable_integral_type(no_ref(op_type)))
        {
            result = promote_integral_type(no_ref(op_type));
        }
        else
        {
            result = no_ref(op_type);
        }

        unary_record_conversion_to_result(result, op);

        return result;
    }
    else if (is_vector_type(no_ref(op_type)))
    {
        type_t* result = no_ref(op_type);

        unary_record_conversion_to_result(result, op);

        return result;
    }
    else
    {
        return get_error_type();
    }
}

static void compute_operator_minus_type(nodecl_t* op, decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_MINUS_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_unary_operator_generic(op, 
            operation_tree, decl_context,
            operand_is_class_or_enum,
            nodecl_make_neg,
            const_value_neg, 
            compute_type_no_overload_neg,
            operand_is_arithmetic_or_unscoped_enum_type_noref,
            operator_unary_minus_pred,
            operator_unary_minus_result,
            /* save_conversions */ 1,
            locus,
            nodecl_output);
}

static char operator_unary_complement_pred(type_t* op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_integral_type(no_ref(op_type)))
    {
        return 1;
    }
    return 0;
}

static type_t* operator_unary_complement_result(type_t** op_type, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_integral_type(no_ref(*op_type)))
    {
        *op_type = get_unqualified_type(no_ref(*op_type));
        if (is_promoteable_integral_type(*op_type))
        {
            *op_type = promote_integral_type(*op_type);
        }

        return (*op_type);
    }
    return get_error_type();
}

static type_t* compute_type_no_overload_complement(nodecl_t *op, char *is_lvalue, const locus_t* locus)
{
    return compute_type_no_overload_neg(op, is_lvalue, locus);
}

static void compute_operator_complement_type(nodecl_t* op, 
        decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_NEG_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_unary_operator_generic(op, 
            operation_tree, decl_context,
            operand_is_class_or_enum,
            nodecl_make_bitwise_not,
            const_value_bitnot, 
            compute_type_no_overload_complement,
            operand_is_integral_or_bool_or_unscoped_enum_type_noref,
            operator_unary_complement_pred,
            operator_unary_complement_result,
            /* save_conversions */ 1,
            locus,
            nodecl_output);
}

static char operator_unary_not_pred(type_t* op_type, const locus_t* locus)
{
    standard_conversion_t to_bool;

    if (standard_conversion_between_types(&to_bool,
                no_ref(op_type), get_bool_type(), locus))
    {
        return 1;
    }
    return 0;
}

static type_t* operator_unary_not_result(type_t** op_type, const locus_t* locus UNUSED_PARAMETER)
{
    // We want the function to be 'bool operator!(bool)' not 'bool
    // operator!(L)' with L something that can be converted to bool
    *op_type = get_bool_type();

    return *op_type;
}

static type_t* compute_type_no_overload_logical_not(nodecl_t *op, char *is_lvalue, const locus_t* locus)
{
    *is_lvalue = 0;

    type_t* op_type = nodecl_get_type(*op);

    standard_conversion_t to_bool;

    if (standard_conversion_between_types(&to_bool,
                op_type, get_bool_type(), locus))
    {
        type_t* result = NULL;
        C_LANGUAGE()
        {
            result = get_signed_int_type();
        }
        CXX_LANGUAGE()
        {
            result = get_bool_type();
        }
        ERROR_CONDITION(result == NULL, "Invalid type", 0);

        unary_record_conversion_to_result(result, op);

        return result;
    }
    else
    {
        return get_error_type();
    }
}


static void compute_operator_not_type(nodecl_t* op, 
        decl_context_t decl_context, 
        const locus_t* locus, 
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_LOGICAL_NOT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    compute_unary_operator_generic(op,
            operation_tree, decl_context,
            operand_is_class_or_enum,
            nodecl_make_logical_not,
            const_value_not,
            compute_type_no_overload_logical_not,
            operand_is_arithmetic_or_unscoped_enum_type_noref,
            operator_unary_not_pred,
            operator_unary_not_result,
            /* save_conversions */ 1,
            locus,
            nodecl_output);
}

static char operator_unary_reference_pred(type_t* t, const locus_t* locus UNUSED_PARAMETER)
{
    return is_lvalue_reference_type(t);
}

static type_t* operator_unary_reference_result(type_t** op_type, const locus_t* locus UNUSED_PARAMETER)
{
    // Mercurium extension
    //
    // T  @reb-ref@ z;
    //
    // &z -> T *&     [a lvalue reference to a pointer type]
    if (is_rebindable_reference_type(*op_type))
    {
        return get_lvalue_reference_type(
                get_pointer_type(no_ref(*op_type))
                );
    }
    // T x, &y;
    //
    // &x -> T*
    // &y -> T*
    else if (is_lvalue_reference_type(*op_type))
    {
        return get_pointer_type(no_ref(*op_type));
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static type_t* compute_type_no_overload_reference(nodecl_t *op, char *is_lvalue, const locus_t* locus UNUSED_PARAMETER)
{
    *is_lvalue = 0;

    type_t* t = nodecl_get_type(*op);

    if (is_any_reference_type(t))
    {
        if (is_rebindable_reference_type(t))
        {
            // Mercurium extension
            // Rebindable references are lvalues
            *is_lvalue = 1;
        }

        return get_pointer_type(no_ref(t));
    }

    return get_error_type();
}

static char contains_wrongly_associated_template_name(AST a, decl_context_t decl_context)
{
    if (ASTType(a) == AST_BITWISE_SHL
            || ASTType(a) == AST_SHR
            || ASTType(a) == AST_ADD
            || ASTType(a) == AST_MINUS
            || ASTType(a) == AST_DIV
            || ASTType(a) == AST_MOD
            || ASTType(a) == AST_MUL
            || ASTType(a) == AST_LOWER_THAN
            || ASTType(a) == AST_GREATER_THAN
            || ASTType(a) == AST_LOWER_OR_EQUAL_THAN
            || ASTType(a) == AST_GREATER_OR_EQUAL_THAN)
    {
        return contains_wrongly_associated_template_name(ASTSon1(a), decl_context);
    }
    else if (ASTType(a) == AST_PREINCREMENT
            || ASTType(a) == AST_PREDECREMENT
            || ASTType(a) == AST_DERREFERENCE
            || ASTType(a) == AST_REFERENCE
            || ASTType(a) == AST_PLUS
            || ASTType(a) == AST_NEG
            || ASTType(a) == AST_LOGICAL_NOT
            || ASTType(a) == AST_BITWISE_NOT)
    {
        return contains_wrongly_associated_template_name(ASTSon0(a), decl_context);
    }
    else if (ASTType(a) == AST_NEW_EXPRESSION) // new TemplateName<
    {
        AST new_type_id_tree = ASTSon2(a);

        type_t* new_type_id = compute_type_for_type_id_tree(new_type_id_tree, decl_context,
                /* out simple type */ NULL, /* gather_info */ NULL);

        scope_entry_t* name = NULL;
        if (is_template_type(new_type_id))
        {
            return 1;
        }
        else if (is_named_type(new_type_id)
                && (name = named_type_get_symbol(new_type_id))
                && ((name->kind == SK_CLASS
                        && is_template_specialized_type(name->type_information))
                    || name->kind == SK_TEMPLATE
                    || name->kind == SK_TEMPLATE_ALIAS
                    || name->kind == SK_TEMPLATE_TEMPLATE_PARAMETER
                    || name->kind == SK_TEMPLATE_TEMPLATE_PARAMETER_PACK))
        {
            return 1;
        }
        else
        {
            return 0;
        }
    }
    else if (ASTType(a) == AST_SYMBOL                         // E + f<
            || ASTType(a) == AST_QUALIFIED_ID                 // E + A::f<
            || ASTType(a) == AST_CLASS_MEMBER_ACCESS          // E + a.f<
            || ASTType(a) == AST_POINTER_CLASS_MEMBER_ACCESS) // E + p->f<
    {
        nodecl_t nodecl_check = nodecl_null();
        diagnostic_context_push_buffered();
        check_expression_impl_(a, decl_context, &nodecl_check);
        diagnostic_context_pop_and_discard();

        if (nodecl_is_err_expr(nodecl_check))
            return 0;

        type_t* t = nodecl_get_type(nodecl_check);
        if (t != NULL
                && is_unresolved_overloaded_type(t))
        {

            scope_entry_list_t *overload_set = unresolved_overloaded_type_get_overload_set(t);
            char found = 0;

            scope_entry_list_iterator_t* it;
            for (it = entry_list_iterator_begin(overload_set);
                    !entry_list_iterator_end(it) && !found;
                    entry_list_iterator_next(it))
            {
                scope_entry_t* function = entry_list_iterator_current(it);
                found = (function->kind == SK_TEMPLATE);
            }

            entry_list_iterator_free(it);
            entry_list_free(overload_set);

            return found;
        }
        else
        {
            return 0;
        }
    }
    else
    {
        return 0;
    }
}

static void parse_lhs_lower_than(AST op,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    check_expression_impl_(op, decl_context, nodecl_output);

    if (!nodecl_is_err_expr(*nodecl_output)
            && contains_wrongly_associated_template_name(op, decl_context))
    {
        // This is something like a + p->f<3>(4) being parsed as a + (p->f<3)>4
        error_printf("%s: error: left-hand side of operator < is a template-name\n",
                ast_location(op));
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(op));
    }
}

static void parse_reference(AST op,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    CXX_LANGUAGE()
    {
        // C++ from now
        // We need this function because this operator is so silly in C++
        if (ASTType(op) == AST_QUALIFIED_ID)
        {
            nodecl_t op_name = nodecl_null();
            compute_nodecl_name_from_id_expression(op, decl_context, &op_name);

            if (nodecl_is_err_expr(op_name))
            {
                error_printf("%s: error: invalid qualified name '%s'\n",
                        ast_location(op), prettyprint_in_buffer(op));
                *nodecl_output = nodecl_make_err_expr(ast_get_locus(op));
                return;
            }

            field_path_t field_path;
            field_path_init(&field_path);
            scope_entry_list_t* entry_list = query_nodecl_name_flags(decl_context, op_name, &field_path, DF_DEPENDENT_TYPENAME);

            if (entry_list == NULL)
            {
                error_printf("%s: error: invalid qualified name '%s'\n",
                        ast_location(op), prettyprint_in_buffer(op));
                *nodecl_output = nodecl_make_err_expr(ast_get_locus(op));
                return;
            }

            scope_entry_t* entry = entry_list_head(entry_list);
            entry_list_free(entry_list);

            if (entry->kind == SK_TEMPLATE)
            {
                entry = named_type_get_symbol(template_type_get_primary_type(entry->type_information));
            }

            if ((entry->kind == SK_VARIABLE
                        || entry->kind == SK_FUNCTION)
                    && symbol_entity_specs_get_is_member(entry)
                    && !symbol_entity_specs_get_is_static(entry))
            {
                // This is a pointer to a member
                *nodecl_output = op_name;
                return;
            }
        }
    }

    // Usual check
    check_expression_impl_(op, decl_context, nodecl_output);
}

static void compute_operator_reference_type(nodecl_t* op,
        decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_BITWISE_AND_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    // If parse_reference passes us a qualified name we now this is a pointer
    // to member reference
    if (nodecl_get_kind(*op) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED
            || nodecl_get_kind(*op) == NODECL_CXX_DEP_NAME_NESTED)
    {
        scope_entry_list_t* entry_list = query_nodecl_name_flags(decl_context, *op, NULL, DF_DEPENDENT_TYPENAME);

        if (entry_list == NULL)
        {
            error_printf("%s: error: name '%s' not found in scope\n",
                    locus_to_str(locus),
                    codegen_to_str(*op, decl_context));

            *nodecl_output = nodecl_make_err_expr(locus);
        }

        ERROR_CONDITION(entry_list == NULL, "Invalid list", 0);

        scope_entry_t* entry = entry_list_head(entry_list);

        entry = entry_advance_aliases(entry);

        if (entry->kind == SK_TEMPLATE)
        {
            entry = named_type_get_symbol(template_type_get_primary_type(entry->type_information));
        }

        if (entry->kind == SK_VARIABLE)
        {
            *nodecl_output = nodecl_make_pointer_to_member(entry, 
                    get_pointer_to_member_type(entry->type_information,
                        symbol_entity_specs_get_class_type(entry)),
                    locus);
        }
        else if (entry->kind == SK_FUNCTION)
        {
            template_parameter_list_t* last_template_args = NULL;
            if (nodecl_name_ends_in_template_id(*op))
            {
                last_template_args = nodecl_name_get_last_template_arguments(*op);
            }
            type_t* t = get_unresolved_overloaded_type(entry_list, last_template_args);
            *nodecl_output = nodecl_make_reference(*op, t, locus);
        }
        else if (entry->kind == SK_DEPENDENT_ENTITY)
        {
            // This is trivially dependent
            *nodecl_output = nodecl_make_reference(*op,
                    get_unknown_dependent_type(),
                    locus);
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
            nodecl_expr_set_is_value_dependent(*nodecl_output,
                    nodecl_expr_is_value_dependent(*op));
        }
        else
        {
            internal_error("Code unreachable", 0);
        }
        entry_list_free(entry_list);
    }
    else
    {
        // This one is special
        if (is_unresolved_overloaded_type(nodecl_get_type(*op)))
        {
            *nodecl_output = nodecl_make_reference(*op,
                    nodecl_get_type(*op),
                    locus);
            nodecl_expr_set_is_type_dependent(*nodecl_output,
                    nodecl_expr_is_type_dependent(*op));
            nodecl_expr_set_is_value_dependent(*nodecl_output,
                    nodecl_expr_is_value_dependent(*op));
            return;
        }

        compute_unary_operator_generic(op, 
                operation_tree, decl_context,
                operand_is_class_or_enum,
                nodecl_make_reference,
                // No constants
                NULL,
                compute_type_no_overload_reference,
                NULL,
                operator_unary_reference_pred,
                operator_unary_reference_result,
                /* save_conversions */ 0,
                locus,
                nodecl_output);
    }
}

struct bin_operator_funct_type_t
{
    void (*pre_lhs)(AST lhs, decl_context_t, nodecl_t*);
    void (*pre_rhs)(nodecl_t lhs, AST rhs, decl_context_t, nodecl_t*);
    void (*func)(nodecl_t* lhs, nodecl_t* rhs, decl_context_t, const locus_t*, nodecl_t*);
};

static void check_expression_strict_operator(
        nodecl_t lhs UNUSED_PARAMETER,
        AST rhs, decl_context_t decl_context, nodecl_t* output)
{
    check_expression_impl_(rhs, decl_context, output);
}

// e1 || e2
static void check_expression_eval_rhs_if_lhs_is_zero(
        nodecl_t lhs,
        AST rhs, decl_context_t decl_context, nodecl_t* output)
{
    if (nodecl_is_constant(lhs)
            && const_value_is_zero(nodecl_get_constant(lhs)))
    {
        // 0 || e2
        // We evaluate e2 normally
        check_expression_impl_(rhs, decl_context, output);
    }
    else
    {
        // e1 || e2 but we do not evaluate e2
        // because either e1 is not constant
        // or it is a nonzero constant
        char do_not_call_constexpr = check_expr_flags.do_not_call_constexpr;
        check_expr_flags.do_not_call_constexpr = 1;

        check_expression_impl_(rhs, decl_context, output);

        check_expr_flags.do_not_call_constexpr = do_not_call_constexpr;

        if (nodecl_is_constant(lhs) // !const_value_is_zero(lhs)
                && !nodecl_is_constant(*output)
                && !nodecl_expr_is_value_dependent(*output))
        {
            // 1 || e2
            //
            // Absorb the rhs as well, so constant evaluation routines
            // see two constant nonzero values and believe everything is constant
            nodecl_set_constant(*output, nodecl_get_constant(lhs));
        }
    }
}


// e1 && e2
static void check_expression_eval_rhs_if_lhs_is_nonzero(
        nodecl_t lhs,
        AST rhs, decl_context_t decl_context, nodecl_t* output)
{
    if (nodecl_is_constant(lhs)
            && const_value_is_nonzero(nodecl_get_constant(lhs)))
    {
        // 1 && e2
        // We evaluate e2 normally
        check_expression_impl_(rhs, decl_context, output);
    }
    else
    {
        // e1 && e2 but we do not evaluate e2 because
        // either e1 is not constant or it as a zero constant
        char do_not_call_constexpr = check_expr_flags.do_not_call_constexpr;
        check_expr_flags.do_not_call_constexpr = 1;

        check_expression_impl_(rhs, decl_context, output);

        check_expr_flags.do_not_call_constexpr = do_not_call_constexpr;

        if (nodecl_is_constant(lhs)
                && !nodecl_is_constant(*output)
                && !nodecl_expr_is_value_dependent(*output))
        {
            // 0 && e2
            //
            // Nullify the rhs as well, so constant evaluation routines
            // see two constant zero values and believe everything is constant
            nodecl_set_constant(*output, nodecl_get_constant(lhs));
        }
    }
}

struct unary_operator_funct_type_t
{
    void (*pre)(AST op, decl_context_t, nodecl_t*);
    void (*func)(nodecl_t* operand, decl_context_t, const locus_t*, nodecl_t*);
};
#undef OPERATOR_FUNCT_INIT
#undef OPERATOR_FUNCT_INIT_PRE

#define OPERATOR_FUNCT_INIT(_x) { .pre_lhs = check_expression_impl_, .pre_rhs = check_expression_strict_operator, .func = _x }
#define OPERATOR_FUNCT_INIT_PRE(pre_lhs_, pre_rhs_, _x) { .pre_lhs = pre_lhs_, .pre_rhs = pre_rhs_, .func = _x }

static struct bin_operator_funct_type_t binary_expression_fun[] =
{
    [AST_ADD]                = OPERATOR_FUNCT_INIT(compute_bin_operator_add_type),
    [AST_MUL]                = OPERATOR_FUNCT_INIT(compute_bin_operator_mul_type),
    [AST_DIV]                = OPERATOR_FUNCT_INIT(compute_bin_operator_div_type),
    [AST_MOD]                = OPERATOR_FUNCT_INIT(compute_bin_operator_mod_type),
    [AST_MINUS]              = OPERATOR_FUNCT_INIT(compute_bin_operator_sub_type),
    [AST_SHR]                = OPERATOR_FUNCT_INIT(compute_bin_operator_shr_type),
    [AST_LOWER_THAN]            = OPERATOR_FUNCT_INIT_PRE(parse_lhs_lower_than,
                                                     check_expression_strict_operator,
                                                     compute_bin_operator_lower_than_type),
    [AST_GREATER_THAN]          = OPERATOR_FUNCT_INIT(compute_bin_operator_greater_than_type),
    [AST_GREATER_OR_EQUAL_THAN] = OPERATOR_FUNCT_INIT(compute_bin_operator_greater_equal_type),
    [AST_LOWER_OR_EQUAL_THAN]   = OPERATOR_FUNCT_INIT(compute_bin_operator_lower_equal_type),
    [AST_EQUAL]              = OPERATOR_FUNCT_INIT(compute_bin_operator_equal_type),
    [AST_DIFFERENT]          = OPERATOR_FUNCT_INIT(compute_bin_operator_different_type),
    [AST_BITWISE_AND]           = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_and_type),
    [AST_BITWISE_XOR]           = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_xor_type),
    [AST_BITWISE_OR]            = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_or_type),
    [AST_BITWISE_SHL]           = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_shl_type),

    [AST_LOGICAL_AND]           = OPERATOR_FUNCT_INIT_PRE(
            check_expression_impl_,
            check_expression_eval_rhs_if_lhs_is_nonzero,
            compute_bin_operator_logical_and_type),
    [AST_LOGICAL_OR]            = OPERATOR_FUNCT_INIT_PRE(
            check_expression_impl_,
            check_expression_eval_rhs_if_lhs_is_zero,
            compute_bin_operator_logical_or_type),

    [AST_POWER]              = OPERATOR_FUNCT_INIT(compute_bin_operator_pow_type),
    [AST_ASSIGNMENT]            = OPERATOR_FUNCT_INIT(compute_bin_operator_assig_type),
    [AST_MUL_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_mul_assig_type),
    [AST_DIV_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_div_assig_type),
    [AST_ADD_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_add_assig_type),
    [AST_SUB_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_sub_assig_type),
    [AST_SHR_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_shr_assig_type),
    [AST_BITWISE_AND_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_and_assig_type),
    [AST_BITWISE_OR_ASSIGNMENT ]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_or_assig_type),
    [AST_BITWISE_XOR_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_xor_assig_type),
    [AST_BITWISE_SHL_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_shl_assig_type),
    [AST_MOD_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_mod_assig_type),

    // Same as above for nodecl
    [NODECL_ADD]                = OPERATOR_FUNCT_INIT(compute_bin_operator_add_type),
    [NODECL_MUL]                = OPERATOR_FUNCT_INIT(compute_bin_operator_mul_type),
    [NODECL_DIV]                = OPERATOR_FUNCT_INIT(compute_bin_operator_div_type),
    [NODECL_MOD]                = OPERATOR_FUNCT_INIT(compute_bin_operator_mod_type),
    [NODECL_MINUS]              = OPERATOR_FUNCT_INIT(compute_bin_operator_sub_type),
    [NODECL_LOWER_THAN]            = OPERATOR_FUNCT_INIT(compute_bin_operator_lower_than_type),
    [NODECL_GREATER_THAN]          = OPERATOR_FUNCT_INIT(compute_bin_operator_greater_than_type),
    [NODECL_GREATER_OR_EQUAL_THAN] = OPERATOR_FUNCT_INIT(compute_bin_operator_greater_equal_type),
    [NODECL_LOWER_OR_EQUAL_THAN]   = OPERATOR_FUNCT_INIT(compute_bin_operator_lower_equal_type),
    [NODECL_EQUAL]              = OPERATOR_FUNCT_INIT(compute_bin_operator_equal_type),
    [NODECL_DIFFERENT]          = OPERATOR_FUNCT_INIT(compute_bin_operator_different_type),
    [NODECL_BITWISE_AND]           = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_and_type),
    [NODECL_BITWISE_XOR]           = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_xor_type),
    [NODECL_BITWISE_OR]            = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_or_type),
    [NODECL_BITWISE_SHL]           = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_shl_type),
    [NODECL_BITWISE_SHR]           = OPERATOR_FUNCT_INIT(compute_bin_operator_shr_type),
    [NODECL_ARITHMETIC_SHR]           = OPERATOR_FUNCT_INIT(compute_bin_operator_shr_type),
    [NODECL_LOGICAL_AND]           = OPERATOR_FUNCT_INIT(compute_bin_operator_logical_and_type),
    [NODECL_LOGICAL_OR]            = OPERATOR_FUNCT_INIT(compute_bin_operator_logical_or_type),
    [NODECL_POWER]              = OPERATOR_FUNCT_INIT(compute_bin_operator_pow_type),
    [NODECL_ASSIGNMENT]            = OPERATOR_FUNCT_INIT(compute_bin_operator_assig_type),
    [NODECL_MUL_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_mul_assig_type),
    [NODECL_DIV_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_div_assig_type),
    [NODECL_ADD_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_add_assig_type),
    [NODECL_MINUS_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_sub_assig_type),
    [NODECL_BITWISE_SHR_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_shr_assig_type),
    [NODECL_ARITHMETIC_SHR_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_shr_assig_type),
    [NODECL_BITWISE_AND_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_and_assig_type),
    [NODECL_BITWISE_OR_ASSIGNMENT ]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_or_assig_type),
    [NODECL_BITWISE_XOR_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_xor_assig_type),
    [NODECL_BITWISE_SHL_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_bitwise_shl_assig_type),
    [NODECL_MOD_ASSIGNMENT]        = OPERATOR_FUNCT_INIT(compute_bin_operator_mod_assig_type),
};

#undef OPERATOR_FUNCT_INIT
#undef OPERATOR_FUNCT_INIT_PRE
#define OPERATOR_FUNCT_INIT(_x) { .pre = check_expression_impl_, .func = _x }
#define OPERATOR_FUNCT_INIT_PRE(_pre, _x) { .pre = _pre, .func = _x }

static struct unary_operator_funct_type_t unary_expression_fun[] =
{
    [AST_DERREFERENCE]          = OPERATOR_FUNCT_INIT(compute_operator_derreference_type),
    [AST_REFERENCE]             = OPERATOR_FUNCT_INIT_PRE(parse_reference, compute_operator_reference_type),
    [AST_PLUS]               = OPERATOR_FUNCT_INIT(compute_operator_plus_type),
    [AST_NEG]                = OPERATOR_FUNCT_INIT(compute_operator_minus_type),
    [AST_LOGICAL_NOT]                = OPERATOR_FUNCT_INIT(compute_operator_not_type),
    [AST_BITWISE_NOT]         = OPERATOR_FUNCT_INIT(compute_operator_complement_type),

    // Same as above for nodecl
    [NODECL_DEREFERENCE]          = OPERATOR_FUNCT_INIT(compute_operator_derreference_type),
    [NODECL_REFERENCE]             = OPERATOR_FUNCT_INIT(compute_operator_reference_type),
    [NODECL_PLUS]               = OPERATOR_FUNCT_INIT(compute_operator_plus_type),
    [NODECL_NEG]                = OPERATOR_FUNCT_INIT(compute_operator_minus_type),
    [NODECL_LOGICAL_NOT]                = OPERATOR_FUNCT_INIT(compute_operator_not_type),
    [NODECL_BITWISE_NOT]         = OPERATOR_FUNCT_INIT(compute_operator_complement_type),
};

#undef OPERATOR_FUNCT_INIT
#undef OPERATOR_FUNCT_INIT_PRE

// Gives a name to an operation node
// 'a + b' will return 'operator+'
static const char *get_operation_function_name(node_t operation_tree)
{
    switch (operation_tree)
    {
        case AST_ADD :
        case NODECL_ADD:
            return STR_OPERATOR_ADD;
        case AST_MUL :
        case NODECL_MUL:
            return STR_OPERATOR_MULT;
        case AST_DIV :
        case NODECL_DIV:
            return STR_OPERATOR_DIV;
        case AST_MOD :
        case NODECL_MOD :
            return STR_OPERATOR_MOD;
        case AST_MINUS :
        case NODECL_MINUS :
            return STR_OPERATOR_MINUS;
        case AST_BITWISE_SHL :
        case NODECL_BITWISE_SHL :
            return STR_OPERATOR_SHIFT_LEFT;
        case AST_SHR :
        case NODECL_ARITHMETIC_SHR:
        case NODECL_BITWISE_SHR:
            return STR_OPERATOR_SHIFT_RIGHT;
        case AST_LOWER_THAN :
        case NODECL_LOWER_THAN:
            return STR_OPERATOR_LOWER_THAN;
        case AST_GREATER_THAN :
        case NODECL_GREATER_THAN:
            return STR_OPERATOR_GREATER_THAN;
        case AST_GREATER_OR_EQUAL_THAN :
        case NODECL_GREATER_OR_EQUAL_THAN:
            return STR_OPERATOR_GREATER_EQUAL;
        case AST_LOWER_OR_EQUAL_THAN :
        case NODECL_LOWER_OR_EQUAL_THAN:
            return STR_OPERATOR_LOWER_EQUAL;
        case AST_EQUAL :
        case NODECL_EQUAL:
            return STR_OPERATOR_EQUAL;
        case AST_DIFFERENT :
        case NODECL_DIFFERENT:
            return STR_OPERATOR_DIFFERENT;
        case AST_BITWISE_AND :
        case NODECL_BITWISE_AND:
            return STR_OPERATOR_BIT_AND;
        case AST_BITWISE_XOR :
        case NODECL_BITWISE_XOR:
            return STR_OPERATOR_BIT_XOR;
        case AST_BITWISE_OR :
        case NODECL_BITWISE_OR:
            return STR_OPERATOR_BIT_OR;
        case AST_LOGICAL_AND :
        case NODECL_LOGICAL_AND:
            return STR_OPERATOR_LOGIC_AND;
        case AST_LOGICAL_OR :
        case NODECL_LOGICAL_OR:
            return STR_OPERATOR_LOGIC_OR;
        case AST_DERREFERENCE :
        case NODECL_DEREFERENCE:
            return STR_OPERATOR_DERREF;
        case AST_REFERENCE : 
        case NODECL_REFERENCE:
            return STR_OPERATOR_REFERENCE;
        case AST_PLUS :
        case NODECL_PLUS:
            return STR_OPERATOR_UNARY_PLUS;
        case AST_NEG :
        case NODECL_NEG:
            return STR_OPERATOR_UNARY_NEG;
        case AST_LOGICAL_NOT :
        case NODECL_LOGICAL_NOT:
            return STR_OPERATOR_LOGIC_NOT;
        case AST_BITWISE_NOT :
        case NODECL_BITWISE_NOT:
            return STR_OPERATOR_BIT_NOT;
        case AST_ASSIGNMENT :
        case NODECL_ASSIGNMENT:
            return STR_OPERATOR_ASSIGNMENT;
        case AST_MUL_ASSIGNMENT :
        case NODECL_MUL_ASSIGNMENT :
            return STR_OPERATOR_MUL_ASSIGNMENT;
        case AST_DIV_ASSIGNMENT :
        case NODECL_DIV_ASSIGNMENT :
            return STR_OPERATOR_DIV_ASSIGNMENT;
        case AST_ADD_ASSIGNMENT :
        case NODECL_ADD_ASSIGNMENT :
            return STR_OPERATOR_ADD_ASSIGNMENT;
        case AST_SUB_ASSIGNMENT :
        case NODECL_MINUS_ASSIGNMENT :
            return STR_OPERATOR_MINUS_ASSIGNMENT;
        case AST_BITWISE_SHL_ASSIGNMENT :
        case NODECL_BITWISE_SHL_ASSIGNMENT :
            return STR_OPERATOR_SHL_ASSIGNMENT;
        case AST_SHR_ASSIGNMENT :
        case NODECL_ARITHMETIC_SHR_ASSIGNMENT:
        case NODECL_BITWISE_SHR_ASSIGNMENT:
            return STR_OPERATOR_SHR_ASSIGNMENT;
        case AST_BITWISE_AND_ASSIGNMENT :
        case NODECL_BITWISE_AND_ASSIGNMENT :
            return STR_OPERATOR_AND_ASSIGNMENT;
        case AST_BITWISE_OR_ASSIGNMENT :
        case NODECL_BITWISE_OR_ASSIGNMENT :
            return STR_OPERATOR_OR_ASSIGNMENT;
        case AST_BITWISE_XOR_ASSIGNMENT :
        case NODECL_BITWISE_XOR_ASSIGNMENT :
            return STR_OPERATOR_XOR_ASSIGNMENT;
        case AST_MOD_ASSIGNMENT :
        case NODECL_MOD_ASSIGNMENT :
            return STR_OPERATOR_MOD_ASSIGNMENT;
        case AST_PREINCREMENT :
        case NODECL_PREINCREMENT :
            return STR_OPERATOR_PREINCREMENT;
        case AST_POSTINCREMENT :
        case NODECL_POSTINCREMENT :
            return STR_OPERATOR_POSTINCREMENT;
        case AST_PREDECREMENT :
        case NODECL_PREDECREMENT :
            return STR_OPERATOR_PREDECREMENT;
        case AST_POSTDECREMENT :
        case NODECL_POSTDECREMENT :
            return STR_OPERATOR_POSTDECREMENT;
        default:
            internal_error("Invalid operation node %s", ast_print_node_type(operation_tree));
    }
}



static void check_unary_expression_(node_t node_kind,
        nodecl_t* op,
        decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    (unary_expression_fun[node_kind].func)(
            op,
            decl_context, 
            locus,
            nodecl_output);

    if (nodecl_is_err_expr(*nodecl_output))
    {
        type_t* t_op = nodecl_get_type(*op);
        C_LANGUAGE()
        {
            t_op = no_ref(t_op);
        }
        error_printf("%s: error: unary %s cannot be applied to operand '%s' (of type '%s')\n",
                locus_to_str(locus),
                get_operation_function_name(node_kind), 
                codegen_to_str(*op, decl_context), print_type_str(t_op, decl_context));

        nodecl_free(*op);
    }
}

static void check_binary_expression_(node_t node_kind,
        nodecl_t* nodecl_lhs,
        nodecl_t* nodecl_rhs,
        decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    (binary_expression_fun[node_kind].func)(
            nodecl_lhs, nodecl_rhs,
            decl_context, 
            locus,
            nodecl_output);

    if (nodecl_is_err_expr(*nodecl_output))
    {
        type_t* lhs_type = nodecl_get_type(*nodecl_lhs);
        type_t* rhs_type = nodecl_get_type(*nodecl_rhs);
        C_LANGUAGE()
        {
            lhs_type = no_ref(lhs_type);
            rhs_type = no_ref(rhs_type);
        }
        error_printf("%s: error: binary %s cannot be applied to operands '%s' (of type '%s') and '%s' (of type '%s')\n",
                locus_to_str(locus),
                get_operation_function_name(node_kind),
                codegen_to_str(*nodecl_lhs, decl_context), print_type_str(lhs_type, decl_context),
                codegen_to_str(*nodecl_rhs, decl_context), print_type_str(rhs_type, decl_context));

        nodecl_free(*nodecl_lhs);
        nodecl_free(*nodecl_rhs);
    }
}


static void check_binary_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST lhs = ASTSon0(expression);
    AST rhs = ASTSon1(expression);

    nodecl_t nodecl_lhs = nodecl_null();
    nodecl_t nodecl_rhs = nodecl_null();

    node_t node_kind = ASTType(expression);
    (binary_expression_fun[node_kind].pre_lhs)(lhs, decl_context, &nodecl_lhs);
    // We pass nodecl_lhs because some C/C++ operators are non-strict based on
    // the lhs
    (binary_expression_fun[node_kind].pre_rhs)(nodecl_lhs, rhs, decl_context, &nodecl_rhs);

    if (nodecl_is_err_expr(nodecl_lhs)
            || nodecl_is_err_expr(nodecl_rhs))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
        return;
    }

    check_binary_expression_(ASTType(expression),
            &nodecl_lhs,
            &nodecl_rhs,
            decl_context,
            ast_get_locus(expression),
            nodecl_output);
}

static void check_unary_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST op = ASTSon0(expression);

    nodecl_t nodecl_op = nodecl_null();

    node_t node_kind = ASTType(expression);
    (unary_expression_fun[node_kind].pre)(op, decl_context, &nodecl_op);

    if (nodecl_is_err_expr(nodecl_op))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        nodecl_free(nodecl_op);
        return;
    }

    check_unary_expression_(ASTType(expression), 
            &nodecl_op, 
            decl_context, 
            ast_get_locus(expression),
            nodecl_output);
}

static void check_throw_expression_nodecl(nodecl_t nodecl_thrown, const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (!nodecl_expr_is_value_dependent(nodecl_thrown)
            && check_expr_flags.must_be_constant)
    {
        error_printf("%s: error: throw-expression in constant-expression\n",
                locus_to_str(locus));
    }
    *nodecl_output = nodecl_make_throw(nodecl_thrown, get_throw_expr_type(), locus);
}

static void check_throw_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_thrown = nodecl_null();
    if (ASTSon0(expression) != NULL)
    {
        check_expression_impl_(ASTSon0(expression), decl_context, &nodecl_thrown);
    }

    check_throw_expression_nodecl(nodecl_thrown, ast_get_locus(expression), nodecl_output);
}

static void cxx_common_name_check(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);

static void compute_symbol_type_from_entry_list(scope_entry_list_t* result, 
        nodecl_t* nodecl_output,
        const locus_t* locus)
{
    scope_entry_t* entry = entry_advance_aliases(entry_list_head(result));

    if (entry->type_information != NULL
            && is_error_type(entry->type_information))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
    }
    else if (entry->kind == SK_ENUMERATOR)
    {
        *nodecl_output = nodecl_make_symbol(entry, locus);

        nodecl_set_type(*nodecl_output, entry->type_information);

        if (nodecl_is_constant(entry->value))
        {
            nodecl_set_constant(*nodecl_output, nodecl_get_constant(entry->value));
            if (const_value_is_zero(nodecl_get_constant(*nodecl_output)))
            {
                nodecl_set_type(*nodecl_output, get_zero_type(entry->type_information));
            }
        }

    }
    else if (entry->kind == SK_VARIABLE
                || entry->kind == SK_FUNCTION)
    {
        *nodecl_output = nodecl_make_symbol(entry, locus);
        if (symbol_entity_specs_get_is_member_of_anonymous(entry))
        {
            nodecl_t accessor = nodecl_shallow_copy(symbol_entity_specs_get_anonymous_accessor(entry));
            *nodecl_output = nodecl_make_class_member_access(
                    accessor,
                    *nodecl_output,
                    /* member literal */ nodecl_null(),
                    entry->type_information,
                    locus);
        }

        if (entry->kind == SK_VARIABLE
                && is_const_qualified_type(entry->type_information)
                && !nodecl_is_null(entry->value)
                && nodecl_is_constant(entry->value))
        {
            nodecl_set_constant(*nodecl_output, nodecl_get_constant(entry->value));
        }

        nodecl_set_type(*nodecl_output, lvalue_ref(entry->type_information));
    }
    else
    {
        error_printf("%s: error: name '%s' not valid in expression\n",
                locus_to_str(locus), entry->symbol_name);
        *nodecl_output = nodecl_make_err_expr(locus);
    }

    entry_list_free(result);
}

static void compute_symbol_type(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    if (IS_C_LANGUAGE)
    {

        scope_entry_list_t* result = NULL;
        result = query_nested_name(decl_context, NULL, NULL, expr, NULL); 

        if (result == NULL)
        {
            error_printf("%s: error: symbol '%s' not found in current scope\n",
                    ast_location(expr), ASTText(expr));

            *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
            return;
        }

        compute_symbol_type_from_entry_list(result, nodecl_output, ast_get_locus(expr));
    }
    else if (IS_CXX_LANGUAGE)
    {
        // C++ names are handled in another routine
        cxx_common_name_check(expr, decl_context, nodecl_output);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static void check_symbol(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    compute_symbol_type(expr, decl_context, nodecl_output);
}

nodecl_t cxx_integrate_field_accesses(nodecl_t base, nodecl_t accessor)
{
    if (nodecl_get_kind(accessor) == NODECL_CLASS_MEMBER_ACCESS)
    {
        nodecl_t accessor_base = nodecl_get_child(accessor, 0);
        nodecl_t accessor_symbol = nodecl_get_child(accessor, 1);
        ERROR_CONDITION(nodecl_get_kind(accessor_symbol) != NODECL_SYMBOL, "Invalid tree when integrating field accesses", 0);

        nodecl_t integrated_nodecl = cxx_integrate_field_accesses(base, accessor_base);

        return nodecl_make_class_member_access(
                integrated_nodecl,
                nodecl_shallow_copy(accessor_symbol),
                /* member literal */ nodecl_null(),
                lvalue_ref(nodecl_get_symbol(accessor_symbol)->type_information),
                nodecl_get_locus(integrated_nodecl));
    }
    else if (nodecl_get_kind(accessor) == NODECL_SYMBOL)
    {
        return nodecl_make_class_member_access(
                nodecl_shallow_copy(base),
                nodecl_shallow_copy(accessor),
                /* member literal */ nodecl_null(),
                lvalue_ref(nodecl_get_symbol(accessor)->type_information),
                nodecl_get_locus(base));
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static char any_is_member_function_of_a_dependent_class(scope_entry_list_t* candidates)
{
    char result = 0;

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(candidates);
            !entry_list_iterator_end(it) && !result;
            entry_list_iterator_next(it))
    {
        scope_entry_t* current_function = entry_list_iterator_current(it);
        result = (symbol_entity_specs_get_is_member(current_function)
                && is_dependent_type(symbol_entity_specs_get_class_type(current_function)));
    }
    entry_list_iterator_free(it);

    return result;
}


static void cxx_compute_name_from_entry_list(
        nodecl_t nodecl_name,
        scope_entry_list_t* entry_list,
        decl_context_t decl_context,
        field_path_t* field_path,
        nodecl_t* nodecl_output)
{
    if (entry_list != NULL
            && entry_list_size(entry_list) == 1)
    {
        scope_entry_t* entry = entry_list_head(entry_list);
        if (entry->kind == SK_DEPENDENT_ENTITY)
        {
            *nodecl_output = nodecl_shallow_copy(nodecl_name);
            nodecl_set_symbol(*nodecl_output, entry);
            nodecl_set_type(*nodecl_output, entry->type_information);
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
            return;
        }
    }

    if (entry_list == NULL)
    {
        error_printf("%s: error: symbol '%s' not found in current scope\n",
                nodecl_locus_to_str(nodecl_name), codegen_to_str(nodecl_name, nodecl_retrieve_context(nodecl_name)));
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
        return;
    }

    scope_entry_t* entry = entry_advance_aliases(entry_list_head(entry_list));

    // Check again if this is an alias to a dependent entity
    if (entry->kind == SK_DEPENDENT_ENTITY)
    {
        *nodecl_output = nodecl_shallow_copy(nodecl_name);
        nodecl_set_symbol(*nodecl_output, entry);
        nodecl_set_type(*nodecl_output, entry->type_information);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }

    if (entry->kind != SK_VARIABLE
                && entry->kind != SK_VARIABLE_PACK
                && entry->kind != SK_ENUMERATOR
                && entry->kind != SK_FUNCTION
                && entry->kind != SK_TEMPLATE // template functions
                && entry->kind != SK_TEMPLATE_NONTYPE_PARAMETER
                && entry->kind != SK_TEMPLATE_NONTYPE_PARAMETER_PACK)
    {
        error_printf("%s: error: name '%s' is not valid in this context\n",
                nodecl_locus_to_str(nodecl_name),
                codegen_to_str(nodecl_name, nodecl_retrieve_context(nodecl_name)));
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
        return;
    }

    if (entry->type_information != NULL
            && is_error_type(entry->type_information))
    {
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
        return;
    }

    template_parameter_list_t* last_template_args = NULL;
    if (nodecl_name_ends_in_template_id(nodecl_name))
    {
        last_template_args = nodecl_name_get_last_template_arguments(nodecl_name);
    }

    if (entry->kind == SK_VARIABLE)
    {
        nodecl_t nodecl_access_to_symbol = nodecl_make_symbol(entry, nodecl_get_locus(nodecl_name));

        nodecl_set_type(nodecl_access_to_symbol, lvalue_ref(entry->type_information));

        scope_entry_t* accessing_symbol = entry;

        if (!symbol_entity_specs_get_is_member(accessing_symbol)
                || symbol_entity_specs_get_is_static(accessing_symbol)
                || check_expr_flags.is_non_executable
                || symbol_is_member_of_dependent_class(entry))
        {
            *nodecl_output = nodecl_access_to_symbol;

            if (symbol_is_member_of_dependent_class(entry))
            {
                // This is like this->x
                nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
                nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
            }
        }
        else
        {
            DEBUG_CODE()
            {
                print_field_path(field_path);
            }

            type_t* this_type = NULL;

            scope_entry_t* this_symbol = resolve_symbol_this(decl_context);
            if (this_symbol != NULL)
            {
                // Construct (*this).x
                this_type = pointer_type_get_pointee_type(this_symbol->type_information);
            }

            scope_entry_t* accessed_class = named_type_get_symbol(symbol_entity_specs_get_class_type(accessing_symbol));
            while (symbol_entity_specs_get_is_anonymous_union(accessed_class)
                    && symbol_entity_specs_get_is_member(accessed_class))
            {
                accessed_class = named_type_get_symbol(symbol_entity_specs_get_class_type(accessed_class));
            }

            if (this_symbol != NULL
                    && class_type_is_base_instantiating(accessed_class->type_information, this_type,
                        nodecl_get_locus(nodecl_name)))
            {
                // Construct (*this).x
                cv_qualifier_t this_qualifier = get_cv_qualifier(this_type);

                nodecl_t nodecl_this_symbol =
                    nodecl_make_symbol(
                            this_symbol,
                            nodecl_get_locus(nodecl_name));

                nodecl_set_type(nodecl_this_symbol, this_symbol->type_information);

                nodecl_t nodecl_this_derref =
                    nodecl_make_dereference(
                            nodecl_this_symbol,
                            get_lvalue_reference_type(this_type),
                            nodecl_get_locus(nodecl_name));

                type_t* qualified_data_member_type = entry->type_information;
                if (!symbol_entity_specs_get_is_mutable(entry))
                {
                    qualified_data_member_type = get_cv_qualified_type(qualified_data_member_type, this_qualifier);
                }
                qualified_data_member_type = lvalue_ref(qualified_data_member_type);

                nodecl_t nodecl_base_access = nodecl_this_derref;

                // Now integrate every item in the field_path
                if (field_path != NULL)
                {
                    ERROR_CONDITION(field_path->length > 1, "Unexpected length for field path", 0);
                    if (field_path->length == 1)
                    {
                        nodecl_base_access = nodecl_make_class_member_access(
                                nodecl_base_access,
                                nodecl_make_symbol(field_path->path[0], nodecl_get_locus(nodecl_name)),
                                /* member_literal */ nodecl_null(),
                                field_path->path[0]->type_information,
                                nodecl_get_locus(nodecl_name));
                    }
                }

                if (symbol_entity_specs_get_is_member_of_anonymous(entry))
                {
                    nodecl_t accessor = symbol_entity_specs_get_anonymous_accessor(entry);
                    nodecl_base_access = cxx_integrate_field_accesses(nodecl_base_access, accessor);
                }

                *nodecl_output = nodecl_make_class_member_access(
                        nodecl_base_access,
                        nodecl_make_symbol(entry, nodecl_get_locus(nodecl_name)),
                        /* member literal */ nodecl_shallow_copy(nodecl_name),
                        qualified_data_member_type,
                        nodecl_get_locus(nodecl_name));
            }
            else
            {
                // Invalid access to a nonstatic member from a "this" lacking context
                error_printf("%s: error: cannot access to nonstatic data member '%s'\n",
                        nodecl_locus_to_str(nodecl_name),
                        get_qualified_symbol_name(entry, entry->decl_context));
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                return;
            }
        }

        if (!nodecl_is_null(entry->value)
                && (nodecl_expr_is_value_dependent(entry->value)
                    || nodecl_expr_is_type_dependent(entry->value)))
        {
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }

        if (is_dependent_type(entry->type_information))
        {
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }

        if (!nodecl_expr_is_value_dependent(*nodecl_output))
        {
            if ((entry->decl_context.current_scope->related_entry == NULL ||
                        !symbol_is_parameter_of_function(entry, entry->decl_context.current_scope->related_entry)))
            {
                if (!is_volatile_qualified_type(no_ref(entry->type_information))
                        && (is_const_qualified_type(no_ref(entry->type_information))
                            || symbol_entity_specs_get_is_constexpr(entry)))
                {
                    if (!nodecl_is_null(entry->value)
                            && nodecl_is_constant(entry->value))
                    {
                        nodecl_set_constant(*nodecl_output, nodecl_get_constant(entry->value));
                        if (symbol_entity_specs_get_is_constexpr(entry))
                        {
                            // ok
                        }
                        else if (is_const_qualified_type(no_ref(entry->type_information)))
                        {
                            if (is_integral_type(no_ref(entry->type_information))
                                    || is_enum_type(no_ref(entry->type_information)))
                            {
                                // ok
                            }
                            else if (check_expr_flags.must_be_constant == MUST_BE_CONSTANT
                                    /* || (check_expr_flags.must_be_constant == MUST_BE_NONTYPE_TEMPLATE_PARAMETER
                                        && !is_array_type(entry->type_information)) */)
                            {
                                error_printf("%s: error: const variable '%s' is not integral or "
                                        "enumeration type in constant expression\n",
                                        nodecl_locus_to_str(nodecl_name),
                                        get_qualified_symbol_name(entry, entry->decl_context));
                                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                                return;
                            }
                        }
                        else
                        {
                            internal_error("Code unreachable", 0);
                        }
                    }
                    else if (check_expr_flags.must_be_constant)
                    {
                        error_printf("%s: error: variable '%s' has not been initialized with a constant expression\n",
                                nodecl_locus_to_str(nodecl_name),
                                get_qualified_symbol_name(entry, entry->decl_context));
                        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                        return;
                    }
                }
                else if (check_expr_flags.must_be_constant == MUST_BE_CONSTANT)
                {
                    if (is_volatile_qualified_type(no_ref(entry->type_information)))
                    {
                        error_printf("%s: error: volatile variable '%s' in constant-expression\n",
                                nodecl_locus_to_str(nodecl_name),
                                get_qualified_symbol_name(entry, entry->decl_context));
                        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                        return;
                    }
                    else
                    {
                        if (IS_CXX11_LANGUAGE)
                        {
                            error_printf("%s: error: variable '%s' is not const nor constexpr in constant-expression\n",
                                    nodecl_locus_to_str(nodecl_name),
                                    get_qualified_symbol_name(entry, entry->decl_context));
                            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                            return;
                        }
                        else
                        {
                            error_printf("%s: error: variable '%s' is not const in constant-expression\n",
                                    nodecl_locus_to_str(nodecl_name),
                                    get_qualified_symbol_name(entry, entry->decl_context));
                            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                            return;
                        }
                    }
                }
            }
            else if (check_expr_flags.must_be_constant)
            {
                error_printf("%s: error: variable '%s' is not allowed in constant-expression\n",
                        nodecl_locus_to_str(nodecl_name),
                        get_qualified_symbol_name(entry, entry->decl_context));
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                return;
            }
        }
    }
    else if (entry->kind == SK_ENUMERATOR)
    {
        *nodecl_output = nodecl_make_symbol(entry, nodecl_get_locus(nodecl_name));
        nodecl_set_type(*nodecl_output, entry->type_information);

        if (is_dependent_type(entry->type_information)
                || (is_enum_type(entry->type_information)
                    && is_dependent_type(enum_type_get_underlying_type(entry->type_information))))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Found '%s' at '%s' to be type dependent\n",
                        nodecl_locus_to_str(nodecl_name), codegen_to_str(nodecl_name, nodecl_retrieve_context(nodecl_name)));
            }
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        }

        if (nodecl_expr_is_value_dependent(entry->value))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Found '%s' at '%s' to be value dependent\n",
                        nodecl_locus_to_str(nodecl_name), codegen_to_str(nodecl_name, nodecl_retrieve_context(nodecl_name)));
            }
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }
        else
        {
            ERROR_CONDITION(!nodecl_is_constant(entry->value), "This should be constant", 0);
            nodecl_set_constant(*nodecl_output, nodecl_get_constant(entry->value));
        }
    }
    else if (entry->kind == SK_FUNCTION
            || entry->kind == SK_TEMPLATE)
    {
        if (entry->kind == SK_TEMPLATE)
        {
            type_t* primary_named_type = template_type_get_primary_type(entry->type_information);
            scope_entry_t* named_type = named_type_get_symbol(primary_named_type);

            if (named_type->kind != SK_FUNCTION)
            {
                error_printf("%s: error: invalid template class-name '%s' in expression\n", 
                        nodecl_locus_to_str(nodecl_name),
                        codegen_to_str(nodecl_name, nodecl_retrieve_context(nodecl_name)));
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                return;
            }
        }

        type_t* t =  get_unresolved_overloaded_type(entry_list, last_template_args);
        *nodecl_output = nodecl_shallow_copy(nodecl_name);
        nodecl_set_type(*nodecl_output, t);

        if (last_template_args != NULL
                && has_dependent_template_parameters(last_template_args))
        {
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        }

        if (any_is_member_function_of_a_dependent_class(entry_list))
        {
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }
    }
    else if (entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER
            || entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK)
    {
        *nodecl_output = nodecl_make_symbol(entry, nodecl_get_locus(nodecl_name));

        // Template parameters may have a dependent type
        if (!is_dependent_type(entry->type_information))
        {
            nodecl_set_type(*nodecl_output, entry->type_information);
        }
        else
        {
            nodecl_set_type(*nodecl_output, get_unknown_dependent_type());
            nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        }
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
    }
    else if (entry->kind == SK_VARIABLE_PACK)
    {
        *nodecl_output = nodecl_make_symbol(entry, nodecl_get_locus(nodecl_name));

        ERROR_CONDITION(!is_pack_type(entry->type_information),
                "This variable pack should have pack type", 0);
        nodecl_set_type(*nodecl_output, pack_type_get_packed_type(entry->type_information));

        // This should always be type dependent as one cannot type
        // void f(int ... x)
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);

        // This is always value dependent
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
    }
    else
    {
        internal_error("code unreachable", 0);
    }
}

// Special g++ identifiers are handled here
char is_cxx_special_identifier(nodecl_t nodecl_name, nodecl_t* nodecl_output)
{
    ERROR_CONDITION(nodecl_is_null(nodecl_name), "Invalid tree", 0);
    ERROR_CONDITION(nodecl_is_err_expr(nodecl_name), "Invalid tree", 0);

    if (nodecl_get_kind(nodecl_name) == NODECL_CXX_DEP_NAME_SIMPLE)
    {
        const char* text = nodecl_get_text(nodecl_name);
        // __null is a special item in g++
        if (strcmp(text, "__null") == 0)
        {
            type_t* t = get_variant_type_zero((CURRENT_CONFIGURATION->type_environment->type_of_ptrdiff_t)());

            *nodecl_output = nodecl_make_integer_literal(
                    t,
                    const_value_get_integer(0, type_get_size(t), 1),
                    nodecl_get_locus(nodecl_name));

            return 1;
        }
    }

    return 0;
}

static void cxx_common_name_check(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_name = nodecl_null();
    compute_nodecl_name_from_id_expression(expr, decl_context, &nodecl_name);

    if (nodecl_is_err_expr(nodecl_name))
    {
        *nodecl_output = nodecl_name;
        return;
    }

    if (is_cxx_special_identifier(nodecl_name, nodecl_output))
        return;

    field_path_t field_path;
    field_path_init(&field_path);

    scope_entry_list_t* result_list = query_nodecl_name_flags(
            decl_context,
            nodecl_name,
            &field_path,
            DF_DEPENDENT_TYPENAME |
            DF_IGNORE_FRIEND_DECL |
            DF_DO_NOT_CREATE_UNQUALIFIED_DEPENDENT_ENTITY);

    cxx_compute_name_from_entry_list(nodecl_name, result_list, decl_context, &field_path, nodecl_output);

    entry_list_free(result_list);
    nodecl_free(nodecl_name);
}

static void solve_literal_symbol(AST expression, decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    const char *tmp = ASTText(ASTSon0(expression));

    const char * prefix = NULL;
    void *p = NULL;
    unpack_pointer(tmp, &prefix, &p);
    
    ERROR_CONDITION(prefix == NULL || p == NULL || strcmp(prefix, "symbol") != 0,
            "Failure during unpack of symbol", 0);

    scope_entry_t* entry = (scope_entry_t*)p;
    scope_entry_list_t* entry_list = entry_list_new(entry);

    if (IS_C_LANGUAGE)
    {
        compute_symbol_type_from_entry_list(entry_list, nodecl_output,
                ast_get_locus(expression));
    }
    else if (IS_CXX_LANGUAGE)
    {
        nodecl_t nodecl_name = nodecl_make_cxx_dep_name_simple(entry->symbol_name,
                ast_get_locus(expression));

        cxx_compute_name_from_entry_list(nodecl_name, entry_list, decl_context, NULL, nodecl_output);

        entry_list_free(entry_list);
        nodecl_free(nodecl_name);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static void check_mcc_debug_array_subscript(AST a,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    AST expr = ast_get_child(a, 0);
    check_expression_impl_(expr, decl_context, nodecl_output);

    if (nodecl_is_err_expr(*nodecl_output))
        return;

    AST length_expr = ast_get_child(a, 1);
    nodecl_t nodecl_length_expr = nodecl_null();
    check_expression_impl_(length_expr, decl_context, &nodecl_length_expr);

    if (nodecl_is_err_expr(nodecl_length_expr))
    {
        *nodecl_output = nodecl_length_expr;
        return;
    }


    if (nodecl_get_kind(*nodecl_output) != NODECL_ARRAY_SUBSCRIPT)
    {
        error_printf("%s: error: @array-subscript-check@ requires an array subscript as the first operand\n",
                ast_location(a));
        return;
    }
    nodecl_t nodecl_subscript_list = nodecl_get_child(*nodecl_output, 1);

    if (!nodecl_is_constant(nodecl_length_expr)
            && !const_value_is_integer(nodecl_get_constant(nodecl_length_expr)))
    {
        error_printf("%s: error: @array-subscript-check@ requires a constant expression of integer kind as the second argument\n",
                ast_location(a));
        return;
    }
    int expected_length = const_value_cast_to_signed_int(nodecl_get_constant(nodecl_length_expr));
    int real_length = nodecl_list_length(nodecl_subscript_list);

    if (expected_length != real_length)
    {
        error_printf("%s: error: array-subscript-check failure, "
                "expected length is '%d' but the subscript list is of length '%d'\n",
                ast_location(a),
                expected_length,
                real_length);
    }
}

static void check_mcc_debug_constant_value_check(AST a,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    check_expression_impl_(ASTSon0(a), decl_context, nodecl_output);

    if (nodecl_is_err_expr(*nodecl_output))
        return;

    if (!nodecl_is_constant(*nodecl_output))
    {
        error_printf("%s: error: const-value-check failure, expression '%s' is not constant expression\n",
                ast_location(a),
                codegen_to_str(*nodecl_output, decl_context));
    }
}

static const_value_t* compute_subconstant_of_array_subscript(
        type_t* subscripted_type,
        nodecl_t subscripted,
        nodecl_t subscript_list)
{
    if (!nodecl_is_constant(subscripted))
        return NULL;

    subscripted_type = no_ref(subscripted_type);
    if (!is_array_type(subscripted_type))
        return NULL;

    int num_subscripts = 0;
    nodecl_t* list = nodecl_unpack_list(subscript_list, &num_subscripts);

    int i;
    for (i = 0; i < num_subscripts; i++)
    {
        if (!nodecl_is_constant(list[i]))
        {
            xfree(list);
            return NULL;
        }
    }

    const_value_t* cval = nodecl_get_constant(subscripted);
    ERROR_CONDITION(!const_value_is_array(cval),
            "Invalid constant value '%s'", const_value_to_str(cval));

    for (i = 0; i < num_subscripts; i++)
    {
        if (!nodecl_is_constant(
                    array_type_get_array_size_expr(subscripted_type)))
        {
            xfree(list);
            return NULL;
        }

        int length =
            const_value_cast_to_signed_int(
                    nodecl_get_constant(
                        array_type_get_array_size_expr(subscripted_type)));

        int idx = const_value_cast_to_signed_int(
                nodecl_get_constant(list[i]));

        if (idx < 0 || idx >= length)
        {
            xfree(list);
            return NULL;
        }

        ERROR_CONDITION(const_value_get_num_elements(cval) <= idx,
                "Constant '%s' has too few elements (index = %d requested)", const_value_to_str(cval), idx);

        cval = const_value_get_element_num(cval, idx);
    }

    xfree(list);

    return cval;
}


static void check_nodecl_array_subscript_expression_c(
        nodecl_t nodecl_subscripted,
        nodecl_t nodecl_subscript,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(nodecl_subscripted);

    if (nodecl_is_err_expr(nodecl_subscripted)
            || nodecl_is_err_expr(nodecl_subscript))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_subscripted);
        nodecl_free(nodecl_subscript);
        return;
    }

    if (check_expr_flags.must_be_constant)
    {
        error_printf("%s: error: array subscript in a constant expression\n",
                nodecl_locus_to_str(nodecl_subscripted));
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    type_t* subscript_type = nodecl_get_type(nodecl_subscript);
    type_t* subscripted_type = nodecl_get_type(nodecl_subscripted);

    if (is_pointer_and_integral_type(no_ref(subscript_type), no_ref(subscripted_type)))
    {
        // C oddity: since E1[E2] is equivalent to *(E1 + E2) and it is also
        // valid *(E2 + E1), then E2[E1] is valid too
        // Swap everything E1[E2] can be E2[E1] if one is a pointer and the other an integral type
        {
            nodecl_t t = nodecl_subscripted;
            nodecl_subscripted = nodecl_subscript;
            nodecl_subscript = t;
        }

        subscripted_type = nodecl_get_type(nodecl_subscripted);
        subscript_type = nodecl_get_type(nodecl_subscript);
    }

    // Builtin cases
    if (is_array_type(no_ref(subscripted_type))
            || is_pointer_type(no_ref(subscripted_type)))
    {
        if (!is_integral_type(no_ref(subscript_type)) &&
                !is_unscoped_enum_type(no_ref(subscript_type)))
        {
            error_printf("%s: error: subscript expression '%s' of type '%s' cannot be implicitly converted to '%s'\n",
                    locus_to_str(nodecl_get_locus(nodecl_subscript)),
                    codegen_to_str(nodecl_subscript, decl_context),
                    print_type_str(no_ref(subscript_type), decl_context),
                    print_type_str(get_ptrdiff_t_type(), decl_context));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_subscripted);
            nodecl_free(nodecl_subscript);
            return;
        }
        else
        {
            // convert int -> ptrdiff_t
            unary_record_conversion_to_result(
                    get_ptrdiff_t_type(),
                    &nodecl_subscript);
        }
    }

    if (is_array_type(no_ref(subscripted_type))
            && (nodecl_get_kind(nodecl_subscripted) == NODECL_ARRAY_SUBSCRIPT)
            && !(array_type_has_region(no_ref(subscripted_type))
                && is_pointer_type(array_type_get_element_type(no_ref(subscripted_type)))))
    {
        type_t* t = lvalue_ref(array_type_get_element_type(no_ref(subscripted_type)));

        // We combine the array subscript list
        nodecl_t nodecl_indexed = nodecl_shallow_copy(nodecl_get_child(nodecl_subscripted, 0));
        nodecl_t nodecl_subscript_list = nodecl_shallow_copy(nodecl_get_child(nodecl_subscripted, 1));

        nodecl_subscript_list = nodecl_append_to_list(nodecl_subscript_list,
                nodecl_shallow_copy(nodecl_subscript));

        *nodecl_output = nodecl_make_array_subscript(
                nodecl_indexed,
                nodecl_subscript_list,
                lvalue_ref(t), locus);

        const_value_t* const_value = compute_subconstant_of_array_subscript(
                subscripted_type,
                nodecl_indexed,
                nodecl_subscript_list);
        nodecl_set_constant(*nodecl_output, const_value);
    }
    else if (is_array_type(no_ref(subscripted_type)))
    {
        type_t* t = lvalue_ref(array_type_get_element_type(no_ref(subscripted_type)));
        // The subscripted type may be T[n] or T(&)[n] and we want it to become T*
        nodecl_t nodecl_indexed = nodecl_shallow_copy(nodecl_subscripted);
        nodecl_t nodecl_subscript_list = nodecl_make_list_1(
                nodecl_shallow_copy(nodecl_subscript)
                );

        unary_record_conversion_to_result(
                get_pointer_type(array_type_get_element_type(no_ref(subscripted_type))),
                &nodecl_indexed);

        *nodecl_output = nodecl_make_array_subscript(
                nodecl_indexed,
                nodecl_subscript_list,
                lvalue_ref(t), locus);

        const_value_t* const_value = compute_subconstant_of_array_subscript(
                subscripted_type,
                nodecl_indexed,
                nodecl_subscript_list);
        nodecl_set_constant(*nodecl_output, const_value);
    }
    else if (is_pointer_type(no_ref(subscripted_type)))
    {
        type_t* t = lvalue_ref(pointer_type_get_pointee_type(no_ref(subscripted_type)));

        nodecl_t nodecl_indexed = nodecl_shallow_copy(nodecl_subscripted);
        nodecl_t nodecl_subscript_list = nodecl_make_list_1(
                nodecl_shallow_copy(nodecl_subscript)
                );

        // The subscripted type may be T*& and we want it to be T*
        unary_record_conversion_to_result(no_ref(subscripted_type), &nodecl_indexed);

        *nodecl_output = nodecl_make_array_subscript(
                nodecl_indexed,
                nodecl_subscript_list,
                lvalue_ref(t), locus);
    }
    else
    {
        error_printf("%s: error: expression '%s[%s]' is invalid since '%s' has type '%s' which is "
                "neither an array-type or pointer-type\n",
                nodecl_locus_to_str(nodecl_subscripted),
                codegen_to_str(nodecl_subscripted, nodecl_retrieve_context(nodecl_subscripted)),
                codegen_to_str(nodecl_subscript, nodecl_retrieve_context(nodecl_subscript)),
                codegen_to_str(nodecl_subscripted, nodecl_retrieve_context(nodecl_subscripted)),
                print_type_str(no_ref(subscripted_type), decl_context));

        *nodecl_output = nodecl_make_err_expr(locus);
    }

    nodecl_free(nodecl_subscripted);
    nodecl_free(nodecl_subscript);
}

static char pointer_type_and_integral_or_unscoped_enum_type(type_t* t1, type_t* t2)
{
    return (is_pointer_type(no_ref(t1)) || is_array_type(no_ref(t1)))
        && (is_integral_type(no_ref(t2)) || is_unscoped_enum_type(no_ref(t2)));
}

static char array_subcript_types_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    // T& operator[](T*, ptrdiff_t)
    // T& operator[](ptrdiff_t, T*)
    return pointer_type_and_integral_or_unscoped_enum_type(lhs, rhs)
        || pointer_type_and_integral_or_unscoped_enum_type(rhs, lhs);
}

static type_t* array_subscript_types_result(type_t** lhs, type_t** rhs, const locus_t* locus UNUSED_PARAMETER)
{
    if (pointer_type_and_integral_or_unscoped_enum_type(*lhs, *rhs))
    {
        *lhs = no_ref(*lhs);
        if (is_array_type(*lhs))
            *lhs = get_pointer_type(array_type_get_element_type(*lhs));

        *rhs = get_ptrdiff_t_type();

        *lhs = get_unqualified_type(*lhs);

        return lvalue_ref(pointer_type_get_pointee_type(*lhs));
    }
    else if (pointer_type_and_integral_or_unscoped_enum_type(*rhs, *lhs))
    {
        *lhs = get_ptrdiff_t_type();

        *rhs = no_ref(*rhs);
        if (is_array_type(*rhs))
            *rhs = get_pointer_type(array_type_get_element_type(*rhs));

        *rhs = get_unqualified_type(*rhs);

        return lvalue_ref(pointer_type_get_pointee_type(*rhs));
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static void check_nodecl_array_subscript_expression_cxx(
        nodecl_t nodecl_subscripted, 
        nodecl_t nodecl_subscript, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(nodecl_subscripted);

    if (nodecl_is_err_expr(nodecl_subscripted)
            || nodecl_is_err_expr(nodecl_subscript))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_subscripted);
        nodecl_free(nodecl_subscript);
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_subscripted)
            || nodecl_expr_is_type_dependent(nodecl_subscript))
    {
        *nodecl_output = nodecl_make_array_subscript(nodecl_subscripted, 
                nodecl_make_list_1(nodecl_subscript),
                get_unknown_dependent_type(), 
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    type_t* subscripted_type = nodecl_get_type(nodecl_subscripted);
    type_t* subscript_type = nodecl_get_type(nodecl_subscript);

    static AST operator_subscript_tree = NULL;
    if (operator_subscript_tree == NULL)
    {
        operator_subscript_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_SUBSCRIPT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    // Try to see if an overload operator[] is useable
    if (is_class_type(no_ref(subscripted_type)))
    {
        scope_entry_list_t* operator_subscript_list = get_member_of_class_type(
                no_ref(subscripted_type),
                operator_subscript_tree,
                decl_context, NULL);

        // Solve operator[]. It is always a member operator
        int num_arguments = 2;
        type_t* argument_types[2] = { subscripted_type, subscript_type };

        scope_entry_list_t* overload_set = unfold_and_mix_candidate_functions(operator_subscript_list,
                /* builtins */ NULL, argument_types + 1, num_arguments - 1,
                decl_context,
                locus,
                /* explicit_template_arguments */ NULL);
        entry_list_free(operator_subscript_list);

        candidate_t* candidate_set = NULL;
        scope_entry_list_iterator_t* it = NULL;
        for (it = entry_list_iterator_begin(overload_set);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it))
        {
            candidate_set = candidate_set_add(candidate_set,
                    entry_list_iterator_current(it),
                    num_arguments,
                    argument_types);
        }
        entry_list_iterator_free(it);
        entry_list_free(overload_set);

        scope_entry_t *orig_overloaded_call = solve_overload(candidate_set,
                decl_context,
                locus);
        scope_entry_t* overloaded_call = entry_advance_aliases(orig_overloaded_call);

        if (overloaded_call != NULL)
        {
            candidate_set_free(&candidate_set);

            if (function_has_been_deleted(decl_context, overloaded_call, locus))
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                nodecl_free(nodecl_subscripted);
                nodecl_free(nodecl_subscript);
                return;
            }

            type_t* param_type = function_type_get_parameter_type_num(overloaded_call->type_information, 0);

            nodecl_t old_nodecl_subscript = nodecl_subscript;
            check_nodecl_function_argument_initialization(nodecl_subscript, decl_context, param_type,
                    /* disallow_narrowing */ 0,
                    &nodecl_subscript);
            if (nodecl_is_err_expr(nodecl_subscript))
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                nodecl_free(nodecl_subscripted);
                nodecl_free(old_nodecl_subscript);
                return;
            }

            type_t* t = function_type_get_return_type(overloaded_call->type_information);

            // a[b] becomes a.operator[](b)
            *nodecl_output = cxx_nodecl_make_function_call(
                    nodecl_make_symbol(overloaded_call, locus),
                    /* called name */ nodecl_null(),
                    nodecl_make_list_2(nodecl_subscripted, nodecl_subscript),
                    // Ideally this should have a specific function form
                    /* function-form */ nodecl_null(),
                    t,
                    decl_context,
                    locus);
            return;
        }
    }

    // Now go back to usual builtins
    builtin_operators_set_t builtin_set;
    build_binary_builtin_operators(
            nodecl_get_type(nodecl_subscripted),
            nodecl_get_type(nodecl_subscript),
            &builtin_set, decl_context,
            operator_subscript_tree,
            array_subcript_types_pred,
            array_subscript_types_result,
            locus);

    scope_entry_list_t* builtins = get_entry_list_from_builtin_operator_set(&builtin_set);

    scope_entry_t* selected_operator = NULL;

    // This is a bit convoluted. First we duplicate lhs and rhs
    nodecl_t lhs = nodecl_shallow_copy(nodecl_subscripted);
    nodecl_t rhs = nodecl_shallow_copy(nodecl_subscript);

    // Now we compute a user defined operator[] (using only the builtins)
    // This call may modify lhs and rhs
    type_t* result = compute_user_defined_bin_operator_type(operator_subscript_tree, 
            &lhs, &rhs, builtins, decl_context, locus, &selected_operator);
    entry_list_free(builtins);

    if (selected_operator != NULL)
    {
        ERROR_CONDITION(!symbol_entity_specs_get_is_builtin(selected_operator), "operator[] is not a builtin\n", 0);

        type_t* param0 = function_type_get_parameter_type_num(selected_operator->type_information, 0);
        type_t* param1 = function_type_get_parameter_type_num(selected_operator->type_information, 1);

        // E1[E2] is E1 + E2 so E2[E1] is valid as well, make E1 always the
        // array or pointer and E2 the index
        if (is_pointer_type(param0))
        {
            // Do nothing
        }
        else if (is_pointer_type(param1))
        {
            nodecl_t tmp = nodecl_subscripted;
            nodecl_subscripted = nodecl_subscript;
            nodecl_subscript = tmp;

            tmp = lhs;
            lhs = rhs;
            rhs = tmp;
        }
        else
        {
            internal_error("Code unreachable", 0);
        }
        subscripted_type = nodecl_get_type(nodecl_subscripted);
        subscript_type = nodecl_get_type(nodecl_subscript);

        if (check_expr_flags.must_be_constant
                && nodecl_get_kind(nodecl_subscripted) == NODECL_SYMBOL
                && is_array_type(no_ref(nodecl_get_symbol(nodecl_subscripted)->type_information))
                && !symbol_entity_specs_get_is_constexpr(nodecl_get_symbol(nodecl_subscripted)))
        {
            if (IS_CXX11_LANGUAGE)
            {
                error_printf("%s: error: array subscript of non-constexpr array '%s' in a constant expression\n",
                        nodecl_locus_to_str(nodecl_subscripted),
                        codegen_to_str(nodecl_subscripted, decl_context));
            }
            else
            {
                error_printf("%s: error: array subscript in a constant expression\n",
                        nodecl_locus_to_str(nodecl_subscripted));
            }
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        // Now make sure multidimensional arrays are properly materialized
        if (is_array_type(no_ref(subscripted_type))
                && (nodecl_get_kind(nodecl_subscripted) == NODECL_ARRAY_SUBSCRIPT)
                && !(array_type_has_region(no_ref(subscripted_type))
                    && is_pointer_type(array_type_get_element_type(no_ref(subscripted_type)))))
        {
            nodecl_t nodecl_indexed = nodecl_shallow_copy(
                    nodecl_get_child(nodecl_subscripted, 0));
            nodecl_t nodecl_subscript_list = nodecl_shallow_copy(
                    nodecl_get_child(nodecl_subscripted, 1));

            nodecl_subscript_list = nodecl_append_to_list(nodecl_subscript_list,
                    rhs);

            *nodecl_output = nodecl_make_array_subscript(
                    nodecl_indexed,
                    nodecl_subscript_list,
                    result, locus);

            nodecl_free(lhs);

            const_value_t* const_value = compute_subconstant_of_array_subscript(
                    subscripted_type,
                    nodecl_indexed,
                    nodecl_subscript_list);
            nodecl_set_constant(*nodecl_output, const_value);

        }
        else
        {
            nodecl_t nodecl_indexed = nodecl_shallow_copy(
                    nodecl_subscripted);
            nodecl_t nodecl_subscript_list = nodecl_make_list_1(rhs);

            *nodecl_output = nodecl_make_array_subscript(
                    lhs,
                    nodecl_subscript_list,
                    result, locus);

            const_value_t* const_value = compute_subconstant_of_array_subscript(
                    subscripted_type,
                    nodecl_indexed,
                    nodecl_subscript_list);
            nodecl_set_constant(*nodecl_output, const_value);
        }
    }
    else
    {

        error_printf("%s: error: in '%s[%s]' no matching operator[] for types '%s'\n",
                nodecl_locus_to_str(nodecl_subscripted),
                codegen_to_str(nodecl_subscripted, nodecl_retrieve_context(nodecl_subscripted)),
                codegen_to_str(nodecl_subscript, nodecl_retrieve_context(nodecl_subscript)),
                print_type_str(subscripted_type, decl_context));
        *nodecl_output = nodecl_make_err_expr(locus);
    }

    nodecl_free(nodecl_subscripted);
    nodecl_free(nodecl_subscript);
}

static void check_nodecl_array_subscript_expression(
        nodecl_t nodecl_subscripted, 
        nodecl_t nodecl_subscript, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    if (IS_C_LANGUAGE)
    {
        check_nodecl_array_subscript_expression_c(nodecl_subscripted,
                nodecl_subscript,
                decl_context,
                nodecl_output);
    }
    else if (IS_CXX_LANGUAGE)
    {
        check_nodecl_array_subscript_expression_cxx(nodecl_subscripted,
                nodecl_subscript,
                decl_context,
                nodecl_output);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static void check_array_subscript_expr(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_subscripted = nodecl_null();
    check_expression_impl_(ASTSon0(expr), decl_context, &nodecl_subscripted);

    nodecl_t nodecl_subscript = nodecl_null();
    check_expression_impl_(ASTSon1(expr), decl_context, &nodecl_subscript);

    check_nodecl_array_subscript_expression(nodecl_subscripted, nodecl_subscript, decl_context, nodecl_output);
}

static void check_unqualified_conversion_function_id(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // This is the case of an "operator T" used alone (not in a class-member access like "a.operator T")
    //
    // The standard says that we should look up T both in class scope and the current scope. To my understanding
    // it tacitly implies the existence of a class scope.

    if (decl_context.class_scope == NULL)
    {
        error_printf("%s: error: a class-scope is required to name a conversion function\n", ast_location(expression));
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        return;
    }

    type_t* conversion_type = NULL;
    /* const char* conversion_name = */ get_conversion_function_name(decl_context, expression, &conversion_type);

    if (conversion_type == NULL)
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        return;
    }

    compute_nodecl_name_from_id_expression(expression, decl_context, nodecl_output);
    // Keep the conversion type
    nodecl_set_child(*nodecl_output, 1,
            nodecl_make_type(conversion_type, ast_get_locus(expression)));
    // Nullify this tree
    nodecl_set_child(*nodecl_output, 2, nodecl_null());

    if (is_dependent_type(conversion_type))
    {
        char ok = compute_type_of_dependent_conversion_type_id(*nodecl_output, decl_context);
        if (!ok)
        {
            *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
            return;
        }

        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    scope_entry_list_t* entry_list = query_conversion_function_info(decl_context, conversion_type,
            ast_get_locus(expression));

    if (entry_list == NULL)
    {
        error_printf("%s: error: 'operator %s' not found in the current scope\n",
                ast_location(expression),
                print_type_str(conversion_type, decl_context));
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        return;
    }
    else
    {
        nodecl_set_type(*nodecl_output, get_unresolved_overloaded_type(entry_list, NULL));
    }
}

static char convert_in_conditional_expr(type_t* from_t1, type_t* to_t2,
        char *is_ambiguous_conversion,
        type_t** converted_type,
        decl_context_t decl_context,
        const locus_t* locus)
{
    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Trying to convert in conditional expression from '%s' to '%s'\n",
                print_declarator(from_t1),
                print_declarator(to_t2));
    }
    *is_ambiguous_conversion = 0;

    if (is_lvalue_reference_type(to_t2)
            // This enforces that the conversion is a direct binding
            && is_lvalue_reference_type(from_t1))
    {
        /*
         * If E2 is a lvalue, E1 can be converted to match E2 if E1 can be implicitly
         * converted to the type 'reference of T2'
         */
        standard_conversion_t result;
        if (standard_conversion_between_types(&result, from_t1, to_t2, locus))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: In conditional expression conversion, "
                        "direct binding to lvalue reference from '%s' to '%s'\n",
                        print_declarator(from_t1),
                        print_declarator(to_t2));
            }
            *converted_type = to_t2;
            return 1;
        }
    }

    if (is_rvalue_reference_type(to_t2)
            // This enforces that the conversion is a direct binding
            && is_rvalue_reference_type(from_t1))
    {
        /*
         * If E2 is a lvalue, E1 can be converted to match E2 if E1 can be implicitly
         * converted to the type 'reference of T2'
         */
        standard_conversion_t result;
        if (standard_conversion_between_types(&result, from_t1, to_t2, locus))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: In conditional expression conversion, "
                        "direct binding to rvalue reference from '%s' to '%s'\n",
                        print_declarator(from_t1),
                        print_declarator(to_t2));
            }
            *converted_type = to_t2;
            return 1;
        }
    }

    if (!is_lvalue_reference_type(to_t2)
            || is_class_type(no_ref(from_t1))
            || is_class_type(no_ref(to_t2)))
    {
        // Try a conversion between derived-to-base values
        if (is_class_type(no_ref(from_t1))
                && is_class_type(no_ref(to_t2))
                && class_type_is_base_instantiating(no_ref(to_t2), no_ref(from_t1), locus)
                && is_more_or_equal_cv_qualified_type(no_ref(to_t2), no_ref(from_t1)))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: In conditional expression conversion, "
                        "conversion from derived (or same) class '%s' to '%s'\n",
                        print_declarator(from_t1),
                        print_declarator(to_t2));
            }
            // If the conversion is applied, E1 is changed to an
            // rvalue of type T2
            *converted_type = no_ref(to_t2);
            return 1;
        }

        // Try an implicit conversion
        if (!is_class_type(no_ref(from_t1))
                || !is_class_type(no_ref(to_t2))
                || !class_type_is_base_instantiating(no_ref(to_t2), no_ref(from_t1), locus))
        {
            nodecl_t nodecl_expr = nodecl_null();
            diagnostic_context_push_buffered();
            check_nodecl_function_argument_initialization(
                    nodecl_make_dummy(from_t1, locus),
                    decl_context,
                    get_unqualified_type(no_ref(to_t2)),
                    /* disallow_narrowing */ 0,
                    &nodecl_expr);
            diagnostic_context_pop_and_discard();

            if (!nodecl_is_err_expr(nodecl_expr))
            {
                DEBUG_CODE()
                {
                    fprintf(stderr, "EXPRTYPE: In conditional expression conversion, "
                            "implicit conversion from '%s' to a rvalue of '%s'\n",
                            print_declarator(from_t1),
                            print_declarator(no_ref(to_t2)));
                }
                *converted_type = no_ref(to_t2);
                return 1;
            }
        }
    }

    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: In conditional expression conversion, "
                "no conversion is possible from '%s' to '%s'\n",
                print_declarator(from_t1),
                print_declarator(to_t2));
    }
    return 0;
}

static char ternary_operator_property(type_t* t1, type_t* t2, type_t* t3, const locus_t* locus UNUSED_PARAMETER)
{
    if (is_bool_type(no_ref(t1)))
    {
        if (is_arithmetic_type(no_ref(t2))
                && is_arithmetic_type(no_ref(t3)))
        {
            return 1;
        }
        else if (is_pointer_type(no_ref(t2))
                && is_pointer_type(no_ref(t3))
                && equivalent_types(
                    get_unqualified_type(no_ref(t2)), 
                    get_unqualified_type(no_ref(t3))))
        {
            return 1;
        }
        else if (is_pointer_to_member_type(no_ref(t2))
                && is_pointer_to_member_type(no_ref(t3))
                && equivalent_types(
                    get_unqualified_type(no_ref(t2)), 
                    get_unqualified_type(no_ref(t3))))
        {
            return 1;
        }
        else if (is_pointer_type(no_ref(t2)) != is_pointer_type(no_ref(t3))
                && is_zero_type_or_nullptr_type(no_ref(t2)) != is_zero_type_or_nullptr_type(no_ref(t3)))
        {
            return 1;
        }
        else if (is_pointer_to_member_type(no_ref(t2)) != is_pointer_to_member_type(no_ref(t3))
                && is_zero_type_or_nullptr_type(t2) != is_zero_type_or_nullptr_type(t3))
        {
            return 1;
        }
    }

    return 0;
}

static type_t* ternary_operator_result(type_t** t1 UNUSED_PARAMETER, 
        type_t** t2, type_t** t3,
        const locus_t* locus)
{
    if (is_arithmetic_type(no_ref(*t2))
            && is_arithmetic_type(no_ref(*t3)))
    {
        *t2 = no_ref(*t2);
        *t3 = no_ref(*t3);

        if (is_promoteable_integral_type(*t2))
            *t2 = promote_integral_type(*t2);

        if (is_promoteable_integral_type(*t3))
            *t3 = promote_integral_type(*t3);

        return usual_arithmetic_conversions(*t2, *t3, locus);
    }
    else
    {
        *t2 = no_ref(*t2);
        return (*t2);
    }
}

static type_t* composite_pointer_to_member(type_t* p1, type_t* p2, const locus_t* locus)
{
    if (equivalent_types(p1, p2))
        return p1;

    if (is_zero_type_or_nullptr_type(p1))
        return p2;

    if (is_zero_type_or_nullptr_type(p2))
        return p1;

    cv_qualifier_t cv_qualif_1 = CV_NONE;
    cv_qualifier_t cv_qualif_2 = CV_NONE;

    advance_over_typedefs_with_cv_qualif(p1, &cv_qualif_1);
    advance_over_typedefs_with_cv_qualif(p2, &cv_qualif_2);

    cv_qualifier_t result_cv = cv_qualif_1 | cv_qualif_2;

    type_t* result = NULL;

    standard_conversion_t sc;
    // This is not exact
    if (standard_conversion_between_types(&sc, p1, p2, locus))
    {
        result = p2;
    }
    else if (standard_conversion_between_types(&sc, p2, p1, locus))
    {
        result = p1;
    }
    else
    {
        internal_error("Unreachable code", 0);
    }

    result = get_cv_qualified_type(result, result_cv);

    return result;
}

static type_t* composite_pointer(type_t* p1, type_t* p2, const locus_t* locus)
{
    p1 = get_unqualified_type(p1);
    p2 = get_unqualified_type(p2);

    if (equivalent_types(p1, p2))
        return p1;

    if (is_zero_type_or_nullptr_type(p1))
        return p2;

    if (is_zero_type_or_nullptr_type(p2))
        return p1;

    cv_qualifier_t cv_qualif_1 = CV_NONE;
    cv_qualifier_t cv_qualif_2 = CV_NONE;

    advance_over_typedefs_with_cv_qualif(p1, &cv_qualif_1);
    advance_over_typedefs_with_cv_qualif(p2, &cv_qualif_2);

    cv_qualifier_t result_cv = cv_qualif_1 | cv_qualif_2;

    type_t* result = NULL;

    standard_conversion_t sc;
    if (is_void_pointer_type(p1) 
            || is_void_pointer_type(p2))
    {
        result = get_pointer_type(get_void_type());
    }
    // This is not exact
    else if (standard_conversion_between_types(&sc, p1, p2, locus))
    {
        result = p2;
    }
    else if (standard_conversion_between_types(&sc, p2, p1, locus))
    {
        result = p1;
    }
    else
    {
        internal_error("Unreachable code", 0);
    }

    result = get_cv_qualified_type(result, result_cv);

    return result;
}

static void check_conditional_expression_impl_nodecl_c(nodecl_t first_op,
        nodecl_t second_op,
        nodecl_t third_op,
        decl_context_t decl_context UNUSED_PARAMETER,
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(first_op);

    if (nodecl_is_err_expr(first_op)
            || nodecl_is_err_expr(second_op)
            || nodecl_is_err_expr(third_op))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    type_t* first_type = nodecl_get_type(first_op);
    type_t* second_type = nodecl_get_type(second_op);
    type_t* third_type = nodecl_get_type(third_op);

    type_t* converted_type = NULL;
    if (!is_vector_type(no_ref(first_type)))
    {
        converted_type = get_signed_int_type();
    }
    else
    {
        converted_type = get_vector_type(get_signed_int_type(), vector_type_get_vector_size(no_ref(first_type)));
    }

    if (is_void_type(no_ref(second_type))
            || is_void_type(no_ref(third_type)))
    {
        /*
         * If either the the second or the third operand is a void type
         */
        /*
         * All lvalue-conversions are applied here
         */
        type_t* operand_types[] = { second_type, third_type };

        int i;
        for (i = 0; i < 2; i++)
        {
            operand_types[i] = no_ref(operand_types[i]);

            if (is_array_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(array_type_get_element_type(operand_types[i]));
            }
            if (is_function_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(operand_types[i]);
            }
        }

        unary_record_conversion_to_result(operand_types[0], &second_op);
        unary_record_conversion_to_result(operand_types[1], &third_op);

        type_t* final_type = get_void_type();

        *nodecl_output = nodecl_make_conditional_expression(
                first_op,
                second_op,
                third_op,
                final_type, locus);

        // Nothing else has to be done for 'void' types
        return;
    }

    standard_conversion_t sc;
    if (!standard_conversion_between_types(&sc, no_ref(first_type), no_ref(converted_type), locus))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(first_op);
        nodecl_free(second_op);
        nodecl_free(third_op);
        return;
    }

    type_t* final_type = NULL;
    if (equivalent_types(second_type, third_type)
            && is_lvalue_reference_type(second_type)
            && is_lvalue_reference_type(third_type))
    {
        // A conditional expression is never a lvalue in C99
        final_type = no_ref(second_type);
    }
    else
    {
        /*
         * Now apply lvalue conversions to both types
         */
        type_t* operand_types[] = { second_type, third_type };

        int i;
        for (i = 0; i < 2; i++)
        {
            operand_types[i] = no_ref(operand_types[i]);

            if (is_array_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(array_type_get_element_type(operand_types[i]));
            }
            else if (is_function_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(operand_types[i]);
            }
        }

        char is_pointer_and_zero =
            (is_pointer_type(operand_types[0]) && is_zero_type_or_nullptr_type(operand_types[1]))
            || (is_pointer_type(operand_types[1]) && is_zero_type_or_nullptr_type(operand_types[0]));

        if (equivalent_types(operand_types[0], operand_types[1]))
        {
            final_type = operand_types[0];
        }
        else if (both_operands_are_arithmetic(operand_types[0], operand_types[1], locus))
        {
            final_type = usual_arithmetic_conversions(operand_types[0], operand_types[1], locus);
        }
        else if (both_operands_are_vector_types(operand_types[0], operand_types[1]))
        {
            final_type = operand_types[0];
        }
        else if ((is_pointer_type(operand_types[0]) && is_pointer_type(operand_types[1]))
                || is_pointer_and_zero)
        {
            final_type = composite_pointer(operand_types[0], operand_types[1], locus);
        }
        else
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        unary_record_conversion_to_result(operand_types[0], &second_op);
        unary_record_conversion_to_result(operand_types[1], &third_op);
    }


    *nodecl_output = nodecl_make_conditional_expression(
            first_op,
            second_op,
            third_op,
            final_type, locus);
}

static void check_conditional_expression_impl_nodecl_cxx(nodecl_t first_op,
        nodecl_t second_op,
        nodecl_t third_op,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(first_op);

    if (nodecl_is_err_expr(first_op)
            || nodecl_is_err_expr(second_op)
            || nodecl_is_err_expr(third_op))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    if (nodecl_expr_is_type_dependent(first_op)
            || nodecl_expr_is_type_dependent(second_op)
            || nodecl_expr_is_type_dependent(third_op))
    {
        *nodecl_output = nodecl_make_conditional_expression(
                first_op,
                second_op,
                third_op,
                get_unknown_dependent_type(),
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    nodecl_t* nodecl_conditional[3] = {
        &first_op,
        &second_op,
        &third_op
    };

    {
        // Simplify unresolved overloads
        int i;
        for (i = 0; i < 3; i++)
        {
            type_t* current_type = nodecl_get_type(*nodecl_conditional[i]);

            if (is_unresolved_overloaded_type(current_type))
            {
                scope_entry_t* entry = unresolved_overloaded_type_simplify(
                        current_type,
                        decl_context,
                        locus);

                if (entry != NULL)
                {
                    if (!update_simplified_unresolved_overloaded_type(
                            entry,
                            decl_context,
                            locus,
                            nodecl_conditional[i]))
                    {
                        *nodecl_output = nodecl_make_err_expr(locus);
                        return;
                    }
                }
            }
        }
    }

    // type_t* first_type = nodecl_get_type(first_op);
    type_t* second_type = nodecl_get_type(second_op);
    type_t* third_type = nodecl_get_type(third_op);

    /*
     * C++ standard is a mess here but we will try to make it clear
     */
    if (is_void_type(no_ref(second_type))
            || is_void_type(no_ref(third_type)))
    {
        /*
         * If either the the second or the third operand is a void type
         */
        /*
         * All lvalue-conversions are applied here
         */
        type_t* operand_types[] = { second_type, third_type };

        int i;
        for (i = 0; i < 2; i++)
        {
            operand_types[i] = no_ref(operand_types[i]);

            if (is_array_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(array_type_get_element_type(operand_types[i]));
            }
            if (is_function_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(operand_types[i]);
            }
        }

        type_t* final_type = NULL;
        if ((is_throw_expr_type(operand_types[0])
                    || is_throw_expr_type(operand_types[1]))
                && !(is_throw_expr_type(operand_types[0])
                    && is_throw_expr_type(operand_types[1])))
        {
            /*
             *  a) If any (but not both) is a throw expression (throw expressions
             *     yield a type of void, a special one) the result is the type of the
             *     other and is a rvalue
             */
            if (is_throw_expr_type(operand_types[0]))
            {
                final_type = operand_types[1];
            }
            else
            {
                final_type = operand_types[0];
            }
        }
        else
        {
            /*
             * b) Both the second and third operands have type void the result is of type void
             * and is a rvalue
             */
            final_type = get_void_type();
        }

        *nodecl_output = nodecl_make_conditional_expression(
                *nodecl_conditional[0],
                *nodecl_conditional[1],
                *nodecl_conditional[2],
                final_type, locus);

        // Nothing else has to be done for 'void' types
        return;
    }

    if (!equivalent_types(no_ref(second_type), no_ref(third_type))
            && (is_class_type(no_ref(second_type))
                || is_class_type(no_ref(third_type))))
    {
        /*
         * otherwise, if the second or the third are different types and either is a class type
         * an attempt to convert one to the other is performed.
         */
        char second_to_third_is_ambig = 0;
        type_t* second_to_third_type = NULL;
        char second_to_third =
            convert_in_conditional_expr(second_type,
                    third_type,
                    &second_to_third_is_ambig,
                    &second_to_third_type,
                    decl_context,
                    locus);

        char third_to_second_is_ambig = 0;
        type_t* third_to_second_type = NULL;
        char third_to_second =
            convert_in_conditional_expr(third_type,
                    second_type,
                    &third_to_second_is_ambig,
                    &third_to_second_type,
                    decl_context,
                    locus);

        if (second_to_third
                && third_to_second)
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: In conditional expression, two-sided conversions are possible"
                        " when agreeing second and third types\n");
            }
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        if (second_to_third)
        {
            if (second_to_third_is_ambig)
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                return;
            }

            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: In conditional expression, converting second type '%s' to '%s'",
                        print_declarator(second_type),
                        print_declarator(second_to_third_type));
            }
            second_type = second_to_third_type;
        }

        if (third_to_second)
        {
            if (third_to_second_is_ambig)
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                return;
            }

            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: In conditional expression, converting third type '%s' to '%s'",
                        print_declarator(third_type),
                        print_declarator(third_to_second_type));
            }
            third_type = third_to_second_type;
        }
    }

    type_t* final_type = NULL;
    if (is_lvalue_reference_type(second_type)
            && is_lvalue_reference_type(third_type)
            && equivalent_types(
                get_unqualified_type(no_ref(second_type)),
                get_unqualified_type(no_ref(third_type))))
    {
        final_type = second_type;
    }
    else
    {
        /*
         * If the second and third operand do not have the same type
         * we rely in overload mechanism
         *
         * Note that 'operator?' cannot be overloaded, overloading mechanism
         * is used there to force a conversion
         */
        if (!equivalent_types(
                    get_unqualified_type(no_ref(second_type)),
                    get_unqualified_type(no_ref(third_type)))
                && (is_class_type(no_ref(second_type))
                    || is_class_type(no_ref(third_type))))
        {
            builtin_operators_set_t builtin_set;

            const char* operator_ternary = UNIQUESTR_LITERAL("operator ?");

            build_ternary_builtin_operators(get_bool_type(),
                    second_type,
                    third_type,
                    &builtin_set,
                    decl_context,
                    operator_ternary,
                    ternary_operator_property,
                    ternary_operator_result,
                    locus
                    );

            scope_entry_list_t* builtins =
                get_entry_list_from_builtin_operator_set(&builtin_set);

            int num_arguments = 3;

            type_t* argument_types[3] = {
                get_bool_type(),
                second_type,
                third_type,
            };

            candidate_t* candidate_set = NULL;
            scope_entry_list_iterator_t *it = NULL;
            for (it = entry_list_iterator_begin(builtins);
                    !entry_list_iterator_end(it);
                    entry_list_iterator_next(it))
            {
                candidate_set = candidate_set_add(candidate_set,
                        entry_list_iterator_current(it),
                        num_arguments,
                        argument_types);
            }
            entry_list_iterator_free(it);
            entry_list_free(builtins);

            scope_entry_t *orig_overloaded_call = solve_overload(candidate_set,
                    decl_context, locus);
            scope_entry_t* overloaded_call = entry_advance_aliases(orig_overloaded_call);

            if (overloaded_call == NULL)
            {
                error_message_overload_failed(candidate_set,
                        operator_ternary,
                        decl_context,
                        num_arguments,
                        argument_types,
                        /* implicit argument */ NULL,
                        locus);
                candidate_set_free(&candidate_set);
                *nodecl_output = nodecl_make_err_expr(locus);
                return;
            }
            candidate_set_free(&candidate_set);

            if (function_has_been_deleted(decl_context, overloaded_call, locus))
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                return;
            }

            // Get the converted types and use them instead of the originals
            second_type = function_type_get_parameter_type_num(overloaded_call->type_information, 1);
            third_type = function_type_get_parameter_type_num(overloaded_call->type_information, 2);
        }

        /*
         * Now apply lvalue conversions to both types
         */
        type_t* operand_types[] = { second_type, third_type };

        int i;
        for (i = 0; i < 2; i++)
        {
            operand_types[i] = no_ref(operand_types[i]);

            if (is_array_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(array_type_get_element_type(operand_types[i]));
            }
            else if (is_function_type(operand_types[i]))
            {
                operand_types[i] = get_pointer_type(operand_types[i]);
            }
        }

        char is_pointer_and_zero =
            (is_pointer_type(operand_types[0]) && is_zero_type_or_nullptr_type(operand_types[1]))
            || (is_pointer_type(operand_types[1]) && is_zero_type_or_nullptr_type(operand_types[0]));

        char is_pointer_to_member_and_zero =
            (is_pointer_to_member_type(operand_types[0]) && is_zero_type_or_nullptr_type(operand_types[1]))
            || (is_pointer_to_member_type(operand_types[1]) && is_zero_type_or_nullptr_type(operand_types[0]));

        if (equivalent_types(operand_types[0], operand_types[1]))
        {
            final_type = operand_types[1];
        }
        else if (both_operands_are_arithmetic(operand_types[0], operand_types[1], locus))
        {
            final_type = usual_arithmetic_conversions(operand_types[0], operand_types[1], locus);
        }
        else if (both_operands_are_vector_types(operand_types[0], operand_types[1]))
        {
            final_type = operand_types[0];
        }
        else if ((is_pointer_type(operand_types[0]) && is_pointer_type(operand_types[1]))
                || is_pointer_and_zero)
        {
            final_type = composite_pointer(operand_types[0], operand_types[1], locus);
        }
        else if ((is_pointer_to_member_type(operand_types[0])
                    && is_pointer_to_member_type(operand_types[1]))
                || is_pointer_to_member_and_zero)
        {
            final_type = composite_pointer_to_member(operand_types[0], operand_types[1], locus);
        }
        else
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

    }

    *nodecl_output = nodecl_make_conditional_expression(
            *nodecl_conditional[0],
            *nodecl_conditional[1],
            *nodecl_conditional[2],
            final_type, locus);
}

static void check_conditional_expression_impl_nodecl(nodecl_t first_op, 
        nodecl_t second_op, 
        nodecl_t third_op, 
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    C_LANGUAGE()
    {
        check_conditional_expression_impl_nodecl_c(first_op, 
                second_op,
                third_op,
                decl_context,
                nodecl_output);
    }
    CXX_LANGUAGE()
    {
        check_conditional_expression_impl_nodecl_cxx(first_op, 
                second_op,
                third_op,
                decl_context,
                nodecl_output);
    }

    if (nodecl_is_err_expr(*nodecl_output))
    {
        if (!nodecl_is_err_expr(first_op)
                && !nodecl_is_err_expr(second_op)
                && !nodecl_is_err_expr(third_op))
        {
            type_t* first_type = nodecl_get_type(first_op);
            type_t* second_type = nodecl_get_type(second_op);
            type_t* third_type = nodecl_get_type(third_op);
            C_LANGUAGE()
            {
                first_type = no_ref(first_type);
                second_type = no_ref(second_type);
                third_type = no_ref(third_type);
            }

            error_printf("%s: error: ternary operand '?' cannot be applied to first operand '%s' (of type '%s'), "
                    "second operand '%s' (of type '%s') and third operand '%s' (of type '%s')\n",
                    nodecl_locus_to_str(first_op),
                    codegen_to_str(first_op, nodecl_retrieve_context(first_op)), print_type_str(first_type, decl_context),
                    codegen_to_str(second_op, nodecl_retrieve_context(second_op)), print_type_str(second_type, decl_context),
                    codegen_to_str(third_op, nodecl_retrieve_context(third_op)), print_type_str(third_type, decl_context));
        }

        nodecl_free(first_op);
        nodecl_free(second_op);
        nodecl_free(third_op);
    }
    else
    {
        if (nodecl_is_constant(first_op))
        {
            if (const_value_is_nonzero(nodecl_get_constant(first_op)))
            {
                nodecl_set_constant(*nodecl_output, nodecl_get_constant(second_op));
            }
            else
            {
                nodecl_set_constant(*nodecl_output, nodecl_get_constant(third_op));
            }
        }

        if (nodecl_expr_is_value_dependent(first_op)
                || nodecl_expr_is_value_dependent(second_op)
                || nodecl_expr_is_value_dependent(third_op))
        {
            nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        }
    }
}

static void check_conditional_expression_impl(AST expression UNUSED_PARAMETER, 
        AST first_op, AST second_op, AST third_op, 
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    /*
     * This is more complex that it might seem at first ...
     */

    nodecl_t nodecl_first_op = nodecl_null();
    check_expression_impl_(first_op, decl_context, &nodecl_first_op);

    char do_not_call_constexpr = check_expr_flags.do_not_call_constexpr;
    // We do not attempt to evaluat the second expression if it is not constant
    // or (if it is constant) if it is zero
    check_expr_flags.do_not_call_constexpr = !nodecl_is_constant(nodecl_first_op)
        || const_value_is_zero(nodecl_get_constant(nodecl_first_op));

    nodecl_t nodecl_second_op = nodecl_null();
    check_expression_impl_(second_op, decl_context, &nodecl_second_op);

    // We do not attempt to evaluate the third expression if it is not constant
    // or (if it is constant) if it is nonzero
    check_expr_flags.do_not_call_constexpr = !nodecl_is_constant(nodecl_first_op)
        || const_value_is_nonzero(nodecl_get_constant(nodecl_first_op));

    nodecl_t nodecl_third_op = nodecl_null();
    check_expression_impl_(third_op, decl_context, &nodecl_third_op);

    check_expr_flags.do_not_call_constexpr = do_not_call_constexpr;

    check_conditional_expression_impl_nodecl(nodecl_first_op, nodecl_second_op, nodecl_third_op,
            decl_context, nodecl_output);
}


static void check_conditional_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST first_op = ASTSon0(expression);
    AST second_op = NULL, third_op = NULL;

    if (ASTType(expression) == AST_CONDITIONAL_EXPRESSION)
    {
        second_op = ASTSon1(expression);
        third_op = ASTSon2(expression);
    }
    else if (ASTType(expression) == AST_GCC_CONDITIONAL_EXPRESSION)
    {
        second_op = first_op;
        third_op = ASTSon1(expression);
    }
    else
    {
        internal_error("Invalid node '%s'\n", ast_print_node_type(ASTType(expression)));
    }

    check_conditional_expression_impl(expression, 
            first_op, second_op, third_op, decl_context, nodecl_output);
}

static void check_nodecl_initializer_clause(nodecl_t initializer_clause, 
        decl_context_t decl_context, 
        type_t* declared_type, 
        char disallow_narrowing,
        nodecl_t* nodecl_output);
static void check_nodecl_equal_initializer(nodecl_t equal_initializer, 
        decl_context_t decl_context, 
        type_t* declared_type, 
        nodecl_t* nodecl_output);
void check_nodecl_expr_initializer_in_argument(nodecl_t expr, 
        decl_context_t decl_context, 
        type_t* declared_type, 
        nodecl_t* nodecl_output);
void check_nodecl_braced_initializer(nodecl_t braced_initializer, 
        decl_context_t decl_context, 
        type_t* declared_type, 
        char is_explicit_type_cast,
        enum initialization_kind initialization_kind,
        nodecl_t* nodecl_output);
static void check_nodecl_parenthesized_initializer(nodecl_t direct_initializer, 
        decl_context_t decl_context, 
        type_t* declared_type, 
        char is_explicit,
        char is_explicit_type_cast,
        char emit_cast,
        nodecl_t* nodecl_output);

static void check_new_expression_impl(
        nodecl_t nodecl_placement_list, 
        nodecl_t nodecl_initializer, 
        type_t* new_type, 
        char is_global,
        decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (check_expr_flags.must_be_constant)
    {
        error_printf("%s: error: new-expression in constant-expression\n",
                locus_to_str(locus));
    }

    char is_new_array = is_array_type(new_type);

    type_t* arguments[MCXX_MAX_FUNCTION_CALL_ARGUMENTS];
    memset(arguments, 0, sizeof(arguments));

    char has_dependent_placement_args = 0;

    int num_placement_items = 0;
    if (!nodecl_is_null(nodecl_placement_list))
    {
        nodecl_t* nodecl_list = nodecl_unpack_list(nodecl_placement_list, &num_placement_items);

        int i;
        for (i = 0; i < num_placement_items; i++)
        {
            if (nodecl_expr_is_type_dependent(nodecl_list[i]))
            {
                has_dependent_placement_args = 1;
            }
            // 0 -> this
            // 1 -> size_t
            arguments[i + 2] = nodecl_get_type(nodecl_list[i]);
        }

        xfree(nodecl_list);
    }

    if (has_dependent_placement_args)
    {
        // Well, not all the whole new is type dependent but the placement arguments are
        nodecl_expr_set_is_type_dependent(nodecl_placement_list, 1);
    }

    if (is_dependent_type(new_type)
            || has_dependent_placement_args)
    {
        // The new type is dependent
        *nodecl_output = nodecl_make_cxx_dep_new(
                nodecl_initializer,
                nodecl_make_type(new_type, locus),
                nodecl_placement_list,
                new_type,
                is_global ? "global" : "",
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    nodecl_t nodecl_allocation_function = nodecl_null();
    nodecl_t nodecl_placement_list_out = nodecl_null();

    // At least the size_t parameter (+1 because we may need an implicit)
    int num_arguments = 2;
    // Note: arguments[0] will be left as NULL since 'operator new' is
    // always static if it is a member function
    arguments[1] = get_size_t_type();
    num_arguments += num_placement_items;

    decl_context_t op_new_context = decl_context;

    if (is_class_type(new_type)
            && !is_global)
    {
        // Instantiate the class if needed
        if (is_named_class_type(new_type))
        {
            scope_entry_t* symbol = named_type_get_symbol(new_type);
            class_type_complete_if_needed(symbol, decl_context, locus);
        }

        op_new_context = class_type_get_inner_context(new_type);
    }
    else
    {
        // Use the global scope
        op_new_context.current_scope = op_new_context.global_scope;
    }

    static AST operation_new_tree = NULL;
    if (operation_new_tree == NULL)
    {
        operation_new_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_NEW_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    static AST operation_new_array_tree = NULL;
    if (operation_new_array_tree == NULL)
    {
        operation_new_array_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_NEW_ARRAY_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    AST called_operation_new_tree = operation_new_tree;

    if (is_new_array)
    {
        called_operation_new_tree = operation_new_array_tree;
    }

    scope_entry_list_t *operator_new_list = query_id_expression(op_new_context, called_operation_new_tree, NULL);

    if (operator_new_list == NULL)
    {
        error_printf("%s: error: no suitable '%s' has been found in the scope\n",
                locus_to_str(locus),
                prettyprint_in_buffer(called_operation_new_tree));
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_placement_list);
        nodecl_free(nodecl_initializer);
        return;
    }

    candidate_t* candidate_set = NULL;
    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(operator_new_list);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* orig_entry = entry_list_iterator_current(it);
        scope_entry_t* entry = entry_advance_aliases(orig_entry);
        if (symbol_entity_specs_get_is_member(entry))
        {
            candidate_set = candidate_set_add(candidate_set,
                    orig_entry,
                    num_arguments,
                    arguments);
        }
        else
        {
            candidate_set = candidate_set_add(candidate_set,
                    orig_entry,
                    num_arguments - 1,
                    arguments + 1);
        }
    }
    entry_list_iterator_free(it);

    scope_entry_t* orig_chosen_operator_new = solve_overload(candidate_set, 
            decl_context, locus);
    scope_entry_t* chosen_operator_new = entry_advance_aliases(orig_chosen_operator_new);
    candidate_set_free(&candidate_set);

    if (chosen_operator_new == NULL)
    {
        // Format a nice message
        const char* argument_call = UNIQUESTR_LITERAL("");

        argument_call = strappend(argument_call, "operator new");
        if (is_new_array)
        {
            argument_call = strappend(argument_call, "[]");
        }
        argument_call = strappend(argument_call, "(");

        int i;
        for (i = 1; i < num_arguments; i++)
        {
            argument_call = strappend(argument_call, print_type_str(arguments[i], decl_context));
            if ((i + 1) < num_arguments)
            {
                argument_call = strappend(argument_call, ", ");
            }
        }
        argument_call = strappend(argument_call, ")");

        const char* message = NULL;
        uniquestr_sprintf(&message, "%s: error: no suitable '%s' found for new-expression\n",
                locus_to_str(locus),
                argument_call);

        diagnostic_candidates(operator_new_list, &message, locus);
        entry_list_free(operator_new_list);

        error_printf("%s", message);

        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_placement_list);
        nodecl_free(nodecl_initializer);
        return;
    }

    if (function_has_been_deleted(decl_context, chosen_operator_new, locus))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_placement_list);
        nodecl_free(nodecl_initializer);
        return;
    }

    // Store conversions
    if (!nodecl_is_null(nodecl_placement_list))
    {
        int num_items = 0, i;
        nodecl_t* list = nodecl_unpack_list(nodecl_placement_list, &num_items);

        for (i = 0; i < num_items; i++)
        {
            nodecl_t nodecl_expr = list[i];

            type_t* param_type = function_type_get_parameter_type_num(chosen_operator_new->type_information, 
                    /* Because the first is always size_t */
                    i + 1);

            nodecl_t old_nodecl_expr = nodecl_expr;
            check_nodecl_function_argument_initialization(nodecl_expr, decl_context, param_type,
                    /* disallow_narrowing */ 0,
                    &nodecl_expr);
            if (nodecl_is_err_expr(nodecl_expr))
            {
                nodecl_free(old_nodecl_expr);
                *nodecl_output = nodecl_expr;
                return;
            }

            nodecl_placement_list_out = nodecl_append_to_list(nodecl_placement_list_out,
                    nodecl_expr);
        }

        xfree(list);
    }

    nodecl_allocation_function = nodecl_make_symbol(chosen_operator_new, locus);

    nodecl_t nodecl_init_out = nodecl_null();

    // Verify the initializer
    check_nodecl_initialization(
            nodecl_initializer,
            decl_context,
            /*  initialized_entry */ NULL,
            new_type,
            &nodecl_init_out,
            /* is_auto */ 0,
            /* is_decltype_auto */ 0);

    type_t* synthesized_type = new_type;

    if (is_array_type(new_type))
    {
        synthesized_type = get_pointer_type(array_type_get_element_type(new_type));
    }
    else
    {
        synthesized_type = get_pointer_type(new_type);
    }

    nodecl_t nodecl_new = nodecl_make_new(
            nodecl_init_out,
            nodecl_make_type(new_type, locus),
            nodecl_placement_list_out,
            nodecl_allocation_function,
            synthesized_type,
            is_global ? "global" : "",
            locus);

    *nodecl_output = nodecl_new;
}

static void check_new_expression(AST new_expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    const locus_t* locus = ast_get_locus(new_expr);

    AST global_op = ASTSon0(new_expr);
    AST new_placement = ASTSon1(new_expr);
    AST new_type_id = ASTSon2(new_expr);
    AST new_initializer = ASTSon3(new_expr);

    nodecl_t nodecl_placement = nodecl_null();

    if (new_placement != NULL)
    {
        AST expression_list = ASTSon0(new_placement);

        if (!check_list_of_expressions(expression_list, decl_context, &nodecl_placement))
        {
            nodecl_free(nodecl_placement);

            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }
    }

    AST type_specifier_seq = ASTSon0(new_type_id);
    AST new_declarator = ASTSon1(new_type_id);

    type_t* dummy_type = NULL;
    gather_decl_spec_t gather_info;
    memset(&gather_info, 0, sizeof(gather_info));

    gather_info.is_cxx_new_declarator = 1;

    nodecl_t dummy_nodecl_output = nodecl_null();
    build_scope_decl_specifier_seq(type_specifier_seq, &gather_info, &dummy_type,
            decl_context, &dummy_nodecl_output);

    if (is_error_type(dummy_type))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_placement);
        return;
    }

    type_t* declarator_type = NULL;
    compute_declarator_type(new_declarator, &gather_info, dummy_type,
            &declarator_type, decl_context, &dummy_nodecl_output);

    nodecl_t nodecl_initializer = nodecl_null();
    if (new_initializer != NULL)
    {
        compute_nodecl_initialization(new_initializer, decl_context,
                /* preserve_top_level_parentheses */ gather_info.is_decltype_auto,
                &nodecl_initializer);
    }
    else
    {
        nodecl_initializer = nodecl_make_cxx_parenthesized_initializer(nodecl_initializer, locus);
    }

    check_new_expression_impl(nodecl_placement,
            nodecl_initializer,
            declarator_type,
            /* is_global */ global_op != NULL,
            decl_context,
            locus,
            nodecl_output);
}

static void check_new_type_id_expr(AST new_expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    check_new_expression(new_expr, decl_context, nodecl_output);
}

UNUSED_PARAMETER static char is_deallocation_function(scope_entry_t* entry)
{
    if (entry->kind != SK_FUNCTION)
        return 0;

    type_t* function_type = entry->type_information;

    if (function_type_get_num_parameters(function_type) == 0
             || function_type_get_num_parameters(function_type) > 2)
        return 0;

    // Only deallocation for classes may have 2 parameters
    if (function_type_get_num_parameters(function_type) == 2
            && !symbol_entity_specs_get_is_member(entry))
        return 0;

    type_t* void_pointer = function_type_get_parameter_type_num(function_type, 0);

    if (!equivalent_types(void_pointer, get_pointer_type(get_void_type())))
        return 0;

    if (function_type_get_num_parameters(function_type) == 2)
    {
        type_t* size_t_type = function_type_get_parameter_type_num(function_type, 1);
        if (!equivalent_types(size_t_type, get_size_t_type()))
            return 0;
    }

    if (is_template_specialized_type(function_type))
        return 0;

    return 1;
}

static void check_delete_expression_nodecl(nodecl_t nodecl_deleted_expr,
        decl_context_t decl_context UNUSED_PARAMETER,
        const locus_t* locus,
        char is_array_delete,
        nodecl_t* nodecl_output)
{
    if (check_expr_flags.must_be_constant)
    {
        error_printf("%s: error: delete-expression in constant-expression\n",
                locus_to_str(locus));
    }

    // FIXME - We are not calling the deallocation function
    type_t* deleted_type = no_ref(nodecl_get_type(nodecl_deleted_expr));

    if (!is_dependent_type(deleted_type))
    {
        if (!is_pointer_type(deleted_type)
                || is_pointer_to_function_type(deleted_type)
                || is_pointer_to_member_type(deleted_type))
        {
            error_printf("%s: error: invalid type '%s' for delete%s expression\n",
                    locus_to_str(locus),
                    is_array_delete ? "[]" : "",
                    print_type_str(deleted_type, decl_context));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_deleted_expr);
            return;
        }

        type_t* full_type = pointer_type_get_pointee_type(deleted_type);

        if (is_named_class_type(no_ref(full_type)))
        {
            scope_entry_t* symbol = named_type_get_symbol(no_ref(full_type));
            class_type_complete_if_possible(symbol, decl_context, locus);
        }

        if (!is_complete_type(full_type))
        {
            error_printf("%s: error: invalid incomplete type '%s' in delete%s expression\n",
                    locus_to_str(locus),
                    print_type_str(full_type, decl_context),
                    is_array_delete ? "[]" : "");
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_deleted_expr);
            return;
        }
    }

    if (is_array_delete)
    {
        *nodecl_output = nodecl_make_delete_array(nodecl_deleted_expr, get_void_type(),
                locus);
    }
    else
    {
        *nodecl_output = nodecl_make_delete(nodecl_deleted_expr, get_void_type(),
                locus);
    }
}


static void check_delete_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    char is_array_delete = 0;
    if (ASTType(expression) == AST_DELETE_ARRAY_EXPR)
    {
        is_array_delete = 1;
    }

    AST deleted_expression = ASTSon1(expression);

    nodecl_t nodecl_deleted_expr = nodecl_null();
    check_expression_impl_(deleted_expression, decl_context, &nodecl_deleted_expr);

    if (nodecl_is_err_expr(nodecl_deleted_expr))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        return;
    }

    check_delete_expression_nodecl(nodecl_deleted_expr, decl_context, 
            ast_get_locus(expression),
            is_array_delete,
            nodecl_output);
}

static const_value_t* cxx_nodecl_make_value_conversion(
        type_t* dest_type,
        type_t* orig_type,
        const_value_t* val,
        char is_explicit_cast,
        const locus_t* locus)
{
    ERROR_CONDITION(is_dependent_type(orig_type),
            "Do not call this function to convert from dependent types", 0);
    ERROR_CONDITION(is_dependent_type(dest_type),
            "Do not call this function to convert to dependent types", 0);

    if (val == NULL)
        return NULL;

    standard_conversion_t scs;
    char there_is_a_scs = standard_conversion_between_types(
            &scs,
            get_unqualified_type(no_ref(orig_type)),
            get_unqualified_type(no_ref(dest_type)),
            locus);

    // Try again with enum types
    if (!there_is_a_scs
            && (is_enum_type(no_ref(orig_type))
            || is_enum_type(no_ref(dest_type))))
    {
        type_t* underlying_orig_type = get_unqualified_type(no_ref(orig_type));
        if (is_enum_type(underlying_orig_type))
            underlying_orig_type = enum_type_get_underlying_type(underlying_orig_type);

        type_t* underlying_dest_type = get_unqualified_type(no_ref(dest_type));
        if (is_enum_type(underlying_dest_type))
            underlying_dest_type = enum_type_get_underlying_type(underlying_dest_type);

        there_is_a_scs = standard_conversion_between_types(
                &scs,
                underlying_orig_type,
                underlying_dest_type,
                locus);
    }

    if (!there_is_a_scs)
        return NULL;

    // The conversions involving values are in scs.conv[1]
    switch (scs.conv[1])
    {
        case SCI_NO_CONVERSION:
            // We can fall here for cases like const int -> int
            break;
        case SCI_FLOATING_PROMOTION:
        case SCI_FLOATING_CONVERSION:
        case SCI_INTEGRAL_FLOATING_CONVERSION:
            val = const_value_cast_to_floating_type_value(
                    val,
                    get_unqualified_type(no_ref(dest_type)));
            break;
        case SCI_INTEGRAL_PROMOTION:
        case SCI_INTEGRAL_CONVERSION:
        case SCI_FLOATING_INTEGRAL_CONVERSION:
            val = const_value_cast_to_bytes(
                    val,
                    type_get_size(get_unqualified_type(no_ref(dest_type))),
                    is_signed_integral_type(get_unqualified_type(no_ref(dest_type))));
            break;
        case SCI_BOOLEAN_CONVERSION:
            val = const_value_get_integer(
                    const_value_is_nonzero(val),
                    type_get_size(get_bool_type()),
                    /* signed */ 1);
            break;
        case SCI_ZERO_TO_POINTER_CONVERSION:
        case SCI_NULLPTR_TO_POINTER_CONVERSION:
        case SCI_ZERO_TO_NULLPTR:
            val = const_value_get_zero(
                    type_get_size(get_unqualified_type(no_ref(dest_type))),
                    /* sign */ 0);
            break;
        case SCI_COMPLEX_TO_FLOAT_CONVERSION:
            val = const_value_cast_to_floating_type_value(
                    const_value_complex_get_real_part(val),
                    get_unqualified_type(no_ref(dest_type)));
            break;
        case SCI_COMPLEX_TO_INTEGRAL_CONVERSION:
            val = const_value_cast_to_bytes(
                    const_value_complex_get_real_part(val),
                    type_get_size(get_unqualified_type(no_ref(dest_type))),
                    is_signed_integral_type(get_unqualified_type(no_ref(dest_type))));
            break;
        case SCI_INTEGRAL_TO_COMPLEX_CONVERSION:
        case SCI_FLOAT_TO_COMPLEX_CONVERSION:
        case SCI_FLOAT_TO_COMPLEX_PROMOTION:
            val = const_value_make_complex(
                    // cast real part (might do a no-op)
                    const_value_cast_to_floating_type_value(
                        val,
                        complex_type_get_base_type(get_unqualified_type(no_ref(dest_type)))),
                    // imag part is set to zero
                    const_value_cast_to_floating_type_value(
                        const_value_get_signed_int(0),
                        complex_type_get_base_type(get_unqualified_type(no_ref(dest_type)))));
            break;
        case SCI_INTEGRAL_TO_POINTER_CONVERSION:
            val = const_value_cast_to_bytes(
                    val,
                    type_get_size(get_unqualified_type(no_ref(dest_type))),
                    /* sign */ 0);
            break;
        case SCI_COMPLEX_PROMOTION:
        case SCI_COMPLEX_CONVERSION:
            val = const_value_make_complex(
                    const_value_cast_to_floating_type_value(
                        const_value_complex_get_real_part(val),
                        complex_type_get_base_type(get_unqualified_type(no_ref(dest_type)))),
                    const_value_cast_to_floating_type_value(
                        const_value_complex_get_imag_part(val),
                        complex_type_get_base_type(get_unqualified_type(no_ref(dest_type)))));
            break;
        case SCI_POINTER_TO_INTEGRAL_CONVERSION:
        case SCI_POINTER_TO_VOID_CONVERSION:
        case SCI_VOID_TO_POINTER_CONVERSION:
        case SCI_POINTER_TO_MEMBER_BASE_TO_DERIVED_CONVERSION:
        case SCI_CLASS_POINTER_DERIVED_TO_BASE_CONVERSION:
            // Leave these untouched
            break;
        default:
            internal_error("Do not know how to handle conversion '%s'\n",
                    sci_conversion_to_str(scs.conv[1]));
    }

    if (there_is_a_scs && !is_explicit_cast)
    {
        // Emit a warning for some common cases
        if (scs.conv[1] == SCI_INTEGRAL_TO_POINTER_CONVERSION)
        {
            warn_printf("%s: warning: conversion from integer type to pointer type\n",
                    locus_to_str(locus));
        }
        else if (scs.conv[2] == SCI_POINTER_TO_INTEGRAL_CONVERSION)
        {
            warn_printf("%s: warning: conversion from pointer type to integer\n",
                    locus_to_str(locus));
        }
    }

    return val;
}

static void compute_fake_types_cast_away_constness(type_t** fake_orig_type, type_t** fake_dest_type,
        type_t* orig_type,
        type_t* dest_type)
{
    if ((is_pointer_type(orig_type)
                && is_pointer_type(dest_type))
            || (is_pointer_to_member_type(orig_type)
                && is_pointer_to_member_type(dest_type)))
    {
        compute_fake_types_cast_away_constness(
                fake_orig_type,
                fake_dest_type,
                pointer_type_get_pointee_type(orig_type),
                pointer_type_get_pointee_type(dest_type));

        *fake_orig_type = get_pointer_type(*fake_orig_type);
        *fake_dest_type = get_pointer_type(*fake_dest_type);
    }
    else
    {
        *fake_orig_type = get_signed_int_type();
        *fake_dest_type = get_signed_int_type();
    }

    *fake_orig_type = get_cv_qualified_type(*fake_orig_type, get_cv_qualifier(orig_type));
    *fake_dest_type = get_cv_qualified_type(*fake_dest_type, get_cv_qualifier(dest_type));
}

static char conversion_casts_away_constness(type_t* orig_type, type_t* dest_type,
        const locus_t* locus)
{
    if ((is_pointer_type(orig_type)
                && is_pointer_type(dest_type))
            || (is_pointer_to_member_type(orig_type)
                && is_pointer_to_member_type(dest_type)))
    {
        type_t* fake_orig_type = NULL;
        type_t* fake_dest_type = NULL;

        compute_fake_types_cast_away_constness(
                &fake_orig_type,
                &fake_dest_type,
                orig_type,
                dest_type);

        standard_conversion_t scs;
        if (!standard_conversion_between_types(&scs, fake_orig_type, fake_dest_type, locus))
            return 1;
    }
    else if (is_lvalue_reference_type(orig_type)
            && is_lvalue_reference_type(dest_type))
    {
        return conversion_casts_away_constness(
                get_pointer_type(no_ref(orig_type)),
                get_pointer_type(no_ref(dest_type)),
                locus);
    }
    else if (is_rvalue_reference_type(orig_type)
            && !is_any_reference_type(dest_type))
    {
        return conversion_casts_away_constness(
                get_pointer_type(no_ref(orig_type)),
                get_pointer_type(no_ref(dest_type)),
                locus);
    }

    return 0;
}

#define RECURSION_PROTECTOR \
static int reentrancy_counter = 0; \
{ \
    reentrancy_counter++; \
    ERROR_CONDITION(reentrancy_counter > 256, "Too many recursive calls (%d), probably broken. Giving up\n", reentrancy_counter); \
}

#define RECURSION_RETURN(ret_) \
do { \
    reentrancy_counter--; \
    ERROR_CONDITION(reentrancy_counter < 0, "Underflow (%d) in recursion counter\n", reentrancy_counter); \
    return ret_; \
} while (0)

static char conversion_is_valid_static_cast(
        nodecl_t *nodecl_expression,
        type_t* dest_type,
        decl_context_t decl_context)
{
    RECURSION_PROTECTOR
#define RETURN(ret_) RECURSION_RETURN(ret_)

    const locus_t* locus = nodecl_get_locus(*nodecl_expression);

    type_t* orig_type = nodecl_get_type(*nodecl_expression);

    // A lvalue of type cv1 B can be cast to reference to cv2 D. B = base, D = derived
    // cv2 >= cv1
    if (is_lvalue_reference_to_class_type(orig_type)
            && (is_lvalue_reference_to_class_type(dest_type)
                || is_rvalue_reference_to_class_type(dest_type))
            && class_type_is_base_instantiating(no_ref(orig_type), no_ref(dest_type), locus)
            && !class_type_is_ambiguous_base_of_derived_class(no_ref(orig_type), no_ref(dest_type))
            && !class_type_is_virtual_base_or_base_of_virtual_base(no_ref(orig_type), no_ref(dest_type))
            && is_more_or_equal_cv_qualified_type(
                no_ref(dest_type),
                no_ref(orig_type)))
        RETURN(1);

    // A value of type cv1 B can be cast to a rvalue reference cv2 D
    // cv2 >= cv1
    if (is_class_type(orig_type)
            && is_rvalue_reference_to_class_type(dest_type)
            && class_type_is_base_instantiating(no_ref(orig_type), no_ref(dest_type), locus)
            && !class_type_is_ambiguous_base_of_derived_class(no_ref(orig_type), no_ref(dest_type))
            && !class_type_is_virtual_base_or_base_of_virtual_base(no_ref(orig_type), no_ref(dest_type))
            && is_more_or_equal_cv_qualified_type(
                no_ref(dest_type),
                no_ref(orig_type)))
        RETURN(1);

    // A lvalue of type cv1 T1 can be cast to type rvalue reference to cv2 T2 if cv2 T2 is reference
    // compatible with cv1 T1
    if (is_lvalue_reference_type(orig_type)
            && is_rvalue_reference_type(dest_type)
            && type_is_reference_compatible_to(
                // cv2 T2
                get_cv_qualified_type(no_ref(dest_type), get_cv_qualifier(no_ref(dest_type))),
                // cv1 T1
                get_cv_qualified_type(no_ref(orig_type), get_cv_qualifier(no_ref(orig_type)))))
        RETURN(1);

    // Otherwise an expression can be explicitly converted to a type T using a
    // static_cast<T>(e) if the declaration T t(e) is well formed for some
    // invented variable t
    nodecl_t nodecl_parenthesized_init =
        nodecl_make_cxx_parenthesized_initializer(
                nodecl_make_list_1(nodecl_shallow_copy(*nodecl_expression)),
                nodecl_get_locus(*nodecl_expression));
    nodecl_t nodecl_static_cast_output = nodecl_null();

    diagnostic_context_push_buffered();
    check_nodecl_parenthesized_initializer(
            nodecl_parenthesized_init,
            decl_context,
            dest_type,
            /* is_explicit */ 1,
            /* is_explicit_type_cast */ 1,
            /* emit_cast */ 0,
            &nodecl_static_cast_output);
    diagnostic_context_pop_and_discard();

    if (!nodecl_is_err_expr(nodecl_static_cast_output))
    {
        nodecl_free(*nodecl_expression);
        *nodecl_expression = nodecl_static_cast_output;
        RETURN(1);
    }
    else
    {
        nodecl_free(nodecl_static_cast_output);
    }

    // Any expression can be explicitly converted to cv void
    if (is_void_type(dest_type))
        RETURN(1);

    // The inverse of any standard conversion sequence not containing
    // lvalue-to-rvalue, array-to-pointer, function-to-pointer, null pointer,
    // null member pointer, boolean conversion
    standard_conversion_t scs;
    char there_is_scs = standard_conversion_between_types(&scs, dest_type, orig_type, locus);

    if (there_is_scs
            && scs.conv[0] != SCI_LVALUE_TO_RVALUE
            && scs.conv[0] != SCI_ARRAY_TO_POINTER
            && scs.conv[0] != SCI_FUNCTION_TO_POINTER

            && scs.conv[1] != SCI_NULLPTR_TO_POINTER_CONVERSION
            && scs.conv[1] != SCI_ZERO_TO_POINTER_CONVERSION
            && scs.conv[1] != SCI_BOOLEAN_CONVERSION)
        RETURN(1);

    // Apply lvalue conversions
    type_t* before_lvalue_conv = orig_type;
    orig_type = no_ref(orig_type);
    if (is_array_type(orig_type))
        orig_type = get_pointer_type(array_type_get_element_type(orig_type));
    else if (is_function_type(orig_type))
        orig_type = get_pointer_type(orig_type);

    if (!equivalent_types(before_lvalue_conv, orig_type))
    {
        unary_record_conversion_to_result(orig_type, nodecl_expression);
    }

    // A scoped enum can be converted to an integral type
    if (is_scoped_enum_type(orig_type)
            && is_integral_type(dest_type))
        RETURN(1);

    // An enum or integral can be converted to enum
    if ((is_enum_type(orig_type)
                || is_integral_type(orig_type))
            && is_enum_type(dest_type))
        RETURN(1);

    // A base pointer can be converted to derived type
    if (is_pointer_to_class_type(orig_type)
            && is_pointer_to_class_type(dest_type)
            && class_type_is_base_instantiating(
                pointer_type_get_pointee_type(orig_type),
                pointer_type_get_pointee_type(dest_type),
                locus)
            && !class_type_is_ambiguous_base_of_derived_class(
                pointer_type_get_pointee_type(orig_type),
                pointer_type_get_pointee_type(dest_type))
            && !class_type_is_virtual_base_or_base_of_virtual_base(
                pointer_type_get_pointee_type(orig_type),
                pointer_type_get_pointee_type(dest_type))
            && is_more_or_equal_cv_qualified_type(
                pointer_type_get_pointee_type(dest_type),
                pointer_type_get_pointee_type(orig_type)))
        RETURN(1);

    // A pointer to member can be converted to a base pointer if the opposite
    // conversion exists
    // cv1 T D::* -> cv2 T B::*
    if (is_pointer_to_member_type(orig_type)
            && is_pointer_to_member_type(dest_type)
            && class_type_is_base_instantiating(
                // B
                pointer_to_member_type_get_class_type(dest_type),
                // D
                pointer_to_member_type_get_class_type(orig_type),
                locus)
            //  T B::* -> T D::*
            && standard_conversion_between_types(&scs, 
                get_unqualified_type(dest_type),
                get_unqualified_type(orig_type),
                locus)
            // cv2 >= cv1
            && is_more_or_equal_cv_qualified_type(
                pointer_type_get_pointee_type(dest_type),
                pointer_type_get_pointee_type(orig_type)))
        RETURN(1);

    // cv1 void* -> cv2 T*  where cv2 >= cv1
    if (is_pointer_to_void_type(orig_type)
            && is_pointer_type(dest_type)
            && is_more_or_equal_cv_qualified_type(
                pointer_type_get_pointee_type(dest_type),
                pointer_type_get_pointee_type(orig_type)))
        RETURN(1);

    // No conversion was possible using static_cast
    RETURN(0);
#undef RETURN
}

static char conversion_is_valid_reinterpret_cast(
        nodecl_t *nodecl_expression,
        type_t* dest_type,
        decl_context_t decl_context)
{
    RECURSION_PROTECTOR
#define RETURN(ret_) RECURSION_RETURN(ret_)

    type_t* orig_type = nodecl_get_type(*nodecl_expression);

    // No way we can reinterpret a template function
    if (is_unresolved_overloaded_type(orig_type))
        return 0;

    if (!is_any_reference_type(dest_type))
    {
        // Apply lvalue conversions
        orig_type = no_ref(orig_type);
        if (is_array_type(orig_type))
            orig_type = get_pointer_type(array_type_get_element_type(orig_type));
        else if (is_function_type(orig_type))
            orig_type = get_pointer_type(orig_type);
        unary_record_conversion_to_result(orig_type, nodecl_expression);
    }

    // Any integral, enumeration, pointer, pointer ot member can be explicitly
    // converted to its own type
    if ((is_integral_type(orig_type)
                || is_enum_type(orig_type)
                || is_pointer_type(orig_type)
                || is_pointer_to_member_type(orig_type)
                // extension: allow vector to convert to themselves...
                || is_vector_type(orig_type))
            && (equivalent_types(get_unqualified_type(orig_type),
                    get_unqualified_type(dest_type))))
        RETURN(1);

    // A pointer can be explicitly converted to any integral type
    if (is_pointer_type(orig_type)
            && is_integral_type(dest_type)
            && type_get_size(orig_type) == type_get_size(dest_type))
        RETURN(1);

    // A nullptr can be converted to integral
    if (is_nullptr_type(orig_type)
            && is_integral_type(dest_type))
        RETURN(1);

    // An integral or enum can be converted to pointer
    if ((is_integral_type(orig_type)
            || is_enum_type(orig_type))
            && is_pointer_type(dest_type))
        RETURN(1);

    // arithmetic to vector
    if (is_arithmetic_type(orig_type)
            && is_vector_type(dest_type))
        RETURN(1);

    // vector to arithmetic
    if (is_vector_type(orig_type)
            && is_arithmetic_type(dest_type))
        RETURN(1);

    // vector to vector
    if (is_vector_type(orig_type)
            && is_vector_type(dest_type))
        RETURN(1);

    // Intel vectors
    {
        int orig_size = 0, dest_size = 0;
        if (is_intel_vector_struct_type(orig_type, &orig_size)
                && is_intel_vector_struct_type(dest_type, &dest_size)
                && (dest_size == orig_size))
            RETURN(1);

        if (vector_type_to_intel_vector_struct_reinterpret_type(orig_type, dest_type))
            RETURN(1);

        if (vector_type_to_intel_vector_struct_reinterpret_type(dest_type, orig_type))
            RETURN(1);
    }

    // A pointer to function can be converted to another pointer to function type
    if (is_pointer_to_function_type(orig_type)
            && is_pointer_to_function_type(dest_type))
        RETURN(1);

    // A pointer can be converted to another pointer
    // NOTE: This includes conversion pointer to object <-> pointer to function
    if (is_pointer_type(orig_type)
            && is_pointer_type(dest_type))
        RETURN(1);

    // It is possible to reinterpret_cast among pointer to member types (of any
    // class) if both are to object or both to function (but not mixed)
    if (is_pointer_to_member_type(orig_type)
            && is_pointer_to_member_type(dest_type)
            && (is_function_type(pointer_type_get_pointee_type(orig_type))
                == is_function_type(pointer_type_get_pointee_type(dest_type))))
        RETURN(1);

    // reinterpret_cast<T&>(x) is valid if reinterpret_cast<T*>(&x) is valid
    // reinterpret_cast<T&&>(x) is valid if reinterpret_cast<T*>(&x) is valid
    nodecl_t n = nodecl_make_type(get_pointer_type(orig_type), NULL);
    if (is_any_reference_type(dest_type)
            && conversion_is_valid_reinterpret_cast(
                &n,
                get_pointer_type(dest_type),
                decl_context))
    {
        nodecl_free(n);
        RETURN(1);
    }
    nodecl_free(n);

    RETURN(0);

#undef RETURN
}

static char conversion_is_valid_dynamic_cast(
        nodecl_t *nodecl_expression,
        type_t* dest_type,
        decl_context_t decl_context UNUSED_PARAMETER)
{
    RECURSION_PROTECTOR
#define RETURN(ret_) RECURSION_RETURN(ret_)

    const locus_t* locus = nodecl_get_locus(*nodecl_expression);

    if (!is_pointer_to_void_type(dest_type)
            && !is_pointer_to_class_type(dest_type)
            && !is_class_type(no_ref(dest_type)))
        RETURN(0);

    type_t* orig_type = nodecl_get_type(*nodecl_expression);

    if (is_pointer_to_class_type(dest_type))
    {
        if (!is_pointer_to_class_type(no_ref(orig_type)))
            RETURN(0);
        if (is_incomplete_type(pointer_type_get_pointee_type(no_ref(orig_type))))
            RETURN(0);
    }
    else if (is_lvalue_reference_to_class_type(dest_type))
    {
        if (!is_lvalue_reference_to_class_type(orig_type))
            RETURN(0);
        if (is_incomplete_type(no_ref(orig_type)))
            RETURN(0);
    }
    else if (is_rvalue_reference_to_class_type(dest_type))
    {
        if (!is_class_type(orig_type))
            RETURN(0);
        if (is_incomplete_type(orig_type))
            RETURN(0);
    }

    if (equivalent_types(get_unqualified_type(orig_type), get_unqualified_type(dest_type))
            && is_more_or_equal_cv_qualified_type(dest_type, orig_type))
        RETURN(1);

    if (is_pointer_type(dest_type)
            && is_zero_type_or_nullptr_type(orig_type))
        RETURN(1);

    if (is_pointer_to_class_type(dest_type)
            && is_pointer_to_class_type(no_ref(orig_type)))
    {
        if (class_type_is_base_instantiating(
                    pointer_type_get_pointee_type(dest_type),
                    pointer_type_get_pointee_type(no_ref(orig_type)),
                    locus))
        {
            if (is_more_or_equal_cv_qualified_type(
                        pointer_type_get_pointee_type(dest_type),
                        pointer_type_get_pointee_type(no_ref(orig_type))))
                RETURN(1);
            else
                RETURN(0);
        }
    }
    else if (is_any_reference_to_class_type(dest_type)
            && is_any_reference_to_class_type(orig_type))
    {
        if (class_type_is_base_instantiating(
                    no_ref(dest_type),
                    no_ref(orig_type),
                    locus))
        {
            if (is_more_or_equal_cv_qualified_type(
                        no_ref(dest_type),
                        no_ref(orig_type)))
                RETURN(1);
            else
                RETURN(0);
        }
    }

    if (!is_lvalue_reference_to_class_type(orig_type)
            && !is_pointer_to_class_type(no_ref(orig_type)))
        RETURN(0);

    if (is_lvalue_reference_to_class_type(orig_type)
            && !class_type_is_polymorphic(no_ref(orig_type)))
        RETURN(0);

    if (is_pointer_to_class_type(orig_type)
            && !class_type_is_polymorphic(pointer_type_get_pointee_type(orig_type)))
        RETURN(0);

    if (is_pointer_to_void_type(dest_type))
        RETURN(1);

    // Here we would perform a runtime check
    RETURN(1);

#undef RETURN
}

static char same_level_pointer(type_t* t1, type_t* t2)
{
    ERROR_CONDITION(!is_pointer_type(t1)
            || !is_pointer_type(t2), "Invalid types", 0);
    while (is_pointer_type(t1)
            && is_pointer_type(t2))
    {
        t1 = pointer_type_get_pointee_type(t1);
        t2 = pointer_type_get_pointee_type(t2);
    }

    if (is_pointer_type(t1)
            || is_pointer_type(t2))
        return 0;

    return 1;
}

static char same_level_pointer_to_member(type_t* t1, type_t* t2)
{
    ERROR_CONDITION(!is_pointer_to_member_type(t1)
            || !is_pointer_to_member_type(t2), "Invalid types", 0);
    while (is_pointer_to_member_type(t1)
            && is_pointer_to_member_type(t2))
    {
        t1 = pointer_type_get_pointee_type(t1);
        t2 = pointer_type_get_pointee_type(t2);
    }

    if (is_pointer_to_member_type(t1)
            || is_pointer_to_member_type(t2))
        return 0;

    return 1;
}

static char conversion_is_valid_const_cast(
        nodecl_t *nodecl_expression,
        type_t* dest_type,
        decl_context_t decl_context)
{
    RECURSION_PROTECTOR
#define RETURN(ret_) RECURSION_RETURN(ret_)

    type_t* orig_type = nodecl_get_type(*nodecl_expression);

    if (is_unresolved_overloaded_type(orig_type))
        RETURN(0);

    if (!is_any_reference_type(dest_type))
    {
        // Apply lvalue conversions
        orig_type = no_ref(orig_type);
        if (is_array_type(orig_type))
            orig_type = get_pointer_type(array_type_get_element_type(orig_type));
        else if (is_function_type(orig_type))
            orig_type = get_pointer_type(orig_type);
        unary_record_conversion_to_result(orig_type, nodecl_expression);
    }

    // Conversion between pointers
    if (is_pointer_type(no_ref(orig_type))
            && is_pointer_type(no_ref(dest_type))
            && same_level_pointer(no_ref(orig_type), no_ref(dest_type)))
        RETURN(1);

    if (is_pointer_to_member_type(no_ref(orig_type))
            && is_pointer_to_member_type(no_ref(dest_type))
            && same_level_pointer_to_member(no_ref(orig_type), no_ref(dest_type)))
        RETURN(1);

    // If it is an object type, check a conversion through pointers
    nodecl_t n =
                nodecl_make_type(get_pointer_type(no_ref(orig_type)),
                        nodecl_get_locus(*nodecl_expression));
    if (!is_pointer_type(no_ref(orig_type))
            && !is_pointer_type(no_ref(dest_type))
            && conversion_is_valid_const_cast(
                &n,
                get_pointer_type(no_ref(dest_type)),
                decl_context))
    {
        nodecl_free(n);
        // T1& -> T2&
        if (is_lvalue_reference_type(orig_type)
                && is_lvalue_reference_type(dest_type))
            RETURN(1);

        // T1& -> T2&&
        // T1&& -> T2&&
        if ((is_lvalue_reference_type(orig_type)
                || is_rvalue_reference_type(orig_type))
                && is_rvalue_reference_type(dest_type))
            RETURN(1);

        // C -> D&&
        if (is_class_type(orig_type)
                && is_rvalue_reference_type(dest_type))
            RETURN(1);
    }
    else
    {
        nodecl_free(n);
    }

    RETURN(0);

#undef RETURN
}

static void check_nodecl_cast_expr(
        nodecl_t nodecl_casted_expr,
        decl_context_t decl_context,
        type_t* declarator_type,
        const char* cast_kind,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (is_dependent_type(declarator_type))
    {
        *nodecl_output = nodecl_make_cast(
                nodecl_casted_expr,
                declarator_type,
                cast_kind,
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_casted_expr))
    {
        *nodecl_output = nodecl_make_cast(
                nodecl_casted_expr,
                declarator_type,
                cast_kind,
                locus);
        nodecl_expr_set_is_value_dependent(*nodecl_output,
                nodecl_expr_is_value_dependent(nodecl_casted_expr));
        return;
    }

    if (is_unresolved_overloaded_type(nodecl_get_type(nodecl_casted_expr)))
    {
        // Simplify this unresolved overloaded type
        scope_entry_t* entry = unresolved_overloaded_type_simplify(
                nodecl_get_type(nodecl_casted_expr),
                decl_context, nodecl_get_locus(nodecl_casted_expr));
        if (entry != NULL)
        {
            if (!update_simplified_unresolved_overloaded_type(
                        entry,
                        decl_context,
                        nodecl_get_locus(nodecl_casted_expr),
                        &nodecl_casted_expr))
            {
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_casted_expr));
                nodecl_free(nodecl_casted_expr);
                return;
            }
        }
    }

    if (!is_any_reference_type(declarator_type)
            && !is_array_type(declarator_type) // This should never happen
            && !is_class_type(declarator_type))
    {
        declarator_type = get_unqualified_type(declarator_type);
    }

#define CONVERSION_ERROR \
    do { \
      const char* message = NULL; \
      uniquestr_sprintf(&message, "%s: error: expression '%s' of type '%s' cannot be converted to '%s' using a %s\n", \
              locus_to_str(locus), \
              codegen_to_str(nodecl_casted_expr, decl_context), \
              print_type_str(nodecl_get_type(nodecl_casted_expr), decl_context), \
              print_type_str(declarator_type, decl_context), \
              cast_kind); \
      if (is_unresolved_overloaded_type(nodecl_get_type(nodecl_casted_expr))) \
      { \
          diagnostic_candidates(unresolved_overloaded_type_get_overload_set(nodecl_get_type(nodecl_casted_expr)), &message, locus); \
      } \
      error_printf("%s", message); \
      *nodecl_output = nodecl_make_err_expr(locus); \
      nodecl_free(nodecl_casted_expr); \
      return; \
    } while (0)

    char is_dynamic_cast = 0;

    if (strcmp(cast_kind, "C") == 0)
    {
        //   const_cast
        //   static_cast
        //   static_cast + const_cast
        //   reinterpret_cast
        //   reinterpret_cast + const_casted
        //
        // This should be the same as checking const_cast, static_cast and
        // reinterpret_cast but not checking const being casted away

        typedef char (*conversion_is_valid_fun_t)(
                nodecl_t *nodecl_expression,
                type_t* dest_type,
                decl_context_t decl_context);

        conversion_is_valid_fun_t conversion_funs[] = {
            conversion_is_valid_const_cast,
            conversion_is_valid_static_cast,
            conversion_is_valid_reinterpret_cast,
            NULL
        };


        int i = 0;
        while (conversion_funs[i] != NULL)
        {
            // Copy the tree because the conversion functions may modify its
            // argument due to lvalue conversions
            nodecl_t nodecl_copy = nodecl_shallow_copy(nodecl_casted_expr);

            if ((conversion_funs[i])(
                    &nodecl_copy,
                    declarator_type,
                    decl_context))
            {
                // Use the first one that works for us
                nodecl_free(nodecl_casted_expr);
                nodecl_casted_expr = nodecl_copy;

                DEBUG_CODE()
                {
                    const char* cast_name = "<<unknown_cast>>";
                    if (conversion_funs[i] == conversion_is_valid_const_cast)
                        cast_name = "const_cast";
                    else if (conversion_funs[i] == conversion_is_valid_static_cast)
                        cast_name = "static_cast";
                    else if (conversion_funs[i] == conversion_is_valid_reinterpret_cast)
                        cast_name = "reinterpret_cast";

                    fprintf(stderr, "EXPRTYPE: '%s' allows this C-style cast\n", cast_name);
                }
                break;
            }

            nodecl_free(nodecl_copy);
            i++;
        }

        if (conversion_funs[i] == NULL)
        {
            type_t* orig_type = nodecl_get_type(nodecl_casted_expr);
            type_t* dest_type = declarator_type;
            C_LANGUAGE()
            {
                // Remove references for the sake of the diagnostic
                orig_type = no_ref(orig_type);
                dest_type = no_ref(dest_type);
            }
            const char* message = NULL;
            uniquestr_sprintf(&message, "%s: error: invalid cast of expression '%s' of type '%s' to type '%s'\n",
                    locus_to_str(locus),
                    codegen_to_str(nodecl_casted_expr, decl_context),
                    print_type_str(orig_type, decl_context),
                    print_type_str(dest_type, decl_context));
            if (is_unresolved_overloaded_type(nodecl_get_type(nodecl_casted_expr)))
            {
                diagnostic_candidates(
                        unresolved_overloaded_type_get_overload_set(nodecl_get_type(nodecl_casted_expr)),
                        &message,
                        locus);
            }
            error_printf("%s", message);
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_casted_expr);

            return;
        }
    }
    else if (strcmp(cast_kind, "static_cast") == 0)
    {
        if (!conversion_is_valid_static_cast(
                    &nodecl_casted_expr,
                    declarator_type,
                    decl_context))
        {
            CONVERSION_ERROR;
        }

        if (conversion_casts_away_constness(nodecl_get_type(nodecl_casted_expr), declarator_type, locus))
        {
            CONVERSION_ERROR;
        }
    }
    else if ((is_dynamic_cast = (strcmp(cast_kind, "dynamic_cast") == 0)))
    {
        if (check_expr_flags.must_be_constant)
        {
            error_printf("%s: error: dynamic_cast in constant expression\n",
                    locus_to_str(locus));
        }
        if (!conversion_is_valid_dynamic_cast(
                    &nodecl_casted_expr,
                    declarator_type,
                    decl_context))
        {
            CONVERSION_ERROR;
        }

        if (conversion_casts_away_constness(nodecl_get_type(nodecl_casted_expr), declarator_type, locus))
        {
            CONVERSION_ERROR;
        }
    }
    else if (strcmp(cast_kind, "reinterpret_cast") == 0)
    {
        if (check_expr_flags.must_be_constant)
        {
            error_printf("%s: error: reinterpret_cast in constant expression\n",
                    locus_to_str(locus));
        }
        if (!conversion_is_valid_reinterpret_cast(
                    &nodecl_casted_expr,
                    declarator_type,
                    decl_context))
        {
            CONVERSION_ERROR;
        }

        if (conversion_casts_away_constness(nodecl_get_type(nodecl_casted_expr), declarator_type, locus))
        {
            CONVERSION_ERROR;
        }
    }
    else if (strcmp(cast_kind, "const_cast") == 0)
    {
        if (!conversion_is_valid_const_cast(
                    &nodecl_casted_expr,
                    declarator_type,
                    decl_context))
        {
            CONVERSION_ERROR;
        }
    }
    else
    {
        internal_error("Code unreachable", 0);
    }

    if (IS_CXX_LANGUAGE)
    {
        int vector_size = 0;
        if (is_class_type(declarator_type)
                // Intel vector extension
                && !is_intel_vector_struct_type(declarator_type, &vector_size)
                )
        {
            scope_entry_t* called_symbol = NULL;
            ERROR_CONDITION(
                    !(
                        nodecl_get_kind(nodecl_casted_expr) == NODECL_FUNCTION_CALL
                        && (called_symbol = nodecl_get_symbol(nodecl_get_child(nodecl_casted_expr, 0))) != NULL
                        && ((symbol_entity_specs_get_is_constructor(called_symbol)
                                && equivalent_types(
                                    get_actual_class_type(symbol_entity_specs_get_class_type(called_symbol)),
                                    get_unqualified_type(get_actual_class_type(declarator_type))))
                            || (symbol_entity_specs_get_is_conversion(called_symbol)
                                && equivalent_types(
                                    get_actual_class_type(function_type_get_return_type(called_symbol->type_information)),
                                    get_unqualified_type(get_actual_class_type(declarator_type)))))
                     ),
                    "This should be a call to a constructor or conversion", 0);

            // Use the call to the constructor rather than a cast node
            *nodecl_output = nodecl_casted_expr;

            return;
        }
    }

    *nodecl_output = nodecl_make_cast(
            nodecl_casted_expr,
            declarator_type,
            cast_kind,
            locus);

    if (nodecl_is_constant(nodecl_casted_expr))
    {
        const_value_t * casted_value = nodecl_get_constant(nodecl_casted_expr);

        const_value_t* converted_value = cxx_nodecl_make_value_conversion(
                declarator_type,
                nodecl_get_type(nodecl_casted_expr),
                casted_value,
                /* is_explicit_type_cast */ 1,
                locus);

        // Propagate zero types
        if (converted_value != NULL)
        {
            if (is_zero_type(nodecl_get_type(nodecl_casted_expr))
                    && const_value_is_zero(converted_value)
                    && (is_integral_type(declarator_type)
                        || is_bool_type(declarator_type)))
            {
                nodecl_set_type(*nodecl_output, get_zero_type(declarator_type));
            }
        }

        nodecl_set_constant(*nodecl_output, converted_value);
    }


    if (!is_dynamic_cast)
    {
        // Expressions of the following form are value-dependent if either the
        // type-id or simple-type-specifier is dependent or the expression or
        // cast-expression is value-dependent:
        //
        // static_cast < type-id > ( expression )
        // const_cast < type-id > ( expression )
        // reinterpret_cast < type-id > ( expression )
        // ( type-id ) ( expression )

        nodecl_expr_set_is_value_dependent(*nodecl_output,
                nodecl_expr_is_value_dependent(nodecl_casted_expr)
                || is_dependent_type(declarator_type));
    }

#undef CONVERSION_ERROR
}

static void check_nodecl_explicit_type_conversion(
        type_t* type_info,
        nodecl_t nodecl_explicit_initializer,
        decl_context_t decl_context,
        nodecl_t* nodecl_output,
        const locus_t* locus)
{
    if (nodecl_is_err_expr(nodecl_explicit_initializer))
    {
        *nodecl_output = nodecl_explicit_initializer;
        return;
    }

    nodecl_t nodecl_initializer = nodecl_null();

    if (nodecl_get_kind(nodecl_explicit_initializer) == NODECL_CXX_PARENTHESIZED_INITIALIZER)
    {
        nodecl_t nodecl_expr_list = nodecl_get_child(nodecl_explicit_initializer, 0);

        // T(e) behaves like (T)e but we have to be careful to avoid
        // constructing NODECL_CAST nodes when type_info is a dependent or
        // when n is type-dependent (if n is only value-dependent then it is fine
        // to have a NODECL_CAST)
        nodecl_t n = nodecl_null();
        if (nodecl_list_length(nodecl_expr_list) == 1)
        {
            n = nodecl_list_head(nodecl_expr_list);
        }

        if (!is_dependent_type(type_info)
                && !nodecl_is_null(n)
                && !nodecl_expr_is_type_dependent(n))
        {
            check_nodecl_cast_expr(n,
                    decl_context,
                    type_info,
                    "C",
                    locus,
                    nodecl_output);

            // Nothing else to do here, this NODECL_CAST is sensible
            return;
        }
        else
        {
            check_nodecl_parenthesized_initializer(
                    nodecl_explicit_initializer,
                    decl_context,
                    type_info,
                    /* is_explicit */ 1,
                    /* is_explicit_type_cast */ 1,
                    /* emit_cast */ 1,
                    &nodecl_initializer);
        }
    }
    else if (nodecl_get_kind(nodecl_explicit_initializer) == NODECL_CXX_BRACED_INITIALIZER)
    {
        check_nodecl_braced_initializer(
                nodecl_explicit_initializer,
                decl_context,
                type_info,
                /* is_explicit_type_cast */ 1,
                IK_DIRECT_INITIALIZATION,
                &nodecl_initializer);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }

    if (is_dependent_type(type_info)
            || nodecl_expr_is_type_dependent(nodecl_initializer))
    {
        *nodecl_output =
            nodecl_make_cxx_explicit_type_cast(nodecl_initializer, type_info, locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output,
                is_dependent_type(type_info));

        // Expressions of the following form are value-dependent if either the
        // type-id or simple-type-specifier is dependent or the expression or
        // cast-expression is value-dependent:
        //
        // simple-type-specifier ( expression-list[opt] )
        nodecl_expr_set_is_value_dependent(*nodecl_output,
                nodecl_expr_is_value_dependent(nodecl_initializer)
                || is_dependent_type(type_info));
    }
    else
    {
        *nodecl_output = nodecl_initializer;
    }
}

static void check_explicit_type_conversion_common(type_t* type_info,
        AST expr, AST explicit_initializer, decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    nodecl_t nodecl_explicit_initializer = nodecl_null();
    compute_nodecl_initialization(explicit_initializer, decl_context,
            /* preserve_top_level_parentheses */ 0,
            &nodecl_explicit_initializer);

    check_nodecl_explicit_type_conversion(type_info, nodecl_explicit_initializer, decl_context,
            nodecl_output, ast_get_locus(expr));
}

static void check_explicit_typename_type_conversion(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST id_expression = ASTSon0(expr);

    scope_entry_list_t* entry_list = query_id_expression_flags(decl_context, id_expression,
            NULL,
            // Do not examine uninstantiated templates
            DF_DEPENDENT_TYPENAME);

    if (entry_list == NULL)
    {
        error_printf("%s: error: 'typename %s' not found in the current scope\n",
                ast_location(id_expression),
                prettyprint_in_buffer(id_expression));
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    scope_entry_t* entry = entry_list_head(entry_list);
    entry_list_free(entry_list);

    if (entry->kind != SK_TYPEDEF
            && entry->kind != SK_ENUM
            && entry->kind != SK_CLASS
            && entry->kind != SK_DEPENDENT_ENTITY
            && entry->kind != SK_TEMPLATE_TYPE_PARAMETER)
    {
        error_printf("%s: error: '%s' does not name a type\n",
                ast_location(expr),
                prettyprint_in_buffer(id_expression));
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    AST explicit_initializer = ASTSon1(expr);

    check_explicit_type_conversion_common(
            get_user_defined_type(entry),
            expr,
            explicit_initializer,
            decl_context, nodecl_output);
}

static void check_explicit_type_conversion(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // An explicit type conversion is of the form
    //
    //   T ( expression-list );
    //   T { expression-list };
    //
    // T has to be a valid typename
    AST type_specifier_seq = ASTSon0(expr);

    type_t *type_info = NULL;

    gather_decl_spec_t gather_info;
    memset(&gather_info, 0, sizeof(gather_info));

    nodecl_t dummy_nodecl_output = nodecl_null();
    build_scope_decl_specifier_seq(type_specifier_seq, &gather_info, &type_info,
            decl_context, &dummy_nodecl_output);

    if (is_error_type(type_info))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    AST explicit_initializer = ASTSon1(expr);
    check_explicit_type_conversion_common(
            type_info,
            expr,
            explicit_initializer,
            decl_context,
            nodecl_output);
}

void check_function_arguments(AST arguments, decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    check_list_of_expressions(arguments, decl_context, nodecl_output);
}

static scope_entry_list_t* do_koenig_lookup(nodecl_t nodecl_simple_name, 
        nodecl_t nodecl_argument_list, 
        decl_context_t decl_context,
        char *can_succeed)
{
    *can_succeed = 1;

    // First try to do a normal lookup
    scope_entry_list_t* entry_list = query_name_str_flags(decl_context, 
            nodecl_get_text(nodecl_simple_name),
            NULL,
            DF_IGNORE_FRIEND_DECL);

    if (entry_list != NULL)
    {
        // If no member is found we still have to perform member
        char invalid = 0,
             still_requires_koenig = 1,
             at_least_one_candidate = 0;

        scope_entry_list_iterator_t *it = NULL;
        for (it = entry_list_iterator_begin(entry_list);
                !entry_list_iterator_end(it) && !invalid;
                entry_list_iterator_next(it))
        {
            scope_entry_t* entry = entry_advance_aliases(entry_list_iterator_current(it));

            if (entry->kind == SK_CLASS
                    || entry->kind == SK_ENUM)
                continue;

            type_t* type = no_ref(advance_over_typedefs(entry->type_information));
            if (entry->kind != SK_FUNCTION
                    && (entry->kind != SK_VARIABLE
                        || (!is_class_type(type)
                            && !is_pointer_to_function_type(type)
                            && !is_dependent_type(type)))
                    && (entry->kind != SK_TEMPLATE
                        || !is_function_type(
                            named_type_get_symbol(template_type_get_primary_type(type))
                            ->type_information))
                    && entry->kind != SK_TEMPLATE_NONTYPE_PARAMETER
                    && entry->kind != SK_DEPENDENT_ENTITY)
            {
                DEBUG_CODE()
                {
                    fprintf(stderr, "EXPRTYPE: Symbol '%s' with type '%s' cannot be called\n",
                            entry->symbol_name,
                            entry->type_information != NULL ? print_declarator(entry->type_information)
                            : " <no type> ");
                }
                invalid = 1;
            }
            else
            {
                at_least_one_candidate = 1;

                // It can be a dependent entity because of a using of an undefined base
                if (entry->kind == SK_DEPENDENT_ENTITY
                        || entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER
                        || symbol_entity_specs_get_is_member(entry)
                        || (entry->kind == SK_VARIABLE
                            && (is_class_type(type)
                                || is_pointer_to_function_type(type)
                                || is_dependent_type(type)))
                        // Or It's a local function
                        || (entry->kind == SK_FUNCTION
                            &&  entry->decl_context.current_scope->kind == BLOCK_SCOPE))
                {
                    still_requires_koenig = 0;
                }
            }
        }
        entry_list_iterator_free(it);

        // This cannot be called at all
        if (invalid || !at_least_one_candidate)
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Trying to call something not callable (it is not a function, template function or object)\n");
            }
            *can_succeed = 0;
            entry_list_free(entry_list);
            return NULL;
        }

        if (!still_requires_koenig)
        {
            // No koenig needed
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Not Koenig will be performed since we found something that "
                        "is member or pointer to function type or dependent entity\n");
            }
            entry_list_free(entry_list);
            return NULL;
        }
    }

    // Build types of arguments
    type_t* argument_types[MCXX_MAX_FUNCTION_CALL_ARGUMENTS];
    int num_arguments = 0;

    memset(argument_types, 0, sizeof(argument_types));

    if (!nodecl_is_null(nodecl_argument_list))
    {
        int num_items = 0, i;
        nodecl_t* list = nodecl_unpack_list(nodecl_argument_list, &num_items);

        for (i = 0; i < num_items; i++)
        {
            nodecl_t nodecl_arg = list[i];

            type_t* argument_type = nodecl_get_type(nodecl_arg);

            if (is_unresolved_overloaded_type(argument_type))
            {
                // If possible, simplify it
                scope_entry_t* entry =
                    unresolved_overloaded_type_simplify(argument_type, decl_context, 
                            nodecl_get_locus(nodecl_arg));
                if (entry != NULL)
                {
                    nodecl_t nodecl_argument = nodecl_null();
                    if (!symbol_entity_specs_get_is_member(entry)
                            || symbol_entity_specs_get_is_static(entry))
                    {
                        argument_type = get_lvalue_reference_type(entry->type_information);
                        nodecl_argument = nodecl_make_symbol(entry, nodecl_get_locus(nodecl_arg));
                    }
                    else
                    {
                        argument_type = get_pointer_to_member_type(
                                entry->type_information,
                                symbol_entity_specs_get_class_type(entry));
                        nodecl_argument = nodecl_make_pointer_to_member(entry, 
                                argument_type,
                                nodecl_get_locus(nodecl_arg));
                    }

                    nodecl_arg = nodecl_argument;
                }

                argument_type = nodecl_get_type(nodecl_arg);
            }

            ERROR_CONDITION(num_arguments >= MCXX_MAX_FUNCTION_CALL_ARGUMENTS, "Too many arguments", 0);

            argument_types[num_arguments] = argument_type;
            num_arguments++;
        }
        xfree(list);
    }

    entry_list_free(entry_list);

    entry_list = koenig_lookup(
            num_arguments,
            argument_types,
            decl_context,
            nodecl_simple_name,
            nodecl_get_locus(nodecl_simple_name));

    if (entry_list != NULL)
    {
        char invalid = 0,
             at_least_one_candidate = 0;

        // If no member is found we still have to perform member
        scope_entry_list_iterator_t *it = NULL;
        for (it = entry_list_iterator_begin(entry_list);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it))
        {
            scope_entry_t* entry = entry_advance_aliases(entry_list_iterator_current(it));

            if (entry->kind == SK_CLASS
                    || entry->kind == SK_ENUM)
                continue;

            type_t* type = no_ref(advance_over_typedefs(entry->type_information));
            if (entry->kind != SK_FUNCTION
                    && (entry->kind != SK_VARIABLE
                        || (!is_class_type(type)
                            && !is_pointer_to_function_type(type)
                            && !is_dependent_type(type)))
                    && (entry->kind != SK_TEMPLATE
                        || !is_function_type(
                            named_type_get_symbol(template_type_get_primary_type(type))
                            ->type_information)))
            {
                invalid = 1;
            }
            {
                at_least_one_candidate = 1;
            }
        }
        if (invalid || !at_least_one_candidate)
        {
            // This can't be called!
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Trying to call something not callable (it is not a function, template function or object)\n");
            }
            *can_succeed = 0;
            entry_list_free(entry_list);
            return NULL;
        }
        entry_list_iterator_free(it);
    }

    // If we arrive here, we have at least one function candidate

    enum cxx_symbol_kind filter_function_names[] =
    {
        SK_VARIABLE,
        SK_FUNCTION,
        SK_TEMPLATE,
        SK_DEPENDENT_ENTITY,
        SK_USING,
    };

    // The entry-list may contain useless symbols (SK_CLASS or SK_ENUM), we should filter it
    scope_entry_list_t* candidates_list = filter_symbol_kind_set(entry_list,
            STATIC_ARRAY_LENGTH(filter_function_names),
            filter_function_names);

    entry_list_free(entry_list);

    return candidates_list;
}

typedef
struct check_arg_data_tag
{
    decl_context_t decl_context;
} check_arg_data_t;


static char arg_type_is_ok_for_param_type_c(type_t* arg_type, type_t* param_type, 
        int num_parameter, nodecl_t *arg, check_arg_data_t *p)
{
    standard_conversion_t result;
    char found_a_conversion = 0;
    found_a_conversion = standard_conversion_between_types(&result, arg_type, param_type, nodecl_get_locus(*arg));
    if (!found_a_conversion)
    {
        if (is_class_type(param_type))
        {
            type_t* class_type = get_actual_class_type(param_type);
            if (is_transparent_union(param_type) || is_transparent_union(class_type))
            {
                scope_entry_list_t* list_of_members = class_type_get_members(class_type);
                scope_entry_list_iterator_t* it = entry_list_iterator_begin(list_of_members);

                while (!found_a_conversion && !entry_list_iterator_end(it))
                {
                    scope_entry_t* current_member = entry_list_iterator_current(it);
                    found_a_conversion = standard_conversion_between_types(&result, arg_type, current_member->type_information,
                            nodecl_get_locus(*arg));
                    entry_list_iterator_next(it);
                }
                entry_list_free(list_of_members);
            }
        }
        if (!found_a_conversion)
        {
            error_printf("%s: error: argument %d of type '%s' cannot be "
                    "converted to type '%s' of parameter\n",
                    nodecl_locus_to_str(*arg),
                    num_parameter + 1,
                    print_type_str(no_ref(arg_type), p->decl_context),
                    print_type_str(param_type, p->decl_context));
        }
    }
    return found_a_conversion;
}

static char arg_type_is_ok_for_param_type_cxx(type_t* arg_type, type_t* param_type, 
        int num_parameter, nodecl_t *arg, check_arg_data_t* p)
{
    nodecl_t old_arg = *arg;
    check_nodecl_function_argument_initialization(*arg, p->decl_context, param_type,
            /* disallow_narrowing */ 0, arg);

    if (nodecl_is_err_expr(*arg))
    {
        error_printf("%s: error: argument %d of type '%s' cannot be "
                "converted to type '%s' of parameter\n",
                nodecl_locus_to_str(*arg),
                num_parameter + 1,
                print_type_str(arg_type, p->decl_context),
                print_type_str(param_type, p->decl_context));
        nodecl_free(old_arg);
        return 0;
    }
    return 1;
}

static type_t* compute_default_argument_conversion(type_t* arg_type,
        decl_context_t decl_context,
        const locus_t* locus,
        char emit_diagnostic)
{
    ERROR_CONDITION(arg_type == NULL, "Invalid type", 0);
    if (is_error_type(arg_type))
        return arg_type;

    type_t* result_type = arg_type;

    if (is_any_reference_type(result_type))
    {
        // T& -> T
        result_type = no_ref(result_type);
    }

    if (is_array_type(result_type))
    {
        // T[] -> T*
        result_type = get_pointer_type(array_type_get_element_type(result_type));
    }

    if (is_function_type(result_type))
    {
        // T(...) -> T(*)(...)
        result_type = get_pointer_type(result_type);
    }

    if (is_float_type(result_type))
    {
        // float -> double
        result_type = get_double_type();
    }
    else if (is_pointer_type(result_type)
            || is_nullptr_type(result_type))
    {
        // Use the pointer type we have right now
    }
    else if (is_pointer_to_member_type(result_type))
    {
        // Use the pointer to member we have right now
    }
    else if (is_enum_type(result_type))
    {
        result_type = enum_type_get_underlying_type(result_type);
    }
    else if (is_char_type(result_type)
            || is_signed_char_type(result_type)
            || is_unsigned_char_type(result_type)
            || is_signed_short_int_type(result_type)
            || is_unsigned_short_int_type(result_type))
    {
        // SUBINT -> int
        result_type = get_signed_int_type();
    }
    else if (is_integral_type(result_type))
    {
        // Do nothing for other integer types: int, long, ...
    }
    else if (is_floating_type(result_type))
    {
        // Do nothing for other floating types: double, long double, ...
    }
    else if (is_class_type(result_type))
    {
        if (IS_CXX_LANGUAGE && !is_pod_type(result_type))
        {
            if (emit_diagnostic)
            {
                warn_printf("%s: warning: passing of non-POD class '%s' to an ellipsis parameter\n",
                        locus_to_str(locus),
                        print_type_str(no_ref(result_type), decl_context));
            }
        }
    }
    else if (is_gcc_builtin_va_list(result_type))
    {
        // We will assume it is magically compatible everywhere (sometimes it
        // is a POD-struct, sometimes it is just void*, ...)
    }
    else
    {
        if (emit_diagnostic)
        {
            error_printf("%s: error: no suitable default argument promotion exists when passing argument of type '%s'\n",
                    locus_to_str(locus),
                    print_type_str(no_ref(result_type), decl_context));
        }
        result_type = get_error_type();
    }

    return result_type;
}

static char check_argument_types_of_call(
        nodecl_t nodecl_called,
        nodecl_t nodecl_argument_list,
        type_t* function_type,
        char (*arg_type_is_ok_for_param_type)(type_t* argument_type,
            type_t* parameter_type,
            int num_parameter,
            nodecl_t *arg, check_arg_data_t*),
        const locus_t* locus,
        check_arg_data_t *data,
        nodecl_t* nodecl_output_argument_list)
{
    ERROR_CONDITION(!is_function_type(function_type), "This is not a function type", 0);

    int num_explicit_arguments = nodecl_list_length(nodecl_argument_list);

    int num_args_to_check = num_explicit_arguments;

    char is_promoting_ellipsis = 0;

    if (!function_type_get_lacking_prototype(function_type))
    {
        if (!function_type_get_has_ellipsis(function_type))
        {
            if (num_explicit_arguments != function_type_get_num_parameters(function_type))
            {
                error_printf("%s: error: call to '%s' expects %d arguments but %d passed\n",
                        locus_to_str(locus),
                        codegen_to_str(nodecl_called, data->decl_context),
                        function_type_get_num_parameters(function_type),
                        num_explicit_arguments);
                return 0;
            }
        }
        else
        {
            int num_parameters = function_type_get_num_parameters(function_type);
            is_promoting_ellipsis =
                is_ellipsis_type(
                        function_type_get_parameter_type_num(function_type, num_parameters - 1)
                        );

            int min_arguments = function_type_get_num_parameters(function_type) - 1;
            num_args_to_check = min_arguments;

            if (num_explicit_arguments < min_arguments)
            {
                error_printf("%s: error: call to '%s' expects at least %d parameters but only %d passed\n",
                        locus_to_str(locus),
                        codegen_to_str(nodecl_called, data->decl_context),
                        min_arguments,
                        num_explicit_arguments);
                return 0;
            }
        }
    }

    *nodecl_output_argument_list = nodecl_null();

    char no_arg_is_faulty = 1;
    if (!nodecl_is_null(nodecl_argument_list))
    {
        int i, num_elements = 0;
        nodecl_t* list = nodecl_unpack_list(nodecl_argument_list, &num_elements);

        for (i = 0; i < num_elements; i++)
        {
            nodecl_t arg = list[i];

            // We do not check unprototyped functions
            if (!function_type_get_lacking_prototype(function_type))
            {
                // Ellipsis is not to be checked
                if (i < num_args_to_check) 
                {
                    type_t* arg_type = nodecl_get_type(arg);
                    type_t* param_type = NULL;
                    param_type = function_type_get_parameter_type_num(function_type, i);

                    if (!arg_type_is_ok_for_param_type(arg_type, param_type, i, &arg, data))
                    {
                        no_arg_is_faulty = 0;
                    }
                }
                else
                {
                    if (is_promoting_ellipsis)
                    {
                        type_t* arg_type = nodecl_get_type(arg);

                        type_t* default_conversion = compute_default_argument_conversion(
                                arg_type,
                                data->decl_context,
                                nodecl_get_locus(arg),
                                /* emit_diagnostic */ 1);
                        if (is_error_type(default_conversion))
                        {
                            no_arg_is_faulty = 0;
                        }
                    }
                }
            }
            else
            {
                type_t* arg_type = nodecl_get_type(arg);

                type_t* default_conversion = compute_default_argument_conversion(
                        arg_type,
                        data->decl_context,
                        nodecl_get_locus(arg),
                        /* emit_diagnostic */ 1);
                if (is_error_type(default_conversion))
                {
                    no_arg_is_faulty = 0;
                }
            }
            *nodecl_output_argument_list = nodecl_append_to_list(*nodecl_output_argument_list,
                    arg);
        }

        xfree(list);
    }

    return no_arg_is_faulty;
}

static char any_is_member_function(scope_entry_list_t* candidates)
{
    char is_member = 0;

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(candidates);
            !entry_list_iterator_end(it) && !is_member;
            entry_list_iterator_next(it))
    {
        is_member |= symbol_entity_specs_get_is_member(entry_list_iterator_current(it));
    }
    entry_list_iterator_free(it);

    return is_member;
}

static char any_is_nonstatic_member_function(scope_entry_list_t* candidates)
{
    char is_member = 0;

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(candidates);
            !entry_list_iterator_end(it) && !is_member;
            entry_list_iterator_next(it))
    {
        is_member |= symbol_entity_specs_get_is_member(entry_list_iterator_current(it))
            && !symbol_entity_specs_get_is_static(entry_list_iterator_current(it));
    }
    entry_list_iterator_free(it);

    return is_member;
}


char can_be_called_with_number_of_arguments(scope_entry_t *entry, int num_arguments)
{
    type_t* function_type = entry->type_information;

    ERROR_CONDITION(!is_function_type(function_type), "This is not a function type", 0);

    int num_parameters = function_type_get_num_parameters(function_type);
    // Number of real parameters, ellipsis are counted as parameters
    // but only in the type system
    if (function_type_get_has_ellipsis(function_type))
        num_parameters--;

    // Simple case everybody considers
    if (num_parameters == num_arguments)
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Function '%s' at '%s' can be called with %d arguments since it matches the number of parameters\n",
                    entry->symbol_name,
                    locus_to_str(entry->locus),
                    num_arguments);
        }
        return 1;
    }
    else if (num_arguments > num_parameters)
    {
        // This can only be done if we have an ellipsis
        if (function_type_get_has_ellipsis(function_type))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Function '%s' at '%s' can be called with %d (although the "
                        "function just has %d parameters) because of ellipsis\n",
                        entry->symbol_name,
                        locus_to_str(entry->locus),
                        num_arguments,
                        num_parameters);
            }
            return 1;
        }
        else
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Function '%s' at '%s' cannot be called with %d arguments "
                        "since it expects %d parameters\n",
                        entry->symbol_name,
                        locus_to_str(entry->locus),
                        num_arguments,
                        num_parameters);
            }
            return 0;
        }
    }
    else if (num_arguments < num_parameters)
    {
        // We have to check that parameter num_arguments has default argument
        scope_entry_t* function_with_defaults = entry;

        if (is_template_specialized_type(entry->type_information))
        {
            function_with_defaults = 
                named_type_get_symbol(
                        template_type_get_primary_type(
                            template_specialized_type_get_related_template_type(
                                entry->type_information)));
        }

        if (symbol_entity_specs_get_num_parameters(function_with_defaults) > 0
                && symbol_entity_specs_get_default_argument_info_num(function_with_defaults, num_arguments) != NULL)
        {
            // Sanity check
            int i;
            for (i = num_arguments; i < num_parameters; i++)
            {
                ERROR_CONDITION(symbol_entity_specs_get_default_argument_info_num(function_with_defaults, i) == NULL,
                        "Bad function parameter declaration info", 0);
            }
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Function '%s' at '%s' can be called with %d arguments "
                        "(although it has %d parameters) because of default arguments\n",
                        function_with_defaults->symbol_name,
                        locus_to_str(function_with_defaults->locus),
                        num_arguments,
                        num_parameters);
            }
            return 1;
        }
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Function '%s' at '%s' cannot be called with %d arguments "
                    "since it expects %d parameters\n",
                    function_with_defaults->symbol_name,
                    locus_to_str(function_with_defaults->locus),
                    num_arguments,
                    num_parameters);
        }
        return 0;
    }

    return 0;
}

static void handle_computed_function_type(
        nodecl_t *nodecl_called,
        type_t** called_type,
        nodecl_t nodecl_argument_list,
        decl_context_t decl_context UNUSED_PARAMETER,
        const locus_t* locus)
{
    nodecl_t nodecl_symbol = *nodecl_called;

    ERROR_CONDITION(nodecl_get_kind(nodecl_symbol) != NODECL_SYMBOL, "Invalid called entity", 0);
    scope_entry_t* generic_name = nodecl_get_symbol(nodecl_symbol);

    int i, num_elements;
    nodecl_t* list = nodecl_unpack_list(nodecl_argument_list, &num_elements);
    type_t* arg_types[num_elements];

    for (i = 0; i < num_elements; i++)
    {
        arg_types[i] = nodecl_get_type(list[i]);
    }

    computed_function_type_t compute_function = computed_function_type_get_computing_function(*called_type);

    const_value_t* const_val = NULL;
    scope_entry_t* specific_name = compute_function(generic_name, arg_types, list, num_elements, &const_val);

    xfree(list);

    if (specific_name == NULL)
    {
        error_printf("%s: error: invalid call to generic function '%s'\n", 
                locus_to_str(locus),
                generic_name->symbol_name);
        *nodecl_called = nodecl_make_err_expr(locus);
        *called_type = get_error_type();
        nodecl_free(nodecl_argument_list);
    }
    else
    {
        *nodecl_called = nodecl_make_symbol(specific_name, locus);
        *called_type = specific_name->type_information;
    }
}

static void check_nodecl_function_call_c(nodecl_t nodecl_called, 
        nodecl_t nodecl_argument_list, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    // Keep the original name, lest it was a dependent call after all
    const locus_t* locus = nodecl_get_locus(nodecl_called);

    type_t* called_type = no_ref(nodecl_get_type(nodecl_called));

    if (is_computed_function_type(called_type))
    {
        nodecl_t nodecl_called_sym = nodecl_shallow_copy(nodecl_called);
        handle_computed_function_type(&nodecl_called_sym, &called_type, nodecl_argument_list, decl_context, locus);

        if (nodecl_is_err_expr(nodecl_called_sym))
        {
            *nodecl_output = nodecl_called_sym;
            nodecl_free(nodecl_called);
            nodecl_free(nodecl_argument_list);
            return;
        }

        nodecl_called = nodecl_called_sym;
    }

    if (!is_function_type(called_type)
            && !is_pointer_to_function_type(called_type))
    {
        error_printf("%s: expression '%s' cannot be called\n", 
                nodecl_locus_to_str(nodecl_called),
                codegen_to_str(nodecl_called, nodecl_retrieve_context(nodecl_called)));
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_called);
        nodecl_free(nodecl_argument_list);
        return;
    }

    if (is_pointer_to_function_type(called_type))
        called_type = pointer_type_get_pointee_type(called_type);

    check_arg_data_t data;
    data.decl_context = decl_context;

    nodecl_t nodecl_argument_list_output = nodecl_null();
    if (!check_argument_types_of_call(
                nodecl_called,
                nodecl_argument_list,
                called_type,
                arg_type_is_ok_for_param_type_c,
                locus,
                &data,
                &nodecl_argument_list_output))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_called);
        nodecl_free(nodecl_argument_list);
        return;
    }

    type_t* return_type = function_type_get_return_type(called_type);

    // Everything else seems fine
    *nodecl_output = cxx_nodecl_make_function_call(
            nodecl_called,
            /* called name */ nodecl_null(),
            nodecl_argument_list_output,
            /* function_form */ nodecl_null(), // We don't need a function form in C language
            return_type,
            decl_context,
            locus);
}

static void check_nodecl_function_call_cxx(
        nodecl_t nodecl_called,
        nodecl_t nodecl_argument_list,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(nodecl_called);

    if (nodecl_get_kind(nodecl_called) == NODECL_PSEUDO_DESTRUCTOR_NAME)
    {
        nodecl_t called_entity = nodecl_get_child(nodecl_called, 0);

        if (!is_class_type(no_ref(nodecl_get_type(called_entity))))
        {
            *nodecl_output = nodecl_make_cast(called_entity,
                    get_void_type(),
                    /* cast_kind */ "C",
                    nodecl_get_locus(called_entity));
            return;
        }
    }

    // Let's build the function form
    nodecl_t function_form = nodecl_null();
    {
        type_t* called_type = nodecl_get_type(nodecl_called);
        if (called_type != NULL &&
                is_unresolved_overloaded_type(called_type))
        {
            template_parameter_list_t* template_args =
                unresolved_overloaded_type_get_explicit_template_arguments(called_type);
            if (template_args != NULL)
            {
                function_form = nodecl_make_cxx_function_form_template_id(locus);
                nodecl_set_template_parameters(function_form, template_args);
            }
        }
    }

    // If any in the expression list is type dependent this call is all dependent
    char any_arg_is_type_dependent = 0;
    char any_arg_is_value_dependent = 0;
    int i, num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_argument_list, &num_items);
    for (i = 0; i < num_items && !any_arg_is_type_dependent; i++)
    {
        nodecl_t argument = list[i];
        if (nodecl_expr_is_type_dependent(argument))
        {
            any_arg_is_type_dependent = 1;
        }
        if (nodecl_expr_is_value_dependent(argument))
        {
            any_arg_is_value_dependent = 1;
        }
    }
    xfree(list);

    scope_entry_t* this_symbol = resolve_symbol_this(decl_context);

    // Let's check the called entity
    // If it is a NODECL_CXX_DEP_NAME_SIMPLE it will require Koenig lookup
    scope_entry_list_t* candidates = NULL;
    nodecl_t nodecl_called_name = nodecl_called;
    if (nodecl_get_kind(nodecl_called) == NODECL_CXX_DEP_NAME_SIMPLE
            // If not NULL it means it has some type and then it is not a Koenig call
            && nodecl_get_type(nodecl_called) == NULL)
    {
        char can_succeed = 1;
        // If can_succeed becomes zero, this call is not possible at all (e.g.
        // we are "calling" a typedef-name or class-name)
        if (!any_arg_is_type_dependent)
        {
            candidates = do_koenig_lookup(nodecl_called, nodecl_argument_list, decl_context, &can_succeed);
        }

        if (candidates == NULL && can_succeed)
        {
            // Try a plain lookup
            candidates = query_nodecl_name_flags(decl_context, nodecl_called,
                    NULL,
                    DF_DEPENDENT_TYPENAME | DF_IGNORE_FRIEND_DECL);
        }

        if (candidates == NULL
                && !any_arg_is_type_dependent)
        {
            error_printf("%s: error: called name '%s' not found in the current scope\n",
                    locus_to_str(nodecl_get_locus(nodecl_called)),
                    codegen_to_str(nodecl_called, nodecl_retrieve_context(nodecl_called)));
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_called));
            nodecl_free(nodecl_called);
            nodecl_free(nodecl_argument_list);
            return;
        }
        else if (candidates != NULL)
        {
            nodecl_t nodecl_tmp = nodecl_null();
            cxx_compute_name_from_entry_list(nodecl_called, candidates, decl_context, NULL, &nodecl_tmp);
            nodecl_called = nodecl_tmp;
        }
    }

    template_parameter_list_t* explicit_template_arguments = NULL;
    type_t* called_type = NULL;

    if (is_unresolved_overloaded_type(nodecl_get_type(nodecl_called)))
    {
        type_t* unresolved_type = nodecl_get_type(nodecl_called);
        candidates = unresolved_overloaded_type_get_overload_set(unresolved_type);
        explicit_template_arguments = unresolved_overloaded_type_get_explicit_template_arguments(unresolved_type);
    }
    else
    {
        called_type = nodecl_get_type(nodecl_called);
    }

    if (this_symbol != NULL
            && is_dependent_type(this_symbol->type_information)
            && any_is_member_function(candidates))
    {
        // If we are doing a call F(X) or A::F(X), F (or A::F) is a member
        // function then we have to act as if (*this).F(X) (or
        // (*this).A::F(X)). This implies that if 'this' is dependent the whole
        // call is dependent
        any_arg_is_type_dependent = 1;
    }

    if (this_symbol == NULL
            && any_is_member_function_of_a_dependent_class(candidates))
    {
        // If 'this' is not available and we are doing a call F(X) or A::F(X)
        // and F or A::F is a member of a dependent class assume the whole call
        // is dependent.
        any_arg_is_type_dependent = 1;
    }

    if (!nodecl_is_err_expr(nodecl_called)
            && (any_arg_is_type_dependent
                || nodecl_expr_is_type_dependent(nodecl_called)))
    {
        // If the called entity or one of the arguments is dependent, all the
        // call is dependent

        if (nodecl_get_kind(nodecl_called_name) == NODECL_CXX_DEP_NAME_SIMPLE)
        {
            // The call is of the form F(X) (where F is an unqualified-id)
            if (!any_arg_is_type_dependent)
            {
                // No argument was found dependent, so the name F should have
                // already been bound here
            }
            else
            {
                // The called name is not bound
                nodecl_called = nodecl_called_name;
            }
        }

        entry_list_free(candidates);

        // Create a dependent call
        *nodecl_output = nodecl_make_cxx_dep_function_call(
                nodecl_called,
                nodecl_argument_list,
                /* alternate_name */ nodecl_null(),
                get_unknown_dependent_type(),
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        // FIXME - What if the nodecl_called is value dependent?
        nodecl_expr_set_is_value_dependent(*nodecl_output, any_arg_is_value_dependent);
        return;
    }

    // This 1+ is room for the implicit argument at the 0-th position
    int num_arguments = 1 + nodecl_list_length(nodecl_argument_list);
    type_t* argument_types[MCXX_MAX_FUNCTION_CALL_ARGUMENTS] = { NULL };

    // Fill the argument_types here
    {
        num_items = 0;
        list = nodecl_unpack_list(nodecl_argument_list, &num_items);
        for (i = 0; i < num_items; i++)
        {
            nodecl_t nodecl_arg = list[i];
            // This +1 is because 0 is reserved for the implicit argument type
            argument_types[i + 1] = nodecl_get_type(nodecl_arg);
        }
        xfree(list);
    }

    // This will be filled later
    nodecl_t nodecl_implicit_argument = nodecl_null();

    // We already know the called type
    // When calling a function using a function name
    // called_type is null
    if (called_type != NULL)
    {
        if (!is_class_type(no_ref(called_type))
                && !is_function_type(no_ref(called_type))
                && !is_pointer_to_function_type(no_ref(called_type)))
        {
            // This cannot be called at all
            error_printf("%s: error: expression '%s' cannot be called\n",
                    locus_to_str(locus),
                    codegen_to_str(nodecl_called, nodecl_retrieve_context(nodecl_called)));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_called);
            nodecl_free(nodecl_argument_list);
            return;
        }
        else if (is_function_type(no_ref(called_type))
                    || is_pointer_to_function_type(no_ref(called_type)))
        {
            // This is a C style check, no overload is involved here
            type_t* function_type = no_ref(called_type);
            if (is_pointer_to_function_type(no_ref(function_type)))
                function_type = pointer_type_get_pointee_type(no_ref(function_type));

            check_arg_data_t data;
            data.decl_context = decl_context;

            nodecl_t nodecl_argument_list_output = nodecl_null();

            if (!check_argument_types_of_call(
                        nodecl_called,
                        nodecl_argument_list,
                        function_type,
                        arg_type_is_ok_for_param_type_cxx,
                        locus,
                        &data,
                        &nodecl_argument_list_output))
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                nodecl_free(nodecl_called);
                nodecl_free(nodecl_argument_list);
                return;
            }

            type_t* return_type = function_type_get_return_type(function_type);

            // Everything seems fine here
            *nodecl_output = cxx_nodecl_make_function_call(
                    nodecl_called,
                    /* called name */ nodecl_null(),
                    nodecl_argument_list_output,
                    function_form,
                    return_type,
                    decl_context,
                    locus);
            return;
        }
        else if (is_class_type(no_ref(called_type)))
        {
            type_t* class_type = no_ref(called_type);

            static AST operator = NULL;
            if (operator == NULL)
            {
                operator = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                        ASTLeaf(AST_FUNCTION_CALL_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
            }

            scope_entry_list_t* first_set_candidates = get_member_of_class_type(class_type, operator, decl_context, NULL);

            entry_list_free(candidates);
            candidates = unfold_and_mix_candidate_functions(first_set_candidates,
                    /* builtins */ NULL, argument_types + 1, num_arguments - 1,
                    decl_context,
                    locus,
                    /* explicit_template_arguments */ NULL);
            entry_list_free(first_set_candidates);

            int num_surrogate_functions = 0;
            if (is_named_class_type(class_type))
            {
                scope_entry_t* symbol = named_type_get_symbol(class_type);
                class_type_complete_if_needed(symbol, decl_context, locus);
            }

            scope_entry_list_t* conversion_list = class_type_get_all_conversions(class_type, decl_context);

            scope_entry_list_iterator_t *it = NULL;
            for (it = entry_list_iterator_begin(conversion_list);
                    !entry_list_iterator_end(it);
                    entry_list_iterator_next(it))
            {
                scope_entry_t* conversion = entry_advance_aliases(entry_list_iterator_current(it));

                type_t* destination_type = function_type_get_return_type(conversion->type_information);

                if (is_pointer_to_function_type(no_ref(destination_type)))
                {
                    // Create a faked surrogate function with the type described below
                    scope_entry_t* surrogate_symbol =
                        xcalloc(1, sizeof(*surrogate_symbol));

                    // Add to candidates
                    candidates = entry_list_add(candidates, surrogate_symbol);

                    surrogate_symbol->kind = SK_FUNCTION;
                    {
                        const char *surrogate_name = NULL;
                        uniquestr_sprintf(&surrogate_name, "<surrogate-function-%d>", num_surrogate_functions);

                        surrogate_symbol->symbol_name = surrogate_name;
                    }

                    // Check this to be the proper context required
                    surrogate_symbol->decl_context = decl_context;

                    surrogate_symbol->locus = locus;

                    // This is a surrogate function created here
                    symbol_entity_specs_set_is_surrogate_function(surrogate_symbol, 1);
                    symbol_entity_specs_set_is_builtin(surrogate_symbol, 1);

                    symbol_entity_specs_set_alias_to(surrogate_symbol, conversion);

                    // Given
                    //
                    // struct A
                    // {
                    //   operator R (*)(P1, .., Pn)();
                    // };
                    //
                    // Create a type
                    //
                    //  R () (R (*) (P1, .., Pn), P1, .., Pn)
                    type_t* conversion_functional_type = pointer_type_get_pointee_type(no_ref(destination_type));
                    type_t* conversion_functional_return_type = function_type_get_return_type(conversion_functional_type);
                    int conversion_functional_num_parameters = function_type_get_num_parameters(conversion_functional_type);

                    // We add one for the first parameter type
                    int surrogate_num_parameters = conversion_functional_num_parameters + 1;
                    if (function_type_get_has_ellipsis(conversion_functional_type))
                    {
                        // Add another for the ellipsis if needed
                        surrogate_num_parameters++;
                    }

                    parameter_info_t parameter_info[MCXX_MAX_FUNCTION_PARAMETERS];
                    memset(parameter_info, 0, sizeof(parameter_info));

                    ERROR_CONDITION(MCXX_MAX_FUNCTION_PARAMETERS <= surrogate_num_parameters, 
                            "Too many surrogate parameters %d", surrogate_num_parameters);

                    // First parameter is the type itself
                    parameter_info[0].type_info = destination_type;

                    int k;
                    for (k = 0; k < conversion_functional_num_parameters; k++)
                    {
                        parameter_info[k + 1].type_info = function_type_get_parameter_type_num(conversion_functional_type, k);
                    }

                    if (function_type_get_has_ellipsis(conversion_functional_type))
                    {
                        parameter_info[k + 1].is_ellipsis = 1;
                        parameter_info[k + 1].type_info = get_ellipsis_type();
                    }

                    // Get the type
                    type_t* surrogate_function_type = get_new_function_type(conversion_functional_return_type, 
                            parameter_info, surrogate_num_parameters, REF_QUALIFIER_NONE);

                    // Set it as the type of function
                    surrogate_symbol->type_information = surrogate_function_type;

                    num_surrogate_functions++;
                }
            }

            // This is the implicit argument
            nodecl_implicit_argument = nodecl_shallow_copy(nodecl_called);
            argument_types[0] = called_type;
        }
    }
    else
    {
        // Expand the candidate set
        scope_entry_list_t* first_set_candidates = candidates;
        candidates = unfold_and_mix_candidate_functions(first_set_candidates,
                /* builtins */ NULL, argument_types + 1, num_arguments - 1,
                decl_context,
                locus,
                explicit_template_arguments);
        entry_list_free(first_set_candidates);
    }

    // Now fill the implicit argument type if not done yet
    if (nodecl_is_null(nodecl_implicit_argument))
    {
        if (nodecl_get_kind(nodecl_called) == NODECL_CLASS_MEMBER_ACCESS)
        {
            nodecl_implicit_argument = nodecl_get_child(nodecl_called, 0);
            argument_types[0] = nodecl_get_type(nodecl_implicit_argument);

            if (!nodecl_is_null(nodecl_get_child(nodecl_called, 2)) 
                    && (nodecl_get_kind(nodecl_get_child(nodecl_called, 2)) == NODECL_CXX_DEP_NAME_NESTED // a.B::f()
                        || nodecl_get_kind(nodecl_get_child(nodecl_called, 2)) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED)) // a.::B::f()
            {
                // We need to get the proper subobject otherwise overload may
                // not allow derived to base conversions
                field_path_t field_path;
                field_path_init(&field_path);
                scope_entry_list_t* extra_query = get_member_of_class_type_nodecl(
                        decl_context,
                        no_ref(get_unqualified_type(argument_types[0])),
                        nodecl_get_child(nodecl_called, 2),
                        &field_path);

                ERROR_CONDITION(extra_query == NULL, "This should not happen", 0);
                entry_list_free(extra_query);

                ERROR_CONDITION(field_path.length > 1, "Unexpected length of field path", 0);
                if (field_path.length == 1)
                {
                    type_t* subobject_type = 
                        get_cv_qualified_type(
                                get_user_defined_type(field_path.path[0]),
                                get_cv_qualifier(argument_types[0]));

                    if (is_lvalue_reference_type(argument_types[0]))
                        subobject_type = get_lvalue_reference_type(subobject_type);
                    else if (is_rvalue_reference_type(argument_types[0]))
                        subobject_type = get_rvalue_reference_type(subobject_type);

                    // Create a conversion to the proper subobject
                    nodecl_implicit_argument = nodecl_make_conversion(
                            nodecl_implicit_argument,
                            subobject_type,
                            nodecl_get_locus(nodecl_called));
                    argument_types[0] = subobject_type;
                }
            }
        }
        else
        {
            if (this_symbol != NULL)
            {
                type_t* ptr_class_type = this_symbol->type_information;
                type_t* class_type = pointer_type_get_pointee_type(ptr_class_type);
                // We make a dereference here, thus the argument must be a lvalue
                argument_types[0] = get_lvalue_reference_type(class_type);

                nodecl_t nodecl_sym = nodecl_make_symbol(this_symbol,
                        nodecl_get_locus(nodecl_called));
                nodecl_set_type(nodecl_sym, ptr_class_type);

                nodecl_implicit_argument =
                    nodecl_make_dereference(
                            nodecl_sym,
                            get_lvalue_reference_type(class_type),
                            nodecl_get_locus(nodecl_called));

                if (any_is_nonstatic_member_function(candidates)
                        && (nodecl_get_kind(nodecl_called_name) == NODECL_CXX_DEP_NAME_NESTED // B::f()
                            || nodecl_get_kind(nodecl_called_name) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED)) // ::B::f()
                {
                    field_path_t field_path;
                    field_path_init(&field_path);

                    diagnostic_context_push_buffered();
                    scope_entry_list_t* extra_query = get_member_of_class_type_nodecl(
                            decl_context,
                            no_ref(get_unqualified_type(class_type)),
                            nodecl_called_name,
                            &field_path);
                    diagnostic_context_pop_and_discard();

                    // This may not exist if we are calling a static member
                    // function of another non-base class
                    if (extra_query != NULL)
                    {
                        entry_list_free(extra_query);

                        ERROR_CONDITION(field_path.length > 1, "Unexpected length of field path", 0);
                        if (field_path.length == 1)
                        {
                            type_t* subobject_type = 
                                get_cv_qualified_type(
                                        get_user_defined_type(field_path.path[0]),
                                        get_cv_qualifier(argument_types[0]));

                            if (is_lvalue_reference_type(argument_types[0]))
                                subobject_type = get_lvalue_reference_type(subobject_type);
                            else if (is_rvalue_reference_type(argument_types[0]))
                                subobject_type = get_rvalue_reference_type(subobject_type);

                            // Create a conversion to the proper subobject
                            nodecl_implicit_argument = nodecl_make_conversion(
                                    nodecl_implicit_argument,
                                    subobject_type,
                                    nodecl_get_locus(nodecl_called));
                            argument_types[0] = subobject_type;
                        }
                    }
                }
            }
        }
    }

    candidate_t* candidate_set = NULL;
    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(candidates);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* orig_entry = entry_list_iterator_current(it);
        scope_entry_t* entry = entry_advance_aliases(orig_entry);

        if (symbol_entity_specs_get_is_member(entry) 
                || symbol_entity_specs_get_is_surrogate_function(entry))
        {
            candidate_set = candidate_set_add(candidate_set,
                    orig_entry,
                    num_arguments,
                    argument_types);
        }
        else
        {
            candidate_set = candidate_set_add(candidate_set,
                    orig_entry,
                    num_arguments - 1,
                    argument_types + 1);
        }
    }
    entry_list_iterator_free(it);
    entry_list_free(candidates);

    scope_entry_t* orig_overloaded_call = solve_overload(candidate_set,
            decl_context,
            locus);
    scope_entry_t* overloaded_call = entry_advance_aliases(orig_overloaded_call);

    if (overloaded_call == NULL)
    {
        // Overload failed
        error_message_overload_failed(candidate_set, 
                codegen_to_str(nodecl_called, nodecl_retrieve_context(nodecl_called)),
                decl_context,
                num_arguments - 1,
                argument_types + 1,
                /* implicit_argument */ argument_types[0],
                locus);
        candidate_set_free(&candidate_set);
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_called);
        nodecl_free(nodecl_argument_list);
        return;
    }
    candidate_set_free(&candidate_set);

    type_t* function_type_of_called = NULL;

    nodecl_t nodecl_argument_list_output = nodecl_null();

    // We are calling a surrogate, this implies calling first the conversion function
    if (symbol_entity_specs_get_is_surrogate_function(overloaded_call))
    {
        ERROR_CONDITION(nodecl_is_null(nodecl_implicit_argument), "There must be an implicit argument when calling a surrogate!", 0);

        nodecl_t nodecl_called_surrogate = nodecl_make_symbol(symbol_entity_specs_get_alias_to(overloaded_call), 
                nodecl_get_locus(nodecl_implicit_argument));
        nodecl_set_type(nodecl_called_surrogate, symbol_entity_specs_get_alias_to(overloaded_call)->type_information);

        nodecl_called = cxx_nodecl_make_function_call(
                nodecl_called_surrogate,
                /* called name */ nodecl_null(),
                nodecl_make_list_1(nodecl_implicit_argument),
                nodecl_make_cxx_function_form_implicit(nodecl_get_locus(nodecl_implicit_argument)),
                function_type_get_return_type(symbol_entity_specs_get_alias_to(overloaded_call)->type_information),
                decl_context,
                nodecl_get_locus(nodecl_implicit_argument)
                );

        overloaded_call = symbol_entity_specs_get_alias_to(overloaded_call);

        function_type_of_called = no_ref(function_type_get_return_type(overloaded_call->type_information));

        if (is_pointer_to_function_type(function_type_of_called))
        {
            function_type_of_called = pointer_type_get_pointee_type(function_type_of_called);
        }
        ERROR_CONDITION(!is_function_type(function_type_of_called), "Invalid function type!\n", 0);
    }
    else
    {
        nodecl_called = nodecl_make_symbol(orig_overloaded_call, locus);
        nodecl_set_type(nodecl_called, overloaded_call->type_information);

        function_type_of_called = overloaded_call->type_information;

        // Add this
        if (!nodecl_is_null(nodecl_implicit_argument)
                && symbol_entity_specs_get_is_member(overloaded_call) 
                && !symbol_entity_specs_get_is_static(overloaded_call))
        {
            nodecl_argument_list_output = nodecl_append_to_list(nodecl_argument_list_output,
                    nodecl_implicit_argument);
        }
    }

    // Note that we check this here because of a surrogate being an alias
    if (function_has_been_deleted(decl_context, overloaded_call, locus))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_called);
        nodecl_free(nodecl_argument_list);
        return;
    }

    if (symbol_entity_specs_get_is_member(overloaded_call)
            && !symbol_entity_specs_get_is_static(overloaded_call))
    {
        // Make sure we got an object
        if (nodecl_is_null(nodecl_implicit_argument))
        {
            error_printf("%s: error: cannot call '%s' without an object\n", 
                    locus_to_str(locus),
                    print_decl_type_str(overloaded_call->type_information,
                        decl_context,
                        get_qualified_symbol_name(overloaded_call, decl_context)));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_called);
            nodecl_free(nodecl_argument_list);
            return;
        }
    }

    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Overload resolution succeeded\n");
    }

    // Update the unresolved call with all the conversions
    {
        int arg_i = 0;
        if (symbol_entity_specs_get_is_member(overloaded_call))
        {
            arg_i = 1;
        }

        num_items = 0;
        list = nodecl_unpack_list(nodecl_argument_list, &num_items);

        char is_promoting_ellipsis = 0;
        int num_parameters = function_type_get_num_parameters(function_type_of_called);
        if (function_type_get_has_ellipsis(function_type_of_called))
        {
            is_promoting_ellipsis = is_ellipsis_type(
                    function_type_get_parameter_type_num(function_type_of_called, num_parameters - 1)
                    );
            num_parameters--;
        }

        for (i = 0; i < num_items; i++, arg_i++)
        {
            nodecl_t nodecl_arg = list[i];

            if (i < num_parameters)
            {
                type_t* param_type = function_type_get_parameter_type_num(function_type_of_called, i);

                nodecl_t nodecl_old_arg = nodecl_arg;
                check_nodecl_function_argument_initialization(nodecl_arg, decl_context, param_type,
                        /* disallow_narrowing */ 0,
                        &nodecl_arg);
                if (nodecl_is_err_expr(nodecl_arg))
                {
                    *nodecl_output = nodecl_arg;
                    nodecl_free(nodecl_old_arg);
                    return;
                }
            }
            else
            {
                if (is_promoting_ellipsis)
                {
                    type_t* arg_type = nodecl_get_type(nodecl_arg);
                    // Ellipsis
                    type_t* default_argument_promoted_type = compute_default_argument_conversion(arg_type,
                            decl_context,
                            nodecl_get_locus(nodecl_arg),
                            /* emit_diagnostic */ 1);

                    if (is_error_type(default_argument_promoted_type))
                    {
                        *nodecl_output = nodecl_make_err_expr(locus);
                        nodecl_free(nodecl_called);
                        nodecl_free(nodecl_argument_list);
                        return;
                    }
                }
            }

            if (nodecl_is_err_expr(nodecl_arg))
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                nodecl_free(nodecl_called);
                nodecl_free(nodecl_argument_list);
                return;
            }

            nodecl_argument_list_output = nodecl_append_to_list(nodecl_argument_list_output, nodecl_arg);
        }

        xfree(list);
    }

    type_t* return_type = function_type_get_return_type(function_type_of_called);

    // Everything seems fine here
    *nodecl_output = cxx_nodecl_make_function_call(nodecl_called, 
            nodecl_called_name,
            nodecl_argument_list_output, 
            function_form,
            return_type,
            decl_context,
            locus);
}

void check_nodecl_function_call(
        nodecl_t nodecl_called, 
        nodecl_t nodecl_argument_list, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    if (IS_C_LANGUAGE)
    {
        check_nodecl_function_call_c(nodecl_called, nodecl_argument_list, decl_context, nodecl_output);
    }
    else if (IS_CXX_LANGUAGE)
    {
        check_nodecl_function_call_cxx(nodecl_called, nodecl_argument_list, decl_context, nodecl_output);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}


// A function call is of the form
//   e1 ( e2 )
static void check_function_call(AST expr, decl_context_t decl_context, nodecl_t *nodecl_output)
{
    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Checking for function call '%s' at '%s'\n", 
                prettyprint_in_buffer(expr),
                ast_location(expr));
    }
    // e1 (in the comment above)
    AST called_expression = ASTSon0(expr);
    // e2 (in the comment above)
    AST arguments = ASTSon1(expr);

    // This one will be filled later
    nodecl_t nodecl_called = nodecl_null();

    nodecl_t nodecl_argument_list = nodecl_null();
    check_function_arguments(arguments, decl_context, &nodecl_argument_list);

    if (!nodecl_is_null(nodecl_argument_list) 
            && nodecl_is_err_expr(nodecl_argument_list))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    CXX_LANGUAGE()
    {
        // Note that koenig lookup is simply disabled by means of parentheses,
        // so the check has to be done here.
        //
        // Unqualified ids are subject to argument dependent lookup
        if (ASTType(called_expression) == AST_SYMBOL
            || ASTType(called_expression) == AST_OPERATOR_FUNCTION_ID)
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Call to '%s' will require argument dependent lookup\n",
                        prettyprint_in_buffer(called_expression));
            }
            compute_nodecl_name_from_id_expression(called_expression, decl_context, &nodecl_called);
            // We tell a Koenig lookup from any other unqualified call because
            // the NODECL_CXX_DEP_NAME_SIMPLE does not have any type
            ERROR_CONDITION(nodecl_get_kind(nodecl_called) != NODECL_CXX_DEP_NAME_SIMPLE
                    || nodecl_get_type(nodecl_called) != NULL, "Invalid node", 0);
        }
        // Although conversion function id's are technically subject to argument dependent lookup
        // they never receive arguments and they always will be members. The latter property makes
        // that argument dependent lookup is actually not applied to conversion functions!!!
        else
        {
            check_expression_impl_(called_expression, decl_context, &nodecl_called);
        }
    }

    C_LANGUAGE()
    {
        AST advanced_called_expression = advance_expression_nest(called_expression);

        if (ASTType(advanced_called_expression) == AST_SYMBOL)
        {
            scope_entry_list_t* result = query_nested_name(decl_context, NULL, NULL, advanced_called_expression, NULL);

            scope_entry_t* entry = NULL;
            if (result == NULL)
            {
                // At this point we should create a new symbol in the global scope
                decl_context_t global_context = decl_context;
                global_context.current_scope = decl_context.global_scope;
                entry = new_symbol(global_context, global_context.current_scope, ASTText(advanced_called_expression));

                entry->kind = SK_FUNCTION;
                entry->locus = ast_get_locus(advanced_called_expression);

                type_t* nonproto_type = get_nonproto_function_type(get_signed_int_type(),
                        nodecl_list_length(nodecl_argument_list));

                entry->type_information = nonproto_type;

                warn_printf("%s: warning: implicit declaration of function '%s' in call\n",
                        ast_location(advanced_called_expression),
                        entry->symbol_name);
            }
            else
            {
                entry = entry_list_head(result);
            }

            if (entry->kind != SK_FUNCTION && entry->kind != SK_VARIABLE)
            {
                nodecl_called = nodecl_make_err_expr(ast_get_locus(expr));
            }
            else
            {
                nodecl_called = nodecl_make_symbol(entry, ast_get_locus(called_expression));
                nodecl_set_type(nodecl_called, entry->type_information);
            }
            entry_list_free(result);
        }
        else
        {
            check_expression_impl_(called_expression, decl_context, &nodecl_called);
        }
    }

    if (nodecl_is_err_expr(nodecl_called))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        nodecl_free(nodecl_called);
        nodecl_free(nodecl_argument_list);
        return;
    }

    check_nodecl_function_call(nodecl_called, nodecl_argument_list, decl_context, nodecl_output);
}


static void check_cast_expr(AST expr, AST type_id, AST casted_expression_list, decl_context_t decl_context,
        const char* cast_kind,
        nodecl_t* nodecl_output)
{
    nodecl_t nodecl_casted_expr = nodecl_null();
    AST casted_expression = ASTSon1(casted_expression_list);
    check_expression_impl_(casted_expression, decl_context, &nodecl_casted_expr);

    if (nodecl_is_err_expr(nodecl_casted_expr))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    AST type_specifier = ASTSon0(type_id);
    AST abstract_declarator = ASTSon1(type_id);

    gather_decl_spec_t gather_info;
    memset(&gather_info, 0, sizeof(gather_info));

    nodecl_t dummy_nodecl_output = nodecl_null();

    type_t* simple_type_info = NULL;
    build_scope_decl_specifier_seq(type_specifier, &gather_info, &simple_type_info,
            decl_context, &dummy_nodecl_output);

    if (is_error_type(simple_type_info))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    type_t* declarator_type = simple_type_info;
    compute_declarator_type(abstract_declarator, &gather_info, simple_type_info,
            &declarator_type, decl_context, &dummy_nodecl_output);

    if (is_error_type(declarator_type))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    int i;
    for (i = 0; i < gather_info.num_vla_dimension_symbols; i++)
    {
        push_extra_declaration_symbol(gather_info.vla_dimension_symbols[i]);
    }

    check_nodecl_cast_expr(nodecl_casted_expr,
            decl_context,
            declarator_type,
            cast_kind,
            ast_get_locus(expr),
            nodecl_output);
}

static void check_nodecl_comma_operand(nodecl_t nodecl_lhs, 
        nodecl_t nodecl_rhs, 
        decl_context_t decl_context,
        nodecl_t* nodecl_output,
        const locus_t* locus)
{
    if (nodecl_is_err_expr(nodecl_lhs))
    {
        *nodecl_output = nodecl_lhs;
        return;
    }

    if (nodecl_is_err_expr(nodecl_rhs))
    {
        *nodecl_output = nodecl_rhs;
        return;
    }

    static AST operation_comma_tree = NULL;
    if (operation_comma_tree == NULL)
    {
        operation_comma_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_COMMA_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    if (nodecl_is_err_expr(nodecl_lhs)
            || nodecl_is_err_expr(nodecl_rhs))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_lhs)
            || nodecl_expr_is_type_dependent(nodecl_rhs))
    {
        *nodecl_output = nodecl_make_comma(nodecl_lhs,
                nodecl_rhs,
                get_unknown_dependent_type(),
                nodecl_get_locus(nodecl_lhs));
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    type_t* lhs_type = nodecl_get_type(nodecl_lhs);
    type_t* rhs_type = nodecl_get_type(nodecl_rhs);


    char requires_overload = 0;

    CXX_LANGUAGE()
    {
        requires_overload = any_operand_is_class_or_enum(no_ref(lhs_type), no_ref(rhs_type));
    }

    if (requires_overload)
    {
        // For comma it is empty
        scope_entry_list_t* builtins = NULL;

        scope_entry_t* selected_operator = NULL;

        // We do not want a warning if no overloads are available
        diagnostic_context_push_buffered();
        type_t* computed_type = compute_user_defined_bin_operator_type(operation_comma_tree,
                &nodecl_lhs,
                &nodecl_rhs,
                builtins,
                decl_context,
                locus,
                &selected_operator);
        diagnostic_context_pop_and_discard();

        if (!is_error_type(computed_type))
        {
            ERROR_CONDITION(selected_operator == NULL, "Invalid operator", 0);
            *nodecl_output =
                cxx_nodecl_make_function_call(
                        nodecl_make_symbol(selected_operator, locus),
                        /* called name */ nodecl_null(),
                        nodecl_make_list_2(nodecl_lhs, nodecl_rhs),
                        // This should be a binary infix but comma breaks everything
                        /* function form */ nodecl_null(),
                        function_type_get_return_type(selected_operator->type_information),
                        decl_context,
                        locus);
            return;
        }
        // We will fall-through if no overload exists
    }

    *nodecl_output = nodecl_make_comma(
                nodecl_lhs,
                nodecl_rhs,
                nodecl_get_type(nodecl_rhs),
                locus);

    if (nodecl_is_constant(nodecl_rhs))
    {
        nodecl_set_constant(*nodecl_output, nodecl_get_constant(nodecl_rhs));
    }

    if (nodecl_expr_is_value_dependent(nodecl_rhs))
    {
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
    }
}

static void check_comma_operand(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST lhs = ASTSon0(expression);
    AST rhs = ASTSon1(expression);

    nodecl_t nodecl_lhs = nodecl_null();
    check_expression_impl_(lhs, decl_context, &nodecl_lhs);

    nodecl_t nodecl_rhs = nodecl_null();
    check_expression_impl_(rhs, decl_context, &nodecl_rhs);

    check_nodecl_comma_operand(nodecl_lhs, nodecl_rhs, decl_context,
            nodecl_output,
            ast_get_locus(expression));
}

enum lambda_capture_default_tag
{
    LAMBDA_CAPTURE_NONE,
    LAMBDA_CAPTURE_COPY,
    LAMBDA_CAPTURE_REFERENCE
};

static void compute_implicit_captures(nodecl_t node,
        enum lambda_capture_default_tag lambda_capture_default,
        scope_entry_list_t** capture_copy_entities,
        scope_entry_list_t** capture_reference_entities,
        scope_entry_t* lambda_symbol,
        char *ok)
{
    if (nodecl_is_null(node))
        return;

    if (nodecl_get_kind(node) == NODECL_OBJECT_INIT)
    {
        return compute_implicit_captures(
                nodecl_get_symbol(node)->value,
                lambda_capture_default,
                capture_copy_entities,
                capture_reference_entities,
                lambda_symbol,
                ok);
    }

    scope_entry_t *entry = nodecl_get_symbol(node);
    if (entry != NULL
            && (entry->kind != SK_VARIABLE
                || symbol_entity_specs_get_is_saved_expression(entry)
                || symbol_entity_specs_get_is_member(entry)
                || (entry->decl_context.current_scope->kind != BLOCK_SCOPE)
                || (entry->decl_context.current_scope->kind == BLOCK_SCOPE
                    && entry->decl_context.current_scope->related_entry == lambda_symbol)
                || symbol_entity_specs_get_is_static(entry)
                || symbol_entity_specs_get_is_extern(entry)))
        entry = NULL;

    if (entry != NULL)
    {
        // Filter the parameters of the lambda
        int i;
        for (i = 0; i < symbol_entity_specs_get_num_related_symbols(lambda_symbol); i++)
        {
            if (symbol_entity_specs_get_related_symbols_num(lambda_symbol, i) == entry)
            {
                entry = NULL;
                break;
            }
        }
    }

    if (entry != NULL)
    {
        if (!entry_list_contains(*capture_copy_entities, entry)
                && !entry_list_contains(*capture_reference_entities, entry))
        {
            if (lambda_capture_default == LAMBDA_CAPTURE_NONE)
            {
                error_printf("%s: error: symbol '%s' has not been captured\n",
                        nodecl_locus_to_str(node),
                        entry->symbol_name);
                *ok = 0;
            }
            else if (strcmp(entry->symbol_name, "this") == 0)
            {
                *capture_copy_entities =
                    entry_list_add(*capture_copy_entities, entry);
            }
            else if (lambda_capture_default == LAMBDA_CAPTURE_COPY)
            {
                *capture_copy_entities =
                    entry_list_add(*capture_copy_entities, entry);
            }
            else if (lambda_capture_default == LAMBDA_CAPTURE_REFERENCE)
            {
                *capture_reference_entities =
                    entry_list_add(*capture_reference_entities, entry);
            }
            else
            {
                internal_error("Code unreachable", 0);
            }
        }
    }

    // Recurse children
    int i;
    for (i = 0; i < MCXX_MAX_AST_CHILDREN; i++)
    {
        compute_implicit_captures(
                nodecl_get_child(node, i),
                lambda_capture_default,
                capture_copy_entities,
                capture_reference_entities,
                lambda_symbol,
                ok);
    }
}

static int lambda_counter = 0;

void implement_lambda_expression(
        decl_context_t decl_context,
        nodecl_t captures,
        scope_entry_t* lambda_symbol, 
        type_t* lambda_function_type,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    // Create class name
    const char* lambda_class_name_str = NULL;
    uniquestr_sprintf(&lambda_class_name_str, "__lambda_class_%d__", lambda_counter);
    lambda_counter++;

    scope_entry_t* lambda_class = new_symbol(decl_context, decl_context.current_scope, lambda_class_name_str);
    lambda_class->locus = locus;
    lambda_class->kind = SK_CLASS;
    lambda_class->type_information = get_new_class_type(decl_context, TT_STRUCT);
    symbol_entity_specs_set_is_user_declared(lambda_class, 1);

    class_type_set_is_lambda(lambda_class->type_information, 1);

    decl_context_t inner_class_context = new_class_context(lambda_class->decl_context, lambda_class);
    class_type_set_inner_context(lambda_class->type_information, inner_class_context);

    int num_captures = 0;
    nodecl_t* capture_list = nodecl_unpack_list(captures, &num_captures);

    instantiation_symbol_map_t* instantiation_symbol_map = instantiation_symbol_map_push(NULL);

    const char* constructor_name = NULL;
    uniquestr_sprintf(&constructor_name, "constructor %s", lambda_class_name_str);
    scope_entry_t* constructor = new_symbol(
            inner_class_context,
            inner_class_context.current_scope,
            constructor_name);
    constructor->locus = locus;
    constructor->kind = SK_FUNCTION;
    constructor->defined = 1;
    symbol_entity_specs_set_is_member(constructor, 1);
    symbol_entity_specs_set_is_user_declared(constructor, 1);
    symbol_entity_specs_set_class_type(constructor, get_user_defined_type(lambda_class));
    symbol_entity_specs_set_is_constructor(constructor, 1);
    symbol_entity_specs_set_access(constructor, AS_PUBLIC);
    symbol_entity_specs_set_is_inline(constructor, 1);
    symbol_entity_specs_set_is_defined_inside_class_specifier(constructor, 1);

    // To be defined if num_captures == 0
    scope_entry_t* ancillary = NULL;
    scope_entry_t* conversion = NULL;

    if (num_captures == 0)
    {
        // Emit a trivial constructor
        symbol_entity_specs_set_is_trivial(constructor, 1);
        constructor->type_information = get_new_function_type(NULL, NULL, 0, REF_QUALIFIER_NONE);

        decl_context_t block_context = new_block_context(inner_class_context);
        block_context.current_scope->related_entry = constructor;

        nodecl_t constructor_function_code = 
            nodecl_make_function_code(
                    nodecl_make_context(
                        nodecl_make_list_1(
                            nodecl_make_compound_statement(
                                nodecl_null(),
                                nodecl_null(),
                                locus)),
                        block_context,
                        locus),
                    nodecl_null(),
                    constructor,
                    locus);

        symbol_entity_specs_set_function_code(constructor,
                constructor_function_code);
        class_type_add_member(lambda_class->type_information, constructor, /* is_definition */ 1);

        // emit a conversion from the class to the pointer type of the function
        // first use a typedef otherwise this function cannot be declared
        type_t* pointer_to_function = get_pointer_type(lambda_function_type);
        scope_entry_t* typedef_function = new_symbol(inner_class_context, inner_class_context.current_scope,
                 "__ptr_fun_type__");
        typedef_function->locus = locus;
        typedef_function->kind = SK_TYPEDEF;
        typedef_function->defined = 1;
        typedef_function->type_information = pointer_to_function;
        symbol_entity_specs_set_is_member(typedef_function, 1);
        symbol_entity_specs_set_is_user_declared(typedef_function, 1);
        symbol_entity_specs_set_class_type(typedef_function, get_user_defined_type(lambda_class));
        symbol_entity_specs_set_access(typedef_function, AS_PRIVATE);
        class_type_add_member(lambda_class->type_information, typedef_function, /* is_definition */ 1);

        // now emit the conversion
        conversion = new_symbol(inner_class_context, inner_class_context.current_scope,
                 "$.operator");
        conversion->kind = SK_FUNCTION;
        conversion->locus = locus;
        conversion->defined = 1;
        symbol_entity_specs_set_is_member(conversion, 1);
        symbol_entity_specs_set_is_user_declared(conversion, 1);
        symbol_entity_specs_set_class_type(conversion, get_user_defined_type(lambda_class));
        conversion->type_information =
            get_new_function_type(get_user_defined_type(typedef_function),
                    NULL, 0, REF_QUALIFIER_NONE);
        symbol_entity_specs_set_is_conversion(conversion, 1);
        symbol_entity_specs_set_access(conversion, AS_PUBLIC);

        class_type_add_member(lambda_class->type_information, conversion, /* is_definition */ 1);

        // now emit an ancillary static member function with the same prototype as the lambda type
        ancillary = new_symbol(inner_class_context, inner_class_context.current_scope,
                 "__ancillary__");
        ancillary->locus = locus;
        ancillary->kind = SK_FUNCTION;
        ancillary->defined = 1;
        symbol_entity_specs_set_is_member(ancillary, 1);
        symbol_entity_specs_set_is_static(ancillary, 1);
        symbol_entity_specs_set_is_user_declared(ancillary, 1);
        symbol_entity_specs_set_class_type(ancillary, get_user_defined_type(lambda_class));
        ancillary->type_information = lambda_function_type;
        symbol_entity_specs_set_access(ancillary, AS_PRIVATE);

        class_type_add_member(lambda_class->type_information, ancillary, /* is_definition */ 1);

        // we will emit ancillary and conversion once the class has been completed
    }
    else
    {
        // Emit a constructor that initializes the fields
        parameter_info_t parameter_info[num_captures + 1];
        memset(parameter_info, 0, sizeof(parameter_info));

        decl_context_t block_context = new_block_context(inner_class_context);
        block_context.current_scope->related_entry = constructor;

        nodecl_t member_initializers = nodecl_null();

        int i;
        for (i = 0; i < num_captures; i++)
        {
            scope_entry_t* sym = nodecl_get_symbol(capture_list[i]);
            char is_capture_by_copy = (nodecl_get_kind(capture_list[i]) == NODECL_CXX_CAPTURE_COPY);

            char is_symbol_this = (strcmp(sym->symbol_name, "this") == 0);

            // type of the parameter (for the function type)
            if (is_capture_by_copy)
            {
                if (is_symbol_this)
                {
                    parameter_info[i].type_info = sym->type_information;
                }
                else
                {
                    parameter_info[i].type_info =
                        lvalue_ref(get_const_qualified_type(no_ref(sym->type_information)));
                }
            }
            else
            {
                parameter_info[i].type_info =
                    lvalue_ref(no_ref(sym->type_information));
            }

            // special fix for this that does not behave like a normal variable
            const char* new_name = NULL;
            if (is_symbol_this)
            {
                new_name = "__this__";
            }
            else
            {
                new_name = sym->symbol_name;
            }

            // register parameter
            scope_entry_t* parameter = new_symbol(
                    block_context,
                    block_context.current_scope,
                    new_name);
            parameter->locus = locus;
            parameter->kind = SK_VARIABLE;
            parameter->defined = 1;
            parameter->type_information = parameter_info[i].type_info;
            symbol_set_as_parameter_of_function(
                    parameter,
                    constructor,
                    /* nesting */ 0, i);
            symbol_entity_specs_add_related_symbols(constructor, parameter);

            nodecl_t nodecl_parameter = nodecl_make_symbol(parameter, locus);
            nodecl_set_type(nodecl_parameter, lvalue_ref(parameter->type_information));

            // register field
            scope_entry_t* field = new_symbol(
                inner_class_context,
                inner_class_context.current_scope,
                new_name);
            field->locus = locus;
            field->defined = 1;
            field->kind = SK_VARIABLE;
            if (is_capture_by_copy)
            {
                field->type_information = get_unqualified_type(no_ref(sym->type_information));
            }
            else
            {
                field->type_information = lvalue_ref(no_ref(sym->type_information));
            }
            symbol_entity_specs_set_is_member(field, 1);
            symbol_entity_specs_set_is_user_declared(field, 1);
            symbol_entity_specs_set_class_type(field, get_user_defined_type(lambda_class));
            symbol_entity_specs_set_access(field, AS_PRIVATE);

            instantiation_symbol_map_add(instantiation_symbol_map, sym, field);

            nodecl_t nodecl_init = nodecl_make_cxx_parenthesized_initializer(
                    nodecl_make_list_1(nodecl_parameter),
                    locus);

            // check that we can initialize the field using the parameter
            check_nodecl_initialization(
                    nodecl_init,
                    block_context,
                    field,
                    field->type_information,
                    &nodecl_init,
                    /* is_auto_type */ 0,
                    /* is_decltype_auto */ 0);

            if (nodecl_is_err_expr(nodecl_init))
            {
                *nodecl_output = nodecl_init;
                xfree(capture_list);
                instantiation_symbol_map_pop(instantiation_symbol_map);
                return;
            }

            nodecl_t nodecl_member_init = nodecl_make_member_init(
                    nodecl_init,
                    field,
                    locus);

            member_initializers = nodecl_append_to_list(
                    member_initializers,
                    nodecl_member_init);

            class_type_add_member(lambda_class->type_information, field, /* is_definition */ 1);
        }

        constructor->type_information =
            get_new_function_type(NULL, parameter_info, num_captures, REF_QUALIFIER_NONE);

        nodecl_t constructor_function_code = 
            nodecl_make_function_code(
                    nodecl_make_context(
                        nodecl_make_list_1(
                            nodecl_make_compound_statement(
                                nodecl_null(),
                                nodecl_null(),
                                locus)),
                        block_context,
                        locus),
                    member_initializers,
                    constructor,
                    locus);

        symbol_entity_specs_set_function_code(constructor,
                constructor_function_code);

        class_type_add_member(lambda_class->type_information, constructor, /* is_definition */ 1);
    }

    // create operator()
    scope_entry_t* operator_call = new_symbol(inner_class_context, inner_class_context.current_scope, STR_OPERATOR_CALL);
    decl_context_t block_context = new_block_context(inner_class_context);
    block_context.current_scope->related_entry = operator_call;

    operator_call->locus = locus;
    operator_call->kind = SK_FUNCTION;
    operator_call->defined = 1;
    symbol_entity_specs_set_is_member(operator_call, 1);
    symbol_entity_specs_set_is_user_declared(operator_call, 1);
    symbol_entity_specs_set_class_type(operator_call, get_user_defined_type(lambda_class));
    symbol_entity_specs_set_access(operator_call, AS_PUBLIC);
    symbol_entity_specs_set_is_inline(operator_call, 1);
    symbol_entity_specs_set_is_defined_inside_class_specifier(operator_call, 1);

    int i;
    for (i = 0; i < symbol_entity_specs_get_num_related_symbols(lambda_symbol); i++)
    {
        scope_entry_t* orig_param = symbol_entity_specs_get_related_symbols_num(lambda_symbol, i);

        scope_entry_t* parameter = new_symbol(block_context, block_context.current_scope, orig_param->symbol_name);
        parameter->locus = locus;
        parameter->kind = SK_VARIABLE;
        parameter->type_information = orig_param->type_information;

        symbol_set_as_parameter_of_function(parameter, operator_call, 
                /* nesting */ 0, i);
        symbol_entity_specs_add_related_symbols(operator_call, parameter);

        instantiation_symbol_map_add(instantiation_symbol_map, orig_param, parameter);
    }

    nodecl_t nodecl_orig_lambda_body = symbol_entity_specs_get_function_code(lambda_symbol);
    ERROR_CONDITION(nodecl_get_kind(nodecl_orig_lambda_body) != NODECL_CONTEXT,
            "Invalid node", 0);
    nodecl_orig_lambda_body = nodecl_get_child(nodecl_orig_lambda_body, 0);

    operator_call->type_information = lambda_function_type;
    if (!symbol_entity_specs_get_is_mutable(lambda_symbol))
    {
        operator_call->type_information = get_const_qualified_type(operator_call->type_information);
    }

    register_symbol_this(block_context,
            lambda_class,
            locus);
    update_symbol_this(operator_call, block_context);
    register_mercurium_pretty_print(operator_call, block_context);

    if (!is_void_type(function_type_get_return_type(operator_call->type_information)))
    {
        scope_entry_t* result_sym = new_symbol(block_context,
                block_context.current_scope,
                ".result"); // This name is currently not user accessible
        result_sym->kind = SK_VARIABLE;
        symbol_entity_specs_set_is_result_var(result_sym, 1);
        result_sym->type_information =
            get_unqualified_type(function_type_get_return_type(operator_call->type_information));

        symbol_entity_specs_set_result_var(operator_call, result_sym);
    }

    nodecl_t nodecl_lambda_body = instantiate_statement(
            nodecl_orig_lambda_body,
            nodecl_retrieve_context(nodecl_orig_lambda_body),
            block_context,
            instantiation_symbol_map);
    instantiation_symbol_map_pop(instantiation_symbol_map);
    instantiation_symbol_map = NULL;
    ERROR_CONDITION(nodecl_is_list(nodecl_lambda_body), "Should not be a list", 0);

    symbol_entity_specs_set_function_code(
            operator_call,
            nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_make_list_1(nodecl_lambda_body),
                    block_context,
                    locus),
                nodecl_null(),
                operator_call,
                locus));

    class_type_add_member(lambda_class->type_information, operator_call,
            /* is_definition */ 1);

    // complete the class
    set_is_complete_type(lambda_class->type_information, 1);

    push_extra_declaration_symbol(lambda_class);

    // rvalue of the class type
    type_t* lambda_type = get_user_defined_type(lambda_class);
    set_is_complete_type(lambda_type, 1);

    nodecl_t nodecl_finish_class = nodecl_null();
    finish_class_type(lambda_class->type_information,
            lambda_type,
            lambda_class->decl_context,
            locus,
            &nodecl_finish_class);

    // Emit the conversion and the ancillary
    if (num_captures == 0)
    {
        ERROR_CONDITION(conversion == NULL || ancillary == NULL, "Invalid symbols", 0);

        // create the ancillary
        block_context = new_block_context(inner_class_context);
        block_context.current_scope->related_entry = ancillary;

        nodecl_t nodecl_argument_list = nodecl_null();
        for (i = 0; i < symbol_entity_specs_get_num_related_symbols(lambda_symbol); i++)
        {
            scope_entry_t* orig_param = symbol_entity_specs_get_related_symbols_num(lambda_symbol, i);

            scope_entry_t* parameter = new_symbol(block_context, block_context.current_scope, orig_param->symbol_name);
            parameter->locus = locus;
            parameter->kind = SK_VARIABLE;
            parameter->type_information = orig_param->type_information;

            nodecl_t nodecl_param_ref = nodecl_make_symbol(parameter, locus);
            nodecl_set_type(nodecl_param_ref, lvalue_ref(parameter->type_information));

            nodecl_argument_list = nodecl_append_to_list(
                    nodecl_argument_list,
                    nodecl_param_ref);

            symbol_set_as_parameter_of_function(parameter, ancillary, 
                    /* nesting */ 0, i);
            symbol_entity_specs_add_related_symbols(ancillary, parameter);
        }

        // The ancillary creates an object of the closure type and invokes the operator()
        scope_entry_t* obj = new_symbol(block_context, block_context.current_scope, "obj");
        obj->locus = locus;
        obj->kind = SK_VARIABLE;
        obj->type_information = get_user_defined_type(lambda_class);
        obj->defined = 1;
        symbol_entity_specs_set_is_user_declared(obj, 1);

        // lambda obj;
        nodecl_t nodecl_ancillary_body = nodecl_null();
        nodecl_ancillary_body = nodecl_append_to_list(
                nodecl_ancillary_body,
                nodecl_make_cxx_def(nodecl_null(), obj, locus));

        nodecl_t nodecl_obj_ref = nodecl_make_symbol(obj, locus);
        nodecl_set_type(nodecl_obj_ref, lvalue_ref(obj->type_information));

        nodecl_t nodecl_member = nodecl_make_symbol(operator_call, locus);

        // obj.operator ()
        nodecl_t nodecl_called = nodecl_make_class_member_access(
                nodecl_obj_ref,
                nodecl_member,
                nodecl_null(),
                get_unresolved_overloaded_type(entry_list_new(operator_call), NULL),
                locus);

        // obj.operator ()( ... args ... )
        nodecl_t nodecl_call_to_operator = nodecl_null();
        check_nodecl_function_call_cxx(
                nodecl_called,
                nodecl_argument_list,
                block_context,
                &nodecl_call_to_operator);

        // return obj.operator ()( ... args ... )
        nodecl_t nodecl_return_stmt = 
            nodecl_make_return_statement(
                    nodecl_call_to_operator,
                    locus);

        nodecl_ancillary_body =
            nodecl_append_to_list(
                    nodecl_ancillary_body,
                    nodecl_return_stmt);

        // {
        //   lambda obj;
        //   return obj.operator()(...args...);
        // }
        nodecl_t nodecl_compound_statement = nodecl_null();
        build_scope_nodecl_compound_statement(
                nodecl_ancillary_body,
                block_context,
                locus,
                &nodecl_compound_statement);

        symbol_entity_specs_set_function_code(
                ancillary,
                nodecl_make_function_code(
                    nodecl_make_context(
                        nodecl_make_list_1(
                            nodecl_compound_statement
                            ),
                        block_context,
                        locus),
                    nodecl_null(),
                    ancillary,
                    locus));
        symbol_entity_specs_set_is_inline(ancillary, 1);
        symbol_entity_specs_set_is_defined_inside_class_specifier(ancillary, 1);

        // Now create the conversor itself
        block_context = new_block_context(inner_class_context);
        block_context.current_scope->related_entry = conversion;

        register_symbol_this(block_context,
                lambda_class,
                locus);
        update_symbol_this(conversion, block_context);

        nodecl_t nodecl_ancillary_ref = nodecl_make_symbol(ancillary, locus);
        nodecl_set_type(nodecl_ancillary_ref, lvalue_ref(ancillary->type_information));

        // {
        //    return __ancillary__;
        // }
        nodecl_compound_statement =
            nodecl_make_compound_statement(
                    nodecl_make_list_1(
                        nodecl_make_return_statement(
                            nodecl_ancillary_ref,
                            locus)),
                    nodecl_null(),
                    locus);

        symbol_entity_specs_set_function_code(
                conversion,
                nodecl_make_function_code(
                    nodecl_make_context(
                        nodecl_make_list_1(
                            nodecl_compound_statement
                            ),
                        block_context,
                        locus),
                    nodecl_null(),
                    conversion,
                    locus));
        symbol_entity_specs_set_is_inline(conversion, 1);
        symbol_entity_specs_set_is_defined_inside_class_specifier(conversion, 1);
    }

    // Now create an instance of the object using the captured symbols
    nodecl_t explicit_initializer = nodecl_null();

    for (i = 0; i < num_captures; i++)
    {
        scope_entry_t* sym = nodecl_get_symbol(capture_list[i]);

        nodecl_t nodecl_sym = nodecl_make_symbol(sym, locus);
        nodecl_set_type(nodecl_sym, lvalue_ref(no_ref(sym->type_information)));

        explicit_initializer = nodecl_append_to_list(
                explicit_initializer,
                nodecl_sym);
    }

    explicit_initializer = nodecl_make_cxx_parenthesized_initializer(
            explicit_initializer,
            locus);

    check_nodecl_explicit_type_conversion(
            lambda_type,
            explicit_initializer,
            decl_context,
            nodecl_output,
            locus);
}

// Note this function only implements C++11
// C++14 lambdas are different and will require a rework of this function
static void check_lambda_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    CXX03_LANGUAGE()
    {
        warn_printf("%s: warning: lambda-expressions are only valid in C++11\n",
                ast_location(expression));
    }

    if (check_expr_flags.must_be_constant)
    {
        error_printf("%s: error: lambda expression in constant-expression",
                ast_location(expression));
    }

    AST lambda_capture = ASTSon0(expression);
    AST lambda_declarator = ASTSon1(expression);
    AST compound_statement = ASTSon2(expression);

    enum lambda_capture_default_tag lambda_capture_default = LAMBDA_CAPTURE_NONE;

    scope_entry_list_t* capture_copy_entities = NULL;
    scope_entry_list_t* capture_reference_entities = NULL;

    if (lambda_capture != NULL)
    {
        AST lambda_capture_default_tree = ASTSon0(lambda_capture);

        if (lambda_capture_default_tree != NULL)
        {
            if (ASTType(lambda_capture_default_tree) == AST_LAMBDA_CAPTURE_DEFAULT_VALUE)
            {
                lambda_capture_default = LAMBDA_CAPTURE_COPY;
            }
            else if (ASTType(lambda_capture_default_tree) == AST_LAMBDA_CAPTURE_DEFAULT_ADDR)
            {
                lambda_capture_default = LAMBDA_CAPTURE_REFERENCE;
            }
            else
            {
                internal_error("Code unreachable", 0);
            }
        }

        AST lambda_capture_list = ASTSon1(lambda_capture);
        AST it;

        if (lambda_capture_list != NULL)
        {
            for_each_element(lambda_capture_list, it)
            {
                AST capture = ASTSon1(it);
                char is_pack = 0;

                if (ASTType(capture) == AST_LAMBDA_CAPTURE_PACK_EXPANSION)
                {
                    is_pack = 1;
                    capture = ASTSon0(capture);
                }

                node_t n = ASTType(capture);
                switch (n)
                {
                    case AST_LAMBDA_CAPTURE_VALUE:
                    case AST_LAMBDA_CAPTURE_ADDRESS:
                        {
                            scope_entry_list_t* entry_list = query_name_str(decl_context, ASTText(capture), NULL);
                            if (entry_list == NULL)
                            {
                                error_printf("%s: error: captured entity '%s' not found in current scope\n",
                                        ast_location(capture),
                                        ASTText(capture));
                                *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
                                return;
                            }
                            else
                            {
                                scope_entry_t* entry = entry_list_head(entry_list);

                                if ((is_pack
                                            || entry->kind != SK_VARIABLE
                                            || entry->decl_context.current_scope->kind != BLOCK_SCOPE
                                            || symbol_entity_specs_get_is_static(entry)
                                            || symbol_entity_specs_get_is_extern(entry)
                                            )
                                        && (!is_pack || entry->kind != SK_VARIABLE_PACK))
                                {
                                    error_printf("%s: error: cannot capture entity '%s'\n",
                                            ast_location(capture),
                                            ASTText(capture));
                                    *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
                                    return;
                                }

                                if (entry_list_contains(capture_reference_entities, entry)
                                        || entry_list_contains(capture_copy_entities, entry))
                                {
                                    error_printf("%s: error: entity '%s' captured more than once\n",
                                            ast_location(capture),
                                            ASTText(capture));
                                }
                                else
                                {
                                    if (n == AST_LAMBDA_CAPTURE_VALUE)
                                        capture_copy_entities = entry_list_add(capture_copy_entities, entry);
                                    else
                                        capture_reference_entities = entry_list_add(capture_reference_entities, entry);
                                }
                            }

                            break;
                        }
                    case AST_LAMBDA_CAPTURE_THIS:
                        {
                            scope_entry_list_t* entry_list = query_name_str(decl_context, UNIQUESTR_LITERAL("this"), NULL);
                            if (entry_list == NULL)
                            {
                                error_printf("%s: error: captured entity 'this' not found in current scope\n",
                                        ast_location(capture));
                                *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
                                return;
                            }
                            else
                            {
                                scope_entry_t* entry = entry_list_head(entry_list);
                                entry_list_free(entry_list);

                                if (entry_list_contains(capture_reference_entities, entry)
                                        || entry_list_contains(capture_copy_entities, entry)
                                        || lambda_capture_default == LAMBDA_CAPTURE_COPY)
                                {
                                    error_printf("%s: error: entity 'this' captured more than once\n",
                                            ast_location(capture));
                                }
                                else
                                {
                                    capture_copy_entities = entry_list_add(capture_copy_entities, entry);
                                }
                            }
                            break;
                        }
                    default:
                        {
                            internal_error("Unexpected node' %s'\n", ast_print_node_type(ASTType(capture)));
                        }
                }
            }
        }
    }

    const char *lambda_symbol_str = NULL;
    uniquestr_sprintf(&lambda_symbol_str, ".lambda_%d", lambda_counter);
    lambda_counter++;

    scope_entry_t* lambda_symbol = new_symbol(decl_context, decl_context.current_scope, lambda_symbol_str);
    lambda_symbol->kind = SK_LAMBDA;
    lambda_symbol->locus = ast_get_locus(expression);

    type_t* function_type = NULL;

    decl_context_t lambda_block_context = new_block_context(decl_context);
    lambda_block_context.current_scope->related_entry = lambda_symbol;
    lambda_symbol->related_decl_context = lambda_block_context;

    AST trailing_return_type = NULL;
    AST mutable = NULL;
    if (lambda_declarator != NULL)
    {
        AST function_declarator = ASTSon0(lambda_declarator);
        mutable = ASTSon1(lambda_declarator);
        trailing_return_type = ASTSon2(lambda_declarator);

        if (trailing_return_type != NULL)
        {
            AST type_id = ASTSon0(trailing_return_type);
            function_type = compute_type_for_type_id_tree(type_id, decl_context,
                    /* out simple type */ NULL, /* gather_info */ NULL);

            if (is_error_type(function_type))
            {
                *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
                return;
            }
        }

        nodecl_t nodecl_function_decl_output = nodecl_null();

        gather_decl_spec_t gather_info;
        memset(&gather_info, 0, sizeof(gather_info));
        // gather_info->in_lambda_declarator = 1;

        // Defined in cxx-buildscope.c
        set_function_type_for_lambda(
                &function_type,
                &gather_info,
                function_declarator,
                decl_context,
                &lambda_block_context,
                &nodecl_function_decl_output);

        if (is_error_type(function_type))
        {
            *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
            return;
        }

        set_parameters_as_related_symbols(lambda_symbol,
                &gather_info,
                /* is_definition */ 1,
                ast_get_locus(expression));

        symbol_entity_specs_set_any_exception(lambda_symbol, gather_info.any_exception);
        int i;
        for (i = 0; i < gather_info.num_exceptions; i++)
        {
            symbol_entity_specs_add_exceptions(lambda_symbol, gather_info.exceptions[i]);
        }
        symbol_entity_specs_set_noexception(lambda_symbol, gather_info.noexception);
    }
    else
    {
        function_type = get_new_function_type(NULL, NULL, 0, REF_QUALIFIER_NONE);

        symbol_entity_specs_set_any_exception(lambda_symbol, 1);
    }

    char body_already_processed = 0;
    nodecl_t nodecl_lambda_body = nodecl_null();

    if (trailing_return_type == NULL)
    {
        // C++11: By default void if the form of the lambda is not a simple
        // return statement
        type_t* return_type = get_void_type();

        AST body = ASTSon0(compound_statement);
        if (body != NULL
                && ASTSon0(body) == NULL
                && ASTType(ASTSon1(body)) == AST_RETURN_STATEMENT)
        {
            AST return_stmt = ASTSon1(body);
            AST return_expr = ASTSon0(return_stmt);
            if (return_expr != NULL
                    && ASTType(return_expr) != AST_INITIALIZER_BRACES)
            {
                nodecl_t nodecl_return_expr = nodecl_null();
                check_expression(return_expr, lambda_block_context, &nodecl_return_expr);

                if (nodecl_is_err_expr(nodecl_return_expr))
                {
                    *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
                    return;
                }

                return_type = nodecl_get_type(nodecl_return_expr);

                // Apply lvalue conversions
                return_type = no_ref(return_type);
                if (is_array_type(return_type))
                    return_type = get_pointer_type(array_type_get_element_type(return_type));
                else if (is_function_type(return_type))
                    return_type = get_pointer_type(return_type);

                nodecl_lambda_body =
                    nodecl_make_list_1(
                            nodecl_make_compound_statement(
                                nodecl_make_list_1(
                                    nodecl_make_return_statement(
                                        nodecl_return_expr,
                                        ast_get_locus(return_stmt)
                                        )
                                    ),
                                nodecl_null(),
                                ast_get_locus(compound_statement)
                                )
                            );

                nodecl_lambda_body = nodecl_make_context(
                        nodecl_lambda_body,
                        lambda_block_context,
                        ast_get_locus(compound_statement)
                        );
                body_already_processed = 1;
            }
        }

        function_type = function_type_replace_return_type(function_type, return_type);
    }

    lambda_symbol->type_information = function_type;

    if (!body_already_processed)
    {
        build_scope_statement(compound_statement, lambda_block_context, &nodecl_lambda_body);
        // We are only interested in the head of this list
        nodecl_lambda_body = nodecl_list_head(nodecl_lambda_body);

        body_already_processed = 1;
    }

    ERROR_CONDITION(nodecl_get_kind(nodecl_lambda_body) != NODECL_CONTEXT,
            "Lambda expression wrongly constructed", 0);

    char ok = 1;
    compute_implicit_captures(nodecl_lambda_body,
            lambda_capture_default,
            &capture_copy_entities,
            &capture_reference_entities,
            lambda_symbol,
            &ok);

    if (!ok)
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        return;
    }

    symbol_entity_specs_set_function_code(lambda_symbol, nodecl_lambda_body);
    symbol_entity_specs_set_is_mutable(lambda_symbol, (mutable != NULL));

    // Create tree that represents explicit captures
    nodecl_t captures = nodecl_null();

    char lambda_class_is_dependent = is_dependent_type(function_type);

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(capture_copy_entities);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* sym = entry_list_iterator_current(it);
        lambda_class_is_dependent = lambda_class_is_dependent
            || is_dependent_type(sym->type_information);

        captures = nodecl_append_to_list(
                captures,
                nodecl_make_cxx_capture_copy(
                    sym,
                    ast_get_locus(expression)));
    }
    entry_list_iterator_free(it);
    entry_list_free(capture_copy_entities);
    capture_copy_entities = NULL;

    for (it = entry_list_iterator_begin(capture_reference_entities);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* sym = entry_list_iterator_current(it);
        lambda_class_is_dependent = lambda_class_is_dependent
            || is_dependent_type(sym->type_information);

        captures = nodecl_append_to_list(
                captures,
                nodecl_make_cxx_capture_reference(
                    sym,
                    ast_get_locus(expression)));
    }
    entry_list_iterator_free(it);
    entry_list_free(capture_reference_entities);
    capture_reference_entities = NULL;

    // A lambda is dependent if the enclosing function is dependent
    scope_entry_t* enclosing_function = NULL;
    if (decl_context.current_scope->kind == BLOCK_SCOPE
            && ((enclosing_function = decl_context.current_scope->related_entry) != NULL)
            && (is_dependent_type(enclosing_function->type_information)
                || (symbol_entity_specs_get_is_member(enclosing_function)
                    && is_dependent_type(symbol_entity_specs_get_class_type(enclosing_function)))))
    {
        lambda_class_is_dependent = 1;
    }
    // A lambda is dependent if the enclosing class is dependent
    //
    // Note that we might be in an nonstatic member initializer so we directly
    // check the class_scope (the current scope is a BLOCK_SCOPE in these
    // environments but there is no related entry)
    else if (decl_context.class_scope != NULL)
    {
        scope_entry_t* enclosing_class_symbol = decl_context.class_scope->related_entry;
        type_t* enclosing_class_type = enclosing_class_symbol->type_information;

        lambda_class_is_dependent = lambda_class_is_dependent || is_dependent_type(enclosing_class_type);
    }

    if (lambda_class_is_dependent)
    {
        *nodecl_output = nodecl_make_cxx_lambda(
                captures,
                lambda_symbol,
                function_type,
                ast_get_locus(expression));
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }

    implement_lambda_expression(
            decl_context,
            captures, lambda_symbol, function_type,
            ast_get_locus(expression),
            nodecl_output);
}

// Used in cxx-typeutils.c
void get_packs_in_expression(nodecl_t nodecl,
        scope_entry_t*** packs_to_expand,
        int *num_packs_to_expand)
{
    if (nodecl_is_null(nodecl))
        return;

    // These are in another context, not the current one
    if (nodecl_get_kind(nodecl) == NODECL_CXX_VALUE_PACK)
        return;

    type_t* t = nodecl_get_type(nodecl);
    if (t != NULL)
    {
        get_packs_in_type(t, packs_to_expand, num_packs_to_expand);
    }

    scope_entry_t* entry = nodecl_get_symbol(nodecl);
    if (entry != NULL
            && (entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK
                || entry->kind == SK_VARIABLE_PACK))
    {
        P_LIST_ADD_ONCE(*packs_to_expand, *num_packs_to_expand, entry);
        return;
    }

    if (nodecl_get_kind(nodecl) == NODECL_CXX_DEP_TEMPLATE_ID)
    {
        template_parameter_list_t* template_arguments = nodecl_get_template_parameters(nodecl);

        int i, N = template_arguments->num_parameters;
        for (i = 0; i < N; i++)
        {
            switch (template_arguments->arguments[i]->kind)
            {
                case TPK_TYPE:
                case TPK_TEMPLATE:
                    {
                        get_packs_in_type(template_arguments->arguments[i]->type,
                                packs_to_expand, num_packs_to_expand);
                        break;
                    }
                case TPK_NONTYPE:
                    {
                        get_packs_in_type(template_arguments->arguments[i]->type,
                                packs_to_expand, num_packs_to_expand);
                        get_packs_in_expression(template_arguments->arguments[i]->value,
                                packs_to_expand, num_packs_to_expand);
                        break;
                    }
                default:
                    internal_error("Code unreachable", 0);
            }
        }
    }

    int i;
    for (i = 0; i < MCXX_MAX_AST_CHILDREN; i++)
    {
        get_packs_in_expression(nodecl_get_child(nodecl, i), packs_to_expand, num_packs_to_expand);
    }
}

static char there_are_template_packs(nodecl_t n)
{
    scope_entry_t** packs_to_expand = NULL;
    int num_packs_to_expand = 0;
    get_packs_in_expression(n, &packs_to_expand, &num_packs_to_expand);

    xfree(packs_to_expand);

    return (num_packs_to_expand > 0);
}

static void check_nodecl_initializer_clause_expansion(nodecl_t pack,
        decl_context_t decl_context UNUSED_PARAMETER,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (!there_are_template_packs(pack))
    {
        error_printf("%s: error: pack expansion does not reference any parameter pack\n", 
                locus_to_str(locus));
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(pack);
        return;
    }

    // This is always type and value dependent
    type_t* pack_type = get_pack_type(nodecl_get_type(pack));
    *nodecl_output = nodecl_make_cxx_value_pack(pack, pack_type, locus);
    nodecl_expr_set_is_type_dependent(*nodecl_output, is_dependent_type(pack_type));
    nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
}

static void check_initializer_clause_pack_expansion(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST expanded_expr = ASTSon0(expression);

    nodecl_t nodecl_expander = nodecl_null();
    check_expression_impl_(expanded_expr, decl_context, &nodecl_expander);

    if (nodecl_is_err_expr(nodecl_expander))
    {
        *nodecl_output = nodecl_expander;
        return;
    }

    check_nodecl_initializer_clause_expansion(nodecl_expander, decl_context, ast_get_locus(expanded_expr), nodecl_output);
}

static char dynamic_cast_requires_runtime_check(nodecl_t nodecl_expr)
{
    type_t* t = nodecl_get_type(nodecl_expr);
    return (is_lvalue_reference_type(t)
            && is_class_type(no_ref(t)));
}

static char typeid_of_lvalue_polymorphic_class(nodecl_t nodecl_expr)
{
    type_t* t = nodecl_get_type(nodecl_expr);
    return (is_lvalue_reference_type(t)
            && class_type_is_polymorphic(t));
}

static char function_is_non_throwing(scope_entry_t* entry)
{
    if (!nodecl_is_null(symbol_entity_specs_get_noexception(entry)))
    {
        return nodecl_is_constant(symbol_entity_specs_get_noexception(entry))
            && const_value_is_nonzero(nodecl_get_constant(symbol_entity_specs_get_noexception(entry)));
    }
    else
    {
        return !symbol_entity_specs_get_any_exception(entry)
            && (symbol_entity_specs_get_num_exceptions(entry) == 0);
    }
}

static char check_nodecl_noexcept_rec(nodecl_t nodecl_expr)
{
    if (nodecl_is_null(nodecl_expr))
        return 1;

    // Stop at these nonevaluated things
    if (nodecl_get_kind(nodecl_expr) == NODECL_SIZEOF
            || nodecl_get_kind(nodecl_expr) == NODECL_CXX_SIZEOF
            || nodecl_get_kind(nodecl_expr) == NODECL_CXX_SIZEOF_PACK
            || nodecl_get_kind(nodecl_expr) == NODECL_ALIGNOF
            || nodecl_get_kind(nodecl_expr) == NODECL_CXX_ALIGNOF
            || nodecl_get_kind(nodecl_expr) == NODECL_CXX_NOEXCEPT)
        return 1;

    if (nodecl_get_kind(nodecl_expr) == NODECL_FUNCTION_CALL)
    {
        nodecl_t called = nodecl_get_child(nodecl_expr, 0);
        scope_entry_t* entry = nodecl_get_symbol(called);

        if (entry != NULL && !function_is_non_throwing(entry))
            return 0;
    }
    else if (nodecl_get_kind(nodecl_expr) == NODECL_THROW)
    {
        return 0;
    }
    else if (nodecl_get_kind(nodecl_expr) == NODECL_CAST
            && strcmp(nodecl_get_text(nodecl_expr), "dynamic_cast") == 0
            && dynamic_cast_requires_runtime_check(nodecl_expr))
    {
        return 0;
    }
    else if (nodecl_get_kind(nodecl_expr) == NODECL_TYPEID
            && typeid_of_lvalue_polymorphic_class(nodecl_expr))
    {
        return 0;
    }

    int i;
    char result = 1;
    for (i = 0; i < MCXX_MAX_AST_CHILDREN; i++)
    {
        result = result && check_nodecl_noexcept_rec(nodecl_get_child(nodecl_expr, i));
    }

    return result;
}

static void check_nodecl_noexcept(nodecl_t nodecl_expr, nodecl_t* nodecl_output)
{
    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_expr;
        return;
    }

    if (nodecl_expr_is_value_dependent(nodecl_expr)
            || nodecl_expr_is_type_dependent(nodecl_expr))
    {
        *nodecl_output = nodecl_make_cxx_noexcept(nodecl_expr, get_bool_type(), nodecl_get_locus(nodecl_expr));
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }

    type_t* t = get_bool_type();
    const_value_t* val = NULL;

    if (check_nodecl_noexcept_rec(nodecl_expr))
    {
        val = const_value_get_one(type_get_size(t), 1);
    }
    else
    {
        val = const_value_get_zero(type_get_size(t), 1);
    }

    *nodecl_output = nodecl_make_boolean_literal(t, val, nodecl_get_locus(nodecl_expr));
}

static void check_noexcept_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST noexcept_expr = ASTSon0(expression);

    nodecl_t nodecl_noexcept = nodecl_null();
    check_expression_non_executable(noexcept_expr, decl_context, &nodecl_noexcept);

    check_nodecl_noexcept(nodecl_noexcept, nodecl_output);
}

static void check_templated_member_access(AST templated_member_access, decl_context_t decl_context, 
        char is_arrow, nodecl_t* nodecl_output)
{
    check_member_access(templated_member_access, decl_context, is_arrow, /*has template tag*/ 1, nodecl_output);
}

static char is_pseudo_destructor_id(decl_context_t decl_context,
        type_t* accessed_type,
        nodecl_t nodecl_member)
{
    // A pseudo destructor id has the following structure
    //
    // ::[opt] nested-name-specifier-seq[opt] type-name1 :: ~ type-name2
    //
    // But we have created a nodecl_name which looks like as a qualified name
    //
    // We first lookup this part
    //
    // ::[opt] nested-name-specifier-seq[opt] type-name1
    //
    // it should give a type equivalent to accessed_type
    //
    // then we have to check that type-name1 and type-name2 mean the same name. Note that
    // both can be typedefs and such, but they must mean the same. type-name2 is looked
    // up in the context of type-name1, lest type-name1 was a qualified name

    nodecl_t nodecl_last_part = nodecl_name_get_last_part(nodecl_member);
    if (nodecl_get_kind(nodecl_last_part) != NODECL_CXX_DEP_NAME_SIMPLE)
    {
        return 0;
    }

    const char* last_name = nodecl_get_text(nodecl_last_part);
    // This is not a destructor-id
    if (last_name[0] != '~')
        return 0;

    // Ignore '~'
    last_name = uniquestr(last_name + 1);

    // Now build ::[opt] nested-name-specifier-seq[opt] type-name1
    scope_entry_t* first_entry = NULL;
    nodecl_t new_list = nodecl_null();
    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_get_child(nodecl_member, 0), &num_items);
    if (num_items >= 2)
    {
        // Build the same list without the last name
        nodecl_t nodecl_new_nested_name = nodecl_null();
        if ((num_items - 1) > 1)
        {
            int i;
            for (i = 0; i < num_items - 1; i++)
            {
                new_list = nodecl_append_to_list(new_list, nodecl_shallow_copy(list[i]));
            }

            if (nodecl_get_kind(nodecl_member) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED)
            {
                nodecl_new_nested_name = nodecl_make_cxx_dep_global_name_nested(new_list, 
                        nodecl_get_locus(nodecl_member));
            }
            else
            {
                nodecl_new_nested_name = nodecl_make_cxx_dep_name_nested(new_list, 
                        nodecl_get_locus(nodecl_member));
            }
        }
        else
        {
            // For the case T::~T, we cannot build a nested name with a single
            // element, so use the element itself
            nodecl_new_nested_name = nodecl_shallow_copy(list[0]);
        }

        scope_entry_list_t* entry_list = query_nodecl_name_flags(decl_context, 
                nodecl_new_nested_name, NULL, DF_DEPENDENT_TYPENAME);

        if (entry_list == NULL)
            return 0;

        scope_entry_t* entry = entry_list_head(entry_list);
        entry_list_free(entry_list);

        // FIXME - This is very infortunate and should be solved in a different way
        if (entry->kind == SK_TEMPLATE_TYPE_PARAMETER)
        {
            entry = lookup_of_template_parameter(
                    decl_context,
                    symbol_entity_specs_get_template_parameter_nesting(entry),
                    symbol_entity_specs_get_template_parameter_position(entry));
        }

        if (entry->kind != SK_TYPEDEF
                && entry->kind != SK_CLASS
                && entry->kind != SK_ENUM)
            return 0;

        first_entry = entry;
    }

    // Now check that type-name2 names the same type we have found so far
    scope_entry_list_t* entry_list = NULL;
    if (first_entry != NULL)
    {
        entry_list = query_name_str(first_entry->decl_context, last_name, NULL);
    }
    else
    {
        entry_list = query_name_str(decl_context, last_name, NULL);
    }

    if (entry_list == NULL)
        return 0;

    scope_entry_t* second_entry = entry_list_head(entry_list);
    entry_list_free(entry_list);

    // FIXME - This is very infortunate and should be solved in a different way
    if (second_entry->kind == SK_TEMPLATE_TYPE_PARAMETER)
    {
        second_entry = lookup_of_template_parameter(
                decl_context,
                symbol_entity_specs_get_template_parameter_nesting(second_entry),
                symbol_entity_specs_get_template_parameter_position(second_entry));
    }

    if (second_entry->kind != SK_TYPEDEF
            && second_entry->kind != SK_CLASS
            && second_entry->kind != SK_ENUM)
        return 0;

    if (first_entry != NULL
            && !equivalent_types(
                get_user_defined_type(first_entry),
                get_user_defined_type(second_entry)))
    {
        return 0;
    }

    if (!equivalent_types(
                get_user_defined_type(second_entry),
                get_unqualified_type(no_ref(accessed_type))))
    {
        return 0;
    }

    return 1;
}

static char compute_path_to_subobject(
        scope_entry_t* derived_class_type,
        scope_entry_t* base_class_type,
        int** path_to_subobject,
        int *num_items
        )
{
    if (equivalent_types(derived_class_type->type_information,
                base_class_type->type_information))
    {
        // Note that the last one is not added
        return 1;
    }
    else
    {
        int num_bases = class_type_get_num_bases(derived_class_type->type_information);
        int i;

        char found_a_path = 0;

        for (i = 0; i < num_bases; i++)
        {
            char is_virtual = 0;
            char is_dependent = 0;
            char is_expansion = 0;
            access_specifier_t access_spec = AS_UNKNOWN;
            scope_entry_t* current_base = class_type_get_base_num(derived_class_type->type_information, i,
                    &is_virtual, &is_dependent, &is_expansion, &access_spec);

            if (is_virtual || is_dependent)
                continue;

            char got_path =
                compute_path_to_subobject(
                        current_base,
                        base_class_type,
                        path_to_subobject,
                        num_items);

            ERROR_CONDITION(got_path && found_a_path, "More than one path found. "
                    "This should not happen for unambiguous bases!\n", 0);

            if (got_path)
            {
                found_a_path = got_path;

                P_LIST_ADD(*path_to_subobject, *num_items, i);
            }
        }

        return found_a_path;
    }
}

static const_value_t* compute_subconstant_of_class_member_access(
        const_value_t* const_value,
        type_t* class_type,
        scope_entry_t* given_base_object,
        scope_entry_t* subobject)
{
    if (const_value == NULL)
        return NULL;

    if (!const_value_is_structured(const_value))
        return NULL;

    ERROR_CONDITION(given_base_object != NULL
            && given_base_object->kind != SK_CLASS, "Invalid base", 0);
    ERROR_CONDITION(subobject->kind != SK_VARIABLE
            && subobject->kind != SK_CLASS, "Invalid subobject", 0);

    if (given_base_object != NULL)
    {
        // Recursively solve this subobject in two steps
        const_value_t* intermediate_value = compute_subconstant_of_class_member_access(
                const_value,
                class_type,
                NULL,
                given_base_object);
        return compute_subconstant_of_class_member_access(
                intermediate_value,
                get_user_defined_type(given_base_object),
                NULL,
                subobject);
    }

    // From here given_base_object == NULL so we do not have to care about it

    const_value_t* result = NULL;

    int length_path = 0;
    int *path_info = NULL;

    char got_path = compute_path_to_subobject(
            named_type_get_symbol(class_type),
            named_type_get_symbol(symbol_entity_specs_get_class_type(subobject)),
            &path_info,
            &length_path);

    ERROR_CONDITION(!got_path, "No path was constructed", 0);

    result = const_value;

    if (const_value_is_unknown(result))
        return NULL;

    int i;
    for (i = 0; i < length_path && result != NULL; i++)
    {
        if (path_info[i] < const_value_get_num_elements(result))
        {
            result = const_value_get_element_num(result, path_info[i]);
        }
        else
        {
            result = NULL;
        }
    }

    xfree(path_info);

    if (result == NULL
            || const_value_is_unknown(result))
        return NULL;

    // Now lookup the data member/direct base
    scope_entry_list_t* subobjects_list = NULL;
    if (subobject->kind == SK_VARIABLE)
        subobjects_list = class_type_get_nonstatic_data_members(symbol_entity_specs_get_class_type(subobject));
    else if (subobject->kind == SK_CLASS)
        // Note that this function skips virtual bases
        subobjects_list = class_type_get_direct_base_classes(subobject->type_information);
    else
        internal_error("Code unreachable", 0);

    i = 0;
    int member_index = -1;
    scope_entry_list_iterator_t* it = NULL;
    for (it = entry_list_iterator_begin(subobjects_list);
            !entry_list_iterator_end(it) && (member_index < 0);
            entry_list_iterator_next(it), i++)
    {
        if (entry_list_iterator_current(it) == subobject)
            member_index = i;
    }

    entry_list_free(subobjects_list);

    // Data members go after bases
    if (subobject->kind == SK_VARIABLE)
        member_index += class_type_get_num_bases(symbol_entity_specs_get_class_type(subobject));

    if (member_index < 0
            || member_index >= const_value_get_num_elements(result))
        return NULL;

    result = const_value_get_element_num(result, member_index);

    if (const_value_is_unknown(result))
        result = NULL;

    return result;
}


static void check_nodecl_member_access(
        nodecl_t nodecl_accessed, 
        nodecl_t nodecl_member,
        decl_context_t decl_context, 
        char is_arrow,
        char has_template_tag,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (nodecl_is_err_expr(nodecl_accessed))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_accessed);
        nodecl_free(nodecl_member);
        return;
    }

    const char* template_tag = has_template_tag ? "template " : "";

    if (nodecl_expr_is_type_dependent(nodecl_accessed))
    {
        if (!is_arrow)
        {
            *nodecl_output = nodecl_make_cxx_class_member_access(
                    nodecl_accessed,
                    nodecl_member,
                    get_unknown_dependent_type(),
                    locus);

            nodecl_set_text(*nodecl_output, template_tag);
        }
        else
        {
            *nodecl_output = nodecl_make_cxx_arrow(
                    nodecl_accessed,
                    nodecl_member,
                    get_unknown_dependent_type(),
                    template_tag,
                    locus);
        }

        nodecl_t nodecl_last_part =  nodecl_name_get_last_part(nodecl_member);
        if (nodecl_get_kind(nodecl_last_part) == NODECL_CXX_DEP_NAME_CONVERSION)
        {
            char ok = compute_type_of_dependent_conversion_type_id(nodecl_last_part,
                    decl_context);
            if (!ok)
            {
                *nodecl_output = nodecl_make_err_expr(locus);
                nodecl_free(nodecl_accessed);
                nodecl_free(nodecl_member);
                return;
            }
        }

        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }


    type_t* accessed_type = nodecl_get_type(nodecl_accessed);
    nodecl_t nodecl_accessed_out = nodecl_accessed;
    char operator_arrow = 0;

    // First we adjust the actually accessed type
    // if we are in '->' syntax
    if (is_arrow)
    {
        if (is_pointer_type(no_ref(accessed_type)))
        {
            accessed_type = lvalue_ref(pointer_type_get_pointee_type(no_ref(accessed_type)));

            nodecl_accessed_out = 
                nodecl_make_dereference(
                        nodecl_accessed,
                        accessed_type,
                        nodecl_get_locus(nodecl_accessed));
        }
        else if (is_array_type(no_ref(accessed_type)))
        {
            accessed_type = lvalue_ref(array_type_get_element_type(no_ref(accessed_type)));

            nodecl_accessed_out = 
                nodecl_make_dereference(
                        nodecl_accessed,
                        accessed_type,
                        nodecl_get_locus(nodecl_accessed));
        }
        else if (IS_CXX_LANGUAGE
                && is_class_type(no_ref(accessed_type)))
        {
            operator_arrow = 1;
        }
        else
        {
            error_printf("%s: error: '->%s' cannot be applied to '%s' (of type '%s')\n",
                    nodecl_locus_to_str(nodecl_accessed),
                    codegen_to_str(nodecl_member, nodecl_retrieve_context(nodecl_member)),
                    codegen_to_str(nodecl_accessed, nodecl_retrieve_context(nodecl_accessed)),
                    print_type_str(no_ref(accessed_type), decl_context));

            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_accessed);
            nodecl_free(nodecl_member);
            return;
        }

    }

    if (operator_arrow)
    {
        // In this case we have to lookup for an arrow operator
        // and then update the accessed type. We will rely on
        // overload mechanism to do it
        static AST arrow_operator_tree = NULL;
        if (arrow_operator_tree == NULL)
        {
            arrow_operator_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                    ASTLeaf(AST_POINTER_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
        }

        // First normalize the type keeping the cv-qualifiers
        cv_qualifier_t cv_qualif = CV_NONE;
        accessed_type = advance_over_typedefs_with_cv_qualif(no_ref(accessed_type), &cv_qualif);
        accessed_type = get_cv_qualified_type(accessed_type, cv_qualif);

        scope_entry_list_t* operator_arrow_list = get_member_of_class_type(accessed_type,
                arrow_operator_tree, decl_context, NULL);

        if (operator_arrow_list == NULL)
        {
            error_printf("%s: error: '->%s' cannot be applied to '%s' (of type '%s')\n",
                    nodecl_locus_to_str(nodecl_accessed),
                    codegen_to_str(nodecl_member, nodecl_retrieve_context(nodecl_member)),
                    codegen_to_str(nodecl_accessed, nodecl_retrieve_context(nodecl_accessed)),
                    print_type_str(nodecl_get_type(nodecl_accessed), decl_context));

            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_accessed);
            nodecl_free(nodecl_member);
            return;
        }

        type_t* argument_types[1] = { 
            /* Note that we want the real original type since it might be a referenced type */
            nodecl_get_type(nodecl_accessed) 
        };

        candidate_t* candidate_set = NULL;
        scope_entry_list_iterator_t *it = NULL;
        for (it = entry_list_iterator_begin(operator_arrow_list);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it))
        {
            scope_entry_t* entry = entry_list_iterator_current(it);
            candidate_set = candidate_set_add(candidate_set,
                    entry,
                    /* num_arguments */ 1,
                    argument_types);
        }
        entry_list_iterator_free(it);
        entry_list_free(operator_arrow_list);

        scope_entry_t* orig_selected_operator_arrow = solve_overload(candidate_set,
                decl_context, nodecl_get_locus(nodecl_accessed));
        scope_entry_t* selected_operator_arrow = entry_advance_aliases(orig_selected_operator_arrow);

        if (selected_operator_arrow == NULL)
        {
            error_message_overload_failed(candidate_set, 
                    "operator->",
                    decl_context,
                    /* num_arguments */ 0, 
                    /* no explicit arguments */ NULL,
                    /* implicit_argument */ argument_types[0],
                    nodecl_get_locus(nodecl_accessed));

            candidate_set_free(&candidate_set);
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_accessed);
            nodecl_free(nodecl_member);
            return;
        }
        candidate_set_free(&candidate_set);

        if (function_has_been_deleted(decl_context, selected_operator_arrow, 
                    nodecl_get_locus(nodecl_accessed)))
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_accessed);
            nodecl_free(nodecl_member);
            return;
        }

        if (!is_pointer_to_class_type(function_type_get_return_type(selected_operator_arrow->type_information)))
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_accessed);
            nodecl_free(nodecl_member);
            return;
        }

        // Now we update the nodecl_accessed with the resulting type, this is used later when solving
        // overload in calls made using this syntax.
        type_t* t = function_type_get_return_type(selected_operator_arrow->type_information);

        // The accessed type is the pointed type
        accessed_type = lvalue_ref(pointer_type_get_pointee_type(no_ref(t)));

        // a -> b becomes (*(a.operator->())).b
        // here we are building *(a.operator->())
        nodecl_accessed_out = 
            nodecl_make_dereference(
                    cxx_nodecl_make_function_call(
                        nodecl_make_symbol(selected_operator_arrow, nodecl_get_locus(nodecl_accessed)),
                        /* called name */ nodecl_null(),
                        nodecl_make_list_1(nodecl_accessed),
                        // Ideally this should be binary infix but this call does not fit in any cathegory
                        /* function form */ nodecl_null(), 
                        t,
                        decl_context,
                        nodecl_get_locus(nodecl_accessed)),
                    pointer_type_get_pointee_type(t), nodecl_get_locus(nodecl_accessed));
    }

    if (IS_CXX_LANGUAGE
            && (is_scalar_type(no_ref(accessed_type)) || is_class_type(no_ref(accessed_type)))
            && is_pseudo_destructor_id(decl_context, no_ref(accessed_type), nodecl_member))
    {
        if (check_expr_flags.must_be_constant)
        {
            error_printf("%s: error: pseudo-destructor call in constant-expression\n",
                    locus_to_str(locus));
        }
        *nodecl_output = nodecl_make_pseudo_destructor_name(
                nodecl_accessed_out,
                get_pseudo_destructor_call_type(),
                locus);
        return;
    }
    else if (!is_class_type(no_ref(accessed_type)))
    {
        error_printf("%s: error: '%s%s' cannot be applied to '%s' (of type '%s')\n",
                nodecl_locus_to_str(nodecl_accessed),
                operator_arrow ? "->" : ".",
                codegen_to_str(nodecl_member, nodecl_retrieve_context(nodecl_member)),
                codegen_to_str(nodecl_accessed, nodecl_retrieve_context(nodecl_accessed)),
                print_type_str(no_ref(accessed_type), decl_context));

        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_accessed);
        nodecl_free(nodecl_member);
        return;
    }

    // Preserve the accessed type for lvalueness checks later
    type_t* orig_accessed_type = accessed_type;
    // Advance over all typedefs the accessed type
    accessed_type = advance_over_typedefs(no_ref(accessed_type));

    field_path_t field_path;
    field_path_init(&field_path);
    scope_entry_list_t* entry_list = get_member_of_class_type_nodecl(
            decl_context,
            accessed_type,
            nodecl_member,
            &field_path);

    if (entry_list == NULL)
    {
        error_printf("%s: error: '%s' is not a member/field of type '%s'\n",
                nodecl_locus_to_str(nodecl_member),
                codegen_to_str(nodecl_member, nodecl_retrieve_context(nodecl_member)),
                print_type_str(no_ref(accessed_type), decl_context));
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_accessed);
        nodecl_free(nodecl_member);
        return;
    }

    cv_qualifier_t cv_accessed = CV_NONE;
    advance_over_typedefs_with_cv_qualif(no_ref(orig_accessed_type), &cv_accessed);

    char ok = 0;
    scope_entry_t* orig_entry = entry_list_head(entry_list);
    scope_entry_t* entry = entry_advance_aliases(orig_entry);
    C_LANGUAGE()
    {
        nodecl_t nodecl_field = nodecl_accessed_out;
        const_value_t* current_const_value = nodecl_get_constant(nodecl_field);
        type_t* current_const_value_type = accessed_type;

        if (symbol_entity_specs_get_is_member_of_anonymous(entry))
        {
            nodecl_t accessor = symbol_entity_specs_get_anonymous_accessor(entry);
            nodecl_field = cxx_integrate_field_accesses(nodecl_field, accessor);
        }

        if (check_expr_flags.must_be_constant)
        {
            error_printf("%s: error: class member access in a constant expression\n",
                    nodecl_locus_to_str(nodecl_field));
        }
        else
        {
            cv_qualifier_t cv_field = CV_NONE;
            advance_over_typedefs_with_cv_qualif(entry->type_information, &cv_field);
            cv_field = cv_accessed | cv_field;

            ok = 1;

            *nodecl_output = nodecl_make_class_member_access(
                    nodecl_field,
                    nodecl_make_symbol(entry, nodecl_get_locus(nodecl_accessed)),
                    /* member form */ nodecl_null(),
                    lvalue_ref(get_cv_qualified_type(no_ref(entry->type_information), cv_field)),
                    nodecl_get_locus(nodecl_accessed));

            const_value_t* subconstant_value = compute_subconstant_of_class_member_access(
                    current_const_value,
                    current_const_value_type,
                    /* subobject */ NULL,
                    orig_entry);
            nodecl_set_constant(*nodecl_output, subconstant_value);
        }
    }

    CXX_LANGUAGE()
    {
        if (entry->kind == SK_VARIABLE)
        {
            if (check_expr_flags.must_be_constant
                    && nodecl_get_kind(nodecl_accessed_out) == NODECL_SYMBOL
                    && !symbol_entity_specs_get_is_constexpr(nodecl_get_symbol(nodecl_accessed_out)))
            {
                if (IS_CXX11_LANGUAGE)
                {
                    error_printf("%s: error: class member access of non-constexpr variable '%s' in a constant expression\n",
                            nodecl_locus_to_str(nodecl_accessed_out),
                            codegen_to_str(nodecl_accessed_out, decl_context));
                }
                else
                {
                    error_printf("%s: error: class member access in a constant expression\n",
                            nodecl_locus_to_str(nodecl_accessed_out));
                }
            }
            else
            {
                ok = 1;

                type_t* type_of_class_member_access = entry->type_information;
                if (is_lvalue_reference_type(entry->type_information))
                {
                    // Already OK
                }
                else if (is_rvalue_reference_type(entry->type_information))
                {
                    // T&& -> T&
                    type_of_class_member_access = get_lvalue_reference_type(no_ref(entry->type_information));
                }
                else
                {
                    // Not a reference, two cases for nonstatic/static
                    if (!symbol_entity_specs_get_is_static(entry))
                    {
                        // Combine both qualifiers
                        cv_qualifier_t cv_field = CV_NONE;
                        advance_over_typedefs_with_cv_qualif(entry->type_information, &cv_field);
                        cv_field = cv_accessed | cv_field;

                        if (symbol_entity_specs_get_is_mutable(entry))
                        {
                            cv_field &= ~CV_CONST;
                        }

                        type_of_class_member_access = get_cv_qualified_type(type_of_class_member_access, cv_field);

                        if (is_lvalue_reference_type(orig_accessed_type))
                        {
                            type_of_class_member_access = get_lvalue_reference_type(type_of_class_member_access);
                        }
                    }
                    else
                    {
                        type_of_class_member_access = get_lvalue_reference_type(type_of_class_member_access);
                    }
                }

                // Now integrate every item in the field_path skipping the first
                // (which is the class type itself) and the last (the accessed subobject
                nodecl_t nodecl_base_access = nodecl_accessed_out;

                const_value_t* current_const_value = nodecl_get_constant(nodecl_base_access);
                type_t* current_const_value_type = accessed_type;

                ERROR_CONDITION(field_path.length > 1, "Unexpected length for field path", 0);
                if (field_path.length == 1)
                {
                    nodecl_base_access = nodecl_make_class_member_access(
                            nodecl_base_access,
                            nodecl_make_symbol(field_path.path[0], nodecl_get_locus(nodecl_accessed)),
                            nodecl_null(),
                            get_user_defined_type(field_path.path[0]),
                            nodecl_get_locus(nodecl_accessed));
                }

                // Integrate also the anonymous accesses
                if (symbol_entity_specs_get_is_member_of_anonymous(entry))
                {
                    nodecl_t accessor = symbol_entity_specs_get_anonymous_accessor(entry);
                    nodecl_base_access = cxx_integrate_field_accesses(nodecl_base_access, accessor);
                }

                *nodecl_output = nodecl_make_class_member_access(
                        nodecl_base_access,
                        nodecl_make_symbol(orig_entry, nodecl_get_locus(nodecl_accessed)),
                        /* member literal */ nodecl_shallow_copy(nodecl_member),
                        type_of_class_member_access,
                        nodecl_get_locus(nodecl_accessed));

                const_value_t* subconstant_value = compute_subconstant_of_class_member_access(
                        current_const_value,
                        current_const_value_type,
                        field_path.length == 1 ? field_path.path[0] : NULL,
                        orig_entry);
                nodecl_set_constant(*nodecl_output, subconstant_value);
            }
        }
        else if (entry->kind == SK_ENUMERATOR)
        {
            ok = 1;

            *nodecl_output = nodecl_make_symbol(orig_entry, nodecl_get_locus(nodecl_accessed));
            nodecl_set_type(*nodecl_output, entry->type_information);

            if (nodecl_expr_is_value_dependent(entry->value))
            {
                DEBUG_CODE()
                {
                    fprintf(stderr, "EXPRTYPE: Found '%s' at '%s' to be value dependent\n",
                            nodecl_locus_to_str(nodecl_accessed), codegen_to_str(nodecl_accessed, nodecl_retrieve_context(nodecl_accessed)));
                }
                nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
            }
            else
            {
                ERROR_CONDITION(!nodecl_is_constant(entry->value), "This should be constant", 0);
                nodecl_set_constant(*nodecl_output, nodecl_get_constant(entry->value));
            }

        }
        // In C++ if we have overload remember it
        else if (entry->kind == SK_FUNCTION
                || entry->kind == SK_TEMPLATE)
        {
            template_parameter_list_t* last_template_args = NULL;
            if (nodecl_name_ends_in_template_id(nodecl_member))
            {
                last_template_args = nodecl_name_get_last_template_arguments(nodecl_member);
            }

            type_t* t = get_unresolved_overloaded_type(entry_list, last_template_args);


            // Note that we do not store anything from the field_path as we
            // will have to reconstruct the accessed subobject when building the
            // function call

            if (last_template_args != NULL
                    && has_dependent_template_parameters(last_template_args))
            {
                // A case like this
                //
                // struct A
                // {
                //    template <typename T>
                //    void f()
                //    {
                //       this->template g<T>(3);
                //    }
                // };
                //
                // Nothing is dependent but the nodecl_member
                *nodecl_output = nodecl_make_cxx_class_member_access(
                        nodecl_accessed_out,
                        nodecl_member,
                        get_unknown_dependent_type(),
                        locus);

                nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
            }
            else
            {
                *nodecl_output = nodecl_make_class_member_access(
                        nodecl_accessed_out,
                        /* This symbol goes unused when we see that its type is already an overload */
                        nodecl_make_symbol(orig_entry, nodecl_get_locus(nodecl_accessed)),
                        /* member literal */ nodecl_shallow_copy(nodecl_member),
                        t,
                        nodecl_get_locus(nodecl_accessed));
            }

            ok = 1;
        }
    }

    entry_list_free(entry_list);

    if (!ok)
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_accessed);
        nodecl_free(nodecl_member);
    }
}

static void check_member_access(AST member_access, decl_context_t decl_context, char is_arrow, char has_template_tag, nodecl_t* nodecl_output)
{
    AST class_expr = ASTSon0(member_access);
    AST id_expression = ASTSon1(member_access);

    nodecl_t nodecl_accessed = nodecl_null();
    check_expression_impl_(class_expr, decl_context, &nodecl_accessed);

    if (nodecl_is_err_expr(nodecl_accessed))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(member_access));
        return;
    }

    nodecl_t nodecl_name = nodecl_null();

    compute_nodecl_name_from_id_expression(id_expression, decl_context, &nodecl_name);
    if (nodecl_is_err_expr(nodecl_name))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(member_access));
        return;
    }

    check_nodecl_member_access(nodecl_accessed, nodecl_name, decl_context, is_arrow, has_template_tag,
            ast_get_locus(member_access),
            nodecl_output);
}

static void check_qualified_id(AST expr, decl_context_t decl_context, nodecl_t *nodecl_output)
{
    cxx_common_name_check(expr, decl_context, nodecl_output);

    if (nodecl_get_kind(*nodecl_output) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED
            || nodecl_get_kind(*nodecl_output) == NODECL_CXX_DEP_NAME_NESTED)
    {
        nodecl_t nodecl_last_part =  nodecl_name_get_last_part(*nodecl_output);
        if (nodecl_get_kind(nodecl_last_part) == NODECL_CXX_DEP_NAME_CONVERSION)
        {
            char ok = compute_type_of_dependent_conversion_type_id(nodecl_last_part,
                    decl_context);
            if (!ok)
            {
                nodecl_t nodecl_name = *nodecl_output;
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
                nodecl_free(nodecl_name);
                return;
            }
        }
    }
}

// This checks that a template-id-expr is feasible in an expression
void check_template_id_expr(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    cxx_common_name_check(expr, decl_context, nodecl_output);
}

static void check_postoperator_user_defined(
        AST operator, 
        nodecl_t postoperated_expr, 
        decl_context_t decl_context,
        scope_entry_list_t* builtins,
        nodecl_t (*nodecl_fun)(nodecl_t, type_t*, const locus_t*),
        nodecl_t* nodecl_output)
{
    type_t* incremented_type = nodecl_get_type(postoperated_expr);

    type_t* argument_types[2] = {
        incremented_type, // Member argument
        get_zero_type(get_signed_int_type()) // Postoperation
    };
    int num_arguments = 2;

    candidate_t* candidate_set = NULL;

    scope_entry_list_t* operator_overload_set = NULL;
    if (is_class_type(no_ref(incremented_type)))
    {
        scope_entry_list_t *operator_entry_list = get_member_of_class_type(no_ref(incremented_type),
                operator, decl_context, NULL);

        operator_overload_set = unfold_and_mix_candidate_functions(operator_entry_list,
                NULL, argument_types + 1, num_arguments - 1,
                decl_context,
                nodecl_get_locus(postoperated_expr),
                /* explicit_template_arguments */ NULL);
        entry_list_free(operator_entry_list);
    }

    // We need to do koenig lookup for non-members
    // otherwise some overloads might not be found
    nodecl_t nodecl_op_name = 
        nodecl_make_cxx_dep_name_simple(
                get_operator_function_name(operator),
                nodecl_get_locus(postoperated_expr));
    scope_entry_list_t *entry_list = koenig_lookup(num_arguments,
            argument_types, decl_context, nodecl_op_name, nodecl_get_locus(postoperated_expr));

    scope_entry_list_t* overload_set = unfold_and_mix_candidate_functions(entry_list,
            builtins, argument_types, num_arguments,
            decl_context,
            nodecl_get_locus(postoperated_expr), /* explicit_template_arguments */ NULL);
    entry_list_free(entry_list);

    scope_entry_list_t* old_overload_set = overload_set;
    overload_set = entry_list_merge(old_overload_set, operator_overload_set);
    entry_list_free(old_overload_set);
    entry_list_free(operator_overload_set);

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(overload_set);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        candidate_set = candidate_set_add(candidate_set,
                entry_list_iterator_current(it),
                num_arguments,
                argument_types);
    }
    entry_list_iterator_free(it);
    entry_list_free(overload_set);

    scope_entry_t* orig_overloaded_call = solve_overload(candidate_set,
            decl_context, 
            nodecl_get_locus(postoperated_expr));
    scope_entry_t* overloaded_call = entry_advance_aliases(orig_overloaded_call);

    if (overloaded_call == NULL)
    {
        error_message_overload_failed(candidate_set, 
                get_operator_function_name(operator),
                decl_context,
                num_arguments, argument_types,
                /* implicit_argument */ NULL,
                nodecl_get_locus(postoperated_expr));
        *nodecl_output = nodecl_make_err_expr(
                nodecl_get_locus(postoperated_expr));
        candidate_set_free(&candidate_set);
        nodecl_free(postoperated_expr);
        return;
    }
    candidate_set_free(&candidate_set);

    if (function_has_been_deleted(decl_context, overloaded_call, 
                nodecl_get_locus(postoperated_expr)))
    {
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(postoperated_expr));
        nodecl_free(postoperated_expr);
        return;
    }

    if (!symbol_entity_specs_get_is_member(overloaded_call))
    {
        type_t* param_type = function_type_get_parameter_type_num(overloaded_call->type_information, 0);

        nodecl_t old_post = postoperated_expr;
        check_nodecl_function_argument_initialization(postoperated_expr, decl_context, param_type,
                /* disallow_narrowing */ 0,
                &postoperated_expr);
        if (nodecl_is_err_expr(postoperated_expr))
        {
            nodecl_free(old_post);
            *nodecl_output = postoperated_expr;
            return;
        }
    }

    if (symbol_entity_specs_get_is_builtin(overloaded_call))
    {
        *nodecl_output = nodecl_fun(
                postoperated_expr,
                function_type_get_return_type(overloaded_call->type_information), 
                nodecl_get_locus(postoperated_expr));
    }
    else
    {
        *nodecl_output =
            cxx_nodecl_make_function_call(
                    nodecl_make_symbol(overloaded_call, 
                        nodecl_get_locus(postoperated_expr)),
                    /* called name */ nodecl_null(),
                    nodecl_make_list_2(/* 0 */
                        postoperated_expr,
                        const_value_to_nodecl(const_value_get_zero(type_get_size(get_signed_int_type()), /* signed */ 1))),
                    nodecl_make_cxx_function_form_unary_postfix(
                        nodecl_get_locus(postoperated_expr)),
                    function_type_get_return_type(overloaded_call->type_information),
                    decl_context,
                    nodecl_get_locus(postoperated_expr));
    }
}

static void check_preoperator_user_defined(AST operator, 
        nodecl_t preoperated_expr,
        decl_context_t decl_context,
        scope_entry_list_t* builtins,
        nodecl_t (*nodecl_fun)(nodecl_t, type_t*, const locus_t*),
        nodecl_t* nodecl_output)
{
    type_t* incremented_type = nodecl_get_type(preoperated_expr);

    type_t* argument_types[1] = {
        incremented_type, 
    };
    int num_arguments = 1;

    candidate_t* candidate_set = NULL;

    scope_entry_list_t* operator_overload_set = NULL;
    if (is_class_type(no_ref(incremented_type)))
    {
        scope_entry_list_t *operator_entry_list = get_member_of_class_type(no_ref(incremented_type),
                operator, decl_context, NULL);

        operator_overload_set = unfold_and_mix_candidate_functions(operator_entry_list,
                NULL, argument_types + 1, num_arguments - 1,
                decl_context,
                nodecl_get_locus(preoperated_expr),
                /* explicit_template_arguments */ NULL);
        entry_list_free(operator_entry_list);
    }

    nodecl_t nodecl_op_name = 
        nodecl_make_cxx_dep_name_simple(
                get_operator_function_name(operator),
                nodecl_get_locus(preoperated_expr));
    scope_entry_list_t *entry_list = koenig_lookup(num_arguments,
            argument_types, decl_context, nodecl_op_name, nodecl_get_locus(preoperated_expr));

    scope_entry_list_t* overload_set = unfold_and_mix_candidate_functions(
            entry_list, builtins, argument_types, num_arguments,
            decl_context,
            nodecl_get_locus(preoperated_expr), /* explicit_template_arguments */ NULL);
    entry_list_free(entry_list);

    scope_entry_list_t* old_overload_set = overload_set;
    overload_set = entry_list_merge(old_overload_set, operator_overload_set);
    entry_list_free(old_overload_set);
    entry_list_free(operator_overload_set);

    scope_entry_list_iterator_t *it = NULL;
    for (it = entry_list_iterator_begin(overload_set);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        candidate_set = candidate_set_add(candidate_set,
                entry_list_iterator_current(it),
                num_arguments,
                argument_types);
    }
    entry_list_iterator_free(it);
    entry_list_free(overload_set);

    scope_entry_t* orig_overloaded_call = solve_overload(candidate_set,
            decl_context, nodecl_get_locus(preoperated_expr));
    scope_entry_t* overloaded_call = entry_advance_aliases(orig_overloaded_call);

    if (overloaded_call == NULL)
    {
        error_message_overload_failed(candidate_set, 
                get_operator_function_name(operator),
                decl_context,
                num_arguments, argument_types,
                /* implicit_argument */ NULL,
                nodecl_get_locus(preoperated_expr));
        candidate_set_free(&candidate_set);
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(preoperated_expr));
        nodecl_free(preoperated_expr);
        return;
    }
    candidate_set_free(&candidate_set);

    if (function_has_been_deleted(decl_context, overloaded_call, nodecl_get_locus(preoperated_expr)))
    {
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(preoperated_expr));
        nodecl_free(preoperated_expr);
        return;
    }

    if (!symbol_entity_specs_get_is_member(overloaded_call))
    {
        type_t* param_type = function_type_get_parameter_type_num(overloaded_call->type_information, 0);

        nodecl_t old_pre = preoperated_expr;
        check_nodecl_function_argument_initialization(preoperated_expr, decl_context, param_type,
                /* disallow_narrowing */ 0,
                &preoperated_expr);
        if (nodecl_is_err_expr(preoperated_expr))
        {
            nodecl_free(old_pre);
            *nodecl_output = preoperated_expr;
            return;
        }
    }

    if (symbol_entity_specs_get_is_builtin(overloaded_call))
    {
        *nodecl_output = 
                nodecl_fun(
                    preoperated_expr,
                    function_type_get_return_type(overloaded_call->type_information), 
                    nodecl_get_locus(preoperated_expr));
    }
    else
    {
        *nodecl_output = 
            cxx_nodecl_make_function_call(
                    nodecl_make_symbol(overloaded_call, nodecl_get_locus(preoperated_expr)),
                    /* called name */ nodecl_null(),
                    nodecl_make_list_1(preoperated_expr),
                    nodecl_make_cxx_function_form_unary_prefix(nodecl_get_locus(preoperated_expr)),
                    function_type_get_return_type(overloaded_call->type_information), 
                    decl_context,
                    nodecl_get_locus(preoperated_expr));
    }
    return;
}

static char postoperator_incr_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_lvalue_reference_type(lhs)
            && (is_arithmetic_type(reference_type_get_referenced_type(lhs))
                || is_pointer_type(reference_type_get_referenced_type(lhs)))
            && !is_const_qualified_type(reference_type_get_referenced_type(lhs))
            && is_zero_type_or_nullptr_type(rhs));
}

static char postoperator_decr_pred(type_t* lhs, type_t* rhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_lvalue_reference_type(lhs)
            && (is_arithmetic_type(reference_type_get_referenced_type(lhs))
                || is_pointer_type(reference_type_get_referenced_type(lhs)))
            && !is_bool_type(reference_type_get_referenced_type(lhs))
            && !is_const_qualified_type(reference_type_get_referenced_type(lhs))
            && is_zero_type_or_nullptr_type(rhs));
}

static type_t* postoperator_result(type_t** lhs, 
        type_t** rhs UNUSED_PARAMETER, // This one holds a zero type
        const locus_t* locus UNUSED_PARAMETER
        )
{
    // a++ returns a value, not a reference
    type_t* result = get_unqualified_type(no_ref(*lhs));
    return result;
}

static void check_nodecl_postoperator(AST operator, 
        nodecl_t postoperated_expr, 
        decl_context_t decl_context, char is_decrement,
        nodecl_t (*nodecl_fun)(nodecl_t, type_t*, const locus_t*),
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(postoperated_expr);
    if (nodecl_is_err_expr(postoperated_expr))
    {
        *nodecl_output = nodecl_make_err_expr(
                nodecl_get_locus(postoperated_expr));
        nodecl_free(postoperated_expr);
        return;
    }

    if (nodecl_expr_is_type_dependent(postoperated_expr))
    {
        *nodecl_output = nodecl_fun(postoperated_expr,
                get_unknown_dependent_type(),
                nodecl_get_locus(postoperated_expr));
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        nodecl_expr_set_is_value_dependent(*nodecl_output,
                nodecl_expr_is_value_dependent(postoperated_expr));
        return;
    }

    type_t* operated_type = nodecl_get_type(postoperated_expr);

    char requires_overload = 0;

    CXX_LANGUAGE()
    {
        requires_overload = is_class_type(no_ref(operated_type))
            || is_enum_type(no_ref(operated_type));
    }

    if (!requires_overload)
    {
        if (is_pointer_type(no_ref(operated_type))
                || is_arithmetic_type(no_ref(operated_type)))
        {
            // Should be an lvalue
            if (!is_lvalue_reference_type(operated_type))
            {
                error_printf("%s: error: operand '%s' of %s is not an lvalue\n",
                        nodecl_locus_to_str(postoperated_expr),
                        codegen_to_str(postoperated_expr, decl_context),
                        is_decrement ? "postdecrement" : "postincrement");
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(postoperated_expr));
                nodecl_free(postoperated_expr);
                return;
            }
            if (is_const_qualified_type(no_ref(operated_type)))
            {
                error_printf("%s: error: operand '%s' of %s is read-only\n",
                        nodecl_locus_to_str(postoperated_expr),
                        codegen_to_str(postoperated_expr, decl_context),
                        is_decrement ? "postdecrement" : "postincrement");
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(postoperated_expr));
                nodecl_free(postoperated_expr);
                return;
            }

            operated_type = reference_type_get_referenced_type(operated_type);

            *nodecl_output = nodecl_fun(postoperated_expr,
                        operated_type,
                        nodecl_get_locus(postoperated_expr));
            return;
        }
        else
        {
            error_printf("%s: error: type '%s' is not valid for %s operator\n",
                    nodecl_locus_to_str(postoperated_expr),
                    print_type_str(no_ref(operated_type), decl_context),
                    is_decrement ? "postdecrement" : "postincrement");
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(postoperated_expr));
            nodecl_free(postoperated_expr);
            return;
        }
    }

    char (*postoperator_pred)(type_t*, type_t*, const locus_t* locus);
    if (is_decrement)
    {
        postoperator_pred = postoperator_decr_pred;
    }
    else
    {
        postoperator_pred = postoperator_incr_pred;
    }

    builtin_operators_set_t builtin_set; 
    build_binary_builtin_operators(
            // Note that we do not remove the left reference
            operated_type, 
            // This is the 0 argument of operator++(T&, 0)
            get_zero_type(get_signed_int_type()),
            &builtin_set,
            decl_context, operator,
            postoperator_pred,
            postoperator_result,
            locus);

    scope_entry_list_t* builtins = get_entry_list_from_builtin_operator_set(&builtin_set);

    // Only C++ after this point
    check_postoperator_user_defined(operator, 
            postoperated_expr, decl_context, builtins, nodecl_fun, nodecl_output);

    entry_list_free(builtins);
}

static char preoperator_incr_pred(type_t* lhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_lvalue_reference_type(lhs)
            && (is_arithmetic_type(reference_type_get_referenced_type(lhs))
                || is_pointer_type(reference_type_get_referenced_type(lhs)))
            && !is_const_qualified_type(reference_type_get_referenced_type(lhs)));
}

static char preoperator_decr_pred(type_t* lhs, const locus_t* locus UNUSED_PARAMETER)
{
    return (is_lvalue_reference_type(lhs)
            && (is_arithmetic_type(reference_type_get_referenced_type(lhs))
                || is_pointer_type(reference_type_get_referenced_type(lhs)))
            && !is_bool_type(reference_type_get_referenced_type(lhs))
            && !is_const_qualified_type(reference_type_get_referenced_type(lhs)));
}

static type_t* preoperator_result(type_t** lhs, const locus_t* locus UNUSED_PARAMETER)
{
    return *lhs;
}

static void check_nodecl_preoperator(AST operator, 
        nodecl_t preoperated_expr, decl_context_t decl_context,
        char is_decrement, nodecl_t (*nodecl_fun)(nodecl_t, type_t*, const locus_t*),
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(preoperated_expr);
    if (nodecl_is_err_expr(preoperated_expr))
    {
        *nodecl_output = nodecl_make_err_expr(
                nodecl_get_locus(preoperated_expr));
        nodecl_free(preoperated_expr);
        return;
    }

    if (nodecl_expr_is_type_dependent(preoperated_expr))
    {
        *nodecl_output = nodecl_fun(preoperated_expr, 
                get_unknown_dependent_type(), 
                nodecl_get_locus(preoperated_expr));
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        nodecl_expr_set_is_value_dependent(*nodecl_output,
                nodecl_expr_is_value_dependent(preoperated_expr));
        return;
    }

    type_t* operated_type = nodecl_get_type(preoperated_expr);

    if (is_pointer_type(no_ref(operated_type))
            || is_arithmetic_type(no_ref(operated_type)))
    {
        // Should be an lvalue
        if (!is_lvalue_reference_type(operated_type))
        {
            error_printf("%s: error: operand '%s' of %s is not an lvalue\n",
                    nodecl_locus_to_str(preoperated_expr),
                    codegen_to_str(preoperated_expr, decl_context),
                    is_decrement ? "predecrement" : "preincrement");
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(preoperated_expr));
            nodecl_free(preoperated_expr);
            return;
        }

        if (is_const_qualified_type(no_ref(operated_type)))
        {
            error_printf("%s: error: operand '%s' of %s is read-only\n",
                    nodecl_locus_to_str(preoperated_expr),
                    codegen_to_str(preoperated_expr, decl_context),
                    is_decrement ? "predecrement" : "preincrement");
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(preoperated_expr));
            nodecl_free(preoperated_expr);
            return;
        }

        *nodecl_output = nodecl_fun(preoperated_expr,
                operated_type, 
                nodecl_get_locus(preoperated_expr));
        return;
    }
    else
    {
        C_LANGUAGE()
        {
            error_printf("%s: error: type '%s' is not valid for %s operator\n",
                    nodecl_locus_to_str(preoperated_expr),
                    print_type_str(no_ref(operated_type), decl_context),
                    is_decrement ? "predecrement" : "preincrement");
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(preoperated_expr));
            nodecl_free(preoperated_expr);
            return;
        }
    }

    char (*preoperator_pred)(type_t*, const locus_t* locus);
    if (is_decrement)
    {
        preoperator_pred = preoperator_decr_pred;
    }
    else
    {
        preoperator_pred = preoperator_incr_pred;
    }

    builtin_operators_set_t builtin_set;
    build_unary_builtin_operators(
            // Note that we do not remove the left reference
            operated_type, 
            &builtin_set,
            decl_context, operator, 
            preoperator_pred,
            preoperator_result,
            locus);

    scope_entry_list_t* builtins = get_entry_list_from_builtin_operator_set(&builtin_set);

    // Only C++ after this point
    check_preoperator_user_defined(operator, 
            preoperated_expr, 
            decl_context, 
            builtins, 
            nodecl_fun,
            nodecl_output);

    entry_list_free(builtins);

    return;
}

static void check_nodecl_postincrement(
        nodecl_t nodecl_postincremented,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_INCREMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    check_nodecl_postoperator(operation_tree,
            nodecl_postincremented,
            decl_context, /* is_decr */ 0,
            nodecl_make_postincrement, nodecl_output);
}

static void check_postincrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // In C++
    //
    // 'e++' is either 'operator++(e, 0)' or 'e.operator++(0)'
    //
    AST postincremented_expr = ASTSon0(expr);

    nodecl_t nodecl_postincremented = nodecl_null();
    check_expression_impl_(postincremented_expr, decl_context, &nodecl_postincremented);

    check_nodecl_postincrement(
            nodecl_postincremented,
            decl_context,
            nodecl_output);
}

static void check_nodecl_postdecrement(nodecl_t nodecl_postdecremented,
    decl_context_t decl_context, nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_DECREMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    check_nodecl_postoperator(operation_tree,
            nodecl_postdecremented,
            decl_context, /* is_decr */ 1,
            nodecl_make_postdecrement, nodecl_output);
}

static void check_postdecrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // In C++
    //
    // 'e--' is either 'operator--(e, 0)' or 'e.operator--(0)'
    //
    AST postdecremented_expr = ASTSon0(expr);

    nodecl_t nodecl_postdecremented = nodecl_null();
    check_expression_impl_(postdecremented_expr, decl_context, &nodecl_postdecremented);

    check_nodecl_postdecrement(
            nodecl_postdecremented,
            decl_context,
            nodecl_output);
}

static void check_nodecl_preincrement(nodecl_t nodecl_preincremented,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_INCREMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    check_nodecl_preoperator(operation_tree,
            nodecl_preincremented,
            decl_context,
            /* is_decr */ 0,
            nodecl_make_preincrement,
            nodecl_output);
}

static void check_preincrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // In C++
    //
    // '++e' is either 'operator++(e)' or 'e.operator++()'
    //
    AST preincremented_expr = ASTSon0(expr);

    nodecl_t nodecl_preincremented = nodecl_null();
    check_expression_impl_(preincremented_expr, decl_context, &nodecl_preincremented);

    check_nodecl_preincrement(nodecl_preincremented, decl_context, nodecl_output);
}

static void check_nodecl_predecrement(
        nodecl_t nodecl_predecremented,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_DECREMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    check_nodecl_preoperator(operation_tree,
            nodecl_predecremented,
            decl_context,
            /* is_decr */ 1,
            nodecl_make_predecrement,
            nodecl_output);
}

static void check_predecrement(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // In C++
    //
    //
    // '--e' is either 'operator--(e)' or 'e.operator--()'
    //
    AST predecremented_expr = ASTSon0(expr);

    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_DECREMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    nodecl_t nodecl_predecremented = nodecl_null();
    check_expression_impl_(predecremented_expr, decl_context, &nodecl_predecremented);

    check_nodecl_predecrement(nodecl_predecremented, decl_context, nodecl_output);
}

static scope_entry_t* get_typeid_symbol(decl_context_t decl_context, const locus_t* locus)
{
    // Lookup for 'std::type_info'
    static scope_entry_t* typeid_sym = NULL;

    // FIXME: This will last accross files
    if (typeid_sym == NULL)
    {
        decl_context_t global_context = decl_context;
        global_context.current_scope = global_context.global_scope;

        scope_entry_list_t* entry_list = query_in_scope_str(global_context, UNIQUESTR_LITERAL("std"), NULL);

        if (entry_list == NULL 
                || entry_list_head(entry_list)->kind != SK_NAMESPACE)
        {
            if (entry_list != NULL)
                entry_list_free(entry_list);

            error_printf("%s: error: namespace 'std' not found when looking up 'std::type_info'. \n"
                    "%s: info: maybe you need '#include <typeinfo>'\n",
                    locus_to_str(locus),
                    locus_to_str(locus));
            return NULL;
        }

        decl_context_t std_context = entry_list_head(entry_list)->related_decl_context;
        entry_list_free(entry_list);
        entry_list = query_in_scope_str(std_context, UNIQUESTR_LITERAL("type_info"), NULL);

        if (entry_list == NULL
                || (entry_list_head(entry_list)->kind != SK_CLASS
                    && entry_list_head(entry_list)->kind != SK_TYPEDEF))
        {
            if (entry_list != NULL)
                entry_list_free(entry_list);

            error_printf("%s: error: namespace 'std' not found when looking up 'std::type_info'. \n"
                    "%s: info: maybe you need '#include <typeinfo>'\n",
                    locus_to_str(locus),
                    locus_to_str(locus));
            return NULL;
        }

        typeid_sym = entry_list_head(entry_list);
        entry_list_free(entry_list);
    }

    return typeid_sym;
}

scope_entry_t* get_std_initializer_list_template(decl_context_t decl_context, 
        const locus_t* locus, 
        char mandatory)
{
    // Lookup for 'std::initializer_list'
    scope_entry_t* result = NULL;

    decl_context_t global_context = decl_context;
    global_context.current_scope = global_context.global_scope;

    scope_entry_list_t* entry_list = query_in_scope_str(global_context, UNIQUESTR_LITERAL("std"), NULL);

    if (entry_list == NULL 
            || entry_list_head(entry_list)->kind != SK_NAMESPACE)
    {
        if (entry_list != NULL)
            entry_list_free(entry_list);
        if (!mandatory)
            return NULL;

        error_printf("%s: error: namespace 'std' not found when looking up 'std::initializer_list'\n"
                "%s: info: maybe you need '#include <initializer_list>'\n",
                locus_to_str(locus),
                locus_to_str(locus));
        return NULL;
    }

    decl_context_t std_context = entry_list_head(entry_list)->related_decl_context;
    entry_list_free(entry_list);

    entry_list = query_in_scope_str(std_context, UNIQUESTR_LITERAL("initializer_list"), NULL);

    if (entry_list == NULL
            || entry_list_head(entry_list)->kind != SK_TEMPLATE)
    {
        if (entry_list != NULL)
            entry_list_free(entry_list);
        if (!mandatory)
            return NULL;

        error_printf("%s: error: template-name 'initializer_list' not found when looking up 'std::initializer_list'\n"
                "%s: info: maybe you need '#include <initializer_list>'\n",
                locus_to_str(locus),
                locus_to_str(locus));
        return NULL;
    }

    result = entry_list_head(entry_list);
    entry_list_free(entry_list);

    return result;
}

static void check_nodecl_typeid_type(type_t* t, 
        decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    scope_entry_t* typeid_type_class = get_typeid_symbol(decl_context, locus);
    if (typeid_type_class == NULL)
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    type_t* typeid_type = lvalue_ref(get_const_qualified_type(get_user_defined_type(typeid_type_class)));

    nodecl_t nodecl_type = nodecl_make_type(t, locus);
    if (is_dependent_type(t))
    {
        nodecl_expr_set_is_type_dependent(nodecl_type, 1);
    }

    *nodecl_output = nodecl_make_typeid(
            nodecl_type,
            typeid_type, locus);
}

static void check_typeid_type(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    type_t* type = compute_type_for_type_id_tree(ASTSon0(expr), decl_context,
            /* out_simple_type */ NULL, /* out_gather_info */ NULL);

    if (is_error_type(type))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expr));
        return;
    }

    check_nodecl_typeid_type(type, decl_context, ast_get_locus(expr), nodecl_output);
}

static void check_nodecl_typeid_expr(nodecl_t nodecl_typeid_expr, 
        decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (nodecl_is_err_expr(nodecl_typeid_expr))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_typeid_expr);
        return;
    }

    scope_entry_t* typeid_type_class = get_typeid_symbol(decl_context, locus);
    if (typeid_type_class == NULL)
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_typeid_expr);
        return;
    }

    type_t* typeid_type = lvalue_ref(get_const_qualified_type(get_user_defined_type(typeid_type_class)));

    *nodecl_output = nodecl_make_typeid(
            nodecl_typeid_expr,
            typeid_type, locus);
}

static void check_typeid_expr(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_typeid_expr = nodecl_null();
    check_expression_impl_(ASTSon0(expr), decl_context, &nodecl_typeid_expr);

    check_nodecl_typeid_expr(nodecl_typeid_expr, decl_context, 
            ast_get_locus(expr), 
            nodecl_output);
}

struct type_init_stack_t
{
    type_t* type;
    int item; // index of array or ith-member
    int num_items; // size of the array or number of members in this struct
    int max_item; // arrays only: maximum index seen so far
    // For classes only
    scope_entry_t** fields;
    // To compute the constant value
    int num_values; // will be num_items unless initializing an unbound array
                    // if num_values < 0 it means that the whole braced initializer
                    // cannot be constant
    const_value_t** values;
};

#define MAX_ITEM(a, b) ((a) > (b) ? (a) : (b))
static char update_stack_to_designator(type_t* declared_type,
        struct type_init_stack_t *type_stack,
        int* type_stack_idx,
        nodecl_t designator_list,
        decl_context_t decl_context)
{
    int designator_list_length = 0;
    nodecl_t* designators = nodecl_unpack_list(designator_list, &designator_list_length);

    type_t* next_type = declared_type;

    int orig_type_stack_idx = *type_stack_idx;
    *type_stack_idx = -1;

    int i = 0;
    while (i < designator_list_length)
    {
        if (is_array_type(next_type))
        {
            // When returning back to this type we want it to continue _after_
            // the previously designated one
            if (*type_stack_idx > 0)
                type_stack[*type_stack_idx].item++;

            (*type_stack_idx)++;
            ERROR_CONDITION(*type_stack_idx == MCXX_MAX_UNBRACED_AGGREGATES, "Too many unbraced aggregates", 0);
            type_stack[*type_stack_idx].item = 0;
            if (*type_stack_idx > orig_type_stack_idx)
                type_stack[*type_stack_idx].max_item = 0;
            type_stack[*type_stack_idx].type = next_type;
            type_stack[*type_stack_idx].fields = NULL;

            if (!array_type_is_unknown_size(next_type))
            {
                ERROR_CONDITION(!nodecl_is_constant(array_type_get_array_size_expr(next_type)), "Invalid array type", 0);
                const_value_t* size = nodecl_get_constant(array_type_get_array_size_expr(next_type));
                type_stack[*type_stack_idx].num_items = const_value_cast_to_signed_int(size);
            }
            else
            {
                type_stack[*type_stack_idx].num_items = -1;
            }
        }
        else if (is_class_type(next_type))
        {
            // When returning back to this type we want it to continue _after_
            // the previously designated one
            if (*type_stack_idx > 0)
                type_stack[*type_stack_idx].item++;

            (*type_stack_idx)++;
            ERROR_CONDITION(*type_stack_idx == MCXX_MAX_UNBRACED_AGGREGATES, "Too many unbraced aggregates", 0);
            type_stack[*type_stack_idx].item = 0;
            type_stack[*type_stack_idx].type = next_type;
            scope_entry_list_t* fields = class_type_get_nonstatic_data_members(next_type);
            entry_list_to_symbol_array(fields, &type_stack[*type_stack_idx].fields, &type_stack[*type_stack_idx].num_items);
            entry_list_free(fields);
        }

        type_t* type_to_be_initialized = type_stack[*type_stack_idx].type;
        nodecl_t current_designator = designators[i];

        if (is_array_type(type_to_be_initialized)
                && nodecl_get_kind(current_designator) == NODECL_C99_INDEX_DESIGNATOR)
        {
            nodecl_t expr = nodecl_get_child(current_designator, 0);
            if (!nodecl_is_constant(expr))
            {
                error_printf("%s: error: index designator [%s] is not constant\n", nodecl_locus_to_str(expr),
                        codegen_to_str(expr, nodecl_retrieve_context(expr)));
            }
            else
            {
                const_value_t* designator_index = nodecl_get_constant(expr);
                if (!array_type_is_unknown_size(type_to_be_initialized))
                {
                    ERROR_CONDITION (!nodecl_is_constant(array_type_get_array_size_expr(type_to_be_initialized)), "Invalid array type", 0);
                    const_value_t* size = nodecl_get_constant(array_type_get_array_size_expr(type_to_be_initialized));
                    if (const_value_is_zero(const_value_lt(designator_index, size)))
                    {
                        warn_printf("%s: warning: index designator [%s] is out of bounds of elements of type %s\n",
                                nodecl_locus_to_str(expr),
                                codegen_to_str(expr, nodecl_retrieve_context(expr)),
                                print_type_str(type_to_be_initialized, nodecl_retrieve_context(expr)));
                    }
                }
                // Move the current item to be the one after the designated index
                type_stack[*type_stack_idx].item = const_value_cast_to_signed_int(designator_index);
                type_stack[*type_stack_idx].max_item = MAX_ITEM(type_stack[*type_stack_idx].max_item, type_stack[*type_stack_idx].item);
                // Move on to the element type only if it is an array or class
                next_type = array_type_get_element_type(type_to_be_initialized);
            }
        }
        else if (is_class_type(type_to_be_initialized)
                && nodecl_get_kind(current_designator) == NODECL_C99_FIELD_DESIGNATOR)
        {
            nodecl_t name = nodecl_get_child(current_designator, 0);
            const char* field_name = nodecl_get_text(name);

            char found = 0;
            int j;
            for (j = 0; j < type_stack[*type_stack_idx].num_items && !found; j++)
            {
                if (strcmp(type_stack[*type_stack_idx].fields[j]->symbol_name, field_name) == 0)
                {
                    found = 1;
                    // Move to that field
                    type_stack[*type_stack_idx].item = j;
                    next_type = type_stack[*type_stack_idx].fields[j]->type_information;
                }
            }

            if (!found)
            {
                // OK, maybe it is an anonymous union member, we need to perform a lookup
                nodecl_t nodecl_name = nodecl_make_cxx_dep_name_simple(field_name,
                                    nodecl_get_locus(current_designator));

                scope_entry_list_t* entry_list = get_member_of_class_type_nodecl(
                        decl_context,
                        type_to_be_initialized,
                        nodecl_name,
                        /* field_path */ NULL);

                nodecl_free(nodecl_name);

                if (entry_list != NULL)
                {
                    scope_entry_t* entry = entry_list_head(entry_list);
                    entry_list_free(entry_list);

                    if (symbol_entity_specs_get_is_member_of_anonymous(entry))
                    {
                        // If this name is a member of an anonymous union then
                        // update the accessor list
                        nodecl_t n = symbol_entity_specs_get_anonymous_accessor(entry);

                        nodecl_t* reversed_list = NULL;
                        int num_items_reversed_list = 0;

                        char done = 0;
                        while (!done)
                        {
                            nodecl_t current_name = nodecl_null();
                            if (nodecl_get_kind(n) == NODECL_SYMBOL)
                            {
                                current_name = n;
                                done = 1;
                            }
                            else if (nodecl_get_kind(n) == NODECL_CLASS_MEMBER_ACCESS)
                            {
                                current_name = nodecl_get_child(n, 1);
                                n = nodecl_get_child(n, 0);
                            }
                            else
                            {
                                internal_error("Unexpected node", 0);
                            }

                            ERROR_CONDITION(nodecl_get_kind(current_name) != NODECL_SYMBOL,
                                    "Invalid node", 0);

                            P_LIST_ADD(reversed_list,
                                    num_items_reversed_list,
                                    nodecl_make_c99_field_designator(
                                        nodecl_make_cxx_dep_name_simple(
                                            nodecl_get_symbol(current_name)->symbol_name,
                                            nodecl_get_locus(current_designator)),
                                        nodecl_get_locus(current_designator)));
                        }

                        // Reverse the list
                        for (j = 0; j < num_items_reversed_list / 2; j++)
                        {
                            nodecl_t tmp = reversed_list[j];
                            reversed_list[j] = reversed_list[num_items_reversed_list - j];
                            reversed_list[num_items_reversed_list - j] = tmp;
                        }
                        // (not reversed anymore despite the name)

                        // Add the current field being looked up
                        P_LIST_ADD(reversed_list,
                                num_items_reversed_list,
                                nodecl_make_c99_field_designator(
                                    nodecl_make_cxx_dep_name_simple(
                                        field_name,
                                        nodecl_get_locus(current_designator)),
                                    nodecl_get_locus(current_designator)));

                        xfree(designators);

                        designators = reversed_list;
                        designator_list_length = num_items_reversed_list;

                        // Restart this type again
                        (*type_stack_idx)--;

                        // Note that we have not consumed any designator here
                        continue;
                    }
                }
            }

            if (!found)
            {
                error_printf("%s: error: designator '.%s' does not name a field of type '%s'\n",
                        nodecl_locus_to_str(current_designator),
                        field_name,
                        print_type_str(type_stack[*type_stack_idx].type, nodecl_retrieve_context(current_designator)));
                return 0;
            }
        }
        else
        {
            error_printf("%s: error: invalid designator for type '%s'\n",
                    nodecl_locus_to_str(current_designator),
                    print_type_str(type_stack[*type_stack_idx].type, nodecl_retrieve_context(current_designator)));
            return 0;
        }

        i++;
    }

    xfree(designators);

    return 1;
}

// This function creates a designator for non designated initializers
static void nodecl_craft_designator(
        nodecl_t nodecl_init,
        struct type_init_stack_t* type_stack,
        int type_stack_idx,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    nodecl_t nodecl_result = nodecl_init;

    while (type_stack_idx >= 0)
    {
        type_t* current_type = type_stack[type_stack_idx].type;
        int item = type_stack[type_stack_idx].item;

        const_value_t* cval = nodecl_get_constant(nodecl_result);

        if (is_array_type(current_type))
        {
            nodecl_result = nodecl_make_index_designator(
                    const_value_to_nodecl(const_value_get_signed_int(item)),
                    nodecl_result,
                    array_type_get_element_type(current_type),
                    locus);
            nodecl_set_constant(nodecl_result, cval);
        }
        else if (is_vector_type(current_type)
                || is_complex_type(current_type))
        {
            // Do nothing with these cases as they cannot be designated actually
        }
        else if (is_class_type(current_type))
        {
            scope_entry_t* field = type_stack[type_stack_idx].fields[item];

            nodecl_result = nodecl_make_field_designator(
                    nodecl_make_symbol(field, locus),
                    nodecl_result,
                    get_unqualified_type(field->type_information),
                    locus);
            nodecl_set_constant(nodecl_result, cval);
        }
        else
        {
            internal_error("Code unreachable", 0);
        }

        type_stack_idx--;
    }

    *nodecl_output = nodecl_result;
}


static void nodecl_make_designator_rec(nodecl_t *nodecl_output, 
        type_t* designated_type, 
        nodecl_t *designators,
        int current_designator,
        int num_designators)
{
    if (current_designator >= num_designators)
        return;

    nodecl_t (*nodecl_ptr_fun)(nodecl_t, nodecl_t, type_t*, const locus_t* locus);

    nodecl_t child_0 = nodecl_null();

    if (nodecl_get_kind(designators[current_designator]) == NODECL_C99_FIELD_DESIGNATOR)
    {
        ERROR_CONDITION(!is_class_type(designated_type), "Invalid type", 0);

        nodecl_ptr_fun = nodecl_make_field_designator;

        nodecl_t nodecl_name = nodecl_get_child(designators[current_designator], 0);
        scope_entry_list_t* entry_list = get_member_of_class_type_nodecl(
                nodecl_retrieve_context(designators[current_designator]),
                designated_type,
                nodecl_name,
                // A field_path_t should not be necessary here in C99
                NULL);
        ERROR_CONDITION(entry_list == NULL, "Invalid designator", 0);
        scope_entry_t* entry = entry_list_head(entry_list);
        designated_type  = entry->type_information;
        entry_list_free(entry_list);

        child_0 = nodecl_make_symbol(entry, 
                nodecl_get_locus(designators[current_designator]));
    }
    else if (nodecl_get_kind(designators[current_designator]) == NODECL_C99_INDEX_DESIGNATOR)
    {
        ERROR_CONDITION(!is_array_type(designated_type), "Invalid type", 0);

        nodecl_ptr_fun = nodecl_make_index_designator;

        child_0 = nodecl_shallow_copy(nodecl_get_child(designators[current_designator], 0));

        designated_type = array_type_get_element_type(designated_type);
    }
    else
        internal_error("Code unreachable", 0);

    nodecl_make_designator_rec(nodecl_output, designated_type, designators, current_designator + 1, num_designators);

    *nodecl_output = (nodecl_ptr_fun)(
            child_0,
            *nodecl_output,
            designated_type,
            nodecl_get_locus(*nodecl_output));
}

static void nodecl_make_designator(nodecl_t *nodecl_output, type_t* declared_type, nodecl_t designator)
{
    int num_designators = 0;
    nodecl_t* designators = nodecl_unpack_list(designator, &num_designators);

    nodecl_make_designator_rec(nodecl_output, declared_type, designators, 0, num_designators);

    xfree(designators);
}

char is_narrowing_conversion_type(type_t* orig_type,
        type_t* dest_type,
        const_value_t* orig_value)
{
    if (is_floating_type(orig_type)
            && is_integer_type(dest_type))
    {
        // from a floating-point type to an integer type, or
        return 1;
    }
    else if ((is_long_double_type(orig_type)
                && is_double_type(dest_type))
            || (is_double_type(orig_type)
                && is_float_type(dest_type)))
    {
        // from long double to double or float, or from double to float, except where
        // the source is a constant expression and the actual value after conversion is
        // within the range of values that can be represented (even if it cannot be
        // represented exactly), or
        if (orig_value != NULL)
        {
            const_value_t* max_dest = floating_type_get_maximum(dest_type);
            const_value_t* min_dest = floating_type_get_minimum(dest_type);
            // Now convert the ranges to the origin (which will be wider)
            max_dest = const_value_cast_to_floating_type_value(max_dest, orig_type);
            min_dest = const_value_cast_to_floating_type_value(min_dest, orig_type);

            if (const_value_is_nonzero(const_value_lte(min_dest, orig_value))
                    && const_value_is_nonzero(const_value_lte(orig_value, max_dest)))
            {
                // min_dest <= orig_value && orig_value <= max_dest
                // No narrowing
                return 0;
            }
        }
        return 1;
    }
    else if ((is_integer_type(orig_type)
                || is_unscoped_enum_type(orig_type))
            && is_floating_type(dest_type))
    {
        // from an integer type or unscoped enumeration type to a
        // floating-point type, except where the source is a constant
        // expression and the actual value after conversion will fit into the
        // target type and will produce the original value when converted back
        // to the original type, or
        if (orig_value != NULL)
        {
            // Verify that the amount of bits to represent this integer is not
            // greater than the number of bits of the mantissa of the
            // destination floating type

            cvalue_uint_t v = 0;
            if (const_value_is_zero(orig_value))
            {
                // Trivial case. No narrowing
                return 0;
            }
            else if (const_value_is_positive(orig_value))
            {
                v = const_value_cast_to_cvalue_uint(orig_value);
            }
            else
            {
                cvalue_int_t v1 = const_value_cast_to_cvalue_int(orig_value);
                if (v1 < 0)
                    v = -v1;
                else
                    v = v1;
            }
            ERROR_CONDITION(v == 0, "Invalid value here", 0);

            // We assume all bits are significative and remove the leading and
            // trailing bits (since v != 0)
            unsigned int num_significative_bits = sizeof(cvalue_uint_t)*8;
            num_significative_bits -= __builtin_clzll(v);
            num_significative_bits -= __builtin_ctzll(v);

            const floating_type_info_t* finfo = floating_type_get_info(dest_type);

            // Note that finfo->p contains the explicitly stored bits
            if (num_significative_bits <= (finfo->p + 1))
            {
                // No narrowing
                return 0;
            }
        }

        return 1;
    }
    else if ((is_integer_type(orig_type)
                || is_unscoped_enum_type(orig_type))
            && (is_integer_type(dest_type) && !is_bool_type(dest_type)))
    {
        if (is_unscoped_enum_type(orig_type))
            orig_type = enum_type_get_underlying_type(orig_type);

        // from an integer type or unscoped enumeration type to an integer type
        // that cannot represent all the values of the original type, except
        // where the source is a constant expression whose value after integral
        // promotions will fit into the target type.
        if ((is_signed_integral_type(orig_type)
                    == is_signed_integral_type(dest_type))
                && (type_get_size(dest_type) >= type_get_size(orig_type)))
        {
            // Their signedness match and the dest is wider or equal than
            // orig. No narrowing
            return 0;
        }
        else if (is_signed_integral_type(dest_type)
                && is_unsigned_integral_type(orig_type)
                && (type_get_size(dest_type) > type_get_size(orig_type)))
        {
            // A signed that is strictly wider than its unsigned equivalent
            // is able to represent all the values of the unsigned.
            // No narrowing
            return 0;
        }

        if (orig_value != NULL)
        {
            if (const_value_is_zero(orig_value))
                // A zero fits everywhere. No narrowing
                return 0;

            if (is_signed_integral_type(orig_type)
                        == is_signed_integral_type(dest_type))
            {
                // Same signedness
                const_value_t* max_dest = integer_type_get_maximum(dest_type);
                const_value_t* min_dest = integer_type_get_maximum(dest_type);

                if (const_value_is_nonzero(
                            const_value_lte(min_dest, orig_value))
                        && const_value_is_negative(
                            const_value_lte(orig_value, max_dest)))
                {
                    // The const value lies between the minimum and maximum of
                    // the dest type
                    // No narrowing
                    return 0;
                }
            }
            else if (is_unsigned_integral_type(dest_type))
            {
                ERROR_CONDITION(!is_signed_integral_type(orig_type), "Must be signed!", 0);
                if (const_value_is_positive(orig_value))
                {
                    if (type_get_size(dest_type) >= type_get_size(orig_type))
                    {
                        // unsigned long x { 1 };
                        // A positive constant of a narrower (or equal) signed
                        // type will always fit
                        // No narrowing
                        return 0;
                    }
                    else
                    {
                        // unsigned int x { 1L };
                        // orig_type is wider, check if the constant is still in the range
                        const_value_t* max_dest = integer_type_get_maximum(dest_type);
                        // cast the maximum to the type of the signed integral (this is safe)
                        max_dest = const_value_cast_to_bytes(
                                max_dest,
                                type_get_size(orig_type),
                                /* signed */ 1);

                        if (const_value_is_nonzero(
                                    const_value_lte(orig_value, max_dest)))
                        {
                            // It fits. No narrowing
                            return 0;
                        }
                    }
                }
            }
            else if (is_signed_integral_type(dest_type))
            {
                ERROR_CONDITION(!is_unsigned_integral_type(orig_type), "Must be unsigned!", 0);

                if (type_get_size(dest_type) <= type_get_size(orig_type))
                {
                    // signed int i { 1LU };
                    // signed int i { 1U };
                    const_value_t* max_dest = integer_type_get_maximum(dest_type);
                    max_dest = const_value_cast_to_bytes(
                            max_dest,
                            type_get_size(orig_type),
                            /* signed */ 0);

                    if (const_value_is_nonzero(
                                const_value_lte(orig_value, max_dest)))
                    {
                        // It fits. No narrowing
                        return 0;
                    }
                }
                else
                {
                    // This case was already handled at the beginning
                    internal_error("Code unreachable", 0);
                }
            }
        }

        // There is narrowing
        return 1;
    }

    return 0;
}

char check_narrowing_conversion(nodecl_t orig_expr,
        type_t* dest_type,
        decl_context_t decl_context)
{
    type_t* source_type = nodecl_get_type(orig_expr);
    if (is_narrowing_conversion_type(
                source_type,
                dest_type,
                nodecl_get_constant(orig_expr)))
    {
        error_printf("%s: error: narrowing conversion from '%s' to '%s' is not allowed\n",
                nodecl_locus_to_str(orig_expr),
                print_type_str(source_type, decl_context),
                print_type_str(dest_type, decl_context));

        return 1;
    }

    return 0;
}

static const_value_t* get_zero_value_of_type(type_t* t)
{
    if (is_integral_type(t)
            || is_enum_type(t))
    {
        if (is_enum_type(t))
        {
            t = enum_type_get_underlying_type(t);
        }

        return const_value_get_zero(type_get_size(t), is_signed_integral_type(t));
    }
    else if (is_float_type(t))
    {
        return const_value_get_float(0.0f);
    }
    else if (is_double_type(t))
    {
        return const_value_get_double(0.0);
    }
    else if (is_long_double_type(t))
    {
        return const_value_get_long_double(0.0L);
    }
    else if (is_pointer_type(t)
            || is_pointer_to_member_type(t))
    {
        return const_value_get_zero(type_get_size(t), /* sign */ 0);
    }
#ifdef HAVE_QUADMATH_H
    else if (is_float128_type(t))
    {
        return const_value_get_float128(0.0Q);
    }
#endif
    else if (is_array_type(t))
    {
        if (array_type_is_unknown_size(t))
            return const_value_make_array(0, NULL);

        nodecl_t n = array_type_get_array_size_expr(t);
        if (!nodecl_is_constant(n))
            return NULL;

        return const_value_make_array_from_scalar(
                const_value_cast_to_signed_int(
                    nodecl_get_constant(n)),
                get_zero_value_of_type(
                    array_type_get_element_type(t)));
    }
    else if (is_class_type(t))
    {
        scope_entry_list_t* fields = class_type_get_nonstatic_data_members(t);

        int num_fields = entry_list_size(fields);

        const_value_t** cval = xcalloc(num_fields, sizeof(*cval));
        scope_entry_list_iterator_t* it = NULL;
        int i;
        for (i = 0, it = entry_list_iterator_begin(fields);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it), i++)
        {
            scope_entry_t* field = entry_list_iterator_current(it);

            cval[i] = get_zero_value_of_type(
                    field->type_information);
        }
        entry_list_iterator_free(it);
        entry_list_free(fields);

        const_value_t* result = const_value_make_struct(num_fields, cval, t);

        xfree(cval);
        return result;
    }
    else
    {
        internal_error("Cannot get zero value of type '%s'\n",
                print_declarator(t));
    }
}

static const_value_t* generate_aggregate_constant(struct type_init_stack_t *type_stack, int type_stack_idx)
{
    const_value_t* braced_constant_value = NULL;
    type_t* initializer_type = type_stack[type_stack_idx].type;

    // Generate the final constant
    if (type_stack[type_stack_idx].num_values >= 0)
    {
        int j;
        for (j = 0; j < type_stack[type_stack_idx].num_values; j++)
        {
            if (type_stack[type_stack_idx].values[j] == NULL)
            {
                type_t* sub_type = NULL;
                if (is_array_type(initializer_type))
                    sub_type = array_type_get_element_type(initializer_type);
                else if (is_vector_type(initializer_type))
                    sub_type = vector_type_get_element_type(initializer_type);
                else if (is_complex_type(initializer_type))
                    sub_type = complex_type_get_base_type(initializer_type);
                else if (is_class_type(initializer_type))
                    sub_type = type_stack[type_stack_idx].fields[j]->type_information;
                else
                    internal_error("Code unreachable", 0);

                if (!is_union_type(initializer_type))
                {
                    type_stack[type_stack_idx].values[j] = get_zero_value_of_type(sub_type);
                }
                else
                {
                    // Inactive members of a union have an unknown constant value
                    type_stack[type_stack_idx].values[j] = const_value_get_unknown();
                }
            }
        }

        if (is_array_type(initializer_type))
        {
            int num_items = 
                const_value_cast_to_signed_int(
                        nodecl_get_constant(
                            array_type_get_array_size_expr(initializer_type)));
            ERROR_CONDITION(type_stack[type_stack_idx].num_values != num_items,
                    "Inconsistency %d != %d",
                    type_stack[type_stack_idx].num_values,
                    num_items);

            braced_constant_value = const_value_make_array(
                    num_items,
                    type_stack[type_stack_idx].values);
        }
        else if (is_vector_type(initializer_type))
        {
            int num_items = vector_type_get_vector_size(initializer_type) 
                / type_get_size(vector_type_get_element_type(initializer_type));
            ERROR_CONDITION(type_stack[type_stack_idx].num_values != num_items,
                    "Inconsistency %d != %d",
                    type_stack[type_stack_idx].num_values,
                    num_items);

            braced_constant_value = const_value_make_vector(num_items, type_stack[type_stack_idx].values);
        }
        else if (is_complex_type(initializer_type))
        {
            ERROR_CONDITION(type_stack[type_stack_idx].num_values != 2,
                    "Inconsistency %d != %d",
                    type_stack[type_stack_idx].num_values,
                    2);

            braced_constant_value = const_value_make_complex(
                    type_stack[type_stack_idx].values[0],
                    type_stack[type_stack_idx].values[1]);
        }
        else if (is_class_type(initializer_type))
        {
            scope_entry_list_t* fields = class_type_get_nonstatic_data_members(initializer_type);
            int num_items = entry_list_size(fields);
            entry_list_free(fields);

            ERROR_CONDITION(type_stack[type_stack_idx].num_values != num_items,
                    "Inconsistency %d != %d",
                    type_stack[type_stack_idx].num_values,
                    num_items);
            braced_constant_value = const_value_make_struct(num_items,
                    type_stack[type_stack_idx].values,
                    initializer_type);
        }
        else
        {
            internal_error("Code unreachable", 0);
        }
    }

    return braced_constant_value;
}

void check_nodecl_braced_initializer(
        nodecl_t braced_initializer,
        decl_context_t decl_context,
        type_t* declared_type,
        char is_explicit_type_cast,
        enum initialization_kind initialization_kind,
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(nodecl_get_kind(braced_initializer) != NODECL_CXX_BRACED_INITIALIZER, "Invalid nodecl", 0);

    if (is_dependent_type(declared_type))
    {
        *nodecl_output = braced_initializer;
        nodecl_set_type(*nodecl_output, declared_type);
        return;
    }

    const locus_t* locus = nodecl_get_locus(braced_initializer);

    char braced_initializer_is_dependent = nodecl_expr_is_type_dependent(braced_initializer);

    nodecl_t initializer_clause_list = nodecl_get_child(braced_initializer, 0);

    scope_entry_t* std_initializer_list_template = get_std_initializer_list_template(decl_context, 
            locus,
            /* mandatory */ 0);

    if (is_named_class_type(no_ref(declared_type)))
    {
        scope_entry_t* symbol = named_type_get_symbol(no_ref(declared_type));
        class_type_complete_if_possible(symbol, decl_context, locus);
    }

    if ((is_rvalue_reference_type(declared_type)
                || (is_lvalue_reference_type(declared_type)
                    && is_const_qualified_type(no_ref(declared_type))))
            && (is_class_type(no_ref(declared_type))
                || is_array_type(no_ref(declared_type))
                || is_vector_type(no_ref(declared_type))
                || is_complex_type(no_ref(declared_type))))
    {
        check_nodecl_braced_initializer(
                braced_initializer,
                decl_context,
                no_ref(declared_type),
                is_explicit_type_cast,
                initialization_kind,
                nodecl_output);
        return;
    }
    else if (IS_CXX11_LANGUAGE
            && is_class_type(declared_type)
            && nodecl_list_length(initializer_clause_list) == 0
            && class_type_get_default_constructor(declared_type) != NULL)
    {
        scope_entry_t* constructor = class_type_get_default_constructor(declared_type);

        if (function_has_been_deleted(decl_context, constructor, 
                    locus))
        {
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        if (initialization_kind & IK_COPY_INITIALIZATION
                && symbol_entity_specs_get_is_explicit(constructor))
        {
            error_printf("%s: error: list copy-initialization would use an explicit default constructor\n",
                    nodecl_locus_to_str(braced_initializer));
            *nodecl_output = nodecl_make_err_expr(
                    locus);
            return;
        }

        // This case is a bit weird, the standard says to value initialize the class object
        // but since it requires a constructor, this is like default initializing it
        *nodecl_output = nodecl_make_value_initialization(constructor, locus);
        return;
    }
    else if ((is_class_type(declared_type)
                || is_array_type(declared_type)
                || is_vector_type(declared_type)
                // Note that complex types are only aggregates in C++
                || is_complex_type(declared_type))
            && is_aggregate_type(declared_type))
    {
        nodecl_t init_list_output = nodecl_null();

        type_t* initializer_type = declared_type;
        const_value_t* braced_constant_value = NULL;
        if (nodecl_is_null(initializer_clause_list)
                && is_array_type(declared_type)
                && array_type_is_unknown_size(declared_type))
        {
            // GCC extension
            // int c[] = { };
            nodecl_t length = nodecl_make_integer_literal(get_signed_int_type(),
                    const_value_get_unsigned_int(0),
                    locus);

            initializer_type = get_array_type(
                    array_type_get_element_type(declared_type),
                    length, decl_context);

            braced_constant_value = const_value_make_array(0, NULL);
        }
        else
        {
            // Special case for this sort of initializations
            //   char a[] = { "hello" };
            // The expression list has only one element of kind expression and this element is an array of chars o wchars
            if (!nodecl_is_null(initializer_clause_list))
            {
                type_t * type_element = nodecl_get_type(nodecl_get_child(nodecl_list_head(initializer_clause_list), 0));
                if (nodecl_list_length(initializer_clause_list) == 1
                        && nodecl_get_kind(nodecl_list_head(initializer_clause_list)) != NODECL_CXX_BRACED_INITIALIZER
                        && (is_array_type(no_ref(type_element))
                            && (is_char_type(array_type_get_element_type(no_ref(type_element)))
                                || is_wchar_t_type(array_type_get_element_type(no_ref(type_element)))
                                || is_char16_t_type(array_type_get_element_type(no_ref(type_element)))
                                || is_char32_t_type(array_type_get_element_type(no_ref(type_element)))
                               )
                           )
                   )
                {
                    // Attempt an interpretation like char a[] = "hello";
                    diagnostic_context_push_buffered();
                    nodecl_t nodecl_tmp = nodecl_shallow_copy(
                            nodecl_get_child(nodecl_list_head(initializer_clause_list), 0)
                            );
                    check_nodecl_expr_initializer(nodecl_tmp,
                            decl_context,
                            declared_type,
                            /* disallow_narrowing */ 0,
                            IK_COPY_INITIALIZATION,
                            nodecl_output);

                    if (!nodecl_is_err_expr(*nodecl_output))
                    {
                        diagnostic_context_pop_and_commit();
                        // It succeeded
                        return;
                    }
                    else
                    {
                        diagnostic_context_pop_and_discard();
                    }

                    nodecl_free(nodecl_tmp);
                    nodecl_free(*nodecl_output);
                    *nodecl_output = nodecl_null();
                }
            }

            struct type_init_stack_t type_stack[MCXX_MAX_UNBRACED_AGGREGATES];
            int type_stack_idx = 0;
            memset(&type_stack[type_stack_idx], 0, sizeof(type_stack[type_stack_idx]));

            type_stack[type_stack_idx].type = declared_type;
            if (is_array_type(declared_type)
                    || is_vector_type(declared_type)
                    || is_complex_type(declared_type))
            {
                type_stack[type_stack_idx].item = 0;
                type_stack[type_stack_idx].fields = NULL;
                if (is_array_type(declared_type))
                {
                    type_stack[type_stack_idx].max_item = 0;
                    if (array_type_is_unknown_size(declared_type))
                    {
                        type_stack[type_stack_idx].num_items = -1;
                    }
                    else
                    {
                        if (nodecl_is_null(array_type_get_array_size_expr(declared_type)))
                        {
                            error_printf("%s: error: initialization not allowed for arrays of undefined size\n",
                                    nodecl_locus_to_str(braced_initializer));
                            *nodecl_output = nodecl_make_err_expr(locus);
                            return;
                        }
                        if (!nodecl_is_constant(array_type_get_array_size_expr(declared_type)))
                        {
                            error_printf("%s: error: initialization not allowed for variable-length arrays\n",
                                    nodecl_locus_to_str(braced_initializer));
                            *nodecl_output = nodecl_make_err_expr(locus);
                            return;
                        }
                        type_stack[type_stack_idx].num_items =
                            const_value_cast_to_signed_int(nodecl_get_constant(array_type_get_array_size_expr(declared_type)));
                    }
                }
                else if (is_vector_type(declared_type))
                {
                    if (is_generic_vector_type(declared_type))
                    {
                        type_stack[type_stack_idx].num_items = -1;
                        type_stack[type_stack_idx].num_values = -1;
                    }
                    else
                        type_stack[type_stack_idx].num_items = 
                            vector_type_get_vector_size(declared_type) / type_get_size(vector_type_get_element_type(declared_type));
                }
                else if (is_complex_type(declared_type))
                {
                    type_stack[type_stack_idx].num_items = 2;
                }
                else
                {
                    internal_error("Code unreachable", 0);
                }
            }
            else if (is_class_type(declared_type))
            {
                scope_entry_list_t* fields = class_type_get_nonstatic_data_members(declared_type);

                type_stack[type_stack_idx].item = 0;
                entry_list_to_symbol_array(fields, &type_stack[type_stack_idx].fields, &type_stack[type_stack_idx].num_items);

                entry_list_free(fields);
            }
            else
            {
                internal_error("Code unreachable", type_stack_idx);
            }

            if (type_stack[type_stack_idx].num_items > 0)
            {
                type_stack[type_stack_idx].num_values = type_stack[type_stack_idx].num_items;
                type_stack[type_stack_idx].values = xcalloc(
                        type_stack[type_stack_idx].num_items,
                        sizeof(*type_stack[type_stack_idx].values));
            }

            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Top level declaration type %s\n", print_declarator(declared_type));
            }

            int i = 0, num_items = 0;
            nodecl_t* list = nodecl_unpack_list(initializer_clause_list, &num_items);
            while (i < num_items)
            {
                nodecl_t nodecl_initializer_clause = list[i];

                char designator_is_ok = 1;
                if (nodecl_get_kind(nodecl_initializer_clause) == NODECL_C99_DESIGNATED_INITIALIZER)
                {
                    designator_is_ok = update_stack_to_designator(declared_type,
                            type_stack,
                            &type_stack_idx,
                            nodecl_get_child(nodecl_initializer_clause, 0),
                            decl_context);
                    // Once the designation has been handled, proceed to use the initializer
                    nodecl_initializer_clause = nodecl_get_child(nodecl_initializer_clause, 1);
                }
                else
                {
                    char too_many_initializers = 0;
                    if ((type_stack[type_stack_idx].num_items != -1)
                            && type_stack[type_stack_idx].item >= type_stack[type_stack_idx].num_items)
                    {
                        warn_printf("%s: warning: too many initializers for type '%s', ignoring\n",
                                nodecl_locus_to_str(nodecl_initializer_clause),
                                print_type_str(type_stack[type_stack_idx].type, decl_context));
                        // We are at the top level object of this braced initializer, give up
                        too_many_initializers = 1;
                    }

                    if (too_many_initializers)
                        break;
                }

                type_t* current_type = type_stack[type_stack_idx].type;
                type_t* type_to_be_initialized = NULL;

                if (is_array_type(current_type))
                {
                    DEBUG_CODE()
                    {
                        fprintf(stderr, "EXPRTYPE: Initialization of array element at index %d\n", type_stack[type_stack_idx].item);
                    }
                    type_stack[type_stack_idx].max_item = MAX_ITEM(type_stack[type_stack_idx].max_item, type_stack[type_stack_idx].item);
                    type_to_be_initialized = array_type_get_element_type(current_type);
                }
                else if (is_vector_type(current_type))
                {
                    type_to_be_initialized = vector_type_get_element_type(current_type);
                }
                else if (is_complex_type(current_type))
                {
                    type_to_be_initialized = complex_type_get_base_type(current_type);
                }
                else if (is_class_type(current_type))
                {
                    int item = type_stack[type_stack_idx].item;
                    scope_entry_t* member = type_stack[type_stack_idx].fields[item];
                    if (symbol_entity_specs_get_is_unnamed_bitfield(member))
                    {
                        // An unnamed bitfield cannot be initialized
                        // Note that we are not consuming any item here
                        type_stack[type_stack_idx].item++;
                        continue;
                    }
                    else
                    {
                        type_to_be_initialized = type_stack[type_stack_idx].fields[item]->type_information;
                    }
                }
                else
                {
                    internal_error("Code unreachable", 0);
                }

                // if the initializer-list for a subaggregate does not begin with a left brace,
                // then only enough initializers from the list are taken to initialize the
                // members of the subaggregate; any remaining initializers are left to
                // initialize the next member of the aggregate of which the current
                // subaggregate is a member.
                //
                // C++ Standard 2003

                if (!is_aggregate_type(type_to_be_initialized)
                        || nodecl_get_kind(nodecl_initializer_clause) == NODECL_CXX_BRACED_INITIALIZER
                        // A array of chars can be initialized only by one string literal
                        || (is_array_type(type_to_be_initialized)
                            && is_char_type(array_type_get_element_type(type_to_be_initialized))
                            && nodecl_get_kind(nodecl_get_child(nodecl_initializer_clause, 0)) == NODECL_STRING_LITERAL))
                {
                    char current_is_braced = (nodecl_get_kind(nodecl_initializer_clause) == NODECL_CXX_BRACED_INITIALIZER);
                    DEBUG_CODE()
                    {
                        if (current_is_braced)
                        {
                            fprintf(stderr, "EXPRTYPE: Braced initializer %s\n", print_declarator(type_to_be_initialized));
                        }
                        else
                        {
                            fprintf(stderr, "EXPRTYPE: Simple initializer %s\n", print_declarator(type_to_be_initialized));
                        }
                    }

                    if (current_is_braced
                            && !is_aggregate_type(type_to_be_initialized)
                            // A class can be braced initialized in C++
                            && !(IS_CXX11_LANGUAGE && is_class_type(type_to_be_initialized)))
                    {
                        warn_printf("%s: warning: redundant brace initializer for type '%s'\n",
                                nodecl_locus_to_str(nodecl_initializer_clause),
                                print_type_str(type_to_be_initialized, decl_context));
                    }

                    // In this case, we only handle one element of the list
                    nodecl_t nodecl_init_output = nodecl_null();
                    check_nodecl_initializer_clause(nodecl_initializer_clause, decl_context, type_to_be_initialized,
                            /* disallow_narrowing */ IS_CXX11_LANGUAGE && !is_vector_type(current_type),
                            &nodecl_init_output);

                    if (nodecl_is_err_expr(nodecl_init_output))
                    {
                        // Free the stack
                        while (type_stack_idx >= 0)
                        {
                            xfree(type_stack[type_stack_idx].values);
                            type_stack[type_stack_idx].values = NULL;

                            xfree(type_stack[type_stack_idx].fields);
                            type_stack[type_stack_idx].fields = NULL;
                            type_stack_idx--;
                        }

                        *nodecl_output = nodecl_make_err_expr(locus);
                        return;
                    }

                    if (designator_is_ok
                            // The intialization is still constant
                            && type_stack[type_stack_idx].num_values >= 0)
                    {
                        if (!nodecl_get_constant(nodecl_init_output))
                        {
                            // Not constant, everything won't we constant either
                            type_stack[type_stack_idx].num_values = -1;
                            xfree(type_stack[type_stack_idx].values);
                            type_stack[type_stack_idx].values = NULL;
                        }
                        else
                        {
                            if (type_stack[type_stack_idx].item >= type_stack[type_stack_idx].num_values)
                            {
                                // Make room
                                type_stack[type_stack_idx].values =
                                    xrealloc(type_stack[type_stack_idx].values,
                                            (type_stack[type_stack_idx].item + 1) *
                                            sizeof(*type_stack[type_stack_idx].values));

                                int j;
                                for (j = type_stack[type_stack_idx].num_values;
                                        j < type_stack[type_stack_idx].item; j++)
                                {
                                    type_stack[type_stack_idx].values[j] = NULL;
                                }

                                type_stack[type_stack_idx].num_values = type_stack[type_stack_idx].item + 1;
                            }
                            type_stack[type_stack_idx].values[type_stack[type_stack_idx].item]
                                = nodecl_get_constant(nodecl_init_output);
                        }
                    }

                    if (nodecl_get_kind(list[i]) == NODECL_C99_DESIGNATED_INITIALIZER)
                    {
                        if (designator_is_ok)
                        {
                            // Keep the designator
                            nodecl_t designator = nodecl_get_child(list[i], 0);
                            nodecl_make_designator(&nodecl_init_output, declared_type, designator);
                        }
                    }
                    else
                    {
                        nodecl_craft_designator(nodecl_init_output,
                                type_stack,
                                type_stack_idx,
                                nodecl_get_locus(list[i]),
                                &nodecl_init_output);
                    }

                    init_list_output = nodecl_append_to_list(init_list_output, nodecl_init_output);
                    // This item has been consumed
                    i++;
                    type_stack[type_stack_idx].item++;

                    if (is_union_type(current_type))
                    {
                        type_stack[type_stack_idx].item = type_stack[type_stack_idx].num_items;
                    }

                    // Now pop stacks as needed
                    while (type_stack_idx > 0
                            && type_stack[type_stack_idx].num_items != -1
                            && type_stack[type_stack_idx].item >= type_stack[type_stack_idx].num_items)
                    {
                        DEBUG_CODE()
                        {
                            fprintf(stderr, "EXPRTYPE: Type %s if fully initialized now\n",
                                    print_declarator(type_stack[type_stack_idx].type));
                        }
                        const_value_t* current_aggregate =
                            generate_aggregate_constant(type_stack, type_stack_idx);

                        xfree(type_stack[type_stack_idx].values);
                        type_stack[type_stack_idx].values = NULL;

                        xfree(type_stack[type_stack_idx].fields);
                        type_stack[type_stack_idx].fields = NULL;

                        type_stack_idx--;

                        if (type_stack[type_stack_idx].num_values >= 0)
                        {
                            if (current_aggregate == NULL)
                            {
                                type_stack[type_stack_idx].num_values = -1;
                                xfree(type_stack[type_stack_idx].values);
                                type_stack[type_stack_idx].values = NULL;
                            }
                            else
                            {
                                if (type_stack[type_stack_idx].item >= type_stack[type_stack_idx].num_values)
                                {
                                    // Make room
                                    type_stack[type_stack_idx].values =
                                        xrealloc(type_stack[type_stack_idx].values,
                                                (type_stack[type_stack_idx].item + 1) *
                                                sizeof(*type_stack[type_stack_idx].values));

                                    int j;
                                    for (j = type_stack[type_stack_idx].num_values;
                                            j < type_stack[type_stack_idx].item; j++)
                                    {
                                        type_stack[type_stack_idx].values[j] = NULL;
                                    }

                                    type_stack[type_stack_idx].num_values = type_stack[type_stack_idx].item + 1;
                                }
                                type_stack[type_stack_idx].values[type_stack[type_stack_idx].item]
                                    = current_aggregate;
                            }
                        }

                        // We have filled an item of the enclosing aggregate
                        type_stack[type_stack_idx].item++;
                    }
                }
                else
                {
                    DEBUG_CODE()
                    {
                        fprintf(stderr, "EXPRTYPE: Unbraced initialization of aggregated type %s\n", print_declarator(type_to_be_initialized));
                    }

                    // Make a first attempt for structure types
                    if (is_class_type(type_to_be_initialized)
                            || is_vector_type(type_to_be_initialized))
                    {
                        nodecl_t nodecl_init_output = nodecl_null();
                        diagnostic_context_push_buffered();
                        nodecl_t nodecl_tmp = nodecl_shallow_copy(nodecl_initializer_clause);
                        check_nodecl_initializer_clause(nodecl_tmp, decl_context,
                                type_to_be_initialized,
                                /* disallow_narrowing */ IS_CXX11_LANGUAGE,
                                &nodecl_init_output);
                        if (!nodecl_is_err_expr(nodecl_init_output))
                        {
                            diagnostic_context_pop_and_commit();
                            // It seems fine
                            init_list_output = nodecl_append_to_list(init_list_output, nodecl_init_output);
                            // Keep the constant
                            if (type_stack[type_stack_idx].num_values >= 0)
                            {
                                if (!nodecl_get_constant(nodecl_init_output))
                                {
                                    // Not constant, everything won't we constant either
                                    type_stack[type_stack_idx].num_values = -1;

                                    xfree(type_stack[type_stack_idx].values);
                                    type_stack[type_stack_idx].values = NULL;
                                }
                                else
                                {
                                    if (type_stack[type_stack_idx].item >= type_stack[type_stack_idx].num_values)
                                    {
                                        // Make room
                                        type_stack[type_stack_idx].values =
                                            xrealloc(type_stack[type_stack_idx].values,
                                                    (type_stack[type_stack_idx].item + 1)*
                                                    sizeof(*type_stack[type_stack_idx].values));

                                        int j;
                                        for (j = type_stack[type_stack_idx].num_values;
                                                j < type_stack[type_stack_idx].item; j++)
                                        {
                                            type_stack[type_stack_idx].values[j] = NULL;
                                        }

                                        type_stack[type_stack_idx].num_values = type_stack[type_stack_idx].item + 1;
                                    }
                                    type_stack[type_stack_idx].values[type_stack[type_stack_idx].item]
                                        = nodecl_get_constant(nodecl_init_output);
                                }
                            }
                            // This item has been consumed
                            i++;
                            type_stack[type_stack_idx].item++;
                            continue;
                        }
                        else
                        {
                            diagnostic_context_pop_and_discard();
                        }
                    }

                    // Now we have to initialize an aggregate but the syntax lacks braces, so we have to push this item
                    // to the type stack and continue from here
                    type_stack_idx++;
                    ERROR_CONDITION(type_stack_idx == MCXX_MAX_UNBRACED_AGGREGATES, "Too many unbraced aggregates", 0);

                    memset(&type_stack[type_stack_idx], 0, sizeof(type_stack[type_stack_idx]));

                    type_stack[type_stack_idx].type = type_to_be_initialized;
                    if (is_array_type(type_to_be_initialized)
                            || is_vector_type(type_to_be_initialized))
                    {
                        if (is_array_type(type_to_be_initialized))
                        {
                            if (array_type_is_unknown_size(type_to_be_initialized))
                            {
                                if (type_stack_idx > 0)
                                {
                                    warn_printf("%s: warning: initialization of flexible array member without braces\n",
                                            nodecl_locus_to_str(nodecl_initializer_clause));
                                }
                                type_stack[type_stack_idx].num_items = -1;
                            }
                            else
                            {
                                type_stack[type_stack_idx].num_items =
                                    const_value_cast_to_signed_int(nodecl_get_constant(array_type_get_array_size_expr(
                                                    type_to_be_initialized)));
                            }
                        }
                    }
                    else if (is_class_type(type_to_be_initialized))
                    {
                        scope_entry_list_t* fields = class_type_get_nonstatic_data_members(type_to_be_initialized);

                        type_stack[type_stack_idx].item = 0;
                        entry_list_to_symbol_array(fields, &type_stack[type_stack_idx].fields, &type_stack[type_stack_idx].num_items);

                        entry_list_free(fields);
                    }
                    else
                    {
                        internal_error("Code unreachable", type_stack_idx);
                    }

                    type_stack[type_stack_idx].num_values = type_stack[type_stack_idx].num_items;
                    if (type_stack[type_stack_idx].num_values > 0)
                    {
                        type_stack[type_stack_idx].values = xcalloc(
                                type_stack[type_stack_idx].num_values,
                                sizeof(*type_stack[type_stack_idx].values));
                    }

                    // Note that we are not consuming any item here
                    continue;
                }
            }

            // Deallocate nodecl list
            xfree(list);

            if (is_array_type(declared_type)
                    && type_stack[0].num_items == -1)
            {
                nodecl_t length = nodecl_make_integer_literal(get_signed_int_type(),
                        const_value_get_unsigned_int(type_stack[0].max_item + 1),
                        locus);

                type_stack[0].type = get_array_type(
                        array_type_get_element_type(declared_type),
                        length, decl_context);
            }
            braced_constant_value = generate_aggregate_constant(type_stack, /* type_stack_idx */ 0);

            initializer_type = type_stack[0].type;

            xfree(type_stack[0].fields);
            type_stack[0].fields = 0;

            xfree(type_stack[0].values);
            type_stack[0].values = 0;
        }

        *nodecl_output = nodecl_make_structured_value(init_list_output,
                /* structured-value-form */ is_explicit_type_cast
                ? nodecl_make_structured_value_braced_typecast(locus)
                : nodecl_make_structured_value_braced_implicit(locus),
                initializer_type, locus);
        nodecl_set_constant(*nodecl_output, braced_constant_value);

        DEBUG_CODE()
        {
            if (braced_constant_value != NULL)
            {
                fprintf(stderr, "EXPRTYPE: Aggregate initializer at %s has const value |%s|\n", 
                        nodecl_locus_to_str(braced_initializer),
                        const_value_to_str(braced_constant_value));
            }
            else
            {
                fprintf(stderr, "EXPRTYPE: Aggregate initializer at %s does not have const value\n", 
                        nodecl_locus_to_str(braced_initializer));
            }
        }

        return;
    }
    else if (is_named_class_type(declared_type)
            && is_template_specialized_type(get_actual_class_type(declared_type))
            && std_initializer_list_template != NULL
            && equivalent_types(template_specialized_type_get_related_template_type(
                    get_actual_class_type(declared_type)),
                    std_initializer_list_template->type_information))
    {
        // This is an initialization of a std::initializer_list<T> using a
        // braced initializer list We have to call the ad-hoc private
        // constructor std::initializer_list<T>(T*, size_type)
        int num_args = 2;
        type_t* arg_list[2];
        memset(arg_list, 0, sizeof(arg_list));

        template_parameter_list_t* template_arguments = template_specialized_type_get_template_arguments(
                get_actual_class_type(declared_type));

        arg_list[0] = get_pointer_type(template_arguments->arguments[0]->type);
        arg_list[1] = get_size_t_type();

        scope_entry_list_t* candidates = NULL;
        scope_entry_t* constructor = NULL;
        char ok = solve_initialization_of_class_type(declared_type,
                arg_list,
                num_args,
                IK_DIRECT_INITIALIZATION | IK_BY_CONSTRUCTOR,
                decl_context,
                locus,
                &constructor,
                &candidates);
        entry_list_free(candidates);

        // FIXME - Narrowing is not correctly verified here...

        if (ok)
        {
            nodecl_t nodecl_arguments_output = nodecl_make_list_2(
                    // Codegen should do the right thing: this call is implicit
                    // and only the first argument for these calls is emitted
                    nodecl_shallow_copy(braced_initializer),
                    /* num items */
                    const_value_to_nodecl(const_value_get_integer(
                            braced_list_type_get_num_types(
                                nodecl_get_type(braced_initializer)),
                            type_get_size(get_size_t_type()),
                            /* signed */ 0)));
            // Note: we cannot call cxx_nodecl_make_function_call because it
            // would attempt to convert the braced initializer into a pointer
            // (which is not allowed by the typesystem)
            *nodecl_output = nodecl_make_function_call(
                    nodecl_make_symbol(constructor, locus),
                    nodecl_arguments_output,
                    /* called name */ nodecl_null(),
                    nodecl_make_cxx_function_form_implicit(locus),
                    declared_type,
                    locus);
        }
        else
        {
            error_printf("%s: error: cannot call internal constructor of '%s' for braced-initializer\n",
                    locus_to_str(locus),
                    print_type_str(declared_type, decl_context));
            *nodecl_output = nodecl_make_err_expr(locus);
        }
        return;
    }
    // Not an aggregate class
    else if (is_class_type(declared_type)
            && !is_aggregate_type(declared_type)
            && !braced_initializer_is_dependent)
    {
        int i, num_args = 0;
        nodecl_t* nodecl_list = nodecl_unpack_list(initializer_clause_list, &num_args);

        type_t* arg_list[num_args + 1];
        memset(arg_list, 0, sizeof(arg_list));

        for (i = 0; i < num_args; i++)
        {
            nodecl_t nodecl_initializer_clause = nodecl_list[i];
            arg_list[i] = nodecl_get_type(nodecl_initializer_clause);
        }

        scope_entry_list_t* candidates = NULL;
        scope_entry_t* constructor = NULL;

        char ok = solve_list_initialization_of_class_type(
                declared_type,
                arg_list,
                num_args,
                initialization_kind | IK_BY_CONSTRUCTOR,
                decl_context,
                locus,
                &constructor,
                &candidates);

        if (!ok)
        {
            error_printf("%s: error: invalid initializer for type '%s'\n",
                    nodecl_locus_to_str(braced_initializer),
                    print_type_str(declared_type, decl_context));

            if (entry_list_size(candidates) != 0)
            {
                const char* message = NULL;
                uniquestr_sprintf(&message,
                        "%s: error: no suitable conversion for list-initialization of type '%s' "
                        "using an expression of type '%s'\n",
                        locus_to_str(nodecl_get_locus(braced_initializer)),
                        print_type_str(declared_type, decl_context),
                        print_type_str(nodecl_get_type(braced_initializer), decl_context));
                diagnostic_candidates(candidates, &message, nodecl_get_locus(braced_initializer));
                error_printf("%s", message);
            }
            entry_list_free(candidates);
            xfree(nodecl_list);
            *nodecl_output = nodecl_make_err_expr(
                    locus);
        }
        else
        {
            if (function_has_been_deleted(decl_context, constructor, locus))
            {
                xfree(nodecl_list);
                *nodecl_output = nodecl_make_err_expr(
                        locus);
                return;
            }

            char is_initializer_constructor = 0;
            int num_parameters = -1;
            char is_promoting_ellipsis = 0;
            if (std_initializer_list_template != NULL)
            {
                // Check if the constructor is an initializer-list one
                num_parameters = function_type_get_num_parameters(constructor->type_information);
                if (function_type_get_has_ellipsis(constructor->type_information))
                {
                    is_promoting_ellipsis = is_ellipsis_type(
                            function_type_get_parameter_type_num(constructor->type_information, num_parameters - 1)
                            );
                    num_parameters--;
                }

                if (num_parameters > 0
                        && can_be_called_with_number_of_arguments(constructor, 1))
                {
                    type_t* first_param = function_type_get_parameter_type_num(constructor->type_information, 0);

                    if (is_class_type(first_param))
                        first_param = get_actual_class_type(first_param);

                    if (is_template_specialized_type(first_param)
                            && equivalent_types(template_specialized_type_get_related_template_type(first_param), 
                                std_initializer_list_template->type_information))
                    {
                        is_initializer_constructor = 1;
                    }
                }
            }

            if (is_initializer_constructor)
            {
                xfree(nodecl_list);

                nodecl_t nodecl_arg = nodecl_null();
                check_nodecl_function_argument_initialization(
                        braced_initializer,
                        decl_context,
                        function_type_get_parameter_type_num(constructor->type_information, 0),
                        /* disallow_narrowing */ 1,
                        &nodecl_arg);

                if (nodecl_is_err_expr(nodecl_arg))
                {
                    xfree(nodecl_list);
                    *nodecl_output = nodecl_make_err_expr(
                            locus);
                    return;
                }

                *nodecl_output = cxx_nodecl_make_function_call(
                        nodecl_make_symbol(constructor, locus),
                        /* called name */ nodecl_null(),
                        nodecl_make_list_1(nodecl_arg),
                        nodecl_make_cxx_function_form_implicit(locus),
                        declared_type,
                        decl_context,
                        locus);
            }
            else
            {
                nodecl_t nodecl_arguments_output = nodecl_null();
                for (i = 0; i < num_args; i++)
                {
                    nodecl_t nodecl_arg = nodecl_list[i];

                    if (i < num_parameters)
                    {
                        type_t* param_type = function_type_get_parameter_type_num(constructor->type_information, i);

                        nodecl_t nodecl_old_arg = nodecl_arg;
                        check_nodecl_function_argument_initialization(nodecl_arg,
                                decl_context,
                                param_type,
                                /* disallow_narrowing */ 1,
                                &nodecl_arg);
                        if (nodecl_is_err_expr(nodecl_arg))
                        {
                            xfree(nodecl_list);
                            *nodecl_output = nodecl_arg;
                            nodecl_free(nodecl_old_arg);
                            return;
                        }
                    }
                    else
                    {
                        if (is_promoting_ellipsis)
                        {
                            type_t* arg_type = nodecl_get_type(nodecl_arg);
                            // Ellipsis
                            type_t* default_argument_promoted_type = compute_default_argument_conversion(arg_type,
                                    decl_context,
                                    nodecl_get_locus(nodecl_arg),
                                    /* emit_diagnostic */ 1);

                            if (is_error_type(default_argument_promoted_type))
                            {
                                xfree(nodecl_list);
                                *nodecl_output = nodecl_make_err_expr(locus);
                                nodecl_free(nodecl_arguments_output);
                                return;
                            }
                        }
                    }

                    nodecl_arguments_output = nodecl_append_to_list(nodecl_arguments_output,
                            nodecl_arg);
                }

                xfree(nodecl_list);

                *nodecl_output = cxx_nodecl_make_function_call(
                        nodecl_make_symbol(constructor,
                            locus),
                        /* called name */ nodecl_null(),
                        nodecl_arguments_output,
                        /* function-form */
                        nodecl_null(),
                        declared_type,
                        decl_context,
                        locus);
            }
        }
        return;
    }
    else if (braced_initializer_is_dependent)
    {
        *nodecl_output = braced_initializer;
        return;
    }
    // Not an aggregate of any kind
    else if (!braced_initializer_is_dependent)
    {
        if (!nodecl_is_null(initializer_clause_list))
        {
            // C++ does not accept things like this: int x = {1, 2}
            int num_items = nodecl_list_length(initializer_clause_list);
            if (num_items != 1)
            {
                CXX_LANGUAGE()
                {
                    error_printf("%s: error: brace initialization with more than one element for type '%s'\n",
                            nodecl_locus_to_str(braced_initializer),
                            print_type_str(declared_type, decl_context));
                    *nodecl_output = nodecl_make_err_expr(locus);
                    return;
                }
                C_LANGUAGE()
                {
                    warn_printf("%s: warning: brace initializer with more than one element initializing type '%s'\n",
                            nodecl_locus_to_str(braced_initializer),
                            print_type_str(declared_type, decl_context));
                }
            }

            nodecl_t initializer_clause = nodecl_list_head(initializer_clause_list);
            nodecl_t nodecl_expr_out = nodecl_null();
            check_nodecl_initializer_clause(initializer_clause, decl_context, declared_type,
                    /* disallow_narrowing */ IS_CXX11_LANGUAGE,
                    &nodecl_expr_out);

            if (nodecl_is_err_expr(nodecl_expr_out))
            {
                *nodecl_output = nodecl_expr_out;
                return;
            }

            *nodecl_output = nodecl_make_structured_value(
                    nodecl_make_list_1(nodecl_expr_out),
                    /* structured-value-form */ is_explicit_type_cast
                    ? nodecl_make_structured_value_braced_typecast(locus)
                    : nodecl_make_structured_value_braced_implicit(locus),
                    nodecl_get_type(nodecl_expr_out),
                    locus);

            nodecl_set_constant(*nodecl_output, nodecl_get_constant(nodecl_expr_out));
            return;
        }
        else
        {
            // Empty is OK
            *nodecl_output = nodecl_make_structured_value(
                    nodecl_null(),
                    /* structured-value-form */ is_explicit_type_cast
                    ? nodecl_make_structured_value_braced_typecast(locus)
                    : nodecl_make_structured_value_braced_implicit(locus),
                    declared_type,
                    locus);
            return;
        }
    }

    internal_error("Code unreachable", 0);
}
#undef MAX_ITEM

typedef
struct designator_path_item_tag
{
    node_t kind;
    nodecl_t value;
} designator_path_item_t;

typedef
struct designator_path_tag
{
    int num_items;
    designator_path_item_t* items;
} designator_path_t;

static void check_nodecl_designation_type(nodecl_t nodecl_designation,
        decl_context_t decl_context, 
        type_t* declared_type,
        type_t** designated_type,
        nodecl_t* nodecl_output,
        designator_path_t* designator_path
        )
{ 
    (*designated_type) = declared_type;

    int num_designators = 0;
    nodecl_t* nodecl_designator_list = nodecl_unpack_list(nodecl_designation, &num_designators);

    ERROR_CONDITION(num_designators == 0, "Invalid number of designators", 0);

    if (designator_path != NULL)
    {
        designator_path->num_items = num_designators;
        designator_path->items = xcalloc(num_designators, sizeof(*designator_path->items));
    }

    int i;
    char ok = 1;
    for (i = 0; i < num_designators && ok; i++)
    {
        nodecl_t nodecl_current_designator = nodecl_designator_list[i];

        switch (nodecl_get_kind(nodecl_current_designator))
        {
            case NODECL_C99_FIELD_DESIGNATOR:
                {
                    if (!is_class_type(*designated_type))
                    {
                        error_printf("%s: in designated initializer '%s', field designator not valid for type '%s'\n",
                                nodecl_locus_to_str(nodecl_current_designator),
                                codegen_to_str(nodecl_current_designator, nodecl_retrieve_context(nodecl_current_designator)),
                                print_type_str(*designated_type, decl_context));
                        ok = 0;
                    }
                    else
                    {
                        nodecl_t nodecl_name = nodecl_get_child(nodecl_current_designator, 0);
                        scope_entry_list_t* entry_list = get_member_of_class_type_nodecl(
                                decl_context,
                                *designated_type,
                                nodecl_name,
                                // A field_path_t should not be necessary here
                                NULL);
                        if (entry_list == NULL)
                        {
                            ok = 0;
                        }
                        else
                        {
                            scope_entry_t* entry = entry_list_head(entry_list);

                            if (entry->kind == SK_VARIABLE)
                            {
                                (*designated_type)  = entry->type_information;

                                if (designator_path != NULL)
                                {
                                    designator_path->items[i].kind = NODECL_FIELD_DESIGNATOR;
                                    designator_path->items[i].value = 
                                        nodecl_make_symbol(entry, 
                                                nodecl_get_locus(nodecl_current_designator));
                                }
                            }
                            else
                            {
                                ok = 0;
                            }
                        }
                    }
                    break;
                }
            case NODECL_C99_INDEX_DESIGNATOR:
                {
                    if (!is_array_type(*designated_type))
                    {
                        error_printf("%s: in designated initializer '%s', subscript designator not valid for type '%s'\n",
                                nodecl_locus_to_str(nodecl_current_designator),
                                codegen_to_str(nodecl_current_designator, nodecl_retrieve_context(nodecl_current_designator)),
                                print_type_str(*designated_type, decl_context));
                        ok = 0;
                    }
                    else
                    {
                        *designated_type = array_type_get_element_type(*designated_type);

                        if (designator_path != NULL)
                        {
                            designator_path->items[i].kind = NODECL_INDEX_DESIGNATOR;
                            designator_path->items[i].value =
                                nodecl_shallow_copy(nodecl_get_child(nodecl_current_designator, 0));
                        }
                    }
                    break;
                }
            default:
                {
                    internal_error("Invalid nodecl '%s'\n", ast_print_node_type(nodecl_get_kind(nodecl_current_designator)));
                }
        }
    }

    xfree(nodecl_designator_list);

    if (!ok)
    {
        *nodecl_output = nodecl_make_err_expr(
                nodecl_get_locus(nodecl_designation));
        nodecl_free(nodecl_designation);
    }
}

void check_contextual_conversion(nodecl_t expression,
        type_t* dest_type,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    nodecl_t direct_init = nodecl_make_cxx_parenthesized_initializer(
            nodecl_make_list_1(expression),
            nodecl_get_locus(expression));

    check_nodecl_parenthesized_initializer(
            direct_init,
            decl_context,
            dest_type,
            /* is_explicit */ 0,
            /* is_explicit_type_cast */ 0,
            /* emit_cast */ 0,
            nodecl_output);
}

static void check_nodecl_parenthesized_initializer(nodecl_t direct_initializer,
        decl_context_t decl_context,
        type_t* declared_type,
        char is_explicit,
        char is_explicit_type_cast UNUSED_PARAMETER,
        char emit_cast,
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(nodecl_get_kind(direct_initializer) != NODECL_CXX_PARENTHESIZED_INITIALIZER, "Invalid nodecl", 0);

    if (is_dependent_type(declared_type))
    {
        *nodecl_output = direct_initializer;
        nodecl_set_type(*nodecl_output, declared_type);
        return;
    }

    nodecl_t nodecl_list = nodecl_get_child(direct_initializer, 0);

    const locus_t* locus = nodecl_get_locus(direct_initializer);

    char direct_initializer_is_dependent = nodecl_expr_is_type_dependent(direct_initializer);
    if (direct_initializer_is_dependent)
    {
        *nodecl_output = direct_initializer;
        nodecl_set_type(*nodecl_output, declared_type);
        return;
    }

    if (is_class_type(declared_type))
    {
        int num_arguments = nodecl_list_length(nodecl_list);
        type_t* arguments[MCXX_MAX_FUNCTION_CALL_ARGUMENTS] = { 0 };

        int i, num_items = 0;
        nodecl_t* list = nodecl_unpack_list(nodecl_list, &num_items);

        for (i = 0; i < num_items; i++)
        {
            nodecl_t nodecl_expr = list[i];

            arguments[i] = nodecl_get_type(nodecl_expr);
        }

        enum initialization_kind initialization_kind = IK_INVALID;
        if (num_items == 1)
        {
            if (is_class_type(no_ref(arguments[0]))
                    && class_type_is_derived_instantiating(
                        get_unqualified_type(no_ref(arguments[0])),
                        declared_type,
                        locus))
            {
                initialization_kind = IK_DIRECT_INITIALIZATION | IK_BY_CONSTRUCTOR;
            }
            else
            {
                initialization_kind = IK_DIRECT_INITIALIZATION | IK_BY_USER_DEFINED_CONVERSION;
            }
        }
        else
        {
            initialization_kind = IK_DIRECT_INITIALIZATION | IK_BY_CONSTRUCTOR;
        }

        scope_entry_list_t* candidates = NULL;
        scope_entry_t* chosen_constructor = NULL;
        char ok = solve_initialization_of_class_type(
                declared_type,
                arguments, num_arguments,
                initialization_kind,
                decl_context,
                locus,
                &chosen_constructor,
                &candidates);

        if (!ok)
        {
            if (entry_list_size(candidates) != 0)
            {
                int j = 0;
                const char* argument_types = "(";
                for (i = 0; i < num_arguments; i++)
                {
                    if (arguments[i] == NULL)
                        continue;

                    if (j > 0)
                        argument_types = strappend(argument_types, ", ");

                    argument_types = strappend(argument_types, print_type_str(arguments[i], decl_context));
                    j++;
                }
                argument_types = strappend(argument_types, ")");

                const char* message = NULL;
                uniquestr_sprintf(&message,
                        "%s: error: no suitable constructor in initialization '%s%s'\n",
                        locus_to_str(locus),
                        print_type_str(declared_type, decl_context),
                        argument_types);
                diagnostic_candidates(candidates, &message, locus);
                error_printf("%s", message);
            }

            entry_list_free(candidates);
            xfree(list);

            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }
        else
        {
            entry_list_free(candidates);
            if (function_has_been_deleted(decl_context, chosen_constructor, locus))
            {
                xfree(list);
                *nodecl_output = nodecl_make_err_expr(locus);
                return;
            }

            nodecl_t argument_list = nodecl_null();

            int num_parameters = function_type_get_num_parameters(chosen_constructor->type_information);
            if (function_type_get_has_ellipsis(chosen_constructor->type_information))
            {
                num_parameters--;
            }

            for (i = 0; i < num_arguments; i++)
            {
                nodecl_t nodecl_arg = list[i];

                if (i < num_parameters)
                {
                    type_t* param_type = function_type_get_parameter_type_num(chosen_constructor->type_information, i);

                    nodecl_t nodecl_old_arg = nodecl_arg;
                    check_nodecl_function_argument_initialization(nodecl_arg,
                            decl_context,
                            param_type,
                            /* disallow_narrowing */ 0,
                            &nodecl_arg);
                    if (nodecl_is_err_expr(nodecl_arg))
                    {
                        *nodecl_output = nodecl_arg;
                        nodecl_free(nodecl_old_arg);
                        return;
                    }
                }
                else
                {
                    type_t* default_argument_promoted_type = compute_default_argument_conversion(
                            nodecl_get_type(nodecl_arg),
                            decl_context,
                            nodecl_get_locus(nodecl_arg),
                            /* emit_diagnostic */ 1);

                    if (is_error_type(default_argument_promoted_type))
                    {
                        xfree(list);
                        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(direct_initializer));
                        return;
                    }
                }

                argument_list = nodecl_append_to_list(argument_list, nodecl_arg);
            }

            cv_qualifier_t cv_qualif = CV_NONE;
            type_t* actual_type = actual_type_of_conversor(chosen_constructor);
            advance_over_typedefs_with_cv_qualif(
                    declared_type,
                    &cv_qualif);
            actual_type = get_cv_qualified_type(actual_type, cv_qualif);

            *nodecl_output = cxx_nodecl_make_function_call(
                    nodecl_make_symbol(chosen_constructor, locus),
                    /* called name */ nodecl_null(),
                    argument_list,
                    is_explicit ? nodecl_null() : nodecl_make_cxx_function_form_implicit(locus),
                    actual_type,
                    decl_context,
                    locus);
        }
        xfree(list);
    }
    else
    {
        if (nodecl_list_length(nodecl_list) > 1)
        {
            error_printf("%s: error: too many initializers when initializing '%s' type\n", 
                    nodecl_locus_to_str(direct_initializer),
                    print_type_str(declared_type, decl_context));

            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(direct_initializer));
        }
        else if (nodecl_list_length(nodecl_list) == 1)
        {
            nodecl_t expr = nodecl_list_head(nodecl_list);

            nodecl_t nodecl_expr_out = nodecl_null();
            check_nodecl_expr_initializer(expr, decl_context, declared_type,
                    /* disallow_narrowing */ 0,
                    IK_DIRECT_INITIALIZATION,
                    &nodecl_expr_out);

            if (nodecl_is_err_expr(nodecl_expr_out))
            {
                *nodecl_output = nodecl_expr_out;
                return;
            }

            // We only build a cast if this is an explicit type cast
            if (emit_cast)
            {
                *nodecl_output = nodecl_make_cast(
                        nodecl_expr_out,
                        declared_type,
                        /* cast_kind */ "C",
                        nodecl_get_locus(nodecl_expr_out));
            }
            else
            {
                *nodecl_output = nodecl_expr_out;
            }

            nodecl_set_constant(*nodecl_output,
                    nodecl_get_constant(nodecl_expr_out));
            nodecl_expr_set_is_value_dependent(*nodecl_output,
                    nodecl_expr_is_value_dependent(nodecl_expr_out));
        }
        else
        {
            // Empty case int()
            *nodecl_output = nodecl_make_structured_value(nodecl_null(),
                    /* structured-value-form */ nodecl_make_structured_value_parenthesized(locus),
                    declared_type, nodecl_get_locus(direct_initializer));
        }
    }
}

static void compute_nodecl_initializer_clause(AST initializer,
        decl_context_t decl_context,
        char preserve_top_level_parentheses,
        nodecl_t* nodecl_output)
{
    switch (ASTType(initializer))
    {
        // Default is an expression
        default:
            {
                check_expression_impl_(initializer, decl_context, nodecl_output);

                if (nodecl_is_err_expr(*nodecl_output))
                    return;

                // We use this for decltype(auto)
                if (preserve_top_level_parentheses
                        && ASTType(initializer) == AST_PARENTHESIZED_EXPRESSION)
                {
                    *nodecl_output = cxx_nodecl_wrap_in_parentheses(*nodecl_output);
                }

                char is_type_dependent = nodecl_expr_is_type_dependent(*nodecl_output);
                char is_value_dependent = nodecl_expr_is_value_dependent(*nodecl_output);

                *nodecl_output = nodecl_make_cxx_initializer(
                        *nodecl_output,
                        nodecl_get_type(*nodecl_output),
                        nodecl_get_locus(*nodecl_output));

                // Propagate attributes as needed
                nodecl_expr_set_is_type_dependent(*nodecl_output, is_type_dependent);
                nodecl_expr_set_is_value_dependent(*nodecl_output, is_value_dependent);
                break;
            }
        case AST_INITIALIZER_BRACES:
            {
                compute_nodecl_braced_initializer(initializer, decl_context, nodecl_output);
                break;
            }
        case AST_DESIGNATED_INITIALIZER :
            {
                compute_nodecl_designated_initializer(initializer, decl_context, nodecl_output);
                break;
            }
        case AST_GCC_INITIALIZER_CLAUSE :
            {
                compute_nodecl_gcc_initializer(initializer, decl_context, nodecl_output);
                break;

            }
            // default: is at the beginning of this switch
    }
}

void check_initializer_clause(AST initializer,
        decl_context_t decl_context,
        type_t* declared_type,
        char is_decltype_auto,
        nodecl_t* nodecl_output)
{
    nodecl_t nodecl_init = nodecl_null();
    compute_nodecl_initializer_clause(initializer,
            decl_context,
            is_decltype_auto,
            &nodecl_init);
    check_nodecl_initializer_clause(nodecl_init, decl_context, declared_type,
            /* disallow_narrowing */ 0,
            nodecl_output);
}


static char operator_bin_pointer_to_pm_pred(type_t* lhs, type_t* rhs, const locus_t* locus)
{
    if (is_pointer_to_class_type(no_ref(lhs))
            && is_pointer_to_member_type(no_ref(rhs))
            && class_type_is_base_instantiating(pointer_type_get_pointee_type(no_ref(lhs)), 
                    pointer_to_member_type_get_class_type(no_ref(rhs)), locus))
    {
        return 1;
    }

    return 0;
}

static type_t* operator_bin_pointer_to_pm_result(type_t** lhs, type_t** rhs, const locus_t* locus UNUSED_PARAMETER)
{
    type_t* c1 = pointer_type_get_pointee_type(no_ref(*lhs));
    *lhs = no_ref(*lhs);
    type_t* t = pointer_type_get_pointee_type(no_ref(*rhs));
    *rhs = no_ref(*rhs);

    // Union of both CV qualifiers
    cv_qualifier_t result_cv = (get_cv_qualifier(c1) | get_cv_qualifier(t));

    return get_lvalue_reference_type(get_cv_qualified_type(t, result_cv));
}

static void check_nodecl_pointer_to_pointer_member(
        nodecl_t nodecl_lhs, 
        nodecl_t nodecl_rhs, 
        decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (nodecl_is_err_expr(nodecl_lhs) 
            || nodecl_is_err_expr(nodecl_rhs))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_lhs)
            || nodecl_expr_is_type_dependent(nodecl_rhs))
    {
        *nodecl_output = nodecl_make_cxx_arrow_ptr_member(
                nodecl_lhs,
                nodecl_rhs,
                get_unknown_dependent_type(),
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    type_t* lhs_type = nodecl_get_type(nodecl_lhs);
    type_t* rhs_type = nodecl_get_type(nodecl_rhs);

    // This is an awkward operator, it requires overload if nodecl_lhs is not a
    // pointer to a class or if nodecl_rhs is a class type (and not a pointer to
    // member type)

    char requires_overload = 0;

    requires_overload = (!is_pointer_to_class_type(no_ref(lhs_type))
            || (!is_pointer_to_member_type(no_ref(rhs_type))
                && is_class_type(no_ref(rhs_type))));

    if (!requires_overload)
    {
        if (!is_pointer_to_class_type(no_ref(lhs_type)))
        {
            error_printf("%s: error: '%s' does not have pointer to class type\n",
                    nodecl_locus_to_str(nodecl_lhs), codegen_to_str(nodecl_lhs, nodecl_retrieve_context(nodecl_lhs)));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_lhs);
            nodecl_free(nodecl_rhs);
            return;
        }

        if (!is_pointer_to_member_type(no_ref(rhs_type)))
        {
            error_printf("%s: error: '%s' is does not have pointer to member type\n",
                    nodecl_locus_to_str(nodecl_rhs), codegen_to_str(nodecl_rhs, nodecl_retrieve_context(nodecl_rhs)));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_lhs);
            nodecl_free(nodecl_rhs);
            return;
        }

        type_t* pm_class_type = 
            pointer_to_member_type_get_class_type(no_ref(rhs_type));

        type_t* pointed_lhs_type =
            pointer_type_get_pointee_type(no_ref(lhs_type));

        if (!equivalent_types(
                    get_actual_class_type(pm_class_type),
                    get_actual_class_type(pointed_lhs_type))
                && !class_type_is_base_instantiating(
                    get_actual_class_type(pm_class_type),
                    get_actual_class_type(pointed_lhs_type),
                    locus))
        {
            error_printf("%s: error: pointer to member of type '%s' is not compatible with an object of type '%s'\n",
                    locus_to_str(locus),
                    print_type_str(no_ref(rhs_type), decl_context), 
                    print_type_str(no_ref(pointed_lhs_type), decl_context));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_lhs);
            nodecl_free(nodecl_rhs);
            return;
        }

        type_t* pm_pointed_type = 
            pointer_type_get_pointee_type(no_ref(rhs_type));

        cv_qualifier_t cv_qualif_object = CV_NONE;
        advance_over_typedefs_with_cv_qualif(pointed_lhs_type, &cv_qualif_object);

        cv_qualifier_t cv_qualif_pointer = CV_NONE;
        advance_over_typedefs_with_cv_qualif(no_ref(rhs_type), &cv_qualif_pointer);

        type_t* t = lvalue_ref(
                    get_cv_qualified_type(
                        pm_pointed_type, 
                        cv_qualif_object | cv_qualif_pointer));

        *nodecl_output = nodecl_make_offset(
                nodecl_make_dereference(
                    nodecl_lhs,
                    lvalue_ref(pointed_lhs_type), 
                    nodecl_get_locus(nodecl_lhs)),
                nodecl_rhs,
                t, 
                locus);
        return;
    }

    // Solve the binary overload of 'operator->*'
    static AST operation_tree = NULL;
    if (operation_tree == NULL)
    {
        operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                ASTLeaf(AST_POINTER_DERREF_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
    }

    builtin_operators_set_t builtin_set; 
    build_binary_builtin_operators(
            lhs_type, rhs_type, 
            &builtin_set,
            decl_context, operation_tree, 
            operator_bin_pointer_to_pm_pred,
            operator_bin_pointer_to_pm_result,
            locus);

    scope_entry_list_t* builtins = get_entry_list_from_builtin_operator_set(&builtin_set);

    scope_entry_t* selected_operator = NULL;

    type_t* computed_type = compute_user_defined_bin_operator_type(operation_tree, 
            &nodecl_lhs, &nodecl_rhs, builtins, decl_context, locus, &selected_operator);

    entry_list_free(builtins);

    if (computed_type != NULL)
    {
        if (symbol_entity_specs_get_is_builtin(selected_operator))
        {
            *nodecl_output = nodecl_make_offset(
                    nodecl_make_dereference(
                        nodecl_lhs,
                        lvalue_ref(pointer_type_get_pointee_type(lhs_type)), 
                        nodecl_get_locus(nodecl_lhs)),
                    nodecl_rhs,
                    computed_type, locus);
        }
        else
        {
            *nodecl_output =
                    cxx_nodecl_make_function_call(
                        nodecl_rhs,
                        /* called name */ nodecl_null(),
                        nodecl_make_list_1(nodecl_lhs),
                        nodecl_make_cxx_function_form_unary_prefix(locus),
                        function_type_get_return_type(selected_operator->type_information),
                        decl_context,
                        locus);
        }

        return;
    }

    *nodecl_output = nodecl_make_err_expr(locus);
    nodecl_free(nodecl_lhs);
    nodecl_free(nodecl_rhs);
}

static void check_pointer_to_pointer_to_member(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST lhs = ASTSon0(expression);
    AST rhs = ASTSon1(expression);

    nodecl_t nodecl_lhs = nodecl_null();
    nodecl_t nodecl_rhs = nodecl_null();

    check_expression_impl_(lhs, decl_context, &nodecl_lhs);
    check_expression_impl_(rhs, decl_context, &nodecl_rhs);

    check_nodecl_pointer_to_pointer_member(nodecl_lhs, 
            nodecl_rhs, 
            decl_context, 
            ast_get_locus(expression),
            nodecl_output);
}

static void check_nodecl_pointer_to_member(
        nodecl_t nodecl_lhs, 
        nodecl_t nodecl_rhs, 
        decl_context_t decl_context, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (nodecl_is_err_expr(nodecl_lhs)
            || nodecl_is_err_expr(nodecl_rhs))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_lhs)
            || nodecl_expr_is_type_dependent(nodecl_lhs))
    {
        *nodecl_output = nodecl_make_cxx_dot_ptr_member(
                nodecl_lhs,
                nodecl_rhs,
                get_unknown_dependent_type(),
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    type_t* lhs_type = nodecl_get_type(nodecl_lhs);
    type_t* rhs_type = nodecl_get_type(nodecl_rhs);

    if (!is_class_type(no_ref(lhs_type)))
    {
        error_printf("%s: error: '%s' does not have class type\n",
                nodecl_locus_to_str(nodecl_lhs), codegen_to_str(nodecl_lhs, nodecl_retrieve_context(nodecl_lhs)));
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
        return;
    }
    if (!is_pointer_to_member_type(no_ref(rhs_type)))
    {
        error_printf("%s: error: '%s' is not a pointer to member\n",
                nodecl_locus_to_str(nodecl_rhs), codegen_to_str(nodecl_rhs, nodecl_retrieve_context(nodecl_rhs)));
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
        return;
    }

    type_t* pm_class_type = 
        pointer_to_member_type_get_class_type(no_ref(rhs_type));

    if (!equivalent_types(get_actual_class_type(no_ref(pm_class_type)), 
                get_actual_class_type(no_ref(lhs_type))) 
            && !class_type_is_base_instantiating(
                get_actual_class_type(no_ref(pm_class_type)),
                get_actual_class_type(no_ref(lhs_type)),
                locus))
    {
        error_printf("%s: error: pointer to member of type '%s' is not compatible with an object of type '%s'\n",
                locus_to_str(locus),
                print_type_str(no_ref(rhs_type), decl_context), print_type_str(no_ref(lhs_type), decl_context));
        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
        return;
    }

    cv_qualifier_t cv_qualif_object = CV_NONE;
    advance_over_typedefs_with_cv_qualif(no_ref(lhs_type), &cv_qualif_object);

    cv_qualifier_t cv_qualif_pointer = CV_NONE;
    advance_over_typedefs_with_cv_qualif(no_ref(rhs_type), &cv_qualif_pointer);

    type_t* t = lvalue_ref(
            get_cv_qualified_type(pointer_type_get_pointee_type(no_ref(rhs_type)), 
                cv_qualif_object | cv_qualif_pointer));

    *nodecl_output = nodecl_make_offset(nodecl_lhs,
            nodecl_rhs,
            t,
            locus);
}

static void check_pointer_to_member(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST lhs = ASTSon0(expression);
    AST rhs = ASTSon1(expression);

    nodecl_t nodecl_lhs = nodecl_null();
    nodecl_t nodecl_rhs = nodecl_null();

    check_expression_impl_(lhs, decl_context, &nodecl_lhs);
    check_expression_impl_(rhs, decl_context, &nodecl_rhs);

    check_nodecl_pointer_to_member(nodecl_lhs, nodecl_rhs, decl_context, 
            ast_get_locus(expression),
            nodecl_output);
}


static void compute_nodecl_equal_initializer(AST initializer,
        decl_context_t decl_context,
        char preserve_top_level_parentheses,
        nodecl_t* nodecl_output)
{
    AST expr = ASTSon0(initializer);

    compute_nodecl_initializer_clause(expr,
            decl_context,
            preserve_top_level_parentheses,
            nodecl_output);

    if (!nodecl_is_err_expr(*nodecl_output))
    {
        char is_type_dependent = nodecl_expr_is_type_dependent(*nodecl_output);
        char is_value_dependent = nodecl_expr_is_value_dependent(*nodecl_output);
        type_t* t = nodecl_get_type(*nodecl_output);

        *nodecl_output = nodecl_make_cxx_equal_initializer(*nodecl_output, 
                t, ast_get_locus(initializer));
        nodecl_expr_set_is_type_dependent(*nodecl_output, is_type_dependent);
        nodecl_expr_set_is_value_dependent(*nodecl_output, is_value_dependent);
    }
}

static void compute_nodecl_braced_initializer(AST initializer, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST initializer_list = ASTSon0(initializer);

    int num_types = 0;
    type_t** types = NULL;

    char any_is_type_dependent = 0;
    *nodecl_output = nodecl_null();
    if (initializer_list != NULL)
    {
        if (ASTType(initializer_list) == AST_AMBIGUITY)
        {
            char result = solve_ambiguous_list_of_initializer_clauses(initializer_list, decl_context, NULL);
            if (result == 0)
            {
                internal_error("Ambiguity not solved %s", ast_location(initializer));
            }
        }

        AST it;
        for_each_element(initializer_list, it)
        {
            AST initializer_clause = ASTSon1(it);

            nodecl_t nodecl_initializer_clause = nodecl_null();
            compute_nodecl_initializer_clause(initializer_clause, decl_context,
                    /* preserve_top_level_parentheses */ 0,
                    &nodecl_initializer_clause);

            if (nodecl_is_err_expr(nodecl_initializer_clause))
            {
                *nodecl_output = nodecl_initializer_clause;
                return;
            }

            any_is_type_dependent = any_is_type_dependent
                || nodecl_expr_is_type_dependent(nodecl_initializer_clause);

            *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_initializer_clause);

            P_LIST_ADD(types, num_types, nodecl_get_type(nodecl_initializer_clause));
        }
    }

    if (nodecl_is_null(*nodecl_output)
            || !nodecl_is_err_expr(*nodecl_output))
    {
        *nodecl_output = nodecl_make_cxx_braced_initializer(*nodecl_output,
                get_braced_list_type(num_types, types),
                ast_get_locus(initializer));
        nodecl_expr_set_is_type_dependent(*nodecl_output, any_is_type_dependent);
    }

    xfree(types);
}

static void compute_nodecl_designator_list(AST designator_list, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST it;
    for_each_element(designator_list, it)
    {
        nodecl_t nodecl_designator = nodecl_null();
        AST designator = ASTSon1(it);
        switch (ASTType(designator))
        {
            case AST_INDEX_DESIGNATOR:
                {
                    nodecl_t nodecl_cexpr = nodecl_null();
                    AST constant_expr = ASTSon0(designator);
                    check_expression_impl_(constant_expr, decl_context, &nodecl_cexpr);

                    if (nodecl_is_err_expr(nodecl_cexpr))
                    {
                        *nodecl_output = nodecl_make_err_expr(ast_get_locus(designator));
                        return;
                    }

                    nodecl_designator = nodecl_make_c99_index_designator(nodecl_cexpr, 
                            nodecl_get_locus(nodecl_cexpr));
                    break;
                }
            case AST_FIELD_DESIGNATOR:
                {
                    AST symbol = ASTSon0(designator);

                    nodecl_designator = nodecl_make_c99_field_designator(
                            nodecl_make_cxx_dep_name_simple(ASTText(symbol), 
                                ast_get_locus(symbol)), 
                            ast_get_locus(symbol));
                    break;
                }
            default:
                {
                    internal_error("Unexpected node kind '%s'\n", ast_print_node_type(ASTType(designator)));
                }
        }

        *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_designator);
    }
}

static void compute_nodecl_designation(AST designation, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST designator_list = ASTSon0(designation);
    compute_nodecl_designator_list(designator_list, decl_context, nodecl_output);
}

static void compute_nodecl_designated_initializer(AST initializer, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST designation = ASTSon0(initializer);
    AST initializer_clause = ASTSon1(initializer);

    nodecl_t nodecl_designation = nodecl_null();
    compute_nodecl_designation(designation, decl_context, &nodecl_designation);
    nodecl_t nodecl_initializer_clause = nodecl_null();
    compute_nodecl_initializer_clause(initializer_clause, decl_context,
            /* preserve_top_level_parentheses */ 0,
            &nodecl_initializer_clause);

    if (nodecl_is_err_expr(nodecl_designation)
            || nodecl_is_err_expr(nodecl_initializer_clause))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(initializer));
        return;
    }

    *nodecl_output = nodecl_make_c99_designated_initializer(
            nodecl_designation, 
            nodecl_initializer_clause, 
            ast_get_locus(initializer));
}

static void compute_nodecl_gcc_initializer(AST initializer, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST symbol = ASTSon0(initializer);
    AST initializer_clause = ASTSon1(initializer);

    // Simplify it as if it were a standard C99 designator
    nodecl_t nodecl_designation = 
        nodecl_make_list_1(
                nodecl_make_c99_field_designator(
                    nodecl_make_cxx_dep_name_simple(ASTText(symbol), 
                        ast_get_locus(symbol)),
                    ast_get_locus(symbol)));
    nodecl_t nodecl_initializer_clause = nodecl_null();
    compute_nodecl_initializer_clause(initializer_clause, decl_context,
            /* preserve_top_level_parentheses */ 0,
            &nodecl_initializer_clause);

    if (nodecl_is_err_expr(nodecl_designation)
            || nodecl_is_err_expr(nodecl_initializer_clause))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(initializer));
        return;
    }

    *nodecl_output = nodecl_make_c99_designated_initializer(
            nodecl_designation, 
            nodecl_initializer_clause, 
            ast_get_locus(initializer));
}

static void compute_nodecl_direct_initializer(AST initializer,
        decl_context_t decl_context,
        char preserve_top_level_parentheses,
        nodecl_t* nodecl_output)
{
    char any_is_type_dependent = 0;
    char any_is_value_dependent = 0;
    nodecl_t nodecl_initializer_list = nodecl_null();

    AST initializer_list = ASTSon0(initializer);
    if (initializer_list != NULL)
    {
        if (!check_list_of_expressions_aux(initializer_list, decl_context,
                    preserve_top_level_parentheses,
                    &nodecl_initializer_list))
        {
            *nodecl_output = nodecl_make_err_expr(
                    ast_get_locus(initializer));
            return;
        }

        int num_items = 0;
        if (!nodecl_is_null(nodecl_initializer_list))
        {
            nodecl_t* nodecl_list = nodecl_unpack_list(nodecl_initializer_list, &num_items);
            for (int i = 0; i < num_items; ++i)
            {
                nodecl_t current_nodecl = nodecl_list[i];
                if (nodecl_is_err_expr(current_nodecl))
                {
                    *nodecl_output = current_nodecl;
                    xfree(nodecl_list);
                    return;
                }

                any_is_type_dependent = any_is_type_dependent ||
                    nodecl_expr_is_type_dependent(current_nodecl);
                any_is_value_dependent = any_is_value_dependent ||
                    nodecl_expr_is_value_dependent(current_nodecl);
            }
            xfree(nodecl_list);
        }
    }

    *nodecl_output = nodecl_make_cxx_parenthesized_initializer(
            nodecl_initializer_list,
            ast_get_locus(initializer));

    nodecl_expr_set_is_type_dependent(*nodecl_output, any_is_type_dependent);
    nodecl_expr_set_is_value_dependent(*nodecl_output, any_is_value_dependent);
}

void compute_nodecl_initialization(AST initializer,
        decl_context_t decl_context,
        char preserve_top_level_parentheses,
        nodecl_t* nodecl_output)
{
    switch (ASTType(initializer))
    {
        case AST_EQUAL_INITIALIZER:
            {
                compute_nodecl_equal_initializer(initializer,
                        decl_context,
                        preserve_top_level_parentheses,
                        nodecl_output);
                break;
            }
        case AST_INITIALIZER_BRACES:
            {
                compute_nodecl_braced_initializer(initializer,
                        decl_context,
                        nodecl_output);
                break;
            }
        case AST_PARENTHESIZED_INITIALIZER :
            {
                compute_nodecl_direct_initializer(initializer,
                        decl_context,
                        preserve_top_level_parentheses,
                        nodecl_output);
                break;
            }
        default:
            {
                internal_error("Unexpected node '%s'\n", ast_print_node_type(ASTType(initializer)));
            }
    }
}

static void unary_record_conversion_to_result_for_initializer(type_t* result, nodecl_t* op)
{
    type_t* op_type = nodecl_get_type(*op);

    // Do not record array conversions here
    if (!is_array_type(result)
            && !equivalent_types(result, op_type))
    {
        *op = cxx_nodecl_make_conversion(*op, result,
                nodecl_get_locus(*op));
    }
}

static void check_nodecl_function_argument_initialization_(
        nodecl_t nodecl_expr,
        decl_context_t decl_context,
        type_t* declared_type,
        enum initialization_kind initialization_kind,
        char disallow_narrowing,
        nodecl_t* nodecl_output)
{
    // We forward NODECL_CXX_BRACED_INITIALIZER to check_nodecl_braced_initializer
    if (nodecl_get_kind(nodecl_expr) == NODECL_CXX_BRACED_INITIALIZER)
    {
        check_nodecl_braced_initializer(nodecl_expr, decl_context, declared_type,
                /* is_explicit_type_cast */ 0,
                initialization_kind,
                nodecl_output);
    }
    else
    {
        check_nodecl_expr_initializer(nodecl_expr,
                decl_context,
                declared_type,
                disallow_narrowing,
                initialization_kind,
                nodecl_output);
    }
}

void check_nodecl_function_argument_initialization(
        nodecl_t nodecl_expr,
        decl_context_t decl_context,
        type_t* declared_type,
        char disallow_narrowing,
        nodecl_t* nodecl_output)
{
    return check_nodecl_function_argument_initialization_(
            nodecl_expr,
            decl_context,
            declared_type,
            IK_COPY_INITIALIZATION,
            disallow_narrowing,
            nodecl_output);
}

void check_nodecl_expr_initializer(nodecl_t nodecl_expr,
        decl_context_t decl_context, 
        type_t* declared_type, 
        char disallow_narrowing,
        enum initialization_kind initialization_kind,
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(nodecl_get_kind(nodecl_expr) == NODECL_CXX_BRACED_INITIALIZER,
            "Do not call this function using a NODECL_CXX_BRACED_INITIALIZER", 0);

    if (is_error_type(declared_type))
    {
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
        return;
    }

    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Conversion from expression '%s' with type '%s' to type '%s'\n",
                codegen_to_str(nodecl_expr, nodecl_retrieve_context(nodecl_expr)),
                print_declarator(nodecl_get_type(nodecl_expr)),
                print_declarator(declared_type));
    }
    const locus_t* locus = nodecl_get_locus(nodecl_expr);
    type_t* initializer_expr_type = nodecl_get_type(nodecl_expr);
    type_t* declared_type_no_cv = get_unqualified_type(declared_type);

    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_expr;
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_expr))
    {
        *nodecl_output = nodecl_expr;
        return;
    }

    if (is_dependent_type(declared_type_no_cv))
    {
        *nodecl_output = nodecl_expr;
        return;
    }

    // Now we have to check whether this can be converted to the declared entity
    C_LANGUAGE()
    {
        standard_conversion_t standard_conversion_sequence;

        char can_be_initialized = 
            (is_string_literal_type(initializer_expr_type)
             && is_array_type(declared_type_no_cv)
             && ((is_character_type(array_type_get_element_type(declared_type_no_cv))
                     && is_character_type(array_type_get_element_type(no_ref(initializer_expr_type))))
                 || (is_wchar_t_type(array_type_get_element_type(declared_type_no_cv))
                     && is_wchar_t_type(array_type_get_element_type(no_ref(initializer_expr_type))))))
            || standard_conversion_between_types(
                    &standard_conversion_sequence,
                    initializer_expr_type,
                    declared_type_no_cv,
                    locus);

        if (!can_be_initialized)
        {
            error_printf("%s: error: initializer '%s' has type '%s' not convertible to '%s'\n",
                    nodecl_locus_to_str(nodecl_expr),
                    codegen_to_str(nodecl_expr, nodecl_retrieve_context(nodecl_expr)),
                    print_decl_type_str(initializer_expr_type, decl_context, ""),
                    print_decl_type_str(declared_type, decl_context, ""));
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
            return;
        }

        *nodecl_output = nodecl_expr;
        unary_record_conversion_to_result_for_initializer(declared_type_no_cv, nodecl_output);
        return;
    }

    if (!is_class_type(declared_type_no_cv))
    {
        scope_entry_t* conversor = NULL;
        scope_entry_list_t* candidates = NULL;
        char can_be_initialized =
            (is_string_literal_type(initializer_expr_type)
             && is_array_type(declared_type_no_cv)
             && ((is_character_type(array_type_get_element_type(declared_type_no_cv))
                     && is_character_type(array_type_get_element_type(no_ref(initializer_expr_type))))
                 || (is_wchar_t_type(array_type_get_element_type(declared_type_no_cv))
                     && is_wchar_t_type(array_type_get_element_type(no_ref(initializer_expr_type))))
                 || (is_char16_t_type(array_type_get_element_type(declared_type_no_cv))
                     && is_char16_t_type(array_type_get_element_type(no_ref(initializer_expr_type))))
                 || (is_char32_t_type(array_type_get_element_type(declared_type_no_cv))
                     && is_char32_t_type(array_type_get_element_type(no_ref(initializer_expr_type))))))
            || solve_initialization_of_nonclass_type(
                        initializer_expr_type,
                        declared_type_no_cv,
                        decl_context,
                        initialization_kind | IK_BY_USER_DEFINED_CONVERSION,
                        &conversor,
                        &candidates,
                        nodecl_get_locus(nodecl_expr));

        if (!can_be_initialized)
        {
            error_printf("%s: error: initializer '%s' has type '%s' not convertible to '%s'\n",
                    nodecl_locus_to_str(nodecl_expr),
                    codegen_to_str(nodecl_expr, nodecl_retrieve_context(nodecl_expr)),
                    print_decl_type_str(initializer_expr_type, decl_context, ""),
                    print_decl_type_str(declared_type, decl_context, ""));

            if (entry_list_size(candidates) != 0)
            {
                const char* message = NULL;
                uniquestr_sprintf(&message,
                        "%s: error: no suitable conversion for initialization of type '%s' "
                        "using an expression of type '%s'\n",
                        locus_to_str(nodecl_get_locus(nodecl_expr)),
                        print_type_str(declared_type_no_cv, decl_context),
                        print_type_str(initializer_expr_type, decl_context));
                diagnostic_candidates(candidates, &message, nodecl_get_locus(nodecl_expr));
                error_printf("%s", message);
            }
            entry_list_free(candidates);

            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }

        if (conversor != NULL)
        {
            if (function_has_been_deleted(decl_context, conversor, locus))
            {
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
                return;
            }

            *nodecl_output = cxx_nodecl_make_function_call(
                    nodecl_make_symbol(conversor,
                        nodecl_get_locus(nodecl_expr)),
                    /* called name */ nodecl_null(),
                    nodecl_make_list_1(nodecl_expr),
                    nodecl_make_cxx_function_form_implicit(
                        nodecl_get_locus(nodecl_expr)),
                    actual_type_of_conversor(conversor),
                    decl_context,
                    nodecl_get_locus(nodecl_expr));

            if (disallow_narrowing)
            {
                check_narrowing_conversion(
                        *nodecl_output,
                        declared_type_no_cv,
                        decl_context);
            }
        }
        else
        {
            if (is_unresolved_overloaded_type(initializer_expr_type))
            {
                update_unresolved_overload_argument(initializer_expr_type,
                        declared_type_no_cv,
                        decl_context,
                        nodecl_get_locus(nodecl_expr),
                        &nodecl_expr);
            }

            *nodecl_output = nodecl_expr;

            if (disallow_narrowing)
            {
                check_narrowing_conversion(
                        *nodecl_output,
                        declared_type_no_cv,
                        decl_context);
            }
            unary_record_conversion_to_result_for_initializer(declared_type_no_cv, nodecl_output);
        }
    }
    else // is_class_type(declared_type_no_cv)
    {
        // Use a constructor
        int num_arguments = 1;
        type_t* arguments[MCXX_MAX_FUNCTION_CALL_ARGUMENTS] = { 0 };

        arguments[0] = initializer_expr_type;

        if (is_class_type(no_ref(initializer_expr_type))
                && class_type_is_derived_instantiating(
                    get_unqualified_type(no_ref(initializer_expr_type)),
                    declared_type,
                    locus))
        {
            initialization_kind |= IK_BY_CONSTRUCTOR;
        }
        else
        {
            initialization_kind |= IK_BY_USER_DEFINED_CONVERSION;
        }

        scope_entry_list_t* candidates = NULL;
        scope_entry_t* chosen_conversor = NULL;
        char ok = solve_initialization_of_class_type(
                declared_type_no_cv,
                arguments, num_arguments,
                initialization_kind,
                decl_context,
                nodecl_get_locus(nodecl_expr),
                &chosen_conversor,
                &candidates);

        if (!ok)
        {
            if (entry_list_size(candidates) != 0)
            {
                const char* message = NULL;
                uniquestr_sprintf(&message,
                        "%s: error: no suitable constructor for initialization of type '%s' "
                        "using an expression of type '%s'\n",
                        locus_to_str(nodecl_get_locus(nodecl_expr)),
                        print_type_str(declared_type_no_cv, decl_context),
                        print_type_str(initializer_expr_type, decl_context));
                diagnostic_candidates(candidates, &message, nodecl_get_locus(nodecl_expr));
                error_printf("%s", message);
            }
            entry_list_free(candidates);

            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
            return;
        }
        else
        {
            entry_list_free(candidates);

            if (function_has_been_deleted(decl_context, chosen_conversor, locus))
            {
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
                return;
            }

            if (function_has_been_deleted(decl_context, chosen_conversor, nodecl_get_locus(nodecl_expr)))
            {
                *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
                return;
            }

            if (symbol_entity_specs_get_is_constructor(chosen_conversor))
            {
                type_t* param_type = function_type_get_parameter_type_num(chosen_conversor->type_information, 0);
                check_nodecl_function_argument_initialization(nodecl_expr,
                        decl_context,
                        param_type,
                        /* disallow_narrowing */ 0,
                        &nodecl_expr);

                ERROR_CONDITION(nodecl_is_err_expr(nodecl_expr),
                        "We have chosen a constructor that cannot be called", 0);
            }
        }

        // Remember a call to the constructor here
        *nodecl_output = cxx_nodecl_make_function_call(
                nodecl_make_symbol(chosen_conversor, nodecl_get_locus(nodecl_expr)),
                /* called name */ nodecl_null(),
                nodecl_make_list_1(nodecl_expr),
                nodecl_make_cxx_function_form_implicit(nodecl_get_locus(nodecl_expr)),
                declared_type_no_cv,
                decl_context,
                nodecl_get_locus(nodecl_expr));
    }
}

void check_nodecl_equal_initializer(nodecl_t nodecl_initializer, 
        decl_context_t decl_context, 
        type_t* declared_type, 
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(nodecl_get_kind(nodecl_initializer) != NODECL_CXX_EQUAL_INITIALIZER, "Invalid nodecl", 0);

    if (nodecl_get_kind(nodecl_initializer) == NODECL_CXX_INITIALIZER
            && (nodecl_expr_is_type_dependent(nodecl_initializer)
                || nodecl_expr_is_value_dependent(nodecl_initializer)))
    {
        // If this is a simple expression that turns to be dependent, then
        // preserve its equal initializer
        // template <typename T> void f() { long t = sizeof(T); }
        *nodecl_output = nodecl_initializer;
    }
    else
    {
        nodecl_t nodecl_expr = nodecl_get_child(nodecl_initializer, 0);
        check_nodecl_initializer_clause(nodecl_expr, decl_context, declared_type,
               /* disallow_narrowing */ 0, nodecl_output);
    }
}

type_t* deduce_auto_initializer(
        nodecl_t nodecl_initializer,
        type_t* type_to_deduce,
        decl_context_t decl_context)
{
    ERROR_CONDITION(nodecl_is_null(nodecl_initializer), "Initializer cannot be NULL", 0);
    nodecl_t nodecl_expression_used_for_deduction = nodecl_initializer;

    char is_braced_initializer = nodecl_get_kind(nodecl_initializer) == NODECL_CXX_BRACED_INITIALIZER
        ||(nodecl_get_kind(nodecl_initializer) == NODECL_CXX_EQUAL_INITIALIZER
                && nodecl_get_kind(nodecl_get_child(nodecl_initializer, 0)) == NODECL_CXX_BRACED_INITIALIZER);

    if (nodecl_get_kind(nodecl_expression_used_for_deduction) == NODECL_CXX_PARENTHESIZED_INITIALIZER)
    {
        nodecl_t nodecl_list = nodecl_get_child(nodecl_expression_used_for_deduction, 0);
        if (nodecl_list_length(nodecl_list) != 1)
        {
            error_printf("%s: error: 'auto' deduction with a parenthesized "
                    "initializer is only possible with one element inside the parentheses\n",
                    nodecl_locus_to_str(nodecl_expression_used_for_deduction));
            return get_error_type();
        }
        nodecl_expression_used_for_deduction = nodecl_list_head(nodecl_list);
    }

    template_parameter_list_t* deduced_template_arguments = NULL;

    if (nodecl_get_type(nodecl_expression_used_for_deduction) != NULL
            && deduce_arguments_of_auto_initialization(
                type_to_deduce,
                nodecl_get_type(nodecl_expression_used_for_deduction),
                decl_context,
                &deduced_template_arguments,
                is_braced_initializer,
                nodecl_get_locus(nodecl_expression_used_for_deduction)))
    {
        type_t* deduced_type = NULL;
        if (!is_braced_initializer)
        {
            // const auto& -> const int&
            deduced_type = update_type_for_auto(
                    type_to_deduce,
                    deduced_template_arguments->arguments[0]->type);
        }
        else
        {
            // const auto& -> const std::initializer_list<T>
            deduced_type = template_type_get_specialized_type(
                    get_std_initializer_list_template(decl_context,
                        nodecl_get_locus(nodecl_expression_used_for_deduction),
                        /* mandatory */ 1)->type_information,
                    deduced_template_arguments,
                    decl_context,
                    nodecl_get_locus(nodecl_expression_used_for_deduction));
            free_template_parameter_list(deduced_template_arguments);
        }

        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Deduced type for auto initializer is '%s'\n",
                    print_declarator(deduced_type));
        }
        return deduced_type;
    }
    else
    {
        error_printf("%s: error: failure when deducing type of '%s' from '%s'\n",
                nodecl_locus_to_str(nodecl_initializer),
                print_type_str(type_to_deduce, decl_context),
                codegen_to_str(nodecl_initializer, decl_context));
        return get_error_type();
    }
}

type_t* deduce_decltype_auto_initializer(
        nodecl_t nodecl_initializer,
        type_t* type_to_deduce, // this should just be decltype(auto)
        decl_context_t decl_context)
{
    ERROR_CONDITION(nodecl_is_null(nodecl_initializer), "Initializer cannot be NULL", 0);
    ERROR_CONDITION(!is_decltype_auto_type(type_to_deduce), "Invalid type", 0)

    nodecl_t nodecl_expression_used_for_deduction = nodecl_initializer;

    if (nodecl_get_kind(nodecl_initializer) == NODECL_CXX_PARENTHESIZED_INITIALIZER)
    {
        nodecl_t nodecl_list = nodecl_get_child(nodecl_expression_used_for_deduction, 0);
        if (nodecl_list_length(nodecl_list) != 1)
        {
            error_printf("%s: error: 'decltype(auto)' deduction with a parenthesized "
                    "initializer is only possible with one element inside the parentheses\n",
                    nodecl_locus_to_str(nodecl_expression_used_for_deduction));
            return get_error_type();
            // *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_initializer));
            // nodecl_free(nodecl_initializer);
            // return;
        }
        nodecl_expression_used_for_deduction = nodecl_list_head(nodecl_list);
    }
    else if (nodecl_get_kind(nodecl_initializer) == NODECL_CXX_BRACED_INITIALIZER)
    {
        error_printf("%s: error: cannot deduce 'decltype(auto)' using a braced initializer\n",
                nodecl_locus_to_str(nodecl_expression_used_for_deduction));
        return get_error_type();
    }
    else if (nodecl_get_kind(nodecl_initializer) == NODECL_CXX_EQUAL_INITIALIZER)
    {
        nodecl_expression_used_for_deduction = nodecl_get_child(nodecl_initializer, 0);
        ERROR_CONDITION(nodecl_get_kind(nodecl_expression_used_for_deduction) != NODECL_CXX_INITIALIZER,
                "Invalid node '%s'",
                ast_print_node_type(nodecl_get_kind(nodecl_expression_used_for_deduction)));
        nodecl_expression_used_for_deduction = nodecl_get_child(nodecl_expression_used_for_deduction, 0);
    }

    ERROR_CONDITION(nodecl_get_kind(nodecl_expression_used_for_deduction) == NODECL_CXX_INITIALIZER
            || nodecl_get_kind(nodecl_expression_used_for_deduction) == NODECL_CXX_BRACED_INITIALIZER
            || nodecl_get_kind(nodecl_expression_used_for_deduction) == NODECL_CXX_PARENTHESIZED_INITIALIZER
            || nodecl_get_kind(nodecl_expression_used_for_deduction) == NODECL_CXX_EQUAL_INITIALIZER,
            "Invalid node at this point '%s'",
            ast_print_node_type(nodecl_get_kind(nodecl_expression_used_for_deduction)));

    return compute_type_of_decltype_nodecl(nodecl_expression_used_for_deduction, decl_context);
}

type_t* compute_type_of_decltype_nodecl(nodecl_t nodecl_expr, decl_context_t decl_context)
{
    if (nodecl_is_err_expr(nodecl_expr))
    {
        error_printf("%s: error: failure when computing type of decltype(%s)\n",
                nodecl_locus_to_str(nodecl_expr),
                codegen_to_str(nodecl_expr, decl_context));
        return get_error_type();
    }

    type_t* computed_type = nodecl_get_type(nodecl_expr);
    ERROR_CONDITION(computed_type == NULL, "Invalid type", 0);

    if (is_unresolved_overloaded_type(computed_type))
    {
        scope_entry_t* solved_function = unresolved_overloaded_type_simplify(
                computed_type,
                decl_context,
                nodecl_get_locus(nodecl_expr));

        if (solved_function != NULL)
        {
            if (!symbol_entity_specs_get_is_member(solved_function)
                    || symbol_entity_specs_get_is_static(solved_function))
            {
                computed_type = solved_function->type_information;
            }
            else
            {
                computed_type = get_pointer_to_member_type(
                        solved_function->type_information,
                        symbol_entity_specs_get_class_type(solved_function));
            }
        }
    }

    if (is_unresolved_overloaded_type(computed_type))
    {
        error_printf("%s: error: decltype(%s) yields an unresolved overload type\n",
                nodecl_locus_to_str(nodecl_expr),
                codegen_to_str(nodecl_expr, decl_context));
        return get_error_type();
    }

    if (is_dependent_type(computed_type))
    {
        return get_typeof_expr_dependent_type(nodecl_expr,
                decl_context,
                /* is_decltype */ 1);
    }

    if (nodecl_get_kind(nodecl_expr) == NODECL_SYMBOL
            || nodecl_get_kind(nodecl_expr) == NODECL_CLASS_MEMBER_ACCESS)
    {
        return no_ref(computed_type);
    }
    else
    {
        return computed_type;
    }
}

static char initializer_self_references(nodecl_t initializer, scope_entry_t* entry)
{
    if (nodecl_is_null(initializer))
        return 0;

    if (nodecl_get_symbol(initializer) == entry)
        return 1;

    int i;
    for (i = 0; i < MCXX_MAX_AST_CHILDREN; i++)
    {
        if (initializer_self_references(nodecl_get_child(initializer, i), entry))
            return 1;
    }

    return 0;
}

void check_nodecl_initialization(
        nodecl_t nodecl_initializer,
        decl_context_t decl_context,
        scope_entry_t* initialized_entry, // May have its type_information updated
        type_t* declared_type,
        nodecl_t* nodecl_output,
        char is_auto,
        char is_decltype_auto)
{
    if (!nodecl_is_null(nodecl_initializer)
            && nodecl_is_err_expr(nodecl_initializer))
    {
        *nodecl_output = nodecl_initializer;
        return;
    }

    if (is_dependent_type(declared_type))
    {
        // The declared entity is dependent
        *nodecl_output = nodecl_initializer;
        return;
    }

    // Note that we do not early return in cases like
    //
    // template <typename T>
    // void f(T t)
    // {
    //    int a[] = { t };
    // }
    //
    // otherwise the size of 'a' would be wrongly computed as an array of
    // unknown size

    if (is_auto)
    {
        if (!nodecl_is_null(nodecl_initializer)
                && nodecl_expr_is_type_dependent(nodecl_initializer))
        {
            *nodecl_output = nodecl_initializer;
            return;
        }

        if (initializer_self_references(nodecl_initializer, initialized_entry))
        {
            error_printf("%s: error: an auto declaration initializer cannot reference the initialized name\n",
                    nodecl_locus_to_str(nodecl_initializer));
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_initializer));
            nodecl_free(nodecl_initializer);
            return;
        }

        type_t* deduced_type = NULL;
        if (is_decltype_auto)
        {
            deduced_type = deduce_decltype_auto_initializer(
                    nodecl_initializer,
                    declared_type,
                    decl_context);
        }
        else
        {
            deduced_type = deduce_auto_initializer(
                    nodecl_initializer,
                    declared_type,
                    decl_context);
        }

        if (is_error_type(deduced_type))
        {
            *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_initializer));
            nodecl_free(nodecl_initializer);
            return;
        }

        if (initialized_entry != NULL)
        {
            initialized_entry->type_information = deduced_type;
        }
        declared_type = deduced_type;
    }

    switch (nodecl_get_kind(nodecl_initializer))
    {
        case NODECL_CXX_EQUAL_INITIALIZER:
            {
                check_nodecl_equal_initializer(nodecl_initializer, decl_context, declared_type, nodecl_output);
                break;
            }
        case NODECL_CXX_BRACED_INITIALIZER:
            {
                check_nodecl_braced_initializer(nodecl_initializer, decl_context, declared_type,
                        /* is_explicit_type_cast */ 0, IK_DIRECT_INITIALIZATION, nodecl_output);
                break;
            }
        case NODECL_CXX_PARENTHESIZED_INITIALIZER:
            {
                check_nodecl_parenthesized_initializer(nodecl_initializer, decl_context, declared_type,
                        /* is_explicit */ 0, /* is_explicit_type_cast */ 0, /* emit_cast */ 0,
                        nodecl_output);
                break;
            }
        default:
            {
                internal_error("Unexpected initializer", 0);
            }
    }
}

static void check_nodecl_initializer_clause(
        nodecl_t initializer_clause, 
        decl_context_t decl_context, 
        type_t* declared_type, 
        char disallow_narrowing,
        nodecl_t* nodecl_output)
{
    if (nodecl_is_err_expr(initializer_clause))
    {
        *nodecl_output = initializer_clause;
        return;
    }

    switch (nodecl_get_kind(initializer_clause))
    {
        case NODECL_CXX_INITIALIZER:
            {
                // This node wraps a simple expression
                check_nodecl_expr_initializer(
                        nodecl_get_child(initializer_clause, 0), 
                        decl_context, declared_type,
                        disallow_narrowing,
                        IK_COPY_INITIALIZATION,
                        nodecl_output);
                break;
            }
        case NODECL_CXX_BRACED_INITIALIZER:
            {
                check_nodecl_braced_initializer(initializer_clause, decl_context, declared_type,
                        /* is_explicit_type_cast */ 0,
                        IK_COPY_INITIALIZATION,
                        nodecl_output);
                break;
            }
        default:
            {
                internal_error("Unexpected nodecl '%s'\n", ast_print_node_type(nodecl_get_kind(initializer_clause)));
            }
    }
}


char check_initialization(AST initializer,
        decl_context_t decl_context,
        scope_entry_t* initialized_entry,
        type_t* declared_type,
        nodecl_t* nodecl_output,
        char is_auto,
        char is_decltype_auto)
{
    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Checking initializer '%s'\n",
                prettyprint_in_buffer(initializer));
    }

    nodecl_t nodecl_init = nodecl_null();

    compute_nodecl_initialization(initializer, decl_context, 
            /* preserve_top_level_parentheses */ is_decltype_auto,
            &nodecl_init);
    check_nodecl_initialization(nodecl_init,
            decl_context,
            initialized_entry,
            declared_type,
            nodecl_output,
            is_auto,
            is_decltype_auto);

    DEBUG_CODE()
    {
        if (!nodecl_is_err_expr(*nodecl_output))
        {
            fprintf(stderr, "EXPRTYPE: Initializer '%s' has type '%s'",
                    prettyprint_in_buffer(initializer),
                    nodecl_get_type(*nodecl_output) == NULL
                    ? "<< no type >>"
                    : print_declarator(nodecl_get_type(*nodecl_output)));

            if (nodecl_is_constant(*nodecl_output))
            {
                const_value_t* v = nodecl_get_constant(*nodecl_output);
                fprintf(stderr, " with a constant value '%s'",
                        const_value_to_str(v));
            }
            if (nodecl_expr_is_type_dependent(*nodecl_output))
            {
                fprintf(stderr, " [TYPE DEPENDENT]");
            }
            if (nodecl_expr_is_value_dependent(*nodecl_output))
            {
                fprintf(stderr, " [VALUE DEPENDENT]");
            }
            fprintf(stderr, "\n");
        }
        else
        {
            fprintf(stderr, "EXPRTYPE: Initializer '%s' does not have any computed type\n",
                    prettyprint_in_buffer(initializer));
        }
    }

    if (CURRENT_CONFIGURATION->strict_typecheck
            && nodecl_is_err_expr(*nodecl_output))
    {
        internal_error("Initializer '%s' at '%s' does not have a valid computed type\n",
                prettyprint_in_buffer(initializer),
                ast_location(initializer));
    }

    return !nodecl_is_err_expr(*nodecl_output);
}

AST advance_expression_nest(AST expr)
{
    return advance_expression_nest_flags(expr, 1);
}

AST advance_expression_nest_flags(AST expr, char advance_parentheses)
{
    AST result = expr;

    for ( ; ; )
    {
        switch (ASTType(result))
        {
            case AST_EXPRESSION : 
            case AST_CONSTANT_EXPRESSION : 
                {
                    result = ASTSon0(result);
                    break;
                }
            case AST_PARENTHESIZED_EXPRESSION :
                {
                    if (advance_parentheses)
                    {
                        result = ASTSon0(result);
                    }
                    else
                    {
                        return result;
                    }
                    break;
                }
            default:
                return result;
        }
    }
}

static void accessible_types_through_conversion(type_t* t, type_t ***result, int *num_types, decl_context_t decl_context,
        const locus_t* locus)
{
    ERROR_CONDITION(is_unresolved_overloaded_type(t), 
            "Do not invoke this function on unresolved overloaded types", 0);
    ERROR_CONDITION(is_dependent_type(t), "Do not invoke this function on dependent types", 0);

    (*num_types) = 0;

    ERROR_CONDITION(is_lvalue_reference_type(t), "Reference types should have been removed here", 0);

    if (is_unscoped_enum_type(t))
    {
        P_LIST_ADD(*result, *num_types, enum_type_get_underlying_type(t));
        return;
    }
    else if (is_class_type(t))
    {
        type_t* class_type = get_actual_class_type(t);
        if (is_named_class_type(t))
        {
            scope_entry_t* symbol = named_type_get_symbol(t);
            class_type_complete_if_needed(symbol, decl_context, 
                    locus);
        }

        scope_entry_list_t* conversion_list = class_type_get_all_conversions(class_type, decl_context);

        scope_entry_list_iterator_t *it = NULL;
        for (it = entry_list_iterator_begin(conversion_list);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it))
        {
            scope_entry_t *conversion = entry_list_iterator_current(it);

            if (!is_template_specialized_type(conversion->type_information))
            {
                type_t* destination_type = function_type_get_return_type(conversion->type_information);

                // The implicit parameter of this operator function is a reference
                // to the class type, this will filter not eligible conversion functions
                // (e.g. given a 'const T' we cannot call a non-const method)
                type_t* implicit_parameter = symbol_entity_specs_get_class_type(conversion);
                if (is_const_qualified_type(conversion->type_information))
                {
                    implicit_parameter = get_cv_qualified_type(implicit_parameter, CV_CONST);
                }
                implicit_parameter = get_lvalue_reference_type(implicit_parameter);

                standard_conversion_t first_sc;
                if (standard_conversion_between_types(&first_sc, get_lvalue_reference_type(t), 
                            implicit_parameter, locus))
                {
                    P_LIST_ADD_ONCE(*result, *num_types, destination_type);
                }
            }
        }
        entry_list_iterator_free(it);
        entry_list_free(conversion_list);
    }
}

static
void build_unary_builtin_operators(type_t* t1,
        builtin_operators_set_t *result,
        decl_context_t decl_context, AST operator, 
        char (*property)(type_t*, const locus_t*),
        type_t* (*result_type)(type_t**, const locus_t*),
        const locus_t* locus)
{
    type_t** accessibles_1 = NULL;
    int num_accessibles_1 = 0;

    if (!is_unresolved_overloaded_type(no_ref(t1)))
    {
        accessible_types_through_conversion(no_ref(t1), &accessibles_1, &num_accessibles_1, decl_context, locus);
        // Add ourselves because we might be things like 'int&'
        // or 'T*&'
        P_LIST_ADD(accessibles_1, num_accessibles_1, t1);
    }

    memset(result, 0, sizeof(*result));

    int i;
    for (i = 0; i < num_accessibles_1; i++)
    {
        type_t* accessible_from_t1 = accessibles_1[i];
        if (property(accessible_from_t1, locus))
        {
            int num_parameters = 1;

            type_t* adjusted_t1 = accessible_from_t1;

            type_t* function_result_type = result_type(&adjusted_t1, locus);

            ERROR_CONDITION(function_result_type == NULL 
                    || is_error_type(function_result_type), "This type cannot be NULL!", 0);

            parameter_info_t parameters[1] =
            {
                { 
                    .is_ellipsis = 0,
                    .type_info = adjusted_t1,
                    .nonadjusted_type_info = NULL,
                },
            };

            type_t* function_type = get_new_function_type(function_result_type,
                    parameters, num_parameters, REF_QUALIFIER_NONE);

            // If this type is already in the type set, do not add it
            char found = 0;

            {
                int k;
                for (k = 0; (k < (*result).num_builtins) && !found; k++)
                {
                    scope_entry_t* sym = &((*result).entry[k]);
                    type_t* builtin_function_type = sym->type_information;

                    found = (equivalent_types(function_type, builtin_function_type));
                }
            }

            if (!found)
            {
                // Fill the minimum needed for this 'faked' function symbol
                (*result).entry[(*result).num_builtins].kind = SK_FUNCTION;
                (*result).entry[(*result).num_builtins].symbol_name = get_operator_function_name(operator);
                symbol_entity_specs_set_is_builtin(&(*result).entry[(*result).num_builtins], 1);
                (*result).entry[(*result).num_builtins].type_information = function_type;
                (*result).entry[(*result).num_builtins].decl_context = decl_context;

                // Add to the results and properly chain things
                (*result).entry_list = entry_list_add((*result).entry_list, &((*result).entry[(*result).num_builtins]));
                (*result).num_builtins++;
            }
        }
    }

    xfree(accessibles_1);
}

static
void build_binary_builtin_operators(type_t* t1, 
        type_t* t2, 
        builtin_operators_set_t *result,
        decl_context_t decl_context, AST operator, 
        char (*property)(type_t*, type_t*, const locus_t*),
        type_t* (*result_type)(type_t**, type_t**, const locus_t*),
        const locus_t* locus)
{
    type_t** accessibles_1 = NULL;
    int num_accessibles_1 = 0;

    type_t** accessibles_2 = NULL;
    int num_accessibles_2 = 0;

    if (!is_unresolved_overloaded_type(no_ref(t1)))
    {
        accessible_types_through_conversion(no_ref(t1), &accessibles_1, &num_accessibles_1, decl_context, locus);
        P_LIST_ADD(accessibles_1, num_accessibles_1, t1);
    }

    if (!is_unresolved_overloaded_type(no_ref(t2)))
    {
        accessible_types_through_conversion(no_ref(t2), &accessibles_2, &num_accessibles_2, decl_context, locus);
        P_LIST_ADD(accessibles_2, num_accessibles_2, t2);
    }

    memset(result, 0, sizeof(*result));

    int i;
    for (i = 0; i < num_accessibles_1; i++)
    {
        type_t* accessible_from_t1 = accessibles_1[i];
        int j;
        for (j = 0; j < num_accessibles_2; j++)
        {
            type_t* accessible_from_t2 = accessibles_2[j];

            if (property(accessible_from_t1, accessible_from_t2, locus))
            {
                int num_parameters = 2;

                type_t* adjusted_t1 = accessible_from_t1;
                type_t* adjusted_t2 = accessible_from_t2;

                type_t* function_result_type = result_type(&adjusted_t1, &adjusted_t2, locus);

                ERROR_CONDITION(function_result_type == NULL 
                        || is_error_type(function_result_type), "This type cannot be NULL!", 0);
                
                parameter_info_t parameters[2] =
                {
                    { 
                        .is_ellipsis = 0,
                        .type_info = adjusted_t1,
                        .nonadjusted_type_info = NULL
                    },
                    {
                        .is_ellipsis = 0,
                        .type_info = adjusted_t2,
                        .nonadjusted_type_info = NULL
                    }
                };

                type_t* function_type = get_new_function_type(function_result_type,
                        parameters, num_parameters, REF_QUALIFIER_NONE);

                // If this type is already in the type set, do not add it
                char found = 0;

                {
                    int k;
                    for (k = 0; (k < (*result).num_builtins) && !found; k++)
                    {
                        scope_entry_t* sym = &((*result).entry[k]);
                        type_t* builtin_function_type = sym->type_information;

                        found = (equivalent_types(function_type, builtin_function_type));
                    }
                }

                if (!found)
                {
                    // Fill the minimum needed for this 'faked' function symbol
                    (*result).entry[(*result).num_builtins].kind = SK_FUNCTION;
                    (*result).entry[(*result).num_builtins].symbol_name = get_operator_function_name(operator);
                    symbol_entity_specs_set_is_builtin(&(*result).entry[(*result).num_builtins], 1);
                    (*result).entry[(*result).num_builtins].type_information = function_type;
                    (*result).entry[(*result).num_builtins].decl_context = decl_context;

                    DEBUG_CODE()
                    {
                        fprintf(stderr, "EXPRTYPE: Generated builtin '%s' for '%s'\n",
                                print_declarator((*result).entry[(*result).num_builtins].type_information),
                                (*result).entry[(*result).num_builtins].symbol_name);
                    }

                    // Add to the results and properly chain things
                    (*result).entry_list = entry_list_add((*result).entry_list, &((*result).entry[(*result).num_builtins]) );
                    (*result).num_builtins++;
                }
            }
        }
    }

    xfree(accessibles_2);
    xfree(accessibles_1);
}

// All this is just for conditional expressions (ternary 'operator ?')
static
void build_ternary_builtin_operators(type_t* t1, 
        type_t* t2, 
        type_t* t3, 
        builtin_operators_set_t *result,
        // Note that since no ternary operator actually exists we use a faked name
        decl_context_t decl_context,
        const char* operator_name, 
        char (*property)(type_t*, type_t*, type_t*, const locus_t*),
        type_t* (*result_type)(type_t**, type_t**, type_t**, const locus_t*),
        const locus_t* locus)
{
    type_t** accessibles_1 = NULL;
    int num_accessibles_1 = 0;

    type_t** accessibles_2 = NULL;
    int num_accessibles_2 = 0;

    type_t** accessibles_3 = NULL;
    int num_accessibles_3 = 0;

    if (!is_unresolved_overloaded_type(no_ref(t1)))
    {
        accessible_types_through_conversion(no_ref(t1), &accessibles_1, &num_accessibles_1, decl_context, locus);
        P_LIST_ADD(accessibles_1, num_accessibles_1, t1);
    }

    if (!is_unresolved_overloaded_type(no_ref(t2)))
    {
        accessible_types_through_conversion(no_ref(t2), &accessibles_2, &num_accessibles_2, decl_context, locus);
        P_LIST_ADD(accessibles_2, num_accessibles_2, t2);
    }

    if (!is_unresolved_overloaded_type(no_ref(t3)))
    {
        accessible_types_through_conversion(no_ref(t3), &accessibles_3, &num_accessibles_3, decl_context, locus);
        P_LIST_ADD(accessibles_3, num_accessibles_3, t3);
    }

    memset(result, 0, sizeof(*result));

    int i;
    for (i = 0; i < num_accessibles_1; i++)
    {
        type_t* accessible_from_t1 = accessibles_1[i];
        int j;
        for (j = 0; j < num_accessibles_2; j++)
        {
            type_t* accessible_from_t2 = accessibles_2[j];

            int k;
            for (k = 0; k < num_accessibles_3; k++)
            {
                type_t* accessible_from_t3 = accessibles_3[k];

                if (property(accessible_from_t1, 
                            accessible_from_t2, 
                            accessible_from_t3,
                            locus))
                {
                    int num_parameters = 3;

                    type_t* adjusted_t1 = accessible_from_t1;
                    type_t* adjusted_t2 = accessible_from_t2;
                    type_t* adjusted_t3 = accessible_from_t3;

                    type_t* function_result_type = result_type(&adjusted_t1, &adjusted_t2, &adjusted_t3,
                            locus);

                    ERROR_CONDITION(function_result_type == NULL 
                            || is_error_type(function_result_type), "This type cannot be NULL!", 0);

                    parameter_info_t parameters[3] =
                    {
                        { 
                            .is_ellipsis = 0,
                            .type_info = adjusted_t1,
                            .nonadjusted_type_info = NULL,
                        },
                        {
                            .is_ellipsis = 0,
                            .type_info = adjusted_t2,
                            .nonadjusted_type_info = NULL,
                        },
                        {
                            .is_ellipsis = 0,
                            .type_info = adjusted_t3,
                            .nonadjusted_type_info = NULL,
                        }
                    };

                    type_t* function_type = get_new_function_type(function_result_type,
                            parameters, num_parameters,
                            REF_QUALIFIER_NONE);

                    // If this type is already in the type set, do not add it
                    char found = 0;

                    {
                        int m;
                        for (m = 0; (m < (*result).num_builtins) && !found; m++)
                        {
                            scope_entry_t* sym = &((*result).entry[m]);
                            type_t* builtin_function_type = sym->type_information;

                            found = (equivalent_types(function_type, builtin_function_type));
                        }
                    }

                    if (!found)
                    {
                        // Fill the minimum needed for this 'faked' function symbol
                        (*result).entry[(*result).num_builtins].kind = SK_FUNCTION;
                        (*result).entry[(*result).num_builtins].symbol_name = operator_name;
                        symbol_entity_specs_set_is_builtin(&(*result).entry[(*result).num_builtins], 1);
                        (*result).entry[(*result).num_builtins].type_information = function_type;
                        (*result).entry[(*result).num_builtins].decl_context = decl_context;

                        DEBUG_CODE()
                        {
                            fprintf(stderr, "EXPRTYPE: Generated builtin '%s' for '%s'\n",
                                    print_declarator((*result).entry[(*result).num_builtins].type_information),
                                    (*result).entry[(*result).num_builtins].symbol_name);
                        }

                        // Add to the results and properly chain things
                        (*result).entry_list = entry_list_add((*result).entry_list, &((*result).entry[(*result).num_builtins]));
                        (*result).num_builtins++;
                    }
                }
            }
        }
    }

    xfree(accessibles_3);
    xfree(accessibles_2);
    xfree(accessibles_1);
}

static void check_sizeof_type(type_t* t, 
        nodecl_t nodecl_expr,
        decl_context_t decl_context, 
        const locus_t* locus, nodecl_t* nodecl_output)
{
    *nodecl_output = nodecl_make_sizeof(
            nodecl_make_type(t, locus),
            nodecl_expr,
            get_size_t_type(),
            locus);
    
    if (!is_dependent_type(t)
            && !type_is_runtime_sized(t))
    {
        CXX_LANGUAGE()
        {
            if (is_named_class_type(t))
            {
                scope_entry_t* symbol = named_type_get_symbol(t);
                class_type_complete_if_needed(symbol, decl_context, 
                       locus);
            }
        }

        if (is_incomplete_type(t))
        {
            error_printf("%s: error: sizeof of incomplete type '%s'\n", 
                    locus_to_str(locus),
                    print_type_str(t, decl_context));
            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_expr);
            return;
        }

        if (!CURRENT_CONFIGURATION->disable_sizeof)
        {
            _size_t type_size = 0;
            type_size = type_get_size(t);


            DEBUG_SIZEOF_CODE()
            {
                fprintf(stderr, "EXPRTYPE: %s: sizeof yields a value of %zu\n",
                        locus_to_str(locus), type_size);
            }

            nodecl_set_constant(*nodecl_output,
                    const_value_get_integer(type_size, type_get_size(get_size_t_type()), 0));
        }
    }
    else if (is_dependent_type(t))
    {
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
    }
}

static void check_nodecl_sizeof_expr(nodecl_t nodecl_expr, decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    const locus_t* locus = nodecl_get_locus(nodecl_expr);

    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_expr))
    {
        *nodecl_output = nodecl_make_cxx_sizeof(nodecl_expr, get_size_t_type(), locus);
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }
    type_t* t = nodecl_get_type(nodecl_expr);

    C_LANGUAGE()
    {
        t = no_ref(t);
    }

    check_sizeof_type(t, nodecl_expr, decl_context, locus, nodecl_output);
}

static void check_sizeof_expr(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST sizeof_expression = ASTSon0(expr);

    nodecl_t nodecl_expr = nodecl_null();
    check_expression_non_executable(sizeof_expression, decl_context, &nodecl_expr);

    check_nodecl_sizeof_expr(nodecl_expr, decl_context, nodecl_output);
}

static void check_sizeof_typeid(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    const locus_t* locus = ast_get_locus(expr);

    AST type_id = ASTSon0(expr);
    type_t* declarator_type = compute_type_for_type_id_tree(type_id, decl_context,
            /* out_simple_type */ NULL, /* out_gather_info */ NULL);
    if (is_error_type(declarator_type))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    check_sizeof_type(declarator_type, /* nodecl_expr */ nodecl_null(), decl_context, locus, nodecl_output);
}

static void check_symbol_sizeof_pack(scope_entry_t* entry,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    int length = -1;

    if (entry == NULL)
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }
    else if (entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK
            || entry->kind == SK_TEMPLATE_TYPE_PARAMETER_PACK
            || entry->kind == SK_TEMPLATE_TEMPLATE_PARAMETER_PACK
            || (entry->kind == SK_VARIABLE_PACK && is_pack_type(entry->type_information)))
    {
        *nodecl_output = nodecl_make_cxx_sizeof_pack(
                nodecl_make_symbol(entry, locus),
                get_size_t_type(),
                locus);
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }
    else if (entry->kind == SK_VARIABLE_PACK
            && is_sequence_of_types(entry->type_information))
    {
        length = sequence_of_types_get_num_types(entry->type_information);
    }
    else if (entry->kind == SK_TYPEDEF_PACK
            || entry->kind == SK_TEMPLATE_PACK)
    {
        length = sequence_of_types_get_num_types(entry->type_information);
    }
    else
    {
        error_printf("%s: error: name '%s' is not a template or parameter pack\n",
                locus_to_str(locus),
                entry->symbol_name);
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    ERROR_CONDITION((length < 0), "Invalid length computed", 0);

    *nodecl_output = const_value_to_nodecl(
            const_value_get_integer(length, type_get_size(get_size_t_type()), 0));
}

static void check_sizeof_pack(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST name = ASTSon0(expr);

    nodecl_t nodecl_name = nodecl_null();
    compute_nodecl_name_from_id_expression(name, decl_context, &nodecl_name);

    if (nodecl_is_err_expr(nodecl_name))
    {
        *nodecl_output = nodecl_name;
        return;
    }

    scope_entry_list_t* result_list = query_nodecl_name_flags(
            decl_context,
            nodecl_name,
            NULL,
            DF_DEPENDENT_TYPENAME |
            DF_IGNORE_FRIEND_DECL |
            DF_DO_NOT_CREATE_UNQUALIFIED_DEPENDENT_ENTITY);

    if (result_list == NULL)
    {
        error_printf("%s: error: symbol '%s' not found in the current scope\n",
                nodecl_locus_to_str(nodecl_name),
                codegen_to_str(nodecl_name, decl_context));
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_name));
        return;
    }

    scope_entry_t* entry = entry_list_head(result_list);

    check_symbol_sizeof_pack(entry, ast_get_locus(expr), nodecl_output);
}

static void check_vla_expression(AST expression,
        decl_context_t decl_context UNUSED_PARAMETER,
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    *nodecl_output = nodecl_make_vla_wildcard(get_signed_int_type(), ast_get_locus(expression));
}

static void compute_nodecl_gcc_offset_designation(AST gcc_offset_designator,
        decl_context_t decl_context,
        nodecl_t* nodecl_output);

static void check_gcc_offset_designation(nodecl_t nodecl_designator,
        decl_context_t decl_context,
        type_t* accessed_type,
        nodecl_t* nodecl_output,
        const locus_t* locus)
{
    if (is_dependent_type(accessed_type))
    {
        *nodecl_output = nodecl_make_offsetof(
                nodecl_make_type(accessed_type, locus),
                nodecl_designator, 
                get_signed_int_type(), 
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }

    CXX_LANGUAGE()
    {
        if (is_named_class_type(accessed_type))
        {
            scope_entry_t* named_type = named_type_get_symbol(accessed_type);
            class_type_complete_if_needed(named_type, decl_context, locus);
        }
    }

    if (!is_complete_type(accessed_type))
    {
        error_printf("%s: error: invalid use of incomplete type '%s'\n",
                locus_to_str(locus),
                print_type_str(accessed_type, decl_context));
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    //the designated_type is not useful in this case
    type_t* designated_type  = NULL;
    nodecl_t error_designation = nodecl_null(); 

    designator_path_t designated_path;

    // FIXME - Remove this function and make a check here
    check_nodecl_designation_type(nodecl_designator, decl_context, 
            accessed_type, &designated_type, &error_designation, &designated_path);

    if(!nodecl_is_null(error_designation) && nodecl_is_err_expr(error_designation)) 
    {
        *nodecl_output = nodecl_make_err_expr(
                nodecl_get_locus(error_designation));
        return;
    }

    ERROR_CONDITION(designated_path.num_items == 0, "Invalid designation", 0);
    ERROR_CONDITION(designated_path.items[designated_path.num_items - 1].kind != NODECL_FIELD_DESIGNATOR,
            "Invalid designator kind", 0);

    scope_entry_t* designated_field = nodecl_get_symbol(designated_path.items[designated_path.num_items - 1].value);

    type_get_size(accessed_type);
    size_t offset_field = symbol_entity_specs_get_field_offset(designated_field);

    *nodecl_output = nodecl_make_offsetof(nodecl_make_type(accessed_type, locus),
            nodecl_designator, get_signed_int_type(),locus);

    const_value_t* cval = const_value_get_integer(offset_field,sizeof(size_t),0);
    nodecl_set_constant(*nodecl_output, cval);
}

static void check_gcc_builtin_offsetof(AST expression,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    const locus_t* locus = ast_get_locus(expression);

    AST type_id = ASTSon0(expression);
    AST member_designator = ASTSon1(expression);
    
    type_t* accessed_type = compute_type_for_type_id_tree(type_id, decl_context,
            /* out_simple_type */ NULL, /* out_gather_info */ NULL);

    if (is_error_type(accessed_type))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    nodecl_t nodecl_designator = nodecl_null();
    compute_nodecl_gcc_offset_designation(member_designator, decl_context, &nodecl_designator);
    
    // Check the designator and synthesize an offset value
    check_gcc_offset_designation(nodecl_designator, decl_context, accessed_type, nodecl_output, 
            locus);
}

static void compute_nodecl_gcc_offset_designation(AST gcc_offset_designator, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST name = ASTSon0(gcc_offset_designator);
    AST designator_list = ASTSon1(gcc_offset_designator);

    nodecl_t nodecl_designator = nodecl_make_list_1(
            nodecl_make_c99_field_designator(
                nodecl_make_cxx_dep_name_simple(ASTText(name), 
                    ast_get_locus(name)),
                ast_get_locus(name)));

    nodecl_t nodecl_designator_list = nodecl_null();
    if (designator_list != NULL)
    {
        compute_nodecl_designator_list(designator_list, decl_context, &nodecl_designator_list);
        *nodecl_output = nodecl_concat_lists(nodecl_designator, nodecl_designator_list);
    }
    else
    {
        *nodecl_output = nodecl_designator;
    }
}

static void check_gcc_builtin_choose_expr(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST selector_expr = ASTSon0(expression);
    AST first_expr = ASTSon1(expression);
    AST second_expr = ASTSon2(expression);

    const locus_t* locus = ast_get_locus(expression);

    // Since the exact type of this expression depends on the value yield by selector_expr
    // we will check the selector_expr and then evaluate it. 
    //
    // Note that this is only valid for C so we do not need to check
    // whether the expression is dependent

    nodecl_t nodecl_selector = nodecl_null();
    check_expression_impl_(selector_expr, decl_context, &nodecl_selector);
    if (nodecl_is_err_expr(nodecl_selector))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    if (!nodecl_is_constant(nodecl_selector))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    if (const_value_is_nonzero(nodecl_get_constant(nodecl_selector)))
    {
        check_expression_impl_(first_expr, decl_context, nodecl_output);
    }
    else
    {
        check_expression_impl_(second_expr, decl_context, nodecl_output);
    }
}

static void check_gcc_builtin_types_compatible_p(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // This builtin always returns an integer type
    AST first_type_tree = ASTSon0(expression);
    AST second_type_tree = ASTSon1(expression);

    const locus_t* locus = ast_get_locus(expression);

    type_t* first_type = compute_type_for_type_id_tree(first_type_tree, decl_context,
           /* out_simple_type */ NULL, /* out_gather_info */ NULL);
    type_t* second_type = compute_type_for_type_id_tree(second_type_tree, decl_context,
           /* out_simple_type */ NULL, /* out_gather_info */ NULL);

    if (is_error_type(first_type)
            || is_error_type(second_type))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    if (!is_dependent_type(first_type)
            && !is_dependent_type(second_type))
    {
        const_value_t* value = 
                equivalent_types(first_type, second_type) ?  
                const_value_get_one(/*bytes*/ 1, /*signed*/ 0) 
                : const_value_get_zero(/*bytes*/ 1,  /*signed*/ 0);
        *nodecl_output = const_value_to_nodecl(value);
    }
    else
    {
        error_printf("%s: error: __builtin_types_compatible_p is not implemented for dependent expressions\n",
                ast_location(expression));
    }
}

static void check_gcc_label_addr(AST expression, 
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    if (decl_context.current_scope->kind != BLOCK_SCOPE)
    {
        error_printf("%s: error: not inside any function, so getting the address of a label is not possible\n",
                ast_location(expression));
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        return;
    }

    AST label = ASTSon0(expression);
    scope_entry_t* sym_label = add_label_if_not_found(ASTText(label), decl_context, ast_get_locus(label));

    // Codegen will take care of this symbol and print &&label instead of &label
    *nodecl_output = nodecl_make_reference(
            nodecl_make_symbol(sym_label, ast_get_locus(label)),
            get_pointer_type(get_void_type()),
            ast_get_locus(expression));
}

static void check_nodecl_gcc_real_or_imag_part(nodecl_t nodecl_expr,
        decl_context_t decl_context UNUSED_PARAMETER,
        char is_real,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    type_t* result_type = NULL;
    if (nodecl_expr_is_type_dependent(nodecl_expr))
    {
        // OK
        result_type = get_unknown_dependent_type();
    }
    else
    {
        type_t* t = no_ref(nodecl_get_type(nodecl_expr));
        if (!is_complex_type(t))
        {
            error_printf("%s: error: operand of '%s' is not a complex type\n",
                    nodecl_locus_to_str(nodecl_expr),
                    is_real ? "__real__" : "__imag__");

            *nodecl_output = nodecl_make_err_expr(locus);
            nodecl_free(nodecl_expr);
            return;
        }

        result_type = complex_type_get_base_type(t);

        if (is_lvalue_reference_type(nodecl_get_type(nodecl_expr)))
        {
            result_type = lvalue_ref(result_type);
        }
    }

    nodecl_t (*fun)(nodecl_t, type_t*, const locus_t*) = nodecl_make_imag_part;
    if (is_real)
    {
        fun = nodecl_make_real_part;
    }

    *nodecl_output = fun(nodecl_expr, result_type, locus);
}

static void check_gcc_real_or_imag_part(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    char is_real = (ASTType(expression) == AST_GCC_REAL_PART);

    nodecl_t nodecl_expr = nodecl_null();
    check_expression_impl_(ASTSon0(expression), decl_context, &nodecl_expr);

    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
    }

    check_nodecl_gcc_real_or_imag_part(nodecl_expr, 
            decl_context, is_real, 
            ast_get_locus(expression), 
            nodecl_output);
}


static void check_gcc_alignof_type(type_t* t,
        nodecl_t nodecl_expr,
        decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    scope_entry_t* symbol = NULL;
    if (!nodecl_is_null(nodecl_expr))
        symbol = nodecl_get_symbol(nodecl_expr);
    nodecl_t symbol_alignment_attr = nodecl_null();
    if (symbol != NULL)
        symbol_alignment_attr = symbol_get_aligned_attribute(symbol);

    const_value_t* alignment_value = NULL;

    if (!nodecl_is_null(symbol_alignment_attr))
    {
        if (nodecl_expr_is_value_dependent(symbol_alignment_attr))
        {
            *nodecl_output = nodecl_make_cxx_alignof(nodecl_expr, get_size_t_type(), locus);
            return;
        }
        else if (!nodecl_is_constant(symbol_alignment_attr))
        {
            ERROR_CONDITION("'aligned' attribute of entity '%s' is not a constant when computing __alignof__\n",
                    nodecl_locus_to_str(nodecl_expr),
                    get_qualified_symbol_name(symbol, symbol->decl_context));
        }
        else
        {
            alignment_value = nodecl_get_constant(symbol_alignment_attr);
        }
    }

    if (is_dependent_type(t))
    {
        *nodecl_output = nodecl_make_alignof(nodecl_make_type(t, locus), get_size_t_type(), locus);
        nodecl_expr_set_is_value_dependent(*nodecl_output, 1);
        return;
    }

    CXX_LANGUAGE()
    {
        if (is_named_class_type(t))
        {
            scope_entry_t* named_type = named_type_get_symbol(t);
            class_type_complete_if_needed(named_type, decl_context, locus);
        }
    }

    if (is_incomplete_type(t))
    {
        error_printf("%s: error: alignof of incomplete type '%s'\n", 
                locus_to_str(locus),
                print_type_str(t, decl_context));

        *nodecl_output = nodecl_make_err_expr(locus);
        nodecl_free(nodecl_expr);
        return;
    }

    if (IS_C_LANGUAGE)
    {
        t = no_ref(t);
    }

    if (nodecl_is_null(nodecl_expr))
    {
        *nodecl_output = nodecl_make_alignof(nodecl_make_type(t, locus), get_size_t_type(), locus);
    }
    else
    {
        *nodecl_output = nodecl_make_alignof(nodecl_expr, get_size_t_type(), locus);
    }

    // Compute the alignment using the type if we have not come with any alignment yet
    if (alignment_value == NULL
            && !CURRENT_CONFIGURATION->disable_sizeof)
    {
        _size_t type_alignment = type_get_alignment(t);

        DEBUG_SIZEOF_CODE()
        {
            fprintf(stderr, "EXPRTYPE: %s: alignof yields a value of %zu\n",
                    locus_to_str(locus), type_alignment);
        }

        alignment_value  = const_value_get_integer(type_alignment, type_get_size(get_size_t_type()), 0);
    }

    nodecl_set_constant(*nodecl_output, alignment_value);
}

static void check_nodecl_gcc_alignof_expr(
        nodecl_t nodecl_expr,
        decl_context_t decl_context,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (nodecl_expr_is_type_dependent(nodecl_expr))
    {
        *nodecl_output = nodecl_make_cxx_alignof(nodecl_expr, get_size_t_type(), locus);
        return;
    }

    type_t* t = nodecl_get_type(nodecl_expr);

    check_gcc_alignof_type(t, nodecl_expr, decl_context, locus, nodecl_output);
}

static void check_gcc_alignof_expr(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST alignof_expr = ASTSon0(expression);
    nodecl_t nodecl_expr = nodecl_null();
    check_expression_non_executable(
            alignof_expr,
            decl_context, &nodecl_expr);

    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(expression));
        return;
    }

    check_nodecl_gcc_alignof_expr(nodecl_expr, decl_context, ast_get_locus(expression), nodecl_output);
}

static void check_gcc_alignof_typeid(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST type_id = ASTSon0(expression);

    type_t* t = compute_type_for_type_id_tree(type_id, decl_context,
           /* out_simple_type */ NULL, /* out_gather_info */ NULL);

    if (is_error_type(t))
    {
        *nodecl_output = nodecl_make_err_expr(ast_get_locus(type_id));
        return;
    }

    check_gcc_alignof_type(t, nodecl_null(), decl_context, ast_get_locus(type_id), nodecl_output);
}

static void check_gcc_postfix_expression(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    const locus_t* locus = ast_get_locus(expression);

    AST type_id = ASTSon0(expression);

    gather_decl_spec_t gather_info;
    memset(&gather_info, 0, sizeof(gather_info));

    type_t* t = compute_type_for_type_id_tree(type_id, decl_context,
           NULL, &gather_info);

    if (is_error_type(t))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    AST braced_init_list = ASTSon1(expression);

    nodecl_t nodecl_braced_init = nodecl_null();
    compute_nodecl_braced_initializer(braced_init_list, decl_context, &nodecl_braced_init);

    if (nodecl_is_err_expr(nodecl_braced_init))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    CXX_LANGUAGE()
    {
        if (gather_info.defined_type != NULL)
        {
            push_extra_declaration_symbol(gather_info.defined_type);
        }
    }

    if (is_dependent_type(t))
    {
        *nodecl_output = nodecl_make_cxx_postfix_initializer(nodecl_braced_init, t, locus);
        return;
    }

    check_nodecl_braced_initializer(nodecl_braced_init, decl_context, t,
            /* is_explicit_type_cast */ 0, IK_DIRECT_INITIALIZATION,
            nodecl_output);

    // This is an lvalue
    if (nodecl_is_err_expr(*nodecl_output))
        return;

    nodecl_set_type(*nodecl_output, get_lvalue_reference_type(no_ref(nodecl_get_type(*nodecl_output))));

    if (nodecl_get_kind(*nodecl_output) == NODECL_STRUCTURED_VALUE)
    {
        nodecl_set_type(*nodecl_output, get_lvalue_reference_type(no_ref(nodecl_get_type(*nodecl_output))));
        nodecl_set_child(*nodecl_output, 1,
                nodecl_make_structured_value_compound_literal(locus));
    }
}

static void check_nodecl_gcc_parenthesized_expression(nodecl_t nodecl_context,
        decl_context_t decl_context UNUSED_PARAMETER,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    nodecl_t nodecl_compound = nodecl_list_head(nodecl_get_child(nodecl_context, 0));

    ERROR_CONDITION(nodecl_get_kind(nodecl_compound) != NODECL_COMPOUND_STATEMENT, "Invalid node", 0);

    nodecl_t nodecl_list_of_stmts = nodecl_get_child(nodecl_compound, 0);

    type_t* computed_type = NULL;

    int num_items = 0;
    nodecl_t* nodecl_list = nodecl_unpack_list(nodecl_list_of_stmts, &num_items);

    if (num_items > 0)
    {
        nodecl_t nodecl_last_stmt = nodecl_list[num_items - 1];

        // This check is only true if the whole statement is not later casted to void
        //
        // if (nodecl_get_kind(nodecl_last_stmt) != NODECL_EXPRESSION_STATEMENT)
        // {
        //     error_printf("%s:%d: error: last statement must be an expression statement\n",
        //                 locus);
        //     *nodecl_output = nodecl_make_err_expr(locus);
        //     return;
        // }

        nodecl_t nodecl_expr = nodecl_get_child(nodecl_last_stmt, 0);

        computed_type = nodecl_get_type(nodecl_expr);
    }
    else if (num_items == 0)
    {
        computed_type = get_void_type();
    }

    if (computed_type == NULL)
    {
        computed_type = get_void_type();
    }
    else if (is_error_type(computed_type))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    xfree(nodecl_list);

    *nodecl_output = nodecl_make_compound_expression(
            nodecl_context,
            computed_type,
            locus);
}

static void check_gcc_parenthesized_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST compound_statement = ASTSon0(expression);
    nodecl_t nodecl_stmt_seq = nodecl_null();

    // This returns a list, we only want the first item of that list
    build_scope_statement(compound_statement, decl_context, &nodecl_stmt_seq);

    ERROR_CONDITION(nodecl_list_length(nodecl_stmt_seq) != 1, "This should be 1", 0);

    nodecl_t nodecl_context = nodecl_list_head(nodecl_stmt_seq);
    ERROR_CONDITION(nodecl_get_kind(nodecl_context) != NODECL_CONTEXT, "Invalid tree", 0);

    check_nodecl_gcc_parenthesized_expression(nodecl_context, 
            decl_context, 
            ast_get_locus(expression),
            nodecl_output);
}

static void check_gcc_builtin_va_arg(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    const locus_t* locus = ast_get_locus(expression);

    // This is an historic builtin we do not handle using the generic builtin
    // mechanism since it has very special syntax involving types
    nodecl_t nodecl_expr = nodecl_null();
    check_expression_impl_(ASTSon0(expression), decl_context, &nodecl_expr);

    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    AST type_id = ASTSon1(expression);

    type_t* t = compute_type_for_type_id_tree(type_id, decl_context,
            /* out_simple_type */ NULL, /* out_gather_info */ NULL);

    if (is_error_type(t))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    *nodecl_output = nodecl_make_gcc_builtin_va_arg(
            nodecl_expr,
            nodecl_make_type(t, locus),
            t, locus);
}

static void check_nodecl_array_section_expression(nodecl_t nodecl_postfix,
        nodecl_t nodecl_lower,
        nodecl_t nodecl_upper,
        nodecl_t nodecl_stride,
        decl_context_t decl_context, 
        char is_array_section_size,
        const locus_t* locus,
        nodecl_t* nodecl_output)
{

    if (nodecl_is_err_expr(nodecl_postfix)
            || (!nodecl_is_null(nodecl_lower) && nodecl_is_err_expr(nodecl_lower))
            || (!nodecl_is_null(nodecl_upper) && nodecl_is_err_expr(nodecl_upper))
            || (!nodecl_is_null(nodecl_stride) && nodecl_is_err_expr(nodecl_stride)))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    if (nodecl_expr_is_type_dependent(nodecl_postfix)
            || (!nodecl_is_null(nodecl_lower) && nodecl_expr_is_type_dependent(nodecl_lower))
            || (!nodecl_is_null(nodecl_upper) && nodecl_expr_is_type_dependent(nodecl_upper)))
    {
        if (is_array_section_size)
        {
            *nodecl_output = nodecl_make_cxx_array_section_size(nodecl_postfix, 
                    nodecl_lower, nodecl_upper, nodecl_stride,
                    get_unknown_dependent_type(),
                    locus);
        }
        else
        {
            *nodecl_output = nodecl_make_cxx_array_section_range(nodecl_postfix, 
                    nodecl_lower, nodecl_upper, nodecl_stride,
                    get_unknown_dependent_type(),
                    locus);
        }

        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }


    type_t* indexed_type = no_ref(nodecl_get_type(nodecl_postfix));

    if (nodecl_is_null(nodecl_lower) && (is_array_type(indexed_type) || is_pointer_type(indexed_type))) 
        nodecl_lower = nodecl_shallow_copy(array_type_get_array_lower_bound(indexed_type));

    if (nodecl_is_null(nodecl_upper) && (is_array_type(indexed_type))) 
        nodecl_upper = nodecl_shallow_copy(array_type_get_array_upper_bound(indexed_type));

#define MAX_NESTING_OF_ARRAY_REGIONS (16)

    type_t* advanced_types[MAX_NESTING_OF_ARRAY_REGIONS];
    int i = 0;
    while (is_array_type(indexed_type) && array_type_has_region(indexed_type))
    {
        ERROR_CONDITION(i == MAX_NESTING_OF_ARRAY_REGIONS, "Too many array regions nested %d\n", i);
        advanced_types[i] = indexed_type;
        indexed_type = array_type_get_element_type(indexed_type);
        i++;
    }

    if (is_array_section_size)
    {
        // L;U:S -> L: (U + L) - 1:S
        if (nodecl_is_constant(nodecl_lower)
                && nodecl_is_constant(nodecl_upper))
        {
            const_value_t* cval_lower = nodecl_get_constant(nodecl_lower);
            const_value_t* cval_upper = nodecl_get_constant(nodecl_upper);

            nodecl_upper =
                const_value_to_nodecl(
                        const_value_sub( // (U +L) - 1
                            const_value_add( // U + L
                                cval_upper, // U
                                cval_lower),// L
                            const_value_get_one(/* num_bytes */ 4, /* signed */ 1)));
        }
        else
        {
            nodecl_upper =
                nodecl_make_minus(
                        nodecl_make_add(
                            nodecl_shallow_copy(nodecl_upper), nodecl_shallow_copy(nodecl_lower),
                            get_signed_int_type(),
                            nodecl_get_locus(nodecl_upper)),
                        nodecl_make_integer_literal(
                            get_signed_int_type(),
                            const_value_get_one(type_get_size(get_signed_int_type()), 1),
                            nodecl_get_locus(nodecl_upper)),
                        get_signed_int_type(),
                        nodecl_get_locus(nodecl_upper));
        }
    }

    nodecl_t nodecl_range = nodecl_make_range(nodecl_lower, nodecl_upper, nodecl_stride, 
            get_signed_int_type(), locus);

    type_t* result_type = NULL;
    if (is_array_type(indexed_type))
    {
        nodecl_t array_lower_bound = array_type_get_array_lower_bound(indexed_type);
        nodecl_t array_upper_bound = array_type_get_array_upper_bound(indexed_type);
        decl_context_t array_decl_context = array_type_get_array_size_expr_context(indexed_type);
    
        result_type = get_array_type_bounds_with_regions(
                array_type_get_element_type(indexed_type),
                array_lower_bound,
                array_upper_bound,
                array_decl_context,
                nodecl_range,
                decl_context);
    }
    else if (is_pointer_type(indexed_type))
    {
        if (i > 0)
        {
            error_printf("%s: error: pointer types only allow one-level array sections\n",
                    nodecl_locus_to_str(nodecl_postfix));
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }
        if (is_void_pointer_type(indexed_type))
        {
            warn_printf("%s: warning: postfix expression '%s' of array section is 'void*', assuming 'char*' instead\n",
                    nodecl_locus_to_str(nodecl_postfix),
                    codegen_to_str(nodecl_postfix, nodecl_retrieve_context(nodecl_postfix)));
            indexed_type = get_pointer_type(get_char_type());
        }
        result_type = get_array_type_bounds_with_regions(
                pointer_type_get_pointee_type(indexed_type),
                nodecl_lower,
                nodecl_upper,
                decl_context,
                nodecl_range,
                decl_context);
    }
    else
    {
        error_printf("%s: warning: array section is invalid since '%s' has type '%s'\n",
                nodecl_locus_to_str(nodecl_postfix),
                codegen_to_str(nodecl_postfix, nodecl_retrieve_context(nodecl_postfix)),
                print_type_str(indexed_type, decl_context));
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    while (i > 0)
    {
        type_t* current_array_type = advanced_types[i - 1];
       
        nodecl_t array_lower_bound = nodecl_shallow_copy(array_type_get_array_lower_bound(current_array_type));
        nodecl_t array_upper_bound = nodecl_shallow_copy(array_type_get_array_upper_bound(current_array_type));
        nodecl_t array_region_lower_bound = nodecl_shallow_copy(array_type_get_region_lower_bound(current_array_type));
        nodecl_t array_region_upper_bound = nodecl_shallow_copy(array_type_get_region_upper_bound(current_array_type));
        nodecl_t array_region_stride = nodecl_shallow_copy(array_type_get_region_stride(current_array_type));

        nodecl_t array_region_range = nodecl_make_range(
                array_region_lower_bound, 
                array_region_upper_bound, 
                array_region_stride,
                current_array_type, 
                locus);
        
        decl_context_t array_decl_context = array_type_get_array_size_expr_context(current_array_type);

        result_type = get_array_type_bounds_with_regions(
                result_type,
                array_lower_bound,
                array_upper_bound,
                array_decl_context,
                array_region_range,
                decl_context);
        i--;
    }

    if (nodecl_get_kind(nodecl_postfix) == NODECL_ARRAY_SUBSCRIPT
            && is_array_type(no_ref(nodecl_get_type(nodecl_postfix))))
    {
        nodecl_t nodecl_indexed = nodecl_get_child(nodecl_postfix, 0);
        nodecl_t nodecl_subscript_list = nodecl_get_child(nodecl_postfix, 1);

        nodecl_subscript_list = nodecl_append_to_list(nodecl_subscript_list, 
                nodecl_range);

        *nodecl_output = nodecl_make_array_subscript(
                nodecl_indexed,
                nodecl_subscript_list,
                result_type, 
                locus);
    }
    else
    {
        *nodecl_output = nodecl_make_array_subscript(nodecl_postfix, 
                nodecl_make_list_1(nodecl_range),
                result_type,
                locus);
    }
}

static void check_array_section_expression(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    const locus_t* locus = ast_get_locus(expression);

    AST postfix_expression = ASTSon0(expression);
    AST lower_bound = ASTSon1(expression);
    AST upper_bound = ASTSon2(expression);
    AST stride = ASTSon3(expression);
    
    nodecl_t nodecl_postfix = nodecl_null();
    check_expression_impl_(postfix_expression, decl_context, &nodecl_postfix);
    
    nodecl_t nodecl_lower = nodecl_null();
    if (lower_bound != NULL)
        check_expression_impl_(lower_bound, decl_context, &nodecl_lower);

    nodecl_t nodecl_upper = nodecl_null();
    if (upper_bound != NULL)
        check_expression_impl_(upper_bound, decl_context, &nodecl_upper);

    nodecl_t nodecl_stride = nodecl_null();
    if (stride != NULL)
    {
        check_expression_impl_(stride, decl_context, &nodecl_stride);
    }
    else
    {
        nodecl_stride = const_value_to_nodecl(const_value_get_one(/* bytes */ 4, /* signed */ 1));
    }
    
    char is_array_section_size = (ASTType(expression) == AST_ARRAY_SECTION_SIZE);

    check_nodecl_array_section_expression(nodecl_postfix, 
            nodecl_lower, nodecl_upper, nodecl_stride,
            decl_context, is_array_section_size, locus, nodecl_output);
}

static void check_nodecl_shaping_expression(nodecl_t nodecl_shaped_expr,
        nodecl_t nodecl_shape_list,
        decl_context_t decl_context UNUSED_PARAMETER, 
        const locus_t* locus,
        nodecl_t* nodecl_output)
{
    if (nodecl_expr_is_type_dependent(nodecl_shaped_expr)
            || nodecl_expr_is_type_dependent(nodecl_shape_list))
    {
        *nodecl_output = nodecl_make_shaping(nodecl_shaped_expr, 
                nodecl_shape_list, 
                get_unknown_dependent_type(),
                locus);
        nodecl_expr_set_is_type_dependent(*nodecl_output, 1);
        return;
    }

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_shape_list, &num_items);

    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t current_expr = list[i];

        type_t *current_expr_type = nodecl_get_type(current_expr);

        standard_conversion_t scs;
        if (!standard_conversion_between_types(&scs,
                    no_ref(current_expr_type),
                    get_signed_int_type(),
                    locus))
        {
            error_printf("%s: error: shaping expression '%s' cannot be converted to 'int'\n",
                    nodecl_locus_to_str(current_expr),
                    codegen_to_str(current_expr, nodecl_retrieve_context(current_expr)));
            xfree(list);
            *nodecl_output = nodecl_make_err_expr(locus);
            return;
        }
    }

    // Now check the shape makes sense
    type_t* shaped_expr_type = nodecl_get_type(nodecl_shaped_expr);

    // Array to pointer conversion
    if (is_array_type(no_ref(shaped_expr_type)))
    {
        shaped_expr_type = get_pointer_type(array_type_get_element_type(no_ref(shaped_expr_type)));
    }

    if (!is_pointer_type(no_ref(shaped_expr_type)))
    {
        error_printf("%s: error: shaped expression '%s' does not have pointer type\n",
                nodecl_locus_to_str(nodecl_shaped_expr),
                codegen_to_str(nodecl_shaped_expr, nodecl_retrieve_context(nodecl_shaped_expr)));
        xfree(list);
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    if (is_void_pointer_type(no_ref(shaped_expr_type)))
    {
        warn_printf("%s: warning: shaped expression '%s' has type 'void*', assuming 'char*' instead\n",
                nodecl_locus_to_str(nodecl_shaped_expr),
                codegen_to_str(nodecl_shaped_expr, nodecl_retrieve_context(nodecl_shaped_expr)));
        shaped_expr_type = get_pointer_type(get_char_type());
    }

    // Synthesize a new type based on what we got
    type_t* result_type = pointer_type_get_pointee_type(no_ref(shaped_expr_type));

    // Traverse the list backwards
    for (i = num_items - 1; i >= 0; i--)
    {
        nodecl_t current_expr = list[i];
        result_type = get_array_type(result_type, current_expr, decl_context);
    }
    xfree(list);

    *nodecl_output = nodecl_make_shaping(nodecl_shaped_expr, 
            nodecl_shape_list, 
            result_type,
            locus);
}

static const char* prettyprint_shape(AST a)
{
    if (a == NULL)
        return "[]";
    else if (ASTType(a) == AST_NODE_LIST)
    {
        const char* result = "";
        AST it;
        for_each_element(a, it)
        {
            AST shape = ASTSon1(it);
            uniquestr_sprintf(&result, "%s[%s]", result, prettyprint_in_buffer(shape));
        }
        return result;
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}


static void check_shaping_expression(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    const locus_t* locus = ast_get_locus(expression);

    AST shaped_expr = ASTSon1(expression);
    AST shape_list = ASTSon0(expression);

    nodecl_t nodecl_shaped_expr = nodecl_null();
    check_expression_impl_(shaped_expr, decl_context, &nodecl_shaped_expr);

    // [X]a[i] is parsed as [X](a[i])
    if (!nodecl_is_err_expr(nodecl_shaped_expr)
            && shaped_expr!= NULL
            &&(ASTType(shaped_expr) == AST_ARRAY_SUBSCRIPT
                || ASTType(shaped_expr) == AST_ARRAY_SECTION
                || ASTType(shaped_expr) == AST_ARRAY_SECTION_SIZE))
    {
        warn_printf("%s: warning: syntax '%s%s' is equivalent to '%s(%s)'\n",
                ast_location(expression),
                prettyprint_shape(shape_list),
                prettyprint_in_buffer(shaped_expr),
                prettyprint_shape(shape_list),
                prettyprint_in_buffer(shaped_expr));

        AST subscripted_item = shaped_expr;
        while (ASTType(subscripted_item) == AST_ARRAY_SUBSCRIPT
                || ASTType(subscripted_item) == AST_ARRAY_SECTION
                || ASTType(subscripted_item) == AST_ARRAY_SECTION_SIZE)
        {
            subscripted_item = ASTSon0(subscripted_item);
        }

        AST subscript_item = shaped_expr;
        const char* array_subscript = "";
        for (;;)
        {
            if (ASTType(subscript_item) == AST_ARRAY_SUBSCRIPT)
            {
                uniquestr_sprintf(&array_subscript, "[%s]%s",
                        prettyprint_in_buffer(ASTSon1(subscript_item)),
                        array_subscript);
            }
            else if (ASTType(subscript_item) == AST_ARRAY_SECTION)
            {
                uniquestr_sprintf(&array_subscript, "[%s:%s]%s",
                        prettyprint_in_buffer(ASTSon1(subscript_item)),
                        prettyprint_in_buffer(ASTSon2(subscript_item)),
                        array_subscript);
            }
            else if (ASTType(subscript_item) == AST_ARRAY_SECTION_SIZE)
            {
                uniquestr_sprintf(&array_subscript, "[%s;%s]%s",
                        prettyprint_in_buffer(ASTSon1(subscript_item)),
                        prettyprint_in_buffer(ASTSon2(subscript_item)),
                        array_subscript);
            }
            else
            {
                break;
            }
            subscript_item = ASTSon0(subscript_item);
        }

        info_printf("%s: info: did you actually mean '(%s%s)%s'?\n",
                ast_location(expression),
                prettyprint_shape(shape_list),
                prettyprint_in_buffer(subscripted_item),
                array_subscript);
    }


    if (nodecl_is_err_expr(nodecl_shaped_expr))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    nodecl_t nodecl_shape_list = nodecl_null();
    if (!check_list_of_expressions(shape_list, decl_context, &nodecl_shape_list))
    {
        *nodecl_output = nodecl_make_err_expr(locus);
        return;
    }

    check_nodecl_shaping_expression(nodecl_shaped_expr, 
            nodecl_shape_list,
            decl_context,
            locus,
            nodecl_output);
}

char check_list_of_initializer_clauses(
        AST initializer_clause_list,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    *nodecl_output = nodecl_null();
    if (initializer_clause_list == NULL)
    {
        // An empty list is OK
        return 1;
    }

    if (ASTType(initializer_clause_list) == AST_AMBIGUITY)
    {
        return solve_ambiguous_list_of_initializer_clauses(initializer_clause_list, decl_context, nodecl_output);
    }
    else
    {
        // Check the beginning of the list
        nodecl_t nodecl_prev_list = nodecl_null();
        check_list_of_initializer_clauses(ASTSon0(initializer_clause_list), decl_context, &nodecl_prev_list);

        if (!nodecl_is_null(nodecl_prev_list)
                && nodecl_is_err_expr(nodecl_prev_list))
        {
            *nodecl_output = nodecl_prev_list;
            return 0;
        }

        nodecl_t nodecl_current = nodecl_null();
        AST initializer = ASTSon1(initializer_clause_list);
        compute_nodecl_initializer_clause(initializer, decl_context,
                /* preserve_top_level_parentheses */ 0, // not important here
                &nodecl_current);

        if (nodecl_is_err_expr(nodecl_current))
        {
            *nodecl_output = nodecl_current;
            return 0;
        }

        *nodecl_output = nodecl_append_to_list(nodecl_prev_list, nodecl_current);

        return 1;
    }

    internal_error("Code unreachable", 0);
}

static void define_defaulted_special_member(
        scope_entry_t* special_member,
        decl_context_t decl_context,
        const locus_t* locus);

char check_default_initialization_of_type(
        type_t* t,
        decl_context_t decl_context,
        const locus_t* locus,
        scope_entry_t** constructor)
{
    if (constructor != NULL)
    {
        *constructor = NULL;
    }

    if (is_lvalue_reference_type(t)
            && !is_rebindable_reference_type(t))
    {
        // References cannot be default initialized
        error_printf("%s: error: reference type '%s' cannot be default initialized\n",
                locus_to_str(locus), print_type_str(t, decl_context));
        return 0;
    }

    if (is_array_type(t))
        t = array_type_get_element_type(t);

    if (is_class_type(t))
    {
        int num_arguments = 0;
        type_t** arguments = NULL;

        scope_entry_list_t* candidates = NULL;
        scope_entry_t* chosen_constructor = NULL;
        char ok = solve_initialization_of_class_type(t,
                arguments, num_arguments,
                IK_DIRECT_INITIALIZATION,
                decl_context,
                locus,
                &chosen_constructor,
                &candidates);

        if (!ok)
        {
            if (entry_list_size(candidates) != 0)
            {
                error_printf("%s: error: no default constructor for class type '%s'\n",
                        locus_to_str(locus),
                        print_type_str(t, decl_context));
            }
            entry_list_free(candidates);
            return 0;
        }
        else
        {
            entry_list_free(candidates);
            if (function_has_been_deleted(decl_context, chosen_constructor, locus))
            {
                return 0;
            }

            if (symbol_entity_specs_get_is_defaulted(chosen_constructor))
            {
                define_defaulted_special_member(chosen_constructor,
                        decl_context,
                        locus);
            }

            if (constructor != NULL)
            {
                *constructor = chosen_constructor;
            }
        }
    }
    return 1;
}

char check_default_initialization(scope_entry_t* entry,
        decl_context_t decl_context, 
        const locus_t* locus,
        scope_entry_t** constructor)
{
    ERROR_CONDITION(entry == NULL, "Invalid entry", 0);

    type_t* t = entry->type_information;
    if (entry->kind == SK_CLASS
            || entry->kind == SK_ENUM)
        t = get_user_defined_type(entry);

    char c = check_default_initialization_of_type(t, decl_context, locus, constructor);

    if (!c)
    {
        error_printf("%s: error: cannot default initialize entity '%s'\n",
                locus_to_str(locus),
                get_qualified_symbol_name(entry, decl_context));
    }

    return c;
}

char check_copy_constructor(scope_entry_t* entry,
        decl_context_t decl_context,
        char has_const,
        const locus_t* locus,
        scope_entry_t** constructor)
{
    if (constructor != NULL)
    {
        *constructor = NULL;
    }

    type_t* t = entry->type_information;
    if (entry->kind == SK_CLASS)
    {
        t = get_user_defined_type(entry);
    }

    if (is_lvalue_reference_type(t))
    {
        return 1;
    }

    if (is_array_type(t))
    {
        t = array_type_get_element_type(t);
    }

    if (is_class_type(t))
    {
        int num_arguments = 1;

        type_t* parameter_type = t;
        if (has_const)
        {
            parameter_type = get_const_qualified_type(parameter_type);
        }
        parameter_type = get_lvalue_reference_type(parameter_type);

        type_t* arguments[1] = { parameter_type };

        scope_entry_list_t* candidates = NULL;
        scope_entry_t* chosen_constructor = NULL;
        char ok = solve_initialization_of_class_type(t,
                arguments, num_arguments,
                IK_DIRECT_INITIALIZATION,
                decl_context,
                locus,
                &chosen_constructor,
                &candidates);

        if (ok)
        {
            if (entry_list_size(candidates) != 0)
            {
                error_printf("%s: error: no copy constructor for type '%s'\n",
                        locus_to_str(locus),
                        print_type_str(t, decl_context));
            }
            entry_list_free(candidates);
            return 0;
        }
        else
        {
            entry_list_free(candidates);
            if (function_has_been_deleted(decl_context, chosen_constructor, locus))
            {
                return 0;
            }

            if (constructor != NULL)
            {
                *constructor = chosen_constructor;
            }
        }
    }
    return 1;
}

char check_copy_assignment_operator(scope_entry_t* entry,
        decl_context_t decl_context,
        char has_const,
        const locus_t* locus,
        scope_entry_t** constructor)
{
    if (constructor != NULL)
    {
        *constructor = NULL;
    }

    type_t* t = entry->type_information;
    if (entry->kind == SK_CLASS)
    {
        t = get_user_defined_type(entry);
    }

    if (is_lvalue_reference_type(t))
    {
        return 1;
    }

    if (is_array_type(t))
    {
        t = array_type_get_element_type(t);
    }

    if (is_class_type(t))
    {
        static AST operation_tree = NULL;
        if (operation_tree == NULL)
        {
            operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                    ASTLeaf(AST_ASSIGNMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
        }

        type_t* argument_type = t;
        if (has_const)
        {
            argument_type = get_const_qualified_type(argument_type);
        }
        argument_type = get_lvalue_reference_type(argument_type);

        int num_arguments = 2;
        type_t* arguments[2] = { argument_type, argument_type };

        scope_entry_list_t* operator_overload_set = NULL;
        scope_entry_list_t* operator_entry_list = class_type_get_copy_assignment_operators(t);
        operator_overload_set = unfold_and_mix_candidate_functions(operator_entry_list,
                NULL, arguments + 1, num_arguments - 1,
                decl_context,
                locus,
                /* explicit template arguments */ NULL);
        entry_list_free(operator_entry_list);

        candidate_t* candidate_set = NULL;
        scope_entry_list_iterator_t *it = NULL;
        for (it = entry_list_iterator_begin(operator_overload_set);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it))
        {
            candidate_set = candidate_set_add(candidate_set,
                    entry_list_iterator_current(it),
                    num_arguments,
                    arguments);
        }
        entry_list_iterator_free(it);

        scope_entry_t *overloaded_call = solve_overload(candidate_set,
                decl_context,
                locus);

        if (overloaded_call == NULL)
        {
            const char*  c = NULL;;
            uniquestr_sprintf(&c, "copy assignment operator of class %s", entry->symbol_name);
            error_message_overload_failed(candidate_set, 
                    c,
                    decl_context,
                    num_arguments, arguments,
                    /* implicit_argument */ NULL,
                    locus);
            entry_list_free(operator_overload_set);
            candidate_set_free(&candidate_set);
            return 0;
        }
        else
        {
            candidate_set_free(&candidate_set);
            entry_list_free(operator_overload_set);
            if (function_has_been_deleted(decl_context, overloaded_call, locus))
            {
                return 0;
            }

            if (constructor != NULL)
            {
                *constructor = overloaded_call;
            }
        }
    }
    return 1;
}

char check_move_assignment_operator(scope_entry_t* entry,
        decl_context_t decl_context,
        char has_const,
        const locus_t* locus,
        scope_entry_t** constructor)
{
    if (constructor != NULL)
    {
        *constructor = NULL;
    }

    type_t* t = entry->type_information;
    if (entry->kind == SK_CLASS)
    {
        t = get_user_defined_type(entry);
    }

    if (is_lvalue_reference_type(t))
    {
        return 1;
    }

    if (is_array_type(t))
    {
        t = array_type_get_element_type(t);
    }

    if (is_class_type(t))
    {
        static AST operation_tree = NULL;
        if (operation_tree == NULL)
        {
            operation_tree = ASTMake1(AST_OPERATOR_FUNCTION_ID,
                    ASTLeaf(AST_ASSIGNMENT_OPERATOR, make_locus("", 0, 0), NULL), make_locus("", 0, 0), NULL);
        }

        type_t* argument_type = t;
        if (has_const)
        {
            argument_type = get_const_qualified_type(argument_type);
        }
        argument_type = get_lvalue_reference_type(argument_type);

        int num_arguments = 2;
        type_t* arguments[2] = { argument_type, argument_type };

        scope_entry_list_t* operator_overload_set = NULL;
        scope_entry_list_t* operator_entry_list = class_type_get_copy_assignment_operators(t);
        operator_overload_set = unfold_and_mix_candidate_functions(operator_entry_list,
                NULL, arguments + 1, num_arguments - 1,
                decl_context,
                locus,
                /* explicit template arguments */ NULL);
        entry_list_free(operator_entry_list);

        candidate_t* candidate_set = NULL;
        scope_entry_list_iterator_t *it = NULL;
        for (it = entry_list_iterator_begin(operator_overload_set);
                !entry_list_iterator_end(it);
                entry_list_iterator_next(it))
        {
            candidate_set = candidate_set_add(candidate_set,
                    entry_list_iterator_current(it),
                    num_arguments,
                    arguments);
        }
        entry_list_iterator_free(it);

        scope_entry_t *overloaded_call = solve_overload(candidate_set,
                decl_context, locus);

        if (overloaded_call == NULL)
        {
            const char*  c = NULL;;
            uniquestr_sprintf(&c, "move assignment operator of class %s", entry->symbol_name);
            error_message_overload_failed(candidate_set, 
                    c,
                    decl_context,
                    num_arguments, arguments,
                    /* implicit_argument */ NULL,
                    locus);
            entry_list_free(operator_overload_set);
            candidate_set_free(&candidate_set);
            return 0;
        }
        else
        {
            candidate_set_free(&candidate_set);
            entry_list_free(operator_overload_set);
            if (function_has_been_deleted(decl_context, overloaded_call, locus))
            {
                return 0;
            }

            if (constructor != NULL)
            {
                *constructor = overloaded_call;
            }
        }
    }
    return 1;
}

char check_default_initialization_and_destruction_declarator(scope_entry_t* entry, decl_context_t decl_context,
        const locus_t* locus)
{
    scope_entry_t* constructor = NULL;
    char ok = check_default_initialization(entry, decl_context, locus, &constructor);

    if (!ok)
        return 0;

    if (is_incomplete_type(entry->type_information))
        return 0;

    if (is_class_type_or_array_thereof(entry->type_information))
    {
        entry->value = nodecl_make_value_initialization(constructor, locus);

        type_t* class_type = entry->type_information;
        if (is_array_type(class_type))
            class_type = array_type_get_element_type(class_type);

        scope_entry_t* destructor = class_type_get_destructor(class_type);
        ERROR_CONDITION(destructor == NULL, "Invalid destructor", 0);

        ensure_function_is_emitted(constructor, decl_context, locus);
        ensure_function_is_emitted(destructor, decl_context, locus);
    }

    return 1;
}

static void diagnostic_single_candidate(scope_entry_t* entry, 
        const char** message,
        const locus_t* locus UNUSED_PARAMETER)
{
    entry = entry_advance_aliases(entry);
    const char *c = NULL;
    uniquestr_sprintf(&c, "%s: note:    %s%s%s\n",
            locus_to_str(entry->locus),
            (symbol_entity_specs_get_is_member(entry) && symbol_entity_specs_get_is_static(entry)) ? "static " : "",
            !is_computed_function_type(entry->type_information)
            ?  print_decl_type_str(entry->type_information, entry->decl_context,
                get_qualified_symbol_name(entry, entry->decl_context)) 
            : " <<generic function>>",
            symbol_entity_specs_get_is_builtin(entry) ? " [built-in]" : ""
            );

    *message = strappend(*message, c);
}

void diagnostic_candidates(scope_entry_list_t* candidates,
        const char** message,
        const locus_t* locus)
{
    const char* c = NULL;
    uniquestr_sprintf(&c, "%s: info: candidates are:\n", locus_to_str(locus));
    *message = strappend(*message, c);

    scope_entry_list_t* unrepeated_candidates = NULL;

    scope_entry_list_iterator_t* it;
    for (it = entry_list_iterator_begin(candidates);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* candidate_fun = entry_list_iterator_current(it);
        candidate_fun = entry_advance_aliases(candidate_fun);

        if (candidate_fun->kind == SK_TEMPLATE)
        {
            // These are not very useful as candidates
            candidate_fun = named_type_get_symbol(
                    template_type_get_primary_type(
                        candidate_fun->type_information));
        }

        unrepeated_candidates = entry_list_add_once(unrepeated_candidates, candidate_fun);
    }
    entry_list_iterator_free(it);

    for (it = entry_list_iterator_begin(unrepeated_candidates);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* candidate_fun = entry_list_iterator_current(it);
        diagnostic_single_candidate(candidate_fun, message, locus);
    }
    entry_list_iterator_free(it);

    entry_list_free(unrepeated_candidates);
}

static void error_message_overload_failed(candidate_t* candidates, 
        const char* name,
        decl_context_t decl_context,
        int num_arguments,
        type_t** arguments,
        type_t* implicit_argument,
        const locus_t* locus)
{
    ERROR_CONDITION(arguments == NULL && num_arguments > 0, 
            "Mismatch between arguments and number of arguments", 0);

    const char* argument_types = "(";

    int i, j = 0;
    for (i = 0; i < num_arguments; i++)
    {
        if (arguments[i] == NULL)
            continue;

        if (j > 0)
            argument_types = strappend(argument_types, ", ");

        argument_types = strappend(argument_types, print_type_str(arguments[i], decl_context));
        j++;
    }

    argument_types = strappend(argument_types, ")");

    const char* message = NULL;
    uniquestr_sprintf(&message, "%s: error: failed overload call to '%s%s'\n",
            locus_to_str(locus), name, argument_types);

    char there_are_nonstatic_members = 0;

    if (candidates != NULL)
    {
        candidate_t* it = candidates;

        scope_entry_list_t* candidate_list = NULL;

        while (it != NULL)
        {
            scope_entry_t* entry = it->entry;

            there_are_nonstatic_members =
                there_are_nonstatic_members || (symbol_entity_specs_get_is_member(entry) && !symbol_entity_specs_get_is_static(entry));

            candidate_list = entry_list_add_once(candidate_list, entry);
            it = it->next;
        }

        diagnostic_candidates(candidate_list, &message, locus);

        entry_list_free(candidate_list);
    }
    else
    {
        const char* c;
        uniquestr_sprintf(&c, "%s: info: no candidate functions\n", locus_to_str(locus));

        message = strappend(message, c);
    }

    if (there_are_nonstatic_members
            && implicit_argument != NULL)
    {
        const char *c;
        uniquestr_sprintf(&c,
                "%s: info: the type of the implicit argument for nonstatic member candidates is '%s'\n",
                locus_to_str(locus),
                print_type_str(implicit_argument, decl_context));

        message = strappend(message, c);
    }

    error_printf("%s", message);
}

static nodecl_t cxx_nodecl_make_conversion_internal(nodecl_t expr,
        type_t* dest_type,
        const locus_t* locus,
        char verify_conversion)
{
    ERROR_CONDITION(nodecl_expr_is_type_dependent(expr),
            "Do not call this function on type dependent expressions", 0);
    ERROR_CONDITION(is_dependent_type(dest_type),
            "Do not call this function to convert to dependent types", 0);

    char is_value_dep = nodecl_expr_is_value_dependent(expr);
    const_value_t* val = cxx_nodecl_make_value_conversion(dest_type,
            nodecl_get_type(expr),
            nodecl_get_constant(expr),
            /* is_explicit_cast */ 0,
            locus);

    // Propagate zero types
    if (val != NULL)
    {
        if (is_zero_type(nodecl_get_type(expr))
                && const_value_is_zero(val)
                && (is_integral_type(dest_type)
                    || is_bool_type(dest_type)))
        {
            dest_type = get_zero_type(dest_type);
        }
    }

    if (verify_conversion)
    {
        standard_conversion_t scs;
        char there_is_a_scs = standard_conversion_between_types(
                &scs, nodecl_get_type(expr),
                get_unqualified_type(dest_type), locus);
        // Try again with enum types
        if (!there_is_a_scs
                && (is_enum_type(nodecl_get_type(expr))
                    || is_enum_type(no_ref(dest_type))))
        {
            type_t* underlying_orig_type = get_unqualified_type(no_ref(nodecl_get_type(expr)));
            if (is_enum_type(underlying_orig_type))
                underlying_orig_type = enum_type_get_underlying_type(underlying_orig_type);

            type_t* underlying_dest_type = get_unqualified_type(no_ref(dest_type));
            if (is_enum_type(underlying_dest_type))
                underlying_dest_type = enum_type_get_underlying_type(underlying_dest_type);

            there_is_a_scs = standard_conversion_between_types(
                    &scs,
                    underlying_orig_type,
                    underlying_dest_type,
                    locus);
        }

        ERROR_CONDITION(!there_is_a_scs, "At this point (%s) there should be a SCS from '%s' to '%s'\n",
                locus_to_str(locus),
                print_declarator(nodecl_get_type(expr)),
                print_declarator(get_unqualified_type(dest_type)));
    }

    nodecl_t result = nodecl_make_conversion(expr, dest_type, locus);

    nodecl_set_constant(result, val);
    nodecl_expr_set_is_value_dependent(result, is_value_dep);

    return result;
}

nodecl_t cxx_nodecl_make_conversion(nodecl_t expr, type_t* dest_type, const locus_t* locus)
{
    return cxx_nodecl_make_conversion_internal(expr, dest_type, locus,
            /* verify conversion */ 1);
}

static nodecl_t constexpr_function_get_returned_expression(nodecl_t nodecl_function_code)
{
    ERROR_CONDITION(nodecl_is_null(nodecl_function_code)
            || nodecl_get_kind(nodecl_function_code) != NODECL_FUNCTION_CODE, "Invalid function code", 0);

    nodecl_t nodecl_context = nodecl_get_child(nodecl_function_code, 0);
    ERROR_CONDITION(nodecl_is_null(nodecl_context), "Invalid node", 0);
    ERROR_CONDITION(nodecl_get_kind(nodecl_context) != NODECL_CONTEXT, "Unexpected node", 0);

    nodecl_t nodecl_body = nodecl_get_child(nodecl_context, 0);
    ERROR_CONDITION(nodecl_is_null(nodecl_body) || !nodecl_is_list(nodecl_body),
            "Invalid node", 0);

    nodecl_t nodecl_compound_statement = nodecl_list_head(nodecl_body);

    nodecl_t nodecl_statements = nodecl_get_child(nodecl_compound_statement, 0);
    ERROR_CONDITION(nodecl_is_null(nodecl_statements)
            || !nodecl_is_list(nodecl_statements), "Invalid node", 0);

    nodecl_t nodecl_return_statement = nodecl_null();

    int i, N = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_statements, &N);
    for (i = 0; i < N; i++)
    {
        if (nodecl_get_kind(list[i]) == NODECL_RETURN_STATEMENT)
        {
            nodecl_return_statement = list[i];
            break;
        }
    }
    xfree(list);

    ERROR_CONDITION(nodecl_is_null(nodecl_return_statement), "Return statement not found", 0);

    nodecl_t nodecl_returned_expression = nodecl_get_child(nodecl_return_statement, 0);

    ERROR_CONDITION(nodecl_is_null(nodecl_returned_expression), "Invalid returned expression", 0);

    return nodecl_returned_expression;
}

static void argument_list_remove_default_arguments(nodecl_t* list_of_arguments, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        if (nodecl_get_kind(list_of_arguments[i]) == NODECL_DEFAULT_ARGUMENT)
        {
            list_of_arguments[i] = nodecl_get_child(list_of_arguments[i], 0);
        }
    }
}

typedef
struct map_of_parameters_with_their_arguments_tag
{
    scope_entry_t* parameter;
    const_value_t* value;
    const_value_t** value_list;
    int num_values;
} map_of_parameters_with_their_arguments_t;

static map_of_parameters_with_their_arguments_t*
constexpr_function_get_constants_of_arguments(
        nodecl_t converted_arg_list,
        scope_entry_t* entry,
        decl_context_t decl_context,
        int *num_map_items)
{
    int num_arguments = 0;
    nodecl_t* list_of_arguments = nodecl_unpack_list(converted_arg_list, &num_arguments);
    argument_list_remove_default_arguments(list_of_arguments, num_arguments);

    char has_ellipsis = 0;
    int num_parameters = function_type_get_num_parameters(entry->type_information);
    if (function_type_get_has_ellipsis(entry->type_information))
    {
        has_ellipsis = 1;
        num_parameters--;
    }

    map_of_parameters_with_their_arguments_t* result = xcalloc(
            num_arguments,
            sizeof(*result));
    *num_map_items = 0;

#define ERROR_MESSAGE_THIS \
        if (check_expr_flags.must_be_constant) \
        { \
            error_printf("%s: error: during call to constexpr member function '%s', implicit argument is not constant\n", \
                    nodecl_locus_to_str(list_of_arguments[current_argument]), \
                    print_decl_type_str(entry->type_information, entry->decl_context, \
                        get_qualified_symbol_name(entry, entry->decl_context))); \
        }

#define ERROR_MESSAGE_REGULAR_ARGUMENT \
        if (check_expr_flags.must_be_constant) \
        { \
            error_printf("%s: error: during call to constexpr %s '%s', argument '%s' in position %d is not constant\n", \
                    nodecl_locus_to_str(list_of_arguments[current_argument]), \
                    symbol_entity_specs_get_is_constructor(entry) ? "constructor" : "function", \
                    print_decl_type_str(entry->type_information, entry->decl_context, \
                        get_qualified_symbol_name(entry, entry->decl_context)), \
                    codegen_to_str(list_of_arguments[current_argument], decl_context), \
                    current_argument + 1); \
        }

#define CHECK_CONSTANT(value, error_message) \
    if (value == NULL) \
    { \
        DEBUG_CODE() \
        { \
            fprintf(stderr, "EXPRTYPE: When evaluating call of '%s', " \
                    "argument %d '%s' (at %s) is not constant, giving up evaluation\n", \
                    get_qualified_symbol_name(entry, entry->decl_context), \
                    current_argument, \
                    codegen_to_str(list_of_arguments[current_argument], CURRENT_COMPILED_FILE->global_decl_context), \
                    nodecl_locus_to_str(list_of_arguments[current_argument])); \
        } \
        error_message; \
        *num_map_items = -1; \
        xfree(list_of_arguments); \
        xfree(result); \
        return NULL; \
    }

    int current_parameter = 0;
    int current_argument = 0;
    while (current_argument < num_arguments)
    {

        scope_entry_t* parameter = NULL;
        if (current_argument == 0
                && symbol_entity_specs_get_is_member(entry)
                && !symbol_entity_specs_get_is_static(entry)
                && !symbol_entity_specs_get_is_constructor(entry))
        {
            // 'this'
            decl_context_t body_context =  nodecl_retrieve_context(
                    nodecl_get_child(symbol_entity_specs_get_function_code(entry), 0)
                    );
            scope_entry_list_t* this_list =
                query_name_str(body_context, UNIQUESTR_LITERAL("this"), NULL);

            ERROR_CONDITION(this_list == NULL, "There should be a 'this'", 0);

            scope_entry_t* this_in_body = entry_list_head(this_list);

            parameter = this_in_body;

            const_value_t* value = nodecl_get_constant(list_of_arguments[current_argument]);
            CHECK_CONSTANT(value, ERROR_MESSAGE_THIS);

            result[current_parameter].parameter = parameter;
            result[current_parameter].value = value;
            result[current_parameter].num_values = -1;

            (*num_map_items)++;

            // Note that we do not advance current_parameter because the
            // implicit parameter is not represented in the type of the
            // function
            current_argument++;
            continue;
        }

        if (has_ellipsis
                && current_parameter >= symbol_entity_specs_get_num_related_symbols(entry))
            break;

        ERROR_CONDITION(current_parameter >= symbol_entity_specs_get_num_related_symbols(entry),
                "Too many arguments", 0);
        parameter = symbol_entity_specs_get_related_symbols_num(entry, current_parameter);

        if (parameter->kind == SK_VARIABLE)
        {
            // Regular parameter/argument
            const_value_t* value = nodecl_get_constant(list_of_arguments[current_argument]);
            CHECK_CONSTANT(value, ERROR_MESSAGE_REGULAR_ARGUMENT);

            result[current_parameter].parameter = parameter;
            result[current_parameter].value = value;
            result[current_parameter].num_values = -1;
            (*num_map_items)++;

            // This parameter has already been processed
            current_parameter++;
            // This argument has already been processed
            current_argument++;
        }
        else if (parameter->kind == SK_VARIABLE_PACK)
        {
            internal_error("SK_VARIABLE_PACK '%s' should not appear as a parameter of a function\n",
                    parameter->symbol_name);
        }
        else
        {
            internal_error("Code unreachable", 0);
        }
    }
    xfree(list_of_arguments);

    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Arguments properly converted to their parameters in constexpr evaluation\n");
    }

    return result;

#undef CHECK_CONSTANT
#undef ERROR_MESSAGE_REGULAR_ARGUMENT
#undef ERROR_MESSAGE_THIS
}

static void free_map_of_parameters_and_values(map_of_parameters_with_their_arguments_t* map,
        int num_map_items)
{
    int i;
    for (i = 0; i < num_map_items; i++)
    {
        xfree(map[i].value_list);
    }
}

static const_value_t* lookup_single_value_in_map(
        map_of_parameters_with_their_arguments_t* map_of_parameters_and_values,
        int num_map_items,
        scope_entry_t* entry)
{
    int i;
    for (i = 0; i < num_map_items; i++)
    {
        if (entry == map_of_parameters_and_values[i].parameter)
        {
            return map_of_parameters_and_values[i].value;
        }
    }
    return NULL;
}

static void constexpr_replace_parameters_with_values_rec(nodecl_t n,
        int num_map_items,
        map_of_parameters_with_their_arguments_t* map_of_parameters_and_values)
{
    if (nodecl_is_null(n))
        return;

    int i;
    for (i = 0; i < MCXX_MAX_AST_CHILDREN; i++)
    {
        constexpr_replace_parameters_with_values_rec(nodecl_get_child(n, i),
                num_map_items, map_of_parameters_and_values);
    }

    if (nodecl_get_kind(n) == NODECL_SYMBOL)
    {
        scope_entry_t* entry = nodecl_get_symbol(n);

        if (entry->symbol_name != NULL
                && entry->kind == SK_VARIABLE
                && strcmp(entry->symbol_name, "this") == 0)
        {
            // We cannot directly replace 'this' only its dereference
        }
        else if (entry->kind == SK_VARIABLE)
        {
            const_value_t* value = lookup_single_value_in_map(map_of_parameters_and_values, num_map_items, entry);

            if (value != NULL)
            {
                type_t* t = nodecl_get_type(n);
                nodecl_t nodecl_value = const_value_to_nodecl(value);
                // Preserve the original type
                nodecl_set_type(nodecl_value, t);

                nodecl_replace(n, nodecl_value);
            }
        }
        else if (entry->kind == SK_VARIABLE_PACK)
        {
            internal_error("SK_VARIABLE_PACK '%s' should have gone away", entry->symbol_name);
        }
    }
    else if (nodecl_get_kind(n) == NODECL_DEREFERENCE
            && nodecl_get_kind(nodecl_get_child(n, 0)) == NODECL_SYMBOL)
    {
        scope_entry_t* entry = nodecl_get_symbol(nodecl_get_child(n, 0));
        if (entry->symbol_name != NULL
                && strcmp(entry->symbol_name, "this") == 0)
        {
            const_value_t* value = lookup_single_value_in_map(map_of_parameters_and_values, num_map_items, entry);

            if (value != NULL)
            {
                nodecl_t nodecl_value = const_value_to_nodecl(value);
                nodecl_replace(n, nodecl_value);
            }
        }
    }
}

static nodecl_t constexpr_replace_parameters_with_values(nodecl_t n,
    int num_map_items,
    map_of_parameters_with_their_arguments_t* map_of_parameters_and_values)
{
    nodecl_t nodecl_result = nodecl_shallow_copy(n);

    constexpr_replace_parameters_with_values_rec(
            nodecl_result,
            num_map_items, map_of_parameters_and_values);

    return nodecl_result;
}

static const_value_t* evaluate_constexpr_constructor(
        scope_entry_t* entry,
        nodecl_t converted_arg_list,
        decl_context_t decl_context,
        const locus_t* locus)
{
    if (function_may_be_instantiated(entry))
    {
        enum must_be_constant_t must_be_constant = check_expr_flags.must_be_constant;
        check_expr_flags.must_be_constant = MUST_NOT_BE_CONSTANT;
        instantiate_template_function(entry, locus);
        check_expr_flags.must_be_constant = must_be_constant;

        push_instantiated_entity(entry);

        nodecl_t nodecl_initializer_list = nodecl_null();
        if (!nodecl_is_null(symbol_entity_specs_get_function_code(entry)))
        {
            nodecl_initializer_list = nodecl_get_child(symbol_entity_specs_get_function_code(entry), 1);
        }

        symbol_entity_specs_set_is_constexpr(entry,
                check_constexpr_constructor(entry, entry->locus,
                    nodecl_initializer_list,
                    /* diagnose */ 0, /* emit_error */ 0));
        if (!symbol_entity_specs_get_is_constexpr(entry))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: After instantiation of '%s' it has become non constexpr\n",
                        get_qualified_symbol_name(entry, entry->decl_context));
            }
            if (check_expr_flags.must_be_constant)
            {
                error_printf("%s: error: constructor '%s' has become non-constexpr after instantiation\n",
                        locus_to_str(locus),
                        print_decl_type_str(entry->type_information, entry->decl_context,
                            get_qualified_symbol_name(entry, entry->decl_context)));
            }
            return NULL;
        }
    }

    if (!entry->defined)
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Evaluation of constexpr constructor fails because the function has not been defined\n");
        }
        if (check_expr_flags.must_be_constant)
        {
            error_printf("%s: error: call to undefined constexpr constructor '%s' in constant-expression\n",
                    locus_to_str(locus),
                    print_decl_type_str(entry->type_information, entry->decl_context,
                        get_qualified_symbol_name(entry, entry->decl_context)));
        }
        return NULL;
    }

    int num_map_items = -1;
    map_of_parameters_with_their_arguments_t* map_of_parameters_and_values =
        constexpr_function_get_constants_of_arguments(converted_arg_list, entry, decl_context, &num_map_items);

    if (num_map_items < 0)
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: When creating the map of parameters to symbols, "
                    "one of the arguments did not yield a constant value\n");
        }
        return NULL;
    }

    nodecl_t nodecl_function_code = symbol_entity_specs_get_function_code(entry);
    ERROR_CONDITION(nodecl_is_null(nodecl_function_code), "Invalid node", 0);

    nodecl_t nodecl_initializers = nodecl_null();
    if (!nodecl_is_null(nodecl_function_code))
        nodecl_initializers = nodecl_get_child(nodecl_function_code, 1);

    type_t* class_type = symbol_entity_specs_get_class_type(entry);
    scope_entry_t* class_sym = named_type_get_symbol(class_type);

    // Special case for delegating constructors
    if (nodecl_list_length(nodecl_initializers) == 1)
    {
        nodecl_t first = nodecl_list_head(nodecl_initializers);
        scope_entry_t* current_member = nodecl_get_symbol(first);
        nodecl_t nodecl_expr = nodecl_get_child(first, 0);

        if (current_member == class_sym)
        {
            // This is a delegating constructor
            nodecl_t nodecl_replaced_expr = constexpr_replace_parameters_with_values(
                    nodecl_expr,
                    num_map_items,
                    map_of_parameters_and_values);

            // Evaluate it recursively
            nodecl_t nodecl_evaluated_expr = instantiate_expression(
                    nodecl_replaced_expr,
                    nodecl_retrieve_context(nodecl_function_code),
                    symbol_entity_specs_get_instantiation_symbol_map(entry),
                    /* pack_index */ -1);

            free_map_of_parameters_and_values(map_of_parameters_and_values, num_map_items);
            return nodecl_get_constant(nodecl_evaluated_expr);
        }
    }

    int num_all_members = 0;
    scope_entry_t** all_members = NULL;
    {
        scope_entry_list_t* direct_base_classes = class_type_get_direct_base_classes(class_type);
        scope_entry_list_t* data_members_list = class_type_get_nonstatic_data_members(class_type);

        scope_entry_list_t* all_members_list = entry_list_concat(direct_base_classes, data_members_list);

        entry_list_free(data_members_list);
        entry_list_free(direct_base_classes);

        entry_list_to_symbol_array(all_members_list, &all_members, &num_all_members);
        entry_list_free(all_members_list);
    }

    const_value_t** values = xcalloc(num_all_members, sizeof(*values));

    int i, N = 0;
    nodecl_t *nodecl_list = nodecl_unpack_list(nodecl_initializers, &N);

    for (i = 0; i < N; i++)
    {
        scope_entry_t* current_member = nodecl_get_symbol(nodecl_list[i]);
        nodecl_t nodecl_expr = nodecl_get_child(nodecl_list[i], 0);

        int member_pos = -1;
        // Find the symbol in the all_members array
        int j;
        for (j = 0; j < num_all_members; j++)
        {
            if (all_members[j] == current_member)
            {
                member_pos = j;
                break;
            }
        }

        ERROR_CONDITION(member_pos < 0,
                "Symbol '%s' not found in the initializer set. Maybe it has not been initialized\n",
                current_member->symbol_name);

        nodecl_t nodecl_replaced_expr = constexpr_replace_parameters_with_values(
                nodecl_expr,
                num_map_items,
                map_of_parameters_and_values);

        nodecl_t nodecl_evaluated_expr = instantiate_expression(
                nodecl_replaced_expr,
                nodecl_retrieve_context(nodecl_function_code),
                symbol_entity_specs_get_instantiation_symbol_map(entry),
                /* pack_index */ -1);

        values[member_pos] = nodecl_get_constant(nodecl_evaluated_expr);
        if (values[member_pos] == NULL)
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Data-member '%s' has an initializer but it does not have a constant value\n",
                        get_qualified_symbol_name(current_member, current_member->decl_context));
            }
            if (check_expr_flags.must_be_constant)
            {
                error_printf("%s: error: during call to constexpr constructor '%s' in constant-expression, data-member '%s' "
                        "is not constant\n",
                        locus_to_str(locus),
                        print_decl_type_str(entry->type_information, entry->decl_context,
                            get_qualified_symbol_name(entry, entry->decl_context)),
                        get_qualified_symbol_name(current_member, current_member->decl_context));
            }

            xfree(all_members);
            xfree(values);
            free_map_of_parameters_and_values(map_of_parameters_and_values, num_map_items);
            return NULL;
        }
    }

    // Check that all members have constant values
    for (i = 0; i < num_all_members; i++)
    {
        if (values[i] == NULL)
        {
            // Since we do not materialize the constructors initializations
            // we will attempt here to use the initializer expression (if any)
            // of this member, or call the default constructor
            scope_entry_t* current_member = all_members[i];

            if (current_member->kind == SK_CLASS)
            {
                scope_entry_t* default_constructor =
                    class_type_get_default_constructor(
                            current_member->type_information);
                ERROR_CONDITION(default_constructor == NULL, "Invalid class", 0);

                if (symbol_entity_specs_get_is_constexpr(default_constructor))
                {
                    values[i] = evaluate_constexpr_constructor(
                            default_constructor,
                            /* no args */ nodecl_null(),
                            decl_context,
                            locus);
                }
            }
            else if (current_member->kind == SK_VARIABLE)
            {
                if (!nodecl_is_null(current_member->value))
                {
                    values[i] = nodecl_get_constant(current_member->value);
                }
                else if (is_class_type(current_member->type_information))
                {
                    scope_entry_t* default_constructor =
                        class_type_get_default_constructor(
                                current_member->type_information);
                    ERROR_CONDITION(default_constructor == NULL, "Invalid class", 0);

                    if (symbol_entity_specs_get_is_constexpr(default_constructor))
                    {
                        values[i] = evaluate_constexpr_constructor(
                                default_constructor,
                                /* no args */ nodecl_null(),
                                decl_context,
                                locus);
                    }
                }
            }
            else
            {
                internal_error("Code unreachable", 0);
            }

            // If still not constant, give up
            if (values[i] == NULL)
            {
                DEBUG_CODE()
                {
                    fprintf(stderr, "EXPRTYPE: Data-member '%s' cannot be default initialized as a constant value\n",
                            get_qualified_symbol_name(current_member, current_member->decl_context));
                }

                if (check_expr_flags.must_be_constant)
                {
                    error_printf("%s: error: during call to constexpr constructor '%s' in constant-expression, data-member '%s' "
                            "cannot be default initialized as a constant value\n",
                            locus_to_str(locus),
                            print_decl_type_str(entry->type_information, entry->decl_context,
                                get_qualified_symbol_name(entry, entry->decl_context)),
                            get_qualified_symbol_name(current_member, current_member->decl_context));
                }

                xfree(all_members);
                xfree(values);
                free_map_of_parameters_and_values(map_of_parameters_and_values, num_map_items);
                return NULL;
            }
        }
    }

    const_value_t* structured_value =
        const_value_make_struct(num_all_members, values, class_type);

    xfree(all_members);
    xfree(values);
    free_map_of_parameters_and_values(map_of_parameters_and_values, num_map_items);

    return structured_value;
}

static const_value_t* evaluate_constexpr_regular_function_call(
        scope_entry_t* entry,
        nodecl_t converted_arg_list,
        decl_context_t decl_context,
        const locus_t* locus)
{
    if (function_may_be_instantiated(entry))
    {
        enum must_be_constant_t must_be_constant = check_expr_flags.must_be_constant;
        check_expr_flags.must_be_constant = MUST_NOT_BE_CONSTANT;
        instantiate_template_function(entry, locus);
        check_expr_flags.must_be_constant = must_be_constant;

        push_instantiated_entity(entry);

        symbol_entity_specs_set_is_constexpr(entry,
                check_constexpr_function(entry, entry->locus,
                    /* diagnose */ 0, /* emit_error */ 0));

        if (!symbol_entity_specs_get_is_constexpr(entry))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: After instantiation of '%s' it has become non constexpr\n",
                        get_qualified_symbol_name(entry, entry->decl_context));
            }
            if (check_expr_flags.must_be_constant)
            {
                error_printf("%s: error: function '%s' has become non-constexpr after instantiation in constant-expression\n",
                        locus_to_str(locus),
                        print_decl_type_str(entry->type_information, entry->decl_context,
                            get_qualified_symbol_name(entry, entry->decl_context)));
            }
            return NULL;
        }
    }

    if (!entry->defined)
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Evaluation of constexpr fails because the function has not been defined\n");
        }
        if (check_expr_flags.must_be_constant)
        {
            error_printf("%s: error: call to undefined constexpr function '%s' in constant-expression\n",
                    locus_to_str(locus),
                    print_decl_type_str(entry->type_information, entry->decl_context,
                        get_qualified_symbol_name(entry, entry->decl_context)));
        }
        return NULL;
    }

    int num_map_items = -1;
    map_of_parameters_with_their_arguments_t* map_of_parameters_and_values =
        constexpr_function_get_constants_of_arguments(converted_arg_list, entry, decl_context, &num_map_items);

    if (num_map_items < 0)
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: When creating the map of parameters to symbols, "
                    "one of the arguments did not yield a constant value\n");
        }
        return NULL;
    }

    nodecl_t nodecl_function_code = symbol_entity_specs_get_function_code(entry);
    ERROR_CONDITION(nodecl_is_null(nodecl_function_code), "Function lacks function code", 0);

    nodecl_t nodecl_returned_expression =
        constexpr_function_get_returned_expression(nodecl_function_code);

    // Replace parameter ocurrences with values
    nodecl_t nodecl_replace_parameters = constexpr_replace_parameters_with_values(
            nodecl_returned_expression,
            num_map_items,
            map_of_parameters_and_values);

    instantiation_symbol_map_t* instantiation_symbol_map = NULL;
    if (symbol_entity_specs_get_is_member(entry))
    {
        instantiation_symbol_map = symbol_entity_specs_get_instantiation_symbol_map(entry);
    }

    nodecl_t nodecl_evaluated_expr = instantiate_expression(nodecl_replace_parameters,
            nodecl_retrieve_context(nodecl_function_code),
            instantiation_symbol_map, /* pack_index */ -1);

    free_map_of_parameters_and_values(map_of_parameters_and_values, num_map_items);

    if (!nodecl_is_constant(nodecl_evaluated_expr))
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Evaluation of regular constexpr call did not give a constant value\n");
        }
        if (check_expr_flags.must_be_constant)
        {
            error_printf("%s: error: call to constexpr function '%s' in constant-expression did not yield a constant value",
                    locus_to_str(locus),
                    print_decl_type_str(entry->type_information, entry->decl_context,
                        get_qualified_symbol_name(entry, entry->decl_context)));
        }
    }

    return nodecl_get_constant(nodecl_evaluated_expr);
}

static const_value_t* evaluate_constexpr_function_call(
        scope_entry_t* entry,
        nodecl_t converted_arg_list,
        decl_context_t decl_context,
        const locus_t* locus)
{
    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Evaluating constexpr call to function '%s'\n",
                get_qualified_symbol_name(entry, entry->decl_context));
    }

    const_value_t* value = NULL;
    if (symbol_entity_specs_get_is_constructor(entry))
    {
        value = evaluate_constexpr_constructor(
                entry,
                converted_arg_list,
                decl_context,
                locus);
    }
    else
    {
        value = evaluate_constexpr_regular_function_call(
                entry,
                converted_arg_list,
                decl_context,
                locus);
    }

    return value;
}

static void define_defaulted_default_constructor(scope_entry_t* entry,
        decl_context_t decl_context UNUSED_PARAMETER,
        const locus_t* locus)
{
    if (!nodecl_is_null(symbol_entity_specs_get_function_code(entry)))
        return;

    nodecl_t default_member_initializer = nodecl_null();
    check_nodecl_member_initializer_list(
            nodecl_null(),
            entry,
            decl_context,
            locus,
            &default_member_initializer);

    decl_context_t new_decl_context = new_block_context(entry->decl_context);

    // Empty body with member initializers
    nodecl_t nodecl_function_code =
        nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_null(),
                    new_decl_context,
                    locus),
                default_member_initializer,
                entry,
                locus);
    symbol_entity_specs_set_function_code(entry, nodecl_function_code);
}

static void apply_function_to_data_layout_members(
        scope_entry_t* entry,
        void (*fun)(scope_entry_t*, decl_context_t, const locus_t*, void *data),
        decl_context_t decl_context,
        const locus_t* locus,
        void *data)
{
    ERROR_CONDITION(entry->kind != SK_CLASS, "Invalid class symbol", 0);

    scope_entry_list_t* virtual_base_classes = class_type_get_virtual_base_classes(entry->type_information);
    scope_entry_list_t* direct_base_classes = class_type_get_direct_base_classes(entry->type_information);
    scope_entry_list_t* nonstatic_data_members = class_type_get_nonstatic_data_members(entry->type_information);

    scope_entry_list_iterator_t* it = NULL;
    for (it = entry_list_iterator_begin(virtual_base_classes);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* current_entry = entry_list_iterator_current(it);
        fun(current_entry, decl_context, locus, data);
    }
    entry_list_iterator_free(it);

    for (it = entry_list_iterator_begin(direct_base_classes);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* current_entry = entry_list_iterator_current(it);
        fun(current_entry, decl_context, locus, data);
    }
    entry_list_iterator_free(it);

    for (it = entry_list_iterator_begin(nonstatic_data_members);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        scope_entry_t* current_entry = entry_list_iterator_current(it);
        if (symbol_entity_specs_get_is_anonymous_union(current_entry))
            continue;
        fun(current_entry, decl_context, locus, data);
    }
    entry_list_iterator_free(it);

    entry_list_free(virtual_base_classes);
    entry_list_free(direct_base_classes);
    entry_list_free(nonstatic_data_members);
}

static void call_destructor_for_data_layout_member(
        scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus,
        void *data UNUSED_PARAMETER)
{
    if (!is_class_type_or_array_thereof(entry->type_information))
        return;

    type_t* class_type = entry->type_information;
    if (is_array_type(class_type))
        class_type = array_type_get_element_type(class_type);

    scope_entry_t* destructor = class_type_get_destructor(class_type);
    ERROR_CONDITION(destructor == NULL, "Invalid class", 0);

    nodecl_t arg = nodecl_make_symbol(entry, locus);
    nodecl_set_type(arg, get_lvalue_reference_type(entry->type_information));

    nodecl_t nodecl_call_to_destructor =
        cxx_nodecl_make_function_call(
                nodecl_make_symbol(destructor, locus),
                /* called name */ nodecl_null(),
                nodecl_make_list_1(arg),
                /* function_form */ nodecl_null(),
                get_void_type(),
                decl_context,
                locus);
    nodecl_free(nodecl_call_to_destructor);
}

static void define_defaulted_destructor(scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus)
{
    apply_function_to_data_layout_members(
            named_type_get_symbol(symbol_entity_specs_get_class_type(entry)),
            call_destructor_for_data_layout_member,
            decl_context,
            locus,
            NULL);

    decl_context_t new_decl_context = new_block_context(entry->decl_context);

    // Empty body
    nodecl_t nodecl_function_code =
        nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_null(),
                    new_decl_context,
                    locus),
                nodecl_null(),
                entry,
                locus);
    symbol_entity_specs_set_function_code(entry, nodecl_function_code);
}

void call_destructor_for_data_layout_members(
        scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus)
{
    apply_function_to_data_layout_members(
            named_type_get_symbol(symbol_entity_specs_get_class_type(entry)),
            call_destructor_for_data_layout_member,
            decl_context,
            locus,
            NULL);
}

typedef
struct special_member_info_tag
{
    scope_entry_list_t* (*function_set)(type_t*);
    const char* (*function_name)(type_t*, decl_context_t);
} special_member_info_t;

static const char* constructor_name(type_t* class_type,
        decl_context_t decl_context)
{
    ERROR_CONDITION(!is_named_class_type(class_type), "Invalid class", 0);

    const char* constructor_name = NULL;
    uniquestr_sprintf(&constructor_name, "%s::%s",
            print_type_str(class_type, decl_context),
            named_type_get_symbol(class_type)->symbol_name);

    return constructor_name;
}

static const char* copy_move_assignment_operator(type_t* class_type,
        decl_context_t decl_context)
{
    ERROR_CONDITION(!is_named_class_type(class_type), "Invalid class", 0);

    const char* constructor_name = NULL;
    uniquestr_sprintf(&constructor_name, "%s::operator=",
            print_type_str(class_type, decl_context));

    return constructor_name;
}

static void call_specific_overloadable_special_member_for_data_layout_member(
        scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus,
        void *p)
{
    special_member_info_t *special_member_info =  (special_member_info_t*)p;

    if (!is_class_type_or_array_thereof(entry->type_information))
        return;

    type_t* class_type = entry->type_information;
    if (is_array_type(class_type))
        class_type = array_type_get_element_type(class_type);

    scope_entry_list_t* copy_constructors = (special_member_info->function_set)(class_type);

    type_t* arg_type = lvalue_ref(class_type);

    scope_entry_list_t* overload_set = unfold_and_mix_candidate_functions(copy_constructors,
            NULL, &arg_type, /* num_arguments */ 1,
            decl_context,
            locus, /* explicit_template_arguments */ NULL);
    entry_list_free(copy_constructors);

    candidate_t* candidate_set = NULL;
    scope_entry_list_iterator_t* it = NULL;
    for (it = entry_list_iterator_begin(overload_set);
            !entry_list_iterator_end(it);
            entry_list_iterator_next(it))
    {
        candidate_set = candidate_set_add(candidate_set,
                entry_list_iterator_current(it),
                1, &arg_type);
    }
    entry_list_iterator_free(it);

    // Now we have all the constructors, perform an overload resolution on them
    scope_entry_t* overload_resolution = solve_overload(
            candidate_set, decl_context, locus);

    if (overload_resolution != NULL)
    {
        candidate_set_free(&candidate_set);

        nodecl_t arg = nodecl_make_symbol(entry, locus);
        nodecl_set_type(arg, arg_type);

        nodecl_t nodecl_call_to_destructor =
            cxx_nodecl_make_function_call(
                    nodecl_make_symbol(overload_resolution, locus),
                    /* called name */ nodecl_null(),
                    nodecl_make_list_1(arg),
                    /* function_form */ nodecl_null(),
                    get_void_type(),
                    decl_context,
                    locus);
        nodecl_free(nodecl_call_to_destructor);
    }
    else
    {
        const char* constructor_name =
            (special_member_info->function_name)(class_type, decl_context);

        error_message_overload_failed(candidate_set,
                constructor_name,
                decl_context,
                1, &arg_type,
                /* implicit_argument */ NULL,
                locus);
        candidate_set_free(&candidate_set);
    }
}

static void define_defaulted_copy_constructor(scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus)
{
    if (!nodecl_is_null(symbol_entity_specs_get_function_code(entry)))
        return;

    special_member_info_t special_member = {
        class_type_get_copy_constructors,
        constructor_name,
    };
    apply_function_to_data_layout_members(
            named_type_get_symbol(symbol_entity_specs_get_class_type(entry)),
            call_specific_overloadable_special_member_for_data_layout_member,
            decl_context,
            locus,
            &special_member);

    decl_context_t new_decl_context = new_block_context(entry->decl_context);

    // Empty body
    nodecl_t nodecl_function_code =
        nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_null(),
                    new_decl_context,
                    locus),
                nodecl_null(),
                entry,
                locus);
    symbol_entity_specs_set_function_code(entry, nodecl_function_code);
}

static void define_defaulted_copy_assignment_operator(scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus)
{
    if (!nodecl_is_null(symbol_entity_specs_get_function_code(entry)))
        return;

    special_member_info_t special_member = {
        class_type_get_copy_assignment_operators,
        copy_move_assignment_operator,
    };
    apply_function_to_data_layout_members(
            named_type_get_symbol(symbol_entity_specs_get_class_type(entry)),
            call_specific_overloadable_special_member_for_data_layout_member,
            decl_context,
            locus,
            &special_member);

    decl_context_t new_decl_context = new_block_context(entry->decl_context);

    // Empty body
    nodecl_t nodecl_function_code =
        nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_null(),
                    new_decl_context,
                    locus),
                nodecl_null(),
                entry,
                locus);
    symbol_entity_specs_set_function_code(entry, nodecl_function_code);
}

static void define_defaulted_move_constructor(scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus)
{
    if (!nodecl_is_null(symbol_entity_specs_get_function_code(entry)))
        return;

    special_member_info_t special_member = {
        class_type_get_move_constructors,
        constructor_name,
    };
    apply_function_to_data_layout_members(
            named_type_get_symbol(symbol_entity_specs_get_class_type(entry)),
            call_specific_overloadable_special_member_for_data_layout_member,
            decl_context,
            locus,
            &special_member);

    decl_context_t new_decl_context = new_block_context(entry->decl_context);

    // Empty body
    nodecl_t nodecl_function_code =
        nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_null(),
                    new_decl_context,
                    locus),
                nodecl_null(),
                entry,
                locus);
    symbol_entity_specs_set_function_code(entry, nodecl_function_code);
}

static void define_defaulted_move_assignment_operator(scope_entry_t* entry,
        decl_context_t decl_context,
        const locus_t* locus)
{
    if (!nodecl_is_null(symbol_entity_specs_get_function_code(entry)))
        return;

    special_member_info_t special_member = {
        class_type_get_move_assignment_operators,
        copy_move_assignment_operator,
    };
    apply_function_to_data_layout_members(
            named_type_get_symbol(symbol_entity_specs_get_class_type(entry)),
            call_specific_overloadable_special_member_for_data_layout_member,
            decl_context,
            locus,
            &special_member);

    decl_context_t new_decl_context = new_block_context(entry->decl_context);

    // Empty body
    nodecl_t nodecl_function_code =
        nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_null(),
                    new_decl_context,
                    locus),
                nodecl_null(),
                entry,
                locus);
    symbol_entity_specs_set_function_code(entry, nodecl_function_code);
}

static void define_defaulted_special_member(
        scope_entry_t* special_member,
        decl_context_t decl_context,
        const locus_t* locus)
{
    ERROR_CONDITION(!symbol_entity_specs_get_is_defaulted(special_member),
            "This special member is not defaulted", 0);
    ERROR_CONDITION(symbol_entity_specs_get_is_deleted(special_member),
            "Attempt to define a deleted special member", 0);

    if (symbol_entity_specs_get_is_default_constructor(special_member))
    {
        define_defaulted_default_constructor(special_member, decl_context, locus);
    }
    else if (symbol_entity_specs_get_is_destructor(special_member))
    {
        define_defaulted_destructor(special_member, decl_context, locus);
    }
    else if (symbol_entity_specs_get_is_copy_constructor(special_member))
    {
        define_defaulted_copy_constructor(special_member, decl_context, locus);
    }
    else if (symbol_entity_specs_get_is_move_constructor(special_member))
    {
        define_defaulted_move_constructor(special_member, decl_context, locus);
    }
    else if (symbol_entity_specs_get_is_copy_assignment_operator(special_member))
    {
        define_defaulted_copy_assignment_operator(special_member, decl_context, locus);
    }
    else if (symbol_entity_specs_get_is_move_assignment_operator(special_member))
    {
        define_defaulted_move_assignment_operator(special_member, decl_context, locus);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}


static void define_inherited_constructor(
        scope_entry_t* new_inherited_constructor,
        scope_entry_t* inherited_constructor,
        const locus_t* locus)
{
    // Do not early return here
#define return 1 = 1;
    enum must_be_constant_t must_be_constant = check_expr_flags.must_be_constant;
    check_expr_flags.must_be_constant = MUST_NOT_BE_CONSTANT;

    int num_parameters = function_type_get_num_parameters(new_inherited_constructor->type_information);
    if (function_type_get_has_ellipsis(new_inherited_constructor->type_information))
        num_parameters--;

    decl_context_t block_context = new_block_context(new_inherited_constructor->decl_context);
    block_context.current_scope->related_entry = new_inherited_constructor;

    nodecl_t nodecl_arg_list = nodecl_null();

    char ok = 1;

    int i;
    for (i = 0; i < num_parameters && ok; i++)
    {
        const char *parameter_name = NULL;
        uniquestr_sprintf(&parameter_name, "parameter#%d", i);

        scope_entry_t* new_param_symbol = new_symbol(block_context,
                block_context.current_scope,
                parameter_name);
        new_param_symbol->kind = SK_VARIABLE;
        new_param_symbol->type_information =
            function_type_get_parameter_type_num(new_inherited_constructor->type_information, i);

        symbol_set_as_parameter_of_function(
                new_param_symbol,
                new_inherited_constructor,
                /* nesting */ 0, i);

        nodecl_t nodecl_symbol_ref = nodecl_make_symbol(new_param_symbol, locus);
        nodecl_set_type(nodecl_symbol_ref, lvalue_ref(new_param_symbol->type_information));

        symbol_entity_specs_add_related_symbols(new_inherited_constructor,
                new_param_symbol);

        type_t* cast_type = new_param_symbol->type_information;
        if (!is_any_reference_type(cast_type))
            cast_type = get_rvalue_reference_type(new_param_symbol->type_information);

        nodecl_t nodecl_arg = nodecl_null();
        check_nodecl_cast_expr(
                nodecl_symbol_ref,
                block_context,
                cast_type,
                "static_cast",
                locus,
                &nodecl_arg);

        if (!nodecl_is_err_expr(nodecl_arg))
        {
            nodecl_arg_list = nodecl_append_to_list(nodecl_arg_list, nodecl_arg);
        }
        else
        {
            ok = 0;
        }
    }

    if (ok)
    {
        nodecl_t nodecl_init = nodecl_make_cxx_parenthesized_initializer(
                nodecl_arg_list,
                locus);

        check_nodecl_initialization(
                nodecl_init,
                block_context,
                named_type_get_symbol(symbol_entity_specs_get_class_type(inherited_constructor)),
                get_unqualified_type(symbol_entity_specs_get_class_type(inherited_constructor)),
                &nodecl_init,
                /* is_auto_type */ 0,
                /* is_decltype_auto */ 0);

        if (!nodecl_is_err_expr(nodecl_init))
        {
            nodecl_t nodecl_member_init_list =
                nodecl_make_list_1(
                        nodecl_make_member_init(
                            nodecl_init,
                            named_type_get_symbol(symbol_entity_specs_get_class_type(inherited_constructor)),
                            locus));

            symbol_entity_specs_set_function_code(new_inherited_constructor,
                    nodecl_make_function_code(
                        nodecl_make_context(
                            nodecl_make_list_1(
                                // Empty body
                                nodecl_make_compound_statement(
                                    nodecl_null(),
                                    nodecl_null(),
                                    locus)),
                            block_context,
                            locus),
                        nodecl_member_init_list,
                        new_inherited_constructor,
                        locus));

            new_inherited_constructor->defined = 1;
            symbol_entity_specs_set_alias_to(new_inherited_constructor, NULL);
            symbol_entity_specs_set_is_instantiable(new_inherited_constructor, 0);
            symbol_entity_specs_set_emission_template(new_inherited_constructor, NULL);
        }
    }

    check_expr_flags.must_be_constant = must_be_constant;
#undef return
}

struct instantiate_default_argument_header_message_fun_data_tag
{
    // scope_entry_t* called_symbol;
    scope_entry_t* function_with_defaults;
    int arg_i;
    const locus_t* locus;
};

static const char* instantiate_default_argument_header_message_fun(void* v)
{
    struct instantiate_default_argument_header_message_fun_data_tag* p =
        (struct instantiate_default_argument_header_message_fun_data_tag*)v;

    const char* default_argument_context_str;
    uniquestr_sprintf(&default_argument_context_str,
            "%s: info: during instantiation of default argument '%s'\n",
            locus_to_str(p->locus),
            codegen_to_str(
                symbol_entity_specs_get_default_argument_info_num(p->function_with_defaults, p->arg_i)->argument,
                p->function_with_defaults->decl_context));

    return default_argument_context_str;
}

nodecl_t cxx_nodecl_make_function_call(
        nodecl_t orig_called,
        nodecl_t called_name,
        nodecl_t arg_list,
        nodecl_t function_form,
        type_t* t,
        decl_context_t decl_context,
        const locus_t* locus)
{
    ERROR_CONDITION(!nodecl_is_null(arg_list)
            && !nodecl_is_list(arg_list), "Argument nodecl is not a list", 0);

    // Adjust the return type
    if (t != NULL
            && !is_any_reference_type(t)
            && !is_array_type(t) // This should never happen
            && !is_class_type(t))
    {
        t = get_unqualified_type(t);
    }

    bool preserve_orig_name = false;

    scope_entry_t* orig_called_symbol = nodecl_get_symbol(orig_called);

    scope_entry_t* called_symbol = entry_advance_aliases(orig_called_symbol);
    nodecl_t called = orig_called;

    if (called_symbol != orig_called_symbol)
    {
        called = nodecl_make_symbol(called_symbol, locus);

        preserve_orig_name = true;
    }

    char any_arg_is_value_dependent = 0;
    any_arg_is_value_dependent = nodecl_expr_is_value_dependent(called);

    // This list will be the same as arg_list but with explicit conversions stored
    nodecl_t converted_arg_list = nodecl_null();

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(arg_list, &num_items);

    type_t* function_type = NULL;
    if (called_symbol != NULL)
    {
        function_type = no_ref(called_symbol->type_information);
    }
    else
    {
        function_type = no_ref(nodecl_get_type(called));
    }
    if (is_pointer_to_function_type(function_type))
    {
        function_type = pointer_type_get_pointee_type(function_type);
    }
    ERROR_CONDITION(!is_function_type(function_type), "%s is not a function type!", 
            function_type == NULL ? "<<NULL>>" : print_declarator(function_type));

    if (called_symbol != NULL
            && !check_expr_flags.is_non_executable)
    {
        if (symbol_entity_specs_get_is_constructor(called_symbol)
                && symbol_entity_specs_get_alias_to(called_symbol) != NULL)
        {
            // If this is an inheriting constructor being odr-used, define it now
            define_inherited_constructor(
                    called_symbol,
                    symbol_entity_specs_get_alias_to(called_symbol),
                    locus);
        }
        else if ((symbol_entity_specs_get_is_default_constructor(called_symbol)
                    || symbol_entity_specs_get_is_copy_constructor(called_symbol)
                    || symbol_entity_specs_get_is_copy_constructor(called_symbol)
                    || symbol_entity_specs_get_is_copy_constructor(called_symbol)
                    || symbol_entity_specs_get_is_destructor(called_symbol))
                && symbol_entity_specs_get_is_defaulted(called_symbol))
        {
            // defaulted special member being odr-used
            define_defaulted_special_member(called_symbol, decl_context, locus);
        }
    }

    char is_promoting_ellipsis = 0;
    int num_parameters = function_type_get_num_parameters(function_type);
    if (function_type_get_has_ellipsis(function_type))
    {
        is_promoting_ellipsis = is_ellipsis_type(
                function_type_get_parameter_type_num(function_type, num_parameters - 1)
                );
        num_parameters--;
    }

    // j is used to index the types of the function type
    // i is used to index the arguments of the call (possibly ignoring the
    // first one if it is known to be 'this')
    int j = 0;
    int i = 0;

    char ignore_this = 0;
    if (called_symbol != NULL
            && symbol_entity_specs_get_is_member(called_symbol)
            && !symbol_entity_specs_get_is_static(called_symbol)
            // Constructors are nonstatic but do not have
            // implicit argument
            && !symbol_entity_specs_get_is_constructor(called_symbol))
    {
        // Ignore the first argument as we know it is 'this'
        i = 1;

        any_arg_is_value_dependent = any_arg_is_value_dependent
            || nodecl_expr_is_value_dependent(list[0]);

        converted_arg_list = nodecl_append_to_list(converted_arg_list, list[0]);
        ignore_this = 1;
    }

    for (; i < num_items; i++, j++)
    {
        any_arg_is_value_dependent = any_arg_is_value_dependent
            || nodecl_expr_is_value_dependent(list[i]);

        type_t* arg_type = nodecl_get_type(list[i]);
        if (j < num_parameters)
        {
            type_t* param_type = function_type_get_parameter_type_num(function_type, j);

            char verify_conversion = 1;
            C_LANGUAGE()
            {
                // Do not verify functions lacking prototype or parameter types that
                // are transparent unions, their validity has been verified elsewhere
                // and do not fit the regular SCS code
                verify_conversion = !(
                        function_type_get_lacking_prototype(function_type)
                        || (is_class_type(param_type)
                            && (is_transparent_union(param_type)
                                || is_transparent_union(get_actual_class_type(param_type))))
                        );
            }

            if (!equivalent_types(
                        get_unqualified_type(arg_type),
                        get_unqualified_type(param_type)))
            {
                list[i] = cxx_nodecl_make_conversion_internal(list[i],
                        param_type,
                        nodecl_get_locus(list[i]),
                        verify_conversion);
            }
        }
        else
        {
            if (is_promoting_ellipsis)
            {
                // We do not emit diagnostic here because it is too late to take any
                // corrective measure, the caller code should have checked it earlier
                type_t* default_conversion = compute_default_argument_conversion(
                        arg_type,
                        /* decl_context is not used since we do not request diagnostics*/
                        CURRENT_COMPILED_FILE->global_decl_context,
                        nodecl_get_locus(list[i]),
                        /* emit diagnostic */ 0);
                ERROR_CONDITION(is_error_type(default_conversion),
                        "This default argument conversion is wrong, should have been checked earlier\n",
                        0);

                list[i] = cxx_nodecl_make_conversion_internal(list[i],
                        default_conversion,
                        nodecl_get_locus(list[i]),
                        /* verify_conversion */ 0);
            }
        }

        converted_arg_list = nodecl_append_to_list(converted_arg_list, list[i]);
    }

    xfree(list);

    if (called_symbol != NULL)
    {
        CXX_LANGUAGE()
        {
            // Update exception stuff
            if (called_symbol->kind == SK_FUNCTION
                    || called_symbol->kind == SK_VARIABLE)
            {
                if (!nodecl_is_null(symbol_entity_specs_get_noexception(called_symbol))
                        && nodecl_expr_is_value_dependent(symbol_entity_specs_get_noexception(called_symbol)))
                {
                    instantiation_symbol_map_t* instantiation_symbol_map = NULL;
                    if (symbol_entity_specs_get_is_member(called_symbol))
                    {
                        instantiation_symbol_map
                            = symbol_entity_specs_get_instantiation_symbol_map(
                                    named_type_get_symbol(symbol_entity_specs_get_class_type(called_symbol)));
                    }
                    nodecl_t new_noexception = instantiate_expression_non_executable(
                            symbol_entity_specs_get_noexception(called_symbol),
                            called_symbol->decl_context,
                            instantiation_symbol_map, /* pack_index */ -1);

                    if (nodecl_is_err_expr(new_noexception))
                    {
                        return new_noexception;
                    }

                    symbol_entity_specs_set_noexception(called_symbol, new_noexception);
                }
                else if (!symbol_entity_specs_get_any_exception(called_symbol)
                        && symbol_entity_specs_get_num_exceptions(called_symbol) != 0)
                {
                    char any_is_dependent = 0;

                    int idx_exception;
                    for (idx_exception = 0; idx_exception < symbol_entity_specs_get_num_exceptions(called_symbol); idx_exception++)
                    {
                        if (is_dependent_type(symbol_entity_specs_get_exceptions_num(called_symbol, idx_exception)))
                        {
                            any_is_dependent = 1;
                            break;
                        }
                    }

                    if (any_is_dependent)
                    {
                        for (idx_exception = 0; idx_exception < symbol_entity_specs_get_num_exceptions(called_symbol); idx_exception++)
                        {
                            type_t* updated_exception = update_type_for_instantiation(
                                    symbol_entity_specs_get_exceptions_num(called_symbol, idx_exception),
                                    called_symbol->decl_context,
                                    locus,
                                    /* instantiation_symbol_map */ NULL,
                                    /* pack_index */ -1);

                            if (is_sequence_of_types(updated_exception))
                            {
                                int idx_seq, n = sequence_of_types_get_num_types(updated_exception);
                                for (idx_seq = 0; idx_seq < n; idx_seq++)
                                {
                                    symbol_entity_specs_add_exceptions(
                                            called_symbol,
                                            sequence_of_types_get_type_num(updated_exception, idx_seq));
                                }
                            }
                            else
                            {
                                symbol_entity_specs_add_exceptions(
                                        called_symbol,
                                        updated_exception);
                            }
                        }
                    }
                }
            }
        }

        if (called_symbol->kind == SK_FUNCTION)
        {
            ensure_function_is_emitted(called_symbol, decl_context, nodecl_get_locus(called));

            CXX_LANGUAGE()
            {
                // Default arguments
                int arg_i = nodecl_list_length(converted_arg_list);
                if (ignore_this)
                {
                    // Do not count the implicit member
                    arg_i--;
                }
                ERROR_CONDITION(arg_i < 0, "Invalid argument count %d\n", arg_i);

                scope_entry_t* function_with_defaults = called_symbol;
                if (is_template_specialized_type(called_symbol->type_information))
                {
                    function_with_defaults =
                        named_type_get_symbol(
                                template_type_get_primary_type(
                                    template_specialized_type_get_related_template_type(
                                        called_symbol->type_information)));
                }

                for(; arg_i < num_parameters; arg_i++)
                {
                    ERROR_CONDITION(
                            symbol_entity_specs_get_default_argument_info_num(function_with_defaults, arg_i) == NULL,
                            "Invalid default argument information %d", arg_i);

                    type_t* default_param_type = function_type_get_parameter_type_num(function_type, arg_i);

                    instantiation_symbol_map_t* instantiation_symbol_map = NULL;
                    if (symbol_entity_specs_get_is_member(called_symbol))
                    {
                        instantiation_symbol_map
                            = symbol_entity_specs_get_instantiation_symbol_map(
                                    named_type_get_symbol(
                                        symbol_entity_specs_get_class_type(called_symbol)));
                    }

                    header_message_fun_t instantiation_header;
                    instantiation_header.message_fun = instantiate_default_argument_header_message_fun;
                    {
                        struct instantiate_default_argument_header_message_fun_data_tag* p = xcalloc(1, sizeof(*p));
                        // p->called_symbol = called_symbol;
                        p->function_with_defaults = function_with_defaults;
                        p->arg_i = arg_i;
                        p->locus = locus;
                        instantiation_header.data = p;
                    }
                    diagnostic_context_push_instantiation(instantiation_header);

                    // We need to update the default argument
                    nodecl_t new_default_argument = instantiate_expression(
                            symbol_entity_specs_get_default_argument_info_num(function_with_defaults, arg_i)->argument,
                            called_symbol->decl_context,
                            instantiation_symbol_map, /* pack_index */ -1);

                    if (nodecl_is_err_expr(new_default_argument))
                    {
                        diagnostic_context_pop_and_commit();
                        return new_default_argument;
                    }

                    check_nodecl_expr_initializer(new_default_argument,
                            called_symbol->decl_context,
                            default_param_type,
                            /* disallow_narrowing */ 0,
                            IK_COPY_INITIALIZATION,
                            &new_default_argument);
                    diagnostic_context_pop_and_commit();

                    if (nodecl_is_err_expr(new_default_argument))
                    {
                        return new_default_argument;
                    }

                    if (!symbol_entity_specs_get_default_argument_info_num(function_with_defaults, arg_i)->is_hidden)
                    {
                        // Wrap the expression in a default argumet node
                        new_default_argument = nodecl_make_default_argument(new_default_argument,
                                locus);
                    }

                    converted_arg_list = nodecl_append_to_list(
                            converted_arg_list,
                            new_default_argument);
                }
            }

            if (symbol_entity_specs_get_is_member(called_symbol)
                    && symbol_entity_specs_get_is_virtual(called_symbol)
                    && (nodecl_is_null(called_name)
                        || (nodecl_get_kind(called_name) != NODECL_CXX_DEP_NAME_NESTED // A::f()
                            && nodecl_get_kind(called_name) != NODECL_CXX_DEP_GLOBAL_NAME_NESTED // ::A::f()
                            && nodecl_get_kind(called_name) != NODECL_CLASS_MEMBER_ACCESS)
                        || (nodecl_get_kind(called_name) == NODECL_CLASS_MEMBER_ACCESS // x.y()
                            && (nodecl_is_null(nodecl_get_child(called_name, 2))
                                || ((nodecl_get_kind(nodecl_get_child(called_name, 2))
                                        != NODECL_CXX_DEP_NAME_NESTED) // x.A::f()
                                    && (nodecl_get_kind(nodecl_get_child(called_name, 2))
                                        != NODECL_CXX_DEP_GLOBAL_NAME_NESTED )  ))) )) // x.::A::f()
            {
                nodecl_t result = nodecl_make_virtual_function_call(called,
                        converted_arg_list,
                        function_form, t,
                        locus);
                nodecl_expr_set_is_value_dependent(result, any_arg_is_value_dependent);

                if (!is_dependent_type(t)
                        && is_named_class_type(t))
                {
                    scope_entry_t* class_sym_ret = named_type_get_symbol(t);
                    class_type_complete_if_needed(class_sym_ret, decl_context, locus);
                }

                return result;
            }
            else
            {
                nodecl_t alternate_name = nodecl_null();
                if (preserve_orig_name)
                {
                    alternate_name = orig_called;
                }
                else if (!nodecl_is_null(called_name))
                {
                    if (nodecl_get_kind(called_name) == NODECL_CLASS_MEMBER_ACCESS
                            && !nodecl_is_null(nodecl_get_child(called_name, 2))
                            && ((nodecl_get_kind(nodecl_get_child(called_name, 2))
                                    == NODECL_CXX_DEP_NAME_NESTED) // x.A::f()
                                || (nodecl_get_kind(nodecl_get_child(called_name, 2))
                                    == NODECL_CXX_DEP_GLOBAL_NAME_NESTED )  )) // x.::A::f
                    {
                        alternate_name = nodecl_get_child(called_name, 2);
                    }
                    else if (nodecl_get_kind(called_name) == NODECL_CXX_DEP_NAME_NESTED
                            || nodecl_get_kind(called_name) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED)
                    {
                        alternate_name = called_name;
                    }
                }

                nodecl_t result = nodecl_make_function_call(called,
                        converted_arg_list,
                        alternate_name,
                        function_form, t,
                        locus);

                if (!check_expr_flags.do_not_call_constexpr)
                {
                    if (symbol_entity_specs_get_is_constexpr(called_symbol))
                    {
                        const_value_t* const_value = evaluate_constexpr_function_call(
                                called_symbol,
                                converted_arg_list,
                                decl_context,
                                locus);

                        nodecl_set_constant(result, const_value);
                    }
                    // For trivial copy/move constructors of literal types, use
                    // the const value (if any) of its first argument
                    else if ((symbol_entity_specs_get_is_copy_constructor(called_symbol)
                                || symbol_entity_specs_get_is_move_constructor(called_symbol))
                            && symbol_entity_specs_get_is_trivial(called_symbol)
                            && is_literal_type(symbol_entity_specs_get_class_type(called_symbol)))
                    {
                        nodecl_t first_argument = nodecl_null();

                        int num_args = 0;
                        nodecl_t* simplify_args = nodecl_unpack_list(converted_arg_list, &num_args);
                        if (num_args > 0)
                        {
                            first_argument = simplify_args[0];
                        }
                        xfree(simplify_args);

                        nodecl_set_constant(result,
                                nodecl_get_constant(first_argument));
                    }
                    // Attempt to evaluate a builtin as well
                    else if (symbol_entity_specs_get_is_builtin(called_symbol)
                            && symbol_entity_specs_get_simplify_function(called_symbol) != NULL)
                    {
                        int num_simplify_args = 0;
                        nodecl_t* simplify_args = nodecl_unpack_list(converted_arg_list, &num_simplify_args);

                        nodecl_t simplified_value = (symbol_entity_specs_get_simplify_function(called_symbol))
                            (called_symbol, num_simplify_args, simplify_args);

                        xfree(simplify_args);

                        if (!nodecl_is_null(simplified_value)
                                && nodecl_is_constant(simplified_value))
                        {
                            nodecl_set_constant(result, nodecl_get_constant(simplified_value));
                        }
                    }
                    else if (check_expr_flags.must_be_constant)
                    {
                        error_printf("%s: error: cannot call non-constexpr '%s' in constant expression\n",
                                locus_to_str(locus),
                                print_decl_type_str(called_symbol->type_information, called_symbol->decl_context,
                                    get_qualified_symbol_name(called_symbol, called_symbol->decl_context)));
                    }
                }

                nodecl_expr_set_is_value_dependent(result, any_arg_is_value_dependent);

                if (!is_dependent_type(t)
                        && is_named_class_type(t))
                {
                    scope_entry_t* class_sym_ret = named_type_get_symbol(t);
                    class_type_complete_if_needed(class_sym_ret, decl_context, locus);
                }

                return result;
            }
        }
        else if (called_symbol->kind == SK_VARIABLE
                && is_pointer_to_function_type(no_ref(called_symbol->type_information)))
        {
            nodecl_t result = nodecl_make_function_call(
                    nodecl_make_dereference(called,
                        lvalue_ref(pointer_type_get_pointee_type(called_symbol->type_information)),
                        nodecl_get_locus(called)),
                    converted_arg_list,
                    /* alternate_name */ nodecl_null(),
                    function_form,
                    // A pointer to function cannot have template arguments
                    t, locus);

            nodecl_expr_set_is_value_dependent(result, any_arg_is_value_dependent);

            if (!is_dependent_type(t)
                    && is_named_class_type(t))
            {
                scope_entry_t* class_sym_ret = named_type_get_symbol(t);
                class_type_complete_if_needed(class_sym_ret, decl_context, locus);
            }

            return result;
        }
        else
        {
            nodecl_t result = nodecl_make_function_call(called,
                    converted_arg_list,
                    /* alternate_name */ nodecl_null(),
                    function_form, t,
                    locus);
            nodecl_expr_set_is_value_dependent(result, any_arg_is_value_dependent);

            if (!is_dependent_type(t)
                    && is_named_class_type(t))
            {
                scope_entry_t* class_sym_ret = named_type_get_symbol(t);
                class_type_complete_if_needed(class_sym_ret, decl_context, locus);
            }

            return result;
        }
    }
    else
    {
        nodecl_t result = nodecl_make_function_call(called,
                converted_arg_list,
                /* alternate_name */ nodecl_null(),
                function_form, t,
                locus);

        if (!is_dependent_type(t)
                && is_named_class_type(t))
        {
            scope_entry_t* class_sym_ret = named_type_get_symbol(t);
            class_type_complete_if_needed(class_sym_ret, decl_context, locus);
        }

        return result;
    }
}

char check_nontype_template_argument_type(type_t* t)
{
    return is_integral_type(t)
        || is_enum_type(t)
        || is_lvalue_reference_type(t)
        || is_pointer_type(t)
        || is_pointer_to_member_type(t)
        || is_dependent_type(t);
}

char check_nodecl_nontype_template_argument_expression(nodecl_t nodecl_expr,
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    if (nodecl_expr_is_value_dependent(nodecl_expr)
            || nodecl_expr_is_type_dependent(nodecl_expr))
    {
        *nodecl_output = nodecl_expr;
        return 1;
    }

    type_t* expr_type = nodecl_get_type(nodecl_expr);

    scope_entry_t* related_symbol = NULL;

    char should_be_a_constant_expression = 1;
    char valid = 0;
    if (nodecl_get_kind(nodecl_expr) == NODECL_SYMBOL
            && nodecl_get_symbol(nodecl_expr)->kind == SK_TEMPLATE_NONTYPE_PARAMETER)
    {
        valid = 1;
    }
    else if (is_integral_type(no_ref(expr_type))
            || is_enum_type(no_ref(expr_type)))
    {
        valid = 1;
    }
    else if (is_pointer_type(no_ref(expr_type))
            || is_function_type(no_ref(expr_type)))
    {
        // &a
        // a
        nodecl_t current_expr = nodecl_expr;
        char lacks_ref = 1;
        if (nodecl_get_kind(current_expr) == NODECL_REFERENCE)
        {
            current_expr = nodecl_get_child(current_expr, 0);
            lacks_ref = 0;
        }
        related_symbol = nodecl_get_symbol(current_expr);
        if (related_symbol != NULL
                && ((related_symbol->kind == SK_VARIABLE 
                        && (!symbol_entity_specs_get_is_member(related_symbol) 
                            || symbol_entity_specs_get_is_static(related_symbol)))
                    || (related_symbol->kind == SK_FUNCTION)
                    || (related_symbol->kind == SK_TEMPLATE 
                        && is_function_type(template_type_get_primary_type(related_symbol->type_information)))))
        {
            if (!lacks_ref)
            {
                valid = 1;
                should_be_a_constant_expression = 0;
            }
            else if ((related_symbol->kind == SK_VARIABLE 
                        && (is_array_type(related_symbol->type_information) 
                            /* || is_pointer_to_function_type(related_symbol->type_information) */))
                    || (related_symbol->kind == SK_FUNCTION)
                    || (related_symbol->kind == SK_TEMPLATE 
                        && is_function_type(template_type_get_primary_type(related_symbol->type_information))))
            {
                valid = 1;
                should_be_a_constant_expression = 0;
            }
        }
    }
    else if (is_unresolved_overloaded_type(expr_type))
    {
        valid = 1;
        should_be_a_constant_expression = 0;
    }
    else if (is_pointer_to_member_type(no_ref(expr_type)))
    {
        // &C::id
        nodecl_t current_expr = nodecl_expr;
        related_symbol = nodecl_get_symbol(current_expr);
        if (related_symbol != NULL)
        {
            valid = 1;
            should_be_a_constant_expression = 0;
        }
    }

    if (!valid)
    {
            error_printf("%s: error: invalid template argument '%s' for a nontype template parameter\n",
                    nodecl_locus_to_str(nodecl_expr),
                    codegen_to_str(nodecl_expr, nodecl_retrieve_context(nodecl_expr)));

        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
        nodecl_free(nodecl_expr);
        return 0;
    }

    if (should_be_a_constant_expression
            && !nodecl_is_constant(nodecl_expr))
    {
        error_printf("%s: error: nontype template argument '%s' is not constant\n",
                nodecl_locus_to_str(nodecl_expr),
                codegen_to_str(nodecl_expr, decl_context));
        *nodecl_output = nodecl_make_err_expr(nodecl_get_locus(nodecl_expr));
        nodecl_free(nodecl_expr);
        return 0;
    }

    *nodecl_output = nodecl_expr;

    if (related_symbol != NULL
            && !symbol_entity_specs_get_is_template_parameter(related_symbol))
    {
        nodecl_set_symbol(*nodecl_output, related_symbol);
    }

    return 1;
}

char check_nontype_template_argument_expression(AST expression, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    nodecl_t nodecl_expr = nodecl_null();

    enum must_be_constant_t must_be_constant = check_expr_flags.must_be_constant;

    check_expr_flags.must_be_constant = MUST_BE_NONTYPE_TEMPLATE_PARAMETER;
    check_expression_impl_(expression, decl_context, &nodecl_expr);
    check_expr_flags.must_be_constant = must_be_constant;

    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_expr;
        return 0;
    }

    return check_nodecl_nontype_template_argument_expression(nodecl_expr,
            decl_context,
            nodecl_output);
}

// Instantiation of expressions
#include "cxx-nodecl-visitor.h"

typedef
struct nodecl_instantiate_expr_visitor_tag
{
    nodecl_external_visitor_t _base_visitor;

    // Context info
    decl_context_t decl_context;

    // Keep the resulting expression here
    nodecl_t nodecl_result;

    // Index of pack expansion
    int pack_index;

    // Instantiation map
    instantiation_symbol_map_t* instantiation_symbol_map;
} nodecl_instantiate_expr_visitor_t;

typedef void (*nodecl_instantiate_expr_visitor_fun_t)(nodecl_instantiate_expr_visitor_t* visitor, nodecl_t node);
typedef void (*nodecl_visitor_fun_t)(nodecl_external_visitor_t* visitor, nodecl_t node);

static inline nodecl_visitor_fun_t instantiate_expr_visitor_fun(nodecl_instantiate_expr_visitor_fun_t p)
{
    return NODECL_VISITOR_FUN(p);
}

// This function for debug only
static const char* codegen_expression_to_str(nodecl_t expr, decl_context_t decl_context)
{
    if (nodecl_is_list(expr))
    {
        int n = 0;
        nodecl_t* list = nodecl_unpack_list(expr, &n);
        int i;
        const char* result = UNIQUESTR_LITERAL("{");

        for (i = 0; i < n; i++)
        {
            if (i > 0)
                result = strappend(result, ", ");

            result = strappend(result,
                    codegen_expression_to_str(list[i], decl_context));
        }

        result = strappend(result, "}");

        return result;
    }
    else
    {
        return codegen_to_str(expr, decl_context);
    }
}

static nodecl_t instantiate_expr_walk(nodecl_instantiate_expr_visitor_t* visitor, nodecl_t node)
{
    visitor->nodecl_result = nodecl_null();
    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Instantiating expression '%s' (kind=%s, %s). constexpr calls evaluation is %s\n",
                codegen_expression_to_str(node, visitor->decl_context),
                !nodecl_is_null(node) ? ast_print_node_type(nodecl_get_kind(node)) : "<<NULL>>",
                !nodecl_is_null(node) ? nodecl_locus_to_str(node) : "<<no-locus>>",
                check_expr_flags.do_not_call_constexpr ? "OFF" : "ON");

    }
    NODECL_WALK(visitor, node);
    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Expression '%s' (kind=%s, %s) instantiated to expression '%s' "
                "with type '%s'. constexpr calls evaluation is %s",
                codegen_expression_to_str(node, visitor->decl_context),
                !nodecl_is_null(node) ? ast_print_node_type(nodecl_get_kind(node)) : "<<NULL>>",
                !nodecl_is_null(node) ? nodecl_locus_to_str(node) : "<<no-locus>>",
                codegen_expression_to_str(visitor->nodecl_result, visitor->decl_context),
                print_declarator(nodecl_get_type(visitor->nodecl_result)),
                check_expr_flags.do_not_call_constexpr ? "OFF" : "ON");
        if (nodecl_is_constant(visitor->nodecl_result))
        {
            fprintf(stderr, " with a constant value of '%s'",
                    const_value_to_str(nodecl_get_constant(visitor->nodecl_result)));
        }
        else
        {
            fprintf(stderr, " without any constant value");
        }
        if (nodecl_expr_is_type_dependent(visitor->nodecl_result))
        {
            fprintf(stderr, " [TYPE DEPENDENT]");
        }
        if (nodecl_expr_is_value_dependent(visitor->nodecl_result))
        {
            fprintf(stderr, " [VALUE DEPENDENT]");
        }
        fprintf(stderr, "\n");
    }
    return visitor->nodecl_result;
}

static nodecl_t instantiate_expr_walk_non_executable(nodecl_instantiate_expr_visitor_t* visitor, nodecl_t node)
{
    // Save the value of 'is_non_executable' of the last expression expression
    char was_non_executable = check_expr_flags.is_non_executable;

    // The current expression is non executable
    check_expr_flags.is_non_executable = 1;

    nodecl_t result = instantiate_expr_walk(visitor, node);

    // Restore the right value
    check_expr_flags.is_non_executable = was_non_executable;

    return result;
}

static void instantiate_expr_init_visitor(nodecl_instantiate_expr_visitor_t*, decl_context_t);

nodecl_t instantiate_expression(
        nodecl_t nodecl_expr, decl_context_t decl_context,
        instantiation_symbol_map_t* instantiation_symbol_map,
        int pack_index)
{
    nodecl_instantiate_expr_visitor_t v;
    memset(&v, 0, sizeof(v));
    v.pack_index = pack_index;
    v.instantiation_symbol_map = instantiation_symbol_map;

    char do_not_call_constexpr = check_expr_flags.do_not_call_constexpr;
    check_expr_flags.do_not_call_constexpr = 0;

    char must_be_constant = check_expr_flags.must_be_constant;
    check_expr_flags.must_be_constant = MUST_NOT_BE_CONSTANT;

    instantiate_expr_init_visitor(&v, decl_context);

    nodecl_t n = instantiate_expr_walk(&v, nodecl_expr);

    check_expr_flags.do_not_call_constexpr = do_not_call_constexpr;
    check_expr_flags.must_be_constant = must_be_constant;

    return n;
}

nodecl_t instantiate_expression_non_executable(
        nodecl_t nodecl_expr, decl_context_t decl_context,
        instantiation_symbol_map_t* instantiation_symbol_map,
        int pack_index)
{
    // Save the value of 'is_non_executable' of the last expression expression
    char was_non_executable = check_expr_flags.is_non_executable;

    // The current expression is non executable
    check_expr_flags.is_non_executable = 1;

    // Check_expression the current expression
    nodecl_t nodecl = instantiate_expression(
        nodecl_expr, decl_context,
        instantiation_symbol_map,
        pack_index);

    // Restore the right value
    check_expr_flags.is_non_executable = was_non_executable;

    return nodecl;
}

static void instantiate_expr_not_implemented_yet(nodecl_instantiate_expr_visitor_t* v UNUSED_PARAMETER,
        nodecl_t nodecl_expr)
{
    internal_error("Instantiation of expression of kind '%s' at '%s' "
            "no implemented yet\n",
            ast_print_node_type(nodecl_get_kind(nodecl_expr)),
            nodecl_locus_to_str(nodecl_expr));
}

static void instantiate_type(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    type_t* t = nodecl_get_type(node);
    t = update_type_for_instantiation(t,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);

    v->nodecl_result = nodecl_make_type(t, nodecl_get_locus(node));
}

static void instantiate_expr_literal(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t result = nodecl_generic_make(nodecl_get_kind(node), nodecl_get_locus(node));

    nodecl_set_type(result, nodecl_get_type(node));
    nodecl_set_constant(result, nodecl_get_constant(node));
    nodecl_set_text(result, nodecl_get_text(node));

    v->nodecl_result = result;
}

static void instantiate_compound_expression(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t new_context = instantiate_statement(node,
            v->decl_context,
            v->decl_context,
            v->instantiation_symbol_map);

    check_nodecl_gcc_parenthesized_expression(new_context, v->decl_context,
            nodecl_get_locus(node),
            &v->nodecl_result);
}

static void add_namespaces_rec(scope_entry_t* sym, nodecl_t *nodecl_extended_parts, const locus_t* locus)
{
    if (sym == NULL
            || sym->symbol_name == NULL)
        return;
    ERROR_CONDITION(sym->kind != SK_NAMESPACE, "Invalid symbol", 0);

    add_namespaces_rec(sym->decl_context.current_scope->related_entry, nodecl_extended_parts, locus);

    if (strcmp(sym->symbol_name, "(unnamed)") == 0)
    {
        // Do nothing
    }
    else
    {
        *nodecl_extended_parts = nodecl_append_to_list(*nodecl_extended_parts, 
                nodecl_make_cxx_dep_name_simple(sym->symbol_name, locus));
    }
}

static void add_classes_rec(type_t* class_type, nodecl_t* nodecl_extended_parts, decl_context_t decl_context,
        const locus_t* locus)
{
    scope_entry_t* class_sym = named_type_get_symbol(class_type);
    if (symbol_entity_specs_get_is_member(class_sym))
    {
        add_classes_rec(symbol_entity_specs_get_class_type(class_sym), nodecl_extended_parts, decl_context, locus);
    }

    nodecl_t nodecl_name = nodecl_make_cxx_dep_name_simple(class_sym->symbol_name, locus);
    if (is_template_specialized_type(class_sym->type_information))
    {
        nodecl_name = nodecl_make_cxx_dep_template_id(
                nodecl_name,
                /* template_tag */ "",
                update_template_argument_list(
                    decl_context,
                    template_specialized_type_get_template_arguments(
                        class_sym->type_information),
                    /* instantiation_symbol_map */ NULL,
                    locus,
                    /* pack_index */ -1),
                locus);
    }

    *nodecl_extended_parts = nodecl_append_to_list(*nodecl_extended_parts, nodecl_name);
}

// This function crafts a full NODECL_CXX_DEP_GLOBAL_NAME_NESTED for a dependent entry and its list of dependent parts
// We may need this when updating a dependent entity
//
// namespace A {
//   template < typename _T >
//   struct B
//   {
//       enum mcc_enum_anon_0
//       {
//         value = 3
//       };
//   };
//
//  template < typename _T, int _N = B<_T>::value >
//  struct C
//  {
//  };
// }
// template < typename _S >
// void f(C<_S> &c);           --->   void f(C<_S, A::B<_S>::value);
//
// We need to complete C<_S> to C<_S, A::B<_S>::value>. Note that the symbol of
// the dependent entity only contains "B<_S>::value" (without the namespace).
// And after updating it we may need extra namespace-or-class qualification.
// So we fully qualify the symbol lest it was a dependent entity again. Note
// that this could bring problems if the symbol is the operand of a reference (&)
// operator.
static nodecl_t complete_nodecl_name_of_dependent_entity(
        scope_entry_t* dependent_entry,
        nodecl_t list_of_dependent_parts,
        decl_context_t decl_context,
        instantiation_symbol_map_t* instantiation_symbol_map,
        char dependent_entry_already_updated,
        int pack_index,
        const locus_t* locus)
{
    // This may be left null if the dependent_entry is not a SK_DEPENDENT_ENTITY
    nodecl_t nodecl_already_updated_extended_parts = nodecl_null();
    nodecl_t nodecl_extended_parts = nodecl_null();

    if (dependent_entry->kind == SK_DEPENDENT_ENTITY)
    {
        dependent_typename_get_components(
                dependent_entry->type_information,
                &dependent_entry,
                &nodecl_already_updated_extended_parts);
    }

    char can_qualify = !(dependent_entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER
            || dependent_entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK
            || dependent_entry->kind == SK_TEMPLATE_TYPE_PARAMETER
            || dependent_entry->kind == SK_TEMPLATE_TYPE_PARAMETER_PACK
            || dependent_entry->kind == SK_TEMPLATE_TEMPLATE_PARAMETER
            || dependent_entry->kind == SK_TEMPLATE_TEMPLATE_PARAMETER_PACK);

    if (can_qualify)
    {
        add_namespaces_rec(dependent_entry->decl_context.namespace_scope->related_entry, &nodecl_extended_parts, locus);

        if (symbol_entity_specs_get_is_member(dependent_entry))
            add_classes_rec(symbol_entity_specs_get_class_type(dependent_entry), &nodecl_extended_parts, decl_context, locus);
    }

    // The dependent entry itself
    nodecl_t nodecl_name = nodecl_make_cxx_dep_name_simple(dependent_entry->symbol_name, locus);
    if (is_template_specialized_type(dependent_entry->type_information))
    {
        template_parameter_list_t* argument_list =
            template_specialized_type_get_template_arguments(dependent_entry->type_information);

        if (!dependent_entry_already_updated)
        {
            argument_list =
                update_template_argument_list(
                        decl_context,
                        template_specialized_type_get_template_arguments(dependent_entry->type_information),
                        instantiation_symbol_map,
                        locus,
                        pack_index);
        }

        nodecl_name = nodecl_make_cxx_dep_template_id(nodecl_name,
                /* template tag */ "",
                argument_list,
                locus);
    }

    nodecl_extended_parts = nodecl_append_to_list(nodecl_extended_parts, nodecl_name);

    if (!nodecl_is_null(nodecl_already_updated_extended_parts))
    {
        nodecl_extended_parts = nodecl_concat_lists(nodecl_extended_parts,
                nodecl_already_updated_extended_parts);
    }

    // Concat with the existing parts but make sure we update the template arguments as well
    int i;
    int num_rest_of_parts = 0;
    nodecl_t* rest_of_parts = nodecl_unpack_list(list_of_dependent_parts, &num_rest_of_parts);
    for (i = 0; i < num_rest_of_parts; i++)
    {
        nodecl_t copied_part = nodecl_shallow_copy(rest_of_parts[i]);

        if (nodecl_get_kind(copied_part) == NODECL_CXX_DEP_TEMPLATE_ID)
        {
            nodecl_set_template_parameters(
                    copied_part,
                    update_template_argument_list(
                        decl_context,
                        nodecl_get_template_parameters(copied_part),
                        instantiation_symbol_map,
                        locus,
                        pack_index));
        }

        nodecl_extended_parts = nodecl_append_to_list(nodecl_extended_parts, copied_part);
    }
    xfree(rest_of_parts);

    nodecl_t (*nodecl_make_cxx_dep_fun_name)(nodecl_t, const locus_t*) = NULL;
    if (can_qualify)
    {
        nodecl_make_cxx_dep_fun_name = nodecl_make_cxx_dep_global_name_nested;
    }
    else
    {
        // Do not globally qualify these cases
        nodecl_make_cxx_dep_fun_name = nodecl_make_cxx_dep_name_nested;
    }

    nodecl_t result =
        nodecl_make_cxx_dep_fun_name(nodecl_extended_parts, locus);

    return result;
}

static void instantiate_dependent_typename(
        nodecl_instantiate_expr_visitor_t* v,
        nodecl_t node
        )
{
    scope_entry_t* sym = nodecl_get_symbol(node);

    ERROR_CONDITION(sym->kind != SK_DEPENDENT_ENTITY, "Invalid symbol", 0);

    scope_entry_list_t *entry_list = query_dependent_entity_in_context(
            v->decl_context,
            sym,
            v->pack_index,
            NULL,
            v->instantiation_symbol_map,
            nodecl_get_locus(node));

    nodecl_t complete_nodecl_name = nodecl_null();
    scope_entry_t* dependent_entry = NULL;
    nodecl_t dependent_parts = nodecl_null();

    dependent_typename_get_components(sym->type_information, &dependent_entry, &dependent_parts);

    char dependent_entry_already_updated = 0;
    if (entry_list != NULL)
    {
        scope_entry_t* updated_symbol = entry_list_head(entry_list);
        if (updated_symbol->kind == SK_DEPENDENT_ENTITY)
        {
            nodecl_t nodecl_dummy = nodecl_null();
            dependent_typename_get_components(
                    updated_symbol->type_information,
                    &dependent_entry,
                    // We cannot update these, let
                    // complete_nodecl_name_of_dependent_entity do that for us
                    &nodecl_dummy);
            dependent_entry_already_updated = 1;
        }
        else if (updated_symbol->kind == SK_CLASS
                || updated_symbol->kind == SK_ENUM)
        {
            dependent_entry_already_updated = 1;
        }
    }

    nodecl_t list_of_dependent_parts = nodecl_get_child(dependent_parts, 0);
    complete_nodecl_name = complete_nodecl_name_of_dependent_entity(dependent_entry,
            list_of_dependent_parts,
            v->decl_context,
            v->instantiation_symbol_map,
            dependent_entry_already_updated,
            v->pack_index,
            nodecl_get_locus(node));

    cxx_compute_name_from_entry_list(
            complete_nodecl_name,
            entry_list,
            v->decl_context,
            NULL,
            &v->nodecl_result);

    entry_list_free(entry_list);
    nodecl_free(complete_nodecl_name);
}

static void instantiate_symbol(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    scope_entry_t* sym = nodecl_get_symbol(node);

    nodecl_t result = nodecl_null();

    if (sym->kind == SK_TEMPLATE_NONTYPE_PARAMETER)
    {
        scope_entry_t* argument = lookup_of_template_parameter(
                v->decl_context,
                symbol_entity_specs_get_template_parameter_nesting(sym),
                symbol_entity_specs_get_template_parameter_position(sym));

        if (argument == NULL)
        {
            result = nodecl_shallow_copy(node);
            nodecl_expr_set_is_value_dependent(result,
                    nodecl_expr_is_value_dependent(node));
            nodecl_expr_set_is_type_dependent(result,
                    nodecl_expr_is_type_dependent(node));
        }
        else if (argument->kind == SK_VARIABLE)
        {
            result = nodecl_shallow_copy(argument->value);
            if (nodecl_expr_is_type_dependent(result))
            {
                nodecl_expr_set_is_value_dependent(result, 1);
            }
        }
        else if (argument->kind == SK_TEMPLATE_NONTYPE_PARAMETER)
        {
            result = nodecl_make_symbol(argument, nodecl_get_locus(node));
            nodecl_set_type(result, nodecl_get_type(node));
            nodecl_expr_set_is_value_dependent(result,
                    nodecl_expr_is_value_dependent(node));
            nodecl_expr_set_is_type_dependent(result,
                    nodecl_expr_is_type_dependent(node));
        }
        else
        {
            result = nodecl_make_err_expr(nodecl_get_locus(node));
            nodecl_free(node);
        }
    }
    else if (sym->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK)
    {
        scope_entry_t* argument = lookup_of_template_parameter(
                v->decl_context,
                symbol_entity_specs_get_template_parameter_nesting(sym),
                symbol_entity_specs_get_template_parameter_position(sym));

        if (argument == NULL)
        {
            result = nodecl_shallow_copy(node);
        }
        else if (argument->kind == SK_VARIABLE_PACK)
        {
            if (v->pack_index < 0)
            {
                result = nodecl_shallow_copy(node);
            }
            else if (nodecl_is_list(argument->value)
                    && v->pack_index < nodecl_list_length(argument->value))
            {
                int num_items;
                nodecl_t* list = nodecl_unpack_list(argument->value, &num_items);
                result = nodecl_shallow_copy(list[v->pack_index]);
                xfree(list);
            }
            else
            {
                result = nodecl_make_err_expr(nodecl_get_locus(node));
                nodecl_free(node);
            }
        }
        else if (argument->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK)
        {
            result = nodecl_make_symbol(argument, nodecl_get_locus(node));
            nodecl_set_type(result, nodecl_get_type(node));
            nodecl_expr_set_is_value_dependent(result, nodecl_expr_is_value_dependent(node));
        }
        // Special case when we are using a nontype template pack inside a pack expansion.
        else if (argument->kind == SK_VARIABLE)
        {
            result = nodecl_shallow_copy(argument->value);
            if (nodecl_expr_is_type_dependent(result))
            {
                nodecl_expr_set_is_value_dependent(result, 1);
            }
        }
        else
        {
            result = nodecl_make_err_expr(nodecl_get_locus(node));
            nodecl_free(node);
        }
    }
    else if (sym->kind == SK_VARIABLE
            && sym->symbol_name != NULL
            && strcmp(sym->symbol_name, "nullptr") == 0)
    {
        // nullptr is special
        pointer_literal_type(nodecl_get_ast(node), v->decl_context, &result);
    }
    else if (sym->kind == SK_VARIABLE
            && sym->symbol_name != NULL
            && strcmp(sym->symbol_name, "this") == 0
            && instantiation_symbol_do_map(v->instantiation_symbol_map, sym) == NULL)
    {
        // 'this'
        resolve_symbol_this_nodecl(v->decl_context, nodecl_get_locus(node), &result);
    }
    else if (sym->kind == SK_DEPENDENT_ENTITY)
    {
        internal_error("This kind of symbol should not be wrapped in a NODECL_SYMBOL\n", 0);
    }
    else if (sym->kind == SK_VARIABLE_PACK)
    {
        scope_entry_t* mapped_symbol = instantiation_symbol_try_to_map(v->instantiation_symbol_map,
                nodecl_get_symbol(node));

        ERROR_CONDITION(mapped_symbol->kind != SK_VARIABLE_PACK, "This should be a variable pack", 0);

        if (v->pack_index < 0)
        {
            nodecl_t nodecl_name = nodecl_make_cxx_dep_name_simple(mapped_symbol->symbol_name, nodecl_get_locus(node));

            scope_entry_list_t* entry_list = entry_list_new(mapped_symbol);
            cxx_compute_name_from_entry_list(nodecl_name, entry_list, v->decl_context, NULL, &result);
            entry_list_free(entry_list);
        }
        else if (nodecl_is_list(mapped_symbol->value)
                && v->pack_index < nodecl_list_length(mapped_symbol->value))
        {
            int num_items;
            nodecl_t* list = nodecl_unpack_list(mapped_symbol->value, &num_items);
            nodecl_t sub_symbol = list[v->pack_index];
            result = nodecl_shallow_copy(sub_symbol);
            xfree(list);
        }
        else
        {
            result = nodecl_make_err_expr(nodecl_get_locus(node));
            nodecl_free(node);
        }
    }
    else
    {
        char was_unresolved = is_unresolved_overloaded_type(nodecl_get_type(node));
        scope_entry_t* mapped_symbol = instantiation_symbol_try_to_map(v->instantiation_symbol_map, nodecl_get_symbol(node));

        // FIXME - Can this name be other than an unqualified thing?
        nodecl_t nodecl_name = nodecl_make_cxx_dep_name_simple(mapped_symbol->symbol_name, nodecl_get_locus(node));

        scope_entry_list_t* entry_list = entry_list_new(mapped_symbol);

        cxx_compute_name_from_entry_list(nodecl_name, entry_list, v->decl_context, NULL, &result);

        entry_list_free(entry_list);
        nodecl_free(nodecl_name);

        if (is_unresolved_overloaded_type(nodecl_get_type(result))
                && !was_unresolved)
        {
            // If the original name was not unresolved then attempt to simplify it here
            scope_entry_t* function = unresolved_overloaded_type_simplify(
                    nodecl_get_type(result),
                    v->decl_context,
                    nodecl_get_locus(result));
            if (function != NULL)
            {
                update_simplified_unresolved_overloaded_type(
                        function,
                        v->decl_context,
                        nodecl_get_locus(result),
                        &result);
            }
        }
    }

    v->nodecl_result = result;
}

static void instantiate_cxx_lambda(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    scope_entry_t* lambda_symbol = nodecl_get_symbol(node);

    scope_entry_t* instantiated_lambda_symbol = xcalloc(1, sizeof(*instantiated_lambda_symbol));
    instantiated_lambda_symbol->decl_context = v->decl_context;
    instantiated_lambda_symbol->kind = lambda_symbol->kind;
    instantiated_lambda_symbol->locus = lambda_symbol->locus;
    instantiated_lambda_symbol->type_information = update_type_for_instantiation(
            lambda_symbol->type_information,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);
    // FIXME - Exceptions
    symbol_entity_specs_copy_from(instantiated_lambda_symbol, lambda_symbol);

    int i, n = symbol_entity_specs_get_num_related_symbols(instantiated_lambda_symbol);
    for (i = 0; i < n; i++)
    {
        scope_entry_t* orig_param = symbol_entity_specs_get_related_symbols_num(instantiated_lambda_symbol, i);

        scope_entry_t* param = xcalloc(1, sizeof(*param));
        param->defined = 1;
        param->kind = SK_VARIABLE;
        param->locus = nodecl_get_locus(node);
        param->type_information = update_type_for_instantiation(
            orig_param->type_information,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);
        symbol_entity_specs_copy_from(param, orig_param);

        symbol_entity_specs_set_related_symbols_num(instantiated_lambda_symbol, i, param);
    }

    nodecl_t captures = nodecl_get_child(node, 0);
    nodecl_t instantiated_captures = nodecl_shallow_copy(captures);

    nodecl_t* list = nodecl_unpack_list(instantiated_captures, &n);
    for (i = 0; i < n; i++)
    {
        scope_entry_t* mapped_symbol =
            instantiation_symbol_try_to_map(v->instantiation_symbol_map, nodecl_get_symbol(list[i]));

        nodecl_set_symbol(list[i], mapped_symbol);
    }
    xfree(list);

    implement_lambda_expression(
            v->decl_context,
            instantiated_captures,
            lambda_symbol,
            lambda_symbol->type_information,
            nodecl_get_locus(node),
            &v->nodecl_result);

    nodecl_free(instantiated_captures);

    n = symbol_entity_specs_get_num_related_symbols(instantiated_lambda_symbol);
    for (i = 0; i < n; i++)
    {
        scope_entry_t* param = symbol_entity_specs_get_related_symbols_num(instantiated_lambda_symbol, i);

        symbol_entity_specs_free(param);
        xfree(param);
    }

    symbol_entity_specs_free(instantiated_lambda_symbol);
    xfree(instantiated_lambda_symbol);
}

static void instantiate_class_member_access(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_accessed = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_member_literal = nodecl_get_child(node, 2);

    ERROR_CONDITION(nodecl_is_null(nodecl_member_literal), "Cannot instantiate this tree", 0);

    check_nodecl_member_access(
            nodecl_accessed,
            nodecl_member_literal,
            v->decl_context,
            /* is_arrow */ 0,
            /* has_template_tag */ 0,
            nodecl_get_locus(node),
            &v->nodecl_result);
}

static nodecl_t update_dep_template_id(
        nodecl_t node,
        decl_context_t new_decl_context,
        instantiation_symbol_map_t* instantiation_symbol_map,
        int pack_index)
{
    template_parameter_list_t* template_args =
        nodecl_get_template_parameters(node);
    template_parameter_list_t* update_template_args =
        update_template_argument_list(new_decl_context,
                template_args,
                instantiation_symbol_map,
                nodecl_get_locus(node),
                pack_index);

    nodecl_t nodecl_name = nodecl_make_cxx_dep_template_id(
            nodecl_shallow_copy(nodecl_get_child(node, 0)), // FIXME - We may have to update this as well!
            nodecl_get_text(node),
            update_template_args,
            nodecl_get_locus(node));

    return nodecl_name;
}

static nodecl_t update_common_dep_name_nested(
        nodecl_t node,
        decl_context_t new_decl_context,
        instantiation_symbol_map_t* instantiation_symbol_map,
        int pack_index,
        nodecl_t (*func)(nodecl_t, const locus_t*))
{
    nodecl_t nodecl_result_list = nodecl_null();
    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_get_child(node, 0), &num_items);

    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t expr = nodecl_shallow_copy(list[i]);
        if (nodecl_get_kind(expr) == NODECL_CXX_DEP_TEMPLATE_ID)
        {
            template_parameter_list_t* template_args =
                nodecl_get_template_parameters(expr);
            template_parameter_list_t* updated_template_args =
                update_template_argument_list(new_decl_context,
                        template_args,
                        instantiation_symbol_map,
                        nodecl_get_locus(expr),
                        pack_index);

            nodecl_set_template_parameters(expr, updated_template_args);
        }
        else if (nodecl_get_kind(expr) == NODECL_CXX_DEP_NAME_CONVERSION)
        {
            internal_error("Not yet implemented", 0);
        }

        nodecl_result_list = nodecl_append_to_list(nodecl_result_list, expr);
    }
    xfree(list);

    nodecl_t nodecl_name = (*func)(nodecl_result_list, nodecl_get_locus(node));

    return nodecl_name;
}

nodecl_t update_cxx_dep_qualified_name(nodecl_t cxx_dep_name,
        decl_context_t new_decl_context,
        instantiation_symbol_map_t* instantiation_symbol_map,
        int pack_index)
{
    if (nodecl_get_kind(cxx_dep_name) == NODECL_CXX_DEP_NAME_SIMPLE)
    {
        return nodecl_shallow_copy(cxx_dep_name);
    }
    else if (nodecl_get_kind(cxx_dep_name) == NODECL_CXX_DEP_TEMPLATE_ID)
    {
        return update_dep_template_id(cxx_dep_name, new_decl_context,
                instantiation_symbol_map, pack_index);
    }
    else if (nodecl_get_kind(cxx_dep_name) == NODECL_CXX_DEP_NAME_NESTED)
    {
        return update_common_dep_name_nested( cxx_dep_name, new_decl_context,
                instantiation_symbol_map, pack_index,
                nodecl_make_cxx_dep_name_nested);
    }
    else if (nodecl_get_kind(cxx_dep_name) == NODECL_CXX_DEP_GLOBAL_NAME_NESTED)
    {
        return update_common_dep_name_nested( cxx_dep_name, new_decl_context,
                instantiation_symbol_map, pack_index,
                nodecl_make_cxx_dep_global_name_nested);
    }
    else if (nodecl_get_kind(cxx_dep_name) == NODECL_CXX_DEP_NAME_CONVERSION)
    {
        internal_error("Not yet implemented", 0);
    }
    else 
    {
        internal_error("Unexpected cxx_dep_name '%s'\n", ast_print_node_type(nodecl_get_kind(cxx_dep_name)));
    }
}

static nodecl_t instantiate_id_expr_of_class_member_access(
        nodecl_instantiate_expr_visitor_t* v,
        nodecl_t node)
{
    return update_cxx_dep_qualified_name(node,
            v->decl_context,
            v->instantiation_symbol_map,
            v->pack_index);
}

static void instantiate_cxx_class_member_access(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_accessed = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_member = instantiate_id_expr_of_class_member_access(v, nodecl_get_child(node, 1));

    check_nodecl_member_access(
            nodecl_accessed,
            nodecl_member,
            v->decl_context,
            /* is_arrow */ 0,
            nodecl_get_text(node) != NULL,
            nodecl_get_locus(node),
            &v->nodecl_result);
}

static void instantiate_cxx_arrow(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_accessed = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_member = instantiate_id_expr_of_class_member_access(v, nodecl_get_child(node, 1));

    check_nodecl_member_access(
            nodecl_accessed,
            nodecl_member,
            v->decl_context,
            /* is_arrow */ 1,
            nodecl_get_text(node) != NULL,
            nodecl_get_locus(node),
            &v->nodecl_result);
}

static void instantiate_array_subscript(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_subscripted = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_subscript = instantiate_expr_walk(v, nodecl_get_child(node, 1));

    check_nodecl_array_subscript_expression_cxx(
            nodecl_subscripted,
            nodecl_subscript,
            v->decl_context,
            &v->nodecl_result);
}

static void instantiate_cxx_array_section_size(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_postfix = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_start = instantiate_expr_walk(v, nodecl_get_child(node, 1));
    nodecl_t nodecl_num_items = instantiate_expr_walk(v, nodecl_get_child(node, 2));
    nodecl_t nodecl_stride = instantiate_expr_walk(v, nodecl_get_child(node, 3));

    check_nodecl_array_section_expression(nodecl_postfix,
            nodecl_start, nodecl_num_items, nodecl_stride,
            v->decl_context, /* is_array_section_size */ 1, nodecl_get_locus(node), &v->nodecl_result);
}

static void instantiate_cxx_array_section_range(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_postfix = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_lower   = instantiate_expr_walk(v, nodecl_get_child(node, 1));
    nodecl_t nodecl_upper   = instantiate_expr_walk(v, nodecl_get_child(node, 2));
    nodecl_t nodecl_stride  = instantiate_expr_walk(v, nodecl_get_child(node, 3));

    check_nodecl_array_section_expression(nodecl_postfix,
            nodecl_lower, nodecl_upper, nodecl_stride,
            v->decl_context, /* is_array_section_size */ 0, nodecl_get_locus(node), &v->nodecl_result);
}

static void instantiate_throw(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_thrown = nodecl_get_child(node, 0);
    nodecl_thrown = instantiate_expr_walk(v, nodecl_thrown);

    check_throw_expression_nodecl(nodecl_thrown, nodecl_get_locus(node), &v->nodecl_result);
}

static void instantiate_binary_op(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_lhs = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_rhs = instantiate_expr_walk(v, nodecl_get_child(node, 1));

    nodecl_t result = nodecl_null();

    if (nodecl_is_err_expr(nodecl_lhs)
            || nodecl_is_err_expr(nodecl_rhs))
    {
        result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
    }
    else
    {
        check_binary_expression_(nodecl_get_kind(node), 
                &nodecl_lhs,
                &nodecl_rhs,
                v->decl_context,
                nodecl_get_locus(node),
                &result);
    }

    v->nodecl_result = result;
}

static void instantiate_logical_and(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_lhs = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    char do_not_call_constexpr = check_expr_flags.do_not_call_constexpr;
    check_expr_flags.do_not_call_constexpr = !nodecl_is_constant(nodecl_lhs)
            || const_value_is_zero(nodecl_get_constant(nodecl_lhs));

    nodecl_t nodecl_rhs = instantiate_expr_walk(v, nodecl_get_child(node, 1));

    check_expr_flags.do_not_call_constexpr = do_not_call_constexpr;

    if (nodecl_is_constant(nodecl_lhs)
            && const_value_is_zero(nodecl_get_constant(nodecl_lhs))
            && !nodecl_is_constant(nodecl_rhs))
    {
        // Propagate the constant to the RHS so the whole expression evaluates
        // as a constant
        nodecl_set_constant(nodecl_rhs, nodecl_get_constant(nodecl_lhs));
    }

    nodecl_t result = nodecl_null();

    if (nodecl_is_err_expr(nodecl_lhs)
            || nodecl_is_err_expr(nodecl_rhs))
    {
        result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
    }
    else
    {
        check_binary_expression_(nodecl_get_kind(node),
                &nodecl_lhs,
                &nodecl_rhs,
                v->decl_context,
                nodecl_get_locus(node),
                &result);
    }

    v->nodecl_result = result;
}

static void instantiate_logical_or(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_lhs = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    char do_not_call_constexpr = check_expr_flags.do_not_call_constexpr;
    check_expr_flags.do_not_call_constexpr = !nodecl_is_constant(nodecl_lhs)
            || const_value_is_nonzero(nodecl_get_constant(nodecl_lhs));

    nodecl_t nodecl_rhs = instantiate_expr_walk(v, nodecl_get_child(node, 1));

    check_expr_flags.do_not_call_constexpr = do_not_call_constexpr;

    if (nodecl_is_constant(nodecl_lhs) &&
            const_value_is_nonzero(nodecl_get_constant(nodecl_lhs))
            && !nodecl_is_constant(nodecl_rhs))
    {
        // Propagate the constant to the rhs so the whole expression evaluates
        // as a constant
        nodecl_set_constant(nodecl_rhs, nodecl_get_constant(nodecl_lhs));
    }

    nodecl_t result = nodecl_null();

    if (nodecl_is_err_expr(nodecl_lhs)
            || nodecl_is_err_expr(nodecl_rhs))
    {
        result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(nodecl_lhs);
        nodecl_free(nodecl_rhs);
    }
    else
    {
        check_binary_expression_(nodecl_get_kind(node),
                &nodecl_lhs,
                &nodecl_rhs,
                v->decl_context,
                nodecl_get_locus(node),
                &result);
    }

    v->nodecl_result = result;
}

static void instantiate_unary_op(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_op = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    nodecl_t result = nodecl_null();

    if (nodecl_is_err_expr(nodecl_op))
    {
        result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(nodecl_op);
    }
    else
    {
        check_unary_expression_(nodecl_get_kind(node),
                &nodecl_op,
                v->decl_context,
                nodecl_get_locus(nodecl_op),
                &result);
    }

    v->nodecl_result = result;
}

static void instantiate_structured_value(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    // If this structured value represents a struct constant, handle it as if
    // it were a literal
    if (nodecl_is_constant(node))
    {
        v->nodecl_result = nodecl_shallow_copy(node);
        return;
    }

    type_t* t = nodecl_get_type(node);

    t = update_type_for_instantiation(t,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_get_child(node, 0), &num_items);

    nodecl_t nodecl_new_list = nodecl_null();

    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t nodecl_new_item = nodecl_new_item = instantiate_expr_walk(v, list[i]);

        if (nodecl_is_err_expr(nodecl_new_item))
        {
            v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
            nodecl_free(nodecl_new_item);
            return;
        }

        nodecl_new_list = nodecl_append_to_list(nodecl_new_list, nodecl_new_item);
    }

    v->nodecl_result =
        nodecl_make_structured_value(nodecl_new_list,
                nodecl_shallow_copy(nodecl_get_child(node, 1)),
                t,
                nodecl_get_locus(node));

    //FIXME: We should check this new structured value
}

static void instantiate_field_designator(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_name = nodecl_shallow_copy(nodecl_get_child(node, 0));
    nodecl_t nodecl_next = instantiate_expr_walk(v, nodecl_get_child(node, 1));

    v->nodecl_result =
        nodecl_make_field_designator(
                nodecl_name,
                nodecl_next,
                nodecl_get_type(node),
                nodecl_get_locus(node));

    nodecl_set_constant(v->nodecl_result,
            nodecl_get_constant(node));
}

static void instantiate_index_designator(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_index = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_next = instantiate_expr_walk(v, nodecl_get_child(node, 1));

    v->nodecl_result =
        nodecl_make_index_designator(
                nodecl_index,
                nodecl_next,
                nodecl_get_type(node),
                nodecl_get_locus(node));

    nodecl_set_constant(v->nodecl_result,
            nodecl_get_constant(node));
}

static void instantiate_reference(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_op = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    if (nodecl_is_err_expr(nodecl_op))
    {
        v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(nodecl_op);
        return;
    }
    else if (nodecl_get_kind(nodecl_op) == NODECL_SYMBOL)
    {
        scope_entry_t* sym = nodecl_get_symbol(nodecl_op);

        if (symbol_entity_specs_get_is_member(sym)
                && !symbol_entity_specs_get_is_static(sym)
                && (sym->kind == SK_VARIABLE
                    || sym->kind == SK_FUNCTION))
        {
            if (sym->kind == SK_VARIABLE)
            {
                v->nodecl_result = nodecl_make_pointer_to_member(sym, 
                        get_pointer_to_member_type(sym->type_information,
                            symbol_entity_specs_get_class_type(sym)),
                        nodecl_get_locus(node));
            }
            else // SK_FUNCTION
            {
                v->nodecl_result = nodecl_op;
            }
            return;
        }
    }

    check_unary_expression_(nodecl_get_kind(node),
            &nodecl_op,
            v->decl_context,
            nodecl_get_locus(nodecl_op),
            &v->nodecl_result);
}

static void instantiate_function_call(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    // This does not have to be instantiated
    nodecl_t nodecl_called = nodecl_shallow_copy(nodecl_get_child(node, 0));
    nodecl_t nodecl_argument_list = nodecl_get_child(node, 1);

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_argument_list, &num_items);

    nodecl_t new_list = nodecl_null();
    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t arg = list[i];

        // Advance default arguments
        if (nodecl_get_kind(arg) == NODECL_DEFAULT_ARGUMENT)
            arg = nodecl_get_child(arg, 0);

        nodecl_t current_arg = instantiate_expr_walk(v, arg);

        if (nodecl_is_err_expr(current_arg))
        {
            v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
            nodecl_free(new_list);
            nodecl_free(current_arg);
            return;
        }

        new_list = nodecl_append_to_list(
                new_list,
                current_arg);
    }

    nodecl_t alternate_name = nodecl_shallow_copy(nodecl_get_child(node, 2));
    nodecl_t function_form = nodecl_shallow_copy(nodecl_get_child(node, 3));

    v->nodecl_result = cxx_nodecl_make_function_call(
            nodecl_called,
            alternate_name,
            new_list,
            function_form,
            nodecl_get_type(node),
            v->decl_context,
            nodecl_get_locus(node));
}

static void instantiate_cxx_dep_function_call(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_called = nodecl_null();
    nodecl_t orig_called = nodecl_get_child(node, 0);

    if (nodecl_get_kind(orig_called) == NODECL_CXX_DEP_NAME_SIMPLE
            && nodecl_get_type(orig_called) == NULL)
    {
        // Preserve koenig lookup calls
        nodecl_called = nodecl_shallow_copy(orig_called);
    }
    else
    {
        nodecl_called = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    }

    if (nodecl_is_err_expr(nodecl_called))
    {
        v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(nodecl_called);
        return;
    }

    nodecl_t nodecl_argument_list = nodecl_get_child(node, 1);

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_argument_list, &num_items);

    nodecl_t new_list = nodecl_null();
    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t current_arg =
                instantiate_expr_walk(v,
                        nodecl_shallow_copy(list[i]));

        // This plays the role of the empty list
        if (nodecl_is_null(current_arg))
            continue;

        if (nodecl_is_err_expr(current_arg))
        {
            v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
            nodecl_free(new_list);
            nodecl_free(current_arg);
            xfree(list);
            return;
        }

        if (nodecl_is_list(current_arg))
        {
            // Instantiation of pack expansions will generate a list here, concat
            new_list = nodecl_concat_lists(new_list,
                    current_arg);
        }
        else
        {
            new_list = nodecl_append_to_list(
                    new_list,
                    current_arg);
        }
    }
    xfree(list);

    check_nodecl_function_call(nodecl_called,
            new_list,
            v->decl_context,
            &v->nodecl_result);

    // Remove the called name (it should not be necessary)
    nodecl_set_child(v->nodecl_result, 2, nodecl_null());
}

static void instantiate_comma_op(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_lhs = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    nodecl_t nodecl_rhs = instantiate_expr_walk(v, nodecl_get_child(node, 1));

    check_nodecl_comma_operand(nodecl_lhs,
            nodecl_rhs,
            v->decl_context,
            &v->nodecl_result,
            nodecl_get_locus(node));
}

static void instantiate_predecrement(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_op = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    check_nodecl_predecrement(nodecl_op, v->decl_context, &v->nodecl_result);
}

static void instantiate_preincrement(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_op = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    check_nodecl_preincrement(nodecl_op, v->decl_context, &v->nodecl_result);
}

static void instantiate_postdecrement(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_op = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    check_nodecl_postdecrement(nodecl_op, v->decl_context, &v->nodecl_result);
}

static void instantiate_postincrement(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_op = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    check_nodecl_postincrement(nodecl_op, v->decl_context, &v->nodecl_result);
}

static void instantiate_gxx_trait(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t lhs_type = nodecl_get_child(node, 0);
    nodecl_t lhs_type_inst = instantiate_expr_walk(v, lhs_type);

    if (nodecl_is_err_expr(lhs_type_inst))
    {
        v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(lhs_type_inst);
        return;
    }
    type_t* instantiated_type_lhs = nodecl_get_type(lhs_type_inst);

    nodecl_t rhs_type = nodecl_get_child(node, 1);
    nodecl_t rhs_type_inst_opt = nodecl_null();
    if (!nodecl_is_null(rhs_type))
    {
        rhs_type_inst_opt = instantiate_expr_walk(v, rhs_type);
        if (nodecl_is_err_expr(rhs_type_inst_opt))
        {
            v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
            nodecl_free(lhs_type_inst);
            nodecl_free(rhs_type_inst_opt);
            return;
        }
    }

    type_t* instantiated_type_rhs_opt =
        (nodecl_is_null(rhs_type_inst_opt)) ? NULL : nodecl_get_type(rhs_type_inst_opt);

    common_check_gxx_type_traits(instantiated_type_lhs,
            instantiated_type_rhs_opt,
            nodecl_get_type(node),
            nodecl_get_text(node),
            nodecl_retrieve_context(node),
            nodecl_get_locus(node),
            &(v->nodecl_result));
}

static void instantiate_dep_sizeof_expr(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t dep_expr = nodecl_get_child(node, 0);

    nodecl_t expr = instantiate_expr_walk(v, dep_expr);

    nodecl_t result = nodecl_null();

    if (nodecl_is_err_expr(expr))
    {
        result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(expr);
    }
    else
    {
        check_nodecl_sizeof_expr(
                expr,
                v->decl_context, 
                &result);
    }

    v->nodecl_result = result;
}

static void instantiate_dep_sizeof_pack(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    scope_entry_t* entry = nodecl_get_symbol(nodecl_get_child(node, 0));
    if (entry != NULL
            && (entry->kind == SK_TEMPLATE_TYPE_PARAMETER_PACK
                || entry->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK
                || entry->kind == SK_TEMPLATE_TEMPLATE_PARAMETER_PACK))
    {
        entry = lookup_of_template_parameter(
                v->decl_context,
                symbol_entity_specs_get_template_parameter_nesting(entry),
                symbol_entity_specs_get_template_parameter_position(entry));

        check_symbol_sizeof_pack(entry, nodecl_get_locus(node), &v->nodecl_result);
    }
    else
    {
        v->nodecl_result = nodecl_make_err_expr(nodecl_get_locus(node));
    }
}

static void instantiate_dep_alignof_expr(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t dep_expr = nodecl_get_child(node, 0);

    nodecl_t expr = instantiate_expr_walk_non_executable(v, dep_expr);

    nodecl_t result = nodecl_null();

    if (nodecl_is_err_expr(expr))
    {
        result = nodecl_make_err_expr(nodecl_get_locus(node));
        nodecl_free(expr);
    }
    else
    {
        check_nodecl_gcc_alignof_expr(expr,
                v->decl_context, 
                nodecl_get_locus(node), 
                &result);
    }

    v->nodecl_result = result;
}

static void instantiate_nondep_sizeof(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_type = nodecl_get_child(node, 0);

    type_t* t = nodecl_get_type(nodecl_type);

    t = update_type_for_instantiation(t, 
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);

    nodecl_t result = nodecl_null();

    check_sizeof_type(t, 
            nodecl_null(),
            v->decl_context, 
            nodecl_get_locus(node),
            &result);

    v->nodecl_result = result;
}

static void instantiate_nondep_alignof(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_type = nodecl_get_child(node, 0);

    type_t* t = nodecl_get_type(nodecl_type);

    t = update_type_for_instantiation(t,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);

    nodecl_t result = nodecl_null();

    check_gcc_alignof_type(t,
            nodecl_null(),
            v->decl_context,
            nodecl_get_locus(node),
            &result);

    v->nodecl_result = result;
}

static void instantiate_offsetof(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_accessed_type = nodecl_get_child(node, 0);

    type_t* accessed_type = nodecl_get_type(nodecl_accessed_type);
    accessed_type = update_type_for_instantiation(accessed_type,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);

    nodecl_t nodecl_designator = nodecl_get_child(node, 1);

    check_gcc_offset_designation(nodecl_designator,
            v->decl_context,
            accessed_type,
            &v->nodecl_result,
            nodecl_get_locus(node));
}

static void instantiate_noexcept(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t dep_expr = nodecl_get_child(node, 0);
    nodecl_t expr = instantiate_expr_walk_non_executable(v, dep_expr);

    check_nodecl_noexcept(expr, &v->nodecl_result);
}

static void instantiate_explicit_type_cast(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    type_t * t = nodecl_get_type(node);
    t = update_type_for_instantiation(t,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);

    nodecl_t explicit_initializer = nodecl_get_child(node, 0);
    nodecl_t instantiated_explicit_initializer = instantiate_expr_walk(v, explicit_initializer);

    check_nodecl_explicit_type_conversion(t,
            instantiated_explicit_initializer,
            v->decl_context,
            &v->nodecl_result,
            nodecl_get_locus(node));
}

static void instantiate_dep_name_simple(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    if (nodecl_get_symbol(node) != NULL
            && nodecl_get_symbol(node)->kind == SK_DEPENDENT_ENTITY)
    {
        instantiate_dependent_typename(v, node);
        return;
    }

    nodecl_t nodecl_name =
        nodecl_make_cxx_dep_name_simple(nodecl_get_text(node),
        nodecl_get_locus(node));

    field_path_t field_path;
    field_path_init(&field_path);

    scope_entry_list_t* result_list = query_nodecl_name_flags(
            v->decl_context,
            nodecl_name,
            &field_path,
            DF_DEPENDENT_TYPENAME |
            DF_IGNORE_FRIEND_DECL |
            DF_DO_NOT_CREATE_UNQUALIFIED_DEPENDENT_ENTITY);
    cxx_compute_name_from_entry_list(nodecl_name, result_list, v->decl_context, &field_path, &v->nodecl_result);

    entry_list_free(result_list);
    nodecl_free(nodecl_name);
}


static void instantiate_dep_template_id(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    if (nodecl_get_symbol(node) != NULL
            && nodecl_get_symbol(node)->kind == SK_DEPENDENT_ENTITY)
    {
        instantiate_dependent_typename(v, node);
        return;
    }

    nodecl_t nodecl_name = update_dep_template_id(node,
            v->decl_context,
            v->instantiation_symbol_map,
            v->pack_index);

    scope_entry_list_t* result_list = query_nodecl_name_flags(
            v->decl_context,
            nodecl_name,
            NULL,
            DF_DEPENDENT_TYPENAME |
            DF_IGNORE_FRIEND_DECL |
            DF_DO_NOT_CREATE_UNQUALIFIED_DEPENDENT_ENTITY);
    cxx_compute_name_from_entry_list(nodecl_name, result_list, v->decl_context, NULL, &v->nodecl_result);

    entry_list_free(result_list);
    nodecl_free(nodecl_name);
}

static void instantiate_common_dep_name_nested(
        nodecl_instantiate_expr_visitor_t* v,
        nodecl_t node,
        nodecl_t (*func)(nodecl_t, const locus_t*))
{
    if (nodecl_get_symbol(node) != NULL
            && nodecl_get_symbol(node)->kind == SK_DEPENDENT_ENTITY)
    {
        instantiate_dependent_typename(v, node);
        return;
    }

    nodecl_t nodecl_name = update_common_dep_name_nested(node, v->decl_context,
            v->instantiation_symbol_map, v->pack_index, func);

    field_path_t field_path;
    field_path_init(&field_path);

    scope_entry_list_t* result_list = query_nodecl_name_flags(
            v->decl_context,
            nodecl_name,
            &field_path,
            DF_DEPENDENT_TYPENAME |
            DF_IGNORE_FRIEND_DECL |
            DF_DO_NOT_CREATE_UNQUALIFIED_DEPENDENT_ENTITY);
    cxx_compute_name_from_entry_list(nodecl_name, result_list, v->decl_context, &field_path, &v->nodecl_result);

    entry_list_free(result_list);
    nodecl_free(nodecl_name);
}

static void instantiate_dep_global_name_nested(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    instantiate_common_dep_name_nested(v, node, &nodecl_make_cxx_dep_global_name_nested);
}

static void instantiate_dep_name_nested(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    instantiate_common_dep_name_nested(v, node, &nodecl_make_cxx_dep_name_nested);
}

static void instantiate_parenthesized_initializer(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_result_list = nodecl_null();

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_get_child(node, 0), &num_items);

    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t expr = instantiate_expr_walk(v, list[i]);

        if (nodecl_is_err_expr(expr))
        {
            v->nodecl_result = expr;
            return;
        }

        if (!nodecl_is_list(expr))
        {
            nodecl_result_list = nodecl_append_to_list(nodecl_result_list, expr);
        }
        else
        {
            nodecl_result_list = nodecl_concat_lists(nodecl_result_list, expr);
        }
    }

    xfree(list);

    v->nodecl_result = nodecl_make_cxx_parenthesized_initializer(nodecl_result_list, nodecl_get_locus(node));
}

static void instantiate_initializer(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t expr = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    if (nodecl_is_err_expr(expr))
    {
        v->nodecl_result = expr;
    }
    else
    {
        v->nodecl_result = nodecl_make_cxx_initializer(expr, nodecl_get_type(expr), nodecl_get_locus(node));
    }
}

static void instantiate_equal_initializer(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t expr = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    if (nodecl_is_err_expr(expr))
    {
        v->nodecl_result = expr;
    }
    else
    {
        v->nodecl_result = nodecl_make_cxx_equal_initializer(expr, nodecl_get_type(expr), nodecl_get_locus(node));
    }
}

static void instantiate_braced_initializer(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_result_list = nodecl_null();

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_get_child(node, 0), &num_items);

    type_t** types = NULL;
    int num_types = 0;

    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t expr = instantiate_expr_walk(v, list[i]);

        if (nodecl_is_err_expr(expr))
        {
            v->nodecl_result = expr;
            return;
        }

        nodecl_result_list = nodecl_append_to_list(nodecl_result_list, expr);

        P_LIST_ADD(types, num_types, nodecl_get_type(expr));
    }

    xfree(list);

    v->nodecl_result = nodecl_make_cxx_braced_initializer(nodecl_result_list,
            get_braced_list_type(num_types, types),
            nodecl_get_locus(node));
}

static void instantiate_conversion(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_expr = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    if (nodecl_expr_is_type_dependent(nodecl_expr))
    {
        v->nodecl_result = nodecl_expr;
    }
    else
    {
        v->nodecl_result = cxx_nodecl_make_conversion(nodecl_expr, 
                nodecl_get_type(node), 
                nodecl_get_locus(node));
    }
}

static void instantiate_parenthesized_expression(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_expr = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    v->nodecl_result = cxx_nodecl_wrap_in_parentheses(nodecl_expr);
}

static void instantiate_cast(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_casted_expr = instantiate_expr_walk(v, nodecl_get_child(node, 0));

    type_t* declarator_type = update_type_for_instantiation(nodecl_get_type(node),
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            v->pack_index);

    const char* cast_kind = nodecl_get_text(node);

    nodecl_t result = nodecl_null();

    check_nodecl_cast_expr(nodecl_casted_expr, 
            v->decl_context, 
            declarator_type, cast_kind,
            nodecl_get_locus(node),
            &result);

    v->nodecl_result = result;
}

static void instantiate_conditional_expression(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_cond = instantiate_expr_walk(v, nodecl_get_child(node, 0));
    if (nodecl_is_err_expr(nodecl_cond))
    {
        v->nodecl_result = nodecl_cond;
        return;
    }

    char do_not_call_constexpr = check_expr_flags.do_not_call_constexpr;
    check_expr_flags.do_not_call_constexpr = !nodecl_is_constant(nodecl_cond)
        || const_value_is_zero(nodecl_get_constant(nodecl_cond));

    nodecl_t nodecl_true = instantiate_expr_walk(v, nodecl_get_child(node, 1));
    if (nodecl_is_err_expr(nodecl_true))
    {
        v->nodecl_result = nodecl_true;
        return;
    }

    check_expr_flags.do_not_call_constexpr = !nodecl_is_constant(nodecl_cond)
        || const_value_is_nonzero(nodecl_get_constant(nodecl_cond));

    nodecl_t nodecl_false = instantiate_expr_walk(v, nodecl_get_child(node, 2));
    if (nodecl_is_err_expr(nodecl_false))
    {
        v->nodecl_result = nodecl_false;
        return;
    }

    check_expr_flags.do_not_call_constexpr = do_not_call_constexpr;

    check_conditional_expression_impl_nodecl(nodecl_cond, nodecl_true, nodecl_false, v->decl_context, &v->nodecl_result);
}

static void instantiate_cxx_value_pack(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t expansion = nodecl_get_child(node, 0);
    int len = get_length_of_pack_expansion_from_expression(expansion, v->decl_context, nodecl_get_locus(node));

    if (len < 0)
    {
        v->nodecl_result = nodecl_shallow_copy(node);
        return;
    }

    nodecl_t nodecl_result = nodecl_null();

    int old_pack_index = v->pack_index;

    char is_value_dependent = 0;
    char is_type_dependent = 0;

    type_t* sequence_type = NULL;

    int i;
    for (i = 0; i < len; i++)
    {
        v->pack_index = i;
        nodecl_t expr = instantiate_expr_walk(v, 
                nodecl_shallow_copy(expansion));

        if (nodecl_is_err_expr(expr))
        {
            v->pack_index = old_pack_index;
            v->nodecl_result = expr;
            nodecl_free(nodecl_result);
            return;
        }

        is_value_dependent = is_value_dependent || nodecl_expr_is_value_dependent(expr);
        is_type_dependent = is_type_dependent || nodecl_expr_is_type_dependent(expr);

        sequence_type = get_sequence_of_types_append_type(sequence_type, nodecl_get_type(expr));

        nodecl_result = nodecl_append_to_list(nodecl_result, expr);
    }

    if (!nodecl_is_null(nodecl_result))
    {
        nodecl_set_type(nodecl_result, sequence_type);
        nodecl_expr_set_is_value_dependent(nodecl_result, is_value_dependent);
        nodecl_expr_set_is_type_dependent(nodecl_result, is_type_dependent);
    }

    v->pack_index = old_pack_index;

    v->nodecl_result = nodecl_result;
}

static void instantiate_cxx_dep_new(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_initializer = nodecl_get_child(node, 0);
    // nodecl_t nodecl_type = nodecl_get_child(node, 1);
    nodecl_t nodecl_placement_list = nodecl_get_child(node, 2);
    type_t* new_type = nodecl_get_type(node);

    nodecl_initializer = instantiate_expr_walk(
            v, nodecl_initializer);

    int num_items = 0;
    nodecl_t* list = nodecl_unpack_list(nodecl_placement_list, &num_items);

    nodecl_t new_nodecl_placement_list = nodecl_null();
    int i;
    for (i = 0; i < num_items; i++)
    {
        nodecl_t new_item = instantiate_expr_walk(v, list[i]);
        new_nodecl_placement_list = nodecl_append_to_list(new_nodecl_placement_list, new_item);
    }
    xfree(list);

    new_type = update_type_for_instantiation(
            new_type,
            v->decl_context,
            nodecl_get_locus(node),
            v->instantiation_symbol_map,
            /* pack_index */ -1);

    char is_global = strcmp(nodecl_get_text(node), "global") == 0;

    check_new_expression_impl(
            new_nodecl_placement_list,
            nodecl_initializer,
            new_type,
            is_global,
            v->decl_context,
            nodecl_get_locus(node),
            &v->nodecl_result);
}

static void instantiate_delete(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_expr = nodecl_get_child(node, 0);

    nodecl_expr = instantiate_expression(
            nodecl_expr,
            v->decl_context,
            v->instantiation_symbol_map,
            /* pack_index */ -1);

    v->nodecl_result = nodecl_make_delete(nodecl_expr, get_void_type(), nodecl_get_locus(node));
}

static void instantiate_delete_array(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    nodecl_t nodecl_expr = nodecl_get_child(node, 0);

    nodecl_expr = instantiate_expression(
            nodecl_expr,
            v->decl_context,
            v->instantiation_symbol_map,
            /* pack_index */ -1);

    v->nodecl_result = nodecl_make_delete_array(nodecl_expr, get_void_type(), nodecl_get_locus(node));
}

static void instantiate_new(nodecl_instantiate_expr_visitor_t* v, nodecl_t node)
{
    v->nodecl_result = nodecl_shallow_copy(node);
}

// Initialization
static void instantiate_expr_init_visitor(nodecl_instantiate_expr_visitor_t* v, decl_context_t decl_context)
{
    nodecl_init_walker((nodecl_external_visitor_t*)v, instantiate_expr_visitor_fun(instantiate_expr_not_implemented_yet));

    v->decl_context = decl_context;

    // Type
    NODECL_VISITOR(v)->visit_type = instantiate_expr_visitor_fun(instantiate_type);

    // Literals
    NODECL_VISITOR(v)->visit_integer_literal = instantiate_expr_visitor_fun(instantiate_expr_literal);
    NODECL_VISITOR(v)->visit_floating_literal = instantiate_expr_visitor_fun(instantiate_expr_literal);
    NODECL_VISITOR(v)->visit_string_literal = instantiate_expr_visitor_fun(instantiate_expr_literal);
    NODECL_VISITOR(v)->visit_boolean_literal = instantiate_expr_visitor_fun(instantiate_expr_literal);

    // Compound expression
    NODECL_VISITOR(v)->visit_compound_expression = instantiate_expr_visitor_fun(instantiate_compound_expression);

    // Symbol
    NODECL_VISITOR(v)->visit_symbol = instantiate_expr_visitor_fun(instantiate_symbol);

    // C++11 lambda
    NODECL_VISITOR(v)->visit_cxx_lambda = instantiate_expr_visitor_fun(instantiate_cxx_lambda);

    // Class member access
    NODECL_VISITOR(v)->visit_class_member_access = instantiate_expr_visitor_fun(instantiate_class_member_access);
    NODECL_VISITOR(v)->visit_cxx_class_member_access = instantiate_expr_visitor_fun(instantiate_cxx_class_member_access);
    NODECL_VISITOR(v)->visit_cxx_arrow = instantiate_expr_visitor_fun(instantiate_cxx_arrow);

    // Array subscript
    NODECL_VISITOR(v)->visit_array_subscript = instantiate_expr_visitor_fun(instantiate_array_subscript);

    // Cxx Array Sections
    NODECL_VISITOR(v)->visit_cxx_array_section_size = instantiate_expr_visitor_fun(instantiate_cxx_array_section_size);
    NODECL_VISITOR(v)->visit_cxx_array_section_range = instantiate_expr_visitor_fun(instantiate_cxx_array_section_range);


    // Throw
    NODECL_VISITOR(v)->visit_throw = instantiate_expr_visitor_fun(instantiate_throw);

    // Binary operations
    NODECL_VISITOR(v)->visit_add = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_mul = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_div = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_mod = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_minus = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_shl = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_shr = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_arithmetic_shr = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_lower_than = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_greater_than = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_greater_or_equal_than = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_lower_or_equal_than = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_equal = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_different = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_and = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_xor = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_or = instantiate_expr_visitor_fun(instantiate_binary_op);

    NODECL_VISITOR(v)->visit_logical_and = instantiate_expr_visitor_fun(instantiate_logical_and);
    NODECL_VISITOR(v)->visit_logical_or = instantiate_expr_visitor_fun(instantiate_logical_or);

    NODECL_VISITOR(v)->visit_power = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_mul_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_div_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_add_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_minus_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_shl_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_shr_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_arithmetic_shr_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_and_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_or_assignment  = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_bitwise_xor_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);
    NODECL_VISITOR(v)->visit_mod_assignment = instantiate_expr_visitor_fun(instantiate_binary_op);

    NODECL_VISITOR(v)->visit_comma = instantiate_expr_visitor_fun(instantiate_comma_op);

    NODECL_VISITOR(v)->visit_preincrement = instantiate_expr_visitor_fun(instantiate_preincrement);
    NODECL_VISITOR(v)->visit_postincrement = instantiate_expr_visitor_fun(instantiate_postincrement);
    NODECL_VISITOR(v)->visit_predecrement = instantiate_expr_visitor_fun(instantiate_predecrement);
    NODECL_VISITOR(v)->visit_postdecrement = instantiate_expr_visitor_fun(instantiate_postdecrement);

    // Unary
    NODECL_VISITOR(v)->visit_dereference = instantiate_expr_visitor_fun(instantiate_unary_op);
    NODECL_VISITOR(v)->visit_reference = instantiate_expr_visitor_fun(instantiate_reference);
    NODECL_VISITOR(v)->visit_plus = instantiate_expr_visitor_fun(instantiate_unary_op);
    NODECL_VISITOR(v)->visit_neg = instantiate_expr_visitor_fun(instantiate_unary_op);
    NODECL_VISITOR(v)->visit_logical_not = instantiate_expr_visitor_fun(instantiate_unary_op);
    NODECL_VISITOR(v)->visit_bitwise_not = instantiate_expr_visitor_fun(instantiate_unary_op);

    NODECL_VISITOR(v)->visit_structured_value = instantiate_expr_visitor_fun(instantiate_structured_value);

    NODECL_VISITOR(v)->visit_field_designator = instantiate_expr_visitor_fun(instantiate_field_designator);
    NODECL_VISITOR(v)->visit_index_designator = instantiate_expr_visitor_fun(instantiate_index_designator);

    // Function call
    NODECL_VISITOR(v)->visit_function_call = instantiate_expr_visitor_fun(instantiate_function_call);
    NODECL_VISITOR(v)->visit_cxx_dep_function_call = instantiate_expr_visitor_fun(instantiate_cxx_dep_function_call);
    NODECL_VISITOR(v)->visit_gxx_trait = instantiate_expr_visitor_fun(instantiate_gxx_trait);

    // Sizeof
    NODECL_VISITOR(v)->visit_sizeof = instantiate_expr_visitor_fun(instantiate_nondep_sizeof);
    NODECL_VISITOR(v)->visit_cxx_sizeof = instantiate_expr_visitor_fun(instantiate_dep_sizeof_expr);
    NODECL_VISITOR(v)->visit_cxx_sizeof_pack = instantiate_expr_visitor_fun(instantiate_dep_sizeof_pack);

    // Alignof
    NODECL_VISITOR(v)->visit_alignof = instantiate_expr_visitor_fun(instantiate_nondep_alignof);
    NODECL_VISITOR(v)->visit_cxx_alignof = instantiate_expr_visitor_fun(instantiate_dep_alignof_expr);

    // Offsetoff
    NODECL_VISITOR(v)->visit_offsetof = instantiate_expr_visitor_fun(instantiate_offsetof);

    // noexcept
    NODECL_VISITOR(v)->visit_cxx_noexcept = instantiate_expr_visitor_fun(instantiate_noexcept);

    // Casts
    NODECL_VISITOR(v)->visit_cast = instantiate_expr_visitor_fun(instantiate_cast);

    // Conversion
    NODECL_VISITOR(v)->visit_conversion = instantiate_expr_visitor_fun(instantiate_conversion);

    NODECL_VISITOR(v)->visit_parenthesized_expression = instantiate_expr_visitor_fun(instantiate_parenthesized_expression);

    // Initializers
    NODECL_VISITOR(v)->visit_cxx_initializer = instantiate_expr_visitor_fun(instantiate_initializer);
    NODECL_VISITOR(v)->visit_cxx_equal_initializer = instantiate_expr_visitor_fun(instantiate_equal_initializer);
    NODECL_VISITOR(v)->visit_cxx_braced_initializer = instantiate_expr_visitor_fun(instantiate_braced_initializer);
    NODECL_VISITOR(v)->visit_cxx_parenthesized_initializer = instantiate_expr_visitor_fun(instantiate_parenthesized_initializer);
    NODECL_VISITOR(v)->visit_cxx_explicit_type_cast = instantiate_expr_visitor_fun(instantiate_explicit_type_cast);

    // Names
    NODECL_VISITOR(v)->visit_cxx_dep_name_simple = instantiate_expr_visitor_fun(instantiate_dep_name_simple);
    NODECL_VISITOR(v)->visit_cxx_dep_template_id = instantiate_expr_visitor_fun(instantiate_dep_template_id);
    NODECL_VISITOR(v)->visit_cxx_dep_name_nested = instantiate_expr_visitor_fun(instantiate_dep_name_nested);
    NODECL_VISITOR(v)->visit_cxx_dep_global_name_nested = instantiate_expr_visitor_fun(instantiate_dep_global_name_nested);

    // Conditional
    NODECL_VISITOR(v)->visit_conditional_expression = instantiate_expr_visitor_fun(instantiate_conditional_expression);

    // Value packs
    NODECL_VISITOR(v)->visit_cxx_value_pack = instantiate_expr_visitor_fun(instantiate_cxx_value_pack);

    // New
    NODECL_VISITOR(v)->visit_new = instantiate_expr_visitor_fun(instantiate_new);
    NODECL_VISITOR(v)->visit_cxx_dep_new = instantiate_expr_visitor_fun(instantiate_cxx_dep_new);

    // Delete
    NODECL_VISITOR(v)->visit_delete = instantiate_expr_visitor_fun(instantiate_delete);
    NODECL_VISITOR(v)->visit_delete_array = instantiate_expr_visitor_fun(instantiate_delete_array);
}


char same_functional_expression(
        nodecl_t n1,
        nodecl_t n2)
{
    if (nodecl_is_null(n1) != nodecl_is_null(n2))
        return 0;

    if (nodecl_is_null(n1))
        return 1;

    if ((nodecl_get_constant(n1) == NULL)
            != (nodecl_get_constant(n2) == NULL))
        return 0;

    if (nodecl_get_constant(n1) != NULL)
    {
        return const_value_is_nonzero(
                const_value_eq(
                    nodecl_get_constant(n1),
                    nodecl_get_constant(n2)));
    }

    if (nodecl_get_kind(n1) != nodecl_get_kind(n2))
        return 0;

    if (nodecl_get_symbol(n1) != NULL
            && nodecl_get_symbol(n2) != NULL)
    {
        scope_entry_t* s1 = nodecl_get_symbol(n1);
        scope_entry_t* s2 = nodecl_get_symbol(n2);

        if (s1 != s2
                && !(s1->kind == SK_VARIABLE
                    && symbol_is_parameter_of_function(s1, get_function_declaration_proxy())
                    && s2->kind == SK_VARIABLE
                    && symbol_is_parameter_of_function(s2, get_function_declaration_proxy())
                    && (symbol_get_parameter_nesting_in_function(s1, get_function_declaration_proxy()) ==
                        symbol_get_parameter_nesting_in_function(s2, get_function_declaration_proxy()))
                    && (symbol_get_parameter_position_in_function(s1, get_function_declaration_proxy()) ==
                        symbol_get_parameter_position_in_function(s2, get_function_declaration_proxy()))
                    && equivalent_types(s1->type_information, s2->type_information))
                && !((s1->kind == SK_TEMPLATE_NONTYPE_PARAMETER
                        || s1->kind == SK_TEMPLATE_NONTYPE_PARAMETER_PACK)
                    && s1->kind == s2->kind
                    && symbol_entity_specs_get_template_parameter_nesting(s1) == symbol_entity_specs_get_template_parameter_nesting(s2)
                    && symbol_entity_specs_get_template_parameter_position(s1) == symbol_entity_specs_get_template_parameter_position(s2))
                && !(s1->kind == SK_DEPENDENT_ENTITY
                    && s2->kind == SK_DEPENDENT_ENTITY
                    && equivalent_types(s1->type_information, s2->type_information)))
        {
            return 0;
        }
    }
    else if (nodecl_get_symbol(n1) != nodecl_get_symbol(n2))
    {
        return 0;
    }

    int i;
    for (i = 0; i < MCXX_MAX_AST_CHILDREN; i++)
    {
        if (!same_functional_expression(
                    nodecl_get_child(n1, i),
                    nodecl_get_child(n2, i)))
            return 0;
    }

    return 1;
}
