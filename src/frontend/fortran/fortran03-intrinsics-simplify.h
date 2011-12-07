#ifndef FORTRAN03_INTRINSICS_SIMPLIFY_H
#define FORTRAN03_INTRINSICS_SIMPLIFY_H

#include <math.h>
#include <limits.h>
#include <float.h>

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


static nodecl_t simplify_huge(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = nodecl_get_type(x);
    t = get_rank0_type(t);

    if (is_float_type(t))
    {
        return nodecl_make_floating_literal(t, const_value_get_float(FLT_MAX), NULL, 0);
    }
    else if (is_double_type(t))
    {
        return nodecl_make_floating_literal(t, const_value_get_double(DBL_MAX), NULL, 0);
    }
    else if (is_long_double_type(t))
    {
        return nodecl_make_floating_literal(t, const_value_get_long_double(LDBL_MAX), NULL, 0);
    }
    else if (is_integer_type(t))
    {
        return nodecl_make_integer_literal(t, integer_type_get_maximum(t), NULL, 0);
    }

    return nodecl_null();
}

static nodecl_t simplify_tiny(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = nodecl_get_type(x);
    t = get_rank0_type(t);

    if (is_float_type(t))
    {
        return nodecl_make_floating_literal(t, const_value_get_float(FLT_MIN), NULL, 0);
    }
    else if (is_double_type(t))
    {
        return nodecl_make_floating_literal(t, const_value_get_double(DBL_MIN), NULL, 0);
    }
    else if (is_long_double_type(t))
    {
        return nodecl_make_floating_literal(t, const_value_get_long_double(LDBL_MIN), NULL, 0);
    }
    else if (is_integer_type(t))
    {
        return nodecl_make_integer_literal(t, const_value_get_one(type_get_size(t), 1), NULL, 0);
    }

    return nodecl_null();
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static nodecl_t simplify_range(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];
    type_t* t = nodecl_get_type(x);

    int value = 0;
    if (is_integer_type(t))
    {
        nodecl_t huge = simplify_huge(1, arguments);
        const_value_t* val = nodecl_get_constant(huge);
        value = log10( const_value_cast_to_8(val) );
    }
    else if (is_floating_type(t))
    {
        nodecl_t huge = simplify_huge(1, arguments);
        nodecl_t tiny = simplify_tiny(1, arguments);
        double huge_val = const_value_cast_to_double(nodecl_get_constant(huge));
        double tiny_val = const_value_cast_to_double(nodecl_get_constant(tiny));

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

    // Get our three reals: float, double, long double
    type_t* real_types[] = { get_float_type(), get_double_type(), get_long_double_type() };

    int num_reals = sizeof(real_types) / sizeof(real_types[0]);

    uint64_t p_ = const_value_cast_to_8(nodecl_get_constant(p));
    uint64_t r_ = const_value_cast_to_8(nodecl_get_constant(r));
    uint64_t radix_ = const_value_cast_to_8(nodecl_get_constant(radix));

    int i;
    for (i = 0; i < num_reals; i++)
    {
        nodecl_t nodecl_type = nodecl_make_type(real_types[i], NULL, 0);

        nodecl_t precision = simplify_precision(1, &nodecl_type);
        nodecl_t range = simplify_range(1, &nodecl_type);
        nodecl_t current_radix = simplify_radix(1, &nodecl_type);

        uint64_t precision_ = const_value_cast_to_8(nodecl_get_constant(precision));
        uint64_t range_ = const_value_cast_to_8(nodecl_get_constant(range));
        uint64_t current_radix_ = const_value_cast_to_8(nodecl_get_constant(current_radix));

        if (p_ <= precision_
                && r_ <= range_
                && (radix_ == 0 || radix_ == current_radix_))
        {
            return nodecl_make_int_literal(type_get_size(real_types[i]));
        }
    }

    return nodecl_null();
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

        if (strcmp(t, "\"ASCII\"") == 0
                || strcmp(t, "'ASCII'") == 0)
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

    type_t* t = get_rank0_type(no_ref(nodecl_get_type(str)));

    if (array_type_is_unknown_size(t))
        return nodecl_null();

    return nodecl_copy(array_type_get_array_size_expr(t));
}

static nodecl_t simplify_kind(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];
     
    type_t* t = no_ref(nodecl_get_type(x));
    t = get_rank0_type(t);

    if (is_complex_type(t))
    {
        t = complex_type_get_base_type(t);
    }

    return nodecl_make_int_literal(type_get_size(t));
}

static nodecl_t simplify_digits(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = no_ref(nodecl_get_type(x));
    t = get_rank0_type(t);

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
    t = get_rank0_type(t);

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
                get_float_type(),
                const_value_get_double(DBL_EPSILON),
                NULL, 0);
    }
    else if (is_long_double_type(t))
    {
        return nodecl_make_floating_literal(
                get_float_type(),
                const_value_get_long_double(LDBL_EPSILON),
                NULL, 0);
    }

    return nodecl_null();
}

static nodecl_t simplify_maxexponent(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = no_ref(nodecl_get_type(x));
    t = get_rank0_type(t);

    const floating_type_info_t* model = floating_type_get_info(t);

    return nodecl_make_int_literal(model->emax);
}

static nodecl_t simplify_minexponent(int num_arguments UNUSED_PARAMETER, nodecl_t* arguments)
{
    nodecl_t x = arguments[0];

    type_t* t = no_ref(nodecl_get_type(x));
    t = get_rank0_type(t);

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
        int i, rank = get_rank_of_type(t);
        nodecl_t nodecl_list = nodecl_null();
        for (i = 0; i < rank; i++)
        {
            if (array_type_is_unknown_size(t))
            {
                return nodecl_null();
            }

            nodecl_list = nodecl_concat_lists(
                    nodecl_make_list_1(nodecl_copy(bound_fun(t))),
                    nodecl_list);

            t = array_type_get_element_type(t);
        }

        return nodecl_make_structured_value(
                nodecl_list,
                get_array_type_bounds(choose_int_type_from_kind(kind, kind_),
                    nodecl_make_one(),
                    nodecl_make_int_literal(kind_),
                    CURRENT_COMPILED_FILE->global_decl_context),
                NULL, 0);
    }
    else
    {
        if (nodecl_is_constant(dim))
        {
            type_t* t = no_ref(nodecl_get_type(array));
            int dim_ = const_value_cast_to_4(nodecl_get_constant(dim));

            int rank = get_rank_of_type(t);

            if ((rank - dim_) < 0)
                return nodecl_null();

            int i;
            for (i = 0; i < (rank - dim_); i++)
            {
                t = array_type_get_element_type(t);
            }

            if (!array_type_is_unknown_size(t))
            {
                return nodecl_copy(bound_fun(t));
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
        int i, rank = get_rank_of_type(t);
        int value = 1;
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

            value = value * const_value_cast_to_4(nodecl_get_constant(size));

            t = array_type_get_element_type(t);
        }

        return nodecl_make_integer_literal(
                choose_int_type_from_kind(kind, kind_),
                const_value_get_signed_int(value),
                NULL, 0);
    }
    else
    {
        if (nodecl_is_constant(dim))
        {
            type_t* t = no_ref(nodecl_get_type(array));
            int dim_ = const_value_cast_to_4(nodecl_get_constant(dim));

            int rank = get_rank_of_type(t);

            if ((rank - dim_) < 0)
                return nodecl_null();

            int i;
            for (i = 0; i < (rank - dim_); i++)
            {
                t = array_type_get_element_type(t);
            }

            if (!array_type_is_unknown_size(t))
            {
                return nodecl_copy(array_type_get_array_size_expr(t));
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
    int i, rank = get_rank_of_type(t);
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
                nodecl_make_list_1(nodecl_copy(size)),
                nodecl_list);

        t = array_type_get_element_type(t);
    }

    if (rank > 0)
    {
        return nodecl_make_structured_value(
                nodecl_list,
                get_array_type_bounds(choose_int_type_from_kind(kind, kind_),
                    nodecl_make_one(),
                    nodecl_make_int_literal(kind_),
                    CURRENT_COMPILED_FILE->global_decl_context),
                NULL, 0);
    }
    else
    {
        return nodecl_make_structured_value(
                nodecl_null(),
                get_array_type_bounds(choose_int_type_from_kind(kind, kind_),
                    nodecl_make_one(),
                    nodecl_make_zero(), 
                    CURRENT_COMPILED_FILE->global_decl_context),
                NULL, 0);
    }
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
                    result = current_arg;
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

#endif
