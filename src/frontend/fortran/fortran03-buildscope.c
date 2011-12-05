#include "fortran03-buildscope.h"
#include "fortran03-scope.h"
#include "fortran03-exprtype.h"
#include "fortran03-prettyprint.h"
#include "fortran03-typeutils.h"
#include "fortran03-intrinsics.h"
#include "fortran03-modules.h"
#include "fortran03-codegen.h"
#include "cxx-ast.h"
#include "cxx-scope.h"
#include "cxx-buildscope.h"
#include "cxx-utils.h"
#include "cxx-entrylist.h"
#include "cxx-typeutils.h"
#include "cxx-tltype.h"
#include "cxx-exprtype.h"
#include "cxx-ambiguity.h"
#include "cxx-limits.h"
#include "cxx-nodecl.h"
#include "cxx-nodecl-output.h"
#include "cxx-pragma.h"
#include "cxx-diagnostic.h"
#include "cxx-placeholders.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "red_black_tree.h"

static void unsupported_construct(AST a, const char* name);
static void unsupported_statement(AST a, const char* name);

static void null_dtor(const void* p UNUSED_PARAMETER) { }

void fortran_initialize_translation_unit_scope(translation_unit_t* translation_unit)
{
    decl_context_t decl_context;
    initialize_translation_unit_scope(translation_unit, &decl_context);

    translation_unit->module_cache = rb_tree_create((int (*)(const void*, const void*))strcasecmp, null_dtor, null_dtor);

    fortran_init_intrinsics(decl_context);
}

static void build_scope_program_unit_seq(AST program_unit_seq, 
        decl_context_t decl_context,
        nodecl_t* nodecl_output);

void build_scope_fortran_translation_unit(translation_unit_t* translation_unit)
{
    AST a = translation_unit->parsed_tree;
    // Technically Fortran does not have a global scope but it is convenient to have one
    decl_context_t decl_context = translation_unit->global_decl_context;


    nodecl_t nodecl_program_units = nodecl_null();
    AST list = ASTSon0(a);
    if (list != NULL)
    {
        build_scope_program_unit_seq(list, decl_context, &nodecl_program_units);
    }

    translation_unit->nodecl = nodecl_make_top_level(nodecl_program_units, ASTFileName(a), ASTLine(a));
}

static void build_scope_program_unit_internal(AST program_unit, 
        decl_context_t decl_context,
        scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output);

void build_scope_program_unit(AST program_unit, 
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    build_scope_program_unit_internal(program_unit,
            decl_context, 
            /* program_unit symbol */ NULL, 
            nodecl_output);
}

static void build_scope_program_unit_seq(AST program_unit_seq, 
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    AST it;
    for_each_element(program_unit_seq, it)
    {
        nodecl_t nodecl_top_level_items = nodecl_null();
        build_scope_program_unit_internal(ASTSon1(it), 
                decl_context, 
                NULL, &nodecl_top_level_items);
        *nodecl_output = nodecl_concat_lists(*nodecl_output,
                nodecl_top_level_items);
    }
}

static scope_entry_t* get_special_symbol(decl_context_t decl_context, const char *name)
{
    ERROR_CONDITION(name == NULL || name[0] != '.', "Name '%s' is not special enough\n", name);

    decl_context_t global_context = decl_context;
    global_context.current_scope = global_context.function_scope;

    scope_entry_list_t* entry_list = query_in_scope_str_flags(global_context, name, DF_ONLY_CURRENT_SCOPE);
    if (entry_list == NULL)
    {
        return NULL;
    }
    scope_entry_t* unknown_info = entry_list_head(entry_list);
    entry_list_free(entry_list);

    return unknown_info;
}


static scope_entry_t* get_or_create_special_symbol(decl_context_t decl_context, const char* name)
{
    scope_entry_t* unknown_info = get_special_symbol(decl_context, name);

    if (unknown_info == NULL)
    {
        // Sign it in function scope
        decl_context_t global_context = decl_context;
        global_context.current_scope = global_context.function_scope;

        unknown_info = new_symbol(global_context, global_context.current_scope, name);
        unknown_info->kind = SK_OTHER;
    }

    return unknown_info;
}

static scope_entry_t* get_untyped_symbols_info(decl_context_t decl_context)
{
    return get_special_symbol(decl_context, ".untyped_symbols");
}

static scope_entry_t* get_or_create_untyped_symbols_info(decl_context_t decl_context)
{
    return get_or_create_special_symbol(decl_context, ".untyped_symbols");
}

scope_entry_t* get_data_symbol_info(decl_context_t decl_context)
{
    return get_special_symbol(decl_context, ".data");
}

static scope_entry_t* get_or_create_data_symbol_info(decl_context_t decl_context)
{
    return get_or_create_special_symbol(decl_context, ".data");
}

scope_entry_t* get_equivalence_symbol_info(decl_context_t decl_context)
{
    return get_special_symbol(decl_context, ".equivalence");
}

static scope_entry_t* get_or_create_equivalence_symbol_info(decl_context_t decl_context)
{
    return get_or_create_special_symbol(decl_context, ".equivalence");
}

static scope_entry_t* get_or_create_not_fully_defined_symbol_info(decl_context_t decl_context)
{
    return get_or_create_special_symbol(decl_context, ".not_fully_defined");
}

static scope_entry_t* get_not_fully_defined_symbol_info(decl_context_t decl_context)
{
    return get_special_symbol(decl_context, ".not_fully_defined");
}

static scope_entry_t* get_or_create_unknown_kind_symbol_info(decl_context_t decl_context)
{
    return get_or_create_special_symbol(decl_context, ".unknown_kind");
}

static scope_entry_t* get_unknown_kind_symbol_info(decl_context_t decl_context)
{
    return get_special_symbol(decl_context, ".unknown_kind");
}

void add_untyped_symbol(decl_context_t decl_context, scope_entry_t* entry)
{
    scope_entry_t* unknown_info = get_or_create_untyped_symbols_info(decl_context);

    P_LIST_ADD_ONCE(unknown_info->entity_specs.related_symbols,
            unknown_info->entity_specs.num_related_symbols,
            entry);
}

void remove_untyped_symbol(decl_context_t decl_context, scope_entry_t* entry)
{
    scope_entry_t* unknown_info = get_untyped_symbols_info(decl_context);
    if (unknown_info == NULL)
        return;

    P_LIST_REMOVE(unknown_info->entity_specs.related_symbols,
            unknown_info->entity_specs.num_related_symbols,
            entry);
}

static void check_untyped_symbols(decl_context_t decl_context)
{
    scope_entry_t* unknown_info = get_untyped_symbols_info(decl_context);
    if (unknown_info == NULL)
        return;

    int i;
    for (i = 0; i < unknown_info->entity_specs.num_related_symbols; i++)
    {
        scope_entry_t* entry = unknown_info->entity_specs.related_symbols[i];
        ERROR_CONDITION(entry->type_information == NULL, "A symbol here should have a void type, not NULL", 0);

        if (!basic_type_is_implicit_none(entry->type_information))
        {
            if (entry->kind == SK_UNDEFINED)
            {
                // The user did nothing with that name to let us discover the
                // exact nature of this symbol so lets assume is a variable
                entry->kind = SK_VARIABLE;
                remove_unknown_kind_symbol(decl_context, entry);
            }
            continue;
        }

        error_printf("%s:%d: error: symbol '%s' has no IMPLICIT type\n",
                entry->file,
                entry->line,
                entry->symbol_name);
    }

    free(unknown_info->entity_specs.related_symbols);
    unknown_info->entity_specs.related_symbols = NULL;
    unknown_info->entity_specs.num_related_symbols = 0;
}

static void update_untyped_symbols(decl_context_t decl_context)
{
    scope_entry_t* unknown_info = get_untyped_symbols_info(decl_context);
    if (unknown_info == NULL)
        return;

    scope_entry_t* not_untyped_anymore[1 + unknown_info->entity_specs.num_related_symbols];
    int num_not_untyped_anymore = 0;

    scope_entry_t* new_untyped[1 + unknown_info->entity_specs.num_related_symbols];
    int num_new_untyped = 0;

    int i;
    for (i = 0; i < unknown_info->entity_specs.num_related_symbols; i++)
    {
        scope_entry_t* entry = unknown_info->entity_specs.related_symbols[i];

        ERROR_CONDITION(entry->type_information == NULL, "Invalid type for unknown entity '%s'\n", entry->symbol_name);

        ERROR_CONDITION(!entry->entity_specs.is_implicit_basic_type, 
                "Only those symbols without an explicit type declaration can appear here", 0);

        type_t* implicit_type = get_implicit_type_for_symbol(decl_context, entry->symbol_name);

        entry->type_information = update_basic_type_with_type(entry->type_information,
                implicit_type);

        if (!is_implicit_none_type(implicit_type))
        {
            DEBUG_CODE()
            {
                fprintf(stderr, "BUILDSCOPE: Type of symbol '%s' at '%s:%d' updated to %s\n", 
                        entry->symbol_name,
                        entry->file,
                        entry->line,
                        entry->type_information == NULL ? "<<NULL>>" : print_declarator(entry->type_information));
            }

            // We will remove it from the untyped set later
            not_untyped_anymore[num_not_untyped_anymore] = entry;
            num_not_untyped_anymore++;
        }
        else
        {
            // We will add them into the untyped set later
            new_untyped[num_new_untyped] = entry;
            num_new_untyped++;
        }
    }

    // Add the newly defined untyped here
    for (i = 0; i < num_new_untyped; i++)
    {
        add_untyped_symbol(decl_context, new_untyped[i]);
    }

    // Remove the now typed here
    for (i = 0; i < num_not_untyped_anymore; i++)
    {
        remove_untyped_symbol(decl_context, not_untyped_anymore[i]);
    }
}

static void add_not_fully_defined_symbol(decl_context_t decl_context, scope_entry_t* entry)
{
    scope_entry_t * not_fully_defined = get_or_create_not_fully_defined_symbol_info(decl_context);
    P_LIST_ADD_ONCE(not_fully_defined->entity_specs.related_symbols,
            not_fully_defined->entity_specs.num_related_symbols,
            entry);
}

static void remove_not_fully_defined_symbol(decl_context_t decl_context, scope_entry_t* entry)
{
    scope_entry_t * not_fully_defined = get_not_fully_defined_symbol_info(decl_context);
    if (not_fully_defined == NULL)
        return;

    P_LIST_REMOVE(not_fully_defined->entity_specs.related_symbols,
            not_fully_defined->entity_specs.num_related_symbols,
            entry);
}

static void check_not_fully_defined_symbols(decl_context_t decl_context)
{
    scope_entry_t * not_fully_defined = get_not_fully_defined_symbol_info(decl_context);
    if (not_fully_defined == NULL)
        return;

    int i;
    for (i = 0; i < not_fully_defined->entity_specs.num_related_symbols; i++)
    {
        scope_entry_t* entry = not_fully_defined->entity_specs.related_symbols[i];

        if (entry->kind == SK_COMMON)
        {
            error_printf("%s:%d: error: COMMON '%s' does not exist\n",
                    entry->file,
                    entry->line,
                    entry->symbol_name + strlen(".common."));
        }
        else if (entry->kind == SK_FUNCTION
                && entry->entity_specs.is_module_procedure)
        {
            error_printf("%s:%d: error: MODULE PROCEDURE '%s' does not exist\n",
                    entry->file,
                    entry->line,
                    entry->symbol_name);
        }
        else
        {
            internal_error("Unexpected symbol in not fully defined symbol set %s '%s'",
                    symbol_kind_name(entry),
                    entry->symbol_name);
        }
    }
}

void add_unknown_kind_symbol(decl_context_t decl_context, scope_entry_t* entry)
{
    scope_entry_t * unknown_kind = get_or_create_unknown_kind_symbol_info(decl_context);
    P_LIST_ADD_ONCE(unknown_kind->entity_specs.related_symbols,
            unknown_kind->entity_specs.num_related_symbols,
            entry);
}

void remove_unknown_kind_symbol(decl_context_t decl_context, scope_entry_t* entry)
{
    scope_entry_t * unknown_kind = get_unknown_kind_symbol_info(decl_context);
    if (unknown_kind == NULL)
        return;
    
    P_LIST_REMOVE(unknown_kind->entity_specs.related_symbols,
            unknown_kind->entity_specs.num_related_symbols,
            entry);
}

void review_unknown_kind_symbol(decl_context_t decl_context)
{
    scope_entry_t * unknown_kind = get_unknown_kind_symbol_info(decl_context);
    if (unknown_kind == NULL)
        return;
    
    int i;
    for (i = 0; i < unknown_kind->entity_specs.num_related_symbols; i++)
    {
        scope_entry_t* entry = unknown_kind->entity_specs.related_symbols[i];
        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
        }
        else 
        {
            internal_error("Unexpected symbol in unknown kind symbol set %s '%s'",
                    symbol_kind_name(entry),
                    entry->symbol_name);
        }
    }

    free(unknown_kind->entity_specs.related_symbols);
    unknown_kind->entity_specs.related_symbols = NULL;
    unknown_kind->entity_specs.num_related_symbols = 0;
}

// This function queries a symbol. If not found it uses implicit info to create
// one adding it to the set of unknown symbols of this context
//
// The difference of this function to fortran_get_variable_with_locus is that
// fortran_get_variable_with_locus always creates a SK_VARIABLE
static scope_entry_t* get_symbol_for_name_(decl_context_t decl_context, 
        AST locus, const char* name,
        char no_implicit)
{
    scope_entry_t* result = NULL;
    scope_entry_list_t* entry_list = query_in_scope_str_flags(decl_context, strtolower(name), DF_ONLY_CURRENT_SCOPE);
    if (entry_list != NULL)
    {
        result = entry_list_head(entry_list);
        entry_list_free(entry_list);
    }
    if (result == NULL)
    {
        result = new_fortran_symbol(decl_context, name);
        if (!no_implicit)
        {
            result->type_information = get_implicit_type_for_symbol(decl_context, result->symbol_name);
        }
        else
        {
            result->type_information = get_void_type();
        }

        add_untyped_symbol(decl_context, result);

        result->entity_specs.is_implicit_basic_type = 1;
        result->file = ASTFileName(locus);
        result->line = ASTLine(locus);

        if (decl_context.current_scope->related_entry != NULL
                && (decl_context.current_scope->related_entry->kind == SK_MODULE
                    || decl_context.current_scope->related_entry->kind == SK_BLOCKDATA))
        {
            scope_entry_t* module = decl_context.current_scope->related_entry;

            P_LIST_ADD_ONCE(module->entity_specs.related_symbols,
                    module->entity_specs.num_related_symbols,
                    result);

            result->entity_specs.in_module = module;
        }
    }

    return result;
}

static scope_entry_t* get_symbol_for_name(decl_context_t decl_context, AST locus, const char* name)
{
    return get_symbol_for_name_(decl_context, locus, name, /* no_implicit */ 0);
}

static scope_entry_t* get_symbol_for_name_untyped(decl_context_t decl_context, AST locus, const char* name)
{
    return get_symbol_for_name_(decl_context, locus, name, /* no_implicit */ 1);
}

static void build_scope_main_program_unit(AST program_unit, decl_context_t
        program_unit_context, scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output);
static void build_scope_subroutine_program_unit(AST program_unit,
        decl_context_t program_unit_context, scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output);
static void build_scope_function_program_unit(AST program_unit, decl_context_t
        program_unit_context, scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output);
static void build_scope_module_program_unit(AST program_unit, decl_context_t
        program_unit_context, scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output);
static void build_scope_block_data_program_unit(AST program_unit,
        decl_context_t program_unit_context, scope_entry_t** program_unit_symbol, 
        nodecl_t* nodecl_output);

static void build_global_program_unit(AST program_unit);

static void handle_opt_value_list(AST io_stmt, AST opt_value_list,
        decl_context_t decl_context,
        nodecl_t* nodecl_output);

static void build_scope_program_unit_internal(AST program_unit, 
        decl_context_t decl_context,
        scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output)
{
    scope_entry_t* _program_unit_symbol = NULL;

    switch (ASTType(program_unit))
    {
        case AST_MAIN_PROGRAM_UNIT:
            {
                decl_context_t program_unit_context = new_program_unit_context(decl_context);

                build_scope_main_program_unit(program_unit, program_unit_context, &_program_unit_symbol, nodecl_output);
                break;
            }
        case AST_SUBROUTINE_PROGRAM_UNIT:
            {
                decl_context_t program_unit_context = new_program_unit_context(decl_context);

                build_scope_subroutine_program_unit(program_unit, program_unit_context, &_program_unit_symbol, nodecl_output);
                break;
            }
        case AST_FUNCTION_PROGRAM_UNIT:
            {
                decl_context_t program_unit_context = new_program_unit_context(decl_context);

                build_scope_function_program_unit(program_unit, program_unit_context, &_program_unit_symbol, nodecl_output);
                break;
            }
        case AST_MODULE_PROGRAM_UNIT :
            {
                decl_context_t program_unit_context = new_program_unit_context(decl_context);

                build_scope_module_program_unit(program_unit, program_unit_context, &_program_unit_symbol, nodecl_output);
                break;
            }
        case AST_BLOCK_DATA_PROGRAM_UNIT:
            {
                decl_context_t program_unit_context = new_program_unit_context(decl_context);

                build_scope_block_data_program_unit(program_unit, program_unit_context, &_program_unit_symbol, nodecl_output);
                break;
            }
        case AST_GLOBAL_PROGRAM_UNIT:
            {
                //  This is a Mercurium extension. 
                //  It does not generate any sort of nodecl (and if it does it is merrily ignored)
                //  Everything is signed in a global scope
                build_global_program_unit(program_unit);
                break;
            }
        default:
            {
                internal_error("Unhandled node type '%s'\n", ast_print_node_type(ASTType(program_unit)));
            }
    }

    if (program_unit_symbol != NULL)
    {
        *program_unit_symbol = _program_unit_symbol;
    }
}

static scope_entry_t* new_procedure_symbol(decl_context_t decl_context, 
        AST name, AST prefix, AST suffix, AST dummy_arg_name_list,
        char is_function);

static char allow_all_statements(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER)
{
    return 1;
}

static void build_scope_program_unit_body(
        AST program_unit_stmts,
        AST internal_subprograms,
        decl_context_t decl_context,
        char (*allowed_statement)(AST, decl_context_t),
        nodecl_t* nodecl_output,
        nodecl_t* nodecl_internal_subprograms);

static void build_scope_main_program_unit(AST program_unit, 
        decl_context_t program_unit_context, 
        scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(program_unit_symbol == NULL, "Invalid parameter", 0)
    DEBUG_CODE()
    {
        fprintf(stderr, "==== [%s] Program unit: PROGRAM ===\n", ast_location(program_unit));
    }

    AST program_stmt = ASTSon0(program_unit);
    const char * program_name = "__MAIN__";
    if (program_stmt != NULL)
    {
        AST name = ASTSon0(program_stmt);
        program_name = ASTText(name);
    }
    scope_entry_t* program_sym = new_fortran_symbol(program_unit_context, program_name);
    program_sym->kind = SK_PROGRAM;
    program_sym->file = ASTFileName(program_unit);
    program_sym->line = ASTLine(program_unit);
    
    remove_unknown_kind_symbol(program_unit_context, program_sym);

    insert_alias(program_unit_context.current_scope->contained_in, program_sym,
            strappend("._", program_sym->symbol_name));

    program_sym->related_decl_context = program_unit_context;
    program_unit_context.current_scope->related_entry = program_sym;

    *program_unit_symbol = program_sym;

    AST program_body = ASTSon1(program_unit);

    nodecl_t nodecl_body = nodecl_null();
    nodecl_t nodecl_internal_subprograms = nodecl_null();
    if (program_body != NULL)
    {
        AST top_level = ASTSon0(program_body);
        AST statement_seq = ASTSon0(top_level);
        AST internal_subprograms = ASTSon1(program_body);

        build_scope_program_unit_body(
                statement_seq, internal_subprograms,
                program_unit_context, allow_all_statements, &nodecl_body, &nodecl_internal_subprograms);
    }

    if (nodecl_is_null(nodecl_body))
    {
        nodecl_body = nodecl_make_list_1(nodecl_make_empty_statement(ASTFileName(program_unit), ASTLine(program_unit)));

    }

    *nodecl_output = nodecl_make_list_1(
            nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_body,
                    program_unit_context,
                    ASTFileName(program_unit), 
                    ASTLine(program_unit)),
                /* initializers */ nodecl_null(),
                nodecl_internal_subprograms,
                program_sym,
                ASTFileName(program_unit), ASTLine(program_unit)));
}

static scope_entry_t* register_function(AST program_unit, 
        decl_context_t program_unit_context)
{
    AST function_stmt = ASTSon0(program_unit);

    AST prefix = ASTSon0(function_stmt);
    AST name = ASTSon1(function_stmt);
    AST function_prototype = ASTSon2(function_stmt);

    AST dummy_arg_name_list = ASTSon0(function_prototype);
    AST suffix = ASTSon1(function_prototype);

    scope_entry_t *new_entry = new_procedure_symbol(program_unit_context,
            name, prefix, suffix, 
            dummy_arg_name_list, /* is_function */ 1);

    return new_entry;
}


static void build_scope_function_program_unit(AST program_unit, 
        decl_context_t program_unit_context, 
        scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(program_unit_symbol == NULL, "Invalid parameter", 0)
    DEBUG_CODE()
    {
        fprintf(stderr, "==== [%s] Program unit: FUNCTION ===\n", ast_location(program_unit));
    }

    scope_entry_t* new_entry = register_function(program_unit, program_unit_context);

    if (new_entry == NULL)
        return;

    insert_alias(program_unit_context.current_scope->contained_in, new_entry,
            strappend("._", new_entry->symbol_name));

    *program_unit_symbol = new_entry;

    nodecl_t nodecl_body = nodecl_null();
    nodecl_t nodecl_internal_subprograms = nodecl_null();
    AST program_body = ASTSon1(program_unit);
    if (program_body != NULL)
    {
        AST top_level = ASTSon0(program_body);
        AST statement_seq = ASTSon0(top_level);
        AST internal_subprograms = ASTSon1(program_body);

        build_scope_program_unit_body(
                statement_seq, internal_subprograms,
                program_unit_context, allow_all_statements, &nodecl_body, &nodecl_internal_subprograms);
    }

    if (nodecl_is_null(nodecl_body))
    {
        nodecl_body = nodecl_make_list_1(nodecl_make_empty_statement(ASTFileName(program_unit), ASTLine(program_unit)));
    }

    int i, num_params = new_entry->entity_specs.num_related_symbols;
    for (i = 0; i < num_params; i++)
    {
        // This happens because nobody uses these dummies (or
        // result) symbols and their undefined state remains
        // Fix them to be plain variables
        if (new_entry->entity_specs.related_symbols[i]->kind == SK_UNDEFINED)
        {
            new_entry->entity_specs.related_symbols[i]->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(program_unit_context, new_entry);
        }
    }

    *nodecl_output = nodecl_make_list_1(
            nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_body,
                    program_unit_context,
                    ASTFileName(program_unit), 
                    ASTLine(program_unit)),
                /* initializers */ nodecl_null(),
                nodecl_internal_subprograms,
                new_entry,
                ASTFileName(program_unit), ASTLine(program_unit)));
}

static scope_entry_t* register_subroutine(AST program_unit,
        decl_context_t program_unit_context)
{
    AST subroutine_stmt = ASTSon0(program_unit);

    AST prefix = ASTSon0(subroutine_stmt);
    AST name = ASTSon1(subroutine_stmt);

    AST function_prototype = ASTSon2(subroutine_stmt);

    AST dummy_arg_name_list = NULL;
    AST suffix = NULL;
    
    if (function_prototype != NULL)
    {
        dummy_arg_name_list = ASTSon0(function_prototype);
        suffix = ASTSon1(function_prototype);
    }

    scope_entry_t *new_entry = new_procedure_symbol(program_unit_context,
            name, prefix, suffix, 
            dummy_arg_name_list, /* is_function */ 0);

    return new_entry;
}

static void build_scope_subroutine_program_unit(AST program_unit, 
        decl_context_t program_unit_context, 
        scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(program_unit_symbol == NULL, "Invalid parameter", 0)
    DEBUG_CODE()
    {
        fprintf(stderr, "==== [%s] Program unit: SUBROUTINE ===\n", ast_location(program_unit));
    }

    scope_entry_t *new_entry = register_subroutine(program_unit, program_unit_context);

    *program_unit_symbol = new_entry;

    insert_alias(program_unit_context.current_scope->contained_in, new_entry,
            strappend("._", new_entry->symbol_name));


    // It is void but it is not implicit
    new_entry->entity_specs.is_implicit_basic_type = 0;

    *program_unit_symbol = new_entry;

    insert_alias(program_unit_context.current_scope->contained_in, new_entry,
            strappend("._", new_entry->symbol_name));

    nodecl_t nodecl_body = nodecl_null();
    nodecl_t nodecl_internal_subprograms = nodecl_null();
    AST program_body = ASTSon1(program_unit);
    if (program_body != NULL)
    {
        AST top_level = ASTSon0(program_body);
        AST statement_seq = ASTSon0(top_level);
        AST internal_subprograms = ASTSon1(program_body);

        build_scope_program_unit_body(
                statement_seq, internal_subprograms,
                program_unit_context, allow_all_statements, &nodecl_body, &nodecl_internal_subprograms);
    }

    if (nodecl_is_null(nodecl_body))
    {
        nodecl_body = nodecl_make_list_1(nodecl_make_empty_statement(ASTFileName(program_unit), ASTLine(program_unit)));
    }

    int i, num_params = new_entry->entity_specs.num_related_symbols;
    for (i = 0; i < num_params; i++)
    {
        // This happens because nobody uses these dummies (or
        // result) symbols and their undefined state remains
        // Fix them to be plain variables
        if (new_entry->entity_specs.related_symbols[i]->kind == SK_UNDEFINED)
        {
            new_entry->entity_specs.related_symbols[i]->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(program_unit_context, new_entry);
        }
    }

    *nodecl_output = nodecl_make_list_1(
            nodecl_make_function_code(
                nodecl_make_context(
                    nodecl_body,
                    program_unit_context,
                    ASTFileName(program_unit), 
                    ASTLine(program_unit)),
                /* initializers */ nodecl_null(),
                nodecl_internal_subprograms,
                new_entry,
                ASTFileName(program_unit), ASTLine(program_unit)));
}

static void build_scope_module_program_unit(AST program_unit, 
        decl_context_t program_unit_context,
        scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output)
{
    ERROR_CONDITION(program_unit_symbol == NULL, "Invalid parameter", 0)
    DEBUG_CODE()
    {
        fprintf(stderr, "==== [%s] Program unit: MODULE ===\n", ast_location(program_unit));
    }

    AST module_stmt = ASTSon0(program_unit);
    AST module_name = ASTSon0(module_stmt);

    scope_entry_t* new_entry = new_fortran_symbol(program_unit_context, ASTText(module_name));
    new_entry->kind = SK_MODULE;
    
    remove_unknown_kind_symbol(program_unit_context, new_entry);

    new_entry->related_decl_context = program_unit_context;
    new_entry->file = ASTFileName(module_stmt);
    new_entry->line = ASTLine(module_stmt);
    program_unit_context.current_scope->related_entry = new_entry;

    AST module_body = ASTSon1(program_unit);

    insert_alias(program_unit_context.current_scope->contained_in, new_entry,
            strappend("._", new_entry->symbol_name));

    *program_unit_symbol = new_entry;

    nodecl_t nodecl_body = nodecl_null();
    nodecl_t nodecl_internal_subprograms = nodecl_null();

    if (module_body != NULL)
    {
        AST statement_seq = ASTSon0(module_body);
        AST internal_subprograms = ASTSon1(module_body);

        build_scope_program_unit_body(
                statement_seq, internal_subprograms,
                program_unit_context, allow_all_statements, &nodecl_body, &nodecl_internal_subprograms);
    }

    // This deserves an explanation: if a module does not contain any procedure
    // we need to remember it appeared (use a NODECL_OBJECT_INIT) otherwise use
    // the procedures of the module
    if (nodecl_is_null(nodecl_internal_subprograms))
    {
        *nodecl_output = nodecl_make_list_1(
                nodecl_make_object_init(new_entry, ASTFileName(program_unit), ASTLine(program_unit)));
    }
    else
    {
        *nodecl_output = nodecl_internal_subprograms;
    }

    // Now adjust attributes of symbols
    int i, num_symbols = new_entry->entity_specs.num_related_symbols;
    for (i = 0; i < num_symbols; i++)
    {
        // This happens because nobody uses these module entities
        // symbols and their undefined state remains
        // Fix them to be plain variables
        if (new_entry->entity_specs.related_symbols[i]->kind == SK_UNDEFINED)
        {
            new_entry->entity_specs.related_symbols[i]->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(program_unit_context, new_entry);
        }

        // Fix their access
        if (new_entry->entity_specs.related_symbols[i]->entity_specs.access == AS_UNKNOWN)
        {
            if (new_entry->entity_specs.access == AS_PRIVATE)
                new_entry->entity_specs.related_symbols[i]->entity_specs.access = AS_PRIVATE;
            else
                new_entry->entity_specs.related_symbols[i]->entity_specs.access = AS_PUBLIC;
        }
    }


    // Keep the module in the file's module cache
    if (!CURRENT_CONFIGURATION->debug_options.disable_module_cache)
    {
        rb_tree_insert(CURRENT_COMPILED_FILE->module_cache, strtolower(new_entry->symbol_name), new_entry);
    }

    // Store the module in a file
    dump_module_info(new_entry);
}

static void build_scope_block_data_program_unit(AST program_unit,
        decl_context_t program_unit_context,
        scope_entry_t** program_unit_symbol,
        nodecl_t* nodecl_output)
{
    // FIXME
    // Do nothing with these
    DEBUG_CODE()
    {
        fprintf(stderr, "=== [%s] Program unit: BLOCK DATA ===\n", ast_location(program_unit));
    }

    ERROR_CONDITION(program_unit_symbol == NULL, "Invalid parameter", 0)
    DEBUG_CODE()
    {
        fprintf(stderr, "==== [%s] Program unit: PROGRAM ===\n", ast_location(program_unit));
    }

    AST program_stmt = ASTSon0(program_unit);
    const char * program_name = "__BLOCK_DATA_UNNAMED__";
    AST name = ASTSon0(program_stmt);
    if (name != NULL)
        program_name = ASTText(name);

    scope_entry_t* program_sym = new_fortran_symbol(program_unit_context, program_name);
    program_sym->kind = SK_BLOCKDATA;
    program_sym->file = ASTFileName(program_unit);
    program_sym->line = ASTLine(program_unit);
    
    remove_unknown_kind_symbol(program_unit_context, program_sym);
    
    insert_alias(program_unit_context.current_scope->contained_in, program_sym,
            strappend("._", program_sym->symbol_name));

    program_sym->related_decl_context = program_unit_context;
    program_unit_context.current_scope->related_entry = program_sym;

    *program_unit_symbol = program_sym;

    AST program_body = ASTSon1(program_unit);

    nodecl_t nodecl_body = nodecl_null();
    nodecl_t nodecl_internal_subprograms = nodecl_null();
    if (program_body != NULL)
    {
        AST statement_seq = program_body;
        AST internal_subprograms = NULL;

        build_scope_program_unit_body(
                statement_seq, internal_subprograms,
                program_unit_context, allow_all_statements, &nodecl_body, &nodecl_internal_subprograms);
    }

    *nodecl_output = nodecl_make_list_1(
            nodecl_make_object_init(program_sym, ASTFileName(program_unit), ASTLine(program_unit)));
}

static void build_global_program_unit(AST program_unit)
{
    decl_context_t program_unit_context = CURRENT_COMPILED_FILE->global_decl_context;
    program_unit_context.function_scope = program_unit_context.current_scope;

    AST program_body = ASTSon0(program_unit);

    AST statement_seq = ASTSon0(program_body);

    nodecl_t nodecl_body = nodecl_null();
    nodecl_t nodecl_internal_subprograms = nodecl_null();

    build_scope_program_unit_body(
            statement_seq, NULL,
            program_unit_context, 
            allow_all_statements, 
            &nodecl_body, 
            &nodecl_internal_subprograms);
}

static type_t* gather_type_from_declaration_type_spec_(AST a, 
        decl_context_t decl_context);

static type_t* gather_type_from_declaration_type_spec(AST a, decl_context_t decl_context)
{
    return gather_type_from_declaration_type_spec_(a, decl_context);
}

static scope_entry_t* new_procedure_symbol(decl_context_t decl_context, 
        AST name, AST prefix, AST suffix, AST dummy_arg_name_list,
        char is_function)
{
    scope_entry_t* entry = NULL;

    entry = fortran_query_name_str(decl_context, ASTText(name));

    if (entry != NULL)
    {
        // We do not allow redeclaration if the symbol has already been defined
        if (entry->defined
                // If not defined it can only be a parameter of the current procedure
                // being given an interface
                || (!entry->entity_specs.is_parameter
                    // Or an advanced declaration of a module procedure found in an INTERFACE at module level
                    && !entry->entity_specs.is_module_procedure
                    // Or a symbol we said something about it in the specification part of a module
                    && !(entry->kind == SK_UNDEFINED
                        && entry->entity_specs.in_module != NULL)))
        {
            error_printf("%s: error: redeclaration of entity '%s'\n", 
                    ast_location(name), 
                    ASTText(name));
            return NULL;
        }
        // It can't be redefined anymore
        if (entry->entity_specs.is_module_procedure)
        {
            entry->entity_specs.is_module_procedure = 0;
            remove_not_fully_defined_symbol(entry->decl_context, entry);
        }
        remove_unknown_kind_symbol(entry->decl_context, entry);
    }

    if (entry == NULL)
    {
        entry = new_fortran_symbol(decl_context, ASTText(name));
    }

    decl_context.current_scope->related_entry = entry;

    entry->kind = SK_FUNCTION;
    entry->file = ASTFileName(name);
    entry->line = ASTLine(name);
    entry->entity_specs.is_implicit_basic_type = 1;
    entry->defined = 1;
    
    remove_unknown_kind_symbol(decl_context, entry);

    type_t* return_type = get_void_type();
    if (is_function)
    {
        return_type = get_implicit_type_for_symbol(decl_context, entry->symbol_name);
    }
    else
    {
        // Not an implicit basic type anymore
        entry->entity_specs.is_implicit_basic_type = 0;
    }

    char function_has_type_spec = 0;
    if (prefix != NULL)
    {
        AST it;
        for_each_element(prefix, it)
        {
            AST prefix_spec = ASTSon1(it);
            ERROR_CONDITION(ASTType(prefix_spec) != AST_ATTR_SPEC, "Invalid tree", 0);

            const char* prefix_spec_str = ASTText(prefix_spec);

            if (strcasecmp(prefix_spec_str, "__declaration__") == 0)
            {
                if (!is_function)
                {
                    error_printf("%s: error: declaration type-specifier is only valid for FUNCTION statement\n",
                            ast_location(prefix_spec));
                }
                else
                {
                    AST declaration_type_spec = ASTSon0(prefix_spec);
                    return_type = gather_type_from_declaration_type_spec(declaration_type_spec, decl_context);

                    if (return_type != NULL)
                    {
                        entry->entity_specs.is_implicit_basic_type = 0;
                        entry->type_information = return_type;
                    }

                    function_has_type_spec = 1;
                }
            }
            else if (strcasecmp(prefix_spec_str, "elemental") == 0)
            {
                entry->entity_specs.is_elemental = 1;
            }
            else if (strcasecmp(prefix_spec_str, "pure") == 0)
            {
                entry->entity_specs.is_pure = 1;
            }
            else if (strcasecmp(prefix_spec_str, "recursive") == 0)
            {
                entry->entity_specs.is_recursive = 1;
            }
            else if ((strcasecmp(prefix_spec_str, "impure") == 0)
                    || (strcasecmp(prefix_spec_str, "module") == 0))
            {
                running_error("%s: error: unsupported specifier for procedures '%s'\n",
                        ast_location(prefix_spec),
                        fortran_prettyprint_in_buffer(prefix_spec));
            }
            else
            {
                internal_error("Invalid tree kind '%s' with spec '%s'\n", 
                        ast_print_node_type(ASTType(prefix_spec)),
                        ASTText(prefix_spec));
            }
        }
    }

    int num_dummy_arguments = 0;
    if (dummy_arg_name_list != NULL)
    {
        int num_alternate_returns = 0;
        AST it;
        for_each_element(dummy_arg_name_list, it)
        {
            AST dummy_arg_name = ASTSon1(it);

            scope_entry_t* dummy_arg = NULL;

            if (strcmp(ASTText(dummy_arg_name), "*") == 0)
            {
                if (is_function)
                {
                    error_printf("%s: error: alternate return is not allowed in a FUNCTION specification\n",
                            ast_location(dummy_arg_name));
                    continue;
                }

                char alternate_return_name[64];
                snprintf(alternate_return_name, 64, ".alternate-return-%d", num_alternate_returns);
                alternate_return_name[63] = '\0';

                dummy_arg = calloc(1, sizeof(*dummy_arg));

                dummy_arg->symbol_name = uniquestr(alternate_return_name);
                // This is actually a label parameter
                dummy_arg->kind = SK_LABEL;
                dummy_arg->type_information = get_void_type();
                dummy_arg->decl_context = decl_context;
                
                num_alternate_returns++;
            }
            else
            {
                dummy_arg = get_symbol_for_name(decl_context, dummy_arg_name, ASTText(dummy_arg_name));

                // Note that we do not set the exact kind of the dummy argument as
                // it might be a function. If left SK_UNDEFINED, we later fix them
                // to SK_VARIABLE
                // Get a reference to its type (it will be properly updated later)
                dummy_arg->type_information = get_lvalue_reference_type(dummy_arg->type_information);

                add_untyped_symbol(decl_context, dummy_arg);
            }

            dummy_arg->file = ASTFileName(dummy_arg_name);
            dummy_arg->line = ASTLine(dummy_arg_name);
            dummy_arg->entity_specs.is_parameter = 1;
            dummy_arg->entity_specs.parameter_position = num_dummy_arguments;

            P_LIST_ADD(entry->entity_specs.related_symbols,
                    entry->entity_specs.num_related_symbols,
                    dummy_arg);

            num_dummy_arguments++;
        }
    }

    AST result = NULL;
    if (suffix != NULL)
    {
        // AST binding_spec = ASTSon0(suffix);
        result = ASTSon1(suffix);
    }

    scope_entry_t* result_sym = NULL;
    if (result != NULL)
    {
        if (!is_function)
        {
            error_printf("%s: error: RESULT is only valid for FUNCTION statement\n",
                    ast_location(result));
        }
        else
        {
            result_sym = get_symbol_for_name(decl_context, result, ASTText(result));

            result_sym->kind = SK_VARIABLE;
            result_sym->file = ASTFileName(result);
            result_sym->line = ASTLine(result);
            result_sym->entity_specs.is_result = 1;
            
            remove_unknown_kind_symbol(decl_context, result_sym);

            result_sym->type_information = get_lvalue_reference_type(return_type);

            return_type = get_indirect_type(result_sym);

            P_LIST_ADD(entry->entity_specs.related_symbols,
                    entry->entity_specs.num_related_symbols,
                    result_sym);
        }
    }
    else if (is_function)
    {
        //Since this function does not have an explicit result variable we will insert an alias
        //that will hide the function name
        result_sym = new_symbol(decl_context, decl_context.current_scope, entry->symbol_name);
        result_sym->kind = SK_VARIABLE;
        result_sym->file = entry->file;
        result_sym->line = entry->line;
        result_sym->entity_specs.is_result = 1;

        result_sym->type_information = get_lvalue_reference_type(return_type);
        result_sym->entity_specs.is_implicit_basic_type = !function_has_type_spec;

        if (!function_has_type_spec)
        {
            // Add it as an explicit unknown symbol because we want it to be
            // updated with a later IMPLICIT
            add_untyped_symbol(decl_context, result_sym);
        }

        return_type = get_indirect_type(result_sym);

        P_LIST_ADD(entry->entity_specs.related_symbols,
                entry->entity_specs.num_related_symbols,
                result_sym);
    }

    // Try to come up with a sensible type for this entity
    parameter_info_t parameter_info[num_dummy_arguments + 1];
    memset(parameter_info, 0, sizeof(parameter_info));

    int i;
    for (i = 0; i < num_dummy_arguments; i++)
    {
        parameter_info[i].type_info = get_indirect_type(entry->entity_specs.related_symbols[i]);
    }

    type_t* function_type = get_new_function_type(return_type, parameter_info, num_dummy_arguments);
    entry->type_information = function_type;

    entry->entity_specs.is_implicit_basic_type = 0;
    entry->related_decl_context = decl_context;

    if (decl_context.current_scope->contained_in != NULL)
    {
        scope_entry_t* enclosing_symbol = decl_context.current_scope->contained_in->related_entry;

        if (enclosing_symbol != NULL)
        {
            if (enclosing_symbol->kind == SK_FUNCTION
                    || enclosing_symbol->kind == SK_PROGRAM
                    || enclosing_symbol->kind == SK_MODULE)
            {
                insert_entry(enclosing_symbol->decl_context.current_scope, entry);

                if (enclosing_symbol->kind == SK_MODULE)
                {
                    // If we are enclosed by a module, we are a module procedure
                    entry->entity_specs.in_module = enclosing_symbol;

                    P_LIST_ADD(enclosing_symbol->entity_specs.related_symbols,
                            enclosing_symbol->entity_specs.num_related_symbols,
                            entry);
                }
            }
        }
    }

    return entry;
}

static scope_entry_t* new_entry_symbol(decl_context_t decl_context, 
        AST name, AST suffix, AST dummy_arg_name_list,
        char is_function)
{
    scope_entry_t* entry = NULL;
    entry = fortran_query_name_str(decl_context, ASTText(name));
    char exist_entry = (entry != NULL);
    if (exist_entry)
    {
        // We do not allow redeclaration if the symbol has already been defined
        if ( entry->defined
                // If not defined it can only be a parameter of the current procedure
                // being given an interface
                || (!entry->entity_specs.is_parameter
                    // Or an advanced declaration of a module procedure found in an INTERFACE at module level
                    && !entry->entity_specs.is_module_procedure
                    // Or a symbol we said something about it in the specification part of a module
                    && !(entry->kind == SK_UNDEFINED
                        && entry->entity_specs.in_module != NULL)
                    // Or an advanced declaration of the function return type
                    && !(entry->kind == SK_UNDEFINED 
                            && is_function)))
        {
            error_printf("%s: error: redeclaration of entity '%s'\n", 
                    ast_location(name), 
                    ASTText(name));
            return NULL;
        }
        // It can't be redefined anymore
        if (entry->entity_specs.is_module_procedure)
        {
            entry->entity_specs.is_module_procedure = 0;
            remove_not_fully_defined_symbol(entry->decl_context, entry);
        }
    }
    else
    {
        entry = new_fortran_symbol(decl_context, ASTText(name));
    }

    entry->kind = SK_FUNCTION;
    entry->file = ASTFileName(name);
    entry->line = ASTLine(name);
    entry->entity_specs.is_entry = 1;
    entry->entity_specs.is_implicit_basic_type = 1;
    entry->defined = 1;
    remove_unknown_kind_symbol(decl_context, entry);
    

    type_t* return_type = get_void_type();
    if (is_function)
    {
        // The return type has been specified
        if(exist_entry)
        {
            entry->entity_specs.is_implicit_basic_type = 0;
            return_type = 
                entry->type_information;
        }
        else
        {
            return_type = 
                get_implicit_type_for_symbol(decl_context, entry->symbol_name);
        }
    }
    else
    {
        // Not an implicit basic type anymore
        entry->entity_specs.is_implicit_basic_type = 0;
    }

    int num_dummy_arguments = 0;
    if (dummy_arg_name_list != NULL)
    {
        int num_alternate_returns = 0;
        AST it;
        for_each_element(dummy_arg_name_list, it)
        {
            AST dummy_arg_name = ASTSon1(it);

            scope_entry_t* dummy_arg = NULL;
            if (strcmp(ASTText(dummy_arg_name), "*") == 0)
            {
                if (is_function)
                {
                    error_printf("%s: error: alternate return is not allowed in a FUNCTION specification\n",
                            ast_location(dummy_arg_name));
                    continue;
                }

                char alternate_return_name[64];
                snprintf(alternate_return_name, 64, ".alternate-return-%d", num_alternate_returns);
                alternate_return_name[63] = '\0';

                dummy_arg = calloc(1, sizeof(*dummy_arg));

                dummy_arg->symbol_name = uniquestr(alternate_return_name);
                // This is actually a label parameter
                dummy_arg->kind = SK_LABEL;
                dummy_arg->type_information = get_void_type();
                dummy_arg->decl_context = decl_context;
                
                num_alternate_returns++;
            }
            else
            {
                dummy_arg = get_symbol_for_name(decl_context, dummy_arg_name, ASTText(dummy_arg_name));

                // Note that we do not set the exact kind of the dummy argument as
                // it might be a function. If left SK_UNDEFINED, we later fix them
                // to SK_VARIABLE
                // Get a reference to its type (it will be properly updated later)
                if (!is_lvalue_reference_type(dummy_arg->type_information)) 
                {
                    dummy_arg->type_information = get_lvalue_reference_type(dummy_arg->type_information);
                    add_untyped_symbol(decl_context, dummy_arg);
                }
            }
            dummy_arg->file = ASTFileName(dummy_arg_name);
            dummy_arg->line = ASTLine(dummy_arg_name);
            dummy_arg->entity_specs.is_parameter = 1;
            dummy_arg->entity_specs.parameter_position = num_dummy_arguments;

            P_LIST_ADD(entry->entity_specs.related_symbols,
                    entry->entity_specs.num_related_symbols,
                    dummy_arg);
            
            num_dummy_arguments++;
        }
    }

    AST result = NULL;
    if (suffix != NULL)
    {
        // AST binding_spec = ASTSon0(suffix);
        result = ASTSon1(suffix);
    }

    scope_entry_t* result_sym = NULL;
    if (result != NULL)
    {
        if (!is_function)
        {
            error_printf("%s: error: RESULT is only valid for FUNCTION statement\n",
                    ast_location(result));
        }
        else
        {
            result_sym = get_symbol_for_name(decl_context, result, ASTText(result));

            result_sym->kind = SK_VARIABLE;
            result_sym->file = ASTFileName(result);
            result_sym->line = ASTLine(result);
            result_sym->entity_specs.is_result = 1;
            
            remove_unknown_kind_symbol(decl_context, result_sym);

            result_sym->type_information = get_lvalue_reference_type(return_type);

            return_type = get_indirect_type(result_sym);

            P_LIST_ADD(entry->entity_specs.related_symbols,
                    entry->entity_specs.num_related_symbols,
                    result_sym);
        }
    }
    else if (is_function)
    {
        //Since this function does not have an explicit result variable we will insert an alias
        //that will hide the function name
        result_sym = new_symbol(decl_context, decl_context.current_scope, entry->symbol_name);
        result_sym->kind = SK_VARIABLE;
        result_sym->file = entry->file;
        result_sym->line = entry->line;
        result_sym->entity_specs.is_result = 1;

        char function_has_type_spec = 0;
        result_sym->type_information = get_lvalue_reference_type(return_type);
        result_sym->entity_specs.is_implicit_basic_type = !function_has_type_spec;

        if (!function_has_type_spec)
        {
            // Add it as an explicit unknown symbol because we want it to be
            // updated with a later IMPLICIT
            add_untyped_symbol(decl_context, result_sym);
        }

        return_type = get_indirect_type(result_sym);

        P_LIST_ADD(entry->entity_specs.related_symbols,
                entry->entity_specs.num_related_symbols,
                result_sym);
    }

    // Try to come up with a sensible type for this entity
    parameter_info_t parameter_info[num_dummy_arguments + 1];
    memset(parameter_info, 0, sizeof(parameter_info));

    int i;
    for (i = 0; i < num_dummy_arguments; i++)
    {
        parameter_info[i].type_info = get_indirect_type(entry->entity_specs.related_symbols[i]);
    }

    type_t* function_type = get_new_function_type(return_type, parameter_info, num_dummy_arguments);
    entry->type_information = function_type;

    entry->entity_specs.is_implicit_basic_type = 0;
    entry->related_decl_context = decl_context;
   

    scope_entry_t* related_entry = decl_context.current_scope->related_entry;
    if (related_entry != NULL 
            && related_entry->entity_specs.in_module != NULL)
    {
        scope_entry_t * sym_module = related_entry->entity_specs.in_module;
        
        entry->entity_specs.in_module = sym_module;
        P_LIST_ADD(sym_module->entity_specs.related_symbols,
                sym_module->entity_specs.num_related_symbols,
                entry);
    }

    return entry;
}

static char statement_is_executable(AST statement);
static void build_scope_ambiguity_statement(AST ambig_stmt, decl_context_t decl_context);

static void build_scope_program_unit_body_declarations(
        char (*allowed_statement)(AST, decl_context_t),
        AST program_unit_stmts,
        decl_context_t decl_context,
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    if (program_unit_stmts != NULL)
    {
        AST it;
        for_each_element(program_unit_stmts, it)
        {
            AST stmt = ASTSon1(it);

            // Exceptionally we run this one first otherwise it is not possible to
            // tell whether this is an executable or non-executable statement
            if (ASTType(stmt) == AST_AMBIGUITY)
            {
                build_scope_ambiguity_statement(stmt, decl_context);
            }

            if (!allowed_statement(stmt, decl_context))
            {
                error_printf("%s: warning: this statement cannot be used in this context\n",
                        ast_location(stmt));
            }
            
            // We only handle nonexecutable statements here and ENTRY statement
            // (which is an oddly defined "executable" statement)
            if (ASTType(stmt) != AST_ENTRY_STATEMENT
                    && statement_is_executable(stmt))
                continue;

            // Nonexecutable statements should not generate nodecls
            // If we pass a NULL we will detect such an attempt 
            fortran_build_scope_statement(stmt, decl_context, NULL);
        }
    }
}

static void build_scope_program_unit_body_executable(
        char (*allowed_statement)(AST, decl_context_t),
        AST program_unit_stmts,
        decl_context_t decl_context,
        nodecl_t* nodecl_output)
{
    if (program_unit_stmts != NULL)
    {
        AST it;
        for_each_element(program_unit_stmts, it)
        {
            AST stmt = ASTSon1(it);

            // Exceptionally we run this one first otherwise it is not possible to
            // tell whether this is an executable or non-executable statement
            if (ASTType(stmt) == AST_AMBIGUITY)
            {
                build_scope_ambiguity_statement(stmt, decl_context);
            }

            if (!allowed_statement(stmt, decl_context))
            {
                error_printf("%s: warning: this statement cannot be used in this context\n",
                        ast_location(stmt));
            }
            
            // We only handle executable statements here
            if (!statement_is_executable(stmt))
                continue;

            nodecl_t nodecl_statement = nodecl_null();
            fortran_build_scope_statement(stmt, decl_context, &nodecl_statement);

            *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_statement);
        }
    }
}

typedef
struct internal_subprograms_info_tag
{
    scope_entry_t* symbol;
    decl_context_t decl_context;
    nodecl_t nodecl_output;
    AST program_unit_stmts;
    AST internal_subprograms;
    const char* filename;
    int line;
} internal_subprograms_info_t;

static int count_internal_subprograms(AST internal_subprograms)
{
    int num_internal_program_units = 0;
    if (internal_subprograms != NULL)
    {
        AST it;
        for_each_element(internal_subprograms, it)
        {
            num_internal_program_units++;
        }
    }
    return num_internal_program_units;
}

static void build_scope_program_unit_body_internal_subprograms_declarations(
        AST internal_subprograms, 
        int num_internal_program_units,
        internal_subprograms_info_t *internal_subprograms_info,
        decl_context_t decl_context)
{
    if (internal_subprograms == NULL)
        return;

    int i = 0;
    AST it;
    for_each_element(internal_subprograms, it)
    {
        ERROR_CONDITION(i >= num_internal_program_units, "Too many internal subprograms", 0);

        decl_context_t subprogram_unit_context = new_internal_program_unit_context(decl_context);

        AST subprogram = ASTSon1(it);

        scope_entry_t* new_entry = NULL;
        switch (ASTType(subprogram))
        {
            case AST_SUBROUTINE_PROGRAM_UNIT:
                {
                    new_entry = register_subroutine(subprogram, subprogram_unit_context);
                    break;
                }
            case AST_FUNCTION_PROGRAM_UNIT:
                {
                    new_entry = register_function(subprogram, subprogram_unit_context);
                    break;
                }
            default:
                {
                    internal_error("Unexpected node of kind %s\n", ast_print_node_type(ASTType(subprogram)));
                }
        }

        if (new_entry != NULL)
        {
            AST program_body = ASTSon1(subprogram);

            AST program_part = ASTSon0(program_body);
            AST program_unit_stmts = ASTSon0(program_part);
            AST n_internal_subprograms = ASTSon1(program_body);

            internal_subprograms_info[i].symbol = new_entry;
            internal_subprograms_info[i].decl_context = subprogram_unit_context;
            internal_subprograms_info[i].program_unit_stmts = program_unit_stmts;
            internal_subprograms_info[i].internal_subprograms = n_internal_subprograms;
            internal_subprograms_info[i].filename = ASTFileName(program_body);
            internal_subprograms_info[i].line = ASTLine(program_body);

            build_scope_program_unit_body_declarations(
                    allow_all_statements,
                    internal_subprograms_info[i].program_unit_stmts, 
                    internal_subprograms_info[i].decl_context,
                    &(internal_subprograms_info[i].nodecl_output));
        }
        i++;
    }
}

static void build_scope_program_unit_body_internal_subprograms_executable(
        AST internal_subprograms, 
        int num_internal_program_units,
        internal_subprograms_info_t *internal_subprograms_info,
        decl_context_t decl_context UNUSED_PARAMETER)
{
    if (internal_subprograms == NULL)
        return;

    int i = 0;
    AST it;
    for_each_element(internal_subprograms, it)
    {
        ERROR_CONDITION(i >= num_internal_program_units, "Too many internal program units", 0);

        // Some error happened during
        // build_scope_program_unit_body_internal_subprograms_declarations and
        // we could not get any symbol for this one. Skip it
        if (internal_subprograms_info[i].symbol != NULL)
        {
            AST n_internal_subprograms = internal_subprograms_info[i].internal_subprograms;

            int n_num_internal_program_units = count_internal_subprograms(n_internal_subprograms);
            // Count how many internal subprograms are there
            internal_subprograms_info_t n_internal_subprograms_info[n_num_internal_program_units + 1];
            memset(n_internal_subprograms_info, 0, sizeof(n_internal_subprograms_info));

            build_scope_program_unit_body_internal_subprograms_declarations(
                    n_internal_subprograms, 
                    n_num_internal_program_units,
                    n_internal_subprograms_info,
                    internal_subprograms_info[i].decl_context);

            // Insert the internal program unit names into the enclosing scope
            int j;
            for (j = 0; j < n_num_internal_program_units; j++)
            {
                insert_entry(internal_subprograms_info[j].decl_context.current_scope, 
                        n_internal_subprograms_info[j].symbol);
            }

            build_scope_program_unit_body_executable(
                    allow_all_statements,
                    internal_subprograms_info[i].program_unit_stmts,
                    internal_subprograms_info[i].decl_context,
                    &(internal_subprograms_info[i].nodecl_output));

            build_scope_program_unit_body_internal_subprograms_executable(
                    n_internal_subprograms, 
                    n_num_internal_program_units,
                    n_internal_subprograms_info,
                    internal_subprograms_info[i].decl_context);

            // Check all the symbols of this program unit
            check_untyped_symbols(internal_subprograms_info[i].decl_context);
            check_not_fully_defined_symbols(internal_subprograms_info[i].decl_context);
            review_unknown_kind_symbol(internal_subprograms_info[i].decl_context);

            // 6) Remember the internal subprogram nodecls
            nodecl_t nodecl_internal_subprograms = nodecl_null();
            for (j = 0; j < n_num_internal_program_units; j++)
            {
                nodecl_internal_subprograms =
                    nodecl_append_to_list(nodecl_internal_subprograms, 
                            n_internal_subprograms_info[j].nodecl_output);
            }


            scope_entry_t* function_symbol = internal_subprograms_info[i].symbol;
            int num_params = function_symbol->entity_specs.num_related_symbols;
            for (j = 0; j < num_params; j++)
            {
                // This happens because nobody uses these dummies (or
                // result) symbols and their undefined state remains
                // Fix them to be plain variables
                if (function_symbol->entity_specs.related_symbols[j]->kind == SK_UNDEFINED)
                {
                    function_symbol->entity_specs.related_symbols[j]->kind = SK_VARIABLE;
                    remove_unknown_kind_symbol(decl_context, function_symbol);
                }
            }

            nodecl_t nodecl_statements = internal_subprograms_info[i].nodecl_output;
            if (nodecl_is_null(nodecl_statements))
            {
                nodecl_statements = nodecl_make_list_1(
                        nodecl_make_empty_statement(
                            internal_subprograms_info[i].filename,
                            internal_subprograms_info[i].line));
            }

            internal_subprograms_info[i].nodecl_output = 
                nodecl_make_function_code(
                        nodecl_make_context(
                            nodecl_statements,
                            internal_subprograms_info[i].decl_context,
                            internal_subprograms_info[i].filename,
                            internal_subprograms_info[i].line),
                        /* initializers */ nodecl_null(),
                        nodecl_internal_subprograms,
                        internal_subprograms_info[i].symbol,
                        internal_subprograms_info[i].filename,
                        internal_subprograms_info[i].line);
        }
        i++;
    }
}

// This function is only used for top level program units
static void build_scope_program_unit_body(
        AST program_unit_stmts,
        AST internal_subprograms,
        decl_context_t decl_context,
        char (*allowed_statement)(AST, decl_context_t),
        nodecl_t* nodecl_output,
        nodecl_t* nodecl_internal_subprograms)
{
    // 1) Program unit declaration only
    build_scope_program_unit_body_declarations(
            allowed_statement,
            program_unit_stmts, 
            decl_context, 
            nodecl_output);

    int num_internal_program_units = count_internal_subprograms(internal_subprograms);
    // Count how many internal subprograms are there
    internal_subprograms_info_t internal_program_units_info[num_internal_program_units + 1];
    memset(internal_program_units_info, 0, sizeof(internal_program_units_info));

    // 2) Internal program units, declaration statements only
    build_scope_program_unit_body_internal_subprograms_declarations(
            internal_subprograms, 
            num_internal_program_units,
            internal_program_units_info,
            decl_context);
    
    // 3) Program unit remaining statements
    build_scope_program_unit_body_executable(
            allowed_statement,
            program_unit_stmts, 
            decl_context,
            nodecl_output);
    
    // 4) Internal program units remaining statements
    build_scope_program_unit_body_internal_subprograms_executable(
            internal_subprograms, 
            num_internal_program_units,
            internal_program_units_info,
            decl_context);
    
    // 5) Check all symbols of this program unit
    check_untyped_symbols(decl_context);
    check_not_fully_defined_symbols(decl_context);
    review_unknown_kind_symbol(decl_context);

    // 6) Remember the internal subprogram nodecls
    int i;
    for (i = 0; i < num_internal_program_units; i++)
    {
        *nodecl_internal_subprograms =
            nodecl_append_to_list(*nodecl_internal_subprograms, 
                    internal_program_units_info[i].nodecl_output);
    }
}

typedef void (*build_scope_statement_function_t)(AST statement, decl_context_t, nodecl_t* nodecl_output);

typedef
enum statement_kind_tag
{
    STMT_KIND_UNKNOWN = 0,
    STMT_KIND_EXECUTABLE = 1,
    STMT_KIND_NONEXECUTABLE = 2,
} statement_kind_t;

static statement_kind_t kind_nonexecutable_0(AST a UNUSED_PARAMETER)
{
    return STMT_KIND_NONEXECUTABLE;
}

static statement_kind_t kind_executable_0(AST a UNUSED_PARAMETER)
{
    return STMT_KIND_EXECUTABLE;
}

static statement_kind_t kind_of_son_1(AST a);

typedef struct build_scope_statement_handler_tag
{
    node_t ast_kind;
    build_scope_statement_function_t handler;
    statement_kind_t (*statement_kind)(AST);
} build_scope_statement_handler_t;

#define STATEMENT_HANDLER_TABLE \
 STATEMENT_HANDLER(AST_ACCESS_STATEMENT,              build_scope_access_stmt,           kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_ALLOCATABLE_STATEMENT,         build_scope_allocatable_stmt,      kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_ALLOCATE_STATEMENT,            build_scope_allocate_stmt,         kind_executable_0    ) \
 STATEMENT_HANDLER(AST_ALL_STOP_STATEMENT,            build_scope_allstop_stmt,          kind_executable_0    ) \
 STATEMENT_HANDLER(AST_ARITHMETIC_IF_STATEMENT,       build_scope_arithmetic_if_stmt,    kind_executable_0    ) \
 STATEMENT_HANDLER(AST_EXPRESSION_STATEMENT,          build_scope_expression_stmt,       kind_executable_0    ) \
 STATEMENT_HANDLER(AST_ASSOCIATE_CONSTRUCT,           build_scope_associate_construct,   kind_executable_0    ) \
 STATEMENT_HANDLER(AST_ASYNCHRONOUS_STATEMENT,        build_scope_asynchronous_stmt,     kind_executable_0    ) \
 STATEMENT_HANDLER(AST_IO_STATEMENT,                  build_io_stmt,                     kind_executable_0    ) \
 STATEMENT_HANDLER(AST_BIND_STATEMENT,                build_scope_bind_stmt,             kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_BLOCK_CONSTRUCT,               build_scope_block_construct,       kind_executable_0    ) \
 STATEMENT_HANDLER(AST_SWITCH_STATEMENT,              build_scope_case_construct,        kind_executable_0    ) \
 STATEMENT_HANDLER(AST_CASE_STATEMENT,                build_scope_case_statement,        kind_executable_0    ) \
 STATEMENT_HANDLER(AST_DEFAULT_STATEMENT,             build_scope_default_statement,     kind_executable_0    ) \
 STATEMENT_HANDLER(AST_CLOSE_STATEMENT,               build_scope_close_stmt,            kind_executable_0    ) \
 STATEMENT_HANDLER(AST_CODIMENSION_STATEMENT,         build_scope_codimension_stmt,      kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_COMMON_STATEMENT,              build_scope_common_stmt,           kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_COMPOUND_STATEMENT,            build_scope_compound_statement,    kind_executable_0    ) \
 STATEMENT_HANDLER(AST_COMPUTED_GOTO_STATEMENT,       build_scope_computed_goto_stmt,    kind_executable_0    ) \
 STATEMENT_HANDLER(AST_ASSIGNED_GOTO_STATEMENT,       build_scope_assigned_goto_stmt,    kind_executable_0    ) \
 STATEMENT_HANDLER(AST_LABEL_ASSIGN_STATEMENT,        build_scope_label_assign_stmt,     kind_executable_0    ) \
 STATEMENT_HANDLER(AST_LABELED_STATEMENT,             build_scope_labeled_stmt,          kind_of_son_1        ) \
 STATEMENT_HANDLER(AST_EMPTY_STATEMENT,               build_scope_continue_stmt,         kind_executable_0    ) \
 STATEMENT_HANDLER(AST_CRITICAL_CONSTRUCT,            build_scope_critical_construct,    kind_executable_0    ) \
 STATEMENT_HANDLER(AST_CONTINUE_STATEMENT,            build_scope_cycle_stmt,            kind_executable_0    ) \
 STATEMENT_HANDLER(AST_DATA_STATEMENT,                build_scope_data_stmt,             kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_DEALLOCATE_STATEMENT,          build_scope_deallocate_stmt,       kind_executable_0    ) \
 STATEMENT_HANDLER(AST_DERIVED_TYPE_DEF,              build_scope_derived_type_def,      kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_DIMENSION_STATEMENT,           build_scope_dimension_stmt,        kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_FOR_STATEMENT,                 build_scope_do_construct,          kind_executable_0    ) \
 STATEMENT_HANDLER(AST_ENUM_DEF,                      build_scope_enum_def,              kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_EQUIVALENCE_STATEMENT,         build_scope_equivalence_stmt,      kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_BREAK_STATEMENT,               build_scope_exit_stmt,             kind_executable_0    ) \
 STATEMENT_HANDLER(AST_EXTERNAL_STATEMENT,            build_scope_external_stmt,         kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_FORALL_CONSTRUCT,              build_scope_forall_construct,      kind_executable_0    ) \
 STATEMENT_HANDLER(AST_FORALL_STATEMENT,              build_scope_forall_stmt,           kind_executable_0    ) \
 STATEMENT_HANDLER(AST_FORMAT_STATEMENT,              build_scope_format_stmt,           kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_GOTO_STATEMENT,                build_scope_goto_stmt,             kind_executable_0    ) \
 STATEMENT_HANDLER(AST_IF_ELSE_STATEMENT,             build_scope_if_construct,          kind_executable_0    ) \
 STATEMENT_HANDLER(AST_IMPLICIT_STATEMENT,            build_scope_implicit_stmt,         kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_IMPORT_STATEMENT,              build_scope_import_stmt,           kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_INTENT_STATEMENT,              build_scope_intent_stmt,           kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_INTERFACE_BLOCK,               build_scope_interface_block,       kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_INTRINSIC_STATEMENT,           build_scope_intrinsic_stmt,        kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_LOCK_STATEMENT,                build_scope_lock_stmt,             kind_executable_0    ) \
 STATEMENT_HANDLER(AST_NAMELIST_STATEMENT,            build_scope_namelist_stmt,         kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_NULLIFY_STATEMENT,             build_scope_nullify_stmt,          kind_executable_0    ) \
 STATEMENT_HANDLER(AST_OPEN_STATEMENT,                build_scope_open_stmt,             kind_executable_0    ) \
 STATEMENT_HANDLER(AST_OPTIONAL_STATEMENT,            build_scope_optional_stmt,         kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_PARAMETER_STATEMENT,           build_scope_parameter_stmt,        kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_CRAY_POINTER_STATEMENT,        build_scope_cray_pointer_stmt,     kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_POINTER_STATEMENT,             build_scope_pointer_stmt,          kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_PRINT_STATEMENT,               build_scope_print_stmt,            kind_executable_0    ) \
 STATEMENT_HANDLER(AST_PROCEDURE_DECL_STATEMENT,      build_scope_procedure_decl_stmt,   kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_PROTECTED_STATEMENT,           build_scope_protected_stmt,        kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_READ_STATEMENT,                build_scope_read_stmt,             kind_executable_0    ) \
 STATEMENT_HANDLER(AST_RETURN_STATEMENT,              build_scope_return_stmt,           kind_executable_0    ) \
 STATEMENT_HANDLER(AST_SAVE_STATEMENT,                build_scope_save_stmt,             kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_SELECT_TYPE_CONSTRUCT,         build_scope_select_type_construct, kind_executable_0    ) \
 STATEMENT_HANDLER(AST_STATEMENT_FUNCTION_STATEMENT,  build_scope_stmt_function_stmt,    kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_STOP_STATEMENT,                build_scope_stop_stmt,             kind_executable_0    ) \
 STATEMENT_HANDLER(AST_PAUSE_STATEMENT,               build_scope_pause_stmt,            kind_executable_0    ) \
 STATEMENT_HANDLER(AST_SYNC_ALL_STATEMENT,            build_scope_sync_all_stmt,         kind_executable_0    ) \
 STATEMENT_HANDLER(AST_SYNC_IMAGES_STATEMENT,         build_scope_sync_images_stmt,      kind_executable_0    ) \
 STATEMENT_HANDLER(AST_SYNC_MEMORY_STATEMENT,         build_scope_sync_memory_stmt,      kind_executable_0    ) \
 STATEMENT_HANDLER(AST_TARGET_STATEMENT,              build_scope_target_stmt,           kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_DECLARATION_STATEMENT,         build_scope_declaration_stmt,      kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_UNLOCK_STATEMENT,              build_scope_unlock_stmt,           kind_executable_0    ) \
 STATEMENT_HANDLER(AST_USE_STATEMENT,                 build_scope_use_stmt,              kind_nonexecutable_0    ) \
 STATEMENT_HANDLER(AST_USE_ONLY_STATEMENT,            build_scope_use_stmt,              kind_executable_0    ) \
 STATEMENT_HANDLER(AST_VALUE_STATEMENT,               build_scope_value_stmt,            kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_VOLATILE_STATEMENT,            build_scope_volatile_stmt,         kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_WAIT_STATEMENT,                build_scope_wait_stmt,             kind_executable_0    ) \
 STATEMENT_HANDLER(AST_WHERE_CONSTRUCT,               build_scope_where_construct,       kind_executable_0    ) \
 STATEMENT_HANDLER(AST_WHERE_STATEMENT,               build_scope_where_stmt,            kind_executable_0    ) \
 STATEMENT_HANDLER(AST_WHILE_STATEMENT,               build_scope_while_stmt,            kind_executable_0    ) \
 STATEMENT_HANDLER(AST_WRITE_STATEMENT,               build_scope_write_stmt,            kind_executable_0    ) \
 STATEMENT_HANDLER(AST_PRAGMA_CUSTOM_CONSTRUCT,       build_scope_pragma_custom_ctr,     kind_executable_0  ) \
 STATEMENT_HANDLER(AST_PRAGMA_CUSTOM_DIRECTIVE,       build_scope_pragma_custom_dir,     kind_nonexecutable_0 ) \
 STATEMENT_HANDLER(AST_UNKNOWN_PRAGMA,                build_scope_unknown_pragma,        kind_nonexecutable_0  ) \
 STATEMENT_HANDLER(AST_STATEMENT_PLACEHOLDER,         build_scope_statement_placeholder, kind_nonexecutable_0  ) \
 STATEMENT_HANDLER(AST_ENTRY_STATEMENT,               build_scope_entry_stmt,            kind_executable_0 ) \
 STATEMENT_HANDLER(AST_TYPEDEF_DECLARATION_STATEMENT, build_scope_typedef_stmt,          kind_nonexecutable_0 ) \

// Prototypes
#define STATEMENT_HANDLER(_kind, _handler, _) \
    static void _handler(AST, decl_context_t, nodecl_t*);
STATEMENT_HANDLER_TABLE
#undef STATEMENT_HANDLER

// Table
#define STATEMENT_HANDLER(_kind, _handler, _stmt_kind) \
   { .ast_kind = _kind, .handler = _handler, .statement_kind = _stmt_kind },
static build_scope_statement_handler_t build_scope_statement_function[] = 
{
  STATEMENT_HANDLER_TABLE
};
#undef STATEMENT_HANDLER

static int build_scope_statement_function_init = 0;

static int build_scope_statement_function_compare(const void *a, const void *b)
{
    build_scope_statement_handler_t *pa = (build_scope_statement_handler_t*)a;
    build_scope_statement_handler_t *pb = (build_scope_statement_handler_t*)b;

    if (pa->ast_kind < pb->ast_kind)
        return -1;
    else if (pa->ast_kind > pb->ast_kind)
        return 1;
    else
        return 0;
}


static void init_statement_array(void)
{
    // Sort the array if needed
    if (!build_scope_statement_function_init)
    {
        // void qsort(void *base, size_t nmemb, size_t size,
        //    int(*compar)(const void *, const void *));
        qsort(build_scope_statement_function, 
                sizeof(build_scope_statement_function) / sizeof(build_scope_statement_function[0]),
                sizeof(build_scope_statement_function[0]),
                build_scope_statement_function_compare);
        build_scope_statement_function_init = 1;
    }
}

static char statement_get_kind(AST statement)
{
    init_statement_array();

    build_scope_statement_handler_t key = { .ast_kind = ASTType(statement) };
    build_scope_statement_handler_t *handler = NULL;

    handler = (build_scope_statement_handler_t*)bsearch(&key, build_scope_statement_function, 
            sizeof(build_scope_statement_function) / sizeof(build_scope_statement_function[0]),
            sizeof(build_scope_statement_function[0]),
            build_scope_statement_function_compare);

    ERROR_CONDITION(handler == NULL 
            || handler->statement_kind == NULL, "Invalid tree kind %s", ast_print_node_type(ASTType(statement)));

    return (handler->statement_kind)(statement);
}

static statement_kind_t kind_of_son_1(AST a)
{
    return statement_get_kind(ASTSon1(a));
}

static char statement_is_executable(AST statement)
{
    return statement_get_kind(statement) == STMT_KIND_EXECUTABLE;
}

#if 0
static char statement_is_nonexecutable(AST statement)
{
    return statement_get_kind(statement) == STMT_KIND_NONEXECUTABLE;
}
#endif

void fortran_build_scope_statement(AST statement, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    init_statement_array();

    DEBUG_CODE()
    {
        fprintf(stderr, "=== [%s] Statement ===\n", ast_location(statement));
    }

    build_scope_statement_handler_t key = { .ast_kind = ASTType(statement) };
    build_scope_statement_handler_t *handler = NULL;

    handler = (build_scope_statement_handler_t*)bsearch(&key, build_scope_statement_function, 
            sizeof(build_scope_statement_function) / sizeof(build_scope_statement_function[0]),
            sizeof(build_scope_statement_function[0]),
            build_scope_statement_function_compare);
    if (handler == NULL 
            || handler->handler == NULL)
    {
        running_error("%s: sorry: unhandled statement %s\n", ast_location(statement), ast_print_node_type(ASTType(statement)));
    }
    else
    {
        (handler->handler)(statement, decl_context, nodecl_output);
    }
}

const char* get_name_of_generic_spec(AST generic_spec)
{
    switch (ASTType(generic_spec))
    {
        case AST_SYMBOL:
            {
                return strtolower(ASTText(generic_spec));
            }
        case AST_OPERATOR_NAME:
            {
                return strtolower(strappend(".operator.", ASTText(generic_spec)));
            }
        case AST_IO_SPEC:
            {
                running_error("%s: sorry: io-specifiers for generic-specifiers not supported\n", 0);
            }
        default:
            {
                internal_error("%s: Invalid generic spec '%s'", 
                        ast_location(generic_spec), ast_print_node_type(ASTType(generic_spec)));
            }
    }
    return NULL;
}


static int compute_kind_specifier(AST kind_expr, decl_context_t decl_context,
        int (*default_kind)(void),
        nodecl_t* nodecl_output)
{
    fortran_check_expression(kind_expr, decl_context, nodecl_output);

    if (nodecl_is_constant(*nodecl_output))
    {
        return const_value_cast_to_4(nodecl_get_constant(*nodecl_output));
    }
    else
    {
        int result = default_kind();
        warn_printf("%s: could not compute KIND specifier, assuming %d\n", 
                ast_location(kind_expr), result);
        return result;
    }
}

static type_t* choose_type_from_kind_table(nodecl_t expr, type_t** type_table, int num_types, int kind_size)
{
    type_t* result = NULL;
    if ((0 < kind_size)
            && (kind_size <= num_types))
    {
        result = type_table[kind_size];
    }

    if (result == NULL)
    {
        error_printf("%s: error: KIND=%d not supported\n", nodecl_get_locus(expr), kind_size);
        result = (type_table[4] != NULL ? type_table[4] : type_table[1]);
        ERROR_CONDITION(result == NULL, "Fallback kind should not be NULL", 0);
    }

    return result;
}

#define MAX_INT_KIND MCXX_MAX_BYTES_INTEGER
static char int_types_init = 0;
static type_t* int_types[MAX_INT_KIND + 1] = { 0 };
type_t* choose_int_type_from_kind(nodecl_t expr, int kind_size)
{
    if (!int_types_init)
    {
        int_types[type_get_size(get_signed_long_long_int_type())] = get_signed_long_long_int_type();
        int_types[type_get_size(get_signed_long_int_type())] = get_signed_long_int_type();
        int_types[type_get_size(get_signed_int_type())] = get_signed_int_type();
        int_types[type_get_size(get_signed_short_int_type())] = get_signed_short_int_type();
        int_types[type_get_size(get_signed_byte_type())] = get_signed_byte_type();
        int_types_init = 1;
    }
    return choose_type_from_kind_table(expr, int_types, MAX_INT_KIND, kind_size);
}
#undef MAX_INT_KIND

#define MAX_FLOAT_KIND MCXX_MAX_BYTES_INTEGER
static char float_types_init = 0;
static type_t* float_types[MAX_FLOAT_KIND + 1] = { 0 };
type_t* choose_float_type_from_kind(nodecl_t expr, int kind_size)
{
    if (!float_types_init)
    {
        int i;
        for (i = 0; i < CURRENT_CONFIGURATION->type_environment->num_float_types; i++)
        {
            float_types[CURRENT_CONFIGURATION->type_environment->all_floats[i]->bits / 8] 
                = get_floating_type_from_descriptor(CURRENT_CONFIGURATION->type_environment->all_floats[i]);
        }
        float_types_init = 1;
    }
    return choose_type_from_kind_table(expr, float_types, MAX_FLOAT_KIND, kind_size);
}
#undef MAX_FLOAT_KIND

#define MAX_LOGICAL_KIND MCXX_MAX_BYTES_INTEGER
static char logical_types_init = 0;
static type_t* logical_types[MAX_LOGICAL_KIND + 1] = { 0 };
type_t* choose_logical_type_from_kind(nodecl_t expr, int kind_size)
{
    if (!logical_types_init)
    {
        logical_types[type_get_size(get_signed_long_long_int_type())] = get_bool_of_integer_type(get_signed_long_long_int_type());
        logical_types[type_get_size(get_signed_long_int_type())] = get_bool_of_integer_type(get_signed_long_int_type());
        logical_types[type_get_size(get_signed_int_type())] = get_bool_of_integer_type(get_signed_int_type());
        logical_types[type_get_size(get_signed_short_int_type())] = get_bool_of_integer_type(get_signed_short_int_type());
        logical_types[type_get_size(get_signed_byte_type())] = get_bool_of_integer_type(get_signed_byte_type());
        logical_types_init = 1;
    }
    return choose_type_from_kind_table(expr, logical_types, MAX_LOGICAL_KIND, kind_size);
}
#undef MAX_LOGICAL_KIND

#define MAX_CHARACTER_KIND MCXX_MAX_BYTES_INTEGER
static char character_types_init = 0;
static type_t* character_types[MAX_CHARACTER_KIND + 1] = { 0 };
type_t* choose_character_type_from_kind(nodecl_t expr, int kind_size)
{
    if (!character_types_init)
    {
        character_types[type_get_size(get_char_type())] = get_char_type();
        character_types_init = 1;
    }
    return choose_type_from_kind_table(expr, character_types, MAX_CHARACTER_KIND, kind_size);
}
#undef MAX_CHARACTER_KIND

static type_t* choose_type_from_kind(AST expr, decl_context_t decl_context, type_t* (*fun)(nodecl_t expr, int kind_size),
        int (*default_kind)(void))
{
    nodecl_t nodecl_output = nodecl_null();
    int kind_size = compute_kind_specifier(expr, decl_context, default_kind, &nodecl_output);
    return fun(nodecl_output, kind_size);
}

static type_t* get_derived_type_name(AST a, decl_context_t decl_context)
{
    ERROR_CONDITION(ASTType(a) != AST_DERIVED_TYPE_NAME, "Invalid tree '%s'\n", ast_print_node_type(ASTType(a)));

    AST name = ASTSon0(a);
    if (ASTSon1(a) != NULL)
    {
        running_error("%s: sorry: unsupported generic type-names", ast_location(ASTSon1(a)));
    }

    type_t* result = NULL;

    scope_entry_t* entry = fortran_query_name_str(decl_context, strtolower(ASTText(name)));
    if (entry != NULL
            && entry->kind == SK_CLASS)
    {
        result = get_user_defined_type(entry);
    }

    return result;
}

static type_t* gather_type_from_declaration_type_spec_(AST a, 
        decl_context_t decl_context)
{
    type_t* result = NULL;
    switch (ASTType(a))
    {
        case AST_INT_TYPE:
            {
                result = get_signed_int_type();
                if (ASTSon0(a) != NULL)
                {
                    result = choose_type_from_kind(ASTSon0(a), decl_context, 
                            choose_int_type_from_kind, fortran_get_default_integer_type_kind);
                }
                break;
            }
        case AST_FLOAT_TYPE:
            {
                result = get_float_type();
                if (ASTSon0(a) != NULL)
                {
                    result = choose_type_from_kind(ASTSon0(a), decl_context, 
                            choose_float_type_from_kind, fortran_get_default_real_type_kind);
                }
                break;
            }
        case AST_DOUBLE_TYPE:
            {
                result = get_double_type();
                break;
            }
        case AST_COMPLEX_TYPE:
            {
                type_t* element_type = NULL; 
                if (ASTType(ASTSon0(a)) == AST_DECIMAL_LITERAL)
                {
                    element_type = choose_type_from_kind(ASTSon0(a), decl_context, 
                            choose_float_type_from_kind, fortran_get_default_real_type_kind);
                }
                else
                {
                    element_type = gather_type_from_declaration_type_spec_(ASTSon0(a), decl_context);
                }

                result = get_complex_type(element_type);
                break;
            }
        case AST_CHARACTER_TYPE:
            {
                result = fortran_get_default_character_type();
                AST char_selector = ASTSon0(a);
                AST len = NULL;
                AST kind = NULL;
                if (char_selector != NULL)
                {
                    len = ASTSon0(char_selector);
                    kind = ASTSon1(char_selector);
                }

                char is_undefined = 0;
                if (kind != NULL)
                {
                    result = choose_type_from_kind(kind, decl_context, 
                            choose_character_type_from_kind, 
                            fortran_get_default_character_type_kind);
                }

                nodecl_t nodecl_len = nodecl_null();
                if (len != NULL
                        && ASTType(len) == AST_SYMBOL
                        && strcmp(ASTText(len), "*") == 0)
                {
                    is_undefined = 1;
                }
                else 
                {
                    if (len == NULL)
                    {
                        nodecl_len = const_value_to_nodecl(const_value_get_one(fortran_get_default_integer_type_kind(), 1));
                    }
                    else
                    {
                        fortran_check_expression(len, decl_context, &nodecl_len);
                    }
                }

                if (!is_undefined)
                {
                    nodecl_t lower_bound = nodecl_make_integer_literal(
                            get_signed_int_type(),
                            const_value_get_one(type_get_size(get_signed_int_type()), 1),
                            nodecl_get_filename(nodecl_len), nodecl_get_line(nodecl_len));
                    result = get_array_type_bounds(result, lower_bound, nodecl_len, decl_context);
                }
                else
                {
                    result = get_array_type(result, nodecl_null(), decl_context);
                }
                break;
            }
        case AST_BOOL_TYPE:
            {
                result = get_bool_of_integer_type(get_signed_int_type());
                if (ASTSon0(a) != NULL)
                {
                    result = choose_type_from_kind(ASTSon0(a), decl_context, 
                            choose_logical_type_from_kind, fortran_get_default_logical_type_kind);
                }
                break;
            }
        case AST_TYPE_NAME:
            {
                result = get_derived_type_name(ASTSon0(a), decl_context);
                if (result == NULL)
                {
                    error_printf("%s: error: invalid type-specifier '%s'\n",
                            ast_location(a),
                            fortran_prettyprint_in_buffer(a));
                    result = get_error_type();
                }
                break;
            }
        case AST_VECTOR_TYPE:
            {
                type_t* element_type = gather_type_from_declaration_type_spec_(ASTSon0(a), decl_context);
                // Generic vector
                result = get_vector_type(element_type, 0);
                break;
            }
        case AST_PIXEL_TYPE:
            {
                running_error("%s: sorry: PIXEL type-specifier not implemented\n",
                        ast_location(a));
                break;
            }
        case AST_CLASS_NAME:
            {
                running_error("%s: sorry: CLASS type-specifier not implemented\n",
                        ast_location(a));
                break;
            }
        default:
            {
                internal_error("Unexpected node '%s'\n", ast_print_node_type(ASTType(a)));
            }
    }

    return result;
}



typedef
struct attr_spec_tag
{
    char is_allocatable;
    char is_asynchronous;

    char is_codimension;
    AST coarray_spec;

    char is_contiguous;

    char is_dimension;
    AST array_spec;

    char is_external;

    char is_intent;
    intent_kind_t intent_kind;

    char is_intrinsic;

    char is_optional;

    char is_constant;

    char is_pointer;
    
    char is_protected;

    char is_save;

    char is_target;

    char is_value;

    char is_volatile;

    char is_public;

    char is_private;

    char is_c_binding;
    const char* c_binding_name;
} attr_spec_t;

#define ATTR_SPEC_HANDLER_LIST \
ATTR_SPEC_HANDLER(allocatable) \
ATTR_SPEC_HANDLER(asynchronous) \
ATTR_SPEC_HANDLER(codimension) \
ATTR_SPEC_HANDLER(contiguous) \
ATTR_SPEC_HANDLER(dimension) \
ATTR_SPEC_HANDLER(external) \
ATTR_SPEC_HANDLER(intent) \
ATTR_SPEC_HANDLER(intrinsic) \
ATTR_SPEC_HANDLER(optional) \
ATTR_SPEC_HANDLER(parameter) \
ATTR_SPEC_HANDLER(pointer) \
ATTR_SPEC_HANDLER(protected) \
ATTR_SPEC_HANDLER(save) \
ATTR_SPEC_HANDLER(target) \
ATTR_SPEC_HANDLER(value) \
ATTR_SPEC_HANDLER(public) \
ATTR_SPEC_HANDLER(private) \
ATTR_SPEC_HANDLER(volatile) \
ATTR_SPEC_HANDLER(bind) 

// Forward declarations
#define ATTR_SPEC_HANDLER(_name) \
    static void attr_spec_##_name##_handler(AST attr_spec_item, decl_context_t decl_context, attr_spec_t* attr_spec);
ATTR_SPEC_HANDLER_LIST
#undef ATTR_SPEC_HANDLER

typedef struct attr_spec_handler_tag {
    const char* attr_name;
    void (*handler)(AST attr_spec_item, decl_context_t decl_context, attr_spec_t* attr_spec);
} attr_spec_handler_t;

// Table of handlers
attr_spec_handler_t attr_spec_handler_table[] = {
#define ATTR_SPEC_HANDLER(_name) \
    { #_name , attr_spec_##_name##_handler },
ATTR_SPEC_HANDLER_LIST
#undef ATTR_SPEC_HANDLER
};

static int attr_handler_cmp(const void *a, const void *b)
{
    return strcasecmp(((attr_spec_handler_t*)a)->attr_name,
            ((attr_spec_handler_t*)b)->attr_name);
}

static char attr_spec_handler_table_init = 0;
 
static void gather_attr_spec_item(AST attr_spec_item, decl_context_t decl_context, attr_spec_t *attr_spec)
{
    if (!attr_spec_handler_table_init)
    {
        qsort(attr_spec_handler_table, 
                sizeof(attr_spec_handler_table) / sizeof(attr_spec_handler_table[0]),
                sizeof(attr_spec_handler_table[0]),
                attr_handler_cmp);
        attr_spec_handler_table_init = 1;
    }

    switch (ASTType(attr_spec_item))
    {
        case AST_ATTR_SPEC:
            {
                attr_spec_handler_t key = { .attr_name = ASTText(attr_spec_item) };

                attr_spec_handler_t* handler = (attr_spec_handler_t*)bsearch(
                        &key,
                        attr_spec_handler_table, 
                        sizeof(attr_spec_handler_table) / sizeof(attr_spec_handler_table[0]),
                        sizeof(attr_spec_handler_table[0]),
                        attr_handler_cmp);

                if (handler == NULL 
                        || handler->handler == NULL)
                {
                    internal_error("Unhandled handler of '%s' (%s)\n", ASTText(attr_spec_item), ast_print_node_type(ASTType(attr_spec_item)));
                }

                (handler->handler)(attr_spec_item, decl_context, attr_spec);
                break;
            }
        default:
            {
                internal_error("Unhandled tree '%s'\n", ast_print_node_type(ASTType(attr_spec_item)));
            }
    }
}

static void attr_spec_allocatable_handler(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, attr_spec_t* attr_spec)
{
    attr_spec->is_allocatable = 1;
}

static void attr_spec_asynchronous_handler(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        attr_spec_t* attr_spec)
{
    attr_spec->is_asynchronous = 1;
}

static void attr_spec_codimension_handler(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        attr_spec_t* attr_spec)
{
    attr_spec->is_codimension = 1;
    attr_spec->coarray_spec = ASTSon0(a);
}

static void attr_spec_contiguous_handler(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER,  
        attr_spec_t* attr_spec)
{
    attr_spec->is_contiguous = 1;
}

static void attr_spec_dimension_handler(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        attr_spec_t* attr_spec)
{
    attr_spec->is_dimension = 1;
    attr_spec->array_spec = ASTSon0(a);
}

static void attr_spec_external_handler(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        attr_spec_t* attr_spec)
{
    attr_spec->is_external = 1;
}

static void attr_spec_intent_handler(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        attr_spec_t* attr_spec)
{
    attr_spec->is_intent = 1;

    const char* intent_kind_str = ASTText(ASTSon0(a));
    if (strcasecmp(intent_kind_str, "in") == 0)
    {
        attr_spec->intent_kind = INTENT_IN;
    }
    else if (strcasecmp(intent_kind_str, "out") == 0)
    {
        attr_spec->intent_kind = INTENT_OUT;
    }
    else if (strcasecmp(intent_kind_str, "inout") == 0)
    {
        attr_spec->intent_kind = INTENT_INOUT;
    }
    else
    {
        internal_error("Invalid intent kind '%s'\n", intent_kind_str);
    }
}

static void attr_spec_intrinsic_handler(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        attr_spec_t* attr_spec)
{
    attr_spec->is_intrinsic = 1;
}

static void attr_spec_optional_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_optional = 1;
}

static void attr_spec_parameter_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_constant = 1;
}

static void attr_spec_pointer_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_pointer = 1;
}

static void attr_spec_protected_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_protected = 1;
}

static void attr_spec_save_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_save = 1;
}

static void attr_spec_target_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_target = 1;
}

static void attr_spec_value_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_value = 1;
}

static void attr_spec_volatile_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_volatile = 1;
}

static void attr_spec_public_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_public = 1;
}

static void attr_spec_private_handler(AST a UNUSED_PARAMETER,
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_private = 1;
}

static void attr_spec_bind_handler(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER,
        attr_spec_t* attr_spec)
{
    attr_spec->is_c_binding = 1;
    if (ASTSon0(a) != NULL)
    {
        attr_spec->c_binding_name = ASTText(ASTSon0(a));
    }
}

static void gather_attr_spec_list(AST attr_spec_list, decl_context_t decl_context, attr_spec_t *attr_spec)
{
    AST it;
    for_each_element(attr_spec_list, it)
    {
        AST attr_spec_item = ASTSon1(it);

        gather_attr_spec_item(attr_spec_item, decl_context, attr_spec);
    }
}

typedef
enum array_spec_kind_tag
{
    ARRAY_SPEC_KIND_NONE = 0,
    ARRAY_SPEC_KIND_EXPLICIT_SHAPE,
    ARRAY_SPEC_KIND_ASSUMED_SHAPE,
    ARRAY_SPEC_KIND_DEFERRED_SHAPE,
    ARRAY_SPEC_KIND_ASSUMED_SIZE,
    ARRAY_SPEC_KIND_IMPLIED_SHAPE,
    ARRAY_SPEC_KIND_ERROR,
} array_spec_kind_t;

static type_t* compute_type_from_array_spec(type_t* basic_type, 
        AST array_spec_list, decl_context_t decl_context,
        array_spec_kind_t* array_spec_kind)
{
    char was_ref = is_lvalue_reference_type(basic_type);

    // explicit-shape-spec   is   [lower:]upper
    // assumed-shape-spec    is   [lower]:
    // deferred-shape-spec   is   :
    // implied-shape-spec    is   [lower:]*

    // As a special case an assumed-size array-spec is [explicit-shape-spec ... ,] [lower:]*

    array_spec_kind_t kind = ARRAY_SPEC_KIND_NONE;

    nodecl_t lower_bound_seq[MCXX_MAX_ARRAY_SPECIFIER];
    memset(lower_bound_seq, 0, sizeof(lower_bound_seq));
    nodecl_t upper_bound_seq[MCXX_MAX_ARRAY_SPECIFIER];
    memset(upper_bound_seq, 0, sizeof(upper_bound_seq));

    int i = 0;

    AST it = NULL;
    for_each_element(array_spec_list, it)
    {
        ERROR_CONDITION(i == MCXX_MAX_ARRAY_SPECIFIER, "Too many array specifiers", 0);

        AST array_spec_item = ASTSon1(it);
        AST lower_bound_tree = ASTSon0(array_spec_item);
        AST upper_bound_tree = ASTSon1(array_spec_item);

        nodecl_t lower_bound = nodecl_null();
        nodecl_t upper_bound = nodecl_null();

        if (lower_bound_tree != NULL
                && (ASTType(lower_bound_tree) != AST_SYMBOL
                    || (strcmp(ASTText(lower_bound_tree), "*") != 0) ))
        {
            fortran_check_expression(lower_bound_tree, decl_context, &lower_bound);
        }

        if (upper_bound_tree != NULL
                && (ASTType(upper_bound_tree) != AST_SYMBOL
                    || (strcmp(ASTText(upper_bound_tree), "*") != 0) ))
        {
            fortran_check_expression(upper_bound_tree, decl_context, &upper_bound);
        }

        if (lower_bound_tree == NULL
                && upper_bound_tree == NULL)
        {
            // (:)
            if (kind == ARRAY_SPEC_KIND_NONE)
            {
                kind = ARRAY_SPEC_KIND_DEFERRED_SHAPE;
            }
            else if (kind != ARRAY_SPEC_KIND_DEFERRED_SHAPE
                    && kind != ARRAY_SPEC_KIND_ASSUMED_SHAPE
                    && kind != ARRAY_SPEC_KIND_ERROR)
            {
                kind = ARRAY_SPEC_KIND_ERROR;
            }
        }
        else if (upper_bound_tree != NULL
                && ASTType(upper_bound_tree) == AST_SYMBOL
                && strcmp(ASTText(upper_bound_tree), "*") == 0)
        {
            // (*)
            // (L:*)
            if (kind == ARRAY_SPEC_KIND_NONE)
            {
                kind = ARRAY_SPEC_KIND_IMPLIED_SHAPE;
            }
            else if (kind == ARRAY_SPEC_KIND_EXPLICIT_SHAPE)
            {
                kind = ARRAY_SPEC_KIND_ASSUMED_SIZE;
            }
            else if (kind != ARRAY_SPEC_KIND_ASSUMED_SIZE
                    && kind != ARRAY_SPEC_KIND_IMPLIED_SHAPE
                    && kind != ARRAY_SPEC_KIND_ERROR)
            {
                kind = ARRAY_SPEC_KIND_ERROR;
            }
        }
        else if (lower_bound_tree != NULL
                && upper_bound_tree == NULL)
        {
            // (L:)
            if (kind == ARRAY_SPEC_KIND_NONE
                    || kind == ARRAY_SPEC_KIND_DEFERRED_SHAPE)
            {
                kind = ARRAY_SPEC_KIND_ASSUMED_SHAPE;
            }
            else if (kind != ARRAY_SPEC_KIND_ASSUMED_SHAPE
                    && kind != ARRAY_SPEC_KIND_ERROR)
            {
                kind = ARRAY_SPEC_KIND_ERROR;
            }
        }
        else if (upper_bound_tree != NULL)
        {
            // (U)
            // (:U)
            // (L:U)
            if (kind == ARRAY_SPEC_KIND_NONE)
            {
                kind = ARRAY_SPEC_KIND_EXPLICIT_SHAPE;
            }
            else if (kind != ARRAY_SPEC_KIND_EXPLICIT_SHAPE
                    && kind != ARRAY_SPEC_KIND_ERROR)
            {
                kind = ARRAY_SPEC_KIND_ERROR;
            }

            if (lower_bound_tree == NULL)
            {
                lower_bound = nodecl_make_integer_literal(
                        get_signed_int_type(),
                        const_value_get_one(type_get_size(get_signed_int_type()), 1),
                        ASTFileName(upper_bound_tree), ASTLine(upper_bound_tree));
            }
        }

        if (kind == ARRAY_SPEC_KIND_ERROR)
            break;

        lower_bound_seq[i] = lower_bound;
        upper_bound_seq[i] = upper_bound;

        i++;
    }

    type_t* array_type = no_ref(basic_type);

    if (kind != ARRAY_SPEC_KIND_ERROR)
    {
        // All dimensions will have the attribute needs descriptor set to 0 or 1
        char needs_descriptor = ((kind == ARRAY_SPEC_KIND_ASSUMED_SHAPE)
                || (kind == ARRAY_SPEC_KIND_DEFERRED_SHAPE));

        // const char* array_spec_kind_name[] = 
        // {
        //     [ARRAY_SPEC_KIND_NONE] = "ARRAY_SPEC_KIND_NONE",
        //     [ARRAY_SPEC_KIND_EXPLICIT_SHAPE] = "ARRAY_SPEC_KIND_EXPLICIT_SHAPE",
        //     [ARRAY_SPEC_KIND_ASSUMED_SHAPE] = "ARRAY_SPEC_KIND_ASSUMED_SHAPE",
        //     [ARRAY_SPEC_KIND_DEFERRED_SHAPE] = "ARRAY_SPEC_KIND_DEFERRED_SHAPE",
        //     [ARRAY_SPEC_KIND_ASSUMED_SIZE] = "ARRAY_SPEC_KIND_ASSUMED_SIZE",
        //     [ARRAY_SPEC_KIND_IMPLIED_SHAPE] = "ARRAY_SPEC_KIND_IMPLIED_SHAPE",
        //     [ARRAY_SPEC_KIND_ERROR] = "ARRAY_SPEC_KIND_ERROR",
        // };

        // fprintf(stderr, "KIND OF ARRAY SPEC -> '%s' || needs_descr = %d\n", 
        //         array_spec_kind_name[kind],
        //         needs_descriptor);

        int j;
        for (j = 0; j < i; j++)
        {
            if (needs_descriptor)
            {
                array_type = get_array_type_bounds_with_descriptor(array_type, lower_bound_seq[j], upper_bound_seq[j], decl_context);
            }
            else
            {
                array_type = get_array_type_bounds(array_type, lower_bound_seq[j], upper_bound_seq[j], decl_context);
            }
        }
    }
    else
    {
        array_type = get_error_type();
    }

    if (array_spec_kind != NULL)
    {
        *array_spec_kind = kind;
    }

    if (was_ref
            && !is_error_type(array_type))
    {
        array_type = get_lvalue_reference_type(array_type);
    }

    return array_type;
}

static void build_scope_access_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    attr_spec_t attr_spec;
    memset(&attr_spec, 0, sizeof(attr_spec));

    AST access_spec = ASTSon0(a);

    gather_attr_spec_item(access_spec, decl_context, &attr_spec);

    AST access_id_list = ASTSon1(a);
    if (access_id_list != NULL)
    {
        AST it;
        for_each_element(access_id_list, it)
        {
            AST access_id = ASTSon1(it);

            const char* name = get_name_of_generic_spec(access_id);

            scope_entry_t* sym = get_symbol_for_name(decl_context, access_id, name);

            if (sym->entity_specs.access != AS_UNKNOWN)
            {
                error_printf("%s: access specifier already given for entity '%s'\n",
                        ast_location(access_id),
                        sym->symbol_name);
            }
            else
            {
                if (attr_spec.is_public)
                {
                    sym->entity_specs.access = AS_PUBLIC;
                }
                else if (attr_spec.is_private)
                {
                    sym->entity_specs.access = AS_PRIVATE;
                }
                else
                {
                    internal_error("Code unreachable", 0);
                }
            }
        }
    }
    else
    {
        scope_entry_t* current_sym = decl_context.current_scope->related_entry;

        if (current_sym == NULL
                || current_sym->kind != SK_MODULE)
        {
            error_printf("%s: error: wrong usage of access-statement\n",
                    ast_location(a));
        }
        else
        {
            if (current_sym->entity_specs.access != AS_UNKNOWN)
            {
                error_printf("%s: error: module '%s' already given a default access\n", 
                        ast_location(a),
                        current_sym->symbol_name);
            }
            if (attr_spec.is_public)
            {
                current_sym->entity_specs.access = AS_PUBLIC;
            }
            else if (attr_spec.is_private)
            {
                current_sym->entity_specs.access = AS_PRIVATE;
            }
            else
            {
                internal_error("Code unreachable", 0);
            }
        }
    }
}

static void build_dimension_decl(AST a, decl_context_t decl_context)
{
    // Do nothing for plain symbols
    if (ASTType(a) == AST_SYMBOL)
        return;

    ERROR_CONDITION(ASTType(a) != AST_DIMENSION_DECL,
            "Invalid tree", 0);

    AST name = ASTSon0(a);
    AST array_spec = ASTSon1(a);
    AST coarray_spec = ASTSon2(a);

    if (coarray_spec != NULL)
    {
       running_error("%s: sorry: coarrays not supported", ast_location(a));
    }

    scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

    char was_ref = is_lvalue_reference_type(entry->type_information);
    
    if(entry->kind == SK_UNDEFINED)
    {
        entry->kind = SK_VARIABLE;
        remove_unknown_kind_symbol(decl_context, entry);
    }

    if (entry->kind != SK_VARIABLE)
    {
        error_printf("%s: error: invalid entity '%s' in dimension declaration\n", 
                ast_location(a),
                ASTText(name));
        return;
    }
    
    if (is_fortran_array_type(no_ref(entry->type_information))
            || is_pointer_to_fortran_array_type(no_ref(entry->type_information)))
    {
        error_printf("%s: error: entity '%s' already has a DIMENSION attribute\n",
                ast_location(a),
                entry->symbol_name);
        return;
    }

    type_t* array_type = compute_type_from_array_spec(no_ref(entry->type_information), 
            array_spec,
            decl_context,
            /* array_spec_kind */ NULL);
    entry->type_information = array_type;

    if (was_ref)
    {
        entry->type_information = get_lvalue_reference_type(entry->type_information);
    }

}

static void build_scope_allocatable_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST allocatable_decl_list = ASTSon0(a);
    AST it;

    for_each_element(allocatable_decl_list, it)
    {
        AST allocatable_decl = ASTSon1(it);
        build_dimension_decl(allocatable_decl, decl_context);

        AST name = NULL;
        if (ASTType(allocatable_decl) == AST_SYMBOL)
        {
            name = allocatable_decl;
        }
        else if (ASTType(allocatable_decl))
        {
            name = ASTSon0(allocatable_decl);
        }

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        if (entry->kind != SK_VARIABLE)
        {
            error_printf("%s: error: invalid entity '%s' in ALLOCATABLE clause\n", 
                    ast_location(name), 
                    ASTText(name));
            continue;
        }

        if (!is_fortran_array_type(no_ref(entry->type_information))
                && !is_pointer_to_fortran_array_type(no_ref(entry->type_information)))
        {
            error_printf("%s: error: ALLOCATABLE attribute cannot be set to scalar entity '%s'\n",
                    ast_location(name),
                    ASTText(name));
            continue;
        }

        if (entry->entity_specs.is_allocatable)
        {
            error_printf("%s: error: attribute ALLOCATABLE was already set for entity '%s'\n",
                    ast_location(name),
                    ASTText(name));
            continue;
        }
        entry->entity_specs.is_allocatable = 1;
    }

}

static void build_scope_allocate_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST type_spec = ASTSon0(a);
    AST allocation_list = ASTSon1(a);
    AST alloc_opt_list = ASTSon2(a);

    if (type_spec != NULL)
    {
        running_error("%s: sorry: type-specifier not supported in ALLOCATE statement\n",
                ast_location(a));
    }

    nodecl_t nodecl_allocate_list = nodecl_null();

    AST it;
    for_each_element(allocation_list, it)
    {
        AST allocate_object = ASTSon1(it);

        // This one is here only for coarrays
        if (ASTType(allocate_object) == AST_DIMENSION_DECL)
        {
            running_error("%s: sorry: coarrays not supported\n", 
                    ast_location(allocate_object));
        }

        AST data_ref = allocate_object;
        nodecl_t nodecl_data_ref;
        fortran_check_expression(data_ref, decl_context, &nodecl_data_ref);

        if (!nodecl_is_err_expr(nodecl_data_ref))
        {
            scope_entry_t* entry = nodecl_get_symbol(nodecl_data_ref);

            if (!entry->entity_specs.is_allocatable
                    && !is_pointer_type(no_ref(entry->type_information)))
            {
                error_printf("%s: error: entity '%s' does not have ALLOCATABLE or POINTER attribute\n", 
                        ast_location(a),
                        entry->symbol_name);
                continue;
            }
        }

        nodecl_allocate_list = nodecl_append_to_list(nodecl_allocate_list, nodecl_data_ref);
    }

    nodecl_t nodecl_opt_value = nodecl_null();
    handle_opt_value_list(a, alloc_opt_list, decl_context, &nodecl_opt_value);

    *nodecl_output = nodecl_make_fortran_allocate_statement(nodecl_allocate_list, 
                nodecl_opt_value,
                ASTFileName(a), ASTLine(a));
}

static void unsupported_statement(AST a, const char* name)
{
    running_error("%s: sorry: %s statement not supported\n", 
            ast_location(a),
            name);
}

static void unsupported_construct(AST a, const char* name)
{
    running_error("%s: sorry: %s construct not supported\n", 
            ast_location(a),
            name);
}

static void build_scope_allstop_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "ALLSTOP");
}

static void build_scope_arithmetic_if_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST numeric_expr = ASTSon0(a);

    AST label_set = ASTSon1(a);
    AST lower = ASTSon0(label_set);
    AST equal = ASTSon1(label_set);
    AST upper = ASTSon2(label_set);

    // warn_printf("%s: warning: deprecated arithmetic-if statement\n", 
    //         ast_location(a));
    nodecl_t nodecl_numeric_expr = nodecl_null();
    fortran_check_expression(numeric_expr, decl_context, &nodecl_numeric_expr);

    scope_entry_t* lower_label = fortran_query_label(lower, decl_context, /* is_definition */ 0);
    scope_entry_t* equal_label = fortran_query_label(equal, decl_context, /* is_definition */ 0);
    scope_entry_t* upper_label = fortran_query_label(upper, decl_context, /* is_definition */ 0);

    *nodecl_output = nodecl_make_fortran_arithmetic_if_statement(
                nodecl_numeric_expr,
                nodecl_make_symbol(lower_label, ASTFileName(lower), ASTLine(lower)),
                nodecl_make_symbol(equal_label, ASTFileName(equal), ASTLine(equal)),
                nodecl_make_symbol(upper_label, ASTFileName(upper), ASTLine(upper)),
                ASTFileName(a), ASTLine(a));

}

static void build_scope_expression_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    DEBUG_CODE()
    {
        fprintf(stderr, "== [%s] Expression statement ==\n",
                ast_location(a));
    }
    AST expr = ASTSon0(a);
    nodecl_t nodecl_expr = nodecl_null();
    if (!fortran_check_expression(expr, decl_context, &nodecl_expr)
            && CURRENT_CONFIGURATION->strict_typecheck)
    {
        internal_error("Could not check expression '%s' at '%s'\n",
                fortran_prettyprint_in_buffer(ASTSon0(a)),
                ast_location(ASTSon0(a)));
    }

    if (!nodecl_is_err_expr(nodecl_expr))
    {
        *nodecl_output = nodecl_make_expression_statement(nodecl_expr,
                ASTFileName(expr),
                ASTLine(expr));
        nodecl_expr_set_is_lvalue(*nodecl_output, nodecl_expr_is_lvalue(nodecl_expr));
    }


}

static void build_scope_associate_construct(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "ASSOCIATE");
}

static void build_scope_asynchronous_stmt(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "ASYNCHRONOUS");
}

static void build_scope_input_output_item_list(AST input_output_item_list, decl_context_t decl_context, nodecl_t* nodecl_output);

static void build_io_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST io_spec_list = ASTSon0(a);
    nodecl_t nodecl_io_spec_list = nodecl_null();
    handle_opt_value_list(a, io_spec_list, decl_context, &nodecl_io_spec_list);

    AST input_output_item_list = ASTSon1(a);

    nodecl_t nodecl_io_items = nodecl_null();
    if (input_output_item_list != NULL)
    {
        build_scope_input_output_item_list(input_output_item_list, decl_context, &nodecl_io_items);
    }

   *nodecl_output = nodecl_make_fortran_io_statement(nodecl_io_spec_list, nodecl_io_items, ASTText(a), ASTFileName(a), ASTLine(a));
}

static const char* get_common_name_str(const char* common_name)
{
    const char *common_name_str = ".common._unnamed";
    if (common_name != NULL)
    {
        common_name_str = strappend(".common.", strtolower(common_name));
    }
    return common_name_str;
}

static scope_entry_t* query_common_name(decl_context_t decl_context, const char* common_name)
{
    scope_entry_t* result = fortran_query_name_str(decl_context, 
            get_common_name_str(common_name));

    return result;
}

static void build_scope_bind_stmt(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST language_binding_spec = ASTSon0(a);
    AST bind_entity_list = ASTSon1(a);

    if (ASTType(language_binding_spec) != AST_BIND_C_SPEC)
    {
        running_error("%s: error: unsupported binding '%s'\n", 
                fortran_prettyprint_in_buffer(language_binding_spec));
    }

    AST it;
    for_each_element(bind_entity_list, it)
    {
        AST bind_entity = ASTSon1(it);

        scope_entry_t* entry = NULL;
        if (ASTType(bind_entity) == AST_COMMON_NAME)
        {
            entry = query_common_name(decl_context, ASTText(ASTSon0(bind_entity)));
        }
        else
        {
            entry = get_symbol_for_name(decl_context, bind_entity, ASTText(bind_entity));
        }

        entry->entity_specs.bind_c = 1;

        if (entry == NULL)
        {
            error_printf("%s: error: unknown entity '%s' in BIND statement\n",
                    ast_location(bind_entity),
                    fortran_prettyprint_in_buffer(bind_entity));
            continue;
        }
    }

}

static void build_scope_block_construct(AST a, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    decl_context_t new_context = fortran_new_block_context(decl_context);

    AST block = ASTSon1(a);
    nodecl_t nodecl_body = nodecl_null();
    fortran_build_scope_statement(block, new_context, &nodecl_body);

    if (block != NULL)
    {
    }


    *nodecl_output = nodecl_make_compound_statement(nodecl_body, nodecl_null(),
            ASTFileName(a), ASTLine(a));
}

static void build_scope_case_construct(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST expr = ASTSon0(a);
    AST statement = ASTSon1(a);

    nodecl_t nodecl_expr = nodecl_null();
    fortran_check_expression(expr, decl_context, &nodecl_expr);

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(statement, decl_context, &nodecl_statement);


    *nodecl_output = nodecl_make_switch_statement(
            nodecl_expr,
            nodecl_statement,
            ASTFileName(a),
            ASTLine(a));
}

static void build_scope_case_statement(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST case_selector = ASTSon0(a);
    AST statement = ASTSon1(a);

    nodecl_t nodecl_expr_list = nodecl_null();
    AST case_value_range_list = ASTSon0(case_selector);
    AST it;
    for_each_element(case_value_range_list, it)
    {
        AST case_value_range = ASTSon1(it);

        if (ASTType(case_value_range) == AST_CASE_VALUE_RANGE)
        {
            AST lower_bound = ASTSon0(case_value_range);
            AST upper_bound = ASTSon1(case_value_range);

            nodecl_t nodecl_lower_bound = nodecl_null();
            nodecl_t nodecl_upper_bound = nodecl_null();

            if (lower_bound != NULL)
                fortran_check_expression(lower_bound, decl_context, &nodecl_lower_bound);
            if (upper_bound != NULL)
                fortran_check_expression(upper_bound, decl_context, &nodecl_upper_bound);

            nodecl_t nodecl_stride = const_value_to_nodecl(const_value_get_one(/* bytes */ fortran_get_default_integer_type_kind(), /* signed */ 1));

            nodecl_t nodecl_triplet = nodecl_make_range(
                    nodecl_lower_bound,
                    nodecl_upper_bound,
                    nodecl_stride,
                    fortran_get_default_integer_type(),
                    ASTFileName(case_value_range),
                    ASTLine(case_value_range));

            nodecl_expr_list = nodecl_append_to_list(nodecl_expr_list, nodecl_triplet);
        }
        else
        {
            nodecl_t nodecl_case_value_range = nodecl_null();
            fortran_check_expression(case_value_range, decl_context, &nodecl_case_value_range);
            nodecl_expr_list = nodecl_append_to_list(nodecl_expr_list, 
                    nodecl_case_value_range);
        }
    }

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(statement, decl_context, &nodecl_statement);

    if (!nodecl_is_list(nodecl_statement))
    {
        nodecl_statement = nodecl_make_list_1(nodecl_statement);
    }


    *nodecl_output = nodecl_make_case_statement(nodecl_expr_list, nodecl_statement, ASTFileName(a), ASTLine(a));
}

static void build_scope_default_statement(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST statement = ASTSon0(a);

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(statement, decl_context, &nodecl_statement);

    if (!nodecl_is_list((nodecl_statement)))
    {
        nodecl_statement = nodecl_make_list_1(nodecl_statement);
    }

    *nodecl_output = nodecl_make_default_statement(nodecl_statement, ASTFileName(a), ASTLine(a));
}

static void build_scope_compound_statement(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST it;

    AST list = ASTSon0(a);

    nodecl_t nodecl_list = nodecl_null();
    for_each_element(list, it)
    {
        AST statement = ASTSon1(it);

        nodecl_t nodecl_statement = nodecl_null();
        fortran_build_scope_statement(statement, decl_context, &nodecl_statement);
        nodecl_list = nodecl_append_to_list(nodecl_list, nodecl_statement);
    }


    *nodecl_output = nodecl_list;
}

static void build_scope_close_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST close_spec_list = ASTSon0(a);
    nodecl_t nodecl_opt_value = nodecl_null();
    handle_opt_value_list(a, close_spec_list, decl_context, &nodecl_opt_value);

    *nodecl_output = nodecl_make_fortran_close_statement(nodecl_opt_value, ASTFileName(a), ASTLine(a));
}

static void build_scope_codimension_stmt(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "CODIMENSION");
}

static scope_entry_t* new_common(decl_context_t decl_context, const char* common_name)
{
    scope_entry_t* common_sym = new_fortran_symbol(decl_context, get_common_name_str(common_name));
    common_sym->kind = SK_COMMON;
    remove_unknown_kind_symbol(decl_context, common_sym);
    return common_sym;
}

static void build_scope_common_stmt(AST a, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST common_block_item_list = ASTSon0(a);

    AST it;
    for_each_element(common_block_item_list, it)
    {
        AST common_block_item = ASTSon1(it);

        AST common_block_object_list = ASTSon1(common_block_item);

        const char* common_name_str = NULL;
        AST common_name = ASTSon0(common_block_item);
        if(common_name != NULL)
        {
            //It is a named common statement
            common_name_str = ASTText(common_name);
        }
       
        // Looking the symbol in all scopes except program unit global scope 
        scope_entry_t* common_sym = fortran_query_name_str(decl_context, get_common_name_str(common_name_str));
        if (common_sym == NULL)
        {
            // If the symbol is not found, we should create a new one
            common_sym = new_common(decl_context, common_name_str);
            common_sym->file = ASTFileName(a);
            common_sym->line = ASTLine(a);
        }
        else
        {
            remove_not_fully_defined_symbol(decl_context, common_sym);
        }
        
        AST it2;
        for_each_element(common_block_object_list, it2)
        {
            AST common_block_object = ASTSon1(it2);

            AST name = NULL;
            AST array_spec = NULL;
            if (ASTType(common_block_object) == AST_SYMBOL)
            {
                name = common_block_object;
            }
            else if (ASTType(common_block_object) == AST_DIMENSION_DECL)
            {
                name = ASTSon0(common_block_object);
                array_spec = ASTSon1(common_block_object);
            }
            else
            {
                internal_error("Unexpected node '%s'\n", ast_print_node_type(ASTType(common_block_object)));
            }

            scope_entry_t* sym = get_symbol_for_name(decl_context, name, ASTText(name));
            
            if (sym->entity_specs.is_in_common)
            {
                error_printf("%s: error: entity '%s' is already in a COMMON\n", 
                        ast_location(name),
                        sym->symbol_name);
                continue;
            }

            if (sym->kind == SK_UNDEFINED)
            {
                sym->kind = SK_VARIABLE;
                remove_unknown_kind_symbol(decl_context, sym);
            }
            // We mark the symbol as non static and is in a common
            sym->entity_specs.is_static = 0;
            sym->entity_specs.is_in_common = 1;
            sym->entity_specs.in_common = common_sym;

            // This name cannot be used as a function name anymore
            if (sym->entity_specs.is_implicit_basic_type)
                sym->entity_specs.is_implicit_but_not_function = 1;

            if (array_spec != NULL)
            {
                if (is_fortran_array_type(no_ref(sym->type_information))
                        || is_pointer_to_fortran_array_type(no_ref(sym->type_information)))
                {
                    error_printf("%s: error: entity '%s' has already a DIMENSION attribute\n",
                            ast_location(a),
                            sym->symbol_name);
                    continue;
                }

                char was_ref = is_lvalue_reference_type(sym->type_information);

                type_t* array_type = compute_type_from_array_spec(no_ref(sym->type_information),
                        array_spec,
                        decl_context,
                        /* array_spec_kind */ NULL);
                sym->type_information = array_type;

                if (was_ref)
                {
                    sym->type_information = get_lvalue_reference_type(sym->type_information);
                }
            }

            P_LIST_ADD(common_sym->entity_specs.related_symbols, common_sym->entity_specs.num_related_symbols, sym);
        }
    }

}

static void build_scope_computed_goto_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // warn_printf("%s: warning: deprecated computed-goto statement\n", 
    //         ast_location(a));
    AST label_list = ASTSon0(a);
    nodecl_t nodecl_label_list = nodecl_null();
    AST it;
    for_each_element(label_list, it)
    {
        AST label = ASTSon1(it);

        scope_entry_t* label_sym = fortran_query_label(label, decl_context, /* is_definition */ 0);

        nodecl_label_list = nodecl_append_to_list(nodecl_label_list, 
                nodecl_make_symbol(label_sym, ASTFileName(label), ASTLine(label)));
    }

    nodecl_t nodecl_expr = nodecl_null();
    fortran_check_expression(ASTSon1(a), decl_context, &nodecl_expr);

    *nodecl_output = nodecl_make_fortran_computed_goto_statement(
            nodecl_label_list,
            nodecl_expr,
            ASTFileName(a),
            ASTLine(a));
}

static void build_scope_assigned_goto_stmt(AST a UNUSED_PARAMETER, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    // warn_printf("%s: warning: deprecated assigned-goto statement\n", 
    //         ast_location(a));

    scope_entry_t* label_var = fortran_get_variable_with_locus(decl_context, ASTSon0(a), ASTText(ASTSon0(a)));

    AST label_list = ASTSon1(a);
    nodecl_t nodecl_label_list = nodecl_null();
    AST it;
    for_each_element(label_list, it)
    {
        AST label = ASTSon1(it);

        scope_entry_t* label_sym = fortran_query_label(label, decl_context, /* is_definition */ 0);

        nodecl_label_list = nodecl_append_to_list(nodecl_label_list, 
                nodecl_make_symbol(label_sym, ASTFileName(label), ASTLine(label)));
    }

    *nodecl_output = nodecl_make_fortran_assigned_goto_statement(
            nodecl_make_symbol(label_var, ASTFileName(a), ASTLine(a)),
            nodecl_label_list,
            ASTFileName(a),
            ASTLine(a));
}

static void build_scope_label_assign_stmt(AST a UNUSED_PARAMETER, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    // warn_printf("%s: warning: deprecated label-assignment statement\n", 
    //         ast_location(a));

    AST literal_const = ASTSon0(a);

    nodecl_t nodecl_literal = nodecl_null();
    fortran_check_expression(literal_const, decl_context, &nodecl_literal);

    AST label_name = ASTSon1(a);

    scope_entry_t* label_var = fortran_get_variable_with_locus(decl_context, label_name, ASTText(label_name));

    if (label_var == NULL)
    {
        error_printf("%s: error: symbol '%s' is unknown\n", ast_location(label_name), ASTText(label_name));
        return;
    }
    ERROR_CONDITION(label_var == NULL, "Invalid symbol", 0);

    *nodecl_output = nodecl_make_fortran_label_assign_statement(
            nodecl_literal,
            nodecl_make_symbol(label_var, ASTFileName(label_name), ASTLine(label_name)),
            ASTFileName(a),
            ASTLine(a));
}

scope_entry_t* fortran_query_label(AST label, 
        decl_context_t decl_context, 
        char is_definition)
{
    decl_context_t global_context = decl_context;
    global_context.current_scope = global_context.function_scope;

    const char* label_text = strappend(".label_", ASTText(label));
    scope_entry_list_t* entry_list = query_name_str(global_context, label_text);

    scope_entry_t* new_label = NULL;
    if (entry_list == NULL)
    {
        new_label = new_symbol(decl_context, decl_context.function_scope, label_text);
        // Fix the symbol name (which for labels does not match the query name)
        new_label->symbol_name = ASTText(label);
        new_label->kind = SK_LABEL;
        new_label->line = ASTLine(label);
        new_label->file = ASTFileName(label);
        new_label->do_not_print = 1;
        new_label->defined = is_definition;
    }
    else
    {
        new_label = entry_list_head(entry_list);
        if (is_definition)
        {
            if (new_label->defined)
            {
                error_printf("%s: error: label %s has already been defined in %s:%d\n",
                        ast_location(label),
                        new_label->symbol_name,
                        new_label->file, new_label->line);
            }
            else
            {
                new_label->defined = 1;
            }
        }
    }

    entry_list_free(entry_list);
    return new_label;
}

static void build_scope_labeled_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST label = ASTSon0(a);
    AST statement = ASTSon1(a);

    // Sign in the label
    scope_entry_t* label_sym = fortran_query_label(label, decl_context, /* is_definition */ 1);

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(statement, decl_context, &nodecl_statement);


    if (!nodecl_is_null(nodecl_statement))
    {
        if (!nodecl_is_list(nodecl_statement))
        {
            nodecl_statement = nodecl_make_list_1(nodecl_statement);
        }

        *nodecl_output = nodecl_make_labeled_statement(nodecl_statement, label_sym, ASTFileName(a), ASTLine(a));
    }
}

static void build_scope_continue_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    // Do nothing for continue
    *nodecl_output = nodecl_make_empty_statement(ASTFileName(a), ASTLine(a));
}

static void build_scope_critical_construct(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "CRITICAL");
}

static void build_scope_cycle_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    // Do nothing for cycle
    *nodecl_output = nodecl_make_continue_statement(ASTFileName(a), ASTLine(a));
}

static void generic_implied_do_handler(AST a, decl_context_t decl_context,
        void (*rec_handler)(AST, decl_context_t, nodecl_t* nodecl_output),
        nodecl_t* nodecl_output)
{
    AST implied_do_object_list = ASTSon0(a);
    AST implied_do_control = ASTSon1(a);

    AST io_do_variable = ASTSon0(implied_do_control);
    AST lower_bound = ASTSon1(implied_do_control);
    AST upper_bound = ASTSon2(implied_do_control);
    AST stride = ASTSon3(implied_do_control);

    nodecl_t nodecl_lower = nodecl_null();
    fortran_check_expression(lower_bound, decl_context, &nodecl_lower);
    nodecl_t nodecl_upper = nodecl_null();
    fortran_check_expression(upper_bound, decl_context, &nodecl_upper);

    nodecl_t nodecl_stride = nodecl_null();
    if (stride != NULL)
    {
        fortran_check_expression(stride, decl_context, &nodecl_stride);
    }
    else
    {
        nodecl_stride = const_value_to_nodecl(const_value_get_one(/* bytes */ fortran_get_default_integer_type_kind(), /* signed */ 1));
    }

    scope_entry_t* do_variable = fortran_get_variable_with_locus(decl_context, io_do_variable, ASTText(io_do_variable));

    if (do_variable == NULL)
    {
        error_printf("%s: error: unknown symbol '%s' in io-implied-do\n", ast_location(io_do_variable), ASTText(io_do_variable));
        *nodecl_output = nodecl_make_err_expr(ASTFileName(io_do_variable), ASTLine(io_do_variable));
        return;
    }

    if (do_variable->kind == SK_UNDEFINED)
    {
        do_variable->kind = SK_VARIABLE;
        remove_unknown_kind_symbol(decl_context, do_variable);
    }
    else if (do_variable->kind != SK_VARIABLE)
    {
        error_printf("%s: error: invalid name '%s' for io-implied-do\n", ast_location(io_do_variable), ASTText(io_do_variable));
        *nodecl_output = nodecl_make_err_expr(ASTFileName(io_do_variable), ASTLine(io_do_variable));
        return;
    }


    nodecl_t nodecl_rec = nodecl_null();
    rec_handler(implied_do_object_list, decl_context, &nodecl_rec);

    *nodecl_output = nodecl_make_fortran_implied_do(
            nodecl_make_symbol(do_variable, ASTFileName(io_do_variable), ASTLine(io_do_variable)),
            nodecl_make_range(nodecl_lower, nodecl_upper, nodecl_stride, 
                fortran_get_default_integer_type(),
                ASTFileName(implied_do_control), ASTLine(implied_do_control)),
            nodecl_rec,
            ASTFileName(a), ASTLine(a));
}

static void build_scope_data_stmt_object_list(AST data_stmt_object_list, decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST it2;
    for_each_element(data_stmt_object_list, it2)
    {
        AST data_stmt_object = ASTSon1(it2);
        if (ASTType(data_stmt_object) == AST_IMPLIED_DO)
        {
            nodecl_t nodecl_implied_do = nodecl_null();
            generic_implied_do_handler(data_stmt_object, decl_context,
                    build_scope_data_stmt_object_list, &nodecl_implied_do);
            *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_implied_do);
        }
        else
        {
            nodecl_t nodecl_data_stmt_object = nodecl_null();
            fortran_check_expression(data_stmt_object, decl_context, &nodecl_data_stmt_object);
            *nodecl_output = nodecl_append_to_list(*nodecl_output, 
                    nodecl_data_stmt_object);

            // Set the SAVE attribute
            scope_entry_t* entry = nodecl_get_symbol(nodecl_data_stmt_object);
            // If the symbol appears in a common, the symbol will never be static
            if (entry != NULL && !entry->entity_specs.is_in_common)
            {
                entry->entity_specs.is_static = 1;
            }
        }
    }
}

static void build_scope_data_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST data_stmt_set_list = ASTSon0(a);

    scope_entry_t* entry = get_or_create_data_symbol_info(decl_context);
    
    AST it;
    for_each_element(data_stmt_set_list, it)
    {
        AST data_stmt_set = ASTSon1(it);

        AST data_stmt_object_list = ASTSon0(data_stmt_set);
        nodecl_t nodecl_item_set = nodecl_null();
        build_scope_data_stmt_object_list(data_stmt_object_list, decl_context, &nodecl_item_set);

        nodecl_t nodecl_data_set = nodecl_null();

        AST data_stmt_value_list = ASTSon1(data_stmt_set);
        AST it2;
        for_each_element(data_stmt_value_list, it2)
        {
            AST data_stmt_value = ASTSon1(it2);
            if (ASTType(data_stmt_value) == AST_MUL)
            {
                nodecl_t nodecl_repeat;
                fortran_check_expression(ASTSon0(data_stmt_value), decl_context, &nodecl_repeat);

                if (!nodecl_is_constant(nodecl_repeat))
                {
                    error_printf("%s: error: data-stmt-repeat '%s' is not a constant expression\n",
                            nodecl_get_locus(nodecl_repeat),
                            codegen_to_str(nodecl_repeat));
                }

                nodecl_t nodecl_value;
                fortran_check_expression(ASTSon1(data_stmt_value), decl_context, &nodecl_value);

                if (!nodecl_is_constant(nodecl_value))
                {
                    error_printf("%s: error: data-stmt-value '%s' is not a constant expression\n",
                            nodecl_get_locus(nodecl_value),
                            codegen_to_str(nodecl_value));
                }

                if (!nodecl_is_constant(nodecl_repeat)
                        || !nodecl_is_constant(nodecl_value))
                    continue;

                if (const_value_is_nonzero
                        (const_value_lt(nodecl_get_constant(nodecl_repeat), 
                                        const_value_get_zero(fortran_get_default_integer_type_kind(), 1))))
                {
                    error_printf("%s: error: data-stmt-repeat is negative\n", nodecl_get_locus(nodecl_repeat));
                    continue;
                }

                uint64_t repeat = const_value_cast_to_8(nodecl_get_constant(nodecl_repeat));
                uint64_t i;
                for (i = 0; i < repeat; i++)
                {
                    nodecl_data_set = nodecl_append_to_list(nodecl_data_set, nodecl_copy(nodecl_value));
                }
            }
            else
            {
                nodecl_t nodecl_value = nodecl_null();

                fortran_check_expression(data_stmt_value, decl_context, &nodecl_value);

                if (!nodecl_is_constant(nodecl_value))
                {
                    error_printf("%s: error: data-stmt-value '%s' is not a constant expression\n",
                            nodecl_get_locus(nodecl_value),
                            codegen_to_str(nodecl_value));
                    continue;
                }

                nodecl_data_set = nodecl_append_to_list(nodecl_data_set, nodecl_value);
            }
        }

        entry->value = nodecl_append_to_list(entry->value, 
                nodecl_make_fortran_data(nodecl_item_set, nodecl_data_set, ASTFileName(data_stmt_set), ASTLine(data_stmt_set)));
        
        // This name cannot be used as a function name anymore
        if (entry->entity_specs.is_implicit_basic_type)
            entry->entity_specs.is_implicit_but_not_function = 1;
    }

}

static void build_scope_deallocate_stmt(AST a, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST allocate_object_list = ASTSon0(a);
    AST dealloc_opt_list = ASTSon1(a);

    nodecl_t nodecl_expr_list = nodecl_null();
    AST it;
    for_each_element(allocate_object_list, it)
    {
        AST allocate_object = ASTSon1(it);

        if (ASTType(allocate_object) == AST_DIMENSION_DECL)
        {
            running_error("%s: sorry: coarrays not supported\n", 
                    ast_location(allocate_object));
        }

        AST data_ref = allocate_object;
        nodecl_t nodecl_data_ref = nodecl_null();
        fortran_check_expression(data_ref, decl_context, &nodecl_data_ref);

        if (!nodecl_is_err_expr(nodecl_data_ref))
        {
            scope_entry_t* entry = nodecl_get_symbol(nodecl_data_ref);

            if (!entry->entity_specs.is_allocatable
                    && !is_pointer_type(no_ref(entry->type_information)))
            {
                error_printf("%s: error: only ALLOCATABLE or POINTER can be used in a DEALLOCATE statement\n", 
                        ast_location(a));
                continue;
            }
        }

        nodecl_expr_list = nodecl_append_to_list(nodecl_expr_list, 
                nodecl_data_ref);
    }

    nodecl_t nodecl_opt_value = nodecl_null();
    handle_opt_value_list(a, dealloc_opt_list, decl_context, &nodecl_opt_value);

    *nodecl_output = nodecl_make_fortran_deallocate_statement(nodecl_expr_list, 
            nodecl_opt_value, 
            ASTFileName(a), 
            ASTLine(a));
}

static void build_scope_derived_type_def(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST derived_type_stmt = ASTSon0(a);
    AST derived_type_body = ASTSon1(a);

    AST type_attr_spec_list = ASTSon0(derived_type_stmt);
    AST name = ASTSon1(derived_type_stmt);
    AST type_param_name_list = ASTSon2(derived_type_stmt);

    if (type_param_name_list != NULL)
    {
        running_error("%s: sorry: derived types with type-parameters are not supported\n",
                ast_location(a));
    }

    char bind_c = 0;

    attr_spec_t attr_spec;
    memset(&attr_spec, 0, sizeof(attr_spec));

    AST it;
    if (type_attr_spec_list != NULL)
    {
        for_each_element(type_attr_spec_list, it)
        {
            AST type_attr_spec = ASTSon1(it);
            switch (ASTType(type_attr_spec))
            {
                case AST_ABSTRACT:
                    {
                        running_error("%s: error: ABSTRACT derived types are not supported\n", 
                                ast_location(type_attr_spec));
                        break;
                    }
                case AST_ATTR_SPEC:
                    {
                        gather_attr_spec_item(type_attr_spec, decl_context, &attr_spec);
                        break;
                    }
                case AST_BIND_C_SPEC:
                    {
                        bind_c = 1;
                        break;
                    }
                default:
                    {
                        internal_error("%s: unexpected tree\n",
                                ast_location(type_attr_spec));
                    }
            }
        }
    }

    scope_entry_t* class_name = new_fortran_symbol(decl_context, ASTText(name));
    class_name->kind = SK_CLASS;
    class_name->file = ASTFileName(name);
    class_name->line = ASTLine(name);
    class_name->type_information = get_new_class_type(decl_context, CK_STRUCT);
    class_name->entity_specs.bind_c = bind_c;
    
    remove_unknown_kind_symbol(decl_context, class_name);
    if (attr_spec.is_public)
    {
        class_name->entity_specs.access = AS_PUBLIC;
    }
    else if (attr_spec.is_private)
    {
        class_name->entity_specs.access = AS_PRIVATE;
    }

    // Derived type body
    AST type_param_def_stmt_seq = ASTSon0(derived_type_body);
    AST private_or_sequence_seq = ASTSon1(derived_type_body);
    AST component_part = ASTSon2(derived_type_body);
    AST type_bound_procedure_part = ASTSon3(derived_type_body);

    if (type_param_def_stmt_seq != NULL)
    {
        running_error("%s: sorry: type-parameter definitions are not supported\n",
                ast_location(type_param_def_stmt_seq));
    }

    char is_sequence = 0;
    char fields_are_private = 0;
    if (private_or_sequence_seq != NULL)
    {
        for_each_element(private_or_sequence_seq, it)
        {
            AST private_or_sequence = ASTSon1(it);

            if (ASTType(private_or_sequence) == AST_SEQUENCE_STATEMENT)
            {
                if (is_sequence)
                {
                    error_printf("%s: error: SEQUENCE statement specified twice", 
                            ast_location(private_or_sequence));
                }
                is_sequence = 1;
            }
            else if (ASTType(private_or_sequence) == AST_ACCESS_STATEMENT)
            {
                if (fields_are_private)
                {
                    error_printf("%s: error: PRIVATE statement specified twice", 
                            ast_location(private_or_sequence));
                }
                // This can only be a private_stmt, no need to check it here
                fields_are_private = 1;
            }
            else
            {
                internal_error("%s: Unexpected statement '%s'\n", 
                        ast_location(private_or_sequence),
                        ast_print_node_type(ASTType(private_or_sequence)));
            }
        }
    }

    if (type_bound_procedure_part != NULL)
    {
        running_error("%s: sorry: type-bound procedures are not supported\n",
                ast_location(type_bound_procedure_part));
    }

    decl_context_t inner_decl_context = new_class_context(class_name->decl_context, class_name);
    class_type_set_inner_context(class_name->type_information, inner_decl_context);

    if (component_part != NULL)
    {
        for_each_element(component_part, it)
        {
            AST component_def_stmt = ASTSon1(it);

            if (ASTType(component_def_stmt) == AST_PROC_COMPONENT_DEF_STATEMENT)
            {
                running_error("%s: sorry: unsupported procedure components in derived type definition\n",
                        ast_location(component_def_stmt));
            }
            ERROR_CONDITION(ASTType(component_def_stmt) != AST_DATA_COMPONENT_DEF_STATEMENT, 
                    "Invalid tree", 0);

            AST declaration_type_spec = ASTSon0(component_def_stmt);
            AST component_attr_spec_list = ASTSon1(component_def_stmt);
            AST component_decl_list = ASTSon2(component_def_stmt);

            type_t* basic_type = gather_type_from_declaration_type_spec(declaration_type_spec, decl_context);

            memset(&attr_spec, 0, sizeof(attr_spec));

            if (component_attr_spec_list != NULL)
            {
                gather_attr_spec_list(component_attr_spec_list, decl_context, &attr_spec);
            }

            AST it2;
            for_each_element(component_decl_list, it2)
            {
                attr_spec_t current_attr_spec = attr_spec;
                AST declaration = ASTSon1(it2);

                AST component_name = ASTSon0(declaration);
                AST entity_decl_specs = ASTSon1(declaration);

                // TODO: We should check that the symbol has not been redefined 
                scope_entry_t* entry = new_fortran_symbol(inner_decl_context, ASTText(component_name));

                entry->kind = SK_VARIABLE;

                entry->file = ASTFileName(declaration);
                entry->line = ASTLine(declaration);
                remove_unknown_kind_symbol(inner_decl_context, entry);

                entry->type_information = basic_type;
                entry->entity_specs.is_implicit_basic_type = 0;

                entry->defined = 1;

                AST initialization = NULL;
                AST array_spec = NULL;
                AST coarray_spec = NULL;
                AST char_length = NULL;

                if (entity_decl_specs != NULL)
                {
                    array_spec = ASTSon0(entity_decl_specs);
                    coarray_spec = ASTSon1(entity_decl_specs);
                    char_length = ASTSon2(entity_decl_specs);
                    initialization = ASTSon3(entity_decl_specs);
                }

                if (array_spec != NULL)
                {
                    if (current_attr_spec.is_dimension)
                    {
                        error_printf("%s: error: DIMENSION attribute specified twice\n", ast_location(declaration));
                    }
                    else
                    {
                        current_attr_spec.is_dimension = 1;
                        current_attr_spec.array_spec = array_spec;
                    }
                }

                if (coarray_spec != NULL)
                {
                    if (current_attr_spec.is_codimension)
                    {
                        error_printf("%s: error: CODIMENSION attribute specified twice\n", ast_location(declaration));
                    }
                    else
                    {
                        current_attr_spec.is_codimension = 1;
                        current_attr_spec.coarray_spec = coarray_spec;
                    }
                }

                if (char_length != NULL)
                {
                    if (!is_fortran_character_type(no_ref(entry->type_information)))
                    {
                        error_printf("%s: error: char-length specified but type is not CHARACTER\n", ast_location(declaration));
                    }

                    if (ASTType(char_length) != AST_SYMBOL
                            || strcmp(ASTText(char_length), "*") != 0)
                    {
                        nodecl_t nodecl_char_length = nodecl_null();
                        fortran_check_expression(char_length, decl_context, &nodecl_char_length);

                        nodecl_t lower_bound = nodecl_make_integer_literal(
                                get_signed_int_type(),
                                const_value_get_one(type_get_size(get_signed_int_type()), 1),
                                ASTFileName(char_length), ASTLine(char_length));

                        entry->type_information = get_array_type_bounds(
                                array_type_get_element_type(entry->type_information), 
                                lower_bound, nodecl_char_length, decl_context);
                    }
                    else
                    {
                        entry->type_information = get_array_type(
                                array_type_get_element_type(entry->type_information), 
                                nodecl_null(), decl_context);
                    }
                }

                // Stop the madness here
                if (current_attr_spec.is_codimension)
                {
                    running_error("%s: sorry: coarrays are not supported\n", ast_location(declaration));
                }

                if (current_attr_spec.is_dimension)
                {
                    type_t* array_type = compute_type_from_array_spec(entry->type_information, 
                            current_attr_spec.array_spec,
                            decl_context,
                            /* array_spec_kind */ NULL);
                    entry->type_information = array_type;
                }

                if (current_attr_spec.is_allocatable)
                {
                    if (!current_attr_spec.is_dimension)
                    {
                        error_printf("%s: error: ALLOCATABLE attribute cannot be used on scalars\n", 
                                ast_location(declaration));
                    }
                    else
                    {
                        entry->entity_specs.is_allocatable = 1;
                        entry->kind = SK_VARIABLE;
                    }
                }

                entry->entity_specs.is_target = current_attr_spec.is_target;
                if (fields_are_private
                        && entry->entity_specs.access == AS_UNKNOWN)
                {
                    entry->entity_specs.access = AS_PRIVATE;
                }

                if (current_attr_spec.is_pointer)
                {
                    entry->type_information = get_pointer_type(entry->type_information);
                }

                entry->entity_specs.is_member = 1;
                entry->entity_specs.class_type = get_user_defined_type(class_name);

                if (initialization != NULL)
                {
                    entry->kind = SK_VARIABLE;
                    nodecl_t nodecl_init = nodecl_null();

                    if (ASTType(initialization) == AST_POINTER_INITIALIZATION
                            && current_attr_spec.is_pointer)
                    {
                        initialization = ASTSon0(initialization);
                        fortran_check_initialization(entry, initialization, decl_context, 
                                /* is_pointer_init */ 1,
                                &nodecl_init);
                    }
                    else if (current_attr_spec.is_pointer)
                    {
                        error_printf("%s: error: a POINTER must be initialized using pointer initialization\n",
                                ast_location(initialization));
                    }
                    else if (ASTType(initialization) == AST_POINTER_INITIALIZATION)
                    {
                        error_printf("%s: error: no POINTER attribute, required for pointer initialization\n",
                                ast_location(initialization));
                    }
                    else
                    {
                        fortran_check_initialization(entry, initialization, decl_context, 
                                /* is_pointer_init */ 0,
                                &nodecl_init);

                    }
                    if (!nodecl_is_err_expr(nodecl_init))
                    {
                        entry->value = nodecl_init;
                    }
                }

                class_type_add_member(class_name->type_information, entry);
            }
        }
    }

    set_is_complete_type(class_name->type_information, 1);

    if (decl_context.current_scope->related_entry != NULL
            && decl_context.current_scope->related_entry->kind == SK_MODULE)
    {
        scope_entry_t* module = decl_context.current_scope->related_entry;

        P_LIST_ADD_ONCE(module->entity_specs.related_symbols,
                module->entity_specs.num_related_symbols,
                class_name);

        class_name->entity_specs.in_module = module;
    }
}

static void build_scope_dimension_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST array_name_dim_spec_list = ASTSon0(a);
    AST it;

    for_each_element(array_name_dim_spec_list, it)
    {
        AST dimension_decl = ASTSon1(it);
        AST name = ASTSon0(dimension_decl);

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        if (is_fortran_array_type(no_ref(entry->type_information))
                || is_pointer_to_fortran_array_type(no_ref(entry->type_information)))
        {
            error_printf("%s: error: entity '%s' already has a DIMENSION attribute\n",
                    ast_location(name),
                    ASTText(name));
            continue;
        }

        char was_ref = is_lvalue_reference_type(entry->type_information);

        char is_pointer = is_pointer_type(no_ref(entry->type_information));

        if (is_pointer_type(no_ref(entry->type_information)))
        {
            entry->type_information = pointer_type_get_pointee_type(no_ref(entry->type_information));
        }

        AST array_spec = ASTSon1(dimension_decl);
        type_t* array_type = compute_type_from_array_spec(no_ref(entry->type_information), 
                array_spec,
                decl_context,
                /* array_spec_kind */ NULL);

        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }
        // This name cannot be used as a function name anymore
        if (entry->entity_specs.is_implicit_basic_type)
            entry->entity_specs.is_implicit_but_not_function = 1;

        entry->type_information = array_type;

        if (is_pointer)
        {
            entry->type_information = get_pointer_type(no_ref(entry->type_information));
        }

        if (was_ref)
        {
            entry->type_information = get_lvalue_reference_type(entry->type_information);
        }
    }

}

static void build_scope_do_construct(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST loop_control = ASTSon0(a);
    AST block = ASTSon1(a);

    AST assig = ASTSon0(loop_control);
    AST upper = ASTSon1(loop_control);
    AST stride = ASTSon2(loop_control);

    nodecl_t nodecl_lower = nodecl_null();
    if (assig != NULL)
    {
        fortran_check_expression(assig, decl_context, &nodecl_lower);

        nodecl_t do_loop_var = nodecl_get_child(nodecl_lower, 0);
        scope_entry_t* sym = nodecl_get_symbol(do_loop_var);
        if (sym != NULL
                && !is_integer_type(no_ref(sym->type_information)))
        {
            warn_printf("%s: warning: loop variable '%s' should be of integer type\n",
                    ast_location(a),
                    codegen_to_str(do_loop_var));
        }
    }
    nodecl_t nodecl_upper = nodecl_null();
    if (upper != NULL)
    {
        fortran_check_expression(upper, decl_context, &nodecl_upper);
    }
    nodecl_t nodecl_stride = nodecl_null();
    if (stride != NULL)
    {
        fortran_check_expression(stride, decl_context, &nodecl_stride);
    }
    else
    {
        nodecl_stride = const_value_to_nodecl(const_value_get_one(/* bytes */ fortran_get_default_integer_type_kind(), /* signed */ 1));
    }

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(block, decl_context, &nodecl_statement);


    *nodecl_output = nodecl_make_for_statement(
            nodecl_make_loop_control(nodecl_lower, 
                nodecl_upper, 
                nodecl_stride, 
                ASTFileName(loop_control), ASTLine(loop_control)),
            nodecl_statement,
            ASTFileName(a), ASTLine(a));
}

static void build_scope_entry_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // if (nodecl_output == NULL)
    // {
    //     fprintf(stderr, "IGNORING ENTRY at %s!\n", ast_location(a));
    //     return;
    // }

    AST name = ASTSon0(a);
    AST dummy_arg_list = ASTSon1(a);
    AST suffix = ASTSon2(a);
     
    // An entry statement must be used in a subroutine or a function
    scope_entry_t* related_sym = decl_context.current_scope->related_entry; 
    if (related_sym == NULL)
    {
        internal_error("%s: error: code unreachable\n", 
                ast_location(a));
    }
    else if (related_sym->kind == SK_PROGRAM) 
    {
        error_printf("%s: error: entry statement '%s' cannot appear within a program\n",
                ast_location(a),
                ASTText(name));
    }

    if (nodecl_output == NULL)
    {
        // We are analyzing this ENTRY statement as if it were a declaration, no nodecls are created
        char is_function = !is_void_type(function_type_get_return_type(related_sym->type_information)); 
        scope_entry_t* entry = new_entry_symbol(decl_context, name, suffix, dummy_arg_list, is_function);

        if (related_sym->entity_specs.in_module != NULL)
        {
            // Our principal procedure is a module procedure, this symbol will live as a sibling
            insert_entry(related_sym->entity_specs.in_module->decl_context.current_scope, entry);
        }

        // And we piggyback the entry in the node so we avoid a query later 
        nodecl_set_symbol(_nodecl_wrap(a), entry);
    }
    else
    {
        // Recover the symbol we piggybacked 
        scope_entry_t* entry = nodecl_get_symbol(_nodecl_wrap(a));

        // Create the entry statement so we remember the entry point
        *nodecl_output = nodecl_make_fortran_entry_statement(entry, ASTFileName(a), ASTLine(a));
    }
}

static void build_scope_enum_def(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_construct(a, "ENUM");
}

static void build_scope_equivalence_stmt(AST a, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST equivalence_set_list = ASTSon0(a);

    scope_entry_t* equivalence_info = get_or_create_equivalence_symbol_info(decl_context);

    AST it;
    for_each_element(equivalence_set_list, it)
    {
        AST equivalence_set = ASTSon1(it);

        AST equivalence_object = ASTSon0(equivalence_set);
        AST equivalence_object_list = ASTSon1(equivalence_set);

        nodecl_t nodecl_equivalence_object = nodecl_null();
        fortran_check_expression(equivalence_object, decl_context, &nodecl_equivalence_object);

        nodecl_t nodecl_equivalence_set = nodecl_null();

        AST it2;
        for_each_element(equivalence_object_list, it2)
        {
            AST equiv_obj = ASTSon1(it2);
            nodecl_t nodecl_current_equiv_obj = nodecl_null();
            fortran_check_expression(equiv_obj, decl_context, &nodecl_current_equiv_obj);

            nodecl_equivalence_set = nodecl_append_to_list(nodecl_equivalence_set, 
                    nodecl_current_equiv_obj);
        }

        nodecl_t nodecl_equivalence = nodecl_make_fortran_equivalence(
                nodecl_equivalence_object,
                nodecl_equivalence_set,
                ASTFileName(equivalence_set),
                ASTLine(equivalence_set));

        equivalence_info->value = nodecl_append_to_list(equivalence_info->value, 
                nodecl_equivalence);
    }

}

static void build_scope_exit_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    // Do nothing for exit
    *nodecl_output = nodecl_make_break_statement(ASTFileName(a), ASTLine(a));
}

static void build_scope_external_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST name_list = ASTSon0(a);
    AST it;

    for_each_element(name_list, it)
    {
        AST name = ASTSon1(it);

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        //If entry already has kind SK_FUNCTION then we must show an error
        if (entry->kind == SK_FUNCTION) 
        {
            // We have seen an INTRINSIC statement before for the same symbol 
            if (entry->entity_specs.is_builtin)
            {
                error_printf("%s: error: entity '%s' already has INTRINSIC attribute and INTRINSIC attribute conflicts with EXTERNAL attribute\n",
                        ast_location(name),
                        entry->symbol_name);
                continue;
            }
            // We have seen an EXTERNAL statement before for the same symbol
            else 
            {
                error_printf("%s: error: entity '%s' already has EXTERNAL attribute\n",
                        ast_location(name),
                        entry->symbol_name);
                continue;
            }
        }
        
        // We mark the symbol as a external function
        entry->kind = SK_FUNCTION;
        entry->entity_specs.is_extern = 1;
        remove_unknown_kind_symbol(decl_context, entry);

        if (is_void_type(no_ref(entry->type_information)))
        {
            // We do not know it, set a type like one of a SUBROUTINE
            entry->type_information = get_nonproto_function_type(get_void_type(), 0);
        }
        else
        {
            entry->type_information = get_nonproto_function_type(entry->type_information, 0);
        }
        remove_untyped_symbol(decl_context, entry);
    }
}

static void build_scope_forall_header(AST a, decl_context_t decl_context, 
        nodecl_t* loop_control_list, nodecl_t* nodecl_mask_expr)
{
    AST type_spec = ASTSon0(a);
    if (type_spec != NULL)
    {
        running_error("%s: sorry: type-specifier not supported in FORALL header\n",
                ast_location(a));
    }

    AST forall_triplet_list = ASTSon1(a);
    AST mask_expr = ASTSon2(a);

    AST it;
    for_each_element(forall_triplet_list, it)
    {
        AST forall_triplet_spec = ASTSon1(it);

        AST name = ASTSon0(forall_triplet_spec);
        AST forall_lower = ASTSon1(forall_triplet_spec);
        AST forall_upper = ASTSon2(forall_triplet_spec);
        AST forall_step  = ASTSon3(forall_triplet_spec);

        nodecl_t nodecl_name = nodecl_null();
        fortran_check_expression(name, decl_context, &nodecl_name);
        nodecl_t nodecl_lower = nodecl_null();
        fortran_check_expression(forall_lower, decl_context, &nodecl_lower);
        nodecl_t nodecl_upper = nodecl_null();
        fortran_check_expression(forall_upper, decl_context, &nodecl_upper);
        nodecl_t nodecl_step = nodecl_null();
        if (forall_step != NULL)
        {
            fortran_check_expression(forall_step, decl_context, &nodecl_step);
        }

        nodecl_t nodecl_triplet = nodecl_make_loop_control(
                nodecl_make_assignment(
                    nodecl_name,
                    nodecl_lower,
                    nodecl_get_type(nodecl_name), 
                    ASTFileName(name), ASTLine(name)),
                nodecl_upper,
                nodecl_step,
                ASTFileName(a),
                ASTLine(a));

        *loop_control_list = nodecl_append_to_list(*loop_control_list,
                nodecl_triplet);
    }

    if (mask_expr != NULL)
    {
        fortran_check_expression(mask_expr, decl_context, nodecl_mask_expr);
    }
}

static void build_scope_forall_construct(AST a, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST forall_construct_stmt = ASTSon0(a);
    AST forall_body_construct_seq = ASTSon1(a);

    AST forall_header = ASTSon1(forall_construct_stmt);

    nodecl_t nodecl_mask = nodecl_null();
    nodecl_t nodecl_loop_control_list = nodecl_null();

    build_scope_forall_header(forall_header, decl_context, 
            &nodecl_loop_control_list, &nodecl_mask);

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(forall_body_construct_seq, decl_context, &nodecl_statement);

    *nodecl_output = nodecl_make_fortran_forall(nodecl_loop_control_list, 
            nodecl_mask, 
            nodecl_statement,
            ASTFileName(a), ASTLine(a));
}

static void build_scope_forall_stmt(AST a, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST forall_header = ASTSon0(a);
    AST forall_assignment_stmts = ASTSon1(a);

    nodecl_t nodecl_mask = nodecl_null();
    nodecl_t nodecl_loop_control_list = nodecl_null();

    build_scope_forall_header(forall_header, decl_context, 
            &nodecl_loop_control_list, &nodecl_mask);

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(forall_assignment_stmts, decl_context, &nodecl_statement);

    *nodecl_output = nodecl_make_fortran_forall(nodecl_loop_control_list, 
            nodecl_mask, 
            nodecl_make_list_1(nodecl_statement),
            ASTFileName(a), ASTLine(a));
}

static void build_scope_format_stmt(AST a,
        decl_context_t decl_context, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{

    AST label = ASTSon0(a);
    AST format = ASTSon1(a);

    // Keep the format in the label
    scope_entry_t* label_sym = fortran_query_label(label, decl_context, /* is_definition */ 0);
    label_sym->value = nodecl_make_text(ASTText(format), ASTFileName(format), ASTLine(format));
}

static void build_scope_goto_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Do nothing for GOTO at the moment
    scope_entry_t* label_symbol = fortran_query_label(ASTSon0(a), decl_context, /* is_definition */ 0);
    *nodecl_output = nodecl_make_goto_statement(label_symbol, ASTFileName(a), ASTLine(a));
}

static void build_scope_if_construct(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST logical_expr = ASTSon0(a);
    AST then_statement = ASTSon1(a);
    AST else_statement = ASTSon2(a);

    nodecl_t nodecl_logical_expr = nodecl_null();
    fortran_check_expression(logical_expr, decl_context, &nodecl_logical_expr);

    nodecl_t nodecl_then = nodecl_null();
    fortran_build_scope_statement(then_statement, decl_context, &nodecl_then);

    nodecl_t nodecl_else = nodecl_null();
    if (else_statement != NULL)
    {
        fortran_build_scope_statement(else_statement, decl_context, &nodecl_else);
    }


    if (!nodecl_is_list(nodecl_then))
    {
        nodecl_then = nodecl_make_list_1(nodecl_then);
    }

    if (!nodecl_is_list(nodecl_else))
    {
        nodecl_else = nodecl_make_list_1(nodecl_else);
    }

    *nodecl_output = nodecl_make_if_else_statement(
            nodecl_logical_expr,
            nodecl_then,
            nodecl_else,
            ASTFileName(a),
            ASTLine(a));
}

static void build_scope_implicit_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST implicit_spec_list = ASTSon0(a);
    if (implicit_spec_list == NULL)
    {
        if (implicit_has_been_set(decl_context))
        {
            if (is_implicit_none(decl_context))
            {
                error_printf("%s: error: IMPLICIT NONE specified twice\n",
                        ast_location(a));
            }
            else 
            {
                error_printf("%s: error: IMPLICIT NONE after IMPLICIT\n",
                        ast_location(a));
            }
        }
        set_implicit_none(decl_context);
    }
    else
    {
        if (implicit_has_been_set(decl_context)
                && is_implicit_none(decl_context))
        {
            error_printf("%s: error: IMPLICIT after IMPLICIT NONE\n",
                    ast_location(a));
        }

        AST it;
        for_each_element(implicit_spec_list, it)
        {
            AST implicit_spec = ASTSon1(it);

            AST declaration_type_spec = ASTSon0(implicit_spec);
            AST letter_spec_list = ASTSon1(implicit_spec);

            type_t* basic_type = gather_type_from_declaration_type_spec(declaration_type_spec, decl_context);

            if (basic_type == NULL)
            {
                error_printf("%s: error: invalid type specifier '%s' in IMPLICIT statement\n",
                        ast_location(declaration_type_spec),
                        fortran_prettyprint_in_buffer(declaration_type_spec));
                continue;
            }

            AST it2;
            for_each_element(letter_spec_list, it2)
            {
                AST letter_spec = ASTSon1(it2);

                AST letter0 = ASTSon0(letter_spec);
                AST letter1 = ASTSon1(letter_spec);

                const char* letter0_str = ASTText(letter0);
                const char* letter1_str = NULL;

                if (letter1 != NULL)
                {
                    letter1_str = ASTText(letter1);
                }

                char valid = 1;
                if (strlen(letter0_str) != 1
                        || !(('a' <= tolower(letter0_str[0]))
                            && (tolower(letter0_str[0]) <= 'z'))
                        || (letter1_str != NULL 
                            && (strlen(letter1_str) != 1
                                || !(('a' <= tolower(letter1_str[0]))
                                    && (tolower(letter1_str[0]) <= 'z')))))
                {
                    error_printf("%s: error: invalid IMPLICIT letter specifier '%s'\n", 
                            ast_location(letter_spec),
                            fortran_prettyprint_in_buffer(letter_spec));
                    valid = 0;
                }

                if (valid)
                {
                    if (letter1_str == NULL)
                        letter1_str = letter0_str;

                    set_implicit_info(decl_context, letter0_str[0], letter1_str[0], basic_type);
                }
            }
        }
    }
    update_untyped_symbols(decl_context);
}

static void build_scope_import_stmt(AST a UNUSED_PARAMETER, decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "IMPORT");
}

static void build_scope_intent_stmt(AST a, decl_context_t decl_context, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST intent_spec = ASTSon0(a);
    AST dummy_arg_name_list = ASTSon1(a);

    AST it;
    for_each_element(dummy_arg_name_list, it)
    {
        AST dummy_arg = ASTSon1(it);

        scope_entry_t* entry = get_symbol_for_name(decl_context, dummy_arg, ASTText(dummy_arg));

        if (!entry->entity_specs.is_parameter)
        {
            error_printf("%s: error: entity '%s' is not a dummy argument\n",
                    ast_location(dummy_arg),
                    fortran_prettyprint_in_buffer(dummy_arg));
            continue;
        }

        if (entry->entity_specs.intent_kind != INTENT_INVALID)
        {
            error_printf("%s: error: entity '%s' already has an INTENT attribute\n",
                    ast_location(dummy_arg),
                    fortran_prettyprint_in_buffer(dummy_arg));
            continue;
        }

        attr_spec_t attr_spec;
        memset(&attr_spec, 0, sizeof(attr_spec));

        gather_attr_spec_item(intent_spec, decl_context, &attr_spec);

        entry->entity_specs.intent_kind = attr_spec.intent_kind;
    }
}

static void build_scope_interface_block(AST a, decl_context_t decl_context, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST interface_stmt = ASTSon0(a);
    AST interface_specification_seq = ASTSon1(a);

    AST abstract = ASTSon0(interface_stmt);
    if (abstract != NULL)
    {
        unsupported_construct(a, "ABSTRACT INTERFACE");
    }

    AST generic_spec = ASTSon1(interface_stmt);

    scope_entry_t** related_symbols = NULL;
    int num_related_symbols = 0;

    if (interface_specification_seq != NULL)
    {
        AST it;
        for_each_element(interface_specification_seq, it)
        {
            AST interface_specification = ASTSon1(it);

            if (ASTType(interface_specification) == AST_PROCEDURE)
            {
                unsupported_statement(interface_specification, "PROCEDURE");
            }
            else if (ASTType(interface_specification) == AST_MODULE_PROCEDURE)
            {
                AST procedure_name_list = ASTSon0(interface_specification);
                AST it2;
                if (decl_context.current_scope->related_entry->kind == SK_MODULE)
                {
                    for_each_element(procedure_name_list, it2)
                    {
                        AST procedure_name = ASTSon1(it2);

                        scope_entry_t* entry = get_symbol_for_name_untyped(decl_context, procedure_name,
                                ASTText(procedure_name));

                        entry->kind = SK_FUNCTION;
                        entry->entity_specs.is_module_procedure = 1;
                        
                        remove_unknown_kind_symbol(decl_context, entry);
                        add_not_fully_defined_symbol(decl_context, entry);

                        if (generic_spec != NULL)
                        {
                            P_LIST_ADD(related_symbols,
                                    num_related_symbols,
                                    entry);
                        }
                    }
                }
                else
                {
                    for_each_element(procedure_name_list, it2)
                    {
                        AST procedure_name = ASTSon1(it2);

                        scope_entry_t* entry = get_symbol_for_name(decl_context, procedure_name,
                                ASTText(procedure_name));

                        if (entry == NULL
                                || entry->entity_specs.from_module == NULL
                                || entry->kind != SK_FUNCTION)
                        {
                            error_printf("%s: error: name '%s' is not a module procedure\n", 
                                    ast_location(procedure_name),
                                    prettyprint_in_buffer(procedure_name));
                        }
                        else
                        {
                            if (generic_spec != NULL)
                            {
                                P_LIST_ADD(related_symbols,
                                        num_related_symbols,
                                        entry);
                            }
                            // We do not insert the symbol since it is already
                            // available in this scope
                        }
                    }
                }
            }
            else if (ASTType(interface_specification) == AST_SUBROUTINE_PROGRAM_UNIT
                    || ASTType(interface_specification) == AST_FUNCTION_PROGRAM_UNIT)
            {
                nodecl_t nodecl_program_unit = nodecl_null();
                scope_entry_t* procedure_sym = NULL;
                build_scope_program_unit_internal(interface_specification, 
                        decl_context, 
                        &procedure_sym,
                        &nodecl_program_unit);

                if (generic_spec != NULL)
                {
                    P_LIST_ADD(related_symbols,
                            num_related_symbols,
                            procedure_sym);
                }

                // Insert this symbol to the enclosing context
                insert_entry(decl_context.current_scope, procedure_sym);
                // And update its context to be the enclosing one
                procedure_sym->decl_context = decl_context;

                remove_untyped_symbol(decl_context, procedure_sym);
                remove_unknown_kind_symbol(decl_context, procedure_sym);
            }
            else
            {
                internal_error("Invalid tree '%s'\n", ast_print_node_type(ASTType(interface_specification)));
            }
        }
    }

    if (generic_spec != NULL)
    {
        const char* name = get_name_of_generic_spec(generic_spec);
        scope_entry_t* generic_spec_sym = fortran_query_name_str(decl_context, name);

        if (generic_spec_sym == NULL)
        {
            generic_spec_sym = new_fortran_symbol(decl_context, name);
            // If this name is not related to a specific interface, make it void
            generic_spec_sym->type_information = get_void_type();

            generic_spec_sym->file = ASTFileName(generic_spec);
            generic_spec_sym->line = ASTLine(generic_spec);
        }

        if (generic_spec_sym->kind != SK_UNDEFINED
                && generic_spec_sym->kind != SK_FUNCTION)
        {
            error_printf("%s: error: redefining symbol '%s'\n", 
                    ast_location(generic_spec),
                    name);
            return;
        }

        // The symbol won't be unknown anymore
        remove_untyped_symbol(decl_context, generic_spec_sym);
        
        generic_spec_sym->kind = SK_FUNCTION;
        generic_spec_sym->entity_specs.is_generic_spec = 1;
        generic_spec_sym->entity_specs.is_implicit_basic_type = 0;
        remove_unknown_kind_symbol(decl_context, generic_spec_sym);

        int i;
        for (i = 0; i < num_related_symbols; i++)
        {
            P_LIST_ADD(generic_spec_sym->entity_specs.related_symbols,
                    generic_spec_sym->entity_specs.num_related_symbols,
                    related_symbols[i]);
        }

        if (decl_context.current_scope->related_entry != NULL
                && decl_context.current_scope->related_entry->kind == SK_MODULE)
        {
            scope_entry_t* module = decl_context.current_scope->related_entry;

            P_LIST_ADD_ONCE(module->entity_specs.related_symbols,
                    module->entity_specs.num_related_symbols,
                    generic_spec_sym);

            generic_spec_sym->entity_specs.in_module = module;
        }
    }
}

static void build_scope_intrinsic_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST intrinsic_list = ASTSon0(a);

    AST it;
    for_each_element(intrinsic_list, it)
    {
        AST name = ASTSon1(it);

        // Intrinsics are kept in global scope
        decl_context_t global_context = decl_context;
        global_context.current_scope = decl_context.global_scope;

        scope_entry_t* entry = fortran_query_name_str(decl_context, ASTText(name));
        scope_entry_t* entry_intrinsic = fortran_query_intrinsic_name_str(global_context, ASTText(name));
        
        // The symbol exists in the current scope 
        if (entry != NULL)
        {
            //If entry already has kind SK_FUNCTION then we must show an error
            if (entry->kind == SK_FUNCTION) 
            {
                // We have seen an INTRINSIC statement before for the same symbol 
                if (entry->entity_specs.is_builtin)
                {
                    error_printf("%s: error: entity '%s' already has INTRINSIC attribute\n",
                            ast_location(name),
                            entry->symbol_name);
                    continue;
                }
                // We have seen an EXTERNAL statement before for the same symbol
                else 
                {
                    error_printf("%s: error: entity '%s' already has EXTERNAL attribute and EXTERNAL attribute conflicts with EXTERNAL attribute\n",
                            ast_location(name),
                            entry->symbol_name);

                    continue;
                }
            }
            // This symbol will be the intrinsic
            else
            { 
                if (entry_intrinsic == NULL || !entry_intrinsic->entity_specs.is_builtin)
                {
                    error_printf("%s: error: name '%s' is not known as an intrinsic\n", 
                            ast_location(name),
                            ASTText(name));
                    continue;
                }

                entry->kind = SK_FUNCTION;  
                entry->entity_specs = entry_intrinsic->entity_specs;
                entry->type_information = entry_intrinsic->type_information;
                remove_unknown_kind_symbol(decl_context, entry);
            }
        }
        // The symbol does not exist, we add an alias to the intrinsic symbol in the current scope
        else
        {
            if (entry_intrinsic == NULL 
                    || !entry_intrinsic->entity_specs.is_builtin)
            {
                error_printf("%s: error: name '%s' is not known as an intrinsic\n", 
                        ast_location(name),
                        ASTText(name));
                continue;
            }
            insert_alias(decl_context.current_scope, entry_intrinsic, strtolower(ASTText(name)));
        }
    }
}

static void build_scope_lock_stmt(AST a UNUSED_PARAMETER, decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "LOCK");
}

static void build_scope_namelist_stmt(AST a, decl_context_t decl_context, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST namelist_item_list = ASTSon0(a);

    AST it;
    for_each_element(namelist_item_list, it)
    {
        AST namelist_item = ASTSon1(it);

        AST common_name = ASTSon0(namelist_item);
        AST namelist_group_object_list = ASTSon1(namelist_item);

        AST name = ASTSon0(common_name);

        scope_entry_t* new_namelist 
            = new_fortran_symbol(decl_context, ASTText(name));

        new_namelist->kind = SK_NAMELIST;
        new_namelist->file = ASTFileName(a);
        new_namelist->line = ASTLine(a);
        
        remove_unknown_kind_symbol(decl_context, new_namelist);

        AST it2;
        for_each_element(namelist_group_object_list, it2)
        {
            AST namelist_item_name = ASTSon1(it2);

            scope_entry_t* namelist_element = get_symbol_for_name(decl_context, namelist_item_name, ASTText(namelist_item_name));

            namelist_element->entity_specs.is_in_namelist = 1;
            namelist_element->entity_specs.namelist = new_namelist;

            P_LIST_ADD(new_namelist->entity_specs.related_symbols,
                    new_namelist->entity_specs.num_related_symbols,
                    namelist_element);
        }
    }

}

static void build_scope_nullify_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST pointer_object_list = ASTSon0(a);

    nodecl_t nodecl_expr_list = nodecl_null();
    AST it;
    for_each_element(pointer_object_list, it)
    {
        AST pointer_object = ASTSon1(it);

        nodecl_t nodecl_pointer_obj = nodecl_null();
        fortran_check_expression(pointer_object, decl_context, &nodecl_pointer_obj);

        scope_entry_t* sym = nodecl_get_symbol(nodecl_pointer_obj);

        if (!is_pointer_type(no_ref(sym->type_information)))
        {
            error_printf("%s: error: '%s' does not designate a POINTER\n",
                    ast_location(a),
                    fortran_prettyprint_in_buffer(pointer_object));
            continue;
        }

        nodecl_expr_list = nodecl_append_to_list(nodecl_expr_list, 
                nodecl_pointer_obj);
    }

    // Could we disguise this as a "x = NULL" expression?
    *nodecl_output = nodecl_make_fortran_nullify_statement(nodecl_expr_list, ASTFileName(a), ASTLine(a));
}

static void build_scope_open_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST connect_spec_list = ASTSon0(a);
    nodecl_t nodecl_opt_value = nodecl_null();
    handle_opt_value_list(a, connect_spec_list, decl_context, &nodecl_opt_value);

    *nodecl_output = nodecl_make_fortran_open_statement(nodecl_opt_value, ASTFileName(a), ASTLine(a));
}

static void build_scope_optional_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST name_list = ASTSon0(a);
    AST it;
    for_each_element(name_list, it)
    {
        AST name = ASTSon1(it);
        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        if (!entry->entity_specs.is_parameter)
        {
            error_printf("%s: error: entity '%s' is not a dummy argument\n",
                    ast_location(name),
                    ASTText(name));
            continue;
        }
        entry->entity_specs.is_optional = 1;
    }

}

static void build_scope_parameter_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST named_constant_def_list = ASTSon0(a);

    AST it;

    for_each_element(named_constant_def_list, it)
    {
        AST named_constant_def = ASTSon1(it);

        AST name = ASTSon0(named_constant_def);
        AST constant_expr = ASTSon1(named_constant_def);

        nodecl_t nodecl_constant = nodecl_null();
        fortran_check_expression(constant_expr, decl_context, &nodecl_constant);

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        if (is_void_type(no_ref(entry->type_information)))
        {
            error_printf("%s: error: unknown entity '%s' in PARAMETER statement\n",
                    ast_location(name),
                    ASTText(name));
            continue;
        }

        if (is_const_qualified_type(no_ref(entry->type_information)))
        {
            error_printf("%s: error: PARAMETER attribute is not compatible with POINTER attribute\n", 
                    ast_location(a));
            continue;
        }

        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }

        entry->type_information = get_const_qualified_type(entry->type_information);
        entry->value = nodecl_constant;
    }

}

static void build_scope_cray_pointer_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST cray_pointer_spec_list = ASTSon0(a);

    AST it;
    for_each_element(cray_pointer_spec_list, it)
    {
        AST cray_pointer_spec = ASTSon1(it);

        AST pointer_name = ASTSon0(cray_pointer_spec);
        AST pointee_decl = ASTSon1(cray_pointer_spec);

        scope_entry_t* pointer_entry = get_symbol_for_name(decl_context, pointer_name, ASTText(pointer_name));

        if (pointer_entry->kind == SK_UNDEFINED)
        {
            pointer_entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, pointer_entry);

            // This nodecl is needed only for choose_int_type_from_kind
            nodecl_t nodecl_sym = nodecl_make_symbol(pointer_entry, ASTFileName(a), ASTLine(a));
            pointer_entry->type_information = choose_int_type_from_kind(nodecl_sym, 
                    CURRENT_CONFIGURATION->type_environment->sizeof_pointer);
        }
        else if (pointer_entry->kind == SK_VARIABLE)
        {
            if (!is_integer_type(pointer_entry->type_information))
            {
                error_printf("%s: error: a Cray pointer must have integer type\n", 
                        ast_location(pointer_name));
                continue;
            }
        }
        else
        {
            error_printf("%s: error: invalid entity '%s' for Cray pointer\n",
                    ast_location(pointer_name),
                    ASTText(pointer_name));
            continue;
        }

        AST pointee_name = pointee_decl;
        AST array_spec = NULL;
        if (ASTType(pointee_decl) == AST_DIMENSION_DECL)
        {
            pointee_name = ASTSon0(pointee_decl);
            array_spec = ASTSon1(pointee_decl);
        }

        scope_entry_t* pointee_entry = get_symbol_for_name(decl_context, pointer_name, ASTText(pointee_name));

        if (pointee_entry->entity_specs.is_cray_pointee)
        {
            error_printf("%s: error: entity '%s' is already a pointee of Cray pointer '%s'\n",
                    ast_location(pointee_name),
                    pointee_entry->symbol_name,
                    pointee_entry->entity_specs.cray_pointer->symbol_name);
            continue;
        }
        if (array_spec != NULL)
        {
            if (is_fortran_array_type(no_ref(pointee_entry->type_information)))
            {
                error_printf("%s: error: entity '%s' has already a DIMENSION attribute\n",
                        ast_location(pointee_name),
                        pointee_entry->symbol_name);
                continue;
            }

            type_t* array_type = compute_type_from_array_spec(no_ref(pointee_entry->type_information), 
                    array_spec,
                    decl_context,
                    /* array_spec_kind */ NULL);
            pointee_entry->type_information = array_type;

            pointee_entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, pointer_entry);
        }

        // We would change it into a SK_VARIABLE but it could be a function, so leave it undefined
        pointee_entry->entity_specs.is_cray_pointee = 1;
        pointee_entry->entity_specs.cray_pointer = pointer_entry;
    }
}

static void build_scope_pointer_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST pointer_decl_list = ASTSon0(a);
    AST it;

    for_each_element(pointer_decl_list, it)
    {
        AST pointer_decl = ASTSon1(it);

        AST name = pointer_decl;
        AST array_spec = NULL;
        if (ASTType(pointer_decl) == AST_DIMENSION_DECL)
        {
            name = ASTSon0(pointer_decl);
            array_spec = ASTSon1(pointer_decl);
        }

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        char was_ref = is_lvalue_reference_type(entry->type_information);

        if (is_pointer_type(no_ref(entry->type_information)))
        {
            error_printf("%s: error: entity '%s' has already the POINTER attribute\n",
                    ast_location(a),
                    entry->symbol_name);
            continue;
        }

        if (is_const_qualified_type(no_ref(entry->type_information)))
        {
            error_printf("%s: error: POINTER attribute is not compatible with PARAMETER attribute\n", 
                    ast_location(a));
            continue;
        }

        if (array_spec != NULL)
        {
            if (is_fortran_array_type(no_ref(entry->type_information))
                    || is_pointer_to_fortran_array_type(no_ref(entry->type_information)))
            {
                error_printf("%s: error: entity '%s' has already a DIMENSION attribute\n",
                        ast_location(a),
                        entry->symbol_name);
                continue;
            }

            type_t* array_type = compute_type_from_array_spec(no_ref(entry->type_information), 
                    array_spec,
                    decl_context,
                    /* array_spec_kind */ NULL);
            entry->type_information = array_type;
        }

        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }

        entry->type_information = get_pointer_type(no_ref(entry->type_information));

        if (was_ref)
        {
            entry->type_information = get_lvalue_reference_type(entry->type_information);
        }
    }

}

static void build_scope_input_output_item(AST input_output_item, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    if (ASTType(input_output_item) == AST_IMPLIED_DO)
    {
        generic_implied_do_handler(input_output_item, decl_context,
                build_scope_input_output_item_list, nodecl_output);
    }
    else 
    {
        fortran_check_expression(input_output_item, decl_context, nodecl_output);
    }
}

static void build_scope_input_output_item_list(AST input_output_item_list, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST it;
    for_each_element(input_output_item_list, it)
    {
        nodecl_t nodecl_item = nodecl_null();
        build_scope_input_output_item(ASTSon1(it), decl_context, &nodecl_item);

        *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_item);
    }
}

static void opt_fmt_value(AST value, decl_context_t decl_context, nodecl_t* nodecl_output);

static void build_scope_print_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST format = ASTSon0(a);
    AST input_output_item_list = ASTSon1(a);

    nodecl_t nodecl_io_items = nodecl_null();
    build_scope_input_output_item_list(input_output_item_list, decl_context, &nodecl_io_items);

    nodecl_t nodecl_format = nodecl_null();
    opt_fmt_value(format, decl_context, &nodecl_format);

    *nodecl_output = nodecl_make_fortran_print_statement(nodecl_get_child(nodecl_format, 0), nodecl_io_items, ASTFileName(a), ASTLine(a));
}

static void build_scope_procedure_decl_stmt(AST a UNUSED_PARAMETER, decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "PROCEDURE");
}

static void build_scope_protected_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "PROTECTED");
}

static void build_scope_read_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST io_control_spec_list = ASTSon0(a);
    nodecl_t nodecl_opt_value = nodecl_null();
    handle_opt_value_list(a, io_control_spec_list, decl_context, &nodecl_opt_value);

    nodecl_t nodecl_io_items = nodecl_null();
    if (ASTSon1(a) != NULL)
    {
        build_scope_input_output_item_list(ASTSon1(a), decl_context, &nodecl_io_items);
    }

   *nodecl_output = nodecl_make_fortran_read_statement(nodecl_opt_value, nodecl_io_items, ASTFileName(a), ASTLine(a));
}

static void build_scope_return_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    if (decl_context.current_scope->related_entry == NULL
            || decl_context.current_scope->related_entry->kind != SK_FUNCTION)
    {
        error_printf("%s: error: RETURN statement not valid in this context", ast_location(a));
        return;
    }

    scope_entry_t* current_function = decl_context.current_scope->related_entry;

    AST int_expr = ASTSon1(a);
    if (int_expr != NULL)
    {
        nodecl_t nodecl_return = nodecl_null();
        fortran_check_expression(ASTSon1(a), decl_context, &nodecl_return);

        if (nodecl_is_err_expr(nodecl_return))
        {
            *nodecl_output = nodecl_return;
        }

        if (!is_void_type(function_type_get_return_type(current_function->type_information)))
        {
            error_printf("%s: error: RETURN with alternate return is only valid in a SUBROUTINE program unit\n", 
                    ast_location(a));
            return;
        }

        *nodecl_output = nodecl_make_fortran_alternate_return_statement(nodecl_return, ASTFileName(a), ASTLine(a));
    }
    else
    {
        if (is_void_type(function_type_get_return_type(current_function->type_information)))
        {
            // SUBROUTINE
            *nodecl_output = nodecl_make_return_statement(nodecl_null(), ASTFileName(a), ASTLine(a));
        }
        else
        {
            // FUNCTION
            *nodecl_output = nodecl_make_return_statement(
                    nodecl_make_symbol(function_get_result_symbol(current_function), ASTFileName(a), ASTLine(a)), 
                    ASTFileName(a), ASTLine(a));
        }
    }
}

static void build_scope_save_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST saved_entity_list = ASTSon0(a);

    AST it;

    if (saved_entity_list == NULL)
    {
        warn_printf("%s: warning: SAVE statement without saved-entity-list is not properly supported at the moment\n",
                ast_location(a));
        return;
    }

    for_each_element(saved_entity_list, it)
    {
        AST saved_entity = ASTSon1(it);

        scope_entry_t* entry = NULL;
        if (ASTType(saved_entity) == AST_COMMON_NAME)
        {
            entry = query_common_name(decl_context, ASTText(ASTSon0(saved_entity)));

            if (entry == NULL)
            {
                entry = new_common(decl_context,ASTText(ASTSon0(saved_entity)));
                entry->file = ASTFileName(a);
                entry->line = ASTLine(a);

                add_not_fully_defined_symbol(decl_context, entry);
            }
        }
        else
        {
            entry = get_symbol_for_name(decl_context, saved_entity, ASTText(saved_entity));
        }

        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }
        entry->entity_specs.is_static = 1;
    }

}

static void build_scope_select_type_construct(AST a, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "SELECT TYPE");
}

static void build_scope_stmt_function_stmt(AST a, decl_context_t decl_context, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST name = ASTSon0(a);
    AST dummy_arg_name_list = ASTSon1(a);
    AST expr = ASTSon2(a);

    scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

    entry->kind = SK_FUNCTION;
    entry->entity_specs.is_stmt_function = 1;
    remove_unknown_kind_symbol(decl_context, entry);

    int num_dummy_arguments = 0;
    if (dummy_arg_name_list != NULL)
    {
        AST it;
        for_each_element(dummy_arg_name_list, it)
        {
            AST dummy_arg_item = ASTSon1(it);
            scope_entry_t* dummy_arg = get_symbol_for_name(decl_context, dummy_arg_item, ASTText(dummy_arg_item));

            if (!is_scalar_type(no_ref(dummy_arg->type_information)))
            {
                error_printf("%s: error: dummy argument '%s' of statement function statement is not a scalar\n",
                        ast_location(dummy_arg_item),
                        fortran_prettyprint_in_buffer(dummy_arg_item));
                return;
            }

            if (dummy_arg->kind == SK_UNDEFINED)
            {
                dummy_arg->kind = SK_VARIABLE;
                remove_unknown_kind_symbol(decl_context, dummy_arg);
            }
            // This can be used latter if trying to give a nonzero rank to this
            // entity
            dummy_arg->entity_specs.is_dummy_arg_stmt_function = 1;

            P_LIST_ADD(entry->entity_specs.related_symbols,
                    entry->entity_specs.num_related_symbols,
                    dummy_arg);

            num_dummy_arguments++;
        }
    }

    // Result symbol (for consistency with remaining functions in the language)
    scope_entry_t* result_sym = calloc(1, sizeof(*result_sym));
    result_sym->symbol_name = entry->symbol_name;
    result_sym->kind = SK_VARIABLE;
    result_sym->decl_context = decl_context;
    result_sym->type_information = entry->type_information;
    result_sym->entity_specs.is_result = 1;

    P_LIST_ADD(entry->entity_specs.related_symbols,
            entry->entity_specs.num_related_symbols,
            result_sym);

    parameter_info_t parameter_info[1 + num_dummy_arguments];
    memset(parameter_info, 0, sizeof(parameter_info));

    int i;
    for (i = 0; i < num_dummy_arguments; i++)
    {
        parameter_info[i].type_info = get_indirect_type(entry->entity_specs.related_symbols[i]);
    }

    type_t* new_type = get_new_function_type(entry->type_information, 
            parameter_info, num_dummy_arguments);

    entry->type_information = new_type;

    fortran_check_expression(expr, decl_context, &entry->value);

}

static void build_scope_stop_stmt(AST a, decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    nodecl_t nodecl_stop_code = nodecl_null();
    AST stop_code = ASTSon0(a);
    if (stop_code != NULL)
    {
        fortran_check_expression(stop_code, decl_context, &nodecl_stop_code);
    }
    

    *nodecl_output = nodecl_make_fortran_stop_statement(nodecl_stop_code, ASTFileName(a), ASTLine(a));
}

static void build_scope_pause_stmt(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    nodecl_t nodecl_pause_code = nodecl_null();
    AST pause_code = ASTSon0(a);
    if (pause_code != NULL)
    {
        fortran_check_expression(pause_code, decl_context, &nodecl_pause_code);
    }
    *nodecl_output = nodecl_make_fortran_pause_statement(nodecl_pause_code, ASTFileName(a), ASTLine(a));
}

static void build_scope_sync_all_stmt(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "SYNC ALL");
}

static void build_scope_sync_images_stmt(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "SYNC IMAGES");
}

static void build_scope_sync_memory_stmt(AST a UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "SYNC MEMORY");
}

static void build_scope_target_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST target_decl_list = ASTSon0(a);

    AST it;
    for_each_element(target_decl_list, it)
    {
        AST target_decl = ASTSon1(it);

        AST name = target_decl;

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));


        if (ASTType(target_decl_list) == AST_DIMENSION_DECL)
        {
            name = ASTSon0(target_decl);
            AST array_spec = ASTSon1(target_decl_list);
            AST coarray_spec = ASTSon2(target_decl_list);

            if (coarray_spec != NULL)
            {
                running_error("%s: sorry: coarrays are not supported\n", ast_location(name));
            }

            if (array_spec != NULL)
            {
                if (is_fortran_array_type(no_ref(entry->type_information))
                        || is_pointer_to_fortran_array_type(no_ref(entry->type_information)))
                {
                    error_printf("%s: error: DIMENSION attribute specified twice for entity '%s'\n", 
                            ast_location(a),
                            entry->symbol_name);
                    continue;
                }

                char was_ref = is_lvalue_reference_type(entry->type_information);

                type_t* array_type = compute_type_from_array_spec(no_ref(entry->type_information),
                        array_spec,
                        decl_context,
                        /* array_spec_kind */ NULL);
                entry->type_information = array_type;

                if (was_ref)
                {
                    entry->type_information = get_lvalue_reference_type(entry->type_information);
                }
            }
        }

        if (entry->entity_specs.is_target)
        {
            error_printf("%s: error: entity '%s' already has TARGET attribute\n", 
                    ast_location(target_decl),
                    entry->symbol_name);
            continue;
        }

        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }
        entry->entity_specs.is_target = 1;
    }

}

static void build_scope_declaration_common_stmt(AST a, decl_context_t decl_context, 
        char is_typedef,
        nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    DEBUG_CODE()
    {
        fprintf(stderr, "== [%s] Declaration statement ==\n", ast_location(a));
    }
    AST declaration_type_spec = ASTSon0(a);
    AST attr_spec_list = ASTSon1(a);
    AST entity_decl_list = ASTSon2(a);

    type_t* basic_type = gather_type_from_declaration_type_spec(declaration_type_spec, decl_context);

    attr_spec_t attr_spec;
    memset(&attr_spec, 0, sizeof(attr_spec));
    
    if (attr_spec_list != NULL)
    {
        gather_attr_spec_list(attr_spec_list, decl_context, &attr_spec);
    }

    AST it;
    for_each_element(entity_decl_list, it)
    {
        attr_spec_t current_attr_spec = attr_spec;
        AST declaration = ASTSon1(it);

        AST name = ASTSon0(declaration);
        AST entity_decl_specs = ASTSon1(declaration);

        // Create a new symbol if it does not exist in the current scope
        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        // If entry has the same name as an intrinsic, we must change things
        scope_entry_t* entry_intrinsic = fortran_query_intrinsic_name_str(decl_context, ASTText(name));
        char can_be_an_intrinsic = (entry_intrinsic != NULL);
        if (can_be_an_intrinsic)
        {
            entry->type_information = basic_type; 
        }
        
        if (!entry->entity_specs.is_implicit_basic_type)
        {
            error_printf("%s: error: entity '%s' already has a basic type\n",
                    ast_location(name),
                    entry->symbol_name);
            continue;
        }
        // else
        // {
        //     // It was not so much defined actually...
        //     entry->defined = 0;
        // }

        if (entry->defined)
        {
            error_printf("%s: error: redeclaration of entity '%s', first declared at '%s:%d'\n",
                    ast_location(declaration),
                    entry->symbol_name,
                    entry->file,
                    entry->line);
            continue;
        }

        if (is_typedef)
        {
            if (entry->kind != SK_UNDEFINED)
            {
                running_error("%s: error: TYPEDEF would overwrite a non undefined entity\n", 
                        ast_location(declaration));
            }
            entry->kind = SK_TYPEDEF;
            remove_unknown_kind_symbol(decl_context, entry);
        }

        remove_untyped_symbol(decl_context, entry);

        entry->type_information = update_basic_type_with_type(entry->type_information, basic_type);
        entry->entity_specs.is_implicit_basic_type = 0;
        entry->entity_specs.is_implicit_but_not_function = 0;
        // entry->defined = 1;
        entry->file = ASTFileName(declaration);
        entry->line = ASTLine(declaration);

        if (entry->kind == SK_FUNCTION)
        {
            // Update the result (if any) as well
            // Note that the result variable is never found through normal lookup, it is
            // only accessible through the function symbol
            int i, num_symbols = entry->entity_specs.num_related_symbols;

            for (i = 0; i < num_symbols; i++)
            {
                scope_entry_t* sym = entry->entity_specs.related_symbols[i];
                if (sym->entity_specs.is_result)
                {
                    sym->type_information = update_basic_type_with_type(sym->type_information, basic_type);
                    sym->entity_specs.is_implicit_basic_type = 0;
                    sym->entity_specs.is_implicit_but_not_function = 0;
                    // sym->defined = 1;
                }
            }
        }

        AST array_spec = NULL;
        AST coarray_spec = NULL;
        AST char_length = NULL;
        AST initialization = NULL;
        if (entity_decl_specs != NULL)
        {
            array_spec = ASTSon0(entity_decl_specs);
            coarray_spec = ASTSon1(entity_decl_specs);
            char_length = ASTSon2(entity_decl_specs);
            initialization = ASTSon3(entity_decl_specs);
        }

        if (array_spec != NULL)
        {
            if (current_attr_spec.is_dimension)
            {
                error_printf("%s: error: DIMENSION attribute specified twice\n", ast_location(declaration));
                continue;
            }
            current_attr_spec.is_dimension = 1;
            current_attr_spec.array_spec = array_spec;
        }

        if (coarray_spec != NULL)
        {
            if (current_attr_spec.is_codimension)
            {
                error_printf("%s: error: CODIMENSION attribute specified twice\n", ast_location(declaration));
                continue;
            }
            current_attr_spec.is_codimension = 1;
            current_attr_spec.coarray_spec = coarray_spec;
        }

        if (char_length != NULL)
        {
            char was_ref = is_lvalue_reference_type(entry->type_information);
            if (ASTType(char_length) != AST_SYMBOL
                    || strcmp(ASTText(char_length), "*") != 0)
            {
                nodecl_t nodecl_char_length = nodecl_null();
                fortran_check_expression(char_length, decl_context, &nodecl_char_length);


                nodecl_t lower_bound = nodecl_make_integer_literal(
                        get_signed_int_type(),
                        const_value_get_one(type_get_size(get_signed_int_type()), 1),
                        ASTFileName(char_length), ASTLine(char_length));
                entry->type_information = get_array_type_bounds(
                        array_type_get_element_type(no_ref(entry->type_information)), 
                        lower_bound, nodecl_char_length, decl_context);

            }
            else
            {
                entry->type_information = get_array_type(
                        array_type_get_element_type(no_ref(entry->type_information)),
                        nodecl_null(), decl_context);
            }

            if (was_ref)
            {
                entry->type_information = get_lvalue_reference_type(entry->type_information);
            }
        }


        // Stop the madness here
        if (current_attr_spec.is_codimension)
        {
            error_printf("%s: sorry: coarrays are not supported\n", ast_location(declaration));
        }

        if (current_attr_spec.is_dimension)
        {
            char was_ref = is_lvalue_reference_type(entry->type_information);
            cv_qualifier_t cv_qualif = get_cv_qualifier(entry->type_information);
            type_t* array_type = compute_type_from_array_spec(no_ref(get_unqualified_type(entry->type_information)),
                    current_attr_spec.array_spec,
                    decl_context,
                    /* array_spec_kind */ NULL);

            if (!is_typedef)
            {
                // From now this entity is only a variable
                entry->kind = SK_VARIABLE;
                remove_unknown_kind_symbol(decl_context, entry);
            }
           
            entry->type_information = array_type;

            entry->type_information = get_cv_qualified_type(entry->type_information, cv_qualif);

            if (was_ref)
            {
                entry->type_information = get_lvalue_reference_type(entry->type_information);
            }
        }

        // FIXME - Should we do something with this attribute?
        if (current_attr_spec.is_value)
        {
            if (!entry->entity_specs.is_parameter)
            {
                error_printf("%s: error: VALUE attribute is only for dummy arguments\n",
                        ast_location(declaration));
                continue;
            }
            else
            {
                char was_ref = is_lvalue_reference_type(entry->type_information);
                if (!was_ref)
                {
                    error_printf("%s: error: VALUE attribute already set\n",
                            ast_location(declaration));
                }
                else
                {
                    entry->type_information = reference_type_get_referenced_type(entry->type_information);
                }
            }
        }

        if (current_attr_spec.is_intent)
        {
            if (!entry->entity_specs.is_parameter)
            {
                error_printf("%s: error: INTENT attribute is only for dummy arguments\n",
                        ast_location(declaration));
                continue;
            }
            entry->entity_specs.intent_kind = current_attr_spec.intent_kind;
        }

        if (current_attr_spec.is_optional)
        {
            if (!entry->entity_specs.is_parameter)
            {
                error_printf("%s: error: OPTIONAL attribute is only for dummy arguments\n",
                        ast_location(declaration));
                continue;
            }
            entry->entity_specs.is_optional = 1;
        }

        if (current_attr_spec.is_allocatable)
        {
            if (!current_attr_spec.is_dimension)
            {
                error_printf("%s: error: ALLOCATABLE attribute cannot be used on scalars\n", 
                        ast_location(declaration));
                continue;
            }
            entry->entity_specs.is_allocatable = 1;
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }

        if (current_attr_spec.is_intrinsic)
        {
            // Intrinsics are kept in global scope
            decl_context_t global_context = decl_context;
            global_context.current_scope = decl_context.global_scope;

            scope_entry_t* intrinsic_name = fortran_query_name_str(global_context, intrinsic_name->symbol_name);
            if (intrinsic_name == NULL
                    || !intrinsic_name->entity_specs.is_builtin)
            {
                error_printf("%s: error: name '%s' is not known as an intrinsic\n", 
                        ast_location(name),
                        ASTText(name));
            }
            else
            {
                remove_entry(entry->decl_context.current_scope, entry);
                insert_alias(entry->decl_context.current_scope, intrinsic_name, intrinsic_name->symbol_name);
                // Do nothing else otherwise we may be overwriting intrinsics
                continue;
            }
        }

        if (current_attr_spec.is_external)
        {
            entry->kind = SK_FUNCTION;
            entry->type_information = get_nonproto_function_type(entry->type_information, 0);
            remove_unknown_kind_symbol(decl_context, entry);
        }

        if (current_attr_spec.is_external)
        {
            entry->entity_specs.is_extern = 1;
        }

        if (current_attr_spec.is_save)
        {
            entry->entity_specs.is_static = 1;
        }

        if (current_attr_spec.is_pointer)
        {
            char was_ref = is_lvalue_reference_type(entry->type_information);
            entry->type_information = get_pointer_type(no_ref(entry->type_information));
            if (was_ref)
            {
                entry->type_information = get_lvalue_reference_type(entry->type_information);
            }
        }

        entry->entity_specs.is_target = current_attr_spec.is_target;

        if (initialization != NULL)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
            nodecl_t nodecl_init = nodecl_null();

            if (ASTType(initialization) == AST_POINTER_INITIALIZATION
                    && current_attr_spec.is_pointer)
            {
                initialization = ASTSon0(initialization);
                fortran_check_initialization(entry, initialization, decl_context, 
                        /* is_pointer_init */ 1,
                        &nodecl_init);
            }
            else if (current_attr_spec.is_pointer)
            {
                error_printf("%s: error: a POINTER must be initialized using pointer initialization\n",
                        ast_location(initialization));
            }
            else if (ASTType(initialization) == AST_POINTER_INITIALIZATION)
            {
                error_printf("%s: error: no POINTER attribute, required for pointer initialization\n",
                        ast_location(initialization));
            }
            else
            {
                fortran_check_initialization(entry, initialization, decl_context, 
                        /* is_pointer_init */ 0,
                        &nodecl_init);

            }
            if (!nodecl_is_err_expr(nodecl_init))
            {
                entry->value = nodecl_init;
                if (!current_attr_spec.is_constant)
                {
                    entry->entity_specs.is_static = 1;
                }
                else
                {
                    entry->type_information = get_const_qualified_type(entry->type_information);
                }
            }
        }

        if (current_attr_spec.is_pointer
                && current_attr_spec.is_constant)
        {
            error_printf("%s: error: PARAMETER attribute is not compatible with POINTER attribute\n",
                    ast_location(declaration));
        }

        if (current_attr_spec.is_constant 
                && initialization == NULL)
        {
            error_printf("%s: error: PARAMETER is missing an initializer\n", ast_location(declaration));
        }

        if (current_attr_spec.is_public
                || current_attr_spec.is_private)
        {
            if (entry->entity_specs.access != AS_UNKNOWN)
            {
                error_printf("%s: access specifier already given for entity '%s'\n",
                        ast_location(declaration),
                        entry->symbol_name);
            }
            if (current_attr_spec.is_public)
            {
                entry->entity_specs.access = AS_PUBLIC;
            }
            else if (current_attr_spec.is_private)
            {
                entry->entity_specs.access = AS_PRIVATE;
            }
        }

        DEBUG_CODE()
        {
            fprintf(stderr, "BUILDSCOPE: Type of symbol '%s' is '%s'\n", entry->symbol_name, print_declarator(entry->type_information));
        }
    }
}

static void build_scope_declaration_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    build_scope_declaration_common_stmt(a, decl_context, 
            /* is_typedef */ 0,
            nodecl_output);
}

// This is an internal Mercurium extension
static void build_scope_typedef_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    build_scope_declaration_common_stmt(a, 
            decl_context, 
            /* is_typedef */ 1,
            nodecl_output);
}

static void build_scope_unlock_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "UNLOCK");
}

static scope_entry_t* query_module_for_symbol_name(scope_entry_t* module_symbol, const char* name)
{
    scope_entry_t* sym_in_module = NULL;
    int i;
    for (i = 0; i < module_symbol->entity_specs.num_related_symbols; i++)
    {
        scope_entry_t* sym = module_symbol->entity_specs.related_symbols[i];

        if (strcasecmp(sym->symbol_name, name) == 0
                // Filter private symbols
                && sym->entity_specs.access != AS_PRIVATE)
        {
            sym_in_module = sym;
            break;
        }
    }

    return sym_in_module;
}

static char come_from_the_same_module(scope_entry_t* new_symbol_used,
        scope_entry_t* existing_symbol)
{
    // Jump all indirections through modules
    if (new_symbol_used->entity_specs.from_module != NULL)
    {
        ERROR_CONDITION(new_symbol_used->entity_specs.alias_to == NULL, "Invalid symbol", 0);
        return come_from_the_same_module(new_symbol_used->entity_specs.alias_to, 
                existing_symbol);
    }
    if (existing_symbol->entity_specs.from_module != NULL)
    {
        ERROR_CONDITION(existing_symbol->entity_specs.alias_to == NULL, "Invalid symbol", 0);
        return come_from_the_same_module(new_symbol_used, 
                existing_symbol->entity_specs.alias_to);
    }

    ERROR_CONDITION(new_symbol_used->entity_specs.in_module == NULL, "This should not happen to a symbol coming from a MODULE\n", 0);

    if (existing_symbol->entity_specs.in_module != NULL)
    {
        return (strcasecmp(new_symbol_used->entity_specs.in_module->symbol_name, 
                existing_symbol->entity_specs.in_module->symbol_name) == 0);
    }

    return 0;
}

static scope_entry_t* insert_symbol_from_module(scope_entry_t* entry, 
        decl_context_t decl_context, 
        const char* aliased_name, 
        scope_entry_t* module_symbol,
        const char* filename,
        int line)
{
    ERROR_CONDITION(aliased_name == NULL, "Invalid alias name", 0);

    scope_entry_list_t* check_repeated_name = query_name_str(decl_context, aliased_name);

    if (check_repeated_name != NULL)
    {
        scope_entry_t* existing_name = entry_list_head(check_repeated_name);
        if (come_from_the_same_module(entry, existing_name))
        {
            return existing_name;
        }
        // We allow the symbol be repeated, using it should be wrong (but this is not checked!)
    }
    entry_list_free(check_repeated_name);

    // Why do we duplicate instead of insert_entry or insert_alias?
    //
    // The reason is that we need to know this symbol comes from a module
    // and its precise USE name, not the original symbol name
    scope_entry_t* current_symbol = NULL;
    current_symbol = new_fortran_symbol(decl_context, aliased_name);

    // Copy everything and restore the name
    *current_symbol = *entry;
    if (current_symbol->kind != SK_UNDEFINED)
    {
        remove_unknown_kind_symbol(decl_context, current_symbol);
    }

    current_symbol->symbol_name = aliased_name;

    current_symbol->entity_specs.from_module = module_symbol;
    current_symbol->entity_specs.alias_to = entry;

    if (strcmp(aliased_name, entry->symbol_name) != 0)
    {
        current_symbol->entity_specs.is_renamed = 1;
    }

    // Make it a member of this module
    if (decl_context.current_scope->related_entry != NULL
            && decl_context.current_scope->related_entry->kind == SK_MODULE)
    {
        scope_entry_t* module = decl_context.current_scope->related_entry;

        P_LIST_ADD_ONCE(module->entity_specs.related_symbols,
                module->entity_specs.num_related_symbols,
                current_symbol);

        current_symbol->entity_specs.in_module = module;
    }

    if (entry->entity_specs.is_generic_spec)
    {
        // Fix what cannot be shared
        current_symbol->entity_specs.num_related_symbols = 0;
        current_symbol->entity_specs.related_symbols = NULL;

        int i;
        for (i = 0; i < entry->entity_specs.num_related_symbols; i++)
        {
            scope_entry_t* new_sym = insert_symbol_from_module(
                    entry->entity_specs.related_symbols[i],
                    decl_context,
                    entry->entity_specs.related_symbols[i]->symbol_name,
                    module_symbol,
                    filename, line);

            P_LIST_ADD(current_symbol->entity_specs.related_symbols,
                    current_symbol->entity_specs.num_related_symbols,
                    new_sym);
        }
    }

    return current_symbol;
}

static void build_scope_use_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST module_nature = NULL;
    AST module_name = NULL;
    AST rename_list = NULL;
    AST only_list = NULL;

    char is_only = 0;

    if (ASTType(a) == AST_USE_STATEMENT)
    {
        module_nature = ASTSon0(a);
        module_name = ASTSon1(a);
        rename_list = ASTSon2(a);
    }
    else if (ASTType(a) == AST_USE_ONLY_STATEMENT)
    {
        module_nature = ASTSon0(a);
        module_name = ASTSon1(a);
        only_list = ASTSon2(a);
        is_only = 1;
    }
    else
    {
        internal_error("Unexpected node %s", ast_print_node_type(ASTType(a)));
    }

    if (module_nature != NULL)
    {
        running_error("%s: error: specifying the nature of the module is not supported\n",
                ast_location(a));
    }

    const char* module_name_str = strtolower(ASTText(module_name));

    scope_entry_t* module_symbol = NULL;

    // Query first in the module cache

    DEBUG_CODE()
    {
        fprintf(stderr, "BUILDSCOPE: Loading module '%s'\n", module_name_str);
    }

    rb_red_blk_node* query = rb_tree_query(CURRENT_COMPILED_FILE->module_cache, module_name_str);
    if (query != NULL)
    {
        module_symbol = (scope_entry_t*)rb_node_get_info(query);
        DEBUG_CODE()
        {
            fprintf(stderr, "BUILDSCOPE: Module '%s' was in the module cache of this file\n", module_name_str);
        }
    }
    else
    {
        DEBUG_CODE()
        {
            fprintf(stderr, "BUILDSCOPE: Loading module '%s' from the filesystem\n", module_name_str);
        }
        // Load the file
        load_module_info(strtolower(ASTText(module_name)), &module_symbol);

        if (module_symbol == NULL)
        {
            running_error("%s: error: cannot load module '%s'\n",
                    ast_location(a),
                    module_name_str);
        }

        // And add it to the cache of opened modules
        ERROR_CONDITION(module_symbol == NULL, "Invalid symbol", 0);
        rb_tree_insert(CURRENT_COMPILED_FILE->module_cache, module_name_str, module_symbol);
    }

    insert_entry(decl_context.current_scope, module_symbol);

    if (!is_only)
    {
        int num_renamed_symbols = 0;
        scope_entry_t* renamed_symbols[MCXX_MAX_RENAMED_SYMBOLS];
        memset(renamed_symbols, 0, sizeof(renamed_symbols));

        if (rename_list != NULL)
        {
            AST it;
            for_each_element(rename_list, it)
            {
                AST rename_tree = ASTSon1(it);

                AST local_name = ASTSon0(rename_tree);
                AST sym_in_module_name = ASTSon1(rename_tree);
                const char* sym_in_module_name_str = get_name_of_generic_spec(sym_in_module_name);
                scope_entry_t* sym_in_module = 
                    query_module_for_symbol_name(module_symbol, sym_in_module_name_str);

                if (sym_in_module == NULL)
                {
                    running_error("%s: error: symbol '%s' not found in module '%s'\n", 
                            ast_location(sym_in_module_name),
                            prettyprint_in_buffer(sym_in_module_name),
                            module_symbol->symbol_name);
                }

                insert_symbol_from_module(sym_in_module, 
                        decl_context, 
                        get_name_of_generic_spec(local_name), 
                        module_symbol, 
                        ASTFileName(local_name), 
                        ASTLine(local_name));

                // "USE M, C => A, D => A" is valid so we avoid adding twice
                // 'A' in the list (it would be harmless, though)
                char found = 0;
                int i;
                for (i = 0; i < num_renamed_symbols && found; i++)
                {
                    found = (renamed_symbols[i] == sym_in_module);
                }
                if (!found)
                {
                    ERROR_CONDITION(num_renamed_symbols == MCXX_MAX_RENAMED_SYMBOLS, "Too many renames", 0);
                    renamed_symbols[num_renamed_symbols] = sym_in_module;
                    num_renamed_symbols++;
                }
            }
        }

        // Now add the ones not renamed
        int i;
        for (i = 0; i < module_symbol->entity_specs.num_related_symbols; i++)
        {
            scope_entry_t* sym_in_module = module_symbol->entity_specs.related_symbols[i];

            if (sym_in_module->entity_specs.access == AS_PRIVATE)
                continue;

            char found = 0;
            int j;
            for (j = 0; j < num_renamed_symbols && !found; j++)
            {
                found = (renamed_symbols[j] == sym_in_module);
            }
            if (!found)
            {
                insert_symbol_from_module(sym_in_module, 
                        decl_context, 
                        sym_in_module->symbol_name, 
                        module_symbol,
                        ASTFileName(a), 
                        ASTLine(a));
            }
        }
    }
    else // is_only
    {
        AST it;
        for_each_element(only_list, it)
        {
            AST only = ASTSon1(it);

            switch (ASTType(only))
            {
                case AST_RENAME:
                    {
                        AST local_name = ASTSon0(only);
                        AST sym_in_module_name = ASTSon1(only);
                        const char * sym_in_module_name_str = ASTText(sym_in_module_name);

                        scope_entry_t* sym_in_module = 
                            query_module_for_symbol_name(module_symbol, sym_in_module_name_str);

                        if (sym_in_module == NULL)
                        {
                            running_error("%s: error: symbol '%s' not found in module '%s'\n", 
                                    ast_location(sym_in_module_name),
                                    prettyprint_in_buffer(sym_in_module_name),
                                    module_symbol->symbol_name);
                        }

                        insert_symbol_from_module(sym_in_module, 
                                decl_context, 
                                get_name_of_generic_spec(local_name), 
                                module_symbol, 
                                ASTFileName(local_name), 
                                ASTLine(local_name));
                        break;
                    }
                default:
                    {
                        // This is a generic name
                        AST sym_in_module_name = only;
                        const char * sym_in_module_name_str = ASTText(sym_in_module_name);

                        scope_entry_t* sym_in_module = 
                            query_module_for_symbol_name(module_symbol, sym_in_module_name_str);

                        if (sym_in_module == NULL)
                        {
                            running_error("%s: error: symbol '%s' not found in module '%s'\n", 
                                    ast_location(sym_in_module_name),
                                    prettyprint_in_buffer(sym_in_module_name),
                                    module_symbol->symbol_name);
                        }

                        insert_symbol_from_module(sym_in_module, 
                                decl_context, 
                                sym_in_module->symbol_name, 
                                module_symbol,
                                ASTFileName(sym_in_module_name), 
                                ASTLine(sym_in_module_name));
                        break;
                    }
            }
        }
    }
}

static void build_scope_value_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST name_list = ASTSon0(a);

    AST it;
    for_each_element(name_list, it)
    {
        AST name = ASTSon1(it);

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        if (!entry->entity_specs.is_parameter)
        {
            error_printf("%s: error: entity '%s' is not a dummy argument\n",
                    ast_location(name),
                    entry->symbol_name);
            continue;
        }

        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }
        if (is_lvalue_reference_type(entry->type_information))
        {
            entry->type_information = reference_type_get_referenced_type(entry->type_information);
        }
        else
        {
            error_printf("%s: error: entity '%s' already had VALUE attribute\n",
                    ast_location(name),
                    entry->symbol_name);
        }
    }

}

static void build_scope_volatile_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    AST name_list = ASTSon0(a);

    AST it;
    for_each_element(name_list, it)
    {
        AST name = ASTSon1(it);

        scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

        if (entry->kind == SK_UNDEFINED)
        {
            entry->kind = SK_VARIABLE;
            remove_unknown_kind_symbol(decl_context, entry);
        }
        char is_ref = is_lvalue_reference_type(entry->type_information);

        if (!is_volatile_qualified_type(no_ref(entry->type_information)))
        {
            if (!is_ref)
            {
                entry->type_information = get_volatile_qualified_type(entry->type_information);
            }
            else
            {
                entry->type_information = get_lvalue_reference_type(
                        get_volatile_qualified_type(no_ref(entry->type_information)));
            }
        }
        else
        {
            error_printf("%s: error: entity '%s' already has VOLATILE attribute\n",
                    ast_location(a), entry->symbol_name);
            continue;
        }
    }

}

static void build_scope_wait_stmt(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    unsupported_statement(a, "WAIT");
}

static void build_scope_where_body_construct_seq(AST a, decl_context_t decl_context, nodecl_t* nodecl_output UNUSED_PARAMETER)
{
    if (a == NULL)
        return;

    AST it;
    for_each_element(a, it)
    {
        AST statement = ASTSon1(it);

        nodecl_t nodecl_statement = nodecl_null();
        fortran_build_scope_statement(statement, decl_context, &nodecl_statement);

        *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_statement);
    }
}

static void build_scope_mask_elsewhere_part_seq(AST mask_elsewhere_part_seq, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    if (mask_elsewhere_part_seq == NULL)
        return;

    AST it;
    for_each_element(mask_elsewhere_part_seq, it)
    {
        AST mask_elsewhere_part = ASTSon1(it);

        AST masked_elsewhere_stmt = ASTSon0(mask_elsewhere_part);
        AST where_body_construct_seq = ASTSon1(mask_elsewhere_part);

        AST expr = ASTSon0(masked_elsewhere_stmt);
        nodecl_t nodecl_expr = nodecl_null();
        fortran_check_expression(expr, decl_context, &nodecl_expr);

        nodecl_t nodecl_statement = nodecl_null();
        build_scope_where_body_construct_seq(where_body_construct_seq, decl_context, &nodecl_statement);

        *nodecl_output = nodecl_append_to_list(*nodecl_output,
                nodecl_make_fortran_where_pair(
                    nodecl_expr,
                    nodecl_statement,
                    ASTFileName(expr),
                    ASTLine(expr)));
    }
}

static void build_scope_where_construct(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST where_construct_stmt = ASTSon0(a);
    AST mask_expr = ASTSon1(where_construct_stmt);
    nodecl_t nodecl_mask_expr = nodecl_null();
    fortran_check_expression(mask_expr, decl_context, &nodecl_mask_expr);
    
    AST where_construct_body = ASTSon1(a);


    AST main_where_body = ASTSon0(where_construct_body);
    nodecl_t nodecl_body = nodecl_null();
    build_scope_where_body_construct_seq(main_where_body, decl_context, &nodecl_body);

    nodecl_t nodecl_where_parts = nodecl_make_list_1( nodecl_make_fortran_where_pair(
                nodecl_mask_expr,
                nodecl_body, ASTFileName(a), ASTLine(a)));

    AST mask_elsewhere_part_seq = ASTSon1(where_construct_body);
    nodecl_t nodecl_elsewhere_parts = nodecl_null();
    build_scope_mask_elsewhere_part_seq(mask_elsewhere_part_seq, decl_context, &nodecl_elsewhere_parts);

    nodecl_where_parts = nodecl_concat_lists(nodecl_where_parts, nodecl_elsewhere_parts);
    
    // Do nothing with elsewhere_stmt ASTSon2(where_construct_body)

    AST elsewhere_body = ASTSon3(where_construct_body);

    if (elsewhere_body != NULL)
    {
        nodecl_t nodecl_elsewhere_body = nodecl_null();
        build_scope_where_body_construct_seq(elsewhere_body, decl_context, &nodecl_elsewhere_body);
        nodecl_where_parts = nodecl_concat_lists(nodecl_where_parts,
            nodecl_make_list_1(nodecl_make_fortran_where_pair(nodecl_null(), nodecl_elsewhere_body, ASTFileName(a), ASTLine(a))));
    }


    *nodecl_output = nodecl_make_fortran_where(nodecl_where_parts, ASTFileName(a), ASTLine(a));
}

static void build_scope_where_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST mask_expr = ASTSon0(a);
    nodecl_t nodecl_mask_expr = nodecl_null();
    fortran_check_expression(mask_expr, decl_context, &nodecl_mask_expr);
    AST where_assignment_stmt = ASTSon1(a);
    nodecl_t nodecl_expression = nodecl_null();
    build_scope_expression_stmt(where_assignment_stmt, decl_context, &nodecl_expression);

    *nodecl_output = nodecl_make_fortran_where(
            nodecl_make_list_1(
                nodecl_make_fortran_where_pair(
                    nodecl_mask_expr,
                    nodecl_make_list_1(nodecl_expression),
                    ASTFileName(a), ASTLine(a))),
            ASTFileName(a),
            ASTLine(a));
}

static void build_scope_while_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST expr = ASTSon0(a);
    AST block = ASTSon1(a);

    nodecl_t nodecl_expr = nodecl_null();
    fortran_check_expression(expr, decl_context, &nodecl_expr);

    if (!is_bool_type(no_ref(nodecl_get_type(nodecl_expr))))
    {
        error_printf("%s: error: condition of DO WHILE loop is not a logical expression\n",
                ast_location(expr));
    }

    nodecl_t nodecl_statement = nodecl_null();
    fortran_build_scope_statement(block, decl_context, &nodecl_statement);


    *nodecl_output = nodecl_make_while_statement(nodecl_expr,
            nodecl_statement, 
            ASTFileName(a), ASTLine(a));
}

static void build_scope_write_stmt(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_opt_value = nodecl_null();
    handle_opt_value_list(a, ASTSon0(a), decl_context, &nodecl_opt_value);

    nodecl_t nodecl_io_items = nodecl_null();
    AST input_output_item_list = ASTSon1(a);
    if (input_output_item_list != NULL)
    {
        build_scope_input_output_item_list(input_output_item_list, decl_context, &nodecl_io_items);
    }

   *nodecl_output = nodecl_make_fortran_write_statement(nodecl_opt_value, nodecl_io_items, ASTFileName(a), ASTLine(a));
}

void fortran_build_scope_statement_pragma(AST a, 
        decl_context_t decl_context, 
        nodecl_t* nodecl_output, 
        void* info UNUSED_PARAMETER)
{
    fortran_build_scope_statement(a, decl_context, nodecl_output);
}

static void build_scope_pragma_custom_ctr(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_pragma_line = nodecl_null();
    common_build_scope_pragma_custom_statement(a, decl_context, nodecl_output, &nodecl_pragma_line, fortran_build_scope_statement_pragma, NULL);
}

static void build_scope_pragma_custom_dir(AST a, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // Do nothing for directives
    common_build_scope_pragma_custom_directive(a, decl_context, nodecl_output);
}

static void build_scope_unknown_pragma(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    *nodecl_output = 
        nodecl_make_unknown_pragma(ASTText(a), ASTFileName(a), ASTLine(a));
}

static void build_scope_statement_placeholder(AST a, decl_context_t decl_context UNUSED_PARAMETER, nodecl_t* nodecl_output)
{
    check_statement_placeholder(a, decl_context, nodecl_output);
}

typedef void opt_value_fun_handler_t(AST io_stmt, AST opt_value, decl_context_t, nodecl_t*);

typedef struct opt_value_map_tag
{
    const char* name;
    opt_value_fun_handler_t *handler;
} opt_value_map_t;

#define OPT_VALUE_LIST \
  OPT_VALUE(access) \
  OPT_VALUE(acquired) \
  OPT_VALUE(action) \
  OPT_VALUE(advance) \
  OPT_VALUE(asynchronous) \
  OPT_VALUE(blank) \
  OPT_VALUE(decimal) \
  OPT_VALUE(delim) \
  OPT_VALUE(direct) \
  OPT_VALUE(encoding) \
  OPT_VALUE(eor) \
  OPT_VALUE(err) \
  OPT_VALUE(end) \
  OPT_VALUE(errmsg) \
  OPT_VALUE(exist) \
  OPT_VALUE(file) \
  OPT_VALUE(fmt) \
  OPT_VALUE(form) \
  OPT_VALUE(formatted) \
  OPT_VALUE(id) \
  OPT_VALUE(iomsg) \
  OPT_VALUE(iostat) \
  OPT_VALUE(iolength) \
  OPT_VALUE(mold) \
  OPT_VALUE(named) \
  OPT_VALUE(newunit) \
  OPT_VALUE(nextrec) \
  OPT_VALUE(nml) \
  OPT_VALUE(number) \
  OPT_VALUE(opened) \
  OPT_VALUE(pad) \
  OPT_VALUE(pending) \
  OPT_VALUE(pos) \
  OPT_VALUE(position) \
  OPT_VALUE(read) \
  OPT_VALUE(readwrite) \
  OPT_VALUE(rec) \
  OPT_VALUE(recl) \
  OPT_VALUE(round) \
  OPT_VALUE(sequential) \
  OPT_VALUE(sign) \
  OPT_VALUE(size) \
  OPT_VALUE(source) \
  OPT_VALUE(stat) \
  OPT_VALUE(status) \
  OPT_VALUE(stream) \
  OPT_VALUE(unformatted) \
  OPT_VALUE(unit) \
  OPT_VALUE(write) \
  OPT_VALUE(ambiguous_io_spec) 

#define OPT_VALUE(_name) \
     static opt_value_fun_handler_t opt_##_name##_handler;
OPT_VALUE_LIST
#undef OPT_VALUE

static opt_value_map_t opt_value_map[] =
{
#define OPT_VALUE(_name) \
     { #_name, opt_##_name##_handler },
    OPT_VALUE_LIST
#undef OPT_VALUE
};

static char opt_value_list_init = 0;

static int opt_value_map_compare(const void* v1, const void* v2)
{
    const opt_value_map_t* p1 = (const opt_value_map_t*) v1;
    const opt_value_map_t* p2 = (const opt_value_map_t*) v2;

    return strcasecmp(p1->name, p2->name);
}

static void handle_opt_value(AST io_stmt, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    opt_value_map_t key;
    key.name = ASTText(opt_value);

    ERROR_CONDITION(key.name == NULL, "Invalid opt_value without name of opt", 0);

    opt_value_map_t *elem =
        (opt_value_map_t*)bsearch(&key, opt_value_map, 
                sizeof(opt_value_map) / sizeof(opt_value_map[1]),
                sizeof(opt_value_map[0]),
                opt_value_map_compare);

    ERROR_CONDITION(elem == NULL, "Invalid opt-value '%s' at %s\n", key.name, ast_location(opt_value));
    ERROR_CONDITION(elem->handler == NULL, "Invalid handler for opt-value '%s'\n", key.name);

    (elem->handler)(io_stmt, opt_value, decl_context, nodecl_output);
}

static void handle_opt_value_list(AST io_stmt, AST opt_value_list, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    if (!opt_value_list_init)
    {
        qsort(opt_value_map, 
                sizeof(opt_value_map) / sizeof(opt_value_map[1]),
                sizeof(opt_value_map[0]),
                opt_value_map_compare);
        opt_value_list_init = 1;
    }

    if (opt_value_list == NULL)
        return;

    AST it;
    for_each_element(opt_value_list, it)
    {
        AST opt_value = ASTSon1(it);
        nodecl_t nodecl_opt_value = nodecl_null();
        handle_opt_value(io_stmt, opt_value, decl_context, &nodecl_opt_value);
        *nodecl_output = nodecl_append_to_list(*nodecl_output, nodecl_opt_value);
    }
}

static char opt_common_int_expr(AST value, decl_context_t decl_context, const char* opt_name, nodecl_t* nodecl_value)
{
    fortran_check_expression(value, decl_context, nodecl_value);
    if (!is_integer_type(no_ref(nodecl_get_type(*nodecl_value)))
            && !(is_pointer_type(no_ref(nodecl_get_type(*nodecl_value)))
                && is_integer_type(pointer_type_get_pointee_type(no_ref(nodecl_get_type(*nodecl_value))))))
    {
        error_printf("%s: error: specifier %s requires a character expression\n",
                ast_location(value),
                opt_name);
        return 0;
    }
    return 1;
}

static char opt_common_character_expr(AST value, decl_context_t decl_context, const char* opt_name, nodecl_t* nodecl_value)
{
    fortran_check_expression(value, decl_context, nodecl_value);
    if (!is_fortran_character_type(no_ref(nodecl_get_type(*nodecl_value)))
            && !is_pointer_to_fortran_character_type(no_ref(nodecl_get_type(*nodecl_value))))
    {
        error_printf("%s: error: specifier %s requires a character expression\n",
                ast_location(value),
                opt_name);
        return 0;
    }
    return 1;
}

static char opt_common_const_character_expr(AST value, decl_context_t decl_context, const char* opt_name, nodecl_t* nodecl_value)
{
    return opt_common_character_expr(value, decl_context, opt_name, nodecl_value);
}

static char opt_common_int_variable(AST value, decl_context_t decl_context, const char* opt_name, nodecl_t* nodecl_value)
{
    fortran_check_expression(value, decl_context, nodecl_value);
    if (nodecl_get_symbol(*nodecl_value) == NULL)
    { 
        scope_entry_t* sym = nodecl_get_symbol(*nodecl_value);
        if (sym == NULL
                || (!is_integer_type(no_ref(sym->type_information))
                    && !(is_pointer_type(no_ref(sym->type_information))
                        && is_integer_type(pointer_type_get_pointee_type(no_ref(sym->type_information))))))
        {
            error_printf("%s: error: specifier %s requires an integer variable\n",
                    ast_location(value),
                    opt_name);
            return 0;
        }
    }
    return 1;
}

static char opt_common_logical_variable(AST value, decl_context_t decl_context, const char* opt_name, nodecl_t* nodecl_value)
{
    fortran_check_expression(value, decl_context, nodecl_value);
    if (nodecl_get_symbol(*nodecl_value) == NULL)
    { 
        scope_entry_t* sym = nodecl_get_symbol(*nodecl_value);
        if (sym == NULL
                || (!is_bool_type(no_ref(sym->type_information))
                    && !(is_pointer_type(no_ref(sym->type_information))
                        && is_bool_type(pointer_type_get_pointee_type(no_ref(sym->type_information))))))
        {
            error_printf("%s: error: specifier %s requires a logical variable\n",
                    ast_location(value),
                    opt_name);
            return 0;
        }
    }
    return 1;
}

static void opt_access_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "ACCESS", &nodecl_value);

    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ACCESS", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_acquired_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    fortran_check_expression(value, decl_context, &nodecl_value);
    if (nodecl_get_symbol(nodecl_value) == NULL
            || !is_bool_type(no_ref(nodecl_get_symbol(nodecl_value)->type_information)))
    {
        error_printf("%s: error: specifier 'ACQUIRED LOCK' requires a logical variable\n",
                ast_location(value));
    }
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ACQUIRED LOCK", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_action_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "ACTION", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ACTION", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_advance_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    nodecl_t nodecl_value = nodecl_null();
    AST value = ASTSon0(opt_value);
    opt_common_const_character_expr(value, decl_context, "ADVANCE", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ADVANCE", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_asynchronous_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "ASYNCHRONOUS", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ASYNCHRONOUS", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_blank_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "BLANK", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "BLANK", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_decimal_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "DECIMAL", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "DECIMAL", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_delim_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "DELIM", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "DELIM", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_direct_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "DIRECT", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "DIRECT", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_encoding_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "ENCODING", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ENCODING", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_eor_handler(AST io_stmt UNUSED_PARAMETER, 
        AST opt_value UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output)
{
    AST label = ASTSon0(opt_value);
    scope_entry_t* entry = fortran_query_label(label, decl_context, /* is_definition */ 0);
    *nodecl_output = nodecl_make_fortran_io_spec(
            nodecl_make_symbol(entry, ASTFileName(label), ASTLine(label)), 
            "EOR", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_err_handler(AST io_stmt UNUSED_PARAMETER, 
        AST opt_value UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output)
{
    AST label = ASTSon0(opt_value);
    scope_entry_t* entry = fortran_query_label(label, decl_context, /* is_definition */ 0);
    *nodecl_output = nodecl_make_fortran_io_spec(
            nodecl_make_symbol(entry, ASTFileName(label), ASTLine(label)), 
            "ERR", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_end_handler(AST io_stmt UNUSED_PARAMETER, 
        AST opt_value UNUSED_PARAMETER, 
        decl_context_t decl_context UNUSED_PARAMETER, 
        nodecl_t* nodecl_output)
{
    AST label = ASTSon0(opt_value);
    scope_entry_t* entry = fortran_query_label(label, decl_context, /* is_definition */ 0);
    *nodecl_output = nodecl_make_fortran_io_spec(
            nodecl_make_symbol(entry, ASTFileName(label), ASTLine(label)), 
            "END", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_errmsg_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, 
        nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "ERRMSG", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ERRMSG", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_exist_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_logical_variable(value, decl_context, "EXIST", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "EXIST", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_file_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "FILE", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "FILE", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_fmt_value(AST value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    if (!(ASTType(value) == AST_SYMBOL
                && strcmp(ASTText(value), "*") == 0))
    {
        nodecl_t nodecl_value = nodecl_null();
        fortran_check_expression(value, decl_context, &nodecl_value);

        type_t* t = nodecl_get_type(nodecl_value);

        char valid = 1;
        if (ASTType(value) != AST_DECIMAL_LITERAL)
        {
            if (!is_fortran_character_type(no_ref(t)))
            {
                valid = 0;
            }
        }
        else
        {
            scope_entry_t* entry = fortran_query_label(value, decl_context, /* is_definition */ 0);
            if (entry == NULL)
            {
                valid = 0;
            }
            else
            {
                nodecl_value = nodecl_make_symbol(entry, ASTFileName(value), ASTLine(value));
            }
        }

        if (!valid)
        {
            error_printf("%s: error: specifier FMT requires a character expression or a label of a FORMAT statement\n",
                    ast_location(value));
        }
        *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "FMT", ASTFileName(value), ASTLine(value));
    }
    else
    {
        *nodecl_output = nodecl_make_fortran_io_spec(
                nodecl_make_text("*", ASTFileName(value), ASTLine(value)), 
                "FMT", ASTFileName(value), ASTLine(value));
    }
}

static void opt_fmt_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    opt_fmt_value(value, decl_context, nodecl_output);
}

static void opt_form_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "FORM", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "FORM", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_formatted_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "FORMATTED", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "FORMATTED", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_id_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_expr(value, decl_context, "ID", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ID", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_iomsg_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "IOMSG", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "IOMSG", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_iostat_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_expr(value, decl_context, "IOSTAT", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "IOSTAT", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_iolength_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_variable(value, decl_context, "IOLENGTH", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "IOLENGTH", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_mold_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    fortran_check_expression(value, decl_context, &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "MOLD", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_named_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_logical_variable(value, decl_context, "NAMED", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "NAMED", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_newunit_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_variable(value, decl_context, "NEWUNIT", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "NEWUNIT", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_nextrec_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_variable(value, decl_context, "NEXTREC", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "NEXTREC", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_nml_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    scope_entry_t* entry = fortran_query_name_str(decl_context, ASTText(value));
    if (entry == NULL
            || entry->kind != SK_NAMELIST)
    {
        error_printf("%s: error: entity '%s' in NML specifier is not a namelist\n",
                ast_location(value),
                ASTText(value));
    }
    *nodecl_output = nodecl_make_fortran_io_spec(
            nodecl_make_symbol(entry, ASTFileName(value), ASTLine(value)), 
            "NML", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_number_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_variable(value, decl_context, "NUMBER", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "NUMBER", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_opened_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_logical_variable(value, decl_context, "OPENED", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "OPENED", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_pad_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "PAD", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "PAD", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_pending_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_logical_variable(value, decl_context, "PENDING", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "PENDING", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_pos_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_expr(value, decl_context, "POS", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "POS", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_position_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "POSITION", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "POSITION", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_read_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "READ", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "READ", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_readwrite_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "READWRITE", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "READWRITE", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_rec_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_expr(value, decl_context, "REC", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "REC", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_recl_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_expr(value, decl_context, "RECL", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "RECL", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_round_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "ROUND", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "ROUND", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_sequential_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "SEQUENTIAL", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "SEQUENTIAL", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_sign_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "SIGN", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "SIGN", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_size_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_expr(value, decl_context, "SIZE", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "SIZE", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_source_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    fortran_check_expression(value, decl_context, &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "SOURCE", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_stat_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_int_variable(value, decl_context, "STAT", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "STAT", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_status_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "STATUS", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "STATUS", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_stream_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "STREAM", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "STREAM", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_unformatted_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "UNFORMATTED", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "UNFORMATTED", ASTFileName(opt_value), ASTLine(opt_value));
}

static void opt_unit_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    // If it is not 'UNIT = *'
    if (!(ASTType(value) == AST_SYMBOL
                && strcmp(ASTText(value), "*") == 0))
    {
        nodecl_t nodecl_value = nodecl_null();
        fortran_check_expression(value, decl_context, &nodecl_value);

        type_t* t = nodecl_get_type(nodecl_value);
        if (!(is_integer_type(no_ref(t))
                    || (nodecl_get_symbol(nodecl_value) != NULL
                        && is_fortran_character_type_or_pointer_to(no_ref(t)))))
        {
            error_printf("%s: error: specifier UNIT requires a character variable or a scalar integer expression\n",
                    ast_location(value));
        }
        *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "UNIT", ASTFileName(opt_value), ASTLine(opt_value));
    }
    else
    {
        *nodecl_output = nodecl_make_fortran_io_spec(
                nodecl_make_text("*", ASTFileName(value), ASTLine(value)),
                "UNIT", ASTFileName(opt_value), ASTLine(opt_value));
    }
}

static void opt_write_handler(AST io_stmt UNUSED_PARAMETER, AST opt_value, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    AST value = ASTSon0(opt_value);
    nodecl_t nodecl_value = nodecl_null();
    opt_common_character_expr(value, decl_context, "WRITE", &nodecl_value);
    *nodecl_output = nodecl_make_fortran_io_spec(nodecl_value, "WRITE", ASTFileName(opt_value), ASTLine(opt_value));
}

static int get_position_in_io_spec_list(AST value)
{
    AST list = ASTParent(value);

    int n = 0;
    AST it;

    for_each_element(list, it)
    {
        n++;
    }

    return n;
}

static void opt_ambiguous_io_spec_handler(AST io_stmt, AST opt_value_ambig, decl_context_t decl_context, nodecl_t* nodecl_output)
{
    // This ambiguous io spec handler exists because of the definition of io-control-spec
    //
    // io-control-spec -> [ UNIT = ] io-unit
    //                    [ FMT = ] format
    //                    [ NML = ] namelist-group-name
    //
    // Based on the following constraints we should be able to disambiguate
    //
    //   a) io-unit without UNIT = should be in the first position of the
    //      io-control-spec-list
    //   b) format without FMT = should be in the second position of the
    //      io-control-spec-list and the first shall be a io-unit
    //   c) namelist-group-name withouth NML = should be in the second position
    //      of the io-control-spec-list and the first shall be a io-unit
    //   d) it is not valid to specify both a format and a namelist-group-name
    //
    // A io-unit must be an scalar-int-expression, a character-variable or a '*'
    // A format must be a default-character-expression, a label or a '*'

    int io_unit_option = -1;
    int namelist_option = -1;
    int format_option = -1;

    int i;
    for (i = 0; i < ast_get_num_ambiguities(opt_value_ambig); i++)
    {
        AST option = ast_get_ambiguity(opt_value_ambig, i);
        const char* t = ASTText(option);
        ERROR_CONDITION((t == NULL), "io-spec is missing text", 0);


        int *p = NULL;
        if (strcasecmp(t, "unit") == 0)
        {
            p = &io_unit_option;
        }
        else if (strcasecmp(t, "fmt") == 0)
        {
            p = &format_option;
        }
        else if (strcasecmp(t, "nml") == 0)
        {
            p = &namelist_option;
        }
        else
        {
            internal_error("%s: Unexpected opt_value_ambig io-spec '%s'\n", ast_location(option), t);
        }

        ERROR_CONDITION(*p >= 0, "%s Repeated ambiguity tree!", ast_location(option));

        *p = i;
    }

    int position = get_position_in_io_spec_list(opt_value_ambig);

    char bad = 0;
    // First item
    if (position == 1)
    {
        if (io_unit_option < 0)
        {
            bad = 1;
        }
        else
        {
            // Force a io-unit
            ast_replace_with_ambiguity(opt_value_ambig, io_unit_option);
        }
    }
    // Second item
    else if (position == 2)
    {
        // We should check that the first one was a io-unit
        AST parent = ASTParent(opt_value_ambig);
        AST previous = ASTSon0(parent);
        AST io_spec = ASTSon1(previous);

        if ((ASTText(io_spec) == NULL)
                || (strcasecmp(ASTText(io_spec), "unit") != 0))
        {
            bad = 1;
        }
        else
        {
            if (namelist_option < 0)
            {
                // This can be only a FMT
                if (format_option < 0)
                {
                    bad = 1;
                }
                else
                {
                    ast_replace_with_ambiguity(opt_value_ambig, format_option);
                }
            }
            else
            {
                // Start checking if it is a real NML
                AST nml_io_spec = ast_get_ambiguity(opt_value_ambig, namelist_option);
                AST value = ASTSon0(nml_io_spec);

                scope_entry_t* entry = fortran_query_name_str(decl_context, ASTText(value));

                if (entry == NULL
                        || entry->kind != SK_NAMELIST)
                {
                    // This must be a FMT
                    if (format_option < 0)
                    {
                        bad = 1;
                    }
                    else
                    {
                        ast_replace_with_ambiguity(opt_value_ambig, format_option);
                    }
                }
                else
                {
                    // This is a NML
                    ast_replace_with_ambiguity(opt_value_ambig, namelist_option);
                }
            }
        }
    }
    else
    {
        bad = 1;
    }

    if (!bad)
    {
        // Not opt_value_ambig anymore
        handle_opt_value(io_stmt, opt_value_ambig, decl_context, nodecl_output);
    }
    else
    {
        error_printf("%s: error: invalid io-control-spec '%s'\n", 
                ast_location(opt_value_ambig),
                fortran_prettyprint_in_buffer(opt_value_ambig));
        *nodecl_output = nodecl_make_err_expr(ASTFileName(opt_value_ambig), ASTLine(opt_value_ambig));
    }
}

static char check_statement_function_statement(AST stmt, decl_context_t decl_context)
{
    // F (X) = Y
    AST name = ASTSon0(stmt);
    AST dummy_arg_name_list = ASTSon1(stmt);
    AST expr = ASTSon2(stmt);

    scope_entry_t* entry = get_symbol_for_name(decl_context, name, ASTText(name));

    if (!is_scalar_type(no_ref(entry->type_information)))
        return 0;

    if (dummy_arg_name_list != NULL)
    {
        AST it;
        for_each_element(dummy_arg_name_list, it)
        {
            AST dummy_name = ASTSon1(it);
            scope_entry_t* dummy_arg = get_symbol_for_name(decl_context, dummy_name, ASTText(dummy_name));

            if (!is_scalar_type(no_ref(dummy_arg->type_information)))
                return 0;
        }
    }

    enter_test_expression();
    nodecl_t nodecl_dummy = nodecl_null();
    fortran_check_expression(expr, decl_context, &nodecl_dummy);
    leave_test_expression();

    if (nodecl_is_err_expr(nodecl_dummy))
        return 0;

    return 1;
}

static void build_scope_ambiguity_statement(AST ambig_stmt, decl_context_t decl_context)
{
    ERROR_CONDITION(ASTType(ambig_stmt) != AST_AMBIGUITY, "Invalid tree %s\n", ast_print_node_type(ASTType(ambig_stmt)));
    ERROR_CONDITION(strcmp(ASTText(ambig_stmt), "ASSIGNMENT") != 0, "Invalid ambiguity", 0);

    int num_ambig = ast_get_num_ambiguities(ambig_stmt);
    int i;

    int result = -1;
    int index_expr = -1;

    for (i = 0; i < num_ambig; i++)
    {
        AST stmt = ast_get_ambiguity(ambig_stmt, i);

        if (ASTType(stmt) == AST_LABELED_STATEMENT)
            stmt = ASTSon1(stmt);

        char ok = 0;
        switch (ASTType(stmt))
        {
            case AST_EXPRESSION_STATEMENT:
                {
                    enter_test_expression();
                    index_expr = i;
                    nodecl_t nodecl_dummy = nodecl_null();
                    fortran_check_expression(ASTSon0(stmt), decl_context, &nodecl_dummy);
                    ok = !nodecl_is_err_expr(nodecl_dummy);
                    leave_test_expression();
                    break;
                }
            case AST_STATEMENT_FUNCTION_STATEMENT:
                {
                    ok = check_statement_function_statement(stmt, decl_context);
                    break;
                }
            default:
                {
                    internal_error("Invalid node '%s' at %s\n", ast_print_node_type(ASTType(ambig_stmt)), ast_location(ambig_stmt));
                }
        }

        if (ok)
        {
            if (result == -1)
            {
                result = i;
            }
            else
            {
                // It means that this could not be actually disambiguated
                result = -2;
            }
        }
    }

    ERROR_CONDITION(index_expr < 0, "Something is utterly broken in this ambiguous node\n", 0);

    if (result < 0)
    {
        // Default to an expression since 99% of times is what people meant
        ast_replace_with_ambiguity(ambig_stmt, index_expr);
    }
    else
    {
        ast_replace_with_ambiguity(ambig_stmt, result);
    }
}

scope_entry_t* function_get_result_symbol(scope_entry_t* entry)
{
    scope_entry_t* result = entry;

    int i;
    for (i = 0; i < entry->entity_specs.num_related_symbols; i++)
    {
        if (entry->entity_specs.related_symbols[i]->entity_specs.is_result)
        {
            result = entry->entity_specs.related_symbols[i];
            break;
        }
    }

    return result;
}
