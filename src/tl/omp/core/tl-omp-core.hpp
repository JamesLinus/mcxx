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




#ifndef TL_OMP_CORE_HPP
#define TL_OMP_CORE_HPP

#include <stack>

#include "tl-nodecl.hpp"
#include "tl-compilerphase.hpp"

#include "tl-omp.hpp"
#include "tl-pragmasupport.hpp"
#include "tl-omp-tasks.hpp"

#include "tl-omp-target.hpp"

#include "tl-lexer.hpp"
namespace TL
{
    namespace OpenMP
    {
    	struct UDRParsedInfo 
		{
			Type type;
            Nodecl::NodeclBase combine_expression;
			Symbol in_symbol;
			Symbol out_symbol;

            UDRParsedInfo() : type(NULL), combine_expression(), in_symbol(NULL), out_symbol(NULL) {}
		};

        class Core : public TL::PragmaCustomCompilerPhase
        {
            private:
                void parse_new_udr(const std::string& str);

                void register_omp_constructs();


                // Handler functions
#define OMP_DIRECTIVE(_directive, _name, _pred) \
                void _name##_handler_pre(TL::PragmaCustomDirective); \
                void _name##_handler_post(TL::PragmaCustomDirective);
#define OMP_CONSTRUCT(_directive, _name, _pred) \
                void _name##_handler_pre(TL::PragmaCustomStatement); \
                void _name##_handler_post(TL::PragmaCustomStatement); \
                void _name##_handler_pre(TL::PragmaCustomDeclaration); \
                void _name##_handler_post(TL::PragmaCustomDeclaration); 
#define OMP_CONSTRUCT_NOEND(_directive, _name, _pred) \
                OMP_CONSTRUCT(_directive, _name, _pred)
#include "tl-omp-constructs.def"
                // Section is special
                OMP_CONSTRUCT("section", section, true)
#undef OMP_CONSTRUCT
#undef OMP_CONSTRUCT_NOEND
#undef OMP_DIRECTIVE

                static bool _already_registered;

                RefPtr<OpenMP::Info> _openmp_info;
                RefPtr<OpenMP::FunctionTaskSet> _function_task_set;

                std::stack<TargetContext> _target_context;

                void common_target_handler_pre(TL::PragmaCustomLine pragma_line, 
                        TargetContext& target_ctx,
                        TL::Scope scope);

                void task_function_handler_pre(TL::PragmaCustomDeclaration construct);
                void task_inline_handler_pre(TL::PragmaCustomStatement construct);

                void get_clause_symbols(
                        PragmaCustomClause clause, 
                        const TL::ObjectList<TL::Symbol> &symbols_in_construct,
                        ObjectList<DataReference>& data_ref_list,
                        bool allow_extended_references = false);
                void get_reduction_symbols(TL::PragmaCustomLine construct, 
                        PragmaCustomClause clause, 
                        const TL::ObjectList<TL::Symbol> &symbols_in_construct,
                        ObjectList<ReductionSymbol>& sym_list);
                void get_data_explicit_attributes(TL::PragmaCustomLine construct,
                        Nodecl::NodeclBase statements,
                        DataSharingEnvironment& data_sharing);
                void get_data_implicit_attributes(TL::PragmaCustomStatement construct, 
                        DataSharingAttribute default_data_attr, 
                        DataSharingEnvironment& data_sharing);
                void get_data_implicit_attributes_task(TL::PragmaCustomStatement construct,
                        DataSharingEnvironment& data_sharing,
                        DataSharingAttribute default_data_attr);

                void get_target_info(TL::PragmaCustomLine pragma_line,
                        DataSharingEnvironment& data_sharing);
                void get_dependences_info(PragmaCustomLine construct, 
                        DataSharingEnvironment& data_sharing);
                void get_dependences_info_clause(PragmaCustomClause clause,
                        DataSharingEnvironment& data_sharing,
                        DependencyDirection dep_attr);

                DataSharingAttribute get_default_data_sharing(TL::PragmaCustomLine construct,
                        DataSharingAttribute fallback_data_sharing);

                void common_parallel_handler(TL::PragmaCustomStatement ctr, DataSharingEnvironment& data_sharing);
                void common_for_handler(Nodecl::NodeclBase nodecl, DataSharingEnvironment& data_sharing);
                void common_workshare_handler(TL::PragmaCustomStatement construct, DataSharingEnvironment& data_sharing);

				RealTimeInfo task_real_time_handler_pre(TL::PragmaCustomLine construct);

                void fix_sections_layout(TL::PragmaCustomStatement construct, const std::string& pragma_name);

                void collapse_loop_first(Nodecl::NodeclBase& construct);

                void parse_declare_reduction(ReferenceScope ref_sc, const std::string& declare_reduction_src);
                void parse_declare_reduction(ReferenceScope ref_sc, Source declare_reduction_src);
                void parse_declare_reduction(ReferenceScope ref_sc,
                        const std::string &name,
                        const std::string &typenames,
                        const std::string &combiner,
                        const std::string &initializer);

                void initialize_builtin_reductions(Scope sc);

                ObjectList<Nodecl::NodeclBase> update_clauses(const ObjectList<Nodecl::NodeclBase>& clauses,
                           TL::Symbol function_symbol);
            public:
                Core();

                virtual void run(TL::DTO& dto);
                virtual void pre_run(TL::DTO& dto);

                virtual void phase_cleanup(TL::DTO& data_flow);

                virtual ~Core() { }

                RefPtr<OpenMP::Info> get_openmp_info();


                //! Used when parsing declare reduction
                static bool _silent_declare_reduction;
        };

        // OpenMP core is a one shot phase, so even if it is in the compiler
        // pipeline twice, it will only run once by default.
        // Call this function to reenable openmp_core. Use this function
        // when you are sure that your changes require a full OpenMP analysis
        void openmp_core_run_next_time(DTO& dto);
    }
}

#endif // TL_OMP_CORE_HPP
