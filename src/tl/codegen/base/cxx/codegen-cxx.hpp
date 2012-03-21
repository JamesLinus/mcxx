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

#ifndef CODEGEN_CXX_HPP
#define CODEGEN_CXX_HPP

#include "codegen-phase.hpp"
#include "tl-scope.hpp"
#include "tl-symbol.hpp"
#include <sstream>
#include <map>
#include <set>

namespace Codegen
{
    class CxxBase : public CodegenPhase
    {
        protected:
            virtual std::string codegen(const Nodecl::NodeclBase&);
        public:
            virtual void push_context(TL::Scope sc);
            virtual void pop_context();

            Ret visit(const Nodecl::Add &);
            Ret visit(const Nodecl::AddAssignment &);
            Ret visit(const Nodecl::Alignof &);
            Ret visit(const Nodecl::ArraySubscript &);
            Ret visit(const Nodecl::Assignment &);
            Ret visit(const Nodecl::BitwiseAnd &);
            Ret visit(const Nodecl::BitwiseAndAssignment &);
            Ret visit(const Nodecl::BitwiseNot &);
            Ret visit(const Nodecl::BitwiseOr &);
            Ret visit(const Nodecl::BitwiseOrAssignment &);
            Ret visit(const Nodecl::BitwiseXor &);
            Ret visit(const Nodecl::BitwiseXorAssignment &);
            Ret visit(const Nodecl::BooleanLiteral &);
            Ret visit(const Nodecl::BreakStatement &);
            Ret visit(const Nodecl::C99DesignatedInitializer &);
            Ret visit(const Nodecl::C99FieldDesignator &);
            Ret visit(const Nodecl::C99IndexDesignator &);
            Ret visit(const Nodecl::CaseStatement &);
            Ret visit(const Nodecl::Cast &);
            Ret visit(const Nodecl::CatchHandler &);
            Ret visit(const Nodecl::ClassMemberAccess &);
            Ret visit(const Nodecl::Comma &);
            Ret visit(const Nodecl::ComplexLiteral &);
            Ret visit(const Nodecl::CompoundExpression &);
            Ret visit(const Nodecl::CompoundStatement &);
            Ret visit(const Nodecl::ConditionalExpression &);
            Ret visit(const Nodecl::Context &);
            Ret visit(const Nodecl::ContinueStatement &);
            Ret visit(const Nodecl::SavedExpr &);
            Ret visit(const Nodecl::Conversion &);
            Ret visit(const Nodecl::CxxArrow &);
            Ret visit(const Nodecl::CxxBracedInitializer &);
            Ret visit(const Nodecl::CxxDepGlobalNameNested &);
            Ret visit(const Nodecl::CxxDepNameConversion &);
            Ret visit(const Nodecl::CxxDepNameNested &);
            Ret visit(const Nodecl::CxxDepNameSimple &);
            Ret visit(const Nodecl::CxxDepTemplateId &);
            Ret visit(const Nodecl::CxxEqualInitializer &);
            Ret visit(const Nodecl::CxxMemberInit &);
            Ret visit(const Nodecl::CxxExplicitTypeCast &);
            Ret visit(const Nodecl::CxxParenthesizedInitializer &);
            Ret visit(const Nodecl::DefaultStatement &);
            Ret visit(const Nodecl::Delete &);
            Ret visit(const Nodecl::DeleteArray &);
            Ret visit(const Nodecl::Derreference &);
            Ret visit(const Nodecl::Different &);
            Ret visit(const Nodecl::Div &);
            Ret visit(const Nodecl::DivAssignment &);
            Ret visit(const Nodecl::DoStatement &);
            Ret visit(const Nodecl::EmptyStatement &);
            Ret visit(const Nodecl::Equal &);
            Ret visit(const Nodecl::ErrExpr &);
            Ret visit(const Nodecl::ExpressionStatement &);
            Ret visit(const Nodecl::FieldDesignator &);
            Ret visit(const Nodecl::FloatingLiteral &);
            Ret visit(const Nodecl::ForStatement &);
            Ret visit(const Nodecl::FunctionCall &);
            Ret visit(const Nodecl::FunctionCode &);
            Ret visit(const Nodecl::TemplateFunctionCode &);
            Ret visit(const Nodecl::GotoStatement &);
            Ret visit(const Nodecl::GreaterOrEqualThan &);
            Ret visit(const Nodecl::GreaterThan &);
            Ret visit(const Nodecl::IfElseStatement &);
            Ret visit(const Nodecl::ImagPart &);
            Ret visit(const Nodecl::IndexDesignator &);
            Ret visit(const Nodecl::IntegerLiteral &);
            Ret visit(const Nodecl::LabeledStatement &);
            Ret visit(const Nodecl::LogicalAnd &);
            Ret visit(const Nodecl::LogicalNot &);
            Ret visit(const Nodecl::LogicalOr &);
            Ret visit(const Nodecl::LoopControl &);
            Ret visit(const Nodecl::LowerOrEqualThan &);
            Ret visit(const Nodecl::LowerThan &);
            Ret visit(const Nodecl::MemberInit &);
            Ret visit(const Nodecl::Minus &);
            Ret visit(const Nodecl::Mod &);
            Ret visit(const Nodecl::ModAssignment &);
            Ret visit(const Nodecl::Mul &);
            Ret visit(const Nodecl::MulAssignment &);
            Ret visit(const Nodecl::Neg &);
            Ret visit(const Nodecl::New &);
            Ret visit(const Nodecl::ObjectInit &);
            Ret visit(const Nodecl::Offset &);
            Ret visit(const Nodecl::Offsetof &);
            Ret visit(const Nodecl::ParenthesizedExpression &);
            Ret visit(const Nodecl::Plus &);
            Ret visit(const Nodecl::PointerToMember &);
            Ret visit(const Nodecl::Postdecrement &);
            Ret visit(const Nodecl::Postincrement &);
            Ret visit(const Nodecl::PragmaClauseArg &);
            Ret visit(const Nodecl::PragmaCustomClause &);
            Ret visit(const Nodecl::PragmaCustomDeclaration &);
            Ret visit(const Nodecl::PragmaCustomDirective &);
            Ret visit(const Nodecl::PragmaCustomLine &);
            Ret visit(const Nodecl::PragmaCustomStatement &);
            Ret visit(const Nodecl::Predecrement &);
            Ret visit(const Nodecl::Preincrement &);
            Ret visit(const Nodecl::PseudoDestructorName &);
            Ret visit(const Nodecl::Range &);
            Ret visit(const Nodecl::RealPart &);
            Ret visit(const Nodecl::Reference &);
            Ret visit(const Nodecl::ReturnStatement &);
            Ret visit(const Nodecl::Shaping &);
            Ret visit(const Nodecl::Shl &);
            Ret visit(const Nodecl::ShlAssignment &);
            Ret visit(const Nodecl::Shr &);
            Ret visit(const Nodecl::ShrAssignment &);
            Ret visit(const Nodecl::Sizeof &);
            Ret visit(const Nodecl::StringLiteral &);
            Ret visit(const Nodecl::StructuredValue &);
            Ret visit(const Nodecl::SubAssignment &);
            Ret visit(const Nodecl::SwitchStatement &);
            Ret visit(const Nodecl::Symbol &);
            Ret visit(const Nodecl::Text &);
            Ret visit(const Nodecl::Throw &);
            Ret visit(const Nodecl::TopLevel &);
            Ret visit(const Nodecl::TryBlock &);
            Ret visit(const Nodecl::Type &);
            Ret visit(const Nodecl::Typeid &);
            Ret visit(const Nodecl::VirtualFunctionCall &);
            Ret visit(const Nodecl::WhileStatement &);

            Ret visit(const Nodecl::CxxDecl& node);

            Ret visit(const Nodecl::Verbatim& node);
            Ret visit(const Nodecl::UnknownPragma& node);
            Ret visit(const Nodecl::GxxTrait& node);
            Ret visit(const Nodecl::GccAsmDefinition& node);
            Ret visit(const Nodecl::GccAsmOperand& node);
            Ret visit(const Nodecl::GccAsmSpec& node);
            Ret visit(const Nodecl::GccBuiltinVaArg& node);
            Ret visit(const Nodecl::UpcSyncStatement& node);
            Ret visit(const Nodecl::SourceComment& node);
            Ret visit(const Nodecl::PreprocessorLine& node);

        private:

            // State
            struct State
            {
                TL::Symbol current_symbol;
                TL::Symbol global_namespace;
                TL::Symbol opened_namespace;

                enum EmitDeclarations
                {
                    EMIT_ALL_DECLARATIONS,
                    EMIT_CURRENT_SCOPE_DECLARATIONS,
                    EMIT_NO_DECLARATIONS,
                };

                EmitDeclarations emit_declarations;

                bool in_condition;
                Nodecl::NodeclBase condition_top;

                bool in_member_declaration;

                bool in_dependent_template_function_code;

                bool inside_structured_value;

                TL::ObjectList<TL::Symbol> classes_being_defined;


                std::set<TL::Type> walked_types;

                // This one is to be used only in define_required_before_class
                std::set<TL::Symbol> being_checked_for_required;

                // Used in define_required_before_class and define_symbol_if_nonnested
                std::set<TL::Symbol> pending_nested_types_to_define;
                
                // Used in define_generic_entities  
                std::set<TL::Symbol> walked_symbols;

                // Object init
                std::set<TL::Symbol> must_be_object_init;

                // This means that we are doing &X and X is a rebindable reference
                bool do_not_derref_rebindable_reference;

                // Not meant to be used directly, use functions 
                // get_indent_level, set_indent_level
                // inc_indent, dec_indent
                int _indent_level;

                State() :
                    global_namespace(),
                    opened_namespace(),
                    emit_declarations(EMIT_NO_DECLARATIONS),
                    in_condition(false),
                    condition_top(Nodecl::NodeclBase::null()),
                    in_member_declaration(false),
                    in_dependent_template_function_code(false),
                    inside_structured_value(),
                    classes_being_defined(),
                    walked_types(),
                    being_checked_for_required(),
                    pending_nested_types_to_define(),
                    walked_symbols(),
                    must_be_object_init(),
                    do_not_derref_rebindable_reference(false),
                    _indent_level(0) { }
            } state;
            // End of State

            std::vector<TL::Scope> _scope_stack;

            bool symbol_is_same_or_nested_in(TL::Symbol symbol, TL::Symbol class_sym);
            bool symbol_is_nested_in_defined_classes(TL::Symbol symbol);

            TL::ObjectList<TL::Symbol> define_required_before_class(TL::Symbol symbol);
            void define_class_symbol_aux(TL::Symbol symbol,
                    TL::ObjectList<TL::Symbol> symbols_defined_inside_class,
                    int level);
            void define_class_symbol(TL::Symbol symbol);

            void define_symbol(TL::Symbol symbol);
            void declare_symbol(TL::Symbol symbol);

            void define_generic_entities(Nodecl::NodeclBase node,
                    void (CxxBase::*decl_sym_fun)(TL::Symbol symbol),
                    void (CxxBase::*def_sym_fun)(TL::Symbol symbol),
                    void (CxxBase::*define_entities_fun)(const Nodecl::NodeclBase& node),
                    void (CxxBase::*define_entry_fun)(
                        const Nodecl::NodeclBase& node, TL::Symbol entry,
                        void (CxxBase::*def_sym_fun_2)(TL::Symbol symbol))
                    );

            bool is_local_symbol(TL::Symbol entry);
            bool is_prototype_symbol(TL::Symbol entry);
            bool all_enclosing_classes_are_user_declared(TL::Symbol entry);

            void define_all_entities_in_trees(const Nodecl::NodeclBase&);
            void define_nonlocal_entities_in_trees(const Nodecl::NodeclBase&);
            void define_nonlocal_nonprototype_entities_in_trees(const Nodecl::NodeclBase& node);
            void define_nonprototype_entities_in_trees(const Nodecl::NodeclBase& node);
            void define_nonnested_entities_in_trees(const Nodecl::NodeclBase&);
            void define_local_entities_in_trees(const Nodecl::NodeclBase&);

            void declare_symbol_if_nonlocal(TL::Symbol);
            void define_symbol_if_nonlocal(TL::Symbol);

            void declare_symbol_if_nonlocal_nonprototype(TL::Symbol);
            void define_symbol_if_nonlocal_nonprototype(TL::Symbol);

            void declare_symbol_if_nonprototype(TL::Symbol);
            void define_symbol_if_nonprototype(TL::Symbol);

            void declare_symbol_if_local(TL::Symbol);
            void define_symbol_if_local(TL::Symbol);

            void declare_symbol_if_nonnested(TL::Symbol);
            void define_symbol_if_nonnested(TL::Symbol);

            void define_specializations_user_declared(TL::Symbol);

            void walk_type_for_symbols(TL::Type, 
                    bool needs_def, 
                    void (CxxBase::* declare_fun)(TL::Symbol),
                    void (CxxBase::* define_fun)(TL::Symbol),
                    void (CxxBase::* define_entities)(const Nodecl::NodeclBase&));

            void entry_just_define(
                    const Nodecl::NodeclBase&, 
                    TL::Symbol symbol,
                    void (CxxBase::*def_sym_fun)(TL::Symbol));

            void entry_local_definition(
                    const Nodecl::NodeclBase&,
                    TL::Symbol entry,
                    void (CxxBase::*def_sym_fun)(TL::Symbol));

            std::map<TL::Symbol, codegen_status_t> _codegen_status;
            void set_codegen_status(TL::Symbol sym, codegen_status_t status);
            codegen_status_t get_codegen_status(TL::Symbol sym);

            void codegen_fill_namespace_list_rec(
                    scope_entry_t* namespace_sym, 
                    scope_entry_t** list, 
                    int* position);
            void codegen_move_namespace_from_to(TL::Symbol from, TL::Symbol to);
            void move_to_namespace_of_symbol(TL::Symbol symbol);

            void indent();
            void inc_indent(int n = 1);
            void dec_indent(int n = 1);

            int get_indent_level();
            void set_indent_level(int);

            void walk_list(const Nodecl::List&, const std::string& separator);
            void walk_expression_list(const Nodecl::List&);
            template <typename Iterator>
            void walk_expression_unpacked_list(Iterator begin, Iterator end);

            template <typename Iterator>
                void codegen_function_call_arguments(Iterator begin, Iterator end, TL::Type function_type, int ignore_n_first);

            template <typename Node>
                CxxBase::Ret visit_function_call(const Node&, bool is_virtual_call);

            static int get_rank_kind(node_t n, const std::string& t);
            static int get_rank(const Nodecl::NodeclBase &n);
            bool same_operation(Nodecl::NodeclBase current_operator, Nodecl::NodeclBase operand);
            static bool operand_has_lower_priority(Nodecl::NodeclBase operation, Nodecl::NodeclBase operand);
            static std::string quote_c_string(int* c, int length, char is_wchar);
            static bool nodecl_calls_to_constructor(const Nodecl::NodeclBase&, TL::Type t);
            static bool nodecl_is_zero_args_call_to_constructor(Nodecl::NodeclBase node);
            static bool nodecl_is_zero_args_structured_value(Nodecl::NodeclBase node);

            static std::string unmangle_symbol_name(TL::Symbol);

            void declare_all_in_template_arguments(TL::TemplateParameters template_arguments);
            
            void declare_all_in_template_header(TL::TemplateParameters template_arguments);

            void codegen_template_headers_all_levels(TL::TemplateParameters template_parameters);
            
            void codegen_template_headers_bounded(TL::TemplateParameters template_parameters, TL::TemplateParameters lim);

            void codegen_template_header(TL::TemplateParameters template_parameters, bool endline = true);

            std::string template_arguments_to_str(TL::Symbol);

            std::string exception_specifier_to_str(TL::Symbol);

            std::string gcc_attributes_to_str(TL::Symbol);
            std::string gcc_asm_specifier_to_str(TL::Symbol);

            virtual Ret unhandled_node(const Nodecl::NodeclBase & n);

            std::string get_declaration(TL::Type t, TL::Scope scope, const std::string& name);
            std::string get_declaration_with_parameters(TL::Type, TL::Scope, const std::string& name,
                    TL::ObjectList<std::string>& names, TL::ObjectList<std::string> & parameter_attributes);
            TL::Type fix_references(TL::Type t);

            TL::Scope get_current_scope() const;
    };
}

#endif // CODEGEN_CXX_HPP
