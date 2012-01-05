/*--------------------------------------------------------------------
  (C) Copyright 2006-2011 Barcelona Supercomputing Center 
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


#include "fortran03-exprtype.h"
#include "fortran03-buildscope.h"
#include "fortran03-scope.h"
#include "fortran03-prettyprint.h"
#include "fortran03-typeutils.h"
#include "fortran03-intrinsics.h"
#include "fortran03-codegen.h"
#include "cxx-exprtype.h"
#include "cxx-entrylist.h"
#include "cxx-ast.h"
#include "cxx-ambiguity.h"
#include "cxx-utils.h"
#include "cxx-tltype.h"
#include "cxx-nodecl.h"
#include "cxx-nodecl-output.h"
#include "cxx-diagnostic.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static void fortran_check_expression_impl_(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_symbol_of_called_name(AST sym, decl_context_t decl_context, nodecl_t* nodecl_output, char is_call_stmt);

static void check_symbol_of_argument(AST sym, decl_context_t decl_context, nodecl_t* nodecl_output);


static nodecl_t fortran_nodecl_adjust_function_argument(
        type_t* parameter_type,
        nodecl_t argument);

char fortran_check_expression(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    fortran_check_expression_impl_(a, decl_context, nodecl_output);

    return !nodecl_is_err_expr(*nodecl_output);
}

typedef void (*check_expression_function_t)(AST statement, decl_context_t, nodecl_t* nodecl_output);
typedef struct check_expression_handler_tag
{
    node_t ast_kind;
    check_expression_function_t handler;
} check_expression_handler_t;

#define STATEMENT_HANDLER_TABLE \
 STATEMENT_HANDLER(AST_ADD, check_add_op) \
 STATEMENT_HANDLER(AST_ARRAY_CONSTRUCTOR, check_array_constructor) \
 STATEMENT_HANDLER(AST_ARRAY_SUBSCRIPT, check_array_ref) \
 STATEMENT_HANDLER(AST_BINARY_LITERAL, check_binary_literal) \
 STATEMENT_HANDLER(AST_BOOLEAN_LITERAL, check_boolean_literal) \
 STATEMENT_HANDLER(AST_COMPLEX_LITERAL, check_complex_literal) \
 STATEMENT_HANDLER(AST_CLASS_MEMBER_ACCESS, check_component_ref) \
 STATEMENT_HANDLER(AST_CONCAT, check_concat_op) \
 STATEMENT_HANDLER(AST_DECIMAL_LITERAL, check_decimal_literal) \
 STATEMENT_HANDLER(AST_DERIVED_TYPE_CONSTRUCTOR, check_derived_type_constructor) \
 STATEMENT_HANDLER(AST_DIFFERENT, check_different_op) \
 STATEMENT_HANDLER(AST_DIV, check_div_op) \
 STATEMENT_HANDLER(AST_EQUAL, check_equal_op) \
 STATEMENT_HANDLER(AST_FLOATING_LITERAL, check_floating_literal) \
 STATEMENT_HANDLER(AST_FUNCTION_CALL, check_function_call) \
 STATEMENT_HANDLER(AST_GREATER_OR_EQUAL_THAN, check_greater_or_equal_than) \
 STATEMENT_HANDLER(AST_GREATER_THAN, check_greater_than) \
 STATEMENT_HANDLER(AST_HEXADECIMAL_LITERAL, check_hexadecimal_literal) \
 STATEMENT_HANDLER(AST_IMAGE_REF, check_image_ref) \
 STATEMENT_HANDLER(AST_LOGICAL_AND, check_logical_and) \
 STATEMENT_HANDLER(AST_LOGICAL_EQUAL, check_logical_equal) \
 STATEMENT_HANDLER(AST_LOGICAL_DIFFERENT, check_logical_different) \
 STATEMENT_HANDLER(AST_LOGICAL_OR, check_logical_or) \
 STATEMENT_HANDLER(AST_LOWER_OR_EQUAL_THAN, check_lower_or_equal_than) \
 STATEMENT_HANDLER(AST_LOWER_THAN, check_lower_than) \
 STATEMENT_HANDLER(AST_MINUS, check_minus_op) \
 STATEMENT_HANDLER(AST_MUL, check_mult_op) \
 STATEMENT_HANDLER(AST_NEG, check_neg_op) \
 STATEMENT_HANDLER(AST_LOGICAL_NOT, check_not_op) \
 STATEMENT_HANDLER(AST_OCTAL_LITERAL, check_octal_literal) \
 STATEMENT_HANDLER(AST_PARENTHESIZED_EXPRESSION, check_parenthesized_expression) \
 STATEMENT_HANDLER(AST_PLUS, check_plus_op) \
 STATEMENT_HANDLER(AST_POWER, check_power_op) \
 STATEMENT_HANDLER(AST_STRING_LITERAL, check_string_literal) \
 STATEMENT_HANDLER(AST_USER_DEFINED_UNARY_OP, check_user_defined_unary_op) \
 STATEMENT_HANDLER(AST_SYMBOL, check_symbol_variable) \
 STATEMENT_HANDLER(AST_ASSIGNMENT, check_assignment) \
 STATEMENT_HANDLER(AST_PTR_ASSIGNMENT, check_ptr_assignment) \
 STATEMENT_HANDLER(AST_AMBIGUITY, disambiguate_expression) \
 STATEMENT_HANDLER(AST_USER_DEFINED_BINARY_OP, check_user_defined_binary_op) \

// Enable this if you really need extremely verbose typechecking
// #define VERBOSE_DEBUG_EXPR 1

#ifdef VERBOSE_DEBUG_EXPR
  // Prototypes
  #define STATEMENT_HANDLER(_kind, _handler) \
      static void _handler(AST, decl_context_t); \
      static void _handler##_(AST a, decl_context_t d) \
      { \
          DEBUG_CODE() \
          { \
              fprintf(stderr, "%s: -> %s\n", ast_location(a), #_handler); \
          } \
          _handler(a, d); \
          DEBUG_CODE() \
          { \
              fprintf(stderr, "%s: <- %s\n", ast_location(a), #_handler); \
          } \
      }
#else
  #define STATEMENT_HANDLER(_kind, _handler) \
      static void _handler(AST, decl_context_t, nodecl_t* nodecl_output); 
#endif

STATEMENT_HANDLER_TABLE
#undef STATEMENT_HANDLER

// Table
#ifdef VERBOSE_DEBUG_EXPR
  #define STATEMENT_HANDLER(_kind, _handler) \
     { .ast_kind = _kind, .handler = _handler##_ },
#else
  #define STATEMENT_HANDLER(_kind, _handler) \
     { .ast_kind = _kind, .handler = _handler },
#endif
static check_expression_handler_t check_expression_function[] = 
{
  STATEMENT_HANDLER_TABLE
};
#undef STATEMENT_HANDLER

static int check_expression_function_init = 0;

static int check_expression_function_compare(const void *a, const void *b)
{
    check_expression_handler_t *pa = (check_expression_handler_t*)a;
    check_expression_handler_t *pb = (check_expression_handler_t*)b;

    if (pa->ast_kind < pb->ast_kind)
        return -1;
    else if (pa->ast_kind > pb->ast_kind)
        return 1;
    else
        return 0;
}

static void fortran_check_expression_impl_(AST expression, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    ERROR_CONDITION(expression == NULL, "Invalid tree for expression", 0);
    ERROR_CONDITION(nodecl_output == NULL, "Nodecl cannot be NULL here", 0);

    // Sort the array if needed
    if (!check_expression_function_init)
    {
        // void qsort(void *base, size_t nmemb, size_t size,
        //    int(*compar)(const void *, const void *));
        qsort(check_expression_function, 
                sizeof(check_expression_function) / sizeof(check_expression_function[0]),
                sizeof(check_expression_function[0]),
                check_expression_function_compare);
        check_expression_function_init = 1;
    }

    check_expression_handler_t key = { .ast_kind = ASTType(expression) };
    check_expression_handler_t *handler = NULL;

    // void *bsearch(const void *key, const void *base,
    //       size_t nmemb, size_t size,
    //       int (*compar)(const void *, const void *));
    handler = (check_expression_handler_t*)bsearch(&key, check_expression_function, 
            sizeof(check_expression_function) / sizeof(check_expression_function[0]),
            sizeof(check_expression_function[0]),
            check_expression_function_compare);
    if (handler == NULL 
            || handler->handler == NULL)
    {
        running_error("%s: sorry: unhandled expression %s\n", 
                ast_location(expression), 
                ast_print_node_type(ASTType(expression)));
    }
    (handler->handler)(expression, decl_context, nodecl_output);

    ERROR_CONDITION(nodecl_is_null(*nodecl_output), "Nodecl cannot be NULL here", 0);

    ERROR_CONDITION(
            (!nodecl_is_err_expr(*nodecl_output)
             && (nodecl_get_type(*nodecl_output) == NULL ||
                 is_error_type(nodecl_get_type(*nodecl_output)))),
            "This should be an error expression", 0);

    DEBUG_CODE()
    {
        if (!nodecl_is_constant(*nodecl_output))
        {
            fprintf(stderr, "EXPRTYPE: %s: '%s' has type '%s'\n",
                    ast_location(expression),
                    fortran_prettyprint_in_buffer(expression),
                    print_declarator(nodecl_get_type(*nodecl_output)));
        }
        else
        {
            fprintf(stderr, "EXPRTYPE: %s: '%s' has type '%s' with a constant value of '%s'\n",
                    ast_location(expression),
                    fortran_prettyprint_in_buffer(expression),
                    print_declarator(nodecl_get_type(*nodecl_output)),
                    codegen_to_str(const_value_to_nodecl(nodecl_get_constant(*nodecl_output))));
        }
    }

    if (!checking_ambiguity() 
            && CURRENT_CONFIGURATION->strict_typecheck)
    {
        if (nodecl_get_type(*nodecl_output) == NULL
                || nodecl_is_err_expr(*nodecl_output))
        {
            internal_error("%s: invalid expression '%s'\n",
                    ast_location(expression),
                    fortran_prettyprint_in_buffer(expression));
        }
    }
}

static type_t* compute_result_of_intrinsic_operator(AST expr, decl_context_t, type_t* lhs_type, type_t* rhs_type, 
        nodecl_t nodecl_lhs, nodecl_t nodecl_rhs, nodecl_t* nodecl_output);

static void common_binary_check(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);
static void common_unary_check(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output);

static void check_add_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_ac_value_list(AST ac_value_list, decl_context_t decl_context, 
        nodecl_t* nodecl_output, 
        type_t** current_type, int *num_items)
{
    AST it;
    for_each_element(ac_value_list, it)
    {
        AST ac_value = ASTSon1(it);

        if (ASTType(ac_value) == AST_IMPLIED_DO)
        {
            AST implied_do_ac_value = ASTSon0(ac_value);

            AST implied_do_control = ASTSon1(ac_value);
            AST ac_do_variable = ASTSon0(implied_do_control);
            AST lower_bound = ASTSon1(implied_do_control);
            AST upper_bound = ASTSon2(implied_do_control);
            AST stride = ASTSon3(implied_do_control);

            nodecl_t nodecl_lower = nodecl_null();
            fortran_check_expression_impl_(lower_bound, decl_context, &nodecl_lower);
            nodecl_t nodecl_upper = nodecl_null();
            fortran_check_expression_impl_(upper_bound, decl_context, &nodecl_upper);
            nodecl_t nodecl_stride = nodecl_null();
            if (stride != NULL)
            {
                fortran_check_expression_impl_(stride, decl_context, &nodecl_stride);
            }
            else
            {
                nodecl_stride = const_value_to_nodecl(const_value_get_one(/* bytes */ fortran_get_default_integer_type_kind(), /* signed */ 1));
            }

            scope_entry_t* do_variable = fortran_get_variable_with_locus(decl_context, ac_do_variable, ASTText(ac_do_variable));

            if (do_variable == NULL)
            {
                running_error("%s: error: unknown symbol '%s' in ac-implied-do\n", ast_location(ac_do_variable), ASTText(ac_do_variable));
            }

            if (do_variable->kind == SK_UNDEFINED)
            {
                do_variable->kind = SK_VARIABLE;
                remove_unknown_kind_symbol(decl_context, do_variable);
            }
            else if (do_variable->kind != SK_VARIABLE)
            {
                running_error("%s: error: invalid name '%s' for ac-implied-do\n", ast_location(ac_do_variable), ASTText(ac_do_variable));
            }

            if (nodecl_is_constant(nodecl_lower)
                    && nodecl_is_constant(nodecl_upper)
                    && nodecl_is_constant(nodecl_stride))
            {
                int val_lower = const_value_cast_to_signed_int(nodecl_get_constant(nodecl_lower));
                int val_upper = const_value_cast_to_signed_int(nodecl_get_constant(nodecl_upper));
                int val_stride = const_value_cast_to_signed_int(nodecl_get_constant(nodecl_stride));

                int trip = (val_upper
                        - val_lower
                        + val_stride)
                    / val_stride;

                if (trip < 0)
                {
                    trip = 0;
                }
                (*num_items) += trip;

                // Save information of the symbol
                type_t* original_type = do_variable->type_information;
                nodecl_t original_value = do_variable->value;

                // Set it as a PARAMETER of kind INTEGER (so we will effectively use its value)
                do_variable->type_information = get_const_qualified_type(fortran_get_default_integer_type());

                int i;
                if (val_stride > 0)
                {
                    for (i = val_lower;
                            i <= val_upper;
                            i+= val_stride)
                    {
                        // Set the value of the variable
                        do_variable->value = const_value_to_nodecl(const_value_get_signed_int(i));

                        check_ac_value_list(implied_do_ac_value, decl_context, nodecl_output, current_type, num_items);
                    }
                }
                else if (val_stride < 0)
                {
                    for (i = val_lower;
                            i >= val_upper;
                            i += val_stride)
                    {
                        // Set the value of the variable
                        do_variable->value = const_value_to_nodecl(const_value_get_signed_int(i));

                        check_ac_value_list(implied_do_ac_value, decl_context, nodecl_output, current_type, num_items);
                    }
                }
                else
                {
                    error_printf("%s: error: step of implied-do is zero\n", ast_location(stride));
                }

                // Restore the variable used for the expansion
                do_variable->type_information = original_type;
                do_variable->value = original_value;
            }
            else
            {
                int current_num_items = 0;
                nodecl_t nodecl_ac_value = nodecl_null();
                check_ac_value_list(implied_do_ac_value, decl_context, &nodecl_ac_value, current_type, &current_num_items);

                nodecl_t nodecl_implied_do = 
                    nodecl_make_fortran_implied_do(
                            nodecl_make_symbol(do_variable, ASTFileName(ac_do_variable), ASTLine(ac_do_variable)),
                            nodecl_make_range(nodecl_lower, 
                                nodecl_upper, 
                                nodecl_stride, 
                                fortran_get_default_integer_type(),
                                ASTFileName(implied_do_control), 
                                ASTLine(implied_do_control)),
                            nodecl_ac_value,
                            ASTFileName(implied_do_control), 
                            ASTLine(implied_do_control));
                (*num_items) = -1;
                *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_implied_do);
            }

        }
        else
        {
            nodecl_t nodecl_expr = nodecl_null();
            fortran_check_expression_impl_(ac_value, decl_context, &nodecl_expr);

            if (nodecl_is_err_expr(nodecl_expr))
            {
                *nodecl_output = nodecl_expr;
                return;
            }
            else if (*current_type == NULL)
            {
                *current_type = get_rank0_type(nodecl_get_type(nodecl_expr));
            }

            if ((*num_items) >= 0)
            {
                type_t* expr_type = nodecl_get_type(nodecl_expr);

                if (!is_array_type(expr_type))
                {
                    (*num_items)++;
                }
                else
                {
                    int num_elements = array_type_get_total_number_of_elements(expr_type);
                    if (num_elements >= 0)
                    {
                        *num_items += num_elements;
                    }
                    else
                    {
                        *num_items = -1;
                    }
                }
            }

            *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_expr);
        }
    }
}

static void check_array_constructor(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST ac_spec = ASTSon0(expr);
    AST type_spec = ASTSon0(ac_spec);

    if (type_spec != NULL)
    {
        error_printf("%s: error: type specifier in array constructors not supported\n",
                ast_location(type_spec));
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    AST ac_value_list = ASTSon1(ac_spec);
    nodecl_t nodecl_ac_value = nodecl_null();
    type_t* ac_value_type = NULL;
    int num_items = 0;
    check_ac_value_list(ac_value_list, decl_context, &nodecl_ac_value, &ac_value_type, &num_items);

    if (nodecl_is_err_expr(nodecl_ac_value))
    {
        *nodecl_output = nodecl_ac_value;
        return;
    }

    // Check for const-ness
    int i, n;
    char all_constants = 1;
    nodecl_t* list = nodecl_unpack_list(nodecl_ac_value, &n);

    const_value_t* constants[n];
    memset(constants, 0, sizeof(constants));

    for (i = 0; i < n && all_constants; i++)
    {
        constants[i] = nodecl_get_constant(list[i]);
        all_constants = all_constants && (constants[i] != NULL);
    }
    free(list);

    if (num_items > 0)
    {
        ac_value_type = get_array_type_bounds(ac_value_type,
                const_value_to_nodecl(const_value_get_one(fortran_get_default_integer_type_kind(), /* signed */ 1)),
                const_value_to_nodecl(const_value_get_signed_int(num_items)),
                decl_context);

        if (all_constants)
        {
            ac_value_type = get_const_qualified_type(ac_value_type);
        }
    }
    else
    {
        ac_value_type = get_array_type_bounds(ac_value_type, nodecl_null(), nodecl_null(), decl_context);
    }

    *nodecl_output = nodecl_make_structured_value(nodecl_ac_value,
            ac_value_type,
            ASTFileName(expr), ASTLine(expr));

    if (all_constants)
    {
        const_value_t* array_val = const_value_make_array(n, constants);
        nodecl_set_constant(*nodecl_output, array_val);
    }
}

static void check_substring(AST expr, decl_context_t decl_context, nodecl_t nodecl_subscripted, nodecl_t* nodecl_output)
{
    type_t* subscripted_type = no_ref(nodecl_get_type(nodecl_subscripted));

    AST subscript_list = ASTSon1(expr);

    int num_subscripts = 0;
    AST it;
    for_each_element(subscript_list, it)
    {
        num_subscripts++;
    }

    if (num_subscripts != 1)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: invalid number of subscripts (%d) in substring expression\n",
                    ast_location(expr),
                    num_subscripts);
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    AST subscript = ASTSon1(subscript_list);

    AST lower = ASTSon0(subscript);
    AST upper = ASTSon1(subscript);
    AST stride = ASTSon2(subscript);

    if (stride != NULL)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: a stride is not valid in a substring expression\n",
                    ast_location(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    nodecl_t nodecl_lower = nodecl_null();
    if (lower != NULL)
        fortran_check_expression_impl_(lower, decl_context, &nodecl_lower);

    nodecl_t nodecl_upper = nodecl_null();
    if (upper != NULL)
        fortran_check_expression_impl_(upper, decl_context, &nodecl_upper);

    type_t* synthesized_type = NULL;

    // Do not compute the exact size at the moment
    synthesized_type = get_array_type_bounds(array_type_get_element_type(subscripted_type), nodecl_lower, nodecl_upper, decl_context);

    nodecl_t nodecl_stride = const_value_to_nodecl(const_value_get_one(/* bytes */ fortran_get_default_integer_type_kind(), /* signed */ 1));
    
    char is_derref_subscripted = nodecl_get_kind(nodecl_subscripted) == NODECL_DERREFERENCE;

    type_t* data_type = synthesized_type;
    if (is_derref_subscripted)
    {
        nodecl_subscripted = nodecl_get_child(nodecl_subscripted, 0);
        data_type = get_pointer_type(data_type);
    }

    *nodecl_output = nodecl_make_array_subscript(
            nodecl_subscripted,
            nodecl_make_list_1(
                nodecl_make_range(nodecl_lower, nodecl_upper, nodecl_stride, fortran_get_default_integer_type(), ASTFileName(expr), ASTLine(expr))),
            data_type,
            ASTFileName(expr), ASTLine(expr));

    nodecl_set_symbol(*nodecl_output, nodecl_get_symbol(nodecl_subscripted));

    if (is_derref_subscripted)
    {
        *nodecl_output = nodecl_make_derreference(
                *nodecl_output,
                synthesized_type,
                nodecl_get_filename(*nodecl_output),
                nodecl_get_line(*nodecl_output));
        nodecl_set_symbol(*nodecl_output, nodecl_get_symbol(nodecl_subscripted));
    }
}


static const_value_t* compute_subconstant_of_array_rec(
        const_value_t* current_rank_value,
        type_t* current_array_type,
        nodecl_t* all_subscripts,
        int current_subscript, 
        int total_subscripts)
{
    nodecl_t current_nodecl_subscript = all_subscripts[(total_subscripts - 1) - current_subscript];
    const_value_t* const_of_subscript = nodecl_get_constant(current_nodecl_subscript);

    const_value_t* result_value = NULL;

    int array_rank_base = const_value_cast_to_signed_int(
            nodecl_get_constant(array_type_get_array_lower_bound(current_array_type)));

    if (const_value_is_range(const_of_subscript))
    {
        int lower = const_value_cast_to_signed_int(const_value_get_element_num(const_of_subscript, 0));
        int upper = const_value_cast_to_signed_int(const_value_get_element_num(const_of_subscript, 1));
        int stride = const_value_cast_to_signed_int(const_value_get_element_num(const_of_subscript, 2));
        int trip = (upper - lower + stride) / stride;

        const_value_t* result_array[trip];
        memset(result_array, 0, sizeof(result_array));

        int i, item = 0;
        if (stride > 0)
        {
            for (i = lower; i <= upper; i += stride, item++)
            {
                if ((current_subscript + 1) == total_subscripts)
                {
                    result_array[item] = const_value_get_element_num(current_rank_value, i);
                }
                else
                {
                    result_array[item] = compute_subconstant_of_array_rec(
                            const_value_get_element_num(current_rank_value, i - array_rank_base),
                            array_type_get_element_type(current_array_type),
                            all_subscripts,
                            current_subscript + 1,
                            total_subscripts);
                }
            }
        }
        else
        {
            for (i = lower; i >= upper; i += stride, item++)
            {
                if ((current_subscript + 1) == total_subscripts)
                {
                    result_array[item] = const_value_get_element_num(current_rank_value, i - array_rank_base);
                }
                else
                {
                    result_array[item] = compute_subconstant_of_array_rec(
                            const_value_get_element_num(current_rank_value, i - array_rank_base),
                            array_type_get_element_type(current_array_type),
                            all_subscripts,
                            current_subscript + 1,
                            total_subscripts);
                }
            }
        }

        result_value = const_value_make_array(trip, result_array);
    }
    else if (const_value_is_array(const_of_subscript))
    {
        int trip = const_value_get_num_elements(const_of_subscript);

        const_value_t* result_array[trip];
        memset(result_array, 0, sizeof(result_array));

        int p;
        for (p = 0; p < trip; p++)
        {
            int i = const_value_cast_to_signed_int(
                    const_value_get_element_num(const_of_subscript, p));

            if ((current_subscript + 1) == total_subscripts)
            {
                result_array[p] = const_value_get_element_num(current_rank_value, i - array_rank_base);
            }
            else
            {
                result_array[p] = compute_subconstant_of_array_rec(
                        const_value_get_element_num(current_rank_value, i - array_rank_base),
                        array_type_get_element_type(current_array_type),
                        all_subscripts,
                        current_subscript + 1,
                        total_subscripts);
            }
        }

        result_value = const_value_make_array(trip, result_array);
    }
    else
    {
        int i = const_value_cast_to_signed_int(const_of_subscript);
        if ((current_subscript + 1) == total_subscripts)
        {
            result_value = const_value_get_element_num(current_rank_value, i - array_rank_base);
        }
        else
        {
            result_value = compute_subconstant_of_array_rec(
                    const_value_get_element_num(current_rank_value, i - array_rank_base),
                    array_type_get_element_type(current_array_type),
                    all_subscripts,
                    current_subscript + 1,
                    total_subscripts);
        }
    }

    ERROR_CONDITION(result_value == NULL, "This is not possible", 0);
    return result_value;
}

static const_value_t* compute_subconstant_of_array(
        const_value_t* current_rank_value,
        type_t* array_type,
        nodecl_t* all_subscripts,
        int total_subscripts)
{
    return compute_subconstant_of_array_rec(current_rank_value, 
            array_type,
            all_subscripts,
            0, total_subscripts);
}

static void check_array_ref_(AST expr, decl_context_t decl_context, nodecl_t nodecl_subscripted, nodecl_t* nodecl_output)
{
    char symbol_is_invalid = 0;

    type_t* array_type = NULL;
    type_t* synthesized_type = NULL;

    int rank_of_type = -1;

    scope_entry_t* symbol = nodecl_get_symbol(nodecl_subscripted);
    if (symbol == NULL
            || (!is_fortran_array_type(no_ref(symbol->type_information))
                && !is_pointer_to_fortran_array_type(no_ref(symbol->type_information))))
    {
        symbol_is_invalid = 1;
    }
    else
    {
        array_type = no_ref(symbol->type_information);
        if (is_pointer_to_fortran_array_type(array_type))
            array_type = pointer_type_get_pointee_type(array_type);

        synthesized_type = get_rank0_type(array_type);
        rank_of_type = get_rank_of_type(array_type);
    }

    AST subscript_list = ASTSon1(expr);

    int num_subscripts = 0;
    AST it;
    for_each_element(subscript_list, it)
    {
        num_subscripts++;
    }

    type_t* dimension_type = array_type;
    nodecl_t nodecl_indexes[num_subscripts];
    nodecl_t nodecl_lower_dim[num_subscripts];
    nodecl_t nodecl_upper_dim[num_subscripts];
    int i;
    for (i = 0; i < num_subscripts; i++)
    {
        nodecl_lower_dim[(num_subscripts - 1) - i] = nodecl_null();
        nodecl_upper_dim[(num_subscripts - 1) - i] = nodecl_null();
        if (is_array_type(dimension_type))
        {
            nodecl_lower_dim[(num_subscripts - 1) - i] = array_type_get_array_lower_bound(dimension_type);

            if (!array_type_is_unknown_size(dimension_type))
                nodecl_upper_dim[(num_subscripts - 1) - i] = array_type_get_array_upper_bound(dimension_type);

            dimension_type = array_type_get_element_type(dimension_type);
        }

        nodecl_indexes[i] = nodecl_null();
    }

    num_subscripts = 0;
    for_each_element(subscript_list, it)
    {
        AST subscript = ASTSon1(it);

        if (ASTType(subscript) == AST_SUBSCRIPT_TRIPLET)
        {
            AST lower = ASTSon0(subscript);
            AST upper = ASTSon1(subscript);
            AST stride = ASTSon2(subscript);
            nodecl_t nodecl_lower = nodecl_null();
            nodecl_t nodecl_upper = nodecl_null();
            nodecl_t nodecl_stride = nodecl_null();
            if (lower != NULL)
            {
                fortran_check_expression_impl_(lower, decl_context, &nodecl_lower);
            }
            else
            {
                nodecl_lower = nodecl_copy(nodecl_lower_dim[num_subscripts]);
            }
            if (upper != NULL)
            {
                fortran_check_expression_impl_(upper, decl_context, &nodecl_upper);
            }
            else
            {
                nodecl_upper = nodecl_copy(nodecl_upper_dim[num_subscripts]);
            }
            if (stride != NULL)
            {
                fortran_check_expression_impl_(stride, decl_context, &nodecl_stride);
            }
            else
            {
                nodecl_stride = const_value_to_nodecl(const_value_get_one(/* bytes */ fortran_get_default_integer_type_kind(), /* signed */ 1));
            }

            if (!nodecl_is_null(nodecl_lower)
                    && nodecl_is_err_expr(nodecl_lower))
            {
                *nodecl_output = nodecl_lower;
                return;
            }

            if (!nodecl_is_null(nodecl_upper)
                    && nodecl_is_err_expr(nodecl_upper))
            {
                *nodecl_output = nodecl_upper;
                return;
            }

            if (nodecl_is_err_expr(nodecl_stride))
            {
                *nodecl_output = nodecl_stride;
                return;
            }

            if (!symbol_is_invalid)
            {
                // FIXME - Stride may imply an array with smaller size (rank is unaffected)
                synthesized_type = get_array_type_bounds(synthesized_type, nodecl_lower, nodecl_upper, decl_context);
            }

            nodecl_indexes[num_subscripts] = nodecl_make_range(
                    nodecl_lower,
                    nodecl_upper,
                    nodecl_stride,
                    fortran_get_default_integer_type(),
                    ASTFileName(subscript),
                    ASTLine(subscript));

            if (!nodecl_is_null(nodecl_lower)
                    && !nodecl_is_null(nodecl_upper)
                    && !nodecl_is_null(nodecl_stride)
                    && nodecl_is_constant(nodecl_lower)
                    && nodecl_is_constant(nodecl_upper)
                    && nodecl_is_constant(nodecl_stride))
            {
                // This range is constant
                nodecl_set_constant(nodecl_indexes[num_subscripts],
                        const_value_make_range(nodecl_get_constant(nodecl_lower),
                            nodecl_get_constant(nodecl_upper),
                            nodecl_get_constant(nodecl_stride)));
            }
        }
        else
        {
            fortran_check_expression_impl_(subscript, decl_context, &nodecl_indexes[num_subscripts]);

            if (nodecl_is_err_expr(nodecl_indexes[num_subscripts]))
            {
                *nodecl_output = nodecl_indexes[num_subscripts];
                return;
            }

            type_t* t = nodecl_get_type(nodecl_indexes[num_subscripts]);

            type_t* rank_0 = get_rank0_type(t);

            if (!is_any_int_type(rank_0))
            {
                if (!checking_ambiguity())
                {
                    warn_printf("%s: warning: subscript of array should be of type INTEGER\n", 
                            ast_location(subscript));
                }
            }

            if (is_pointer_type(no_ref(t)))
                t = pointer_type_get_pointee_type(no_ref(t));

            t = no_ref(t);

            if (is_fortran_array_type(t))
            {
                synthesized_type = rebuild_array_type(synthesized_type, t);
            }
        }
        num_subscripts++;
    }

    if (symbol_is_invalid)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: data reference '%s' does not designate an array name\n",
                    ast_location(expr), fortran_prettyprint_in_buffer(ASTSon0(expr)));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    if (num_subscripts != rank_of_type)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: mismatch in subscripts of array reference, expecting %d got %d\n",
                    ast_location(expr),
                    get_rank_of_type(symbol->type_information),
                    num_subscripts);
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    char all_subscripts_const = 1;
    nodecl_t nodecl_list = nodecl_null();
    for (i = num_subscripts-1; i >= 0; i--)
    {
        if (!nodecl_is_constant(nodecl_indexes[i]))
            all_subscripts_const = 0;
        nodecl_list = nodecl_append_to_list(nodecl_list, nodecl_indexes[i]);
    }

    char is_derref_subscripted = nodecl_get_kind(nodecl_subscripted) == NODECL_DERREFERENCE;

    type_t* data_type = synthesized_type;
    if (is_derref_subscripted)
    {
        nodecl_subscripted = nodecl_get_child(nodecl_subscripted, 0);
        data_type = get_pointer_type(data_type);
    }

    *nodecl_output = nodecl_make_array_subscript(nodecl_subscripted, 
            nodecl_list,
            data_type,
            ASTFileName(expr),
            ASTLine(expr));
    nodecl_set_symbol(*nodecl_output, symbol);

    if (is_derref_subscripted)
    {
        *nodecl_output = nodecl_make_derreference(
                *nodecl_output,
                synthesized_type,
                nodecl_get_filename(*nodecl_output),
                nodecl_get_line(*nodecl_output));
        nodecl_set_symbol(*nodecl_output, nodecl_get_symbol(nodecl_subscripted));
    }

    if (is_const_qualified_type(no_ref(symbol->type_information))
            && all_subscripts_const)
    {
        const_value_t* subconstant = compute_subconstant_of_array(
                nodecl_get_constant(symbol->value),
                array_type,
                nodecl_indexes,
                num_subscripts);
        nodecl_set_constant(*nodecl_output, subconstant);
    }
}

static void check_array_ref(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_subscripted = nodecl_null();
    fortran_check_expression_impl_(ASTSon0(expr), decl_context, &nodecl_subscripted);

    if (nodecl_is_err_expr(nodecl_subscripted))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    type_t* subscripted_type = nodecl_get_type(nodecl_subscripted);

    if (is_fortran_array_type(no_ref(subscripted_type))
            || is_pointer_to_fortran_array_type(no_ref(subscripted_type)))
    {
        check_array_ref_(expr, decl_context, nodecl_subscripted, nodecl_output);
        return;
    }
    else if (is_fortran_character_type(get_rank0_type(no_ref(subscripted_type)))
            || is_pointer_to_fortran_character_type(get_rank0_type(no_ref(subscripted_type))))
    {
        check_substring(expr, decl_context, nodecl_subscripted, nodecl_output);
        return;
    }

    if (!checking_ambiguity())
    {
        error_printf("%s: error: invalid entity '%s' for subscript expression\n",
                ast_location(expr),
                fortran_prettyprint_in_buffer(ASTSon0(expr)));
    }
    *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
}

static char in_string_set(char c, const char* char_set)
{
    int i;
    int len = strlen(char_set);
    for (i = 0; i < len; i++)
    {
        if (tolower(c) == tolower(char_set[i]))
            return 1;
    }

    return 0;
}

static unsigned int get_kind_of_unsigned_value(unsigned long long v)
{
    const unsigned long long b[] = {0xFFUL, 0xFFFFULL, 0xFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL }; 
    const unsigned long long S[] = {1ULL,   2ULL,      4ULL,          8ULL };

    int MAX = (sizeof(S) / sizeof(S[0]));

    int i;
    for (i = 0; i < MAX; i++)
    {
        if (v <= b[i])
        {
            return S[i];
        } 
    }

    return S[MAX-1];
}

static void compute_boz_literal(AST expr, const char *valid_prefix, int base, nodecl_t* nodecl_output)
{
    const char* literal_token = ASTText(expr);

    char literal_text[strlen(literal_token) + 1];
    memset(literal_text, 0, sizeof(literal_text));

    char *q = literal_text;

    char had_prefix = 0;
    if (in_string_set(*literal_token, valid_prefix))
    {
        literal_token++;
        had_prefix = 1;
    }

    ERROR_CONDITION(*literal_token != '\''
            && *literal_token != '\"', "Invalid expr token '%s'!", literal_token);

    const char delim = *literal_token;

    // Jump delimiter
    literal_token++;

    while (*literal_token != delim)
    {
        *q = *literal_token;
        literal_token++;
        q++;
    }

    if (!had_prefix)
    {
        literal_token++;
        if (!in_string_set(*literal_token, valid_prefix))
        {
            ERROR_CONDITION(*literal_token != '\''
                    && *literal_token != '\"', "Invalid expr token!", 0);
        }
    }


    unsigned long long int value = strtoull(literal_text, NULL, base);

    unsigned int kind_size = get_kind_of_unsigned_value(value);

    // We need a nodecl just for diagnostic purposes
    nodecl_t loc = nodecl_make_text(ASTText(expr), ASTFileName(expr), ASTLine(expr));
    type_t* integer_type = choose_int_type_from_kind(loc, kind_size);
    nodecl_free(loc);

    const_value_t* const_value = const_value_get_integer(value, kind_size, /* signed */ 1);

    *nodecl_output = nodecl_make_fortran_boz_literal(
            integer_type, ASTText(expr), const_value,
            ASTFileName(expr),
            ASTLine(expr));
}


static void check_binary_literal(AST expr, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    compute_boz_literal(expr, "b", 2, nodecl_output);
}

static void check_boolean_literal(AST expr, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    const_value_t* const_value = NULL;
    if (strcasecmp(ASTText(expr), ".true.") == 0)
    {
        const_value = const_value_get_one(1, 1);
    }
    else if (strcasecmp(ASTText(expr), ".false.") == 0)
    {
        const_value = const_value_get_zero(1, 1);
    }
    else
    {
        internal_error("Invalid boolean literal", 0);
    }

    *nodecl_output = nodecl_make_boolean_literal(fortran_get_default_logical_type(), const_value, 
            ASTFileName(expr), ASTLine(expr));
}

static void check_complex_literal(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Const value does not support yet complex numbers, simply compute its
    // type
    AST real_part = ASTSon0(expr);
    AST imag_part = ASTSon1(expr);

    nodecl_t nodecl_real = nodecl_null();
    fortran_check_expression_impl_(real_part, decl_context, &nodecl_real);
    if (nodecl_is_err_expr(nodecl_real))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    nodecl_t nodecl_imag = nodecl_null();
    fortran_check_expression_impl_(imag_part, decl_context, &nodecl_imag);
    if (nodecl_is_err_expr(nodecl_imag))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    type_t* real_part_type = no_ref(nodecl_get_type(nodecl_real));
    type_t* imag_part_type = no_ref(nodecl_get_type(nodecl_imag));

    type_t* result_type = NULL;
    if (is_integer_type(real_part_type)
            && is_integer_type(imag_part_type))
    {
        result_type = get_complex_type(get_float_type());
    }
    else if (is_floating_type(real_part_type)
            || is_floating_type(imag_part_type))
    {
        type_t* element_type = NULL;
        if (is_floating_type(real_part_type))
        {
            element_type = real_part_type;
        }

        if (is_floating_type(imag_part_type))
        {
            if (element_type == NULL)
            {
                element_type = imag_part_type;
            }
            else
            {
                // We will choose the bigger one (note that element_type here
                // is already real_part_type, no need to check that case)
                if (type_get_size(imag_part_type) > type_get_size(real_part_type))
                {
                    element_type = imag_part_type;
                }
            }
        }

        result_type = get_complex_type(element_type);
    }
    else
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: invalid complex constant '%s'\n", 
                    ast_location(expr),
                    fortran_prettyprint_in_buffer(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    if (!nodecl_is_constant(nodecl_real))
    {
        error_printf("%s: error: real part '%s' of complex constant is not constant\n",
            ast_location(real_part),
            fortran_prettyprint_in_buffer(real_part));
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }
    if (!nodecl_is_constant(nodecl_imag))
    {
        error_printf("%s: error: imaginary part '%s' of complex constant is not constant\n",
            ast_location(imag_part),
            fortran_prettyprint_in_buffer(imag_part));
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    const_value_t* complex_constant = const_value_make_complex(
            nodecl_get_constant(nodecl_real),
            nodecl_get_constant(nodecl_imag));

    *nodecl_output = nodecl_make_complex_literal(
            nodecl_real, nodecl_imag, 
            result_type,
            ASTFileName(expr), ASTLine(expr));

    nodecl_set_constant(*nodecl_output, complex_constant);
}

static void check_component_ref(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_base = nodecl_null();
    fortran_check_expression_impl_(ASTSon0(expr), decl_context, &nodecl_base);

    if (nodecl_is_err_expr(nodecl_base))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    type_t* t = no_ref(nodecl_get_type(nodecl_base));

    if (is_pointer_type(t))
        t = pointer_type_get_pointee_type(t);

    type_t* class_type = get_rank0_type(t);

    if (!is_pointer_to_class_type(class_type)
            && !is_class_type(class_type))
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: '%s' does not denote a derived type\n",
                    ast_location(expr),
                    fortran_prettyprint_in_buffer(ASTSon0(expr)));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    if (is_pointer_to_class_type(class_type))
    {
        class_type = pointer_type_get_pointee_type(t);
    }

    decl_context_t class_context = class_type_get_inner_context(get_actual_class_type(class_type));

    const char* field = ASTText(ASTSon1(expr));
    scope_entry_t* entry = query_name_in_class(class_context, field);

    if (entry == NULL)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: '%s' is not a component of '%s'\n",
                    ast_location(expr),
                    field,
                    fortran_print_type_str(class_type));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    if (get_rank_of_type(t) != 0
            && get_rank_of_type(entry->type_information) != 0)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: in data-reference '%s' both parts have nonzero rank\n",
                    ast_location(expr),
                    fortran_prettyprint_in_buffer(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    type_t* synthesized_type = entry->type_information;

    char is_pointer = 0;
    if (is_pointer_type(no_ref(synthesized_type)))
    {
        is_pointer = 1;
        synthesized_type = pointer_type_get_pointee_type(no_ref(synthesized_type));
    }
    
    if (!is_fortran_array_type(synthesized_type)
            && !is_pointer_to_fortran_array_type(synthesized_type))
    {
        synthesized_type = rebuild_array_type(synthesized_type, t);
    }

    if (is_pointer)
    {
        // We do this in two steps since we want the symbol in the class member access as well
        *nodecl_output = nodecl_make_class_member_access(nodecl_base, 
                nodecl_make_symbol(entry, ASTFileName(ASTSon1(expr)), ASTLine(ASTSon1(expr))),
                get_pointer_type(synthesized_type),
                ASTFileName(expr), ASTLine(expr)),
        nodecl_set_symbol(*nodecl_output, entry);

        *nodecl_output = 
            nodecl_make_derreference(
                    *nodecl_output,
                    synthesized_type,
                    ASTFileName(expr), ASTLine(expr));
    }
    else
    {
        *nodecl_output = nodecl_make_class_member_access(nodecl_base, 
                nodecl_make_symbol(entry, ASTFileName(ASTSon1(expr)), ASTLine(ASTSon1(expr))),
                synthesized_type,
                ASTFileName(expr), ASTLine(expr));
    }

    nodecl_set_symbol(*nodecl_output, entry);

    if (nodecl_is_constant(nodecl_base))
    {
        // The base is const, thus this component reference is const as well
        const_value_t* const_value = nodecl_get_constant(nodecl_base);
        ERROR_CONDITION(!const_value_is_structured(const_value), "Invalid constant value for data-reference of part", 0);

        // First figure the index inside the const value
        int i = 0;
        scope_entry_list_t* components = class_type_get_nonstatic_data_members(class_type);

        scope_entry_list_iterator_t* iter = NULL;
        for (iter = entry_list_iterator_begin(components);
                !entry_list_iterator_end(iter);
                entry_list_iterator_next(iter), i++)
        {
            scope_entry_t* current_member = entry_list_iterator_current(iter);
            if (current_member == entry)
            {
                break;
            }
        }
        entry_list_iterator_free(iter);

        ERROR_CONDITION((i == entry_list_size(components)), "This should not happen", 0);

        const_value_t* const_value_member = const_value_get_element_num(const_value, i);

        nodecl_set_constant(*nodecl_output, const_value_member);
    }
}

static void check_concat_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static char kind_is_integer_literal(const char* c)
{
    while (*c != '\0')
    {
        if (!isdigit(*c))
            return 0;
        c++;
    }
    return 1;
}

static int compute_kind_from_literal(const char* p, AST expr, decl_context_t decl_context)
{
    if (kind_is_integer_literal(p))
    {
        return atoi(p);
    }
    else
    {
        scope_entry_t* sym = fortran_get_variable_with_locus(decl_context, expr, p);
        if (sym == NULL
                || sym->kind != SK_VARIABLE
                || !is_const_qualified_type(no_ref(sym->type_information)))
        {
            if (!checking_ambiguity())
            {
                fprintf(stderr, "%s: invalid kind '%s'\n", 
                        ast_location(expr), 
                        p);
            }
            return 0;
        }

        ERROR_CONDITION(nodecl_is_null(sym->value),
                "Invalid constant for kind '%s'", sym->symbol_name);

        ERROR_CONDITION(!nodecl_is_constant(sym->value),
                "Invalid nonconstant expression for kind '%s'", 
                codegen_to_str(sym->value));

        return const_value_cast_to_4(nodecl_get_constant(sym->value));
    }
}

static void check_decimal_literal(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    const char* c = ASTText(expr);

    char decimal_text[strlen(c) + 1];
    memset(decimal_text, 0, sizeof(decimal_text));

    char *q = decimal_text;
    const char* p = c;

    while (*p != '\0'
            && *p != '_')
    {
        *q = *p;
        p++;
        q++;
    }

    int kind = 4;
    if (*p == '_')
    {
        p++;
        kind = compute_kind_from_literal(p, expr, decl_context);
        if (kind == 0)
        {
            *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
            return;
        }
    }

    long long int value = strtoll(decimal_text, NULL, 10);

    const_value_t* const_value = const_value_get_integer(value, kind, 1);
    nodecl_t nodecl_fake = nodecl_make_text(decimal_text, ASTFileName(expr), ASTLine(expr));
    type_t* t = choose_int_type_from_kind(nodecl_fake, 
            kind);


    *nodecl_output = nodecl_make_integer_literal(t, const_value, 
            ASTFileName(expr), ASTLine(expr));
}

static void check_derived_type_constructor(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST derived_type_spec = ASTSon0(expr);
    AST component_spec_list = ASTSon1(expr);

    AST type_param_spec_list = ASTSon1(derived_type_spec);
    if (type_param_spec_list != NULL)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: sorry: derived types with type parameters not supported\n", ast_location(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    AST derived_name = ASTSon0(derived_type_spec);
    scope_entry_t* entry = fortran_get_variable_with_locus(decl_context, derived_name, ASTText(derived_name));

    if (entry == NULL
            || entry->kind != SK_CLASS)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: '%s' is not a derived-type-name\n",
                    ast_location(expr),
                    ASTText(derived_name));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    char all_components_are_const = 1;

    scope_entry_list_t* nonstatic_data_members = class_type_get_nonstatic_data_members(entry->type_information);

    nodecl_t initialization_expressions[entry_list_size(nonstatic_data_members) + 1];
    memset(initialization_expressions, 0, sizeof(initialization_expressions));

    int member_index = 0;
    int component_position = 1;
    if (component_spec_list != NULL)
    {
        AST it;
        for_each_element(component_spec_list, it)
        {
            AST component_spec = ASTSon1(it);
            AST component_name = ASTSon0(component_spec);
            AST component_data_source = ASTSon1(component_spec);

            int current_member_index = 0;

            scope_entry_t* member = NULL;
            if (component_name == NULL)
            {
                if (member_index < 0)
                {
                    if (!checking_ambiguity())
                    {
                        error_printf("%s: error: component specifier at position %d lacks a component name", ast_location(component_spec),
                                component_position);
                    }
                    *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
                    return;
                }

                if (member_index > entry_list_size(nonstatic_data_members))
                {
                    if (!checking_ambiguity())
                    {
                        error_printf("%s: error: too many specifiers in derived-type constructor\n", ast_location(component_spec));
                    }
                    *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
                    return;
                }

                scope_entry_list_iterator_t* iter = entry_list_iterator_begin(nonstatic_data_members);
                int i;
                for (i = 0; i < member_index; i++)
                {
                    entry_list_iterator_next(iter);
                }
                member = entry_list_iterator_current(iter);

                entry_list_iterator_free(iter);

                current_member_index = member_index;

                member_index++;
            }
            else
            {
                decl_context_t class_context = class_type_get_inner_context(get_actual_class_type(entry->type_information));

                const char* field = ASTText(component_name);
                member = query_name_in_class(class_context, field);
                if (member == NULL)
                {
                    if (!checking_ambiguity())
                    {
                        error_printf("%s: error: component specifier '%s' is not a component of '%s'\n",
                                ast_location(expr),
                                field,
                                fortran_print_type_str(entry->type_information));
                    }
                    *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
                    return;
                }

                current_member_index = 0;
                scope_entry_list_iterator_t* iter = NULL;
                for (iter = entry_list_iterator_begin(nonstatic_data_members);
                        !entry_list_iterator_end(iter);
                        entry_list_iterator_next(iter), current_member_index++)
                {
                    scope_entry_t* current_member = entry_list_iterator_current(iter);
                    if (current_member == member)
                        break;
                }

                ERROR_CONDITION((current_member_index == entry_list_size(nonstatic_data_members)), "This should never happen", 0);

                member_index = -1;
            }

            if (!nodecl_is_null(initialization_expressions[current_member_index]))
            {
                error_printf("%s: error: component '%s' initialized more than once\n",
                        ast_location(expr),
                        member->symbol_name);
                *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
                return;
            }

            nodecl_t nodecl_expr = nodecl_null();
            fortran_check_expression_impl_(component_data_source, decl_context, &nodecl_expr);

            if (nodecl_is_err_expr(nodecl_expr))
            {
                *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
                return;
            }

            if (!nodecl_is_constant(nodecl_expr))
            {
                all_components_are_const = 0;
            }

            initialization_expressions[current_member_index] = nodecl_expr;

            component_position++;
        }
    }

    nodecl_t nodecl_initializer_list = nodecl_null();
    scope_entry_list_iterator_t* iter = NULL;

    // Now review components not initialized yet
    int i = 0;
    for (iter = entry_list_iterator_begin(nonstatic_data_members);
            !entry_list_iterator_end(iter);
            entry_list_iterator_next(iter), i++)
    {
        scope_entry_t* member = entry_list_iterator_current(iter);

        if (nodecl_is_null(initialization_expressions[i]))
        {
            if (nodecl_is_null(member->value))
            {
                error_printf("%s: error: component '%s' lacks an initializer\n",
                        ast_location(expr),
                        member->symbol_name);
                *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
                return;
            }
            else
            {
                // This should be const, shouldn't it?
                if (!nodecl_is_constant(member->value))
                {
                    all_components_are_const = 0;
                }

                initialization_expressions[i] = member->value;
            }
        }

        nodecl_initializer_list = nodecl_append_to_list(nodecl_initializer_list, 
                nodecl_make_field_designator(
                    nodecl_make_symbol(member, ASTFileName(expr), ASTLine(expr)),
                    initialization_expressions[i],
                    ASTFileName(expr),
                    ASTLine(expr)));
    }
    entry_list_iterator_free(iter);

    *nodecl_output = nodecl_make_structured_value(nodecl_initializer_list, 
            get_user_defined_type(entry), 
            ASTFileName(expr), ASTLine(expr));
    nodecl_set_symbol(*nodecl_output, entry);

    if (all_components_are_const)
    {
        const_value_t* items[nodecl_list_length(nodecl_initializer_list) + 1];
        memset(items, 0, sizeof(items));

        int num_items = entry_list_size(nonstatic_data_members);

        for (i = 0; i < num_items; i++)
        {
            items[i] = nodecl_get_constant(initialization_expressions[i]);
        }

        const_value_t* value = const_value_make_struct(num_items, items);
        nodecl_set_constant(*nodecl_output, value);
    }

    entry_list_free(nonstatic_data_members);
}

static void check_different_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_div_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_equal_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_floating_literal(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
   char* floating_text = strdup(strtolower(ASTText(expr)));

   unsigned int kind = 4;
   char *q = NULL; 
   if ((q = strchr(floating_text, '_')) != NULL)
   {
       *q = '\0';
       q++;
       kind = compute_kind_from_literal(q, expr, decl_context);
       if (kind == 0)
       {
           *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
           return;
       }
   }
   else if ((q = strchr(floating_text, 'd')) != NULL)
   {
       *q = 'e';
       kind = 8;
   }

    nodecl_t nodecl_fake = nodecl_make_text(floating_text, ASTFileName(expr), ASTLine(expr));
   type_t* t = choose_float_type_from_kind(nodecl_fake, kind);


   const_value_t *value = NULL;
   if (kind == (floating_type_get_info(get_float_type())->bits / 8))
   {
       float f = strtof(floating_text, NULL);
       value = const_value_get_float(f);
   }
   else if (kind == (floating_type_get_info(get_double_type())->bits / 8))
   {
       double d = strtod(floating_text, NULL);
       value = const_value_get_double(d);
   }
   else if (kind == (floating_type_get_info(get_long_double_type())->bits / 8))
   {
       long double ld = strtold(floating_text, NULL);
       value = const_value_get_long_double(ld);
   }
   else if (is_other_float_type(t))
   {
#ifdef HAVE_QUADMATH_H
       // __float128
       if (kind == 16)
       {
           __float128 f128 = strtoflt128(floating_text, NULL);
           value = const_value_get_float128(f128);
       }
       else
#endif
       {
           running_error("%s: error: literals of KIND=%d not supported yet\n", ast_location(expr), kind);
       }
   }
   else
   {
       running_error("Code unreachable, invalid floating literal", 0);
   }

   *nodecl_output = nodecl_make_floating_literal(t, value, ASTFileName(expr), ASTLine(expr));

   free(floating_text);
}

static char check_argument_association(
        scope_entry_t* function UNUSED_PARAMETER, 
        type_t* formal_type,
        type_t* real_type,
        nodecl_t real_argument,

        char ranks_must_agree,

        char diagnostic,
        int argument_num,
        const char* filename, int line)
{
    formal_type = no_ref(formal_type);
    real_type = no_ref(real_type);

    if (!equivalent_tk_types(formal_type, real_type))
    {
        if (!checking_ambiguity()
                && diagnostic)
        {
            error_printf("%s:%d: error: type or kind '%s' of actual argument %d does not agree type or kind '%s' of dummy argument\n",
                    filename, line,
                    fortran_print_type_str(real_type),
                    argument_num + 1,
                    fortran_print_type_str(formal_type));
        }
        return 0;
    }

    if (// If both types are pointers or ...
            ((is_pointer_type(formal_type)
              && is_pointer_type(real_type))
             // ... the dummy argument is an array requiring descriptor ...
             || (is_array_type(formal_type) 
                 && array_type_with_descriptor(formal_type))
             // Or we explicitly need ranks to agree
             || ranks_must_agree
             )
            // then their ranks should match
            && get_rank_of_type(formal_type) != get_rank_of_type(real_type))
    {
        if (!checking_ambiguity()
                && diagnostic)
        {
            error_printf("%s:%d: error: rank %d of actual argument %d does not agree rank %d of dummy argument\n",
                    filename, line,
                    get_rank_of_type(real_type),
                    argument_num + 1,
                    get_rank_of_type(formal_type));
        }
        return 0;
    }

    // If the actual argument is a scalar, ...
    if (!is_fortran_array_type(real_type))
    {
        // ... the dummy argument should be a scalar ...
        if (is_fortran_array_type(formal_type))
        {
            char ok = 0;
            // ... unless the actual argument is an element of an array ...
            if (nodecl_get_kind(real_argument) == NODECL_ARRAY_SUBSCRIPT)
            {
                ok = 1;
                // ... that is _not_ an assumed shape (an array requiring descriptor) or pointer array ...
                scope_entry_t* array = nodecl_get_symbol(nodecl_get_child(real_argument, 0));

                if (array != NULL)
                {
                    // ... or a substring of such element ...
                    if (is_fortran_character_type(no_ref(array->type_information)))
                    {
                        // The argument was X(1)(1:2), we are now in X(1)  get 'X'
                        if (nodecl_get_kind(nodecl_get_child(real_argument, 0)) == NODECL_ARRAY_SUBSCRIPT)
                        {
                            array = nodecl_get_symbol(
                                    nodecl_get_child(
                                        nodecl_get_child(real_argument, 0),
                                        0));
                        }
                        else
                        {
                            // This is just X(1:2) 
                            ok = 0;
                        }
                    }

                    if (ok
                            && array != NULL
                            && (array_type_with_descriptor(no_ref(array->type_information))
                                || is_pointer_to_fortran_array_type(no_ref(array->type_information))))
                    {
                        ok = 0;
                    }
                }
            }
            // Fortran 2003: or a default character
            else if (is_fortran_character_type(real_type)
                    && equivalent_types(get_unqualified_type(array_type_get_element_type(real_type)),
                        fortran_get_default_character_type()))
            {
                ok = 1;
            }

            if (!ok)
            {
                if (!checking_ambiguity()
                        && diagnostic)
                {
                    error_printf("%s:%d: error: scalar type '%s' of actual argument %d cannot be associated to non-scalar type '%s' of dummy argument\n",
                            filename, line,
                            fortran_print_type_str(real_type),
                            argument_num + 1,
                            fortran_print_type_str(formal_type));
                }
                return 0;
            }
        }
    }

    // Everything looks fine here
    return 1;
}

typedef
struct actual_argument_info_tag
{
    const char* keyword;
    type_t* type;
    char not_present;
    nodecl_t argument;
} actual_argument_info_t;

static scope_entry_t* get_specific_interface(scope_entry_t* symbol, 
        int num_arguments, 
        const char **keyword_names,
        nodecl_t* nodecl_actual_arguments)
{
    DEBUG_CODE()
    {
        fprintf(stderr, "EXPRTYPE: Getting specific interface of '%s' called with the following argument types\n",
                symbol->symbol_name);

        int i;
        for (i = 0; i < num_arguments; i++)
        {
            fprintf(stderr, "EXPRTYPE:    Name: %s\n", 
                    keyword_names[i] != NULL ? keyword_names[i] : "<<no-name>>");
            fprintf(stderr, "EXPRTYPE:    Argument: %s\n", 
                    fortran_print_type_str(nodecl_get_type(nodecl_actual_arguments[i])));
        }
    }

    scope_entry_t* result = NULL;
    int k;
    for (k = 0; k < symbol->entity_specs.num_related_symbols; k++)
    {
        scope_entry_t* specific_symbol = symbol->entity_specs.related_symbols[k];

        char ok = 1;

        // Complete with those arguments that are not present
        // Reorder the arguments
        actual_argument_info_t argument_types[MCXX_MAX_FUNCTION_CALL_ARGUMENTS];
        memset(argument_types, 0, sizeof(argument_types));

        DEBUG_CODE()
        {
            fprintf(stderr, "EXPRTYPE: Checking with specific interface %s:%d\n",
                    specific_symbol->file, 
                    specific_symbol->line);
            int i;
            for (i = 0; (i < num_arguments) && ok; i++)
            {
                type_t* formal_type = no_ref(function_type_get_parameter_type_num(specific_symbol->type_information, i));

                fprintf(stderr, "EXPRTYPE:    Name: %s\n", 
                       specific_symbol->entity_specs.related_symbols[i] != NULL ? 
                       specific_symbol->entity_specs.related_symbols[i]->symbol_name : 
                       "<<no-name>>");
                fprintf(stderr, "EXPRTYPE:    Parameter: %s\n", 
                        fortran_print_type_str(formal_type));
            }
        }

        int i;
        for (i = 0; (i < num_arguments) && ok; i++)
        {
            int position = -1;
            if (keyword_names[i] == NULL)
            {
                position = i;
            }
            else
            {
                int j;
                for (j = 0; j < specific_symbol->entity_specs.num_related_symbols; j++)
                {
                    scope_entry_t* related_sym = specific_symbol->entity_specs.related_symbols[j];

                    if (!related_sym->entity_specs.is_parameter)
                        continue;

                    if (strcasecmp(related_sym->symbol_name, keyword_names[i]) == 0)
                    {
                        position = related_sym->entity_specs.parameter_position;
                    }
                }
                if (position < 0)
                {
                    ok = 0;
                    break;
                }
            }
            if (argument_types[position].type != NULL)
            {
                ok = 0;
                break;
            }
            argument_types[position].type = nodecl_get_type(nodecl_actual_arguments[i]);
        }

        if (ok)
        {
            // Now complete with the optional ones
            for (i = 0; (i < specific_symbol->entity_specs.num_related_symbols) && ok; i++)
            {
                scope_entry_t* related_sym = specific_symbol->entity_specs.related_symbols[i];

                if (related_sym->entity_specs.is_parameter)
                {
                    if (argument_types[related_sym->entity_specs.parameter_position].type == NULL)
                    {
                        if (related_sym->entity_specs.is_optional)
                        {
                            argument_types[related_sym->entity_specs.parameter_position].type = related_sym->type_information;
                            argument_types[related_sym->entity_specs.parameter_position].not_present = 1;
                            num_arguments++;
                        }
                        else 
                        {
                            ok = 0;
                            break;
                        }
                    }
                }
            }
        }

        if (ok)
        {
            if (num_arguments != function_type_get_num_parameters(specific_symbol->type_information))
                ok = 0;
        }

        if (ok)
        {
            // Now check that every type matches, otherwise error
            for (i = 0; (i < num_arguments) && ok; i++)
            {
                type_t* formal_type = no_ref(function_type_get_parameter_type_num(specific_symbol->type_information, i));
                type_t* real_type = no_ref(argument_types[i].type);

                // Note that for ELEMENTAL some more checks should be done
                if (specific_symbol->entity_specs.is_elemental) 
                {
                    real_type = get_rank0_type(real_type);
                }

                if (!check_argument_association(
                            specific_symbol,
                            formal_type, 
                            real_type, 
                            argument_types[i].argument,

                            /* ranks_must_agree only if non-elemental */ !specific_symbol->entity_specs.is_elemental,

                            /* do_diagnostic */ 0,
                            /* argument_num */ i,
                            NULL, 0))
                {
                    ok = 0;
                    break;
                }
            }
        }

        if (ok)
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Current specifier DOES match\n");
            }
            if (result == NULL)
            {
                result = specific_symbol;
            }
            else
            {
                // More than one match, ambiguity detected
                DEBUG_CODE()
                {
                    fprintf(stderr, "EXPRTYPE: More than one generic specifier matched!\n");
                }
                return NULL;
            }
        }
        else
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "EXPRTYPE: Current specifier does NOT match\n");
            }
        }
    }

    DEBUG_CODE()
    {
        if (result != NULL)
        {
            fprintf(stderr, "EXPRTYPE: Specifier '%s' at '%s:%d' matches\n", 
                    result->symbol_name,
                    result->file,
                    result->line);
        }
    }

    return result;
}

static char inside_context_of_symbol(decl_context_t decl_context, scope_entry_t* entry)
{
    scope_t* sc = decl_context.current_scope;
    while (sc != NULL)
    {
        if (sc->related_entry == entry)
            return 1;
        sc = sc->contained_in;
    }
    return 0;
}


static void check_called_symbol(
        scope_entry_t* symbol, 
        decl_context_t decl_context, 
        AST location,
        AST procedure_designator,
        int num_actual_arguments,
        nodecl_t* nodecl_actual_arguments,
        const char** actual_arguments_keywords,
        char is_call_stmt,
        // out
        type_t** result_type,
        scope_entry_t** called_symbol,
        nodecl_t* nodecl_simplify)
{
    if (symbol == NULL
            || symbol->kind != SK_FUNCTION)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: in %s, '%s' does not designate a procedure\n",
                    ast_location(location),
                    !is_call_stmt ? "function reference" : "CALL statement",
                    fortran_prettyprint_in_buffer(procedure_designator));
        }
        *result_type = get_error_type();
        return;
    }

    if (inside_context_of_symbol(decl_context, symbol)
            && !symbol->entity_specs.is_recursive)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: cannot call recursively '%s'\n",
                    ast_location(location),
                    fortran_prettyprint_in_buffer(procedure_designator));
        }
        *result_type = get_error_type();
        return;
    }

    type_t* return_type = NULL; 
    // This is a generic procedure reference
    if (symbol->entity_specs.is_builtin
            && is_computed_function_type(symbol->type_information))
    {
        if (CURRENT_CONFIGURATION->disable_intrinsics)
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: call to intrinsic '%s' not implemented\n", 
                        ast_location(location),
                        strtoupper(symbol->symbol_name));
            }
            *result_type = get_error_type();
            return;
        }

        scope_entry_t* entry = fortran_intrinsic_solve_call(symbol, 
                actual_arguments_keywords,
                nodecl_actual_arguments, 
                num_actual_arguments, 
                nodecl_simplify);

        if (entry == NULL)
        {
            const char* actual_arguments_str = "(";

            int i;
            for (i = 0; i < num_actual_arguments; i++)
            {
                char c[256];
                snprintf(c, 255, "%s%s", i != 0 ? ", " : "", 
                        fortran_print_type_str(nodecl_get_type(nodecl_actual_arguments[i])));
                c[255] = '\0';

                actual_arguments_str = strappend(actual_arguments_str, c);
            }
            actual_arguments_str = strappend(actual_arguments_str, ")");
            
            if (!checking_ambiguity())
            {
                error_printf("%s: error: call to intrinsic %s%s failed\n", 
                        ast_location(location),
                        strtoupper(symbol->symbol_name),
                        actual_arguments_str);
            }
            *result_type = get_error_type();
            return;
        }

        if (!nodecl_is_null(*nodecl_simplify))
        {
            return_type = nodecl_get_type(*nodecl_simplify);
        }
        else if (entry->entity_specs.is_elemental)
        {
            // Try to come up with a common_rank
            int common_rank = -1;
            int i;
            for (i = 0; i < num_actual_arguments; i++)
            {
                int current_rank = get_rank_of_type(nodecl_get_type(nodecl_actual_arguments[i]));
                if (common_rank < 0)
                {
                    common_rank = current_rank;
                }
                else if (current_rank != common_rank
                        && current_rank != 0)
                {
                    common_rank = -1;
                    break;
                }
            }

            if (common_rank == 0)
            {
                return_type = function_type_get_return_type(entry->type_information);
            }
            else if (common_rank > 0)
            {
                return_type = get_n_ranked_type(
                        function_type_get_return_type(entry->type_information),
                        common_rank, decl_context);
            }
            else
            {
                if (!checking_ambiguity())
                {
                    error_printf("%s: error: mismatch of ranks in call to elemental intrinsic '%s'\n",
                            ast_location(location),
                            strtoupper(symbol->symbol_name));
                }

                *result_type = get_error_type();
                return;
            }
        }
        else
        {
            return_type = function_type_get_return_type(entry->type_information);
        }

        // We are calling the deduced intrinsic
        symbol = entry;
    }
    else
    {
        if (symbol->entity_specs.is_generic_spec)
        {
            scope_entry_t* specific_symbol = get_specific_interface(symbol, num_actual_arguments, 
                actual_arguments_keywords,
                nodecl_actual_arguments);
            if (specific_symbol == NULL)
            {
                if (!checking_ambiguity())
                {
                    error_printf("%s: error: no specific interface matches generic interface '%s' in function reference\n",
                            ast_location(location),
                            fortran_prettyprint_in_buffer(procedure_designator));
                }

                *result_type = get_error_type();
                return;
            }
            symbol = specific_symbol;
        }

        // This is now a specfic procedure reference
        ERROR_CONDITION (!is_function_type(symbol->type_information), "Invalid type for function symbol!\n", 0);

        // Complete with those arguments that are not present
        // Reorder the arguments
        actual_argument_info_t argument_info_items[MCXX_MAX_FUNCTION_CALL_ARGUMENTS];
        memset(argument_info_items, 0, sizeof(argument_info_items));

        int i;
        for (i = 0; i < num_actual_arguments; i++)
        {
            int position = -1;
            if (actual_arguments_keywords[i] == NULL)
            {
                position = i;
            }
            else
            {
                int j;
                for (j = 0; j < symbol->entity_specs.num_related_symbols; j++)
                {
                    scope_entry_t* related_sym = symbol->entity_specs.related_symbols[j];

                    if (!related_sym->entity_specs.is_parameter)
                        continue;

                    if (strcasecmp(related_sym->symbol_name, actual_arguments_keywords[i]) == 0)
                    {
                        position = related_sym->entity_specs.parameter_position;
                    }
                }
                if (position < 0)
                {
                    if (!checking_ambiguity())
                    {
                        error_printf("%s: error: keyword '%s' is not a dummy argument of function '%s'\n",
                                ast_location(location), 
                                actual_arguments_keywords[i],
                                symbol->symbol_name);
                    }
                    *result_type = get_error_type();
                    return;
                }
            }

            if (argument_info_items[position].type != NULL)
            {
                if (!checking_ambiguity())
                {
                    error_printf("%s: error: argument keyword '%s' specified more than once\n",
                            ast_location(location), actual_arguments_keywords[i]);
                }
                *result_type = get_error_type();
                return;
            }
            argument_info_items[position].type = nodecl_get_type(nodecl_actual_arguments[i]);
            argument_info_items[position].argument = nodecl_actual_arguments[i];
        }

        int num_completed_arguments = num_actual_arguments;

        // Now complete with the optional ones
        for (i = 0; i < symbol->entity_specs.num_related_symbols; i++)
        {
            scope_entry_t* related_sym = symbol->entity_specs.related_symbols[i];

            if (related_sym->entity_specs.is_parameter)
            {
                if (argument_info_items[related_sym->entity_specs.parameter_position].type == NULL)
                {
                    if (related_sym->entity_specs.is_optional)
                    {
                        argument_info_items[related_sym->entity_specs.parameter_position].type = related_sym->type_information;
                        argument_info_items[related_sym->entity_specs.parameter_position].not_present = 1;
                        num_completed_arguments++;
                    }
                    else 
                    {
                        if (!checking_ambiguity())
                        {
                            error_printf("%s: error: dummy argument '%s' of function '%s' has not been specified in function reference\n",
                                    ast_location(location),
                                    related_sym->symbol_name,
                                    symbol->symbol_name);
                        }
                        *result_type = get_error_type();
                        return;
                    }
                }
            }
        }

        if (!function_type_get_lacking_prototype(symbol->type_information) 
                && num_completed_arguments > function_type_get_num_parameters(symbol->type_information))
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: too many actual arguments in function reference to '%s'\n",
                        ast_location(location),
                        symbol->symbol_name);
            }
            *result_type = get_error_type();
            return;
        }

        ERROR_CONDITION(!function_type_get_lacking_prototype(symbol->type_information) 
                && (num_completed_arguments != function_type_get_num_parameters(symbol->type_information)), 
                "Mismatch between arguments and the type of the function %d != %d", 
                num_completed_arguments,
                function_type_get_num_parameters(symbol->type_information));

        char argument_type_mismatch = 0;
        int common_rank = -1;
        if (!function_type_get_lacking_prototype(symbol->type_information))
        {
            actual_argument_info_t fixed_argument_info_items[MCXX_MAX_FUNCTION_CALL_ARGUMENTS];
            memcpy(fixed_argument_info_items, argument_info_items, sizeof(fixed_argument_info_items));

            if (symbol->entity_specs.is_elemental)
            {
                // We may have to adjust the ranks, first check that all the
                // ranks match
                char ok = 1;
                for (i = 0; i < num_completed_arguments && ok; i++)
                {
                    int current_rank = get_rank_of_type(fixed_argument_info_items[i].type); 
                    if (common_rank < 0)
                    {
                        common_rank = current_rank;
                    }
                    else if ((common_rank != current_rank)
                            && current_rank != 0)
                    {
                        ok = 0;
                    }
                }

                if (ok)
                {
                    // Remove rank if they match, otherwise let it fail later
                    for (i = 0; i < num_completed_arguments && ok; i++)
                    {
                        fixed_argument_info_items[i].type = get_rank0_type(fixed_argument_info_items[i].type);
                    }
                }
            }

            for (i = 0; i < num_completed_arguments; i++)
            {
                type_t* formal_type = no_ref(function_type_get_parameter_type_num(symbol->type_information, i));
                type_t* real_type = no_ref(fixed_argument_info_items[i].type);

                if (is_pointer_type(formal_type)
                        && !fixed_argument_info_items[i].not_present)
                {
                    scope_entry_t* sym = nodecl_get_symbol(fixed_argument_info_items[i].argument);
                    if (sym != NULL 
                            && sym->kind == SK_VARIABLE
                            && is_pointer_type(sym->type_information))
                    {
                        ERROR_CONDITION(nodecl_get_kind(fixed_argument_info_items[i].argument) != NODECL_DERREFERENCE,
                                "Invalid pointer acess", 0);
                        real_type = no_ref(sym->type_information);
                    }
                }

                if (!check_argument_association(
                            symbol,
                            formal_type, 
                            real_type, 
                            fixed_argument_info_items[i].argument,

                            /* ranks_must_agree */ 0,

                            /* diagnostics */ 1,
                            /* argument_num */ i,
                            ASTFileName(location), ASTLine(location)))
                {
                    // if (!checking_ambiguity())
                    // {
                    //     error_printf("%s: error: type mismatch in argument %d between the "
                    //             "real argument %s and the dummy argument %s\n",
                    //             ast_location(location),
                    //             i + 1,
                    //             fortran_print_type_str(real_type),
                    //             fortran_print_type_str(formal_type));
                    // }
                    argument_type_mismatch = 1;
                }
            }
        }

        if (argument_type_mismatch)
        {
            *result_type = get_error_type();
            return;
        }

        return_type = function_type_get_return_type(symbol->type_information);

        if (symbol->entity_specs.is_elemental
                && !is_void_type(return_type))
        {
            if (common_rank > 0)
            {
                return_type = get_n_ranked_type(return_type, common_rank, decl_context);
            }
        }
    }

    if (is_void_type(return_type))
    {
        if (!is_call_stmt)
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: invalid function reference to a SUBROUTINE\n",
                        ast_location(location));
            }
            *result_type = get_error_type();
            return;
        }
    }
    else
    {
        if (is_call_stmt)
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: invalid CALL statement to a FUNCTION\n",
                        ast_location(location));
            }
            *result_type = get_error_type();
            return;
        }
    }

    *result_type = return_type;
    *called_symbol = symbol;
}

static void check_function_call(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    char is_call_stmt = (ASTText(expr) != NULL
            && (strcmp(ASTText(expr), "call") == 0));

    AST procedure_designator = ASTSon0(expr);
    AST actual_arg_spec_list = ASTSon1(expr);

    nodecl_t nodecl_proc_designator = nodecl_null();
    check_symbol_of_called_name(procedure_designator, decl_context, &nodecl_proc_designator, is_call_stmt);

    int num_actual_arguments = 0;

    nodecl_t nodecl_arguments[MCXX_MAX_FUNCTION_CALL_ARGUMENTS];
    memset(nodecl_arguments, 0, sizeof(nodecl_arguments));
    const char* argument_keywords[MCXX_MAX_FUNCTION_CALL_ARGUMENTS];
    memset(argument_keywords, 0, sizeof(argument_keywords));

    // Check arguments
    if (actual_arg_spec_list != NULL)
    {
        char with_keyword = 0;
        char wrong_arg_spec_list = 0;
        AST it;
        for_each_element(actual_arg_spec_list, it)
        {
            AST actual_arg_spec = ASTSon1(it);
            AST keyword = ASTSon0(actual_arg_spec);
            if (keyword != NULL)
            {
                with_keyword = 1;
                argument_keywords[num_actual_arguments] = ASTText(keyword);
            }
            else if (with_keyword) // keyword == NULL
            {
                error_printf("%s: error: in function call, '%s' argument requires a keyword\n",
                        ast_location(actual_arg_spec),
                        fortran_prettyprint_in_buffer(actual_arg_spec));
                return;
            }

            AST actual_arg = ASTSon1(actual_arg_spec);

            if (ASTType(actual_arg) != AST_ALTERNATE_RESULT_SPEC)
            {
                nodecl_t nodecl_argument = nodecl_null();

                // If the actual_arg is a symbol, we'll do a special checking
                // The reason: detect intrinsic functions in arguments
                if (ASTType(actual_arg) == AST_SYMBOL)
                {
                    check_symbol_of_argument(actual_arg, decl_context, &nodecl_argument);
                }
                else
                {
                    fortran_check_expression_impl_(actual_arg, decl_context, &nodecl_argument);
                }


                if (nodecl_is_err_expr(nodecl_argument))
                {
                    wrong_arg_spec_list = 1;
                }

                nodecl_arguments[num_actual_arguments] = nodecl_argument;
            }
            else
            {
                if (!is_call_stmt)
                {
                    if (!checking_ambiguity())
                    {
                        error_printf("%s: error: only CALL statement allows an alternate return\n",
                                ast_location(actual_arg_spec));
                    }
                    *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
                    return;
                }

                scope_entry_t* label = fortran_query_label(ASTSon0(actual_arg), 
                        decl_context, /* is_definition */ 0);

                nodecl_arguments[num_actual_arguments] = nodecl_make_fortran_alternate_return_argument(
                        label, get_void_type(),
                        ASTFileName(actual_arg), ASTLine(actual_arg));
            }

            num_actual_arguments++;
        }

        if (wrong_arg_spec_list)
        {
            *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
            return;
        }
    }

    if (nodecl_is_err_expr(nodecl_proc_designator))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    scope_entry_t* symbol = nodecl_get_symbol(nodecl_proc_designator);

    type_t* result_type = NULL;
    scope_entry_t* called_symbol = NULL;
    nodecl_t nodecl_simplify = nodecl_null();
    check_called_symbol(symbol, 
            decl_context, 
            expr, 
            procedure_designator, 
            num_actual_arguments,
            nodecl_arguments,
            argument_keywords,
            is_call_stmt,
            // out
            &result_type,
            &called_symbol,
            &nodecl_simplify
            );

    // ERROR_CONDITION(called_symbol == NULL, "Invalid symbol called returned by check_called_symbol", 0);
    ERROR_CONDITION(result_type == NULL, "Invalid type returned by check_called_symbol", 0);

    if (is_error_type(result_type))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    // Recheck arguments so we can associate them their dummy argument name
    // (if the function is not implicit)
    nodecl_t nodecl_argument_list = nodecl_null();
    if (actual_arg_spec_list != NULL)
    {
        int parameter_index = 0;
        AST it;
        for_each_element(actual_arg_spec_list, it)
        {
            AST actual_arg_spec = ASTSon1(it);

            AST keyword = ASTSon0(actual_arg_spec);
            AST actual_arg = ASTSon1(actual_arg_spec);

            nodecl_t nodecl_argument_spec = nodecl_null();
            if (ASTType(actual_arg) != AST_ALTERNATE_RESULT_SPEC)
            {
                char no_argument_info = 0;
                scope_entry_t* parameter = NULL;
                if (keyword == NULL)
                {
                    ERROR_CONDITION(parameter_index < 0, "Invalid index", 0);
                    if (called_symbol->entity_specs.num_related_symbols <= parameter_index)
                    {
                        no_argument_info = 1;
                    }
                    else
                    {
                        parameter = called_symbol->entity_specs.related_symbols[parameter_index];
                    }
                    parameter_index++;
                }
                else
                {
                    const char* param_name = ASTText(keyword);
                    int j;
                    for (j = 0; j < called_symbol->entity_specs.num_related_symbols; j++)
                    {
                        if (strcasecmp(called_symbol->entity_specs.related_symbols[j]->symbol_name, param_name) == 0)
                        {
                            parameter = called_symbol->entity_specs.related_symbols[j];
                            break;
                        }
                    }
                    parameter_index = -1;
                }

                nodecl_t nodecl_argument = nodecl_null();
                if (ASTType(actual_arg) == AST_SYMBOL) 
                {
                    check_symbol_of_argument(actual_arg, decl_context, &nodecl_argument);
                }
                else
                {
                    fortran_check_expression_impl_(actual_arg, decl_context, &nodecl_argument);
                }
                ERROR_CONDITION(nodecl_is_err_expr(nodecl_argument), "This should not have happened", 0);

                if (no_argument_info)
                {
                    nodecl_argument_spec = nodecl_argument;
                }
                else
                {
                    ERROR_CONDITION(parameter == NULL, "We did not find the parameter", 0);
                    nodecl_argument = fortran_nodecl_adjust_function_argument(
                            parameter->type_information,
                            nodecl_argument);

                    nodecl_argument_spec = nodecl_make_fortran_named_pair_spec(
                            nodecl_make_symbol(parameter, ASTFileName(actual_arg_spec), ASTLine(actual_arg_spec)),
                            nodecl_argument,
                            ASTFileName(actual_arg_spec), ASTLine(actual_arg_spec));
                }
            }
            else
            {
                scope_entry_t* label = fortran_query_label(ASTSon0(actual_arg),
                        decl_context, /* is_definition */ 0);

                nodecl_argument_spec = nodecl_make_fortran_alternate_return_argument(
                        label, get_void_type(),
                        ASTFileName(actual_arg), ASTLine(actual_arg));

                parameter_index++;
            }

            nodecl_argument_list = nodecl_append_to_list(nodecl_argument_list, nodecl_argument_spec);
        }
    }

    if (nodecl_is_null(nodecl_simplify))
    {
        *nodecl_output = nodecl_make_function_call(
                nodecl_make_symbol(called_symbol, ASTFileName(procedure_designator), ASTLine(procedure_designator)),
                nodecl_argument_list,
                result_type,
                ASTFileName(expr), ASTLine(expr));
    }
    else
    {
        *nodecl_output = nodecl_simplify;
    }
}

static void check_greater_or_equal_than(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_greater_than(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_hexadecimal_literal(AST expr, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    // We allow X and Z
    compute_boz_literal(expr, "xz", 16, nodecl_output);
}

static void check_image_ref(AST expr UNUSED_PARAMETER, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    error_printf("%s: sorry: image references not supported\n", 
            ast_location(expr));
    *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
}

static void check_logical_and(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_logical_equal(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_logical_different(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_logical_or(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_lower_or_equal_than(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_lower_than(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_minus_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_mult_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void check_neg_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_unary_check(expr, decl_context, nodecl_output);
}

static void check_not_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_unary_check(expr, decl_context, nodecl_output);
}

static void check_octal_literal(AST expr, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    compute_boz_literal(expr, "o", 8, nodecl_output);
}

static void check_parenthesized_expression(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_expr = nodecl_null();
    fortran_check_expression_impl_(ASTSon0(expr), decl_context, &nodecl_expr);


    *nodecl_output = nodecl_make_parenthesized_expression(
            nodecl_expr,
            nodecl_get_type(nodecl_expr),
            ASTFileName(expr), ASTLine(expr));

    if (nodecl_is_constant(nodecl_expr))
    {
        nodecl_set_constant(*nodecl_output, nodecl_get_constant(nodecl_expr));
    }
}

static void check_plus_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_unary_check(expr, decl_context, nodecl_output);
}

static void check_power_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    common_binary_check(expr, decl_context, nodecl_output);
}

static void common_binary_intrinsic_check(AST expr, decl_context_t, type_t* lhs_type, type_t* rhs_type, 
        nodecl_t nodecl_lhs, nodecl_t nodecl_rhs, nodecl_t* nodecl_output);
static void common_binary_check(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST lhs = ASTSon0(expr);
    AST rhs = ASTSon1(expr);
    nodecl_t nodecl_lhs = nodecl_null();
    fortran_check_expression_impl_(lhs, decl_context, &nodecl_lhs);
    nodecl_t nodecl_rhs = nodecl_null();
    fortran_check_expression_impl_(rhs, decl_context, &nodecl_rhs);

    type_t* lhs_type = nodecl_get_type(nodecl_lhs);
    type_t* rhs_type = nodecl_get_type(nodecl_rhs);

    if (nodecl_is_err_expr(nodecl_lhs)
            || nodecl_is_err_expr(nodecl_rhs))
    {
       *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr)); 
       return;
    }

    common_binary_intrinsic_check(expr, decl_context, lhs_type, rhs_type, nodecl_lhs, nodecl_rhs, nodecl_output);
}

static void common_binary_intrinsic_check(AST expr, decl_context_t decl_context, type_t* lhs_type, type_t* rhs_type,
        nodecl_t nodecl_lhs, nodecl_t nodecl_rhs, nodecl_t* nodecl_output)
{
    compute_result_of_intrinsic_operator(expr, decl_context, lhs_type, rhs_type, nodecl_lhs, nodecl_rhs, nodecl_output);
}

static void common_unary_intrinsic_check(AST expr, decl_context_t decl_context, type_t* rhs_type,
        nodecl_t nodecl_rhs, nodecl_t* nodecl_output);

static void common_unary_check(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output) 
{
    AST rhs = ASTSon0(expr);
    nodecl_t nodecl_expr = nodecl_null();
    fortran_check_expression_impl_(rhs, decl_context, &nodecl_expr);

    type_t* rhs_type = nodecl_get_type(nodecl_expr);

    if (nodecl_is_err_expr(nodecl_expr))
    {
       *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr)); 
       return;
    }

    common_unary_intrinsic_check(expr, decl_context, rhs_type, nodecl_expr, nodecl_output);

}

static void common_unary_intrinsic_check(AST expr, decl_context_t decl_context, type_t* rhs_type,
        nodecl_t nodecl_rhs, nodecl_t* nodecl_output)
{
    compute_result_of_intrinsic_operator(expr, decl_context, NULL, rhs_type, nodecl_null(), nodecl_rhs, nodecl_output);
}

static void check_string_literal(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    const char* literal = ASTText(expr);

    char kind[31] = { 0 };
    char has_kind = 0;

    if ((has_kind = (literal[0] != '"'
                    && literal[0] != '\'')))
    {
        char *q = kind;
        while (*literal != '_'
                && ((unsigned int)(q - kind) < (sizeof(kind) - 1)))
        {
            literal++;
        }
        if (*literal != '_')
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: KIND specifier is too long\n",
                        ast_location(expr));
            }
            *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
            return;
        }
        literal++;

        if (!checking_ambiguity())
        {
            warn_printf("%s: warning: ignoring KIND=%s of character-literal, assuming KIND=1\n",
                    kind,
                    ast_location(expr));
        }
    }

    int length = strlen(literal);

    char real_string[length + 1];
    int real_length = 0;
    char delim = literal[0];
    literal++; // Jump delim

    while (*literal != delim
                || *(literal+1) == delim)
    {
        ERROR_CONDITION(real_length >= (length+1), "Wrong construction of real string", 0);

        if (*literal !=  delim)
        {
            real_string[real_length] = *literal;
            literal++;
        }
        else 
        {
            real_string[real_length] = *literal;
            // Jump both '' or ""
            literal += 2;
        }

        real_length++;
    }
    real_string[real_length] = '\0';

    nodecl_t one = nodecl_make_integer_literal(
            fortran_get_default_logical_type(), 
            const_value_get_signed_int(1), 
            ASTFileName(expr),
            ASTLine(expr));
    nodecl_t length_tree = nodecl_make_integer_literal(fortran_get_default_logical_type(), 
            const_value_get_signed_int(real_length), 
            ASTFileName(expr),
            ASTLine(expr));

    type_t* t = get_array_type_bounds(fortran_get_default_character_type(), one, length_tree, decl_context);

    const_value_t* value = const_value_make_string(real_string, real_length);

    *nodecl_output = nodecl_make_string_literal(t, value, ASTFileName(expr), ASTLine(expr));
}

static void check_user_defined_unary_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // This is an AST_NAMED_PAIR_SPEC with no name. This way it is easier to
    // reuse common function call code
    AST operand = ASTSon1(expr);
    AST operand_expr = ASTSon1(operand);

    nodecl_t nodecl_expr = nodecl_null();
    fortran_check_expression_impl_(operand_expr, decl_context, &nodecl_expr);

    if (nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    AST operator = ASTSon0(expr);
    const char* operator_name = strtolower(strappend(".operator.", ASTText(operator)));
    scope_entry_t* call_sym = fortran_query_name_str(decl_context, operator_name);

    if (call_sym == NULL)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: unknown user-defined operator '%s'\n", ast_location(expr), ASTText(operator));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    int num_actual_arguments = 1;
    const char* keyword_arguments[1] = { NULL };
    nodecl_t nodecl_arguments[1] = { nodecl_expr };

    type_t* result_type = NULL;
    scope_entry_t* called_symbol = NULL;
    nodecl_t nodecl_simplify = nodecl_null();
    check_called_symbol(call_sym, 
            decl_context, 
            expr, 
            operator, 
            num_actual_arguments,
            nodecl_arguments,
            keyword_arguments,
            /* is_call_stmt */ 0,
            // out
            &result_type,
            &called_symbol,
            &nodecl_simplify);

    ERROR_CONDITION(result_type == NULL, "Invalid type returned by check_called_symbol", 0);

    if (is_error_type(result_type))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    *nodecl_output = nodecl_make_function_call(
            nodecl_make_symbol(called_symbol, ASTFileName(expr), ASTLine(expr)),
            nodecl_make_list_1(
                nodecl_make_fortran_named_pair_spec(nodecl_null(),
                    fortran_nodecl_adjust_function_argument(
                        function_type_get_parameter_type_num(called_symbol->type_information, 0),
                        nodecl_expr),
                    ASTFileName(operand_expr),
                    ASTLine(operand_expr))),
            result_type,
            ASTFileName(expr),
            ASTLine(expr));
}

static void check_user_defined_binary_op(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // This is an AST_NAMED_PAIR_SPEC with no name. This way it is easier to
    // reuse common function call code
    AST lhs = ASTSon1(expr);
    AST lhs_expr = ASTSon1(lhs);

    nodecl_t nodecl_lhs = nodecl_null();
    fortran_check_expression_impl_(lhs_expr, decl_context, &nodecl_lhs);

    if (nodecl_is_err_expr(nodecl_lhs))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    AST rhs = ASTSon2(expr);
    AST rhs_expr = ASTSon1(rhs);

    nodecl_t nodecl_rhs = nodecl_null();
    fortran_check_expression_impl_(rhs_expr, decl_context, &nodecl_rhs);

    if (nodecl_is_err_expr(nodecl_rhs))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    AST operator = ASTSon0(expr);
    const char* operator_name = strtolower(strappend(".operator.", ASTText(operator)));
    scope_entry_t* call_sym = fortran_get_variable_with_locus(decl_context, operator, operator_name);

    if (call_sym == NULL)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: unknown user-defined operator '%s'\n", ast_location(expr), ASTText(operator));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    int num_actual_arguments = 2;
    const char* keywords[2] = { NULL, NULL };
    nodecl_t nodecl_arguments[2] = { nodecl_lhs, nodecl_rhs };

    type_t* result_type = NULL;
    scope_entry_t* called_symbol = NULL;
    nodecl_t nodecl_simplify = nodecl_null();
    check_called_symbol(call_sym, 
            decl_context, 
            /* location */ expr, 
            operator, 
            num_actual_arguments,
            nodecl_arguments,
            keywords,
            /* is_call_stmt */ 0,
            // out
            &result_type,
            &called_symbol,
            &nodecl_simplify);

    ERROR_CONDITION(result_type == NULL, "Invalid type returned by check_called_symbol", 0);

    if (is_error_type(result_type))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    *nodecl_output = nodecl_make_function_call(
            nodecl_make_symbol(called_symbol, ASTFileName(expr), ASTLine(expr)),
            nodecl_make_list_2(
                nodecl_make_fortran_named_pair_spec(nodecl_null(),
                    fortran_nodecl_adjust_function_argument(
                        function_type_get_parameter_type_num(called_symbol->type_information, 0),
                        nodecl_lhs),
                    ASTFileName(lhs_expr),
                    ASTLine(lhs_expr)),
                nodecl_make_fortran_named_pair_spec(nodecl_null(),
                    fortran_nodecl_adjust_function_argument(
                        function_type_get_parameter_type_num(called_symbol->type_information, 1),
                        nodecl_rhs),
                    ASTFileName(rhs_expr),
                    ASTLine(rhs_expr))),
            result_type,
            ASTFileName(expr),
            ASTLine(expr));
}

#if 0
static char is_name_of_funtion_call(AST expr)
{
    return ASTParent(expr) != NULL
        && ASTType(ASTParent(expr)) == AST_FUNCTION_CALL;
}
#endif

static void check_symbol_of_called_name(AST sym, decl_context_t decl_context, nodecl_t* nodecl_output, char is_call_stmt)
{ 
    if (ASTType(sym) != AST_SYMBOL)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: expression is not a valid procedure designator\n", ast_location(sym));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(sym), ASTLine(sym));
        return;
    }

    // Look the symbol up. This will ignore INTRINSIC names
    scope_entry_t* entry = fortran_query_name_str(decl_context, ASTText(sym));
    if (entry == NULL)
    {
        char entry_is_an_intrinsic = 0;
        
        // We did not find anything.
        //
        // Does this name match the name of an INTRINSIC?
        entry = fortran_query_intrinsic_name_str(decl_context, ASTText(sym));
        if (entry != NULL)
        {
            // It names an intrinsic
            entry_is_an_intrinsic = 1; 

            // A call is only valid for builtin subroutines
            if (!!is_call_stmt != !!entry->entity_specs.is_builtin_subroutine)
            {
                entry_is_an_intrinsic = 0;
            }

            if (entry_is_an_intrinsic) 
            {
                // Inserting the intrinsic as an alias is okay here since there
                // is no doubt about the name being the INTRINSIC symbol
                insert_alias(decl_context.current_scope, entry, strtolower(ASTText(sym)));
            }
        }

        if (!entry_is_an_intrinsic) 
        {
            // Well, it does not name an intrinsic either (_or_ it does name
            // one but it does match its usage) (i.e. CALLing an INTRINSIC
            // FUNCTION)
            if (is_call_stmt)
            {
                // We did not find the symbol. But this is okay since this is a CALL.
                // CALL does not need a type, thus IMPLICIT plays no role here
                // Just sign in the symbol and give it an unprototyped type (= implicit interface)
                decl_context_t program_unit_context = decl_context.current_scope->related_entry->related_decl_context;
                entry = new_fortran_symbol(program_unit_context, ASTText(sym));
                entry->kind = SK_FUNCTION;
                entry->file = ASTFileName(sym);
                entry->line = ASTLine(sym);
                entry->type_information = get_nonproto_function_type(get_void_type(), 0);
                
                remove_unknown_kind_symbol(decl_context, entry);
            }
            else
            {
                if (is_implicit_none(decl_context))
                {
                    // This is not a CALL and we are under IMPLICIT NONE. Something is amiss
                    if (!checking_ambiguity())
                    {
                        error_printf("%s: error: '%s' is not a function name\n", ast_location(sym), ASTText(sym));
                    }
                    *nodecl_output = nodecl_make_err_expr(ASTFileName(sym), ASTLine(sym));
                    return;
                }
                else if (!is_implicit_none(decl_context))
                {
                    // This a new function brought to you by IMPLICIT after a function reference
                    entry = new_fortran_implicit_symbol(decl_context, sym, strtolower(ASTText(sym)));
                    entry->kind = SK_FUNCTION;
                    entry->type_information = 
                        get_nonproto_function_type(get_implicit_type_for_symbol(decl_context, entry->symbol_name), 0);
                    
                    remove_unknown_kind_symbol(decl_context, entry);
                }
            }

            // Do not allow its type be redefined anymore
            entry->entity_specs.is_implicit_basic_type = 0;
        }
    }
    else
    {
        if (entry->kind == SK_UNDEFINED)
        {
            // Make it a function
            entry->kind = SK_FUNCTION;
            remove_unknown_kind_symbol(decl_context, entry);
            
            // and update its type
            if (entry->entity_specs.alias_to != NULL
                    && entry->entity_specs.alias_to->entity_specs.is_builtin)
            {
                /*
                 * Heads up here!
                 *
                 * PROGRAM P
                 *   IMPLICIT NONE
                 *
                 *   CALL F1(SQRT)      !!! (1)
                 *   CALL F2(SQRT(1.2)) !!! (2)
                 * END PROGRAM P
                 *
                 * Initially in (1) we created an SK_UNDEFINED with an alias to the intrinsic SQRT because
                 * we were not 100% sure if this was going to be a variable or the called intrinsic. Then in (2)
                 * our suspicions get confirmed: SQRT was indeed an INTRINSIC, but we created a fake symbol
                 * which now we want it to behave like the intrinsic.
                 *
                 * Note 1. If (2) were removed, then SQRT usage is wrong. 
                 *
                 * Note 2. Nothing of this happens if SQRT is stated as an
                 * intrinsic using an INTRINSIC :: SQRT statement.
                 */

                remove_untyped_symbol(decl_context, entry);

                // This is a bit crude but will do since intrinsics are not meant to be changed elsewhere
                scope_entry_t* intrinsic_symbol = entry->entity_specs.alias_to;
                *entry = *intrinsic_symbol;
            }
            else
            {
                scope_entry_t * intrinsic_sym = fortran_query_intrinsic_name_str(decl_context, entry->symbol_name);
                if (intrinsic_sym == NULL || entry->entity_specs.is_parameter)
                {
                    // This is the usual case, when instead of SQRT the user wrote
                    // SRTQ (and we are not in IMPLICIT NONE)
                    entry->type_information = get_nonproto_function_type(entry->type_information, 0);
                }
                else 
                {
                    // From now, the symbol is an intrinsic
                    *entry = *intrinsic_sym;
                }
            }
        }

        if (entry->kind != SK_FUNCTION)
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: '%s' is not a %s name\n", ast_location(sym), entry->symbol_name,
                        is_call_stmt ? "subroutine" : "function");
            }
            *nodecl_output = nodecl_make_err_expr(ASTFileName(sym), ASTLine(sym));
            return;
        }

        // Generic specifiers are not to be handled here
        if (!is_computed_function_type(entry->type_information))
        {
            if (is_call_stmt
                    && entry->entity_specs.is_implicit_basic_type)
            {
                entry->type_information = get_nonproto_function_type(get_void_type(), 0);
            }
        }

        // This symbol is not untyped anymore
        remove_untyped_symbol(decl_context, entry);
        // nor its type can be redefined (this would never happen in real Fortran because of statement ordering)
        entry->entity_specs.is_implicit_basic_type = 0;
    }
    
    *nodecl_output = nodecl_make_symbol(entry, ASTFileName(sym), ASTLine(sym));
    nodecl_set_type(*nodecl_output, entry->type_information);
}

static void check_symbol_of_argument(AST sym, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Look the symbol up. This will ignore INTRINSIC names
    scope_entry_t* entry = fortran_query_name_str(decl_context, ASTText(sym));
    if (entry == NULL)
    {
        // We did not find anything.
        //
        // Does this name match the name of an INTRINSIC?
        entry = fortran_query_intrinsic_name_str(decl_context, ASTText(sym));

        if (entry != NULL)
        {
            scope_entry_t* original_intrinsic = entry;
            // It names an intrinsic
            if (!is_implicit_none(decl_context))
            {
                // If we are _not_ in an IMPLICIT NONE we just create a new
                // symbol. Later it might be promoted to a SK_VARIABLE or
                // SK_FUNCTION
                entry = new_fortran_implicit_symbol(decl_context, sym, ASTText(sym));
            }
            else
            {
                // Under IMPLICIT NONE we are just unsure about this,
                // so just remember the intrinsic
                //
                // See a long comment in check_symbol_of_called_name
                // explaining this case
                entry = new_fortran_symbol(decl_context, ASTText(sym));
                entry->file = ASTFileName(sym);
                entry->line = ASTLine(sym);
                entry->type_information = get_implicit_none_type();

                // This is actually an implicit none, so it is actually
                // something untyped!
                add_untyped_symbol(decl_context, entry);
            }

            // Remember the intrinsic we named
            entry->entity_specs.alias_to = original_intrinsic;
        }
        else
        {   
            // It is not an intrinsic
            if (!is_implicit_none(decl_context))
            {
                // If we are _not_ in IMPLICIT NONE then just create a
                // SK_UNDEFINED symbol. Later it might be promoted to a
                // SK_VARIABLE or SK_FUNCTION 
                entry = new_fortran_implicit_symbol(decl_context, sym, ASTText(sym));
            }
            else
            {
                // Under IMPLICIT NONE this means something is amiss
                if (!checking_ambiguity())
                {
                    error_printf("%s: error: Symbol '%s' has not IMPLICIT type\n", ast_location(sym),
                            fortran_prettyprint_in_buffer(sym));
                }
                *nodecl_output = nodecl_make_err_expr(ASTFileName(sym), ASTLine(sym));
                return;
            }
        }
    }
    
    if (entry == NULL || 
           (entry->kind != SK_VARIABLE &&
            entry->kind != SK_FUNCTION && 
            entry->kind != SK_UNDEFINED))
    {

        if (!checking_ambiguity())
        {
            error_printf("%s: error: '%s' cannot be an argument\n", ast_location(sym), entry->symbol_name);
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(sym), ASTLine(sym));
        return;
    }
    if (entry->kind == SK_VARIABLE)
    {
        // It might happen that dummy arguments/result do not have any implicit
        // type here (because the input code is wrong)
        if (is_error_type(entry->type_information))
        {
            // This error should have already been signaled elsewhere
            *nodecl_output = nodecl_make_err_expr(ASTFileName(sym), ASTLine(sym));
            return;
        }

        *nodecl_output = nodecl_make_symbol(entry, ASTFileName(sym), ASTLine(sym));
        nodecl_set_type(*nodecl_output, entry->type_information);

        if (is_const_qualified_type(no_ref(entry->type_information))
                && !nodecl_is_null(entry->value)
                && nodecl_is_constant(entry->value))
        {
            nodecl_set_constant(*nodecl_output, nodecl_get_constant(entry->value));
        }

        if (is_pointer_type(no_ref(entry->type_information)))
        {
            *nodecl_output = 
                nodecl_make_derreference(
                        *nodecl_output,
                        pointer_type_get_pointee_type(no_ref(entry->type_information)),
                        ASTFileName(sym), ASTLine(sym));
            nodecl_set_symbol(*nodecl_output, entry);
        }
    }
    else if (entry->kind == SK_UNDEFINED)
    {
        *nodecl_output = nodecl_make_symbol(entry, ASTFileName(sym), ASTLine(sym));
        nodecl_set_type(*nodecl_output, entry->type_information);
    }
    else if (entry->kind == SK_FUNCTION)
    {
        *nodecl_output = nodecl_make_symbol(entry, ASTFileName(sym), ASTLine(sym));
        nodecl_set_type(*nodecl_output, entry->type_information);
    }
    else
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(sym), ASTLine(sym));
    }
}

static void check_symbol_variable(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Entry will never be an intrinsic function
    scope_entry_t* entry = fortran_get_variable_with_locus(decl_context, expr, ASTText(expr));

    // When IMPLICIT NONE fortran_query_name_no_builtin_with_locus can return NULL
    if (entry == NULL)
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: unknown entity '%s'\n",
                    ast_location(expr),
                    fortran_prettyprint_in_buffer(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    if (entry == NULL || 
            (entry->kind != SK_VARIABLE 
             && entry->kind != SK_UNDEFINED))
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: code unreachable\n", ast_location(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    entry->kind = SK_VARIABLE;
    remove_unknown_kind_symbol(decl_context, entry);

    // It might happen that dummy arguments/result do not have any implicit
    // type here (because the input code is wrong)
    if (is_error_type(entry->type_information))
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: entity '%s' does not have any IMPLICIT type\n",
                    ast_location(expr),
                    fortran_prettyprint_in_buffer(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    *nodecl_output = nodecl_make_symbol(entry, ASTFileName(expr), ASTLine(expr));
    nodecl_set_type(*nodecl_output, entry->type_information);

    if (is_const_qualified_type(no_ref(entry->type_information))
            && !nodecl_is_null(entry->value)
            && nodecl_is_constant(entry->value))
    {
        nodecl_set_constant(*nodecl_output, nodecl_get_constant(entry->value));
    }

    if (is_pointer_type(no_ref(entry->type_information)))
    {
        *nodecl_output = 
            nodecl_make_derreference(
                    *nodecl_output,
                    pointer_type_get_pointee_type(no_ref(entry->type_information)),
                    ASTFileName(expr), ASTLine(expr));
        nodecl_set_symbol(*nodecl_output, entry);
    }
}

static void conform_types_in_assignment(type_t* lhs_type, type_t* rhs_type, type_t** conf_lhs_type, type_t** conf_rhs_type);

static char is_intrinsic_assignment(type_t* lvalue_type, type_t* rvalue_type)
{
    lvalue_type = no_ref(lvalue_type);
    rvalue_type = no_ref(rvalue_type);

    if (is_pointer_type(lvalue_type))
    {
        lvalue_type = pointer_type_get_pointee_type(lvalue_type);
    }
    if (is_pointer_type(rvalue_type))
    {
        rvalue_type = pointer_type_get_pointee_type(rvalue_type);
    }

    type_t* conf_lhs_type = NULL;
    type_t* conf_rhs_type = NULL;

    conform_types_in_assignment(lvalue_type, rvalue_type, &conf_lhs_type, &conf_rhs_type);

    if ((is_integer_type(conf_lhs_type)
                || is_floating_type(conf_lhs_type)
                || is_complex_type(conf_lhs_type))
            && (is_integer_type(conf_rhs_type)
                || is_floating_type(conf_rhs_type)
                || is_complex_type(conf_rhs_type)))
        return 1;

    if (is_fortran_character_type(conf_lhs_type)
            && is_fortran_character_type(conf_rhs_type)
            && equivalent_types(
                get_unqualified_type(array_type_get_element_type(conf_lhs_type)), 
                get_unqualified_type(array_type_get_element_type(conf_rhs_type)))) 
    {
        return 1;
    }

    if (is_bool_type(conf_lhs_type)
            && is_bool_type(conf_rhs_type))
        return 1;

    if (is_class_type(conf_lhs_type)
            && is_class_type(conf_rhs_type)
            && equivalent_types(
                get_unqualified_type(conf_lhs_type), 
                get_unqualified_type(conf_rhs_type)))
        return 1;

    return 0;
}

static char is_defined_assignment(AST expr, AST lvalue, 
        AST rvalue UNUSED_PARAMETER,
        nodecl_t nodecl_lvalue, nodecl_t nodecl_rvalue, 
        decl_context_t decl_context, scope_entry_t** entry)
{
    const char* operator_name = ".operator.=";
    scope_entry_t* call_sym = fortran_query_name_str(decl_context, operator_name);

    if (call_sym == NULL)
        return 0;

    int num_actual_arguments = 2;
    const char* keyword_names[2] = { NULL, NULL };
    nodecl_t nodecl_arguments[2] = { nodecl_lvalue, nodecl_rvalue };

    type_t* result_type = NULL;

    AST operator_designation = ASTLeaf(AST_SYMBOL, ast_get_filename(lvalue), ast_get_line(lvalue), "=");

    enter_test_expression();
    nodecl_t nodecl_simplify = nodecl_null();
    check_called_symbol(call_sym, 
            decl_context,
            /* location */ expr,
            operator_designation,
            num_actual_arguments,
            nodecl_arguments,
            keyword_names,
            /* is_call_stmt */ 1, // Assignments must be subroutines!
            // out
            &result_type,
            entry,
            &nodecl_simplify);
    leave_test_expression();

    ast_free(operator_designation);

    return !is_error_type(result_type);
}

static void check_assignment(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST lvalue = ASTSon0(expr);
    AST rvalue = ASTSon1(expr);
    
    nodecl_t nodecl_lvalue = nodecl_null();
    fortran_check_expression_impl_(lvalue, decl_context, &nodecl_lvalue);
    if (nodecl_is_err_expr(nodecl_lvalue))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }
    type_t* lvalue_type = nodecl_get_type(nodecl_lvalue);

    nodecl_t nodecl_rvalue = nodecl_null();
    fortran_check_expression_impl_(rvalue, decl_context, &nodecl_rvalue);
    if (nodecl_is_err_expr(nodecl_rvalue))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }
    type_t* rvalue_type = nodecl_get_type(nodecl_rvalue);
    
    scope_entry_t* assignment_op = NULL;
    char is_defined_assig = is_defined_assignment(expr, lvalue, rvalue, nodecl_lvalue, nodecl_rvalue, decl_context, &assignment_op);

    if (!is_defined_assig
            && !is_intrinsic_assignment(lvalue_type, rvalue_type))
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: cannot assign to a variable of type '%s' a value of type '%s'\n",
                    ast_location(expr),
                    fortran_print_type_str(lvalue_type),
                    fortran_print_type_str(rvalue_type));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }


    if (!is_defined_assig)
    {
        if (!equivalent_types(no_ref(lvalue_type), no_ref(rvalue_type)))
        {
            nodecl_rvalue = nodecl_make_conversion(nodecl_rvalue, 
                    lvalue_type,
                    nodecl_get_filename(nodecl_rvalue), nodecl_get_line(nodecl_rvalue));
        }
        *nodecl_output = nodecl_make_assignment(nodecl_lvalue, nodecl_rvalue, lvalue_type, ASTFileName(expr), ASTLine(expr));
    }
    else
    {
        *nodecl_output = nodecl_make_function_call(
                nodecl_make_symbol(assignment_op, ASTFileName(expr), ASTLine(expr)),
                nodecl_make_list_2(
                    nodecl_make_fortran_named_pair_spec(
                        nodecl_null(),
                        nodecl_lvalue,
                        ASTFileName(lvalue), ASTLine(lvalue)),
                    nodecl_make_fortran_named_pair_spec(
                        nodecl_null(),
                        nodecl_rvalue,
                        ASTFileName(rvalue), ASTLine(rvalue))),
                get_void_type(),
                ASTFileName(expr), ASTLine(expr));
    }

    if (nodecl_is_constant(nodecl_rvalue))
    {
        nodecl_set_constant(*nodecl_output, nodecl_get_constant(nodecl_rvalue));
    }
}

void fortran_check_initialization(
        scope_entry_t* entry,
        AST expr, 
        decl_context_t decl_context, 
        char is_pointer_initialization,
        nodecl_t* nodecl_output)
{
    char ok = 1;
    if (entry->entity_specs.is_parameter)
    {
        error_printf("%s: error: a dummy argument cannot have initializer\n", 
                ast_location(expr));
        ok = 0;
    }

    if (!ok)
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    fortran_check_expression(expr, decl_context, nodecl_output);

    if (nodecl_is_err_expr(*nodecl_output))
        return;

    if (is_pointer_initialization)
    {
        // Just check that is => NULL()
        scope_entry_t* function_called = NULL;
        if (nodecl_get_kind(*nodecl_output) != NODECL_FUNCTION_CALL
                || ((function_called = nodecl_get_symbol(nodecl_get_child(*nodecl_output, 0))) == NULL)
                || strcasecmp(function_called->symbol_name, "null") != 0
                || !function_called->entity_specs.is_builtin)
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: pointer initializer of '%s' is not '=> NULL()'",
                        ast_location(expr),
                        entry->symbol_name);
            }
            *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
            return;
        }
    }
    else
    {
        // Check if the initializer is valid
        if (!is_intrinsic_assignment(entry->type_information, 
                    nodecl_get_type(*nodecl_output)))
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: initializer '%s' of type '%s' is not valid to initialize '%s' of type '%s'\n",
                        ast_location(expr),
                        codegen_to_str(*nodecl_output),
                        fortran_print_type_str(nodecl_get_type(*nodecl_output)),
                        entry->symbol_name,
                        fortran_print_type_str(entry->type_information));
            }
            *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
            return;
        }

        if (!nodecl_is_constant(*nodecl_output))
        {
            if (!checking_ambiguity())
            {
                error_printf("%s: error: initializer '%s' is not a constant expression\n",
                        ast_location(expr),
                        codegen_to_str(*nodecl_output));
            }
            *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
            return;
        }
    }
}


static void check_ptr_assignment(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST lvalue = ASTSon0(expr);
    AST rvalue = ASTSon1(expr);

    nodecl_t nodecl_lvalue = nodecl_null();
    fortran_check_expression_impl_(lvalue, decl_context, &nodecl_lvalue);

    if (nodecl_is_err_expr(nodecl_lvalue))
    {
        *nodecl_output = nodecl_lvalue;
        return;
    }

    nodecl_t nodecl_rvalue = nodecl_null();
    fortran_check_expression_impl_(rvalue, decl_context, &nodecl_rvalue);

    if (nodecl_is_err_expr(nodecl_rvalue))
    {
        *nodecl_output = nodecl_rvalue;
        return;
    }

    type_t* rvalue_type = nodecl_get_type(nodecl_rvalue);
    if (is_error_type(rvalue_type))
    {
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    scope_entry_t* lvalue_sym = NULL;
    if (nodecl_get_symbol(nodecl_lvalue) != NULL)
    {
        lvalue_sym = nodecl_get_symbol(nodecl_lvalue);
    }
    if (lvalue_sym == NULL
            || lvalue_sym->kind != SK_VARIABLE
            || !is_pointer_type(no_ref(lvalue_sym->type_information)))
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: left hand of pointer assignment is not a pointer data-reference\n",
                    ast_location(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    scope_entry_t* rvalue_sym = NULL;
    if (nodecl_get_symbol(nodecl_rvalue) != NULL)
    {
        rvalue_sym = nodecl_get_symbol(nodecl_rvalue);
    }
    if (rvalue_sym == NULL
            || rvalue_sym->kind != SK_VARIABLE
            || (!is_pointer_type(no_ref(rvalue_sym->type_information)) &&
                !rvalue_sym->entity_specs.is_target))
    {
        if (!checking_ambiguity())
        {
            error_printf("%s: error: right hand of pointer assignment is not a POINTER or TARGET data-reference\n",
                    ast_location(expr));
        }
        *nodecl_output = nodecl_make_err_expr(ASTFileName(expr), ASTLine(expr));
        return;
    }

    if (is_pointer_type(no_ref(rvalue_sym->type_information)))
    {
        ERROR_CONDITION(nodecl_get_kind(nodecl_rvalue) != NODECL_DERREFERENCE,
                "References to pointers must be derreferenced!", 0);
        // Get the inner part of the derreference
        nodecl_rvalue = nodecl_get_child(nodecl_rvalue, 0);
    }
    else
    {
        // Build a reference here
        nodecl_rvalue = nodecl_make_reference(nodecl_rvalue,
                get_pointer_type(no_ref(rvalue_sym->type_information)),
                nodecl_get_filename(nodecl_rvalue),
                nodecl_get_line(nodecl_rvalue));
    }

    ERROR_CONDITION(nodecl_get_kind(nodecl_lvalue) != NODECL_DERREFERENCE, 
            "A reference to a pointer entity must be derreferenced", 0);

    // Get the inner part of the derreference
    nodecl_lvalue = nodecl_get_child(nodecl_lvalue, 0);

    *nodecl_output = nodecl_make_assignment(
            nodecl_lvalue,
            nodecl_rvalue,
            no_ref(lvalue_sym->type_information),
            ASTFileName(expr),
            ASTLine(expr));
}

static void disambiguate_expression(AST expr, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    int num_ambig = ast_get_num_ambiguities(expr);

    int i;
    int correct_option = -1;
    int function_call = -1;
    for (i = 0; i < num_ambig; i++)
    {
        AST current_expr = ast_get_ambiguity(expr, i);

        nodecl_t nodecl_check_expr = nodecl_null();
        switch (ASTType(current_expr))
        {
            case AST_FUNCTION_CALL:
            case AST_ARRAY_SUBSCRIPT:
            case AST_DERIVED_TYPE_CONSTRUCTOR:
                {
                    enter_test_expression();
                    fortran_check_expression_impl_(current_expr, decl_context, &nodecl_check_expr);
                    leave_test_expression();

                    if (ASTType(current_expr) == AST_FUNCTION_CALL)
                    {
                        function_call = i;
                    }
                    break;
                }
            default:
                {
                    internal_error("%s: unexpected node '%s'\n", 
                            ast_location(expr),
                            ast_print_node_type(ASTType(expr)));
                    break;
                }
        }

        if (!nodecl_is_err_expr(nodecl_check_expr))
        {
            if (correct_option < 0)
            {
                correct_option = i;
            }
            else
            {
                internal_error("%s: more than one interpretation valid for '%s'\n",
                        fortran_prettyprint_in_buffer(current_expr));
            }
        }
    }

    if (correct_option < 0)
    {
        ERROR_CONDITION(function_call < 0, "Invalid ambiguity", 0);
        // Default to function call as a fallback
        ast_replace_with_ambiguity(expr, function_call);
    }
    else
    {
        ast_replace_with_ambiguity(expr, correct_option);
    }

    // We want the diagnostics again
    fortran_check_expression_impl_(expr, decl_context, nodecl_output);
}

static type_t* common_kind(type_t* t1, type_t* t2)
{
    t1 = no_ref(t1);
    t2 = no_ref(t2);

    ERROR_CONDITION(!is_scalar_type(t1)
            || !is_scalar_type(t2), "One of the types is not scalar", 0);

    _size_t s1 = type_get_size(t1);
    _size_t s2 = type_get_size(t2);

    if (s1 > s2)
        return t1;
    else 
        return t2;
}

static type_t* first_type(type_t* t1, type_t* t2 UNUSED_PARAMETER)
{
    return t1;
}

static type_t* second_type(type_t* t1 UNUSED_PARAMETER, type_t* t2)
{
    return t2;
}

static type_t* combine_character_array(type_t* t1, type_t* t2)
{
    t1 = no_ref(t1);
    t2 = no_ref(t2);

    if (is_pointer_to_fortran_character_type(t1))
        t1 = pointer_type_get_pointee_type(t1);
    if (is_pointer_to_fortran_character_type(t2))
        t1 = pointer_type_get_pointee_type(t2);

    nodecl_t length1 = array_type_get_array_size_expr(t1);
    nodecl_t length2 = array_type_get_array_size_expr(t2);

    type_t* char1 = array_type_get_element_type(t1);
    type_t* char2 = array_type_get_element_type(t2);

    if (!equivalent_types(get_unqualified_type(char1), get_unqualified_type(char2)))
        return NULL;

    type_t* result = NULL;
    if (!nodecl_is_null(length1)
            && !nodecl_is_null(length2))
    {
        nodecl_t lower = nodecl_make_integer_literal(
                fortran_get_default_logical_type(), 
                const_value_get_signed_int(1), 
                NULL, 0);
        nodecl_t upper = nodecl_null();
        if (nodecl_is_constant(length1) 
                && nodecl_is_constant(length2))
        {
            upper = const_value_to_nodecl(
                    const_value_add(nodecl_get_constant(length1),
                        nodecl_get_constant(length2)));
        }
        else
        {
            upper = nodecl_make_add(
                    nodecl_copy(length1),
                    nodecl_copy(length2),
                    fortran_get_default_logical_type(),
                    NULL, 0);
        }

        result = get_array_type_bounds(char1, 
                lower, 
                upper, 
                array_type_get_array_size_expr_context(t1));
    }
    else
    {
        result = get_array_type(char1, nodecl_null(), array_type_get_array_size_expr_context(t1));
    }

    return result;
}

static type_t* logical_type(type_t* t1 UNUSED_PARAMETER, type_t* t2 UNUSED_PARAMETER)
{
    return fortran_get_default_logical_type();
}

static char is_logical_type(type_t* t1)
{
    t1 = no_ref(t1);
    return is_bool_type(t1);
}

typedef
struct operand_types_tag
{
    char (*lhs_type)(type_t*);
    char (*rhs_type)(type_t*);
    type_t* (*common_type)(type_t*, type_t*);
    char convert_to_common;
} operand_types_t;

enum { DO_CONVERT_TO_RESULT = 1, DO_NOT_CONVERT_TO_RESULT = 0};

static operand_types_t arithmetic_unary[] = 
{
    { NULL, is_integer_type, second_type, DO_NOT_CONVERT_TO_RESULT },
    { NULL, is_floating_type, second_type, DO_NOT_CONVERT_TO_RESULT },
    { NULL, is_complex_type, second_type, DO_NOT_CONVERT_TO_RESULT },
};

static operand_types_t arithmetic_binary[] =
{
    { is_integer_type, is_integer_type, common_kind, DO_CONVERT_TO_RESULT },
    { is_integer_type, is_floating_type,  second_type, DO_CONVERT_TO_RESULT },
    { is_integer_type, is_complex_type, second_type, DO_CONVERT_TO_RESULT },
    { is_floating_type, is_integer_type, first_type, DO_CONVERT_TO_RESULT },
    { is_floating_type, is_floating_type, common_kind, DO_CONVERT_TO_RESULT },
    { is_floating_type, is_complex_type, second_type, DO_CONVERT_TO_RESULT },
    { is_complex_type, is_integer_type, first_type, DO_CONVERT_TO_RESULT },
    { is_complex_type, is_floating_type, first_type, DO_CONVERT_TO_RESULT },
    { is_complex_type, is_complex_type, common_kind, DO_CONVERT_TO_RESULT },
};

static operand_types_t arithmetic_binary_power[] =
{
    { is_integer_type, is_integer_type, common_kind, DO_NOT_CONVERT_TO_RESULT },
    { is_integer_type, is_floating_type,  second_type, DO_NOT_CONVERT_TO_RESULT },
    { is_integer_type, is_complex_type, second_type, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_integer_type, first_type, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_floating_type, common_kind, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_complex_type, second_type, DO_NOT_CONVERT_TO_RESULT },
    { is_complex_type, is_integer_type, first_type, DO_NOT_CONVERT_TO_RESULT },
    { is_complex_type, is_floating_type, first_type, DO_NOT_CONVERT_TO_RESULT },
    { is_complex_type, is_complex_type, common_kind, DO_NOT_CONVERT_TO_RESULT },
};


static operand_types_t concat_op[] = 
{
    { is_fortran_character_type_or_pointer_to, is_fortran_character_type_or_pointer_to, combine_character_array, DO_NOT_CONVERT_TO_RESULT },
};

static operand_types_t relational_equality[] =
{
    { is_integer_type, is_integer_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_integer_type, is_floating_type,  logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_integer_type, is_complex_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_integer_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_floating_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_complex_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_complex_type, is_integer_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_complex_type, is_floating_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_complex_type, is_complex_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_fortran_character_type, is_fortran_character_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
};

static operand_types_t relational_weak[] =
{
    { is_integer_type, is_integer_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_integer_type, is_floating_type,  logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_integer_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_floating_type, is_floating_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
    { is_fortran_character_type, is_fortran_character_type, logical_type, DO_NOT_CONVERT_TO_RESULT },
};

static operand_types_t logical_unary[] =
{
    { NULL, is_logical_type, second_type, DO_NOT_CONVERT_TO_RESULT },
};

static operand_types_t logical_binary[] =
{
    { is_logical_type, is_logical_type, common_kind, DO_NOT_CONVERT_TO_RESULT }
};

typedef struct operand_map_tag
{
    node_t node_type;
    operand_types_t* operand_types;
    int num_operands;

    const_value_t* (*compute_const)(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);

    const char* op_symbol_name;

    nodecl_t (*compute_nodecl)(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs, type_t* t, const char* filename, int line);
} operand_map_t;

#define HANDLER_MAP(_node_op, _operands, _compute_const, _operator_symbol_name, _nodecl_fun) \
{ _node_op, _operands, sizeof(_operands) / sizeof(_operands[0]), _compute_const, _operator_symbol_name, _nodecl_fun }

static const_value_t* const_unary_plus(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_unary_neg(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_add(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_sub(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_mult(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_div(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_power(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_equal(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_not_equal(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_lt(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_lte(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_gt(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_gte(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_unary_not(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_and(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_or(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);
static const_value_t* const_bin_concat(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs);

#define NODECL_FUN_2BIN(x) binary_##x

#define NODECL_FUN_2BIN_DEF(x) \
static nodecl_t binary_##x(nodecl_t nodecl_lhs UNUSED_PARAMETER, nodecl_t nodecl_rhs, type_t* t, const char* filename, int line) \
{ \
    return x(nodecl_rhs, t, filename, line); \
}

NODECL_FUN_2BIN_DEF(nodecl_make_plus)
NODECL_FUN_2BIN_DEF(nodecl_make_neg)
NODECL_FUN_2BIN_DEF(nodecl_make_logical_not)

static operand_map_t operand_map[] =
{
    // Arithmetic unary
    HANDLER_MAP(AST_PLUS, arithmetic_unary, const_unary_plus, ".operator.+", NODECL_FUN_2BIN(nodecl_make_plus)),
    HANDLER_MAP(AST_NEG, arithmetic_unary, const_unary_neg, ".operator.-", NODECL_FUN_2BIN(nodecl_make_neg)),
    // Arithmetic binary
    HANDLER_MAP(AST_ADD, arithmetic_binary, const_bin_add, ".operator.+", nodecl_make_add),
    HANDLER_MAP(AST_MINUS, arithmetic_binary, const_bin_sub, ".operator.-", nodecl_make_minus),
    HANDLER_MAP(AST_MUL, arithmetic_binary, const_bin_mult, ".operator.*", nodecl_make_mul),
    HANDLER_MAP(AST_DIV, arithmetic_binary, const_bin_div, ".operator./", nodecl_make_div),
    HANDLER_MAP(AST_POWER, arithmetic_binary_power, const_bin_power, ".operator.**", nodecl_make_power),
    // String concat
    HANDLER_MAP(AST_CONCAT, concat_op, const_bin_concat, ".operator.//", nodecl_make_concat),
    // Relational strong
    HANDLER_MAP(AST_EQUAL, relational_equality, const_bin_equal, ".operator.==", nodecl_make_equal),
    HANDLER_MAP(AST_DIFFERENT, relational_equality, const_bin_not_equal, ".operator./=", nodecl_make_different),
    // Relational weak
    HANDLER_MAP(AST_LOWER_THAN, relational_weak, const_bin_lt, ".operator.<", nodecl_make_lower_than),
    HANDLER_MAP(AST_LOWER_OR_EQUAL_THAN, relational_weak, const_bin_lte, ".operator.<=", nodecl_make_lower_or_equal_than),
    HANDLER_MAP(AST_GREATER_THAN, relational_weak, const_bin_gt, ".operator.>", nodecl_make_greater_than),
    HANDLER_MAP(AST_GREATER_OR_EQUAL_THAN, relational_weak, const_bin_gte, ".operator.>=", nodecl_make_greater_or_equal_than),
    // Unary logical
    HANDLER_MAP(AST_LOGICAL_NOT, logical_unary, const_unary_not, ".operator..not.", NODECL_FUN_2BIN(nodecl_make_logical_not)),
    // Binary logical
    HANDLER_MAP(AST_LOGICAL_EQUAL, logical_binary, const_bin_equal, ".operator..eqv.", nodecl_make_equal),
    HANDLER_MAP(AST_LOGICAL_DIFFERENT, logical_binary, const_bin_not_equal, ".operator..neqv.", nodecl_make_different),
    HANDLER_MAP(AST_LOGICAL_AND, logical_binary, const_bin_and, ".operator..and.", nodecl_make_logical_and),
    HANDLER_MAP(AST_LOGICAL_OR, logical_binary, const_bin_or, ".operator..or.", nodecl_make_logical_or),
};
static char operand_map_init = 0;

static int compare_map_items(const void* a, const void* b)
{
    const operand_map_t* pa = (const operand_map_t*)a;
    const operand_map_t* pb = (const operand_map_t*)b;

    if (pa->node_type > pb->node_type)
        return 1;
    else if (pa->node_type < pb->node_type)
        return -1;
    else
        return 0;
}

static const char * get_operator_for_expr(AST expr);

static type_t* rerank_type(type_t* rank0_common, type_t* lhs_type, type_t* rhs_type);

static void conform_types(type_t* lhs_type, type_t* rhs_type, 
        type_t** conf_lhs_type, type_t** conf_rhs_type);

static type_t* compute_result_of_intrinsic_operator(AST expr, decl_context_t decl_context, 
        type_t* lhs_type, 
        type_t* rhs_type,
        nodecl_t nodecl_lhs,
        nodecl_t nodecl_rhs,
        nodecl_t* nodecl_output)
{
    lhs_type = no_ref(lhs_type);
    rhs_type = no_ref(rhs_type);

    // Remove pointer, which is actually only used for data refs
    if (is_pointer_type(lhs_type))
        lhs_type = pointer_type_get_pointee_type(lhs_type);
    if (is_pointer_type(rhs_type))
        rhs_type = pointer_type_get_pointee_type(rhs_type);

    type_t* conf_lhs_type = NULL;
    type_t* conf_rhs_type = NULL;

    conform_types(lhs_type, rhs_type, &conf_lhs_type, &conf_rhs_type);

    if (!operand_map_init)
    {
        qsort(operand_map, 
                sizeof(operand_map) / sizeof(operand_map[0]), 
                sizeof(operand_map[0]),
                compare_map_items);

        operand_map_init = 1;
    }

    operand_map_t key = { .node_type = ASTType(expr) };
    operand_map_t* value = (operand_map_t*)bsearch(&key, operand_map,
                sizeof(operand_map) / sizeof(operand_map[0]), 
                sizeof(operand_map[0]),
                compare_map_items);

    if (value == NULL)
    {
        internal_error("%s: unexpected expression '%s' for intrinsic operation\n", 
                ast_location(expr),
                fortran_prettyprint_in_buffer(expr));
    }

    type_t* result = NULL;
    char convert_to_common = 0;

    operand_types_t* operand_types = value->operand_types;
    int i;
    for (i = 0; i < value->num_operands && result == NULL; i++)
    {
        if (((lhs_type == NULL 
                        && operand_types[i].lhs_type == NULL)
                    || ((operand_types[i].lhs_type)(conf_lhs_type)))
                && ((operand_types[i].rhs_type)(conf_rhs_type)))
        {
            result = (operand_types[i].common_type)(conf_lhs_type, conf_rhs_type);
            convert_to_common = (operand_types[i].convert_to_common == DO_CONVERT_TO_RESULT);
            break;
        }
    }

    if (result == NULL)
    {
        result = get_error_type();
        // Now try with a user defined operator
        scope_entry_t* call_sym = fortran_get_variable_with_locus(decl_context, expr, value->op_symbol_name);

        // Perform a resolution by means of a call check
        if (call_sym != NULL)
        {
            int num_actual_arguments = 0;
            const char* keywords[2] = { NULL, NULL };
            nodecl_t nodecl_arguments[2] = { nodecl_null(), nodecl_null() };
            if (lhs_type == NULL)
            {
                num_actual_arguments = 1;
                nodecl_arguments[0] = nodecl_rhs;
            }
            else
            {
                num_actual_arguments = 2;
                nodecl_arguments[0] = nodecl_lhs;
                nodecl_arguments[1] = nodecl_rhs;
            }

            AST operator_designation = ASTLeaf(AST_SYMBOL, ast_get_filename(expr), ast_get_line(expr), get_operator_for_expr(expr));

            scope_entry_t* called_symbol = NULL;
            nodecl_t nodecl_simplify = nodecl_null();
            check_called_symbol(call_sym, 
                    decl_context, 
                    /* location */ expr, 
                    operator_designation,
                    num_actual_arguments,
                    nodecl_arguments,
                    keywords,
                    /* is_call_stmt */ 0,
                    // out
                    &result,
                    &called_symbol,
                    &nodecl_simplify);

            ast_free(operator_designation);

            // Restore the rank of the common type
            if (!is_error_type(result))
            {
                result = rerank_type(result, lhs_type, rhs_type);

                nodecl_t nodecl_argument_list = nodecl_null();

                if (nodecl_is_null(nodecl_lhs))
                {
                    nodecl_argument_list = nodecl_make_list_1(nodecl_rhs);
                }
                else
                {
                    nodecl_argument_list = nodecl_make_list_2(nodecl_rhs, nodecl_lhs);
                }

                *nodecl_output = nodecl_make_function_call(
                        nodecl_make_symbol(call_sym, ast_get_filename(expr), ast_get_line(expr)),
                        nodecl_argument_list,
                        result,
                        ASTFileName(expr), ASTLine(expr));

                if (nodecl_is_null(nodecl_simplify)
                        && nodecl_is_constant(nodecl_simplify))
                {
                    nodecl_set_constant(*nodecl_output, nodecl_get_constant(nodecl_simplify));
                }
            }
        }

        if (is_error_type(result))
        {
            if (lhs_type != NULL)
            {
                if (!checking_ambiguity())
                {
                    error_printf("%s: error: invalid operand types %s and %s for intrinsic binary operator '%s'\n",
                            ast_location(expr),
                            fortran_print_type_str(lhs_type),
                            fortran_print_type_str(rhs_type),
                            get_operator_for_expr(expr));
                }
                return get_error_type();
            }
            else
            {
                if (!checking_ambiguity())
                {
                    error_printf("%s: error: invalid operand types %s for intrinsic unary operator '%s'\n",
                            ast_location(expr),
                            fortran_print_type_str(rhs_type),
                            get_operator_for_expr(expr));
                }
                return get_error_type();
            }
        }
        
    }
    else
    {
        // Restore the rank of the common type
        result = rerank_type(result, lhs_type, rhs_type);

        const_value_t* val = NULL;
        if (value->compute_const != NULL)
        {
            if (lhs_type != NULL) 
            {
                // Binary
                val = value->compute_const(nodecl_lhs, nodecl_rhs);
            }
            else
            {
                val = value->compute_const(nodecl_null(), nodecl_rhs);
            }
        }

        // Keep the conversions
        if (convert_to_common)
        {
            if (lhs_type != NULL
                    && !equivalent_types(no_ref(result), no_ref(lhs_type)))
            {
                nodecl_lhs = nodecl_make_conversion(nodecl_lhs, result, 
                        nodecl_get_filename(nodecl_lhs), nodecl_get_line(nodecl_lhs));
            }
            if (!equivalent_types(no_ref(result), no_ref(rhs_type)))
            {
                nodecl_rhs = nodecl_make_conversion(nodecl_rhs, result, 
                        nodecl_get_filename(nodecl_rhs), nodecl_get_line(nodecl_rhs));
            }
        }

        *nodecl_output = value->compute_nodecl(nodecl_lhs, nodecl_rhs, result, ASTFileName(expr), ASTLine(expr));
        nodecl_set_constant(*nodecl_output, val);
    }


    return result;
}

const char* operator_names[] =
{
    [AST_PLUS] = "+",
    [AST_NEG] = "-",
    [AST_ADD] = "+",
    [AST_MINUS] = "-",
    [AST_MUL] = "*",
    [AST_DIV] = "/",
    [AST_POWER] = "**",
    [AST_CONCAT] = "//",
    [AST_EQUAL] = "==",
    [AST_DIFFERENT] = "/=",
    [AST_LOWER_THAN] = "<",
    [AST_LOWER_OR_EQUAL_THAN] = "<=",
    [AST_GREATER_THAN] = ">",
    [AST_GREATER_OR_EQUAL_THAN] = ">=",
    [AST_LOGICAL_NOT] = ".NOT.",
    [AST_LOGICAL_EQUAL] = ".EQV.",
    [AST_LOGICAL_DIFFERENT] = ".NEQV.",
    [AST_LOGICAL_AND] = ".AND.",
    [AST_LOGICAL_OR] = ".OR.",
};

static const char * get_operator_for_expr(AST expr)
{
    return operator_names[ASTType(expr)];
}

static void conform_types_(type_t* lhs_type, type_t* rhs_type, 
        type_t** conf_lhs_type, type_t** conf_rhs_type,
        char conform_only_lhs)
{
    lhs_type = no_ref(lhs_type);
    rhs_type = no_ref(rhs_type);

    if (!is_fortran_array_type(lhs_type)
            && !is_fortran_array_type(rhs_type))
    {
        *conf_lhs_type = lhs_type;
        *conf_rhs_type = rhs_type;
    }
    else if ((is_fortran_array_type(lhs_type)
                && !is_fortran_array_type(rhs_type))
            || (!conform_only_lhs
                && !is_fortran_array_type(lhs_type)
                && is_fortran_array_type(rhs_type)))
    {
        // One is array and the other is scalar
        *conf_lhs_type = get_rank0_type(lhs_type);
        *conf_rhs_type = get_rank0_type(rhs_type);
    }
    else 
    {
        // Both are arrays, they only conform if their rank (and ultimately its
        // shape but this is not always checkable) matches
        if (get_rank_of_type(lhs_type) == get_rank_of_type(rhs_type))
        {
            *conf_lhs_type = get_rank0_type(lhs_type);
            *conf_rhs_type = get_rank0_type(rhs_type);
        }
        else
        // Do not conform
        {
            *conf_lhs_type = lhs_type;
            *conf_rhs_type = rhs_type;
        }
    }
}

static void conform_types_in_assignment(type_t* lhs_type, type_t* rhs_type, 
        type_t** conf_lhs_type, type_t** conf_rhs_type)
{
    conform_types_(lhs_type, rhs_type, conf_lhs_type, conf_rhs_type,
            /* conform_only_left */ 1);
}

static void conform_types(type_t* lhs_type, type_t* rhs_type, 
        type_t** conf_lhs_type, type_t** conf_rhs_type)
{
    conform_types_(lhs_type, rhs_type, conf_lhs_type, conf_rhs_type,
            /* conform_only_left */ 0);
}

static type_t* rerank_type(type_t* rank0_common, type_t* lhs_type, type_t* rhs_type)
{
    lhs_type = no_ref(lhs_type);
    rhs_type = no_ref(rhs_type);

    if (is_fortran_array_type(lhs_type))
    {
        // They should have the same rank and shape so it does not matter very much which one we use, right?
        return rebuild_array_type(rank0_common, lhs_type);
    }
    else if (is_fortran_array_type(rhs_type))
    {
        return rebuild_array_type(rank0_common, rhs_type);
    }
    else
    {
        return rank0_common;
    }
}

static const_value_t* const_bin_(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs,
        const_value_t* (*compute)(const_value_t*, const_value_t*))
{
    if (nodecl_is_constant(nodecl_lhs)
            && nodecl_is_constant(nodecl_rhs))
    {
        return compute(nodecl_get_constant(nodecl_lhs),
                nodecl_get_constant(nodecl_rhs));
    }
    return NULL;
}

static const_value_t* const_unary_(nodecl_t nodecl_lhs, const_value_t* (*compute)(const_value_t*))
{
    if (nodecl_is_constant(nodecl_lhs))
    {
        return compute(nodecl_get_constant(nodecl_lhs));
    }
    return NULL;
}

static const_value_t* const_unary_plus(nodecl_t nodecl_lhs UNUSED_PARAMETER, nodecl_t nodecl_rhs)
{
    return const_unary_(nodecl_rhs, const_value_plus);
}

static const_value_t* const_unary_neg(nodecl_t nodecl_lhs UNUSED_PARAMETER, nodecl_t nodecl_rhs)
{
    return const_unary_(nodecl_rhs, const_value_neg);
}

static const_value_t* const_bin_add(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_add);
}

static const_value_t* const_bin_sub(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_sub);
}

static const_value_t* const_bin_mult(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_mul);
}

static const_value_t* const_bin_div(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_div);
}

static const_value_t* const_bin_power(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_pow);
}

static const_value_t* const_bin_concat(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_string_concat);
}

static const_value_t* const_bin_equal(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_eq);
}

static const_value_t* const_bin_not_equal(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_neq);
}

static const_value_t* const_bin_lt(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_lt);
}

static const_value_t* const_bin_lte(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_lte);
}

static const_value_t* const_bin_gt(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_gt);
}

static const_value_t* const_bin_gte(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_gte);
}

static const_value_t* const_unary_not(nodecl_t nodecl_lhs UNUSED_PARAMETER, nodecl_t nodecl_rhs)
{
    return const_unary_(nodecl_rhs, const_value_not);
}

static const_value_t* const_bin_and(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_and);
}

static const_value_t* const_bin_or(nodecl_t nodecl_lhs, nodecl_t nodecl_rhs)
{
    return const_bin_(nodecl_lhs, nodecl_rhs, const_value_or);
}

type_t* common_type_of_binary_operation(type_t* t1, type_t* t2)
{
    t1 = no_ref(t1);
    t2 = no_ref(t2);

    if (is_pointer_type(t1))
        t1 = pointer_type_get_pointee_type(t1);
    if (is_pointer_type(t2))
        t2 = pointer_type_get_pointee_type(t2);

    if ((is_bool_type(t1) && is_bool_type(t2))
            || (is_integer_type(t1) && is_integer_type(t2))
            || (is_floating_type(t1) && is_floating_type(t2))
            || (is_complex_type(t1) && is_complex_type(t2)))
    {
        return common_kind(t1, t2);
    }
    else 
    {
        int i;
        int max = sizeof(arithmetic_binary) / sizeof(arithmetic_binary[0]);
        for (i = 0; i < max; i++)
        {
            if ((arithmetic_binary[i].lhs_type)(t1)
                    && (arithmetic_binary[i].rhs_type)(t2))
            {
                return  (arithmetic_binary[i].common_type)(t1, t2);
            }
        }
    }
    return NULL;
}

type_t* common_type_of_equality_operation(type_t* t1, type_t* t2)
{
    t1 = no_ref(t1);
    t2 = no_ref(t2);

    if (is_pointer_type(t1))
        t1 = pointer_type_get_pointee_type(t1);
    if (is_pointer_type(t2))
        t2 = pointer_type_get_pointee_type(t2);

    int i;
    int max = sizeof(relational_equality) / sizeof(relational_equality[0]);
    for (i = 0; i < max; i++)
    {
        if ((relational_equality[i].lhs_type)(t1)
                && (relational_equality[i].rhs_type)(t2))
        {
            return  (relational_equality[i].common_type)(t1, t2);
        }
    }
    return NULL;
}

static nodecl_t fortran_nodecl_adjust_function_argument(
        type_t* parameter_type,
        nodecl_t argument)
{
    if (is_pointer_type(no_ref(parameter_type)))
    {
        ERROR_CONDITION(nodecl_get_kind(argument) != NODECL_DERREFERENCE, "Invalid pointer access", 0);
        argument = nodecl_get_child(argument, 0);
    }

    return argument;
}
