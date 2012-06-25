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

#ifndef FORTRAN03_INTRINSICS_SIMPLIFY_H
#define FORTRAN03_INTRINSICS_SIMPLIFY_H

#include <math.h>
#include <limits.h>
#include <float.h>
#include <complex.h>

static nodecl_t nodecl_make_int_literal(int n)
{
    return nodecl_make_integer_literal(fortran_get_default_integer_type(), 
            const_value_get_integer(n, type_get_size(fortran_get_default_integer_type()), 1), 
            NULL, 0);
}

static nodecl_t nodecl_make_zero(void)
{
    return nodecl_make_int_literal(0);
}

static nodecl_t nodecl_make_one(void)
{
    return nodecl_make_int_literal(1);
}

static nodecl_t simplify_precision(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = nodecl_get_type(x);

    const floating_type_info_t * model = floating_type_get_info(t);

    // In mercurium radix is always 2
    int k = 0;

    int precision = (((model->p - 1) * log10(model->base)) + k);

    return nodecl_make_int_literal(precision);
}

static const_value_t* get_huge_value(type_t* t)
{
    t = fortran_get_rank0_type(t);

    if (is_floating_type(t))
    {
        if (is_float_type(t))
        {
            return const_value_get_float(FLT_MAX);
        }
        else if (is_double_type(t))
        {
            return const_value_get_double(DBL_MAX);
        }
        else if (is_long_double_type(t))
        {
            return const_value_get_long_double(LDBL_MAX);
        }
        else 
        {
#ifdef HAVE_QUADMATH_H
            const floating_type_info_t* floating_info = floating_type_get_info(t);
            if (floating_info->bits == 128)
            {
                return const_value_get_float128(FLT128_MAX);
            }
#endif
        }
    }
    else if (is_integer_type(t))
    {
        return integer_type_get_maximum(t);
    }

    return NULL;
}


static nodecl_t simplify_huge(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = nodecl_get_type(x);

    const_value_t* val = get_huge_value(t);

    if (val != NULL)
    {
        return const_value_to_nodecl(val);
    }

    return nodecl_null();
}

static const_value_t* get_tiny_value(type_t* t)
{
    t = fortran_get_rank0_type(t);

    if (is_floating_type(t))
    {
        if (is_float_type(t))
        {
            return const_value_get_float(FLT_MIN);
        }
        else if (is_double_type(t))
        {
            return const_value_get_double(DBL_MIN);
        }
        else if (is_long_double_type(t))
        {
            return const_value_get_long_double(LDBL_MIN);
        }
        else 
        {
#ifdef HAVE_QUADMATH_H
            const floating_type_info_t* floating_info = floating_type_get_info(t);
            if (floating_info->bits == 128)
            {
                return const_value_get_float128(FLT128_MIN);
            }
#endif
        }
    }
    else if (is_integer_type(t))
    {
        return const_value_get_one(type_get_size(t), 1);
    }

    return NULL;
}

static nodecl_t simplify_tiny(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = nodecl_get_type(x);

    const_value_t* val = get_tiny_value(t);

    if (val != NULL)
    {
        return const_value_to_nodecl(val);
    }

    return nodecl_null();
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static nodecl_t simplify_range(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];
    type_t* t = no_ref(nodecl_get_type(x));

    int value = 0;
    if (is_integer_type(t))
    {
        const_value_t* val = get_huge_value(t);
        value = log10( const_value_cast_to_8(val) );
    }
    else if (is_floating_type(t))
    {
        double huge_val = const_value_cast_to_double(get_huge_value(t));
        double tiny_val = const_value_cast_to_double(get_tiny_value(t));

        value = MIN(log10(huge_val), -log10(tiny_val));
    }
    else if (is_complex_type(t))
    {
        // Not yet implemented
        return nodecl_null();
    }

    return nodecl_make_int_literal(value);
}

static nodecl_t simplify_radix(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments UNUSED_PARAMETER)
{
    // Radix is always 2 in our compiler
    return nodecl_make_int_literal(2);
}

static nodecl_t simplify_selected_real_kind(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t p = arguments[0];
    nodecl_t r = arguments[1];
    nodecl_t radix = arguments[2];

    if (nodecl_is_null(p))
        p = nodecl_make_zero();
    if (nodecl_is_null(r))
        r = nodecl_make_zero();
    if (nodecl_is_null(radix))
        radix = nodecl_make_zero();

    if (!nodecl_is_constant(p)
            || !nodecl_is_constant(r)
            || !nodecl_is_constant(radix))
        return nodecl_null();


    uint64_t p_ = const_value_cast_to_8(nodecl_get_constant(p));
    uint64_t r_ = const_value_cast_to_8(nodecl_get_constant(r));
    uint64_t radix_ = const_value_cast_to_8(nodecl_get_constant(radix));

    int num_reals = CURRENT_CONFIGURATION->type_environment->num_float_types;

    int i;
    for (i = 0; i < num_reals; i++)
    {
        type_t* real_type = get_floating_type_from_descriptor(CURRENT_CONFIGURATION->type_environment->all_floats[i]);

        // Reuse other simplification routines. We build a convenience node here
        nodecl_t nodecl_type = nodecl_make_type(real_type, NULL, 0);

        nodecl_t precision = simplify_precision(1, &nodecl_type);
        nodecl_t range = simplify_range(1, &nodecl_type);
        nodecl_t current_radix = simplify_radix(1, &nodecl_type);

        uint64_t precision_ = const_value_cast_to_8(nodecl_get_constant(precision));
        uint64_t range_ = const_value_cast_to_8(nodecl_get_constant(range));
        uint64_t current_radix_ = const_value_cast_to_8(nodecl_get_constant(current_radix));

        nodecl_free(nodecl_type);

        if (p_ <= precision_
                && r_ <= range_
                && (radix_ == 0 || radix_ == current_radix_))
        {
            return nodecl_make_int_literal(type_get_size(real_type));
        }
    }

    return nodecl_make_int_literal(-1);
}

static nodecl_t simplify_selected_int_kind(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t r = arguments[0];

    if (!nodecl_is_constant(r))
        return nodecl_null();

    int r_ = const_value_cast_to_4(nodecl_get_constant(r));

    int64_t range = 1;
    int i;
    for (i = 0; i < r_; i++)
    {
        range *= 10;
    }

    const_value_t* c1 = const_value_get_signed_long_long_int(range);
    const_value_t* c2 = const_value_get_signed_long_long_int(-range);

    type_t* t1 = const_value_get_minimal_integer_type(c1);
    type_t* t2 = const_value_get_minimal_integer_type(c2);

    int kind_1 = type_get_size(t1);
    int kind_2 = type_get_size(t2);

    int kind = kind_1 > kind_2 ? kind_1 : kind_2;

    return nodecl_make_int_literal(kind);
}

static nodecl_t simplify_selected_char_kind(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t name = arguments[0];

    if (nodecl_get_kind(name) == NODECL_STRING_LITERAL)
    {
        const char* t = nodecl_get_text(name);

        if ((strcmp(t, "\"ASCII\"") == 0)
                || (strcmp(t, "'ASCII'") == 0)
                // gfortran
                || (strcmp(t, "\"DEFAULT\"") == 0)
                || (strcmp(t, "'DEFAULT'") == 0))
        {
            return nodecl_make_int_literal(1);
        }
        else
        {
            // We do not support anything else
            return nodecl_make_int_literal(-1);
        }
    }

    return nodecl_null();
}

static nodecl_t simplify_bit_size(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t i = arguments[0];

    return nodecl_make_int_literal(type_get_size(no_ref(nodecl_get_type(i))) * 8);
}

static nodecl_t simplify_len(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t str = arguments[0];

    type_t* t = fortran_get_rank0_type(no_ref(nodecl_get_type(str)));

    if (array_type_is_unknown_size(t))
        return nodecl_null();

    return nodecl_shallow_copy(array_type_get_array_size_expr(t));
}

static nodecl_t simplify_kind(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];
     
    type_t* t = no_ref(nodecl_get_type(x));
    t = fortran_get_rank0_type(t);

    if (is_complex_type(t))
    {
        t = complex_type_get_base_type(t);
    }
    else if (fortran_is_character_type(t))
    {
        t = array_type_get_element_type(t);
    }

    return nodecl_make_int_literal(type_get_size(t));
}

static nodecl_t simplify_digits(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = no_ref(nodecl_get_type(x));
    t = fortran_get_rank0_type(t);

    if (is_integer_type(t))
    {
        return nodecl_make_int_literal(type_get_size(t) * 8 - 1);
    }
    else if (is_floating_type(t))
    {
        const floating_type_info_t* model = floating_type_get_info(t);

        return nodecl_make_int_literal(model->p + 1);
    }

    return nodecl_null();
}

static nodecl_t simplify_epsilon(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = no_ref(nodecl_get_type(x));
    t = fortran_get_rank0_type(t);

    if (is_float_type(t))
    {
        return nodecl_make_floating_literal(
                get_float_type(),
                const_value_get_float(FLT_EPSILON),
                NULL, 0);
    }
    else if (is_double_type(t))
    {
        return nodecl_make_floating_literal(
                get_double_type(),
                const_value_get_double(DBL_EPSILON),
                NULL, 0);
    }
    else if (is_long_double_type(t))
    {
        return nodecl_make_floating_literal(
                get_long_double_type(),
                const_value_get_long_double(LDBL_EPSILON),
                NULL, 0);
    }

    return nodecl_null();
}

static nodecl_t simplify_maxexponent(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = no_ref(nodecl_get_type(x));
    t = fortran_get_rank0_type(t);

    const floating_type_info_t* model = floating_type_get_info(t);

    return nodecl_make_int_literal(model->emax);
}

static nodecl_t simplify_minexponent(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = no_ref(nodecl_get_type(x));
    t = fortran_get_rank0_type(t);

    const floating_type_info_t* model = floating_type_get_info(t);

    return nodecl_make_int_literal(model->emin);
}

static nodecl_t simplify_xbound(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments,
        nodecl_t (*bound_fun)(type_t*))
{
    nodecl_t array = arguments[0];
    nodecl_t dim = arguments[1];
    nodecl_t kind = arguments[2];

    int kind_ = type_get_size(fortran_get_default_integer_type());
    if (!nodecl_is_null(kind))
    {
        if (!nodecl_is_constant(kind))
            return nodecl_null();

        kind_ = const_value_cast_to_4(nodecl_get_constant(kind));
    }

    if (nodecl_is_null(dim))
    {
        type_t* t = no_ref(nodecl_get_type(array));
        int i, rank = fortran_get_rank_of_type(t);
        nodecl_t nodecl_list = nodecl_null();
        for (i = 0; i < rank; i++)
        {
            if (array_type_is_unknown_size(t))
            {
                return nodecl_null();
            }

            nodecl_list = nodecl_concat_lists(
                    nodecl_make_list_1(nodecl_shallow_copy(bound_fun(t))),
                    nodecl_list);

            t = array_type_get_element_type(t);
        }

        nodecl_t result = nodecl_make_structured_value(
                nodecl_list,
                get_array_type_bounds(choose_int_type_from_kind(kind, kind_),
                    nodecl_make_one(),
                    nodecl_make_int_literal(kind_),
                    CURRENT_COMPILED_FILE->global_decl_context),
                NULL, 0);

        if (rank > 0)
        {
            const_value_t* const_vals[rank];

            t = no_ref(nodecl_get_type(array));
            for (i = 0; i < rank; i++)
            {
                nodecl_t bound = nodecl_shallow_copy(bound_fun(t)); 
                ERROR_CONDITION(!nodecl_is_constant(bound), "This should be constant!", 0);

                const_vals[rank - i - 1] = const_value_cast_to_bytes(nodecl_get_constant(bound), kind_, /* signed */ 1);

                t = array_type_get_element_type(t);
            }

            nodecl_set_constant(result, 
                    const_value_make_array(rank, const_vals));
        }

        return result;
    }
    else
    {
        if (nodecl_is_constant(dim))
        {
            type_t* t = no_ref(nodecl_get_type(array));
            int dim_ = const_value_cast_to_4(nodecl_get_constant(dim));

            int rank = fortran_get_rank_of_type(t);

            if ((rank - dim_) < 0)
                return nodecl_null();

            int i;
            for (i = 0; i < (rank - dim_); i++)
            {
                t = array_type_get_element_type(t);
            }

            if (!array_type_is_unknown_size(t))
            {
                nodecl_t bound = bound_fun(t);
                ERROR_CONDITION(!nodecl_is_constant(bound), "This should be constant!", 0);

                return const_value_to_nodecl_with_basic_type(nodecl_get_constant(bound), 
                        choose_int_type_from_kind(kind, kind_));
            }
        }
    }

    return nodecl_null();
}

static nodecl_t simplify_lbound(int num_arguments, nodecl_t* arguments)
{
    return simplify_xbound(num_arguments, arguments, array_type_get_array_lower_bound);
}

static nodecl_t simplify_ubound(int num_arguments, nodecl_t* arguments)
{
    return simplify_xbound(num_arguments, arguments, array_type_get_array_upper_bound);
}

static nodecl_t simplify_size(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t array = arguments[0];
    nodecl_t dim = arguments[1];
    nodecl_t kind = arguments[2];

    int kind_ = type_get_size(fortran_get_default_integer_type());
    if (!nodecl_is_null(kind))
    {
        if (!nodecl_is_constant(kind))
            return nodecl_null();

        kind_ = const_value_cast_to_4(nodecl_get_constant(kind));
    }

    if (nodecl_is_null(dim))
    {
        type_t* t = no_ref(nodecl_get_type(array));
        int value = array_type_get_total_number_of_elements(t);
        if (value == -1)
        {
            return nodecl_null();
        }
        else
        {
            return nodecl_make_integer_literal(
                    choose_int_type_from_kind(kind, kind_),
                    const_value_get_signed_int(value),
                    NULL, 0);
        }
    }
    else
    {
        if (nodecl_is_constant(dim))
        {
            type_t* t = no_ref(nodecl_get_type(array));
            int dim_ = const_value_cast_to_4(nodecl_get_constant(dim));

            int rank = fortran_get_rank_of_type(t);

            if ((rank - dim_) < 0)
                return nodecl_null();

            int i;
            for (i = 0; i < (rank - dim_); i++)
            {
                t = array_type_get_element_type(t);
            }

            if (!array_type_is_unknown_size(t))
            {
                return nodecl_shallow_copy(array_type_get_array_size_expr(t));
            }
        }
    }

    return nodecl_null();
}

static nodecl_t simplify_shape(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t array = arguments[0];
    nodecl_t kind = arguments[1];

    int kind_ = type_get_size(fortran_get_default_integer_type());
    if (!nodecl_is_null(kind))
    {
        if (!nodecl_is_constant(kind))
            return nodecl_null();

        kind_ = const_value_cast_to_4(nodecl_get_constant(kind));
    }

    nodecl_t nodecl_list = nodecl_null();

    type_t* t = no_ref(nodecl_get_type(array));
    int i, rank = fortran_get_rank_of_type(t);
    for (i = 0; i < rank; i++)
    {
        if (array_type_is_unknown_size(t))
        {
            return nodecl_null();
        }

        nodecl_t size = array_type_get_array_size_expr(t);

        // We could do a bit more here
        if (!nodecl_is_constant(size))
            return nodecl_null();

        nodecl_list = nodecl_concat_lists(
                nodecl_make_list_1(nodecl_shallow_copy(size)),
                nodecl_list);

        t = array_type_get_element_type(t);
    }

    nodecl_t result = nodecl_null();
    if (rank > 0)
    {
        result = nodecl_make_structured_value(
                nodecl_list,
                get_array_type_bounds(
                    choose_int_type_from_kind(kind, kind_),
                    nodecl_make_one(),
                    nodecl_make_int_literal(rank),
                    CURRENT_COMPILED_FILE->global_decl_context),
                NULL, 0);

        const_value_t* const_vals[rank];

        t = no_ref(nodecl_get_type(array));
        for (i = 0; i < rank; i++)
        {
            nodecl_t size = array_type_get_array_size_expr(t);

            const_vals[rank - i - 1] = nodecl_get_constant(size);

            t = array_type_get_element_type(t);
        }

        nodecl_set_constant(
                result,
                const_value_make_array(rank, const_vals));
    }
    else
    {
        result = nodecl_make_structured_value(
                nodecl_null(),
                get_array_type_bounds(
                    choose_int_type_from_kind(kind, kind_),
                    nodecl_make_one(),
                    nodecl_make_zero(), 
                    CURRENT_COMPILED_FILE->global_decl_context),
                NULL, 0);
    }

    return result;
}

static nodecl_t simplify_max_min(int num_arguments, nodecl_t* arguments,
        const_value_t* (combine)(const_value_t*, const_value_t*))
{
    nodecl_t result = nodecl_null();
    int i;
    for (i = 0; i < num_arguments; i++)
    {
        nodecl_t current_arg = arguments[i];
        if (i == 0)
        {
            if (nodecl_is_constant(current_arg))
            {
                result = current_arg;
            }
            else
            {
                result = nodecl_null();
                break;
            }
        }
        else
        {
            if (nodecl_is_constant(current_arg))
            {
                const_value_t *current_val = nodecl_get_constant(result);

                const_value_t *new_val = nodecl_get_constant(current_arg);

                const_value_t* t = combine(new_val, current_val);

                if (const_value_is_nonzero(t))
                {
                    result = const_value_to_nodecl(const_value_cast_as_another( new_val, current_val ));
                }
            }
            else
            {
                result = nodecl_null();
                break;
            }
        }
    }

    return result;
}

static nodecl_t simplify_max_min_plus_conv(int num_arguments, nodecl_t* arguments,
        const_value_t* (combine)(const_value_t*, const_value_t*),
        const_value_t* (convert)(const_value_t*))
{
    nodecl_t result = simplify_max_min(num_arguments, arguments, combine);

    if (!nodecl_is_null(result))
    {
        result = const_value_to_nodecl(convert(nodecl_get_constant(result)));
    }

    return result;
}

static nodecl_t simplify_max(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min(num_arguments, arguments, const_value_gt);
}

static nodecl_t simplify_min(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min(num_arguments, arguments, const_value_lt);
}

static nodecl_t simplify_max1(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_gt, const_value_round_to_zero);
}

static nodecl_t simplify_min1(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_lt, const_value_round_to_zero);
}

static nodecl_t simplify_amax0(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_gt, const_value_cast_to_float_value);
}

static nodecl_t simplify_amin0(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_lt, const_value_cast_to_float_value);
}

static nodecl_t simplify_amax1(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_gt, const_value_cast_to_float_value);
}

static nodecl_t simplify_amin1(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_lt, const_value_cast_to_float_value);
}

static nodecl_t simplify_dmax1(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_gt, const_value_cast_to_double_value);
}

static nodecl_t simplify_dmin1(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_lt, const_value_cast_to_double_value);
}

static nodecl_t simplify_max0(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_gt, const_value_cast_to_signed_int_value);
}

static nodecl_t simplify_min0(int num_arguments, nodecl_t* arguments)
{
    return simplify_max_min_plus_conv(num_arguments, arguments, const_value_lt, const_value_cast_to_signed_int_value);
}

static nodecl_t simplify_int(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t arg = arguments[0];
    nodecl_t arg_kind = arguments[1];

    int kind = fortran_get_default_integer_type_kind();
    if (!nodecl_is_null(arg_kind))
    {
        kind = const_value_cast_to_4(nodecl_get_constant(arg_kind));
    }

    if (!nodecl_is_constant(arg))
        return nodecl_null();

    const_value_t* v = nodecl_get_constant(arg);

    if (const_value_is_integer(v))
    {
        return nodecl_make_integer_literal(
                choose_int_type_from_kind(arg, kind),
                const_value_cast_to_bytes(v, kind, 1),
                NULL, 0);
    }
    else if (const_value_is_floating(v))
    {
        return nodecl_make_integer_literal(
                choose_int_type_from_kind(arg, kind),
                const_value_round_to_zero_bytes(v, kind),
                NULL, 0);
    }

    return nodecl_null();
}

static nodecl_t simplify_real(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t arg = arguments[0];
    nodecl_t arg_kind = arguments[1];

    if (nodecl_is_constant(arg))
    {
        const_value_t* value = nodecl_get_constant(arg);

        int kind = 0;
        if (!nodecl_is_null(arg_kind))
        {
            ERROR_CONDITION(!nodecl_is_constant(arg_kind), "Kind must be constant here", 0);
            const_value_t* kind_value = nodecl_get_constant(arg_kind);
            kind = const_value_cast_to_4(kind_value);
        }
        else
        {
            kind = fortran_get_default_real_type_kind();
        }

        type_t* float_type = choose_float_type_from_kind(arg_kind, kind);

        if (const_value_is_complex(value))
        {
            value = const_value_complex_get_real_part(value);
        }

        if (is_float_type(float_type))
        {
            return const_value_to_nodecl(const_value_cast_to_float_value(value));
        }
        else if (is_double_type(float_type))
        {
            return const_value_to_nodecl(const_value_cast_to_double_value(value));
        }
        else if (is_long_double_type(float_type))
        {
            return const_value_to_nodecl(const_value_cast_to_long_double_value(value));
        }
        else
        {
            running_error("Invalid floating type", 0);
        }
    }

    return nodecl_null();
}

static nodecl_t simplify_float(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t argument_list[2] = { arguments[0], 
        const_value_to_nodecl(const_value_get_signed_int(fortran_get_default_real_type_kind())) }; 
    return simplify_real(2, argument_list);
}

static nodecl_t simplify_char(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    if (nodecl_is_constant(arguments[0]))
    {
        char c = const_value_cast_to_1(nodecl_get_constant(arguments[0]));
        return const_value_to_nodecl(const_value_make_string(&c, 1));
    }

    return nodecl_null();
}

static nodecl_t simplify_achar(int num_arguments, nodecl_t* arguments)
{
    return simplify_char(num_arguments, arguments);
}


static int flatten_array_count_elements(const_value_t* v)
{
    if (const_value_is_array(v))
    {
        int r = 0;
        int i, N = const_value_get_num_elements(v);
        for (i = 0; i < N; i++)
        {
            r += flatten_array_count_elements(const_value_get_element_num(v, i));
        }

        return r;
    }
    else
    {
        return 1;
    }
}

void flatten_array_rec(const_value_t* v, const_value_t*** scalar_item)
{
    if (const_value_is_array(v))
    {
        int i, N = const_value_get_num_elements(v);
        for (i = 0; i < N; i++)
        {
            flatten_array_rec(const_value_get_element_num(v, i), scalar_item);
        }
    }
    else
    {
        (**scalar_item) = v;
        (*scalar_item)++;
    }
}

static const_value_t* flatten_array(const_value_t* v)
{
    int N = flatten_array_count_elements(v);
    const_value_t* flattened_items[N];

    const_value_t** pos = flattened_items;
    flatten_array_rec(v, &pos);

    const_value_t* result = const_value_make_array(N, flattened_items);

    return result;
}

static int flatten_array_count_elements_with_mask(const_value_t* v, const_value_t* mask)
{
    if (const_value_is_array(v) != const_value_is_array(mask))
            return -1;

    if (const_value_is_array(v))
    {
        int r = 0;
        int i, N = const_value_get_num_elements(v);
        for (i = 0; i < N; i++)
        {
            r += flatten_array_count_elements_with_mask(
                    const_value_get_element_num(v, i),
                    const_value_get_element_num(mask, i));
        }

        return r;
    }
    else 
    {
        if (const_value_is_nonzero(mask))
            return 1;
        else
            return 0;
    }
}

void flatten_array_mask_rec(const_value_t* v, const_value_t* mask, const_value_t*** scalar_item)
{
    if (const_value_is_array(v))
    {
        int i, N = const_value_get_num_elements(v);
        for (i = 0; i < N; i++)
        {
            flatten_array_mask_rec(
                    const_value_get_element_num(v, i), 
                    const_value_get_element_num(mask, i), 
                    scalar_item);
        }
    }
    else
    {
        if (const_value_is_nonzero(mask))
        {
            (**scalar_item) = v;
            (*scalar_item)++;
        }
    }
}

static const_value_t* flatten_array_with_mask(const_value_t* v, const_value_t* mask)
{
    if (mask == NULL)
        return flatten_array(v);

    int N = flatten_array_count_elements_with_mask(v, mask);
    if (N < 0)
        return NULL;

    const_value_t* flattened_items[N];

    const_value_t** pos = flattened_items;
    flatten_array_mask_rec(v, mask, &pos);

    const_value_t* result = const_value_make_array(N, flattened_items);

    return result;
}

static void compute_factors_of_array_indexing(
        int N,
        int* shape, 
        int* factors)
{
    int i;
    for (i = 0; i < N; i++)
    {
        factors[i] = 1;
        int j;
        for (j = 0; j < i; j++)
        {
            factors[i] = factors[i] * shape[j];
        }
    }
}

#if 0
static void determine_array_subscript_of_lineal_index(
        int N,
        int* factors,

        int index_,

        int* subscript
        )
{
    int i;
    int val = index_;
    for (i = N - 1; i > 1; i--)
    {
        subscript[i] = val / factors[i];
        val = val % factors[i];
    }

    subscript[0] = val;
}
#endif

static void permute_subscript(
        int N,
        int *subscript,
        int *permutation,

        int *out
        )
{
    int i;
    for (i = 0; i < N; i++)
    {
        out[permutation[i]] = subscript[i];
    }
}

static void determine_lineal_index_of_array_subscript(
        int N,
        int *subscript,

        int *factors,

        int *index_)
{
    (*index_) = 0;

    int i;
    for (i = 0; i < N; i++)
    {
        (*index_) = (*index_) + factors[i] * subscript[i];
    }
}

static const_value_t* reshape_array_from_flattened_rec(
        int N,
        int rank,
        int* shape,
        int* factors,
        int* subscript,
        
        const_value_t* flattened_array,
        const_value_t* flattened_pad
        )
{
    if (rank == N)
    {
        int index_ = 0;

        determine_lineal_index_of_array_subscript(N, subscript, factors, &index_);

        // int i;
        // fprintf(stderr, "(");
        // for (i = 0; i < N; i++)
        // {
        //     if (i > 0)
        //         fprintf(stderr, ", ");
        //     fprintf(stderr, "%d", subscript[i]);
        // }
        // fprintf(stderr, ") -> %d\n", index_);

        if (index_ < 0)
            return NULL;

        if (index_ >= const_value_get_num_elements(flattened_array))
        {
            if (flattened_pad == NULL)
                return NULL;
            else
            {
                int start = index_ - const_value_get_num_elements(flattened_array);
                const_value_t* result = const_value_get_element_num(flattened_pad, start % const_value_get_num_elements(flattened_pad));
                return result;
            }
        }

        const_value_t* value = const_value_get_element_num(flattened_array, index_);

        return value;
    }
    else
    {
        int i;
        int current_rank = N - rank - 1;
        subscript[current_rank] = 0;
        int size = shape[current_rank];
        const_value_t* result[size];
        for (i = 0; i < size; i++)
        {
            result[i] = reshape_array_from_flattened_rec(
                    N,
                    rank + 1,
                    shape,
                    factors,
                    subscript,
                    
                    flattened_array,
                    flattened_pad
                    );

            subscript[current_rank]++;
        }

        return const_value_make_array(size, result);
    }
}

static const_value_t* reshape_array_from_flattened(
         const_value_t* flattened_array, 
         const_value_t* const_val_shape,
         const_value_t* flattened_pad,
         const_value_t* order
         )
{
    int N = const_value_get_num_elements(const_val_shape);
    int shape[N];

    int i;
    for (i = 0; i < N; i++)
    {
        shape[i] = const_value_cast_to_signed_int(const_value_get_element_num(const_val_shape, i));
    }


    int permutation[N];
    if (order == NULL)
    {
        for (i = 0; i < N; i++)
        {
            permutation[i] = i;
        }
    }
    else
    {
        for (i = 0; i < N; i++)
        {
            permutation[i] = const_value_cast_to_signed_int(const_value_get_element_num(order, i)) - 1;
        }
    }

    // We first have to permute the shape to get the proper factors
    int temp_shape[N];
    permute_subscript(N, shape, permutation, temp_shape);

    int temp_factors[N];
    compute_factors_of_array_indexing(N, temp_shape, temp_factors);

    // But factors appear in the given shape order, so we have to permute them as well
    int factors[N];
    permute_subscript(N, temp_factors, permutation, factors);

    int subscript[N];
    memset(subscript, 0, sizeof(subscript));

#if 0
    fprintf(stderr, "RESHAPE!!!\n");
#define PRINT_ARRAY(x) \
        fprintf(stderr, "%s = (", #x); \
        for (i = 0; i < N; i++) \
        { \
            if (i > 0) \
                fprintf(stderr, ", "); \
            fprintf(stderr, "%d", x[i]); \
        } \
        fprintf(stderr, ")\n"); 
    PRINT_ARRAY(shape)
    PRINT_ARRAY(factors)
    PRINT_ARRAY(permutation)
#endif

    return reshape_array_from_flattened_rec(
            N,
            /* rank */ 0,
            shape,
            factors,
            subscript,

            flattened_array,
            flattened_pad
            );
}

static nodecl_t simplify_reshape(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    if (nodecl_is_constant(arguments[0])
            && nodecl_is_constant(arguments[1])
            && (nodecl_is_null(arguments[2]) || nodecl_is_constant(arguments[2]))
            && (nodecl_is_null(arguments[3]) || nodecl_is_constant(arguments[3])))
    {
        const_value_t* shape = nodecl_get_constant(arguments[1]);

        const_value_t* pad = NULL;
        if (!nodecl_is_null(arguments[2]))
            pad = nodecl_get_constant(arguments[2]);
        const_value_t* order = NULL;
        if (!nodecl_is_null(arguments[3]))
            order = nodecl_get_constant(arguments[3]);

        type_t *base_type = fortran_get_rank0_type(nodecl_get_type(arguments[0]));

        const_value_t* flattened_source = flatten_array(nodecl_get_constant(arguments[0]));
        const_value_t* flattened_pad = NULL;
        if (pad != NULL)
        {
            flattened_pad = flatten_array(pad);
        }

        const_value_t* val = reshape_array_from_flattened(
                flattened_source,
                shape,
                flattened_pad,
                order
                );
        if (val == NULL)
            return nodecl_null();

        nodecl_t result = const_value_to_nodecl_with_basic_type(val, base_type);

        return result;
    }
    return nodecl_null();
}

typedef int index_info_t[MCXX_MAX_ARRAY_SPECIFIER];

static const_value_t* reduce_for_a_given_dimension(
        const_value_t* original_array, 
        const_value_t* mask, 
        int num_dimensions,
        int reduced_dimension,
        index_info_t index_info,
        const_value_t* (*combine)(const_value_t* a, const_value_t* b),
        const_value_t* neuter)
{
    const_value_t* result = NULL;

    const_value_t* considered_array = original_array;
    const_value_t* considered_mask_array = mask;

    int i = 0;
    for (i = 0; i < reduced_dimension - 1; i++)
    {
        considered_array = const_value_get_element_num(considered_array, 
                index_info[i]);

        if (considered_mask_array != NULL)
        {
            considered_mask_array = const_value_get_element_num(considered_mask_array, 
                index_info[i]);
        }
    }

    // Now we are in the reduced dimension
    int N = const_value_get_num_elements(considered_array);
    for (i = 0; i < N; i++)
    {
        const_value_t* current_value = const_value_get_element_num(considered_array, i);

        const_value_t* current_mask = NULL;
        if (considered_mask_array != NULL)
        {
            current_mask = const_value_get_element_num(considered_mask_array, i);
        }

        int j;
        for (j = reduced_dimension; j < num_dimensions; j++)
        {
            current_value = const_value_get_element_num(current_value, index_info[j]);

            if (current_mask != NULL)
            {
                current_mask = const_value_get_element_num(current_mask, index_info[j]);
            }
        }

        if (current_mask == NULL
                || const_value_is_nonzero(current_mask))
        {
            if (result == NULL)
            {
                result = current_value;
            }
            else
            {
                result = combine(result, current_value);
            }
        }
    }

    if (result == NULL)
    {
        result = neuter;
    }

    return result;
}


static const_value_t* reduce_recursively(
        const_value_t* original_array, 
        const_value_t* mask, 
        int reduced_dimension,
        int num_dimensions,
        index_info_t index_info,
        const_value_t* (*combine)(const_value_t* a, const_value_t* b),
        const_value_t* neuter,

        const_value_t* current_array, 
        int current_dimension)
{
    if ((current_dimension + 1) < num_dimensions)
    {
        int i, N = const_value_get_num_elements(current_array);

        const_value_t* tmp[N];

        for (i = 0; i < N; i++)
        {
            index_info[current_dimension] = i;

            tmp[i] = reduce_recursively(original_array,
                    mask,
                    reduced_dimension,
                    num_dimensions,
                    index_info,
                    combine,
                    neuter,

                    const_value_get_element_num(current_array, i),
                    current_dimension + 1);

            // Early return if this is the reduced dimension
            if ((current_dimension + 1) == reduced_dimension)
            {
                return tmp[i];
            }
        }

        return const_value_make_array(N, tmp);
    }
    else if ((current_dimension + 1) == num_dimensions)
    {
        int i, N = const_value_get_num_elements(current_array);
        const_value_t* tmp[N];

        for (i = 0; i < N; i++)
        {
            index_info[current_dimension] = i;

            tmp[i] = reduce_for_a_given_dimension(original_array, mask, num_dimensions, reduced_dimension, index_info, combine, neuter);

            // Early return if this is the reduced dimension
            if ((current_dimension + 1) == reduced_dimension)
            {
                return tmp[i];
            }
        }

        return const_value_make_array(N, tmp);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static const_value_t* simplify_maxminval_aux(
        const_value_t* array_constant,
        const_value_t* dim_constant,
        const_value_t* mask_constant,
        int num_dimensions,
        const_value_t* (*combine)(const_value_t* a, const_value_t* b),
        const_value_t* neuter
        )
{
    // no DIM=
    if ((dim_constant == NULL)
            // or rank 1
            || (num_dimensions == 1))
    {
        // Case 1) Reduce all values into a scalar
        const_value_t* values = flatten_array_with_mask(array_constant, mask_constant);
        int num_values = const_value_get_num_elements(values);
        if (num_values > 0)
        {
            const_value_t* reduced_val = const_value_get_element_num(values, 0);
            int i;
            for (i = 1; i < num_values; i++)
            {
                const_value_t* current_val = const_value_get_element_num(values, i);
                reduced_val = combine(reduced_val, current_val);
            }

            return reduced_val;
        }
        else
        {
            // Degenerated case
            return neuter;
        }
    }
    else
    {
        // Case 2) Multidimensional reduction
        // Recursively traverse all elements
        //
        index_info_t index_info;
        memset(index_info, 0, sizeof(index_info));

        int reduced_dim = const_value_cast_to_signed_int(dim_constant);

        // This is in fortran order, but we internally use C order +1
        reduced_dim = num_dimensions - reduced_dim + 1;

        return reduce_recursively(
                array_constant,
                mask_constant,
                reduced_dim,
                num_dimensions,
                index_info,
                combine,
                neuter,

                array_constant,
                0);
    }
}

static nodecl_t simplify_maxminval(int num_arguments, 
        nodecl_t* arguments,
        int num_dimensions,
        const_value_t* (*combine)(const_value_t* a, const_value_t* b),
        const_value_t* neuter
        )
{
    nodecl_t array = nodecl_null();
    nodecl_t dim = nodecl_null();
    nodecl_t mask = nodecl_null();
    if (num_arguments == 2)
    {
        array = arguments[0];
        mask = arguments[1];
    }
    else if (num_arguments == 3)
    {
        array = arguments[0];
        dim = arguments[1];
        mask = arguments[2];
    }
    else
    {
        internal_error("Code unreachable", 0);
    }

    if (!nodecl_is_constant(array)
            || (!nodecl_is_null(dim) && !nodecl_is_constant(dim))
            || (!nodecl_is_null(mask) && !nodecl_is_constant(mask)))
        return nodecl_null();

    const_value_t* v = simplify_maxminval_aux(
            nodecl_get_constant(array),
            nodecl_is_null(dim) ? NULL : nodecl_get_constant(dim),
            nodecl_is_null(mask) ? NULL : nodecl_get_constant(mask),
            num_dimensions,
            combine,
            neuter);

    if (v == NULL)
        return nodecl_null();

    return const_value_to_nodecl(v);
}

static const_value_t* const_value_compute_max(const_value_t* a, const_value_t* b)
{
    // a > b
    if (const_value_is_nonzero(const_value_gt(a, b)))
    {
        return a;
    }
    else
    {
        return b;
    }
}

static const_value_t* get_max_neuter_for_type(type_t* t)
{
    if (is_integer_type(t))
    {
        return integer_type_get_minimum(t);
    }
    else if (is_floating_type(t))
    {
        return get_huge_value(t);
    }
    else if (fortran_is_character_type(t))
    {
        nodecl_t nodecl_size = array_type_get_array_size_expr(t);
        const_value_t* size_constant = nodecl_get_constant(nodecl_size);
        ERROR_CONDITION(size_constant == NULL, "This should not happen", 0);

        int size = const_value_cast_to_signed_int(size_constant);
        ERROR_CONDITION(size <= 0, "This should not happen", 0);

        const_value_t* values[size];

        const_value_t* zero = const_value_get_zero(type_get_size(array_type_get_element_type(t)), 1);

        int i;
        for (i = 0; i < size; i++)
        {
            values[i] = zero;
        }

        return const_value_make_string_from_values(size, values);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static nodecl_t simplify_maxval(int num_arguments, nodecl_t* arguments)
{
    nodecl_t array = arguments[0];

    type_t* array_type = no_ref(nodecl_get_type(array));
    type_t* element_type = fortran_get_rank0_type(array_type);
    int num_dimensions = fortran_get_rank_of_type(array_type);

    return simplify_maxminval(
            num_arguments, arguments,
            num_dimensions,
            const_value_compute_max,
            get_max_neuter_for_type(element_type));
}

static const_value_t* const_value_compute_min(const_value_t* a, const_value_t* b)
{
    // a < b
    if (const_value_is_nonzero(const_value_lt(a, b)))
    {
        return a;
    }
    else
    {
        return b;
    }
}

static const_value_t* get_min_neuter_for_type(type_t* t)
{
    // We do not use integer_type_get_minimum because fortran does not use two's complement
    if (is_integer_type(t))
    {
        return integer_type_get_maximum(t);
    }
    else if (is_floating_type(t))
    {
        return get_huge_value(t);
    }
    else if (fortran_is_character_type(t))
    {
        nodecl_t nodecl_size = array_type_get_array_size_expr(t);
        const_value_t* size_constant = nodecl_get_constant(nodecl_size);
        ERROR_CONDITION(size_constant == NULL, "This should not happen", 0);

        int size = const_value_cast_to_signed_int(size_constant);
        ERROR_CONDITION(size <= 0, "This should not happen", 0);

        const_value_t* values[size];

        const_value_t* zero = integer_type_get_maximum(array_type_get_element_type(t));

        int i;
        for (i = 0; i < size; i++)
        {
            values[i] = zero;
        }

        return const_value_make_string_from_values(size, values);
    }
    else
    {
        internal_error("Code unreachable", 0);
    }
}

static nodecl_t simplify_minval(int num_arguments, nodecl_t* arguments)
{
    nodecl_t array = arguments[0];

    type_t* array_type = no_ref(nodecl_get_type(array));
    type_t* element_type = fortran_get_rank0_type(array_type);
    int num_dimensions = fortran_get_rank_of_type(array_type);

    return simplify_maxminval(
            num_arguments, arguments,
            num_dimensions,
            const_value_compute_min,
            get_min_neuter_for_type(element_type));
}

static const_value_t* compute_abs(const_value_t* cval)
{
    // Array case
    if (const_value_is_array(cval))
    {
        int i, N = const_value_get_num_elements(cval);
        const_value_t* array[N];

        for (i = 0; i < N; i++)
        {
            array[i] = compute_abs ( const_value_get_element_num(cval, i) );
            if (array[i] == NULL)
                return NULL;
        }

        return const_value_make_array(N, array);
    }

    if (const_value_is_integer(cval)
            || const_value_is_floating(cval))
    {
        if (const_value_is_negative(cval))
        {
            cval = const_value_neg(cval);
        }
        return cval;
    }
    else if (const_value_is_complex(cval))
    {
        const_value_t* real_part = const_value_complex_get_real_part(cval);
        const_value_t* imag_part = const_value_complex_get_imag_part(cval);

        const_value_t* result = 
            const_value_sqrt(
                    const_value_add(
                        const_value_square(real_part),
                        const_value_square(imag_part)));

        return result;
    }
    else 
        return NULL;
}

static nodecl_t simplify_abs(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    const_value_t* abs_value = compute_abs(cval);
    if (abs_value == NULL)
        return nodecl_null();

    return const_value_to_nodecl(abs_value);
}

static const_value_t* common_real_function_1(
        const_value_t* cval, 
        char domain_check(const_value_t*),
        float funf(float),
        double fun(double),
        long double funl(long double),
#ifdef HAVE_QUADMATH_H
        __float128 funq(__float128),
#else
        void *funq UNUSED_PARAMETER,
#endif
        _Complex float cfunf(_Complex float),
        _Complex double cfun(_Complex double),
        _Complex long double cfunl(_Complex long double),
#ifdef HAVE_QUADMATH_H
        __complex128 cfunq(__complex128)
#else
        void *cfunq UNUSED_PARAMETER
#endif
         )
{
    // Array case
    if (const_value_is_array(cval))
    {
        int i, N = const_value_get_num_elements(cval);
        const_value_t* array[N];

        for (i = 0; i < N; i++)
        {
            array[i] = common_real_function_1(
                    const_value_get_element_num(cval, i),
                    domain_check,
                    funf, fun, funl, funq,
                    cfunf, cfun, cfunl, cfunq);

            if (array[i] == NULL)
                return NULL;
        }

        return const_value_make_array(N, array);
    }

    if (domain_check != NULL
            && !domain_check(cval))
        return NULL;

    if (funf != NULL && const_value_is_float(cval))
    {
        return const_value_get_float( funf(const_value_cast_to_float(cval)) );
    }
    else if (fun != NULL && const_value_is_double(cval))
    {
        return const_value_get_double( fun(const_value_cast_to_double(cval)) );
    }
    else if (funl != NULL && const_value_is_long_double(cval))
    {
        return const_value_get_long_double( funl(const_value_cast_to_long_double(cval)) );
    }
#ifdef HAVE_QUADMATH_H
    else if (funq != NULL && const_value_is_float128(cval))
    {
        return const_value_get_float128( funq(const_value_cast_to_float128(cval)) );
    }
#endif
    else if (const_value_is_complex(cval))
    {
        const_value_t* rval = const_value_complex_get_real_part(cval);
        if (cfunf != NULL && const_value_is_float(rval))
        {
            return const_value_get_complex_float( cfunf(const_value_cast_to_complex_float(cval)) );
        }
        else if (cfun != NULL && const_value_is_double(rval))
        {
            return const_value_get_complex_double( cfun(const_value_cast_to_complex_double(cval)) );
        }
        else if (cfunl != NULL && const_value_is_long_double(rval))
        {
            return const_value_get_complex_long_double( cfunl(const_value_cast_to_complex_long_double(cval)) );
        }
#ifdef HAVE_QUADMATH_H
        else if (cfunq != NULL && const_value_is_float128(rval))
        {
            return const_value_get_complex_float128( cfunq(const_value_cast_to_complex_float128(cval)) );
        }
#endif
    }

    return NULL;
}

static nodecl_t common_real_function_1_to_nodecl(
        const_value_t* cval, 
        char domain_check(const_value_t*),
        float funf(float),
        double fun(double),
        long double funl(long double),
#ifdef HAVE_QUADMATH_H
        __float128 funq(__float128),
#else
        void *funq,
#endif
        _Complex float cfunf(_Complex float),
        _Complex double cfun(_Complex double),
        _Complex long double cfunl(_Complex long double),
#ifdef HAVE_QUADMATH_H
        __complex128 cfunq(__complex128)
#else
        void *cfunq
#endif
         )
{
    const_value_t* result = common_real_function_1(
            cval,
            domain_check,
            funf,
            fun,
            funl,
            funq,

            cfunf,
            cfun,
            cfunl,
            cfunq);

    if (result == NULL)
        return nodecl_null();

    return const_value_to_nodecl(result);
}

static char abs_value_is_lte_1(const_value_t* cval)
{
    const_value_t* cval_abs = cval;
    if (!const_value_is_complex(cval))
    {
        if (const_value_is_negative(cval))
        {
            cval_abs = const_value_neg(cval);
        }
    }
    else
    {
        const_value_t* real_part = const_value_complex_get_real_part(cval);
        const_value_t* imag_part = const_value_complex_get_imag_part(cval);

        cval_abs = const_value_sqrt(
                const_value_add(
                    const_value_square(real_part),
                    const_value_square(imag_part)));
    }

    return !const_value_is_zero(
            const_value_lte(cval_abs, const_value_get_float(1.0f))
            );
}

static nodecl_t simplify_acos(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            abs_value_is_lte_1,
            acosf,
            acos,
            acosl,
#ifdef HAVE_QUADMATH_H
            acosq,
#else
            NULL,
#endif
            cacosf,
            cacos,
            cacosl,
#ifdef HAVE_QUADMATH_H
            cacosq
#else
            NULL
#endif
            );

}

static nodecl_t simplify_acosh(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            abs_value_is_lte_1,
            acoshf,
            acosh,
            acoshl,
#ifdef HAVE_QUADMATH_H
            acoshq,
#else
            NULL,
#endif
            cacoshf,
            cacosh,
            cacoshl,
#ifdef HAVE_QUADMATH_H
            cacoshq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_aimag(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return const_value_to_nodecl(const_value_complex_get_imag_part(cval));
}

static const_value_t* compute_aint(const_value_t* cval)
{
    // Array case
    if (const_value_is_array(cval))
    {
        int i, N = const_value_get_num_elements(cval);
        const_value_t* array[N];

        for (i = 0; i < N; i++)
        {
            array[i] = compute_aint ( const_value_get_element_num(cval, i) );
            if (array[i] == NULL)
                return NULL;
        }

        return const_value_make_array(N, array);
    }

    return const_value_round_to_zero( cval );
}

static nodecl_t simplify_aint(int num_arguments, nodecl_t* arguments)
{
    nodecl_t arg = arguments[0];
    nodecl_t kind = nodecl_null();
    if (num_arguments == 2)
    {
        kind = arguments[1];
    }

    if (!nodecl_is_constant(arg)
            || (!nodecl_is_null(kind) && !nodecl_is_constant(kind)))
        return nodecl_null();

    int kind_ = type_get_size(no_ref(nodecl_get_type(arg)));
    if (!nodecl_is_null(kind))
    {
        kind_ = const_value_cast_to_signed_int(nodecl_get_constant(kind));
    }

    return const_value_to_nodecl_with_basic_type(
            compute_aint(nodecl_get_constant(arg)),
            choose_int_type_from_kind(kind, kind_));
}

static const_value_t* compute_anint(const_value_t* cval)
{
    // Array case
    if (const_value_is_array(cval))
    {
        int i, N = const_value_get_num_elements(cval);
        const_value_t* array[N];

        for (i = 0; i < N; i++)
        {
            array[i] = compute_anint ( const_value_get_element_num(cval, i) );
            if (array[i] == NULL)
                return NULL;
        }

        return const_value_make_array(N, array);
    }

    return const_value_round_to_nearest( cval );
}

static nodecl_t simplify_anint(int num_arguments, nodecl_t* arguments)
{
    nodecl_t arg = arguments[0];
    nodecl_t kind = nodecl_null();
    if (num_arguments == 2)
    {
        kind = arguments[1];
    }

    if (!nodecl_is_constant(arg)
            || (!nodecl_is_null(kind) && !nodecl_is_constant(kind)))
        return nodecl_null();

    int kind_ = type_get_size(no_ref(nodecl_get_type(arg)));
    if (!nodecl_is_null(kind))
    {
        kind_ = const_value_cast_to_signed_int(nodecl_get_constant(kind));
    }

    return const_value_to_nodecl_with_basic_type(
            compute_anint(nodecl_get_constant(arg)),
            choose_int_type_from_kind(kind, kind_));
}

static nodecl_t simplify_asin(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            abs_value_is_lte_1,
            asinf,
            asin,
            asinl,
#ifdef HAVE_QUADMATH_H
            asinq,
#else
            NULL,
#endif
            casinf,
            casin,
            casinl,
#ifdef HAVE_QUADMATH_H
            casinq
#else
            NULL
#endif
            );

}

static nodecl_t simplify_asinh(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            abs_value_is_lte_1,
            asinhf,
            asinh,
            asinhl,
#ifdef HAVE_QUADMATH_H
            asinhq,
#else
            NULL,
#endif
            casinhf,
            casinh,
            casinhl,
#ifdef HAVE_QUADMATH_H
            casinhq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_atan(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            atanf,
            atan,
            atanl,
#ifdef HAVE_QUADMATH_H
            atanq,
#else
            NULL,
#endif
            catanf,
            catan,
            catanl,
#ifdef HAVE_QUADMATH_H
            catanq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_atanh(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            atanhf,
            atanh,
            atanhl,
#ifdef HAVE_QUADMATH_H
            atanhq,
#else
            NULL,
#endif
            catanhf,
            catanh,
            catanhl,
#ifdef HAVE_QUADMATH_H
            catanhq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_cos(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            cosf,
            cos,
            cosl,
#ifdef HAVE_QUADMATH_H
            cosq,
#else
            NULL,
#endif
            ccosf,
            ccos,
            ccosl,
#ifdef HAVE_QUADMATH_H
            ccosq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_cosh(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            coshf,
            cosh,
            coshl,
#ifdef HAVE_QUADMATH_H
            coshq,
#else
            NULL,
#endif
            ccoshf,
            ccosh,
            ccoshl,
#ifdef HAVE_QUADMATH_H
            ccoshq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_sin(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            sinf,
            sin,
            sinl,
#ifdef HAVE_QUADMATH_H
            sinq,
#else
            NULL,
#endif
            csinf,
            csin,
            csinl,
#ifdef HAVE_QUADMATH_H
            csinq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_sinh(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            sinhf,
            sinh,
            sinhl,
#ifdef HAVE_QUADMATH_H
            sinhq,
#else
            NULL,
#endif
            csinhf,
            csinh,
            csinhl,
#ifdef HAVE_QUADMATH_H
            csinhq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_tan(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            tanf,
            tan,
            tanl,
#ifdef HAVE_QUADMATH_H
            tanq,
#else
            NULL,
#endif
            ctanf,
            ctan,
            ctanl,
#ifdef HAVE_QUADMATH_H
            ctanq
#else
            NULL
#endif
            );
}

static nodecl_t simplify_tanh(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            /* domain_check */ NULL,
            tanhf,
            tanh,
            tanhl,
#ifdef HAVE_QUADMATH_H
            tanhq,
#else
            NULL,
#endif
            ctanhf,
            ctanh,
            ctanhl,
#ifdef HAVE_QUADMATH_H
            ctanhq
#else
            NULL
#endif
            );
}

static char value_is_positive(const_value_t* cval)
{
    if (const_value_is_complex(cval))
        return 1;

    return !const_value_is_negative(cval);
}

static nodecl_t simplify_sqrt(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    if (!nodecl_is_constant(x))
        return nodecl_null();

    const_value_t* cval = nodecl_get_constant(x);

    return common_real_function_1_to_nodecl(
            cval,
            value_is_positive,
            sqrtf,
            sqrt,
            sqrtl,
#ifdef HAVE_QUADMATH_H
            sqrtq,
#else
            NULL,
#endif
            csqrtf,
            csqrt,
            csqrtl,
#ifdef HAVE_QUADMATH_H
            csqrtq
#else
            NULL
#endif
            );
}

#endif
