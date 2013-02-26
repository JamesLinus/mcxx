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



#ifndef TL_TEST_ANALYSIS_PHASE_HPP
#define TL_TEST_ANALYSIS_PHASE_HPP

#include "tl-compilerphase.hpp"
#include "tl-nodecl-visitor.hpp"

namespace TL {
namespace Analysis {

    //! Phase that allows testing compiler analysis
    class LIBTL_CLASS TestAnalysisPhase : public CompilerPhase
    {
    private:
        std::string _pcfg_enabled_str;
        bool _pcfg_enabled;
        void set_pcfg( const std::string pcfg_enabled_str );

        std::string _use_def_enabled_str;
        bool _use_def_enabled;
        void set_use_def( const std::string use_def_enabled_str );

        std::string _liveness_enabled_str;
        bool _liveness_enabled;
        void set_liveness( const std::string liveness_enabled_str );

        std::string _reaching_defs_enabled_str;
        bool _reaching_defs_enabled;
        void set_reaching_defs( const std::string reaching_defs_enabled_str );

        std::string _induction_vars_enabled_str;
        bool _induction_vars_enabled;
        void set_induction_vars( const std::string induction_vars_enabled_str );

        std::string _auto_scope_enabled_str;
        bool _auto_scope_enabled;
        void set_auto_scope( const std::string auto_scope_enabled_str );

    public:
        //! Constructor of this phase
        TestAnalysisPhase();

        //!Entry point of the phase
        virtual void run(TL::DTO& dto);
    };
}
}

#endif  // TL_TEST_ANALYSIS_PHASE_HPP
