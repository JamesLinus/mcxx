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


#include "tl-source.hpp"
#include "tl-lowering-visitor.hpp"
#include "tl-nodecl-utils.hpp"

namespace TL { namespace Nanox {

    void LoweringVisitor::visit(const Nodecl::OpenMP::Single& construct)
    {
        Nodecl::NodeclBase environment = construct.get_environment();
        Nodecl::NodeclBase statements = construct.get_statements();

        walk(statements);

        statements = construct.get_statements();

        Nodecl::NodeclBase placeholder;

        TL::Source transform_code, final_barrier;
        transform_code
            << "{"
            << as_type(::get_bool_type()) << " single_guard;"
            << "nanos_err_t err = nanos_omp_single(&single_guard);"
            << "if (err != NANOS_OK) nanos_handle_error(err);"

            << "if (single_guard)"
            << "{"
            << statement_placeholder(placeholder)
            << "}"
            << "}"
            ;

        FORTRAN_LANGUAGE()
        {
            // Parse in C
            Source::source_language = SourceLanguage::C;
        }
        Nodecl::NodeclBase n = transform_code.parse_statement(construct);
        FORTRAN_LANGUAGE()
        {
            Source::source_language = SourceLanguage::Current;
        }

        Nodecl::Utils::SimpleSymbolMap empty_symbol_map;
        Nodecl::NodeclBase copied_statements = Nodecl::Utils::deep_copy(statements, placeholder, empty_symbol_map);
        placeholder.integrate(copied_statements);

        construct.integrate(n);
    }

} }
