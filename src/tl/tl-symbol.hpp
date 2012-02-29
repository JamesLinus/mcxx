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




#ifndef TL_SYMBOL_HPP
#define TL_SYMBOL_HPP

#include "tl-common.hpp"
#include <string>
#include <vector>
#include <sstream>
#include "tl-object.hpp"
#include "tl-nodecl-fwd.hpp"
#include "tl-symbol-fwd.hpp"
#include "tl-scope-fwd.hpp"
#include "tl-type-fwd.hpp"
#include "tl-objectlist.hpp"
#include "cxx-gccsupport-decls.h"
#include "cxx-scope.h"

namespace TL
{
    //! \addtogroup Wrap 
    //! @{
    
    //! This class wraps a symbolic entity in the compiler
    class LIBTL_CLASS Symbol : public Object
    {
        public:
            //! Returns an invalid symbol
            static const Symbol invalid();

            //! States whether this is an invalid symbol
            bool is_invalid() const;

            //! States whether this is a valid symbol
            bool is_valid() const;

            Symbol()
                : _symbol(NULL)
            {
            }

            Symbol(scope_entry_t* symbol)
                : _symbol(symbol)
            {
            }

            //! Implements the access to extended attributes of a Symbol
            virtual tl_type_t* get_extended_attribute(const std::string& name) const;
            //! Implements the access to extended attributes of a Symbol
            virtual bool set_extended_attribute(const std::string&, const tl_type_t &data);

            //! Constructs a Symbol after a reference to Object
            Symbol(RefPtr<Object> obj)
            {
                RefPtr<Symbol> pint = RefPtr<Symbol>::cast_dynamic(obj);
                if (pint.get_pointer() != NULL)
                {
                    this->_symbol = pint->_symbol;
                }
                else
                {
                    if (typeid(*obj.get_pointer()) != typeid(Undefined))
                    {
                        std::cerr << "Bad initialization of Symbol" << std::endl;
                    }
                    this->_symbol = NULL;
                }
            }

            //! Gets the type related to this symbol
            Type get_type() const;
            //! Gets the unqualified name of the symbol
            std::string get_name() const;

            //! Returns a fully qualified name
            /*!
             * \remark This function will give bogus names to templates parameters. Use get_qualified_name(Scope)
             * instead.
             */
            std::string get_qualified_name(bool without_template_id = false) const;
            
            //! Returns a fully qualified name
            /*!
             * \param sc Scope used to lookup template parameter names
             */
            std::string get_qualified_name(Scope sc, bool without_template_id = false) const;

            //! Returns the part of the qualified name that involves classes
            std::string get_class_qualification(bool without_template_id = false) const;
            
            //! Returns the part of the qualified name that involves classes
            /*!
             * \param sc Scope used to lookup template parameters names
             */
            std::string get_class_qualification(Scope sc, bool without_template_id = false) const;

            //! Gets the scope where this symbol is defined
            Scope get_scope() const;
    
            //! Gets the scope related to this symbol
            /*
             * The scoping unit introduced by namespaces [C++] and program units [Fortran] and functions [C]
             */
            Scope get_related_scope() const;

            //! Returns the location of the symbol
            std::string get_locus() const;

            //! Returns the filename where the symbol was declared
            std::string get_filename() const;

            //! Returns the line where the symbol was declared
            int get_line() const;

            virtual ~Symbol()
            {
            }

            bool operator<(Symbol s) const;
            bool operator==(Symbol s) const;
            bool operator!=(Symbol s) const;
            Symbol& operator=(Symbol s);

            //! States whether this symbol is a variable
            bool is_variable() const;
            //! States whether this symbol is the result variable
            /*! \note Only meaningful in Fortran */
            bool is_result_variable() const;
            //! States whether this symbol is a label
            bool is_label() const;
            //! States whether this symbol is a typedef
            bool is_typedef() const;
            //! States whether this symbol is a class
            bool is_class() const;
            //! States whether this symbol is an enum name
            bool is_enum() const;
            //! States whether this symbol is an enumerator name
            bool is_enumerator() const;
            //! States whether this symbol is template name
            bool is_template() const;
            //! States whether this symbol is a function
            bool is_function() const;

            //! States whether this symbol is a MODULE PROCEDURE
            /*!
             * A module procedure is a procedure (function or subroutine)
             * defined inside a module. Note that a module procedure returns
             * true for Symbol::is_in_module and Symbol::is_function. The
             * converse is not always true, a Symbol::is_function might be
             * Symbol::is_in_module but not be a module procedure.
             *
             * In the example below, QUUX is a module procedure while FOO is not
             *
             * MODULE M
             *   INTERFACE
             *     SUBROUTINE FOO
             *     END SUBROUTINE FOO
             *   END INTERFACE
             *  CONTAINS
             *   SUBROUTINE QUUX
             *   END SUBROUTINE QUUX
             * END MODULE M
             */
            bool is_module_procedure() const;

            //! States whether this function is nested
            bool is_nested_function() const;
            //! States whether this symbol is a statement function statmeent symbol
            /*! \note This is only meaningful in Fortran */
            bool is_statement_function_statement() const;
            //! States whether this symbol is a template function
            bool is_template_function_name() const;
            //! States whether this symbol is an anonymous union
            bool is_anonymous_union() const;
            //! States that this symbol is the injected class name
            bool is_injected_class_name() const;

            //! States that this symbol is the PROGRAM program unit
            /*! \note This only applies to Fortran */
            bool is_fortran_main_program() const;

            //! States that this symbol is a MODULE program unit
            /*! \note This only applies to Fortran */
            bool is_fortran_module() const;
            
            //! States that this symbol is a component of a MODULE program unit
            /*! \note This only applies to Fortran */
            bool is_in_module() const;

            //! Returns the MODULE symbol where this symbol belongs
            /*! \note This only makes sense if is_in_module returned true */
            Symbol in_module() const;

            //! States that this symbol is available in this program unit because of a USE statement
            /*! \note This only applies to Fortran */
            bool is_from_module() const;
            
            //! States that this symbol is available in this program unit because of a USE statement
            /*! \note This only makes sense if is_from_module returned true */
            Symbol from_module() const;

            //! Returns the original symbol from the module
            /*!
             * Symbols where is_from_module returns true, have an alias to the real symbol
             * of the module. Use this function to get it
             */
            Symbol aliased_from_module() const;

            //! States that this symbol is a BLOCK DATA program unit
            /*! \note This only applies to Fortran */
            bool is_fortran_blockdata() const;
            
            //! States whether this symbol is a parameter of a function
            bool is_parameter() const;

            //! States whether this symbol is a template parameter
            bool is_template_parameter() const;

            //! States whether what was named is a dependent entity
            bool is_dependent_entity() const;

            //! States if this is a member entity
            bool is_member() const;

            //! Gets the byte offset of the storage unit of this member
            /*!
             * Only meaningful for nonstatic variables otherwise it returns 0
             *
             * \note Make sure you have requested the size of the class, otherwise this field
             * will have not been computed yet
             */
            int get_offset() const;

            //! States if this symbol is artificial
            /*!
              See Scope::new_artificial_symbol for more information on
              artificial symbols
              */
            bool is_artificial() const;

            //! Returns the class to which this member belongs
            Type get_class_type() const;

            //! Returns the access specifier of a member or base class
            access_specifier_t get_access_specifier();

            //! Returns the INTENT specifier for a dummy argument
            /*! \note Fortran only */
            intent_kind_t get_intent_kind() const;

            //! States whether this symbol has been initialized
            bool has_initialization() const;
            //! Returns the initialization tree
            Nodecl::NodeclBase get_initialization() const;

            //! Returns the nodecl stored in the field _value
            Nodecl::NodeclBase get_value() const;
            
            //! States whether this symbol is static
            bool is_static() const;
            //! States whether this symbol is register
            bool is_register() const;
            
            //! States whether this symbol is __thread
            bool is_thread() const;

            //! States if this member is a bitfield
            bool is_bitfield() const;
            
            //! Returns the size of the bitfield
            Nodecl::NodeclBase get_bitfield_size() const;

            //! Returns the offset of the first byte used by the bitfield
            /*!
             * This is the offset in bytes where the first bit is laid out
             *
             * struct A
             * {
             *   char c;
             *   int x : 12;
             * };
             *
             * Symbol::get_offset of x will return 0 since a storage unit of an int
             * can start at offset 0 of the struct
             *
             * Symbol::get_bitfield_offset of x will return 1, since the first bit is
             * laid out right after c
             *
             * \note Since one cannot get the address of a bitfield, they do not have
             * a well defined offset. Symbol::get_offset is the offset of the storage unit
             * which they occupy (so they may be apparently sharing the offset with another
             * member, like in the example shown above)
             */
            int get_bitfield_offset() const;

            //! First bit of a bitfield inside its byte
            int get_bitfield_first() const;

            //! Last bit of a bitfield inside its byte
            /*!
             * It will be the same as first if the bitfield is 1 bit wide
             *
             * Do not assume that last is always greater than first, it can be
             * lower. Whether it is lower or bigger depends on the architecture
             */
            int get_bitfield_last() const;

            //! States whether the symbol is user declared
            /*!
             * \note This only applies to member functions
             */
            bool is_user_declared() const;

            //! States whether this symbol is extern
            /*!
            * \bug : This only holds if the 'extern' qualifier was given
            * in the declaration of the symbol but global symbols
            * without it are 'extern' too. Using 'is_static' is better
            * till this gets fixed
            */
            bool is_extern() const;

            //! States whether this symbol is mutable
            bool is_mutable() const;
            // States whether this template is exported
            /*
             * \bug The compiler does not honour this flag (it will always return false)
             */
            bool is_exported_template() const;

            //! States whether this function was defined inline
            bool is_inline() const;

            //! States whether this member function was defined as virtual
            bool is_virtual() const;

            //! States whether this member function is a pure virtual function
            bool is_pure() const;

            //! States whether this member function is a conversion function
            bool is_conversion_function() const;

            //! States whether this member function is a destructor
            bool is_destructor() const;

            //! States whether this member function is a constructor
            bool is_constructor() const;

            //! States whether this member function is a constructor flagged as explicit
            bool is_explicit_constructor() const;

            //! States whether symbol exists just because was mentioned in a friend declaration
            /*!
             * This symbol has not been technically declared by the user but the compiler
             * created it because it appeared in a friend declaration
             */
            bool is_friend_declared() const;
            
            //! States whether this symbol is an ENTRY
            bool is_entry() const;

            //! States whether this function was defined with no exception-specifier
            bool function_throws_any_exception() const;

            //! Returns the thrown exceptions
            /*!
             * \note This function returns empty if function_throws_any_exception is true
             * but it may return empty if it function_throws_any_exception returns false.
             * That latter case means 'throw()'
             */
            ObjectList<TL::Type> get_thrown_exceptions() const;

            //! States whether the symbol has been defined in namespace scope
            bool has_namespace_scope() const;
            //! States whether the symbol has been defined in block scope
            bool has_block_scope() const;
            //! This is an alias for has_block_scope
            bool has_local_scope() const; 
            //! States whether the symbol has been defined in class scope
            /*!
             * This is roughly equivalent to a member symbol
             */
            bool has_class_scope() const;
            //! States whether this symbol has template scope
            /*!
             * Only template parameters should have template scope
             */
            bool has_template_scope() const;
            //! States whether this symbol has prototype scope
            /*!
             * Named parameter declarations of functional declarators have
             * prototype scope only if the declaration itself is not a function
             * definition.
             */
            bool has_prototype_scope() const;

            //! States whether this symbol has gcc attributes
            bool has_gcc_attributes() const;

            //! Special symbol for using A::x inside classes
            bool is_using_symbol() const;

            //! States whether the symbol is actually a builtin of the compiler
            bool is_builtin() const;

            //! States whether the symbol is actually a Fortran intrinsic
            /*!
              This is actually an alias of is_builtin
              */
            bool is_intrinsic() const;

            //! Returns the definition tree
            /*! 
              This is only valid for functions and class symbols.  It will be
              an invalid tree if the function has not been defined or if it is
              an incomplete class type symbol name

              For functions it returns the function definition. For classes the
              class specifier.
             */
            Nodecl::NodeclBase get_definition_tree() const;

            // States whether the symbol is defined 
            /*! This function might not make sense for all kind of symbols
             */
            bool is_defined() const;

            // States whether the symbol is defined inside a class specifier
            bool is_defined_inside_class() const; 

            //! Do not use unless told to do so
            scope_entry_t* get_internal_symbol() const
            {
                return _symbol;
            }

            //! Internal usage
            bool not_to_be_printed() const;

            //! Is a COMMON name
            /*! 
              States whether this symbol is the symbol of a Fortran COMMON name. 

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_fortran_common() const;

            //! This symbol is ALLOCATABLE
            /*! 
              States whether this symbol has the ALLOCATABLE attribute set
              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_allocatable() const;

            //! This symbol is in a common
            /*! 
              States whether this symbol has been defined to be in a COMMON.
              Use get_common to retrieve the COMMON symbol containing this symbol

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_in_common() const;

            //! Returns the COMMON symbol in which this entity belongs
            /*! Only meaningful if is_in_common returned true */
            Symbol in_common() const;

            //! States if this entity is a CRAY pointee
            bool is_cray_pointee() const;

            //! Returns the CRAY pointer of a CRAY pointee
            Symbol get_cray_pointer() const;

            //! This symbol is a NAMELIST
            /*! \note Only meaningful in Fortran */
            bool is_fortran_namelist() const;

            //! This symbol is in a namelist
            /*! 
              States whether this symbol has been defined to be in a NAMELIST
              Use get_common to retrieve the NAMELIST symbol containing this symbol

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_in_namelist() const;

            //! This symbol is OPTIONAL
            /*! 
              States whether this dummy argument is an OPTIONAL dummy argument

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_optional() const;

            //! This Fortran program unit has a global SAVE
            /*!
             * States whether this program unit has a SAVE specifier with an empty name-list
             */
            bool is_saved_program_unit() const;

            //! This symbol is TARGET
            /*! 
              States whether this symbol has the TARGET attribute

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_target() const;

            //! This symbol is ELEMENTAL
            /*! 
              States whether this dummy argument has the ELEMENTAL attribute

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_elemental() const;

            //! This symbol is RECURSIVE
            /*! 
              States whether this dummy argument has the RECURSIVE attribute

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_recursive() const;

            //! This symbol is RESULT
            /*! 
              States whether this symbol is a RESULT attribute

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_result() const;

            //! This symbol is a generic-specifier name
            /*! 
              States whether this symbol is a generic-specifier name

              This function is only meaningful in Fortran. In C/C++ it always returns false
              */
            bool is_generic_specifier() const;

            //! States if this symbol has a linkage different to that of the base language
            bool has_nondefault_linkage() const;

            //! Returns the linkage identifier or empty if is the default
            std::string get_linkage() const;

            //! Returns the symbols related to this one
            /*!
             * The exact set returned depends on the kind of the symbol as kept by the frontend
             */
            ObjectList<TL::Symbol> get_related_symbols() const;

            //! Returns the gcc attributes of this symbol
            ObjectList<GCCAttribute> get_gcc_attributes() const;

            //! __asm__ specifier
            /*!
             * The tree related to the __asm__ specifier. This is a GCC extension
             */
            Nodecl::NodeclBase get_asm_specification() const;

            Nodecl::Symbol make_nodecl(const std::string& filename = "", int line = 0) const;
        private:
            scope_entry_t* _symbol;
    };

    class LIBTL_CLASS GCCAttribute
    {
        private:
            gather_gcc_attribute_t _attr;
        public:
            GCCAttribute(gather_gcc_attribute_t attr) : _attr(attr) { }

            std::string get_attribute_name() const;
            Nodecl::List get_expression_list() const;
    };
    
    //! @}
}

#endif // TL_SYMBOL_HPP
