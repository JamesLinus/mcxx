/*--------------------------------------------------------------------
 ( C) Copyright 2006-2014 Barcelona Supercomputing Center             *
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

#include <climits>

#include "cxx-cexpr.h"
#include "tl-ranges-common.hpp"
#include "tl-expression-reduction.hpp"

namespace TL {
namespace Analysis {
namespace Utils {

namespace {

    // ***************************************************************************** //
    // ************* initialize global variables for ranges operations ************* //

    const_value_t* one = const_value_get_one(/*bytes*/ 4, /*signed*/ 1);
    const_value_t* zero = const_value_get_zero(/*bytes*/ 4, /*signed*/ 1);
    const_value_t* long_max = const_value_get_integer(LONG_MAX, /*num_bytes*/sizeof(long), /*sign*/1);
    NBase plus_inf = Nodecl::Analysis::PlusInfinity::make(Type::get_long_int_type(), long_max);
    const_value_t* long_min = const_value_get_integer(LONG_MIN, /*num_bytes*/sizeof(long), /*sign*/1);
    NBase minus_inf = Nodecl::Analysis::MinusInfinity::make(Type::get_long_int_type(), long_min);

    // *********** END initialize global variables for ranges operations *********** //
    // ***************************************************************************** //

    NBase get_max(const NBase& n1, const NBase& n2)
    {
        NBase result;
        if(n1.is<Nodecl::Analysis::PlusInfinity>())
        {
            result = n1;
        }
        else if(n2.is<Nodecl::Analysis::PlusInfinity>())
        {
            result = n2;
        }
        else if(n1.is<Nodecl::Analysis::MinusInfinity>())
        {
            result = n2;
        }
        else if(n2.is<Nodecl::Analysis::MinusInfinity>())
        {
            result = n1;
        }
        else if(n1.is_constant() && n2.is_constant())
        {
            const_value_t* n1_c = n1.get_constant();
            const_value_t* n2_c = n2.get_constant();
            if(const_value_is_positive(const_value_sub(n1_c, n2_c)))
                result = n1;
            else
                result = n2;
        }
        else
        {
            // Avoid nesting Maximum operations
            Nodecl::List exprs;
            if (n1.is<Nodecl::Analysis::Maximum>())
                exprs.append(n1.as<Nodecl::Analysis::Maximum>().get_expressions());
            else if (!Nodecl::Utils::nodecl_is_in_nodecl_list(n1, exprs))
                exprs.append(n1);
            if (n2.is<Nodecl::Analysis::Maximum>())
            {
                const Nodecl::List& n2_exprs = n2.as<Nodecl::Analysis::Minimum>().get_expressions().as<Nodecl::List>();
                for (Nodecl::List::const_iterator it = n2_exprs.begin(); it != n2_exprs.end(); ++it)
                    if (!Nodecl::Utils::nodecl_is_in_nodecl_list(*it, exprs))
                        exprs.append(*it);
            }
            else if (!Nodecl::Utils::nodecl_is_in_nodecl_list(n2, exprs))
                exprs.append(n2);
            result = Nodecl::Analysis::Maximum::make(exprs, n2.get_type());
        }
        return result;
    }
    
    NBase get_min(const NBase& n1, const NBase& n2)
    {
        NBase result;
        if(n1.is<Nodecl::Analysis::PlusInfinity>())
        {
            result = n2;
        }
        else if(n2.is<Nodecl::Analysis::PlusInfinity>())
        {
            result = n1;
        }
        else if(n1.is<Nodecl::Analysis::MinusInfinity>())
        {
            result = n1;
        }
        else if(n2.is<Nodecl::Analysis::MinusInfinity>())
        {
            result = n2;
        }
        else if(n1.is_constant() && n2.is_constant())
        {
            const_value_t* n1_c = n1.get_constant();
            const_value_t* n2_c = n2.get_constant();
            if(const_value_is_positive(const_value_sub(n1_c, n2_c)))
                result = n2;
            else
                result = n1;
        }
        else
        {
            // Avoid nesting Minimum operations
            Nodecl::List exprs;
            if (n1.is<Nodecl::Analysis::Minimum>())
                exprs.append(n1.as<Nodecl::Analysis::Minimum>().get_expressions());
            else if (!Nodecl::Utils::nodecl_is_in_nodecl_list(n1, exprs))
                exprs.append(n1);
            if (n2.is<Nodecl::Analysis::Minimum>())
            {
                const Nodecl::List& n2_exprs = n2.as<Nodecl::Analysis::Minimum>().get_expressions().as<Nodecl::List>();
                for (Nodecl::List::const_iterator it = n2_exprs.begin(); it != n2_exprs.end(); ++it)
                    if (!Nodecl::Utils::nodecl_is_in_nodecl_list(*it, exprs))
                        exprs.append(*it);
            }
            else if (!Nodecl::Utils::nodecl_is_in_nodecl_list(n2, exprs))
                exprs.append(n2);
            result = Nodecl::Analysis::Minimum::make(exprs, n1.get_type());
        }
        return result;
    }
}

    bool nodecl_is_Z_range(const NBase& n)
    {
        if (n.is<Nodecl::Range>())
        {
            const NBase& lb = n.as<Nodecl::Range>().get_lower();
            const NBase& ub = n.as<Nodecl::Range>().get_upper();
            if (lb.is<Nodecl::Analysis::MinusInfinity>()
                && ub.is<Nodecl::Analysis::PlusInfinity>())
                return true;
        }
        return false;
    }

    NBase boundary_addition(const NBase& b1, const NBase& b2)
    {
        // 1.- Check errors
        ERROR_CONDITION(b1.is<Nodecl::Analysis::PlusInfinity>() && b2.is<Nodecl::Analysis::MinusInfinity>(),
                        " Cannot add +inf + -inf. Undefined result.\n", 0);
        ERROR_CONDITION(b1.is<Nodecl::Analysis::MinusInfinity>() && b2.is<Nodecl::Analysis::PlusInfinity>(),
                        " Cannot add -inf + +inf. Undefined result.\n", 0);

        // 2.- Add the boundaries (avoiding overflows when operating with constants)
        NBase b;
        if (b1.is<Nodecl::Analysis::MinusInfinity>()
                || (b1.is_constant() && const_value_is_zero(const_value_sub(b1.get_constant(), long_min)))
                || b1.is<Nodecl::Analysis::PlusInfinity>()
                || (b1.is_constant() && const_value_is_zero(const_value_sub(b1.get_constant(), long_max))))
            b = b1;             // -inf + x = -inf, +inf + x = +inf
        else if (b2.is<Nodecl::Analysis::MinusInfinity>()
                || (b2.is_constant() && const_value_is_zero(const_value_sub(b2.get_constant(), long_min)))
                || b2.is<Nodecl::Analysis::PlusInfinity>()
                || (b2.is_constant() && const_value_is_zero(const_value_sub(b2.get_constant(), long_max))))
            b = b2;             // x + -inf = -inf, x + +inf = +inf
        else if (b1.is_constant() && b2.is_constant())
            b = NBase(const_value_to_nodecl(const_value_add(b1.get_constant(), b2.get_constant())));
        else
            b = Nodecl::Add::make(b1, b2, Type::get_int_type());

        return b;
    }

    NBase range_addition(const NBase& r1, const NBase& r2)
    {
        // 1.- Base cases
        if (r1.is<Nodecl::Analysis::EmptyRange>())
            return r2;
        if (r2.is<Nodecl::Analysis::EmptyRange>())
            return r1;

        // 2.- Check the integrity of the operands
        ERROR_CONDITION(!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>(),
                        "Cannot add '%s' + '%s'. Expecting ranges.\n",
                        r1.prettyprint().c_str(), r2.prettyprint().c_str());

        // 3.- Get the boundaries of the ranges to be added
        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();

        // 4.- Compute the lower bound
        const NBase& lb = boundary_addition(r1_lb, r2_lb);

        // 5.- Compute the upper bound
        const NBase& ub = boundary_addition(r1_ub, r2_ub);

        // 6.- The increment of a range not representing an induction variable is always 0
        const NBase& zero_nodecl = NBase(const_value_to_nodecl(zero));

        // 7.- Build the range
        NBase result = Nodecl::Range::make(lb, ub, zero_nodecl, Type::get_int_type());

        // 8.- Report result if we are in debug mode
        if (RANGES_DEBUG)
            std::cerr << "        Range Addition " << r1.prettyprint() << " + " << r2.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    NBase boundary_subtraction(const NBase& b1, const NBase& b2)
    {
        // 1.- Check errors
        if ((b1.is<Nodecl::Analysis::PlusInfinity>()
                && b2.is<Nodecl::Analysis::PlusInfinity>())
            || (b1.is<Nodecl::Analysis::MinusInfinity>()
                && b2.is<Nodecl::Analysis::MinusInfinity>()))
            return Nodecl::NodeclBase::null();

        // 2.- Subtract the boundaries (avoiding overflows when operating with constants)
        NBase b;
        if (b1.is<Nodecl::Analysis::MinusInfinity>()
                || (b1.is_constant() && const_value_is_zero(const_value_sub(b1.get_constant(), long_min)))
                || b1.is<Nodecl::Analysis::PlusInfinity>()
                || (b1.is_constant() && const_value_is_zero(const_value_sub(b1.get_constant(), long_max))))
            b = b1;             // -inf - x = -inf, +inf - x = +inf
        else if (b2.is<Nodecl::Analysis::MinusInfinity>()
                || (b2.is_constant() && const_value_is_zero(const_value_sub(b2.get_constant(), long_min))))
            b = plus_inf.shallow_copy();             // x - -inf = +inf
            else if (b2.is<Nodecl::Analysis::PlusInfinity>()
                || (b2.is_constant() && const_value_is_zero(const_value_sub(b2.get_constant(), long_max))))
            b = minus_inf.shallow_copy();            // x - +inf = -inf
        else if (b1.is_constant() && b2.is_constant())
            b = NBase(const_value_to_nodecl(const_value_sub(b1.get_constant(), b2.get_constant())));
        else
            b = Nodecl::Minus::make(b1, b2, Type::get_int_type());

        return b;
    }

    NBase range_subtraction(const NBase& r1, const NBase& r2)
    {
        // 1.- Check the integrity of the operands
        ERROR_CONDITION(!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>(),
                        "Cannot subtract '%s' - '%s'. Expecting ranges.\n",
                        r1.prettyprint().c_str(), r2.prettyprint().c_str());

        // 2.- Get the boundaries of the ranges to be subtracted
        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();

        // 3.- Compute the lower bound
        NBase lb = boundary_subtraction(r1_lb, r2_lb);

        // 4.- Compute the upper bound
        NBase ub = boundary_subtraction(r1_ub, r2_ub);

        // 5.- Base case
        // The increment of a range not representing an induction variable is always 0
        const NBase& zero_nodecl = NBase(const_value_to_nodecl(zero));
        if (lb.is_null() || ub.is_null())
            return Nodecl::Range::make(
                    minus_inf.shallow_copy(),
                    plus_inf.shallow_copy(),
                    zero_nodecl,
                    Type::get_int_type());

        // 6.- It may happen that the range is not consistent after the subtraction.
        //     Normalize it here
        if (lb.is_constant() && ub.is_constant())
        {
            const_value_t* lb_c = lb.get_constant();
            const_value_t* ub_c = ub.get_constant();
            if (const_value_is_positive(const_value_sub(lb_c, ub_c)))
            {
                const NBase& tmp = ub.shallow_copy();
                ub = lb.shallow_copy();
                lb = tmp;
            }
        }

        // 7.- Build the range
        NBase result = Nodecl::Range::make(lb, ub, zero_nodecl, Type::get_int_type());

        if (RANGES_DEBUG)
            std::cerr << "        Range Subtraction " << r1.prettyprint() << " - " << r2.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    NBase boundary_multiplication(const NBase& b1, const NBase& b2)
    {
        NBase b;

        if (b1.is<Nodecl::Analysis::MinusInfinity>())
        {
            if (b2.is<Nodecl::Analysis::MinusInfinity>())
            {   // -inf * -inf
                b = plus_inf.shallow_copy();
            }
            else if (b2.is<Nodecl::Analysis::PlusInfinity>())
            {   // -inf * inf
                b = minus_inf.shallow_copy();
            }
            else if (b2.is_constant())
            {
                const_value_t* b2_const = b2.get_constant();
                if (const_value_is_zero(b2_const))
                {   // -inf * 0
                    return Nodecl::NodeclBase::null();
                }
                else if (const_value_is_negative(b2_const))
                {   // -inf * -k
                    b = plus_inf.shallow_copy();
                }
                else
                {   // -inf * k
                    b = minus_inf.shallow_copy();
                }
            }
            else
            {   // can't compute it
                return Nodecl::NodeclBase::null();
            }
        }
        else if (b1.is<Nodecl::Analysis::PlusInfinity>())
        {
            if (b2.is<Nodecl::Analysis::MinusInfinity>())
            {   // inf * -inf
                b = minus_inf.shallow_copy();
            }
            else if (b2.is<Nodecl::Analysis::PlusInfinity>())
            {   // inf * inf
                b = plus_inf.shallow_copy();
            }
            else if (b2.is_constant())
            {
                const_value_t* b2_const = b2.get_constant();
                if (const_value_is_zero(b2_const))
                {   // inf * 0
                    return Nodecl::NodeclBase::null();
                }
                else if (const_value_is_negative(b2_const))
                {   // inf * -k
                    b = minus_inf.shallow_copy();
                }
                else
                {   // inf * k
                    b = plus_inf.shallow_copy();
                }
            }
            else
            {   // can't compute it
                return Nodecl::NodeclBase::null();
            }
        }
        else if (b1.is_constant())
        {
            const_value_t* b1_const = b1.get_constant();
            if (b2.is<Nodecl::Analysis::MinusInfinity>())
            {
                if (const_value_is_zero(b1_const))
                {   // 0 * -inf
                    return Nodecl::NodeclBase::null();
                }
                else if (const_value_is_negative(b1_const))
                {   // -k * -inf
                    b = plus_inf.shallow_copy();
                }
                else
                {   // k * -inf
                    b = minus_inf.shallow_copy();
                }
            }
            else if (b2.is<Nodecl::Analysis::PlusInfinity>())
            {
                if (const_value_is_zero(b1_const))
                {   // 0 * inf
                    return Nodecl::NodeclBase::null();
                }
                else if (const_value_is_negative(b1_const))
                {   // -k * inf
                    b = minus_inf.shallow_copy();
                }
                else
                {   // k * inf
                    b = plus_inf.shallow_copy();
                }
            }
            else if (b2.is_constant())
            {   // k1 * k2
                b = NBase(const_value_to_nodecl(const_value_mul(b1.get_constant(),
                                                                b2.get_constant())));
            }
            else
            {
                b = Nodecl::NodeclBase::null();
            }
        }
        else
        {
            return Nodecl::NodeclBase::null();
        }

        return b;
    }

    NBase range_multiplication(const NBase& r1, const NBase& r2)
    {
        // 1.- Check the integrity of the operands
        if (!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>())
            return Nodecl::Mul::make(r1, r2, r1.get_type());

        // 2.- Get the boundaries of the ranges to be subtracted
        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();
        TL::Type t = r1_lb.get_type();

        // 3.- Compute the lower bound
        NBase lb = boundary_multiplication(r1_lb, r2_lb);

        // 4.- Compute the upper bound
        NBase ub = boundary_multiplication(r1_ub, r2_ub);

        // 5.- Base case
        // The increment of a range not representing an induction variable is always 0
        const NBase& zero_nodecl = NBase(const_value_to_nodecl(zero));
        if (lb.is_null() || ub.is_null())
            return Nodecl::Range::make(
                    minus_inf.shallow_copy(),
                    plus_inf.shallow_copy(),
                    zero_nodecl,
                    Type::get_int_type());

        // 6.- Build the range
        NBase result = Nodecl::Range::make(lb, ub, zero_nodecl, Type::get_int_type());

        if (RANGES_DEBUG)
            std::cerr << "        Range Multiplication " << r1.prettyprint() << " * " << r2.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    NBase boundary_division(const NBase& b1, const NBase& b2)
    {
        NBase b;

        if (b2.is_constant() && const_value_is_zero(b2.get_constant()))
        {   // Base case: x / 0
            return Nodecl::NodeclBase::null();
        }
        else if (const_value_is_zero(b1.get_constant()))
        {   // Base case: 0 / x, x != +-inf
            return NBase(const_value_to_nodecl(zero));
        }
        else if ((b1.is<Nodecl::Analysis::MinusInfinity>()
                    || b1.is<Nodecl::Analysis::PlusInfinity>())
                && (b2.is<Nodecl::Analysis::MinusInfinity>()
                    || b2.is<Nodecl::Analysis::PlusInfinity>()))
        {   // Base case: +-inf / +-inf
            return Nodecl::NodeclBase::null();
        }

        // Rest of cases
        if (b1.is<Nodecl::Analysis::MinusInfinity>())
        {
            if (b2.is_constant())
            {
                const_value_t* b2_const = b2.get_constant();
                if (const_value_is_positive(b2_const))
                {   // -inf / k
                    return minus_inf.shallow_copy();
                }
                else
                {   // -inf / -k
                    return plus_inf.shallow_copy();
                }
            }
            else
            {   // -inf / v
                return Nodecl::NodeclBase::null();
            }
        }
        else if (b1.is<Nodecl::Analysis::PlusInfinity>())
        {
            if (b2.is_constant())
            {
                const_value_t* b2_const = b2.get_constant();
                if (const_value_is_positive(b2_const))
                {   // +inf / k
                    return plus_inf.shallow_copy();
                }
                else
                {   // +inf / -k
                    return minus_inf.shallow_copy();
                }
            }
            else
            {   // +inf / v
                return Nodecl::NodeclBase::null();
            }
        }
        else if (b1.is_constant())
        {
            const_value_t* b1_const = b1.get_constant();
            if (const_value_is_positive(b1_const))
            {
                if (b2.is<Nodecl::Analysis::MinusInfinity>())
                {   // k / -inf
                    return minus_inf.shallow_copy();
                }
                else if (b2.is<Nodecl::Analysis::PlusInfinity>())
                {   // k / +inf
                    return plus_inf.shallow_copy();
                }
                else if (b2.is_constant())
                {   // k1 / k2
                    return NBase(const_value_to_nodecl(const_value_div(b1_const,
                                                                       b2.get_constant())));
                }
                else
                {   // k / v
                    return Nodecl::NodeclBase::null();
                }
            }
            else    // b1 is negative
            {
                if (b2.is<Nodecl::Analysis::MinusInfinity>())
                {   // -k / -inf
                    return plus_inf.shallow_copy();
                }
                else if (b2.is<Nodecl::Analysis::PlusInfinity>())
                {   // -k / +inf
                    return minus_inf.shallow_copy();
                }
                else if (b2.is_constant())
                {   // -k1 / k2
                    return NBase(const_value_to_nodecl(const_value_div(b1_const,
                                                                       b2.get_constant())));
                }
                else
                {   // -k / v
                    return Nodecl::NodeclBase::null();
                }
            }
        }
        else
        {   // v / ...
            return Nodecl::NodeclBase::null();
        }

        return b;
    }

    NBase range_division(const NBase& r1, const NBase& r2)
    {
        // 1.- Check the integrity of the operands
        if(!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>())
            return Nodecl::Div::make(r1, r2, r1.get_type());

        // 2.- Get the boundaries of the ranges to be subtracted
        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();
        TL::Type t = r1_lb.get_type();

        // 3.- Compute the lower bound
        NBase lb = boundary_division(r1_lb, r2_lb);

        // 4.- Compute the upper bound
        NBase ub = boundary_division(r1_ub, r2_ub);

        // 5.- Base case
        // The increment of a range not representing an induction variable is always 0
        const NBase& zero_nodecl = NBase(const_value_to_nodecl(zero));
        if (lb.is_null() || ub.is_null())
            return Nodecl::Range::make(
                minus_inf.shallow_copy(),
                                       plus_inf.shallow_copy(),
                                       zero_nodecl,
                                       Type::get_int_type());

        // 6.- Build the range
        NBase result = Nodecl::Range::make(lb, ub, zero_nodecl, Type::get_int_type());

        if (RANGES_DEBUG)
            std::cerr << "        Range Division " << r1.prettyprint() << " / " << r2.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    // A[al, au] − B[bl, bu] =  ∅, A and B completely overlap or B contains A
    //                          A, A and B do not overlap
    //                          [al, bl] U [bu, au], A contains B
    //                          [al, bl], al < bl
    //                          [au, bu], al > bl
    // When the operation cannot be computed, we return the nodecl representing the operation
    NBase range_sub(const NBase& r1, const NBase& r2)
    {
        ERROR_CONDITION((!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>()), 
                        "range_sub operation can only be applied to ranges, but parameters are '%s' and '%s'.\n", 
                        ast_print_node_type(r1.get_kind()), ast_print_node_type(r2.get_kind()));
        
        NBase result;
        
        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();
        TL::Type t = r1_lb.get_type();
        
        if (Nodecl::Utils::structurally_equal_nodecls(r1_lb, r2_lb)
                && Nodecl::Utils::structurally_equal_nodecls(r1_ub, r2_ub))
        {   // The two ranges are exactly the same
            result = Nodecl::Analysis::EmptyRange::make();
        }
        else if (r1_lb.is_constant() && r1_ub.is_constant() && r2_lb.is_constant() && r2_ub.is_constant())
        {   // Let's compare the constant values
            const_value_t* r1_lb_c = const_value_cast_to_signed_int_value(r1_lb.get_constant());
            const_value_t* r1_ub_c = const_value_cast_to_signed_int_value(r1_ub.get_constant());
            const_value_t* r2_lb_c = const_value_cast_to_signed_int_value(r2_lb.get_constant());
            const_value_t* r2_ub_c = const_value_cast_to_signed_int_value(r2_ub.get_constant());
            
            if( const_value_is_positive(const_value_sub(r1_lb_c, r2_ub_c)) || 
                const_value_is_positive(const_value_sub(r2_lb_c, r1_ub_c)) )
            {   // r1 and r2 do not overlap
                result = r1.shallow_copy();
            }
            else
            {
                const_value_t* lb_dif = const_value_sub( r1_lb_c, r2_lb_c );
                const_value_t* ub_dif = const_value_sub( r1_ub_c, r2_ub_c );
                if(const_value_is_zero(lb_dif) && const_value_is_zero(ub_dif))
                {   // r1 and r2 are the same range
                    result = Nodecl::Analysis::EmptyRange::make();
                }
                else 
                {
                    NBase zero_nodecl = NBase(const_value_to_nodecl(zero));
                    if((const_value_is_positive(lb_dif) || const_value_is_zero(lb_dif)) && 
                       (const_value_is_negative(ub_dif) || const_value_is_zero(ub_dif)))
                    {   // r2 contains r1
                        result = Nodecl::Analysis::EmptyRange::make();
                    }
                    else if((const_value_is_negative(lb_dif) || const_value_is_zero(lb_dif)) && 
                            (const_value_is_positive(ub_dif) || const_value_is_zero(ub_dif)))
                    {   // r2 is contained in r1
                        const_value_t* new_left_ub_c = const_value_sub(r2_lb_c, one);
                        Nodecl::Range left_range = 
                                Nodecl::Range::make(r1_lb.shallow_copy(), NBase(const_value_to_nodecl(new_left_ub_c)), 
                                                    zero_nodecl.shallow_copy(), t);
                        const_value_t* new_right_lb_c = const_value_add(r2_ub_c, one);
                        Nodecl::Range right_range = 
                                Nodecl::Range::make(NBase(const_value_to_nodecl(new_right_lb_c)), r1_ub.shallow_copy(), 
                                                    zero_nodecl.shallow_copy(), t);
                        if(const_value_is_zero(lb_dif))
                            result = right_range;
                        else if(const_value_is_zero(ub_dif))
                            result = left_range;
                        else
                            result = Nodecl::Analysis::RangeUnion::make( left_range, right_range, t );
                    }
                    else
                    {   // r1 and r2 overlap at some point
                        NBase lb, ub;
                        if(const_value_is_positive(lb_dif))
                        {   // lower bound is the same in both ranges
                            lb = NBase(const_value_to_nodecl(const_value_add(r2_ub_c, one)));
                            ub = r1_ub.shallow_copy();
                        }
                        else
                        {   // r2 lower bound is smaller than r1
                            lb = r1_lb.shallow_copy();
                            ub = NBase(const_value_to_nodecl(const_value_sub(r2_lb_c, one)));
                        }
                        result = Nodecl::Range::make(lb, ub, zero_nodecl.shallow_copy(), t);
                    }
                }
            }
        }
        else
        {   // We are not able to synthesize the result
            result = Nodecl::Analysis::RangeSub::make(r1, r2, t);
        }

        if (RANGES_DEBUG)
            std::cerr << "        Range Sub " << r1.prettyprint() << " - " << r2.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }
    
    // A[al, au] ∩ B[bl, bu] = (al<bl<au || al<bu<au) ? [max(al, bl), min(au, bu)]      -> overlap
    //                                                : ∅
    NBase range_intersection(const NBase& n, const NBase& m)
    {
        NBase result;
        // If one element is the empty range, then the intersection is that element
        if (n.is<Nodecl::Analysis::EmptyRange>())
            result = n;
        else if (m.is<Nodecl::Analysis::EmptyRange>())
            result = m;
        // If one element is the range [-inf, +inf], then the intersection is the other,
        // because it is certainly enclosed in [-inf, +inf]
        else if (nodecl_is_Z_range(n))
            result = m;
        else if (nodecl_is_Z_range(m))
            result = n;
        // If the situation is more complicated, try some simplification
        else if(n.is<Nodecl::Range>() && m.is<Nodecl::Analysis::RangeUnion>())
        {   // Try to intersect with one part or the other of the union
            Nodecl::Analysis::RangeUnion tmp = m.as<Nodecl::Analysis::RangeUnion>();
            NBase intersect_lhs = range_intersection(n, tmp.get_lhs());
            NBase intersect_rhs = range_intersection(n, tmp.get_rhs());
            if (intersect_lhs.is<Nodecl::Analysis::EmptyRange>())
                result = intersect_rhs;
            else if (intersect_rhs.is<Nodecl::Analysis::EmptyRange>())
                result = intersect_lhs;
            else
                result = range_union(intersect_lhs, intersect_rhs);
        }
        // If we are intersecting something that is not a range, just create the intersection node and return it
        else if(!n.is<Nodecl::Range>() || !m.is<Nodecl::Range>())
            result = Nodecl::Analysis::RangeIntersection::make(n, m, n.get_type());

        // If some intersection has been computed, return it now!
        if (!result.is_null())
        {
            if (RANGES_DEBUG)
                std::cerr << "        Range Intersection " << n.prettyprint() << " ∩ " << m.prettyprint()
                          << " = " << result.prettyprint() << std::endl;
            return result;
        }

        ERROR_CONDITION((!n.is<Nodecl::Range>() || !m.is<Nodecl::Range>()),
                        "range_intersection operation can only be applied to ranges at this point, but parameters are '%s' and '%s'.\n",
                        ast_print_node_type(n.get_kind()), ast_print_node_type(m.get_kind()));

        NBase lb_n = n.as<Nodecl::Range>().get_lower();
        NBase ub_n = n.as<Nodecl::Range>().get_upper();
        NBase lb_m = m.as<Nodecl::Range>().get_lower();
        NBase ub_m = m.as<Nodecl::Range>().get_upper();
        TL::Type t = lb_n.get_type();
        NBase lb = get_max(lb_n, lb_m);
        NBase ub = get_min(ub_n, ub_m);

        if (lb.is_constant() && ub.is_constant() && 
            const_value_is_positive(const_value_sub(lb.get_constant(), ub.get_constant())))
        {   // Check whether the range is consistent
            result = Nodecl::Analysis::EmptyRange::make();
        }
        else
        {
            NBase zero_nodecl = NBase(const_value_to_nodecl(zero));
            result = Nodecl::Range::make(lb, ub, zero_nodecl, t);
        }

        if (RANGES_DEBUG)
            std::cerr << "        Range Intersection " << n.prettyprint() << " ∩ " << m.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    static NBase range_union_rec(const Nodecl::Range& n, const Nodecl::Range& m)
    {
        NBase result;

        NBase n_lb = n.get_lower();
        NBase n_ub = n.get_upper();
        NBase m_lb = m.get_lower();
        NBase m_ub = m.get_upper();

        if ((n_lb == m_lb) && (n_ub == m_ub))
        {   // The two ranges are exactly the same
            result = n.shallow_copy();
        }
        else
        {
            // FIXME We are not taking into account the type of the variables. Assuming integers
            TL::Type t = n_lb.get_type();

            // Try to operate with constant values
            const_value_t *n_lb_c=NULL, *n_ub_c=NULL, *m_lb_c=NULL, *m_ub_c=NULL;
            if (n_lb.is<Nodecl::Analysis::MinusInfinity>())
                n_lb_c = const_value_get_integer(INT_MIN, /*num_bytes*/4, /*sign*/1);
            else if (n_lb.is_constant())
                n_lb_c = n_lb.get_constant();

            if (m_lb.is<Nodecl::Analysis::MinusInfinity>())
                m_lb_c = const_value_get_integer(INT_MIN, /*num_bytes*/4, /*sign*/1);
            else if (m_lb.is_constant())
                m_lb_c = m_lb.get_constant();

            if (n_ub.is<Nodecl::Analysis::PlusInfinity>())
                n_ub_c = const_value_get_integer(INT_MAX, /*num_bytes*/4, /*sign*/1);
            else if (n_ub.is_constant())
                n_ub_c = n_ub.get_constant();

            if (m_ub.is<Nodecl::Analysis::PlusInfinity>())
                m_ub_c = const_value_get_integer(INT_MAX, /*num_bytes*/4, /*sign*/1);
            else if (m_ub.is_constant())
                m_ub_c = m_ub.get_constant();

            NBase zero_nodecl(const_value_to_nodecl(zero));
            if ((n_lb_c!=NULL) && (n_ub_c!=NULL) && (m_lb_c!=NULL) && (m_ub_c!=NULL))
            {
                if (const_value_is_positive(const_value_sub(n_lb_c, m_ub_c))
                    || const_value_is_positive(const_value_sub(m_lb_c, n_ub_c)))
                {   // n and m do not overlap
                    if (t.is_integral_type())
                    {   // If the boundaries are contiguous, we can still merge the ranges
                        if (const_value_is_one(const_value_sub(n_lb_c, m_ub_c)))
                        {
                            result = Nodecl::Range::make(m_lb.shallow_copy(), n_ub.shallow_copy(), zero_nodecl, t);
                        }
                        else if (const_value_is_one(const_value_sub(m_lb_c, n_ub_c)))
                        {
                            result = Nodecl::Range::make(n_lb.shallow_copy(), m_ub.shallow_copy(), zero_nodecl, t);
                        }
                        else
                        {
                            result = Nodecl::Analysis::RangeUnion::make( n.shallow_copy(), m.shallow_copy(), t );
                        }
                    }
                    else
                    {
                        result = Nodecl::Analysis::RangeUnion::make( n.shallow_copy(), m.shallow_copy(), t );
                    }
                }
                else if (const_value_is_zero(const_value_sub(n_lb_c, m_lb_c))
                    && const_value_is_zero(const_value_sub(n_ub_c, m_ub_c)))
                {   // n and m are the same range
                    result = n.shallow_copy();
                }
                else
                {   // n and m overlap in some way
                    NBase lb = const_value_is_positive(const_value_sub(n_lb_c, m_lb_c)) ? m_lb.shallow_copy()
                                                                                        : n_lb.shallow_copy();
                    NBase ub = const_value_is_positive(const_value_sub(n_ub_c, m_ub_c)) ? n_ub.shallow_copy()
                                                                                        : m_ub.shallow_copy();
                    result = Nodecl::Range::make(lb, ub, zero_nodecl, t);
                }
            }
            else
            {
                // Try some more simplification: [n_lb:n_ub] U [m_lb:m_ub]
                if (m_lb.is_constant() && n_ub.is_constant()
                        && const_value_is_one(const_value_sub(m_lb.get_constant(), n_ub.get_constant())))
                {   // m_lb == n_ub+1
                    result = Nodecl::Range::make(n_lb, m_ub, zero_nodecl, t);
                }
                else if (m_ub.is_constant() && n_lb.is_constant()
                        && const_value_is_one(const_value_sub(n_lb.get_constant(), m_ub.get_constant())))
                {   // m_ub == n_lb-1
                    result = Nodecl::Range::make(m_lb, n_ub, zero_nodecl, t);
                }
                else if (n_lb.is_constant() && m_lb.is_constant()
                        && const_value_is_one(const_value_sub(n_lb.get_constant(), m_lb.get_constant())))
                {   // n_lb = m_lb+1
                    if (Nodecl::Utils::structurally_equal_nodecls(n_ub, m_ub, /*skip_conversions*/true))
                        result = m.shallow_copy();
                    else
                        result = Nodecl::Range::make(m_lb.shallow_copy(), get_max(n_ub, m_ub), zero_nodecl, t);
                }
                else if (n_lb.is_constant() && m_lb.is_constant()
                    && const_value_is_one(const_value_sub(m_lb.get_constant(), n_lb.get_constant())))
                {   // m_lb = n_lb+1
                    if (Nodecl::Utils::structurally_equal_nodecls(n_ub, m_ub, /*skip_conversions*/true))
                        result = n.shallow_copy();
                    else
                        result = Nodecl::Range::make(n_lb.shallow_copy(), get_max(n_ub, m_ub), zero_nodecl, t);
                }
                else
                {
                    result = Nodecl::Analysis::RangeUnion::make(n.shallow_copy(), m.shallow_copy(), t);
                }
            }
        }

        return result;
    }

    static NBase range_and_rangeunion_union(const Nodecl::Range& r, const Nodecl::Analysis::RangeUnion& u)
    {
        NBase result;
        NBase u_lhs = u.get_lhs();
        NBase u_rhs = u.get_rhs();
        Type t = Type::get_int_type();

        if (u_lhs.is<Nodecl::Range>())
        {
            NBase tmp = range_union_rec(r, u_lhs.as<Nodecl::Range>());
            if (tmp.is<Nodecl::Range>())
            {   // r ∪ u_lhs has been simplified
                if (u_rhs.is<Nodecl::Range>())
                    result = range_union_rec(tmp.as<Nodecl::Range>(), u_rhs.as<Nodecl::Range>());
                else
                    result = Nodecl::Analysis::RangeUnion::make(tmp, u_rhs, t);
            }
            else
            {   // r ∪ u_lhs has not been simplified
                if (u_rhs.is<Nodecl::Range>())
                {
                    tmp = range_union_rec(r, u_rhs.as<Nodecl::Range>());
                    if (tmp.is<Nodecl::Range>())
                        result = range_union_rec(tmp.as<Nodecl::Range>(), u_lhs.as<Nodecl::Range>());
                    else
                        result = Nodecl::Analysis::RangeUnion::make(tmp, u_lhs, t);
                }
                else
                    result = Nodecl::Analysis::RangeUnion::make(tmp, u_rhs, t);
            }
        }
        else if (u_rhs.is<Nodecl::Range>())
        {
            NBase tmp = range_union_rec(r, u_rhs.as<Nodecl::Range>());
            result = Nodecl::Analysis::RangeUnion::make(tmp, u_lhs, t);
        }
        else
        {
            result = Nodecl::Analysis::RangeUnion::make(r.shallow_copy(), u.shallow_copy(), t);
        }

        if (RANGES_DEBUG)
            std::cerr << "        Range Union " << r.prettyprint() << " ∪ " << u.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    // A[al, au] ∪ B[bl, bu] = (al<bl<au || al<bu<au) ? [min(al, bl), max(au, bu)]  -> overlap
    //                                                : [al, au] ∪ [bl, bu]         -> cannot synthesize the result as a unique range
    NBase range_union(const NBase& n, const NBase& m)
    {
        // Base case : some node is null
        // Nodes may be null when computing a Phi node the first time

        if (n.is_null())
            return m;
        else if (m.is_null())
            return n;

        // Union for allowed operations
        NBase result;
        // If one element is the empty range, then the union is the other element
        if (n.is<Nodecl::Analysis::EmptyRange>())
            result = m.shallow_copy();
        else if (m.is<Nodecl::Analysis::EmptyRange>())
            result = n.shallow_copy();
        // If n or m is [-inf, +inf], then the union is [-inf, +inf]
        else if (nodecl_is_Z_range(n))
            result = n.shallow_copy();
        else if (nodecl_is_Z_range(m))
            result = m.shallow_copy();
        // Try simple case when the lb_n == ub_m+1 or vice-versa
        TL::Type t = Type::get_int_type();
        if (n.is<Nodecl::Range>() && m.is<Nodecl::Analysis::RangeUnion>())
            result = range_and_rangeunion_union(n.as<Nodecl::Range>(), m.as<Nodecl::Analysis::RangeUnion>());
        else if (n.is<Nodecl::Analysis::RangeUnion>() && m.is<Nodecl::Range>())
            result = range_and_rangeunion_union(m.as<Nodecl::Range>(), n.as<Nodecl::Analysis::RangeUnion>());
        else if (n.is<Nodecl::Analysis::RangeUnion>() && m.is<Nodecl::Analysis::RangeUnion>())
        {   // ( [r1] U [r2] ) U ( [r3] U [r4] )
            Nodecl::Analysis::RangeUnion n_union = n.as<Nodecl::Analysis::RangeUnion>();
            // try r1 U ( [r3] U [r4] )
            result = range_and_rangeunion_union(n_union.get_lhs().as<Nodecl::Range>(),
                                                m.as<Nodecl::Analysis::RangeUnion>());
            // try r2 U result
            result = range_and_rangeunion_union(n_union.get_rhs().as<Nodecl::Range>(),
                                                result.as<Nodecl::Analysis::RangeUnion>());
        }
        // If we are uniting something that is not a range, just create the union node
        else if (n.is<Nodecl::Analysis::RangeIntersection>() || n.is<Nodecl::Analysis::RangeUnion>()
                    || m.is<Nodecl::Analysis::RangeIntersection>() || m.is<Nodecl::Analysis::RangeUnion>())
            result = Nodecl::Analysis::RangeUnion::make(n.shallow_copy(), m.shallow_copy(), n.get_type());

        // If some union has been computed, return it now!
        if (!result.is_null())
        {
            if (RANGES_DEBUG)
                std::cerr << "        Range Union " << n.prettyprint() << " ∪ " << m.prettyprint()
                          << " = " << result.prettyprint() << std::endl;
            return result;
        }

        ERROR_CONDITION((!n.is<Nodecl::Range>() || !m.is<Nodecl::Range>()),
                        "range_union operation can only be applied to ranges at this point, but parameters are '%s' and '%s'.\n",
                        ast_print_node_type(n.get_kind()), ast_print_node_type(m.get_kind()));

        result = range_union_rec(n.as<Nodecl::Range>(), m.as<Nodecl::Range>());

        if (RANGES_DEBUG)
            std::cerr << "        Range Union " << n.prettyprint() << " ∪ " << m.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    static NBase nodecl_value_add(const NBase& n, const NBase& v)
    {
        Optimizations::ReduceExpressionVisitor rev; // Try to reduce the expression
        Type t = n.get_type();
        if (n.is<Nodecl::Analysis::MinusInfinity>() || n.is<Nodecl::Analysis::PlusInfinity>())
            return n;
        else if (n.is_constant() && v.is_constant())
            return const_value_to_nodecl(const_value_add(n.get_constant(), v.get_constant()));
        else if (n.is<Nodecl::Analysis::Minimum>())
        {
            const Nodecl::List& exprs = n.as<Nodecl::Analysis::Minimum>().get_expressions().as<Nodecl::List>();
            Nodecl::List new_exprs;
            for (Nodecl::List::const_iterator it = exprs.begin(); it != exprs.end(); ++it)
            {
                NBase e = *it;
                rev.walk(nodecl_value_add(e, v));
                if (!Nodecl::Utils::nodecl_is_in_nodecl_list(e, new_exprs))
                    new_exprs.append(e);
            }
            return Nodecl::Analysis::Minimum::make(new_exprs, t);
        }
        else if (n.is<Nodecl::Analysis::Maximum>())
        {
            Nodecl::List exprs = n.as<Nodecl::Analysis::Maximum>().get_expressions().as<Nodecl::List>();
            Nodecl::List new_exprs;
            for (Nodecl::List::const_iterator it = exprs.begin(); it != exprs.end(); ++it)
            {
                NBase e = *it;
                rev.walk(nodecl_value_add(e, v));
                if (!Nodecl::Utils::nodecl_is_in_nodecl_list(e, new_exprs))
                    new_exprs.append(e);
            }
            return Nodecl::Analysis::Maximum::make(new_exprs, t);
        }
        else
            return Nodecl::Add::make(n, v, t);
    }

    Nodecl::Range range_value_add(const Nodecl::Range& r, const NBase& v)
    {
        NBase lb = r.get_lower();
        NBase ub = r.get_upper();

        NBase new_lb = nodecl_value_add(lb, v);
        NBase new_ub = nodecl_value_add(ub, v);

        Nodecl::Range result = Nodecl::Range::make(new_lb, new_ub, NBase(const_value_to_nodecl(zero)), v.get_type());

        if (RANGES_DEBUG)
            std::cerr << "        Range Value Add " << r.prettyprint() << " + " << v.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    Nodecl::Range range_value_sub(const Nodecl::Range& r, const NBase& v)
    {
        NBase lb = r.get_lower();
        NBase ub = r.get_upper();
        Type t(lb.get_type());

        NBase new_lb, new_ub;
        // compute the lower bound
        if(lb.is<Nodecl::Analysis::MinusInfinity>() || lb.is<Nodecl::Analysis::PlusInfinity>())
            new_lb = lb;
        else if(lb.is_constant() && v.is_constant())
            new_lb = const_value_to_nodecl(const_value_sub(lb.get_constant(), v.get_constant()));
        else
            new_lb = Nodecl::Minus::make(lb, v, t);
        // compute the upper bound
        if(ub.is<Nodecl::Analysis::MinusInfinity>() || ub.is<Nodecl::Analysis::PlusInfinity>())
            new_ub = ub;
        else if(ub.is_constant() && v.is_constant())
            new_ub = const_value_to_nodecl(const_value_sub(ub.get_constant(), v.get_constant()));
        else
            new_ub = Nodecl::Minus::make(ub, v, t);

        Nodecl::Range result = Nodecl::Range::make(new_lb, new_ub, NBase(const_value_to_nodecl(zero)), t);

        if (RANGES_DEBUG)
            std::cerr << "        Range Value Subtract " << r.prettyprint() << " - " << v.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    Nodecl::Range range_value_mul(const Nodecl::Range& r, const NBase& v)
    {
        NBase lb = r.get_lower();
        NBase ub = r.get_upper();
        Type t(lb.get_type());

        NBase new_lb, new_ub;
        // compute the lower bound
        if (lb.is<Nodecl::Analysis::MinusInfinity>() || lb.is<Nodecl::Analysis::PlusInfinity>())
            new_lb = lb;
        else if (lb.is_constant() && v.is_constant())
            new_lb = const_value_to_nodecl(const_value_mul(lb.get_constant(), v.get_constant()));
        else
            new_lb = Nodecl::Div::make(lb, v, t);
        // compute the upper bound
        if (ub.is<Nodecl::Analysis::MinusInfinity>() || ub.is<Nodecl::Analysis::PlusInfinity>())
            new_ub = ub;
        else if (ub.is_constant() && v.is_constant())
            new_ub = const_value_to_nodecl(const_value_mul(ub.get_constant(), v.get_constant()));
        else
            new_ub = Nodecl::Div::make(ub, v, t);

        Nodecl::Range result = Nodecl::Range::make(new_lb, new_ub, NBase(const_value_to_nodecl(zero)), t);

        if (RANGES_DEBUG)
            std::cerr << "        Range Value Multiplication " << r.prettyprint() << " * " << v.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    Nodecl::Range range_value_div(const Nodecl::Range& r, const NBase& v)
    {
        NBase lb = r.get_lower();
        NBase ub = r.get_upper();
        Type t(lb.get_type());

        NBase new_lb, new_ub;
        // compute the lower bound
        if (lb.is<Nodecl::Analysis::MinusInfinity>() || lb.is<Nodecl::Analysis::PlusInfinity>())
            new_lb = lb;
        else if (lb.is_constant() && v.is_constant())
            new_lb = const_value_to_nodecl(const_value_div(lb.get_constant(), v.get_constant()));
        else
            new_lb = Nodecl::Div::make(lb, v, t);
        // compute the upper bound
        if (ub.is<Nodecl::Analysis::MinusInfinity>() || ub.is<Nodecl::Analysis::PlusInfinity>())
            new_ub = ub;
        else if (ub.is_constant() && v.is_constant())
            new_ub = const_value_to_nodecl(const_value_div(ub.get_constant(), v.get_constant()));
        else
            new_ub = Nodecl::Div::make(ub, v, t);
        
        Nodecl::Range result = Nodecl::Range::make(new_lb, new_ub, NBase(const_value_to_nodecl(zero)), t);

        if (RANGES_DEBUG)
            std::cerr << "        Range Value Division " << r.prettyprint() << " / " << v.prettyprint()
                      << " = " << result.prettyprint() << std::endl;

        return result;
    }

    // ******************************************************************************************* //
    // ******************************* Range Analysis Constraints ******************************** //

    Constraint::Constraint()
        : _ssa_sym(Symbol()), _value(NBase::null()),
          _kind(__Undefined)
    {}

    Constraint::Constraint(const TL::Symbol& ssa_sym,
                           const NBase& value, const ConstraintKind& kind)
        : _ssa_sym(ssa_sym), _value(value), _kind(kind)
    {}

    const TL::Symbol& Constraint::get_symbol() const
    {
        return _ssa_sym;
    }

    void Constraint::set_symbol(const TL::Symbol& s)
    {
        _ssa_sym = s;
    }

    NBase& Constraint::get_value()
    {
        return _value;
    }

    const ConstraintKind& Constraint::get_kind() const
    {
        return _kind;
    }

    bool Constraint::operator!=(const Constraint& c) const
    {
        return ((this->_ssa_sym != c._ssa_sym)
                    || !Nodecl::Utils::structurally_equal_nodecls(this->_value, c._value,
                                                                  /*skip_conversions*/true));
    }

    bool Constraint::operator==(const Constraint& c) const
    {
        return ((this->_ssa_sym == c._ssa_sym)
                    && Nodecl::Utils::structurally_equal_nodecls(this->_value, c._value,
                                                                 /*skip_conversions*/true));
    }

    inline std::string print_constraint_kind(ConstraintKind c_kind)
    {
        switch(c_kind)
        {
            #undef CONSTRAINT_KIND
            #define CONSTRAINT_KIND(X) case __##X : return #X;
            CONSTRAINT_KIND_LIST
            #undef CONSTRAINT_KIND
            default: WARNING_MESSAGE("Unexpected type of node '%d'", c_kind);
        }
        return "";
    }

    void Constraint::print_constraint()
    {
        if (RANGES_DEBUG)
        {
            std::cerr << "    " << print_constraint_kind(_kind)
                      << " Constraint " << _ssa_sym.get_name()
                      << " = " << _value.prettyprint()
                      << std::endl;
        }
    }

    // ***************************** END Range Analysis Constraints ****************************** //
    // ******************************************************************************************* //

    InfinityCalculator::InfinityCalculator()
    {}

    NBase InfinityCalculator::compute(Nodecl::NodeclBase val)
    {
        return walk(val);
    }

    NBase InfinityCalculator::unhandled_node(const Nodecl::NodeclBase& n)
    {
        internal_error("Unhandled node type '%s' while unsing the inifinity calculator for expression '%s'\n",
                       ast_print_node_type(n.get_kind()),
                       n.prettyprint().c_str());
        return NBase::null();
    }

    NBase InfinityCalculator::visit(const Nodecl::Add& n)
    {
        NBase lhs = walk(n.get_lhs());
        NBase rhs = walk(n.get_rhs());
        return boundary_addition(lhs, rhs);
    }

    NBase InfinityCalculator::visit(const Nodecl::Analysis::MinusInfinity& n)
    {
        return n;
    }

    NBase InfinityCalculator::visit(const Nodecl::Analysis::PlusInfinity& n)
    {
        return n;
    }

    NBase InfinityCalculator::visit(const Nodecl::BooleanLiteral& n)
    {
        return n;
    }

    NBase InfinityCalculator::visit(const Nodecl::ComplexLiteral& n)
    {
        return n;
    }

    NBase InfinityCalculator::visit(const Nodecl::Div& n)
    {
        NBase lhs = walk(n.get_lhs());
        NBase rhs = walk(n.get_rhs());
        return boundary_division(lhs, rhs);
    }

    NBase InfinityCalculator::visit(const Nodecl::FloatingLiteral& n)
    {
        return n;
    }

    NBase InfinityCalculator::visit(const Nodecl::IntegerLiteral& n)
    {
        return n;
    }

    NBase InfinityCalculator::visit(const Nodecl::Minus& n)
    {
        NBase lhs = walk(n.get_lhs());
        NBase rhs = walk(n.get_rhs());
        return boundary_subtraction(lhs, rhs);
    }

    NBase InfinityCalculator::visit(const Nodecl::Mul& n)
    {
        NBase lhs = walk(n.get_lhs());
        NBase rhs = walk(n.get_rhs());
        return boundary_multiplication(lhs, rhs);
    }

    NBase InfinityCalculator::visit(const Nodecl::Neg& n)
    {
        NBase rhs = walk(n.get_rhs());
        return Nodecl::Neg::make(rhs, n.get_type());
    }

    NBase InfinityCalculator::visit(const Nodecl::Plus& n)
    {
        NBase rhs = walk(n.get_rhs());
        return Nodecl::Plus::make(rhs, n.get_type());
    }

    NBase InfinityCalculator::visit(const Nodecl::StringLiteral& n)
    {
        return n;
    }

    NBase InfinityCalculator::visit(const Nodecl::Symbol& n)
    {
        return n;
    }

}
}
}
