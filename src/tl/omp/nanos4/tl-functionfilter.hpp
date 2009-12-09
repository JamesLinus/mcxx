/*--------------------------------------------------------------------
  (C) Copyright 2006-2009 Barcelona Supercomputing Center 
                          Centro Nacional de Supercomputacion
  
  This file is part of Mercurium C/C++ source-to-source compiler.
  
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

#ifndef TL_INSTRUMENTFILTER_HPP
#define TL_INSTRUMENTFILTER_HPP

#include <set>
#include <string>

namespace TL
{
    namespace Nanos4
    {
        class FunctionFilterFile 
        {
            private:
                bool _filter_inverted;
                std::set<std::string> _filter_set;
            public:
                FunctionFilterFile() { }
                bool match(const std::string& function_name);

                void set_inverted(bool b);
                void init(const std::string& filter_file_name, const std::string& filter_mode_var);
        };
    }
}

#endif // TL_INSTRUMENTFILTER_HPP