/*--------------------------------------------------------------------
 ( C) Copyright 2006-2013 Barcelona Supercomputing Center             *
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

#include <limits>

#include "cxx-cexpr.h"

#include "tl-nodecl-calc.hpp"
#include "tl-range-analysis-utils.hpp"

namespace TL {
namespace Analysis {
namespace Utils {

    Optimizations::Calculator calc;
    
    // A[al, au] − B[bl, bu] =  ∅, A and B completely overlap or B contains A
    //                          A, A and B do not overlap
    //                          [al, bl] U [bu, au], A contains B
    //                          [al, bl], al < bl
    //                          [au, bu], al > bl
    // When the operation cannot be computed, we return the nodecl representing the operation
    NBase range_sub(const NBase& r1, const NBase& r2)
    {
        if(r1.is<Nodecl::Analysis::EmptyRange>() || r2.is<Nodecl::Analysis::EmptyRange>())
            return r1.shallow_copy();
        
        ERROR_CONDITION((!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>()), 
                        "range_sub operation can only be applied to ranges, but parameters are '%s' and '%s'.\n", 
                        ast_print_node_type(r1.get_kind()), ast_print_node_type(r2.get_kind()));
        
        NBase result;
        
        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();
        TL::Type t = r1_lb.get_type();
        
        if( ( r1_lb == r2_lb ) && ( r1_ub == r2_ub ) )
        {   // The two ranges are exactly the same
            result = Nodecl::Analysis::EmptyRange::make();
        }
        else if( r1_lb.is_constant() && r1_ub.is_constant() && r2_lb.is_constant() && r2_ub.is_constant() )
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
                const_value_t* one = const_value_get_one(/*bytes*/ 4, /*signed*/ 1);
                NBase one_nodecl = NBase(const_value_to_nodecl(one));
                if(const_value_is_zero(lb_dif) && const_value_is_zero(ub_dif))
                {   // r1 and r2 are the same range
                    result = Nodecl::Analysis::EmptyRange::make();
                }
                else 
                {
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
                                                    one_nodecl.shallow_copy(), t);
                        const_value_t* new_right_lb_c = const_value_add(r2_ub_c, one);
                        Nodecl::Range right_range = 
                                Nodecl::Range::make(NBase(const_value_to_nodecl(new_right_lb_c)), r1_ub.shallow_copy(), 
                                                    one_nodecl.shallow_copy(), t);
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
                        result = Nodecl::Range::make(lb, ub, one_nodecl.shallow_copy(), t);
                    }
                }
            }
        }
        else
        {   // We are not able to synthesize the result
            result = Nodecl::Analysis::RangeSub::make(r1, r2, t);
        }
        
        return result;
    }
    
    // A[al, au] ∩ B[bl, bu] = (al<bl<au || al<bu<au) ? [max(al, bl), min(au, bu)]      -> overlap
    //                                                : ∅
    NBase range_intersection(const NBase& r1, const NBase& r2)
    {
        if(r1.is<Nodecl::Analysis::EmptyRange>())
            return r2.shallow_copy();
        else if(r2.is<Nodecl::Analysis::EmptyRange>())
            return r1.shallow_copy();
        
        ERROR_CONDITION((!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>()), 
                        "range_intersection operation can only be applied to ranges, but parameters are '%s' and '%s'.\n", 
                        ast_print_node_type(r1.get_kind()), ast_print_node_type(r2.get_kind()));
        
        NBase result;

        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();
        TL::Type t = r1_lb.get_type();
        
        if( ( r1_lb == r2_lb ) && ( r1_ub == r2_ub ) )
        {   // The two ranges are exactly the same
            result = r1.shallow_copy( );
        }
        else if( r1_lb.is_constant() && r1_ub.is_constant() && r2_lb.is_constant() && r2_ub.is_constant() )
        {   // Let's compare the constant values
            const_value_t* r1_lb_c = const_value_cast_to_signed_int_value(r1_lb.get_constant());
            const_value_t* r1_ub_c = const_value_cast_to_signed_int_value(r1_ub.get_constant());
            const_value_t* r2_lb_c = const_value_cast_to_signed_int_value(r2_lb.get_constant());
            const_value_t* r2_ub_c = const_value_cast_to_signed_int_value(r2_ub.get_constant());
            
            if( const_value_is_positive( const_value_sub( r1_lb_c, r2_ub_c ) ) || 
                const_value_is_positive( const_value_sub( r2_lb_c, r1_ub_c ) ) )
            {   // r1 and r2 do not overlap
                result = Nodecl::Analysis::EmptyRange::make();
            }
            else if( const_value_is_zero( const_value_sub( r1_lb_c, r2_lb_c ) ) && 
                     const_value_is_zero( const_value_sub( r1_ub_c, r2_ub_c ) ) )
            {   // r1 and r2 are the same range
                result = r1.shallow_copy();
            }
            else
            {   // r1 and r2 overlap in some way
                NBase lb, ub ;
                if( const_value_is_positive( const_value_sub( r1_lb_c, r2_lb_c ) ) )
                {
                    lb = r1_lb.shallow_copy();
                    ub = r2_ub.shallow_copy();
                }
                else
                {
                    lb = r2_lb.shallow_copy();
                    ub = r1_ub.shallow_copy();
                }
                const_value_t* one = const_value_get_one(/*bytes*/ 4, /*signed*/ 1);
                result = Nodecl::Range::make( lb, ub, NBase(const_value_to_nodecl(one)), t );
            }
        }
        else
        {   // We are not able to synthesize the result
            result = Nodecl::Analysis::RangeIntersection::make( r1.shallow_copy(), r2.shallow_copy(), t );
        }
        
        return result;
    }
    
    // A[al, au] ∪ B[bl, bu] = (al<bl<au || al<bu<au) ? [min(al, bl), max(au, bu)]      -> overlap
    //                                                : [al, au] ∪ [bl, bu]             -> cannot synthesize the result as a unique range
    NBase range_union( const NBase& r1, const NBase& r2 )
    {
        if(r1.is<Nodecl::Analysis::EmptyRange>() && r2.is<Nodecl::Analysis::EmptyRange>())
            return r1.shallow_copy();
        
        ERROR_CONDITION((!r1.is<Nodecl::Range>() || !r2.is<Nodecl::Range>()), 
                        "range_union operation can only be applied to ranges, but parameters are '%s' and '%s'.\n", 
                        ast_print_node_type(r1.get_kind()), ast_print_node_type(r2.get_kind()));
        
        NBase result;
        
        NBase r1_lb = r1.as<Nodecl::Range>().get_lower();
        NBase r1_ub = r1.as<Nodecl::Range>().get_upper();
        NBase r2_lb = r2.as<Nodecl::Range>().get_lower();
        NBase r2_ub = r2.as<Nodecl::Range>().get_upper();
        TL::Type t = r1_lb.get_type();
        
        if( ( r1_lb == r2_lb ) && ( r1_ub == r2_ub ) )
        {   // The two ranges are exactly the same
            result = r1.shallow_copy( );
        }
        else if( r1_lb.is_constant() && r1_ub.is_constant() && r2_lb.is_constant() && r2_ub.is_constant() )
        {
            const_value_t* r1_lb_c = const_value_cast_to_signed_int_value(r1_lb.get_constant());
            const_value_t* r1_ub_c = const_value_cast_to_signed_int_value(r1_ub.get_constant());
            const_value_t* r2_lb_c = const_value_cast_to_signed_int_value(r2_lb.get_constant());
            const_value_t* r2_ub_c = const_value_cast_to_signed_int_value(r2_ub.get_constant());
            
            if( const_value_is_positive( const_value_sub( r1_lb_c, r2_ub_c ) ) || 
                const_value_is_positive( const_value_sub( r2_lb_c, r1_ub_c ) ) )
            {   // r1 and r2 do not overlap
                result = Nodecl::Analysis::RangeUnion::make( r1.shallow_copy(), r2.shallow_copy(), t );
            }
            else if( const_value_is_zero( const_value_sub( r1_lb_c, r2_lb_c ) ) && 
                     const_value_is_zero( const_value_sub( r1_ub_c, r2_ub_c ) ) )
            {   // r1 and r2 are the same range
                result = r1.shallow_copy();
            }
            else
            {   // r1 and r2 overlap in some way
                NBase lb = const_value_is_positive( const_value_sub( r1_lb_c, r2_lb_c ) ) ? r2_lb.shallow_copy() 
                                                                                                       : r1_lb.shallow_copy();
                NBase ub = const_value_is_positive( const_value_sub( r1_ub_c, r2_ub_c ) ) ? r1_ub.shallow_copy() 
                                                                                                       : r2_ub.shallow_copy();
                const_value_t* one = const_value_get_one(/*bytes*/ 4, /*signed*/ 1);
                result = Nodecl::Range::make( lb, ub, NBase(const_value_to_nodecl(one)), t );
            }
        }
        else
        {
            result = Nodecl::Analysis::RangeUnion::make( r1.shallow_copy(), r2.shallow_copy(), t );
        }
        
        return result;
    }

    // ******************************************************************************************* //
    // ******************************* Range Analysis Constraints ******************************** //
    
    Constraint::Constraint()
        : _constr_sym(Symbol()), _constraint(NBase::null())
    {}
        
    Constraint::Constraint( const TL::Symbol& constr_sym, const NBase& constraint )
        : _constr_sym( constr_sym ), _constraint( constraint )
    {}
        
    TL::Symbol Constraint::get_symbol() const 
    {
        return _constr_sym;
    }
        
    NBase Constraint::get_constraint() const 
    {
        return _constraint;
    }
        
    bool Constraint::operator!=(const Constraint& c) const
    {
        return ((this->_constr_sym != c._constr_sym) ||
                !Nodecl::Utils::structurally_equal_nodecls(this->_constraint, c._constraint, /*skip_conversion_nodes*/true));
    }
    
    bool Constraint::operator==(const Constraint& c) const
    {
        return ((this->_constr_sym == c._constr_sym) &&
                Nodecl::Utils::structurally_equal_nodecls(this->_constraint, c._constraint, /*skip_conversion_nodes*/true));
    }
    
    // ***************************** END Range Analysis Constraints ****************************** //
    // ******************************************************************************************* //
    
std::string prettyprint_range_values_map(Utils::RangeValuesMap s, bool print_in_dot )
    {
        std::string result = "";
        int line_size = 0;
        for(Utils::RangeValuesMap::iterator it = s.begin(); it != s.end(); ++it)
        {
            std::string it_str = it->first.prettyprint() + "= {";
                ObjectList<Utils::RangeValue_tag> values = it->second;
                for(ObjectList<Utils::RangeValue_tag>::iterator itv = values.begin(); itv != values.end();)
                {
                    if(!itv->n->is_null())
                        it_str += itv->n->prettyprint();
                    else
                    {
                        NBase lb = itv->iv->get_lb();
                        NBase ub = itv->iv->get_ub();
                        NBase incr = itv->iv->get_increment();

                        it_str += "[ " + (lb.is_null()   ? "NULL" : lb.prettyprint())
                                + ":"  + (ub.is_null()   ? "NULL" : ub.prettyprint())
                                + ":"  + (incr.is_null() ? "NULL" : incr.prettyprint())
                                + ":"   + itv->iv->get_type_as_string() + " ]";
                    }

                    ++itv;
                    if(itv != values.end())
                        it_str += ", ";
                }
                it_str += "}; ";

                if(line_size + it_str.size() > 100)
                {
                    result += "$$";
                    line_size = it_str.size();
                }
                else
                    line_size += it_str.size();
                result += it_str;
                if(line_size > 100)
                    result += "$$";
        }

        if(!result.empty())
        {
            result = result.substr(0, result.size() - 2);
            if(print_in_dot)
                makeup_dot_block(result);
        }

        return result;
    }
    
}
}
}