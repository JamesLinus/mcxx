/*--------------------------------------------------------------------
  (C) Copyright 2006-2011 Barcelona Supercomputing Center 
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



#include "tl-omp-core.hpp"
#include "tl-nodecl-alg.hpp"

namespace TL
{
    namespace OpenMP
    {
        FunctionTaskTargetInfo::FunctionTaskTargetInfo()
            : _copy_in(),
            _copy_out(),
            _copy_inout(),
            _device_list(),
            _copy_deps()
        {
        }

        bool FunctionTaskTargetInfo::can_be_ommitted()
        {
            return _copy_in.empty()
                && _copy_out.empty()
                && _copy_inout.empty()
                && _device_list.empty();
        }

        void FunctionTaskTargetInfo::set_copy_in(const ObjectList<CopyItem>& copy_items)
        {
            _copy_in = copy_items;
        }

        void FunctionTaskTargetInfo::set_copy_out(const ObjectList<CopyItem>& copy_items)
        {
            _copy_out = copy_items;
        }

        void FunctionTaskTargetInfo::set_copy_inout(const ObjectList<CopyItem>& copy_items)
        {
            _copy_inout = copy_items;
        }

        ObjectList<CopyItem> FunctionTaskTargetInfo::get_copy_in() const
        {
            return _copy_in;
        }

        ObjectList<CopyItem> FunctionTaskTargetInfo::get_copy_out() const
        {
            return _copy_out;
        }

        ObjectList<CopyItem> FunctionTaskTargetInfo::get_copy_inout() const
        {
            return _copy_inout;
        }

        void FunctionTaskTargetInfo::set_copy_deps(bool b)
        {
            _copy_deps = b;
        }

        bool FunctionTaskTargetInfo::has_copy_deps() const
        {
            return _copy_deps;
        }

        void FunctionTaskTargetInfo::set_device_list(const ObjectList<std::string>& device_list)
        {
            _device_list = device_list;
        }

        ObjectList<std::string> FunctionTaskTargetInfo::get_device_list() const
        {
            return _device_list;
        }


        FunctionTaskInfo::FunctionTaskInfo(Symbol sym,
                ObjectList<FunctionTaskDependency> parameter_info,
                FunctionTaskTargetInfo target_info)
            : _sym(sym),
            _parameters(parameter_info),
            _implementation_table(),
            _target_info(target_info)
        {
        }

        Symbol FunctionTaskInfo::get_symbol() const
        {
            return _sym;
        }

        FunctionTaskInfo::implementation_table_t FunctionTaskInfo::get_implementation_table() const
        {
           return _implementation_table;
        }
        
        ObjectList<Symbol> FunctionTaskInfo::get_involved_parameters() const
        {
            ObjectList<Symbol> result;

            for (ObjectList<FunctionTaskDependency>::const_iterator it = _parameters.begin();
                    it != _parameters.end();
                    it++)
            {
                Nodecl::NodeclBase expr(it->get_data_reference());

                ObjectList<Symbol> current_syms = Nodecl::Utils::get_all_symbols(expr);
                result.insert(current_syms);
            }

            return result;
        }

        ObjectList<FunctionTaskDependency> FunctionTaskInfo::get_parameter_info() const
        {
            return _parameters;
        }

        void FunctionTaskInfo::add_device(const std::string& device_name)
        {
            _implementation_table[device_name] = Symbol(NULL);
        }

        void FunctionTaskInfo::add_device_with_implementation(
                const std::string& device_name,
                Symbol implementor_symbol)
        {
            _implementation_table[device_name] = implementor_symbol;
        }

        ObjectList<std::string> FunctionTaskInfo::get_all_devices()
        {
            ObjectList<std::string> result;
            for (implementation_table_t::iterator it = _implementation_table.begin();
                    it != _implementation_table.end();
                    it++)
            {
                result.append(it->first);
            }

            return result;
        }

        ObjectList<FunctionTaskInfo::implementation_pair_t> FunctionTaskInfo::get_devices_with_implementation() const
        {
            ObjectList<implementation_pair_t> result;

            for (implementation_table_t::const_iterator it = _implementation_table.begin();
                    it != _implementation_table.end();
                    it++)
            {
                if (it->second.is_valid())
                {
                    implementation_pair_t pair(*it);
                    result.append(pair);
                }
            }

            return result;
        }

        void FunctionTaskInfo::set_real_time_info(const RealTimeInfo & rt_info)
        {
            _real_time_info = rt_info;
        }

        RealTimeInfo FunctionTaskInfo::get_real_time_info()
        {
            return _real_time_info;
        }
        
        void FunctionTaskInfo::set_if_clause_conditional_expression(Nodecl::NodeclBase expr)
        {
            _if_clause_cond_expr = expr;
        }
        
        Nodecl::NodeclBase FunctionTaskInfo::get_if_clause_conditional_expression() const
        {
            return _if_clause_cond_expr;
        }

        FunctionTaskSet::FunctionTaskSet()
        {
        }

        bool FunctionTaskSet::is_function_task(Symbol sym) const
        {
            return (_map.find(sym) != _map.end());
        }

        bool FunctionTaskSet::is_function_task_or_implements(Symbol sym) const
        {
            if (is_function_task(sym))
                return true;

            typedef std::map<Symbol, FunctionTaskInfo>::const_iterator iterator;

            for (iterator it = _map.begin();
                    it != _map.end();
                    it++)
            {
                typedef ObjectList<FunctionTaskInfo::implementation_pair_t>::iterator dev_iterator;
                ObjectList<FunctionTaskInfo::implementation_pair_t> devices_and_implementations = it->second.get_devices_with_implementation();

                for (dev_iterator dev_it = devices_and_implementations.begin();
                        dev_it != devices_and_implementations.end();
                        dev_it++)
                {
                    if (dev_it->second == sym)
                        return true;
                }
            }

            return false;
        }

        FunctionTaskInfo& FunctionTaskSet::get_function_task(Symbol sym)
        {
            return _map.find(sym)->second;
        }

        const FunctionTaskInfo& FunctionTaskSet::get_function_task(Symbol sym) const
        {
            return _map.find(sym)->second;
        }

        void FunctionTaskSet::add_function_task(Symbol sym, const FunctionTaskInfo& function_info)
        {
            std::pair<Symbol, FunctionTaskInfo> pair(sym, function_info);
            _map.insert(pair);
        }

        bool FunctionTaskSet::empty() const
        {
            return _map.empty();
        }

        FunctionTaskTargetInfo FunctionTaskInfo::get_target_info() const
        {
            return _target_info;
        }

        struct FunctionTaskDependencyGenerator : public Functor<FunctionTaskDependency, Nodecl::NodeclBase>
        {
            private:
                DependencyDirection _direction;
                Nodecl::NodeclBase _ref_tree;

            public:
                FunctionTaskDependencyGenerator(DependencyDirection direction,
                        Nodecl::NodeclBase ref_tree)
                    : _direction(direction), _ref_tree(ref_tree)
                {
                }

                FunctionTaskDependency do_(FunctionTaskDependencyGenerator::ArgType nodecl) const
                {
                    DataReference expr(nodecl);

                    return FunctionTaskDependency(expr, _direction);
                }
        };

        struct FunctionCopyItemGenerator : public Functor<CopyItem, Nodecl::NodeclBase>
        {
            private:
                CopyDirection _copy_direction;
                Nodecl::NodeclBase _ref_tree;

            public:
                FunctionCopyItemGenerator(CopyDirection copy_direction,
                        Nodecl::NodeclBase ref_tree)
                    : _copy_direction(copy_direction), _ref_tree(ref_tree)
                {
                }

                CopyItem do_(FunctionCopyItemGenerator::ArgType node) const
                {
                    DataReference data_ref(node);

                    return CopyItem(data_ref, _copy_direction);
                }
        };

        static bool is_useless_dependence(const FunctionTaskDependency& function_dep)
        {
            internal_error("Not yet implemented", 0);
            Nodecl::NodeclBase expr(function_dep.get_data_reference());

            if (expr.is<Nodecl::Symbol>())
            {
                Symbol sym = expr.get_symbol();
                if (sym.is_parameter()
                        && !sym.get_type().is_any_reference())
                {
                    return true;
                }
            }
            return false;
        }

        static void dependence_list_check(ObjectList<FunctionTaskDependency>& function_task_param_list)
        {
            ObjectList<FunctionTaskDependency>::iterator begin_remove = std::remove_if(function_task_param_list.begin(),
                    function_task_param_list.end(),
                    is_useless_dependence);

            for (ObjectList<FunctionTaskDependency>::iterator it = begin_remove;
                    it != function_task_param_list.end();
                    it++)
            {
                DependencyDirection direction(it->get_direction());
                Nodecl::NodeclBase expr(it->get_data_reference());

                if (expr.is<Nodecl::Symbol>())
                {
                    Symbol sym = expr.get_symbol();
                    if (sym.is_parameter()
                            && !sym.get_type().is_any_reference())
                    {
                        // Copy semantics of values in C/C++ lead to this fact
                        // If the dependence is output (or inout) this should
                        // be regarded as an error
                        if ((direction & DEP_DIR_OUT) == DEP_DIR_OUT)
                        {
                            running_error("%s: error: output dependence '%s' "
                                    "only names a parameter. The value of a parameter is never copied out of a function "
                                    "so it cannot generate an output dependence",
                                    expr.get_locus().c_str(),
                                    expr.prettyprint().c_str());
                        }
                        else
                        {
                            std::cerr << expr.get_locus() << ": warning: "
                                "skipping useless dependence '"<< expr.prettyprint() << "'. The value of a parameter "
                                "is always copied and cannot define an input dependence"
                                << std::endl;
                        }
                    }
                }
            }

            // Remove useless expressions
            function_task_param_list.erase(begin_remove, function_task_param_list.end());
        }

        void Core::task_function_handler_pre(TL::PragmaCustomDeclaration construct)
        {
            Nodecl::NodeclBase param_ref_tree = construct.get_context_of_parameters();

            TL::PragmaCustomLine pragma_line = construct.get_pragma_line();

            RealTimeInfo rt_info = task_real_time_handler_pre(pragma_line);

            PragmaCustomClause input_clause = pragma_line.get_clause("input");
            ObjectList<Nodecl::NodeclBase> input_arguments;
            if (input_clause.is_defined())
            {
                input_arguments = input_clause.get_arguments_as_expressions(param_ref_tree);
            }

            PragmaCustomClause output_clause = pragma_line.get_clause("output");
            ObjectList<Nodecl::NodeclBase> output_arguments;
            if (output_clause.is_defined())
            {
                output_arguments = output_clause.get_arguments_as_expressions(param_ref_tree);
            }

            PragmaCustomClause inout_clause = pragma_line.get_clause("inout");
            ObjectList<Nodecl::NodeclBase> inout_arguments;
            if (inout_clause.is_defined())
            {
                inout_arguments = inout_clause.get_arguments_as_expressions(param_ref_tree);
            }

            PragmaCustomClause reduction_clause = pragma_line.get_clause("concurrent");
            ObjectList<Nodecl::NodeclBase> reduction_arguments;
            if (reduction_clause.is_defined())
            {
                reduction_arguments = reduction_clause.get_arguments_as_expressions(param_ref_tree);
            }

            Symbol function_sym = construct.get_symbol();

            if (!function_sym.is_function())
            {
                std::cerr << construct.get_locus()
                    << ": warning: '#pragma omp task' cannot be applied to this declaration since it does not declare a function, skipping" << std::endl;
                return;
            }

            Type function_type = function_sym.get_type();

            bool has_ellipsis = false;
            function_type.parameters(has_ellipsis);

            if (has_ellipsis)
            {
                std::cerr << construct.get_locus()
                    << ": warning: '#pragma omp task' cannot be applied to functions declarations with ellipsis, skipping" << std::endl;
                return;
            }

            if (!function_type.returns().is_void())
            {
                std::cerr << construct.get_locus()
                    << ": warning: '#pragma omp task' cannot be applied to functions returning non-void, skipping" << std::endl;
                return;
            }

            ObjectList<FunctionTaskDependency> dependence_list;
            FunctionTaskTargetInfo target_info;

             dependence_list.append(input_arguments.map(FunctionTaskDependencyGenerator(DEP_DIR_IN,
                             param_ref_tree)));

             dependence_list.append(output_arguments.map(FunctionTaskDependencyGenerator(DEP_DIR_OUT,
                             param_ref_tree)));

             dependence_list.append(inout_arguments.map(FunctionTaskDependencyGenerator(DEP_DIR_INOUT,
                             param_ref_tree)));

             dependence_list.append(reduction_arguments.map(FunctionTaskDependencyGenerator(DEP_CONCURRENT,
                             param_ref_tree)));

             dependence_list_check(dependence_list);

             // Now gather task information
             if (!_target_context.empty())
             {
                 TargetContext& target_context = _target_context.top();

                 ObjectList<CopyItem> copy_in = target_context.copy_in.map(FunctionCopyItemGenerator(
                             COPY_DIR_IN, param_ref_tree));
                 target_info.set_copy_in(copy_in);

                 ObjectList<CopyItem> copy_out = target_context.copy_out.map(FunctionCopyItemGenerator(
                             COPY_DIR_OUT, param_ref_tree));
                 target_info.set_copy_out(copy_out);

                 ObjectList<CopyItem> copy_inout = target_context.copy_inout.map(FunctionCopyItemGenerator(
                             COPY_DIR_INOUT, param_ref_tree));
                 target_info.set_copy_inout(copy_inout);

                 target_info.set_device_list(target_context.device_list);

                 target_info.set_copy_deps(target_context.copy_deps);
             }


            FunctionTaskInfo task_info(function_sym, dependence_list, target_info);

            //adding real time information to the task
            task_info.set_real_time_info(rt_info);
            
            // Support if clause 
            PragmaCustomClause if_clause = pragma_line.get_clause("if");
            if (if_clause.is_defined())
            {
                ObjectList<Nodecl::NodeclBase> expr_list = if_clause.get_arguments_as_expressions(param_ref_tree);
                if (expr_list.size() != 1)
                {
                    running_error("%s: error: clause 'if' requires just one argument\n",
                            construct.get_locus().c_str());
                }
                task_info.set_if_clause_conditional_expression(expr_list[0]);
            }

            std::cerr << construct.get_locus()
                << ": note: adding task function '" << function_sym.get_name() << "'" << std::endl;
            _function_task_set->add_function_task(function_sym, task_info);
        }

        void Core::task_inline_handler_pre(TL::PragmaCustomStatement construct)
        {
            TL::PragmaCustomLine pragma_line = construct.get_pragma_line();

            RealTimeInfo rt_info = task_real_time_handler_pre(pragma_line);
            
            DataSharingEnvironment& data_sharing = _openmp_info->get_new_data_sharing(construct);
            _openmp_info->push_current_data_sharing(data_sharing);

            //adding real time information to the task
            data_sharing.set_real_time_info(rt_info);

            get_data_explicit_attributes(pragma_line, data_sharing);

            get_dependences_info(pragma_line, data_sharing);

            DataSharingAttribute default_data_attr = get_default_data_sharing(pragma_line, /* fallback */ DS_UNDEFINED);
            get_data_implicit_attributes_task(construct, data_sharing, default_data_attr);

            // Target info applies after
            get_target_info(pragma_line, data_sharing);
        }


        RealTimeInfo Core::task_real_time_handler_pre(TL::PragmaCustomLine construct)
        {
            RealTimeInfo rt_info;

            //looking for deadline clause
            PragmaCustomClause deadline_clause = construct.get_clause("deadline");
            if (deadline_clause.is_defined())
            {
                ObjectList<Nodecl::NodeclBase> deadline_exprs =
                    deadline_clause.get_arguments_as_expressions();
                
                if(deadline_exprs.size() != 1) 
                {
                    std::cerr << construct.get_locus()
                              << ": warning: '#pragma omp task deadline' "
                              << "has a wrong number of arguments, skipping"
                              << std::endl;
                }
                else 
                {
                    rt_info.set_time_deadline(deadline_exprs[0]);
                }

            }

            //looking for release_deadline clause
            PragmaCustomClause release_clause = construct.get_clause("release_after");
            if (release_clause.is_defined())
            {
                ObjectList<Nodecl::NodeclBase> release_exprs =
                    release_clause.get_arguments_as_expressions();
                
                if(release_exprs.size() != 1) 
                {
                    std::cerr << construct.get_locus()
                              << ": warning: '#pragma omp task release_deadline' "
                              << "has a wrong number of arguments, skipping"
                              << std::endl;
                }
                else
                {
                    rt_info.set_time_release(release_exprs[0]);
                }
            }
            
            //looking for onerror clause
            PragmaCustomClause on_error_clause = construct.get_clause("onerror");
            if (on_error_clause.is_defined())
            {
                ObjectList<std::string> on_error_args =
                    on_error_clause.get_tokenized_arguments(ExpressionTokenizer());
                
                if(on_error_args.size() != 1) 
                {
                    std::cerr << construct.get_locus()
                              << ": warning: '#pragma omp task onerror' "
                              << "has a wrong number of arguments, skipping"
                              << std::endl;
                }
                else
                {
                    Lexer l = Lexer::get_current_lexer();

                    ObjectList<Lexer::pair_token> tokens = l.lex_string(on_error_args[0]);
                    switch (tokens.size())
                    {
                        
                        // tokens structure: 'indentifier'
                        case 1:
                        {
                            if ((IS_C_LANGUAGE   && (tokens[0].first != TokensC::IDENTIFIER)) ||
                                (IS_CXX_LANGUAGE && (tokens[0].first != TokensCXX::IDENTIFIER)))
                            {
                                  std::cerr << construct.get_locus()
                                            << ": warning: '#pragma omp task onerror' "
                                            << "first token must be an identifier, skipping"
                                            << std::endl;
                            }
                            else
                            {
                                rt_info.add_error_behavior(tokens[0].second);
                            }
                            break;
                        }

                        //tokens structure: 'identifier:identifier'
                        case 3:
                        {
                            if ((IS_C_LANGUAGE   && (tokens[0].first != TokensC::IDENTIFIER)) ||
                                (IS_CXX_LANGUAGE && (tokens[0].first != TokensCXX::IDENTIFIER)))
                            {
                                std::cerr << construct.get_locus()
                                          << ": warning: '#pragma omp task onerror' "
                                          << "first token must be an identifier, skipping"
                                          << std::endl;
                            }
                            else if (tokens[1].first != (int)':')
                            {
                                std::cerr << construct.get_locus()
                                          << ": warning: '#pragma omp task onerror' "
                                          << "second token must be a colon, skipping"
                                          << std::endl;
                            }
                            else if ((IS_C_LANGUAGE   && (tokens[2].first != TokensC::IDENTIFIER)) ||
                                     (IS_CXX_LANGUAGE && (tokens[2].first != TokensCXX::IDENTIFIER)))
                            {
                                std::cerr << construct.get_locus()
                                          << ": warning: '#pragma omp task onerror' "
                                          << "third token must be an identifier, skipping"
                                          << std::endl;
                            }
                            else
                            {
                                rt_info.add_error_behavior(tokens[0].second, tokens[2].second);
                            }
                            break;
                        }
                        default:
                        {
                            std::cerr 
                                  << construct.get_locus()
                                  << ": warning: '#pragma omp task onerror' "
                                  << "has a wrong number of tokens. "
                                  << "It is expecting 'identifier:identifier' "
                                  << "or 'indentifier', skipping"
                                  << std::endl;
                        }
                    }
                }
            }

            return rt_info;
        }
    }
}
