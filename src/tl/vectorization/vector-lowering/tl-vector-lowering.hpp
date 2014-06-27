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

#ifndef VECTOR_LOWERING_PHASE_HPP
#define VECTOR_LOWERING_PHASE_HPP

#include "tl-compilerphase.hpp"

namespace TL
{
    namespace Vectorization
    {
        class VectorLoweringPhase : public TL::CompilerPhase
        {
            private:
                bool _knc_enabled;
                bool _avx2_enabled;
                bool _prefer_gather_scatter;
                bool _prefer_mask_gather_scatter;

                std::string _knc_enabled_str;
                std::string _avx2_enabled_str;
                std::string _intel_compiler_profile_str;
                std::string _prefer_gather_scatter_str;
                std::string _prefer_mask_gather_scatter_str;

                void set_knc(const std::string knc_enabled_str);
                void set_avx2(const std::string avx2_enabled_str);
                void set_intel_compiler_profile(
                        const std::string intel_compiler_profile_str);
                void set_prefer_gather_scatter(
                        const std::string prefer_gather_scatter_str);
                void set_prefer_mask_gather_scatter(
                        const std::string prefer_mask_gather_scatter_str);

            public:
                VectorLoweringPhase();
                virtual void run(TL::DTO& dto);
        };


        /*
        class VectorLoweringVisitor : public Nodecl::ExhaustiveVisitor<void>
        {
            public:
                VectorLoweringVisitor();

                virtual void visit(const Nodecl::OpenMP::Simd& simd_node);
                virtual void visit(const Nodecl::OpenMP::SimdFunction& simd_node);
        };
        */
    }
}

#endif // VECTOR_LOWERING_PHASE_HPP