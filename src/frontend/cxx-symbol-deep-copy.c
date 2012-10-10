#include "cxx-symbol-deep-copy.h"
#include "cxx-nodecl-deep-copy.h"
#include "cxx-typeutils.h"
#include "cxx-utils.h"

// This function is machine generated in cxx-symbol-deep-copy-entity-specs.c
extern void symbol_deep_copy_entity_specs(scope_entry_t* dest,
        scope_entry_t* source,
        decl_context_t new_decl_context,
        symbol_map_t* symbol_map);

void symbol_deep_copy(scope_entry_t* dest, 
        scope_entry_t* source,
        decl_context_t new_decl_context,
        symbol_map_t* symbol_map)
{
    ERROR_CONDITION(source->kind == SK_CLASS, "Local class replication not implemented yet", 0);
    ERROR_CONDITION(source->kind == SK_NAMESPACE, "Namespaces should not be replicated!", 0);

    // Note that context is not copied, thus this symbol should already have a
    // meaningful context for itself
    dest->kind = source->kind;

    dest->defined = source->defined;

    dest->type_information = type_deep_copy(source->type_information, new_decl_context, symbol_map);

    dest->related_decl_context = source->related_decl_context;

    dest->value = nodecl_deep_copy(source->value, new_decl_context, symbol_map);

    dest->file = source->file;
    dest->line = source->line;

    dest->do_not_print = source->do_not_print;

    symbol_deep_copy_entity_specs(dest, source, new_decl_context, symbol_map);

    if (dest->kind == SK_FUNCTION)
    {
        int i;
        for (i = 0; i < dest->entity_specs.num_related_symbols; i++)
        {
            scope_entry_t* current_symbol = dest->entity_specs.related_symbols[i];
            if (!current_symbol->entity_specs.is_result)
            {
                symbol_set_as_parameter_of_function(current_symbol, dest,
                        symbol_get_parameter_position_in_function(source->entity_specs.related_symbols[i], source));
            }
        }
    }

    // FIXME - Not copying extended data. There is no way to reliably copy such information
}
