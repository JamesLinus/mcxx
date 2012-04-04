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




#include <typeinfo>
#include <cctype>
#include "cxx-utils.h"
#include "cxx-diagnostic.h"
#include "tl-pragmasupport.hpp"

namespace TL
{
    ObjectList<std::string> NullClauseTokenizer::tokenize(const std::string& str) const
    {
        ObjectList<std::string> result;
        result.append(str);
        return result;
    }

    ObjectList<std::string> ExpressionTokenizer::tokenize(const std::string& str) const
    {
        int bracket_nesting = 0;
        ObjectList<std::string> result;

        std::string temporary("");
        for (std::string::const_iterator it = str.begin();
                it != str.end();
                it++)
        {
            const char & c(*it);

            if (c == ',' 
                    && bracket_nesting == 0
                    && temporary != "")
            {
                result.append(temporary);
                temporary = "";
            }
            else
            {
                if (c == '('
                        || c == '{'
                        || c == '[')
                {
                    bracket_nesting++;
                }
                else if (c == ')'
                        || c == '}'
                        || c == ']')
                {
                    bracket_nesting--;
                }
                temporary += c;
            }
        }

        if (temporary != "")
        {
            result.append(temporary);
        }

        return result;
    }

    ObjectList<std::string> ExpressionTokenizerTrim::tokenize(const std::string& str) const
    {
        ObjectList<std::string> result;
        result = ExpressionTokenizer::tokenize(str);

        std::transform(result.begin(), result.end(), result.begin(), trimExp);

        return result;
    }

    std::string ExpressionTokenizerTrim::trimExp (const std::string &str) 
    {

        ssize_t first = str.find_first_not_of(" \t");
        ssize_t last = str.find_last_not_of(" \t");

        return str.substr(first, last - first + 1);
    }

    // Initialize here the warnings to the dispatcher
    PragmaCustomCompilerPhase::PragmaCustomCompilerPhase(const std::string& pragma_handled)
        : _pragma_handled(pragma_handled)
    {
    }

    void PragmaCustomCompilerPhase::pre_run(DTO& data_flow)
    {
        // Do nothing
    }

    void PragmaCustomCompilerPhase::run(DTO& data_flow)
    {
        Nodecl::NodeclBase node = data_flow["nodecl"];

        this->walk(node);
    }

    void PragmaCustomCompilerPhase::walk(Nodecl::NodeclBase& node)
    {
        PragmaVisitor visitor(_pragma_handled, _pragma_map_dispatcher);
        visitor.walk(node);
    }

    void PragmaCustomCompilerPhase::register_directive(const std::string& str)
    {
        register_new_directive(_pragma_handled.c_str(), str.c_str(), 0, 0);
    }

    void PragmaCustomCompilerPhase::register_construct(const std::string& str, bool bound_to_statement)
    {
        if (IS_FORTRAN_LANGUAGE)
        {
            register_new_directive(_pragma_handled.c_str(), str.c_str(), 1, bound_to_statement);
        }
        else
        {
            register_new_directive(_pragma_handled.c_str(), str.c_str(), 1, 0);
        }
    }

    void PragmaCustomCompilerPhase::warning_pragma_unused_clauses(bool warning)
    {
        // _pragma_dispatcher.set_warning_clauses(warning);
    }

    PragmaMapDispatcher& PragmaCustomCompilerPhase::dispatcher()
    {
        return _pragma_map_dispatcher;
    }

    std::string PragmaClauseArgList::get_raw_arguments() const
    {
        std::string result;

        for (Nodecl::List::const_iterator it = this->begin();
                it != this->end();
                it++)
        {
            result += it->get_text();
        }

        return result;
    }

    ObjectList<std::string> PragmaClauseArgList::get_tokenized_arguments(const ClauseTokenizer& tokenizer) const
    {
        std::string raw = this->get_raw_arguments();
        return tokenizer.tokenize(raw);
    }

    ObjectList<Nodecl::NodeclBase> PragmaClauseArgList::get_arguments_as_expressions(Source::ReferenceScope ref_scope, 
            const ClauseTokenizer& tokenizer) const
    {
        ObjectList<std::string> str_list = this->get_tokenized_arguments(tokenizer);
        ObjectList<Nodecl::NodeclBase> result;

        for (ObjectList<std::string>::iterator it = str_list.begin();
                it != str_list.end();
                it++)
        {
            Source src = *it;

            Nodecl::NodeclBase current_expr = src.parse_expression(ref_scope);

            result.append(current_expr);
        }

        return result;
    }

    ObjectList<Nodecl::NodeclBase> PragmaClauseArgList::get_arguments_as_expressions(const ClauseTokenizer& tokenizer) const
    {
        return get_arguments_as_expressions(this->retrieve_context(), tokenizer);
    }

    std::string PragmaCustomSingleClause::get_raw_arguments() const
    {
        return PragmaClauseArgList(this->get_arguments().as<Nodecl::List>()).get_raw_arguments();
    }

    ObjectList<std::string> PragmaCustomSingleClause::get_tokenized_arguments(const ClauseTokenizer& tokenizer) const
    {
        return PragmaClauseArgList(this->get_arguments().as<Nodecl::List>()).get_tokenized_arguments(tokenizer);
    }

    ObjectList<Nodecl::NodeclBase> PragmaCustomSingleClause::get_arguments_as_expressions(Source::ReferenceScope ref_scope, 
            const ClauseTokenizer& tokenizer) const
    {
        return PragmaClauseArgList(this->get_arguments().as<Nodecl::List>()).get_arguments_as_expressions(ref_scope, tokenizer);
    }

    ObjectList<Nodecl::NodeclBase> PragmaCustomSingleClause::get_arguments_as_expressions(const ClauseTokenizer& tokenizer) const
    {
        return get_arguments_as_expressions(this->retrieve_context(), tokenizer);
    }

    ObjectList<std::string> PragmaCustomClause::get_raw_arguments() const
    {
        ObjectList<std::string> result;
        for (PragmaCustomClauseList::const_iterator it = _pragma_clauses.begin();
                it != _pragma_clauses.end();
                it++)
        {
            result.append(TL::PragmaCustomSingleClause(*it).get_raw_arguments());
        }
        return result;
    }

    PragmaCustomClause::PragmaCustomClause(Nodecl::PragmaCustomLine pragma_line, 
            ObjectList<Nodecl::PragmaCustomClause> pragma_clauses)
        : _pragma_line(pragma_line), 
          _pragma_clauses(pragma_clauses)
    {
    }

    bool PragmaCustomClause::is_defined() const
    {
        return !_pragma_clauses.empty();
    }

    bool PragmaCustomParameter::is_defined() const
    {
        return !this->empty();
    }

    bool PragmaCustomClause::is_singleton() const
    {
        return _pragma_clauses.size() == 1;
    }

    ObjectList<std::string> PragmaCustomClause::get_tokenized_arguments(const ClauseTokenizer& tokenizer) const
    {
        ObjectList<std::string> result;
        for (PragmaCustomClauseList::const_iterator it = _pragma_clauses.begin();
                it != _pragma_clauses.end();
                it++)
        {
            result.append(TL::PragmaCustomSingleClause(*it).get_tokenized_arguments(tokenizer));
        }
        return result;
    }

    ObjectList<Nodecl::NodeclBase> PragmaCustomClause::get_arguments_as_expressions(
        Source::ReferenceScope ref_scope, 
        const ClauseTokenizer & tokenizer) const
    {
        ObjectList<Nodecl::NodeclBase> result;
        for (PragmaCustomClauseList::const_iterator it = _pragma_clauses.begin();
                it != _pragma_clauses.end();
                it++)
        {
            result.append(TL::PragmaCustomSingleClause(*it).get_arguments_as_expressions(ref_scope, tokenizer));
        }
        return result;
    }

    std::string PragmaCustomClause::get_locus() const
    {
        return _pragma_line.get_locus();
    }

    ObjectList<Nodecl::NodeclBase> PragmaCustomClause::get_arguments_as_expressions(const ClauseTokenizer& tokenizer) const
    {
        return this->get_arguments_as_expressions(_pragma_line.retrieve_context(), tokenizer);
    }

    TL::PragmaCustomClause PragmaCustomLine::get_clause(const ObjectList<std::string>& aliased_names,
            const ObjectList<std::string>& deprecated_names) const
    {
        ObjectList<Nodecl::PragmaCustomClause> result;
        Nodecl::List clauses = this->get_clauses().as<Nodecl::List>();

        for (Nodecl::List::iterator it = clauses.begin(); 
                it != clauses.end();
                it++)
        {
            Nodecl::PragmaCustomClause clause = it->as<Nodecl::PragmaCustomClause>();

            if (aliased_names.contains(clause.get_text())
                    || deprecated_names.contains(clause.get_text()))
            {
                result.append(clause);
            }
            if (deprecated_names.contains(clause.get_text()))
            {
                warn_printf("%s: warning: clause '%s' is deprecated. Instead use '%s'\n",
                        clause.get_locus().c_str(),
                        clause.get_text().c_str(),
                        aliased_names[0].c_str());
            }
        }

        return PragmaCustomClause(*this, result);
    }

    TL::PragmaCustomClause PragmaCustomLine::get_clause(const ObjectList<std::string>& aliased_names) const
    {
        return get_clause(aliased_names, ObjectList<std::string>());
    }

    TL::PragmaCustomClause PragmaCustomLine::get_clause(const std::string &name, 
            const ObjectList<std::string>& deprecated_names) const
    {
        ObjectList<std::string> set;
        set.append(name);
        return get_clause(set, deprecated_names);
    }
    
    TL::PragmaCustomClause PragmaCustomLine::get_clause(const std::string &name, const std::string& deprecated_name) const
    {
        ObjectList<std::string> deprecated_names;
        deprecated_names.append(deprecated_name);

        return get_clause(name, deprecated_names);
    }

    TL::PragmaCustomClause PragmaCustomLine::get_clause(const std::string &name) const
    {
        return get_clause(name, ObjectList<std::string>());
    }

    ObjectList<TL::PragmaCustomSingleClause> PragmaCustomLine::get_all_clauses() const
    {
        ObjectList<TL::PragmaCustomSingleClause> result;
        Nodecl::List clauses = this->get_clauses().as<Nodecl::List>();

        for (Nodecl::List::iterator it = clauses.begin(); 
                it != clauses.end();
                it++)
        {
            Nodecl::PragmaCustomClause clause = it->as<Nodecl::PragmaCustomClause>();
            result.append(clause);
        }

        return result;
    }

    ObjectList<std::string> PragmaCustomLine::get_all_clause_names() const
    {
        ObjectList<std::string> result;
        Nodecl::List clauses = this->get_clauses().as<Nodecl::List>();

        for (Nodecl::List::iterator it = clauses.begin(); 
                it != clauses.end();
                it++)
        {
            result.append(it->get_text());
        }

        return result;
    }

    PragmaCustomParameter PragmaCustomLine::get_parameter() const
    {
        return PragmaCustomParameter(this->get_parameters().as<Nodecl::List>());
    }

    TL::PragmaCustomLine TL::PragmaCustomDeclaration::get_pragma_line() const
    {
        return TL::PragmaCustomLine(this->Nodecl::PragmaCustomDeclaration::get_pragma_line().as<Nodecl::PragmaCustomLine>());
    }

    TL::PragmaCustomLine TL::PragmaCustomStatement::get_pragma_line() const
    {
        return TL::PragmaCustomLine(this->Nodecl::PragmaCustomStatement::get_pragma_line().as<Nodecl::PragmaCustomLine>());
    }

    TL::PragmaCustomLine TL::PragmaCustomDirective::get_pragma_line() const
    {
        return TL::PragmaCustomLine(this->Nodecl::PragmaCustomDirective::get_pragma_line().as<Nodecl::PragmaCustomLine>());
    }

    Source::ReferenceScope TL::PragmaCustomDirective::get_context_of_declaration() const
    {
        return Source::ReferenceScope(this->Nodecl::PragmaCustomDirective::get_context_of_decl().as<Source::ReferenceScope>());
    }
    
    bool PragmaUtils::is_pragma_construct(const std::string& prefix, 
            const std::string& pragma_name,
            Nodecl::NodeclBase n)
    {
        Nodecl::PragmaCustomLine pragma_line;
        std::string current_prefix;
        if (n.is<Nodecl::PragmaCustomDirective>())
        {
            current_prefix = n.get_text();
            pragma_line = n.as<Nodecl::PragmaCustomDirective>().get_pragma_line().as<Nodecl::PragmaCustomLine>();
        }
        else if (n.is<Nodecl::PragmaCustomStatement>())
        {
            current_prefix = n.get_text();
            pragma_line = n.as<Nodecl::PragmaCustomStatement>().get_pragma_line().as<Nodecl::PragmaCustomLine>();
        }
        else if (n.is<Nodecl::PragmaCustomDeclaration>())
        {
            current_prefix = n.get_text();
            pragma_line = n.as<Nodecl::PragmaCustomDeclaration>().get_pragma_line().as<Nodecl::PragmaCustomLine>();
        }
        else
        {
            return false;
        }

        return (current_prefix == prefix
                && pragma_line.get_text() == pragma_name);
    }
    
    Nodecl::PragmaCustomLine TL::PragmaCustomClause::get_pragma_line() const
    {
        return _pragma_line;
    }
}
