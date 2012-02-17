#include "fortran03-typeutils.h"
#include "fortran03-codegen.h"
#include "cxx-nodecl-decls.h"
#include "cxx-limits.h"
#include "cxx-utils.h"
#include <string.h>

const char* fortran_print_type_str(type_t* t)
{
    t = no_ref(t);

    if (is_error_type(t))
    {
        return "<error-type>";
    }

    const char* result = "";
    char is_pointer = 0;
    if (is_pointer_type(t))
    {
        is_pointer = 1;
        t = pointer_type_get_pointee_type(t);
    }

    struct array_spec_tag {
        nodecl_t lower;
        nodecl_t upper;
        char is_undefined;
    } array_spec_list[MCXX_MAX_ARRAY_SPECIFIER] = { { nodecl_null(), nodecl_null(), 0 }  };

    int array_spec_idx;
    for (array_spec_idx = MCXX_MAX_ARRAY_SPECIFIER - 1; 
            is_fortran_array_type(t);
            array_spec_idx--)
    {
        if (array_spec_idx < 0)
        {
            internal_error("too many array dimensions %d\n", MCXX_MAX_ARRAY_SPECIFIER);
        }

        if (!array_type_is_unknown_size(t))
        {
            array_spec_list[array_spec_idx].lower = array_type_get_array_lower_bound(t);
            array_spec_list[array_spec_idx].upper = array_type_get_array_upper_bound(t);
        }
        else
        {
            array_spec_list[array_spec_idx].is_undefined = 1;
        }

        t = array_type_get_element_type(t);
    }

    char is_array = (array_spec_idx != (MCXX_MAX_ARRAY_SPECIFIER - 1));

    if (is_bool_type(t)
            || is_integer_type(t)
            || is_floating_type(t)
            || is_double_type(t)
            || is_complex_type(t))
    {
        const char* type_name = NULL;
        char c[128] = { 0 };

        if (is_bool_type(t))
        {
            type_name = "LOGICAL";
        }
        else if (is_integer_type(t))
        {
            type_name = "INTEGER";
        }
        else if (is_floating_type(t))
        {
            type_name = "REAL";
        }
        else if (is_complex_type(t))
        {
            type_name = "COMPLEX";
        }
        else
        {
            internal_error("unreachable code", 0);
        }

        size_t size = type_get_size(t);
        if (is_floating_type(t))
        {
            // KIND of floats is their size in byes (using the bits as in IEEE754) 
            size = (floating_type_get_info(t)->bits) / 8;
        }
        else if (is_complex_type(t))
        {
            // KIND of a complex is the KIND of its component type
            type_t* f = complex_type_get_base_type(t);
            size = (floating_type_get_info(f)->bits) / 8;
        }

        snprintf(c, 127, "%s(%zd)", type_name, size);
        c[127] = '\0';

        result = uniquestr(c);
    }
    else if (is_class_type(t))
    {
        scope_entry_t* entry = named_type_get_symbol(t);
        char c[128] = { 0 };
        snprintf(c, 127, "TYPE(%s)", 
                entry->symbol_name);
        c[127] = '\0';

        result = uniquestr(c);
    }
    else if (is_fortran_character_type(t))
    {
        nodecl_t length = array_type_get_array_size_expr(t);
        char c[128] = { 0 };
        snprintf(c, 127, "CHARACTER(LEN=%s)",
                nodecl_is_null(length) ? "*" : codegen_to_str(length));
        c[127] = '\0';
        result = uniquestr(c);
    }
    else if (is_function_type(t))
    {
        result = "PROCEDURE";
        // result = strappend(result, "(");

        // int i;
        // 
        // int n = function_type_get_num_parameters(t);
        // for (i = 0; i < n; i++)
        // {
        //     type_t* param_type = function_type_get_parameter_type_num(t, i);
        //     char c[256];
        //     snprintf(c, 255, "%s%s", (i == 0 ? "" : ", "), 
        //             fortran_print_type_str(param_type));
        //     c[255] = '\0';

        //     result = strappend(result, c);
        // }

        // result = strappend(result, ")");
    }
    else 
    {
        internal_error("Not a FORTRAN printable type '%s'\n", print_declarator(t));
    }

    if (is_pointer)
    {
        result = strappend(result, ", POINTER");
    }

    if (is_array)
    {
        array_spec_idx++;
        result = strappend(result, ", DIMENSION(");

        while (array_spec_idx <= (MCXX_MAX_ARRAY_SPECIFIER - 1))
        {
            if (!array_spec_list[array_spec_idx].is_undefined)
            {
                result = strappend(result, codegen_to_str(array_spec_list[array_spec_idx].lower));
                result = strappend(result, ":");
                result = strappend(result, codegen_to_str(array_spec_list[array_spec_idx].upper));
            }
            else
            {
                result = strappend(result, ":");
            }
            if ((array_spec_idx + 1) <= (MCXX_MAX_ARRAY_SPECIFIER - 1))
            {
                result = strappend(result, ", ");
            }
            array_spec_idx++;
        }

        result = strappend(result, ")");
    }

    return result;
}

char is_pointer_to_array_type(type_t* t)
{
    t = no_ref(t);

    return (is_pointer_type(t)
            && is_array_type(pointer_type_get_pointee_type(t)));
}

int get_rank_of_type(type_t* t)
{
    t = no_ref(t);

    if (!is_fortran_array_type(t)
            && !is_pointer_to_fortran_array_type(t))
        return 0;

    if (is_pointer_to_fortran_array_type(t))
    {
        t = pointer_type_get_pointee_type(t);
    }

    int result = 0;
    while (is_fortran_array_type(t))
    {
        result++;
        t = array_type_get_element_type(t);
    }

    return result;
}

type_t* get_rank0_type_internal(type_t* t, char ignore_pointer)
{
    t = no_ref(t);

    if (ignore_pointer && is_pointer_type(t))
        t = pointer_type_get_pointee_type(t);

    while (is_fortran_array_type(t))
    {
        t = array_type_get_element_type(t);
    }
    return t;
}

type_t* get_rank0_type(type_t* t)
{
    return get_rank0_type_internal(t, /* ignore_pointer */ 0);
}

char is_fortran_character_type(type_t* t)
{
    t = no_ref(t);

    return (is_array_type(t)
            && is_character_type(array_type_get_element_type(t)));
}

char is_fortran_character_type_or_pointer_to(type_t* t)
{
    t = no_ref(t);
    return is_pointer_to_fortran_character_type(t)
        || is_fortran_character_type(t);
}

char is_pointer_to_fortran_character_type(type_t* t)
{
    t = no_ref(t);

    if (is_pointer_type(t))
    {
        return is_fortran_character_type(pointer_type_get_pointee_type(t));
    }
    return 0;
}

type_t* replace_return_type_of_function_type(type_t* function_type, type_t* new_return_type)
{
    ERROR_CONDITION(!is_function_type(function_type), "Must be a function type", 0);

    int num_parameters = function_type_get_num_parameters(function_type);
    if (!function_type_get_lacking_prototype(function_type))
    {
        parameter_info_t parameter_info[1 + num_parameters];
        memset(&parameter_info, 0, sizeof(parameter_info));
        int i;
        for (i = 0; i < num_parameters; i++)
        {
            parameter_info[i].type_info = function_type_get_parameter_type_num(function_type, i);
        }

        return get_new_function_type(new_return_type, parameter_info, num_parameters);
    }
    else
    {
        return get_nonproto_function_type(new_return_type, num_parameters);
    }
}

char equivalent_tk_types(type_t* t1, type_t* t2)
{
    type_t* r1 = get_rank0_type_internal(t1, /* ignore pointer */ 1);
    type_t* r2 = get_rank0_type_internal(t2, /* ignore pointer */ 1);

    // Preprocess for character types
    if (is_fortran_character_type(r1))
    {
        r1 = get_unqualified_type(array_type_get_element_type(r1));
    }
    if (is_fortran_character_type(r2))
    {
        r2 = get_unqualified_type(array_type_get_element_type(r2));
    }

    return equivalent_types(get_unqualified_type(r1), get_unqualified_type(r2));
}

char equivalent_tkr_types(type_t* t1, type_t* t2)
{
    if (!equivalent_tk_types(t1, t2))
        return 0;

    int rank1 = get_rank_of_type(t1);
    int rank2 = get_rank_of_type(t2);

    if (rank1 != rank2)
        return 0;

    return 1;
}

type_t* update_basic_type_with_type(type_t* type_info, type_t* basic_type)
{
    if (is_pointer_type(type_info))
    {
        return get_pointer_type(
                update_basic_type_with_type(pointer_type_get_pointee_type(type_info), basic_type)
                );
    }
    else if (is_fortran_array_type(type_info))
    {
        return get_array_type_bounds(
                update_basic_type_with_type(array_type_get_element_type(type_info), basic_type),
                array_type_get_array_lower_bound(type_info),
                array_type_get_array_upper_bound(type_info),
                array_type_get_array_size_expr_context(type_info));

    }
    else if (is_function_type(type_info))
    {
        return replace_return_type_of_function_type(type_info, basic_type);
    }
    else if (is_lvalue_reference_type(type_info))
    {
        return get_lvalue_reference_type(
                update_basic_type_with_type(reference_type_get_referenced_type(type_info), basic_type));
    }
    else
    {
        return basic_type;
    }
}

char basic_type_is_implicit_none(type_t* t)
{
    if (t == NULL)
    {
        return 0;
    }
    else if (is_implicit_none_type(t))
    {
        return 1;
    }
    else if (is_array_type(t))
    {
        return basic_type_is_implicit_none(array_type_get_element_type(t));
    }
    else if (is_function_type(t))
    {
        return basic_type_is_implicit_none(function_type_get_return_type(t));
    }
    else if (is_lvalue_reference_type(t))
    {
        return basic_type_is_implicit_none(reference_type_get_referenced_type(t));
    }
    else if (is_pointer_type(t))
    {
        return basic_type_is_implicit_none(pointer_type_get_pointee_type(t));
    }
    else
        return 0;
}

char is_fortran_array_type(type_t* t)
{
    t = no_ref(t);

    return is_array_type(t)
        && !is_fortran_character_type(t);
}

char is_pointer_to_fortran_array_type(type_t* t)
{
    t = no_ref(t);

    return is_pointer_type(t)
        && is_fortran_array_type(pointer_type_get_pointee_type(t));
}

char is_fortran_array_type_or_pointer_to(type_t* t)
{
    return is_fortran_array_type(t)
        || is_pointer_to_fortran_array_type(t);
}

char fortran_is_scalar_type(type_t* t)
{
    return (!is_pointer_type(t)
            && !is_pointer_to_member_type(t)
            && !is_array_type(t)
            && !is_lvalue_reference_type(t)
            && !is_rvalue_reference_type(t)
            && !is_function_type(t)
            && !is_vector_type(t));
}

type_t* rebuild_array_type(type_t* rank0_type, type_t* array_type)
{
    rank0_type = no_ref(rank0_type);

    ERROR_CONDITION(!fortran_is_scalar_type(rank0_type)
            && !is_fortran_character_type(rank0_type), "Invalid rank0 type", 0);

    if (!is_fortran_array_type(array_type))
    {
        return rank0_type;
    }
    else
    {
        type_t* t = rebuild_array_type(rank0_type, array_type_get_element_type(array_type));
        if (!array_type_is_unknown_size(array_type))
        {
            return get_array_type_bounds(t, 
                    array_type_get_array_lower_bound(array_type),
                    array_type_get_array_upper_bound(array_type),
                    array_type_get_array_size_expr_context(array_type));
        }
        else
        {
            return get_array_type(t, nodecl_null(), array_type_get_array_size_expr_context(array_type));
        }
    }
}

type_t* get_n_ranked_type(type_t* scalar_type, int rank, decl_context_t decl_context)
{
    scalar_type = no_ref(scalar_type);

    ERROR_CONDITION(is_fortran_array_type(scalar_type), "This is not a scalar type!", 0);

    if (rank == 0)
    {
        return scalar_type;
    }
    else if (rank > 0)
    {
        return get_array_type(get_n_ranked_type(scalar_type, rank-1, decl_context), nodecl_null(), decl_context);
    }
    else
    {
        internal_error("Invalid rank %d\n", rank);
    }
}

char is_fortran_intrinsic_type(type_t* t)
{
    t = no_ref(t);

    if (is_pointer_type(t))
        t = pointer_type_get_pointee_type(t);

    return (is_integer_type(t)
            || is_floating_type(t)
            || is_complex_type(t)
            || is_bool_type(t)
            || is_fortran_character_type(t));
}

char are_conformable_types(type_t* t1, type_t* t2)
{
    t1 = no_ref(t1);
    t2 = no_ref(t2);

    if (get_rank_of_type(t1) == get_rank_of_type(t2))
        return 1;
    else if (get_rank_of_type(t1) == 1
            || get_rank_of_type(t2) == 1)
        return 1;
    else
        return 0;
}

type_t* fortran_get_default_integer_type(void)
{
    return get_signed_int_type();
}

type_t* fortran_get_default_real_type(void)
{
    return get_float_type();
}

type_t* fortran_get_default_logical_type(void)
{
    return get_bool_of_integer_type(fortran_get_default_integer_type());
}

type_t* fortran_get_default_character_type(void)
{
    return get_char_type();
}

int fortran_get_default_integer_type_kind(void)
{
    return type_get_size(fortran_get_default_integer_type());
}

int fortran_get_default_real_type_kind(void)
{
    return type_get_size(fortran_get_default_real_type());
}

int fortran_get_default_logical_type_kind(void)
{
    return type_get_size(fortran_get_default_logical_type());
}

int fortran_get_default_character_type_kind(void)
{
    return type_get_size(fortran_get_default_character_type());
}
