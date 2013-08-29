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

#ifndef TL_OMP_SIMD_HPP
#define TL_OMP_SIMD_HPP

#include "tl-pragmasupport.hpp"
#include "tl-vectorizer.hpp"

namespace TL
{
    namespace OpenMP
    {
        //! This class transforms 
        class Simd : public TL::PragmaCustomCompilerPhase
        {
            public:
                Simd();

                virtual void run(TL::DTO& dto);
                virtual void pre_run(TL::DTO& dto);

                virtual ~Simd() { }

            private:
                std::string _simd_enabled_str;
                std::string _svml_enabled_str;
                std::string _fast_math_enabled_str;
                std::string _mic_enabled_str;

                bool _simd_enabled;
                bool _svml_enabled;
                bool _fast_math_enabled;
                bool _mic_enabled;

                void set_simd(const std::string simd_enabled_str);
                void set_svml(const std::string svml_enabled_str);
                void set_fast_math(const std::string fast_math_enabled_str);
                void set_mic(const std::string set_mic_enabled_str);
        };

        class SimdVisitor : public Nodecl::ExhaustiveVisitor<void>
        {
            private:
                TL::Vectorization::Vectorizer& _vectorizer;

                unsigned int _vector_length;
                unsigned int _mask_size;
                std::string _device_name;
                bool _support_masking;

                void process_suitable_clause(const Nodecl::List& environment,
                        Nodecl::List& suitable_expressions);
                void process_vectorlengthfor_clause(const Nodecl::List& environment, 
                        TL::Type& vectorlengthfor_type);
                Nodecl::List process_reduction_clause(const Nodecl::List& environment,
                        TL::ObjectList<TL::Symbol>& reductions,
                        std::map<TL::Symbol, TL::Symbol>& new_external_vector_symbol_map,
                        const Nodecl::ForStatement& for_statement);

            public:
                SimdVisitor(bool fast_math_enabled, bool svml_enabled, bool mic_enabled);
                
                virtual void visit(const Nodecl::OpenMP::Simd& simd_node);
                virtual void visit(const Nodecl::OpenMP::SimdFor& simd_node);
                virtual void visit(const Nodecl::OpenMP::SimdFunction& simd_node);
                virtual Nodecl::FunctionCode common_simd_function(const Nodecl::OpenMP::SimdFunction& simd_node,
                        const Nodecl::FunctionCode& function_code,
                        const Nodecl::List& suitable_expresions,
                        const TL::Type& vectorlengthfor_type,
                        const bool masked_version);
        };

        class FunctionDeepCopyFixVisitor : public Nodecl::ExhaustiveVisitor<void>
        {
            private:
                const TL::Symbol& _orig_symbol;
                const TL::Symbol& _new_symbol;

            public:
                FunctionDeepCopyFixVisitor(const TL::Symbol& orig_symbol,
                        const TL::Symbol& new_symbol);
                
                virtual void visit(const Nodecl::Symbol& n);
        };
    }
}

#endif // TL_OMP_SIMD_HPP
