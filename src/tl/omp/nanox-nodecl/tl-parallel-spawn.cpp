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

namespace TL { namespace Nanox {

    void LoweringVisitor::parallel_spawn(
            OutlineInfo& outline_info,
            Nodecl::NodeclBase construct,
            Nodecl::NodeclBase num_replicas,
            const std::string& outline_name,
            TL::Symbol structure_symbol)
    {
        Source nanos_create_wd,
        nanos_create_wd_and_run,
        immediate_decl;

        Symbol current_function = Nodecl::Utils::get_enclosing_function(construct);

        Nodecl::NodeclBase code = current_function.get_function_code();

        Nodecl::Context context = (code.is<Nodecl::TemplateFunctionCode>())
            ? code.as<Nodecl::TemplateFunctionCode>().get_statements().as<Nodecl::Context>()
            : code.as<Nodecl::FunctionCode>().get_statements().as<Nodecl::Context>();

        TL::Scope function_scope = context.retrieve_context();
        Source struct_arg_type_name;

        struct_arg_type_name
            << ((structure_symbol.get_type().is_template_specialized_type()
                        &&  structure_symbol.get_type().is_dependent()) ? "typename " : "")
            << structure_symbol.get_qualified_name(function_scope);

        Source struct_size;
        Source dynamic_size;
        struct_size << "sizeof(imm_args)" << dynamic_size;

        // Fill argument structure
        allocate_immediate_structure(
                outline_info,
                struct_arg_type_name,
                struct_size,
                // out
                immediate_decl,
                dynamic_size);

        Source translation_fun_arg_name, xlate_function_name;

        translation_fun_arg_name << "(nanos_translate_args_t)0";

        Source copy_ol_decl,
               copy_ol_arg, 
               copy_ol_setup,
               copy_imm_arg,
               copy_imm_setup;

        Source num_dependences;

        nanos_create_wd
            << "nanos_create_wd_compact("
            <<       "&nanos_wd_, "
            <<       "&nanos_wd_const_data.base, "
            <<       "&dyn_props, "
            <<       struct_size << ", "
            <<       "(void**)&ol_args, "
            <<       "nanos_current_wd(), "
            <<       copy_ol_arg << ");"
            ;

        nanos_create_wd_and_run
            << "nanos_create_wd_and_run_compact("
            <<       "&nanos_wd_const_data.base, "
            <<       "&dyn_props, "
            <<       struct_size << ", "
            <<       "&imm_args,"
            <<       num_dependences << ", "
            <<       "dependences, "
            <<       copy_imm_arg << ", "
            <<       translation_fun_arg_name << ");"
            ;

        Source const_wd_info;
        const_wd_info << fill_const_wd_info(struct_arg_type_name,
                /* is_untied */ false,
                /* mandatory_creation */ true,
                /* wd_description */ current_function.get_name(),
                outline_info,
                construct);

        Source num_threads;
        if (num_replicas.is_null())
        {
            num_threads << "nanos_omp_get_max_threads()";
        }
        else
        {
            num_threads << as_expression(num_replicas);
        }

        Nodecl::NodeclBase fill_outline_arguments_tree,
            fill_dependences_outline_tree;
        Source fill_outline_arguments,
               fill_dependences_outline;

        Nodecl::NodeclBase fill_immediate_arguments_tree,
            fill_dependences_immediate_tree;
        Source fill_immediate_arguments,
               fill_dependences_immediate;

        Source dependence_type;
        dependence_type
            << "nanos_data_access_t*";

        Source spawn_code;
        spawn_code
            << "{"
            <<   const_wd_info
            <<   immediate_decl
            <<   "unsigned int nanos_num_threads = " << num_threads << ";"
            <<   "nanos_err_t err;"
            <<   "nanos_team_t nanos_team = (nanos_team_t)0;"
            <<   "nanos_thread_t nanos_team_threads[nanos_num_threads];"
            <<   "err = nanos_create_team(&nanos_team, (nanos_sched_t)0, &nanos_num_threads,"
            <<              "(nanos_constraint_t*)0, /* reuse_current */ 1, nanos_team_threads);"
            <<   "if (err != NANOS_OK) nanos_handle_error(err);"
            <<   "nanos_wd_dyn_props_t dyn_props;"
            <<   "dyn_props.tie_to = (nanos_thread_t)0;"
            <<   "dyn_props.priority = 0;"
            <<   "unsigned int nth_i;"
            <<   "for (nth_i = 1; nth_i < nanos_num_threads; nth_i = nth_i + 1)"
            <<   "{"
            //   We have to create a nanos_wd_ tied to a thread
            <<      "dyn_props.tie_to = nanos_team_threads[nth_i];"
            <<      struct_arg_type_name << " *ol_args = 0;"
            <<      "nanos_wd_t nanos_wd_ = (nanos_wd_t)0;"
            <<      copy_ol_decl
            <<      "err = " << nanos_create_wd
            <<      "if (err != NANOS_OK) nanos_handle_error(err);"
            // This is a placeholder because arguments are filled using the base language (possibly Fortran)
            <<      statement_placeholder(fill_outline_arguments_tree)
            <<      fill_dependences_outline
            <<      copy_ol_setup
            <<      "err = nanos_submit(nanos_wd_, 0, (" <<  dependence_type << ") 0, (nanos_team_t)0);"
            <<      "if (err != NANOS_OK) nanos_handle_error(err);"
            <<   "}"
            <<   "dyn_props.tie_to = nanos_team_threads[0];"
            // This is a placeholder because arguments are filled using the base language (possibly Fortran)
            <<   statement_placeholder(fill_immediate_arguments_tree)
            <<   fill_dependences_immediate
            <<   copy_imm_setup
            <<   "err = " << nanos_create_wd_and_run
            <<   "if (err != NANOS_OK) nanos_handle_error(err);"
            <<   "err = nanos_end_team(nanos_team);"
            <<   "if (err != NANOS_OK) nanos_handle_error(err);"
            << "}"
            ;

        fill_arguments(construct, outline_info, fill_outline_arguments, fill_immediate_arguments);

        // Fill dependences for outline
        num_dependences << count_dependences(outline_info);

        int num_copies = 0;
        fill_copies(construct,
                outline_info,
                /* parameter_outline_info */ NULL,
                structure_symbol,

                num_copies,
                copy_ol_decl,
                copy_ol_arg,
                copy_ol_setup,
                copy_imm_arg,
                copy_imm_setup,
                xlate_function_name);

        fill_dependences(construct, 
                outline_info, 
                /* accessor */ Source("ol_args->"),
                fill_dependences_outline);
        fill_dependences(construct, 
                outline_info, 
                /* accessor */ Source("imm_args."),
                fill_dependences_immediate);


        FORTRAN_LANGUAGE()
        {
            // Parse in C
            Source::source_language = SourceLanguage::C;
        }
        Nodecl::NodeclBase spawn_code_tree = spawn_code.parse_statement(construct);
        FORTRAN_LANGUAGE()
        {
            Source::source_language = SourceLanguage::Current;
        }

        if (!fill_outline_arguments.empty())
        {
            Nodecl::NodeclBase new_tree = fill_outline_arguments.parse_statement(fill_outline_arguments_tree);
            fill_outline_arguments_tree.replace(new_tree);
        }

        if (!fill_immediate_arguments.empty())
        {
            Nodecl::NodeclBase new_tree = fill_immediate_arguments.parse_statement(fill_immediate_arguments_tree);
            fill_immediate_arguments_tree.replace(new_tree);
        }

        construct.replace(spawn_code_tree);
    }
} }
