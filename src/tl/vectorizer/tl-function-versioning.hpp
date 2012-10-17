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

#ifndef TL_FUNCTION_VERSIONING_HPP
#define TL_FUNCTION_VERSIONING_HPP

#include "tl-nodecl-base.hpp"
#include <map>


namespace TL 
{ 
    namespace Vectorization 
    {
        enum FunctionPriority{ SIMD_FUNC_PRIORITY = 2, DEFAULT_FUNC_PRIORITY = 1, NAIVE_FUNC_PRIORITY = 0};

        class VectorFunctionVersion
        {
            private:
                const Nodecl::NodeclBase _func_version;
                const FunctionPriority _priority;
                const std::string _device;
                const unsigned int _vector_length;
                const TL::Type _target_type;

            public:
                VectorFunctionVersion(const Nodecl::NodeclBase& func_version, 
                        const std::string& device, 
                        const unsigned int vector_length, 
                        const TL::Type& _target_type,
                        const FunctionPriority priority);

                const Nodecl::NodeclBase get_version() const;
                bool has_kind(const std::string& device,
                        const unsigned int vector_length,
                        const TL::Type& target_type) const;
                bool is_better_than(const VectorFunctionVersion& func_version) const;
        };


        class FunctionVersioning
        {
            private:
                typedef std::multimap<const std::string, const VectorFunctionVersion> versions_map_t;
                versions_map_t _versions;

            public:
                FunctionVersioning();

                void add_version(const std::string& func_name, const VectorFunctionVersion& func_version);
                const Nodecl::NodeclBase get_best_version(const std::string& func_name, 
                        const std::string& device,
                        const unsigned int vector_length,
                        const TL::Type& _target_type) const;
        };


    }
}

#endif //TL_FUNCTION_VERSIONING_HPP

