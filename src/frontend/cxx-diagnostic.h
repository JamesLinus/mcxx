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

#ifndef CXX_DIAGNOSTIC_H
#define CXX_DIAGNOSTIC_H

#include "cxx-macros.h"

MCXX_BEGIN_DECLS

void diagnostics_reset(void);
int diagnostics_get_error_count(void);
int diagnostics_get_warn_count(void);

void error_printf(const char* format, ...) CHECK_PRINTF(1,2);
void warn_printf(const char* format, ...)  CHECK_PRINTF(1,2);
void info_printf(const char* format, ...)  CHECK_PRINTF(1,2);

MCXX_END_DECLS

#endif // CXX_DIAGNOSTIC_H
