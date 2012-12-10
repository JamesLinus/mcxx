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

#include <errno.h>

#include "tl-devices.hpp"
#include "tl-compilerpipeline.hpp"
#include "tl-multifile.hpp"
#include "tl-source.hpp"
#include "codegen-phase.hpp"
#include "codegen-cxx.hpp"
#include "cxx-cexpr.h"
#include "cxx-driver-utils.h"
#include "cxx-process.h"
#include "cxx-cexpr-fwd.h"

#include "nanox-fpga.hpp"

#include "cxx-nodecl.h"
#include "cxx-graphviz.h"

#include "tl-nanos.hpp"



using namespace TL;
using namespace TL::Nanox;

const std::string DeviceFPGA::HLS_VPREF = "_hls_var_";
const std::string DeviceFPGA::HLS_I = HLS_VPREF + "i";
const std::string DeviceFPGA::hls_in = HLS_VPREF + "in";
const std::string DeviceFPGA::hls_out = HLS_VPREF + "out";


static std::string fpga_outline_name(const std::string &name)
{
    return "_fpga_" + name;
}

static void print_ast_dot(const Nodecl::NodeclBase &node)
{
    std::cerr << std::endl << std::endl;
    ast_dump_graphviz(nodecl_get_ast(node.get_internal_nodecl()), stderr);
    std::cerr << std::endl << std::endl;
}

void DeviceFPGA::create_outline(CreateOutlineInfo &info,
        Nodecl::NodeclBase &outline_placeholder,
        Nodecl::NodeclBase &output_statements,
        Nodecl::Utils::SymbolMap* &symbol_map)
{
    if (IS_FORTRAN_LANGUAGE)
        running_error("Fortran for FPGA devices is not supported yet\n", 0);

    // Unpack DTO
    const std::string& device_outline_name = fpga_outline_name(info._outline_name);
    const Nodecl::NodeclBase& original_statements = info._original_statements;
    const TL::Symbol& arguments_struct = info._arguments_struct;
    const TL::Symbol& called_task = info._called_task;

    TL::Symbol current_function =
        original_statements.retrieve_context().get_decl_context().current_scope->related_entry;

    if (current_function.is_nested_function())
    {
        if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
            running_error("%s: error: nested functions are not supported\n",
                    original_statements.get_locus().c_str());
    }

    const TL::Scope & called_scope = called_task.get_scope();
    Source unpacked_arguments, private_entities;

    TL::ObjectList<OutlineDataItem*> data_items = info._data_items;
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        switch ((*it)->get_sharing())
        {
            case OutlineDataItem::SHARING_PRIVATE:
                {
                    break;
                }
            case OutlineDataItem::SHARING_SHARED:
            case OutlineDataItem::SHARING_CAPTURE:
            case OutlineDataItem::SHARING_CAPTURE_ADDRESS:
                {
                    TL::Type param_type = (*it)->get_in_outline_type();

                    Source argument;
                    if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
                    {
                        // Normal shared items are passed by reference from a pointer,
                        // derreference here
                        if ((*it)->get_sharing() == OutlineDataItem::SHARING_SHARED
                                && !(IS_CXX_LANGUAGE && (*it)->get_symbol().get_name() == "this"))
                        {
                            argument << "*(args." << (*it)->get_field_name() << ")";
                        }
                        // Any other thing is passed by value
                        else
                        {
                            argument << "args." << (*it)->get_field_name();
                        }

                        if (IS_CXX_LANGUAGE
                                && (*it)->get_allocation_policy() == OutlineDataItem::ALLOCATION_POLICY_TASK_MUST_DESTROY)
                        {
                            internal_error("Not yet implemented: call the destructor", 0);
                        }
                    }
                    else
                    {
                        internal_error("running error", 0);
                    }

                    unpacked_arguments.append_with_separator(argument, ", ");
                    break;
                }
            case OutlineDataItem::SHARING_REDUCTION:
                {
                    // Pass the original reduced variable as if it were a shared
                    Source argument;
                    if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
                    {
                        argument << "*(args." << (*it)->get_field_name() << ")";
                    }
                    else
                    {
                        internal_error("running error", 0);
                    }
                    unpacked_arguments.append_with_separator(argument, ", ");

                    std::string name = (*it)->get_symbol().get_name();

                    private_entities
                        << "rdp_" << name << " = " << as_expression( (*it)->get_reduction_info()->get_identity()) << ";"
                        ;

                    break;
                }
            default:
                {
                    internal_error("Unexpected data sharing kind", 0);
                }
        }
    }

    // Add the user function to the intermediate file -> to HLS
    if (called_task.is_valid())
    {
        //find out if the function is already in the list
        //Currently ckecking function name only
        bool found = false;
        const std::string &orig_name = called_task.get_name();
        for (Nodecl::List::iterator it = _fpga_file_code.begin();
                it != _fpga_file_code.end() && !found;
                it++)
        {
            found = (it->get_symbol().get_name() == orig_name);
        }


        //if function is in the list, do not add it again
        if (!found)
        {
            Nodecl::NodeclBase tmp_task = Nodecl::Utils::deep_copy(
                        called_task.get_function_code(),
                        called_task.get_scope());

            if (_dump_ast != "0")
            {
                //write ast to a file
                std::string filename = called_task.get_name() + "_ast.dot";
                //include cxx-nodecl.hpp
                FILE* out_file = fopen(filename.c_str(), "w");
                ast_dump_graphviz(
                        nodecl_get_ast(called_task.get_function_code().get_internal_nodecl()),
                        out_file);
                fclose(out_file);
            }

            //add pragmas to the output code (only when working in stream mode)
//            add_hls_pragmas(tmp_task, outline_info);


            Nodecl::NodeclBase wrapper = gen_hls_wrapper(called_task, info._data_items);

            _fpga_file_code.append(wrapper);
            _fpga_file_code.append(tmp_task);
            //TODO: Add inline pragma to called task #pragma HLS inline
            // Remove the user function definition from the original source because

            // It is used only in the intermediate file
            // ^^ Are we completely sure about this? 
            Nodecl::Utils::remove_from_enclosing_list(called_task.get_function_code());
        }
    }


    // Create the new unpacked function
    TL::Symbol unpacked_function = new_function_symbol_unpacked(
            current_function,
            device_outline_name + "_unpacked",
            info._data_items,
            symbol_map);

    // The unpacked function must not be static and must have external linkage because
    // this function is called from the original source 
    unpacked_function.get_internal_symbol()->entity_specs.is_static = 0;
    if (IS_C_LANGUAGE)
    {
        unpacked_function.get_internal_symbol()->entity_specs.linkage_spec = "\"C\"";
    }

    Nodecl::NodeclBase unpacked_function_code, unpacked_function_body;
    build_empty_body_for_function(unpacked_function,
            unpacked_function_code,
            unpacked_function_body);

    Source fpga_params;
    //Only generate scalar parameter passing when it's necessary
    if (task_has_scalars(data_items))
    {
        fpga_params = fpga_param_code(info._data_items, symbol_map, called_scope);
    }

    Source unpacked_source;
    unpacked_source
        << "{"
        << private_entities
        << fpga_params
        << statement_placeholder(outline_placeholder)
        << "}"
        ;

    Nodecl::NodeclBase new_unpacked_body =
        unpacked_source.parse_statement(unpacked_function_body);
    unpacked_function_body.replace(new_unpacked_body);

    Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, unpacked_function_code);

    // Add a declaration of the unpacked function symbol in the original source
    if (IS_CXX_LANGUAGE)
    {
        Nodecl::NodeclBase nodecl_decl = Nodecl::CxxDecl::make(
                /* optative context */ nodecl_null(),
                unpacked_function,
                original_statements.get_filename(),
                original_statements.get_line());
        Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, nodecl_decl);
    }

    // Create the outline function
    //The outline function has always only one parameter which name is 'args'
    ObjectList<std::string> structure_name;
    structure_name.append("args");

    //The type of this parameter is an struct (i. e. user defined type)
    ObjectList<TL::Type> structure_type;
    structure_type.append(TL::Type(
                get_user_defined_type(
                    arguments_struct.get_internal_symbol())).get_lvalue_reference_to());

    TL::Symbol outline_function = new_function_symbol(
            current_function,
            device_outline_name,
            TL::Type::get_void_type(),
            structure_name,
            structure_type);

    Nodecl::NodeclBase outline_function_code, outline_function_body;
    build_empty_body_for_function(outline_function,
            outline_function_code,
            outline_function_body);

    Source outline_src;
    Source instrument_before,
           instrument_after;

    outline_src
        << "{"
        <<      instrument_before
        <<      device_outline_name << "_unpacked(" << unpacked_arguments << ");"
        <<      instrument_after
        << "}"
        ;

    Nodecl::NodeclBase new_outline_body = outline_src.parse_statement(outline_function_body);
    outline_function_body.replace(new_outline_body);
    Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, outline_function_code);

    output_statements = Nodecl::EmptyStatement::make(
                original_statements.get_filename(),
                original_statements.get_line());
}

DeviceFPGA::DeviceFPGA()
    : DeviceProvider(std::string("fpga"))
{
    set_phase_name("Nanox FPGA support");
    set_phase_description("This phase is used by Nanox phases to implement FPGA device support");
    register_parameter("dump_fpga_ast",
            "Dumps ast of functions to be implemented in the FPGA into a dot file",
            _dump_ast, "0");
}

void DeviceFPGA::pre_run(DTO& dto)
{
}

void DeviceFPGA::run(DTO& dto)
{
    DeviceProvider::run(dto);
}

bool DeviceFPGA::task_has_scalars(TL::ObjectList<OutlineDataItem*> & dataitems)
{
    for (ObjectList<OutlineDataItem*>::iterator it = dataitems.begin();
            it != dataitems.end();
            it++)
    {
        /*
         * We happily assume that everything that does not need a copy is a scalar
         * Which is true as long everything that is not a scalar is going to be copied
         * We could also use DataReference to check this
         */
        if((*it)->get_copies().empty())
        {
            return true;
        }
    }
    return false;
}

void DeviceFPGA::get_device_descriptor(DeviceDescriptorInfo& info,
        Source &ancillary_device_description,
        Source &device_descriptor,
        Source &fortran_dynamic_init)
{

    std::string outline_name = info._outline_name;
    Source device_outline_name;

    device_outline_name << fpga_outline_name(outline_name);
    if (Nanos::Version::interface_is_at_least("master", 5012))
    {
        ancillary_device_description
            << comment("FPGA device descriptor")
            << "static nanos_smp_args_t "
            << outline_name << "_args = { (void(*)(void*))" << device_outline_name << "};"
            ;
    }
    else
    {
        internal_error("Unsupported Nanos version.", 0);
    }
    device_descriptor
        << "{ nanos_fpga_factory,  &" << outline_name << "_args },";
        ;

}

//write/close intermediate files, free temporal nodes, etc.
void DeviceFPGA::phase_cleanup(DTO& data_flow)
{
    //XXX: We may need to create one file per function 
    //  if we want to work with multiple accelerators
    if (!_fpga_file_code.is_null())
    {
        std::string original_filename = TL::CompilationProcess::get_current_file().get_filename();
        std::string new_filename = "hls_" + original_filename;

        FILE* ancillary_file = fopen(new_filename.c_str(), "w");
        if (ancillary_file == NULL)
        {
            running_error("%s: error: cannot open file '%s'. %s\n",
                    original_filename.c_str(),
                    new_filename.c_str(),
                    strerror(errno));
        }


        compilation_configuration_t* configuration = CURRENT_CONFIGURATION;
        ERROR_CONDITION (configuration == NULL, "The compilation configuration cannot be NULL", 0);

        // Make sure phases are loaded (this is needed for codegen)
        load_compiler_phases(configuration);

        TL::CompilationProcess::add_file(new_filename, "fpga");

        //Remove the intermediate source file
        ::mark_file_for_cleanup(new_filename.c_str());

        Codegen::CxxBase* phase = reinterpret_cast<Codegen::CxxBase*>(configuration->codegen_phase);

        phase->codegen_top_level(_fpga_file_code, ancillary_file);
        fclose(ancillary_file);

        // Do not forget the clear the code for next files
        _fpga_file_code.get_internal_nodecl() = nodecl_null();
    }
}

TL::Symbol DeviceFPGA::new_function_symbol(
            TL::Symbol current_function,
        const std::string& name,
        TL::Type return_type,
        ObjectList<std::string> parameter_names,
        ObjectList<TL::Type> parameter_types)
{
    Scope sc = current_function.get_scope();

    // FIXME - Wrap
    decl_context_t decl_context = sc.get_decl_context();

    scope_entry_t* entry = new_symbol(decl_context, decl_context.current_scope, name.c_str());
    entry->entity_specs.is_user_declared = 1;

    entry->kind = SK_FUNCTION;
    entry->file = "";
    entry->line = 0;

    // Make it static
    entry->entity_specs.is_static = 1;

    // Make it member if the enclosing function is member
    if (current_function.is_member())
    {
        entry->entity_specs.is_member = 1;
        entry->entity_specs.class_type = current_function.get_class_type().get_internal_type();

        entry->entity_specs.access = AS_PUBLIC;

        ::class_type_add_member(entry->entity_specs.class_type, entry);
    }

    ERROR_CONDITION(parameter_names.size() != parameter_types.size(), "Mismatch between names and types", 0);

    decl_context_t function_context ;
    function_context = new_function_context(decl_context);
    function_context = new_block_context(function_context);

    function_context.function_scope->related_entry = entry;
    function_context.block_scope->related_entry = entry;

    entry->related_decl_context = function_context;

    parameter_info_t* p_types = new parameter_info_t[parameter_types.size()];

    parameter_info_t* it_ptypes = &(p_types[0]);
    ObjectList<TL::Type>::iterator type_it = parameter_types.begin();
    for (ObjectList<std::string>::iterator it = parameter_names.begin();
            it != parameter_names.end();
            it++, it_ptypes++, type_it++)
    {
        scope_entry_t* param = new_symbol(function_context, function_context.current_scope, it->c_str());
        param->entity_specs.is_user_declared = 1;
        param->kind = SK_VARIABLE;
        param->file = "";
        param->line = 0;

        param->defined = 1;

        symbol_set_as_parameter_of_function(param, entry, entry->entity_specs.num_related_symbols);

        param->type_information = get_unqualified_type(type_it->get_internal_type());

        P_LIST_ADD(entry->entity_specs.related_symbols,
                entry->entity_specs.num_related_symbols,
                param);

        it_ptypes->is_ellipsis = 0;
        it_ptypes->nonadjusted_type_info = NULL;
        it_ptypes->type_info = get_indirect_type(param);
    }

    type_t *function_type = get_new_function_type(
            return_type.get_internal_type(),
            p_types,
            parameter_types.size());

    entry->type_information = function_type;

    delete[] p_types;

    return entry;
}


TL::Symbol DeviceFPGA::new_function_symbol_unpacked(
        TL::Symbol current_function,
        const std::string& function_name,
        TL::ObjectList<OutlineDataItem*>& data_items,
        Nodecl::Utils::SymbolMap*& out_symbol_map)
{
    Scope sc = current_function.get_scope();

    decl_context_t decl_context = sc.get_decl_context();
    decl_context_t function_context;

    function_context = new_function_context(decl_context);
    function_context = new_block_context(function_context);

    // Create all the symbols and an appropiate mapping

    Nodecl::Utils::SimpleSymbolMap *symbol_map = new Nodecl::Utils::SimpleSymbolMap();

    TL::ObjectList<TL::Symbol> parameter_symbols, private_symbols;

    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        //print_dataItem_info(*it, sc);
        TL::Symbol sym = (*it)->get_symbol();

        std::string name;
        if (sym.is_valid())
        {
            name = sym.get_name();
            if (IS_CXX_LANGUAGE
                    && name == "this")
            {
                name = "this_";
            }
        }
        else
        {
            name = (*it)->get_field_name();
        }

        bool already_mapped = false;

        switch ((*it)->get_sharing())
        {
            case OutlineDataItem::SHARING_PRIVATE:
                {
                    scope_entry_t* private_sym = ::new_symbol(function_context, function_context.current_scope, name.c_str());
                    private_sym->kind = SK_VARIABLE;
                    private_sym->type_information = (*it)->get_in_outline_type().get_internal_type();
                    private_sym->defined = private_sym->entity_specs.is_user_declared = 1;

                    if (sym.is_valid())
                    {
                        symbol_map->add_map(sym, private_sym);

                        // Copy attributes that must be preserved
                        private_sym->entity_specs.is_allocatable = !sym.is_member() && sym.is_allocatable();
                    }

                    private_symbols.append(private_sym);
                    break;
                }
            case OutlineDataItem::SHARING_SHARED:
            case OutlineDataItem::SHARING_CAPTURE:
            case OutlineDataItem::SHARING_CAPTURE_ADDRESS:
                {
                    scope_entry_t* private_sym = ::new_symbol(function_context, function_context.current_scope,
                            name.c_str());
                    private_sym->kind = SK_VARIABLE;
                    private_sym->type_information = (*it)->get_in_outline_type().get_internal_type();
                    private_sym->defined = private_sym->entity_specs.is_user_declared = 1;


                    if (sym.is_valid())
                    {
                        private_sym->entity_specs.is_optional = sym.is_optional();
                        private_sym->entity_specs.is_allocatable =
                            !sym.is_member() && sym.is_allocatable();
                        if (!already_mapped)
                        {
                            symbol_map->add_map(sym, private_sym);
                        }
                    }

                    private_sym->entity_specs.is_allocatable = 
                        sym.is_allocatable() ||
                        (((*it)->get_allocation_policy() & OutlineDataItem::ALLOCATION_POLICY_TASK_MUST_DEALLOCATE_ALLOCATABLE) 
                         == OutlineDataItem::ALLOCATION_POLICY_TASK_MUST_DEALLOCATE_ALLOCATABLE);

                    parameter_symbols.append(private_sym);

                    break;
                }
            case OutlineDataItem::SHARING_REDUCTION:
                {
                    // Original reduced variable. Passed as we pass shared parameters
                    TL::Type param_type = (*it)->get_in_outline_type();
                    scope_entry_t* shared_reduction_sym = ::new_symbol(function_context, function_context.current_scope,
                            (*it)->get_field_name().c_str());
                    shared_reduction_sym->kind = SK_VARIABLE;
                    shared_reduction_sym->type_information = param_type.get_internal_type();
                    shared_reduction_sym->defined = shared_reduction_sym->entity_specs.is_user_declared = 1;
                    parameter_symbols.append(shared_reduction_sym);

                    shared_reduction_sym->entity_specs.is_allocatable = sym.is_valid()
                        && !sym.is_member()
                        && sym.is_allocatable();

                    // Private vector of partial reductions. This is a local pointer variable
                    // rdv stands for reduction vector
                    TL::Type private_reduction_vector_type = (*it)->get_private_type();
                    if (IS_C_LANGUAGE
                            || IS_CXX_LANGUAGE)
                    {
                        // T*
                        private_reduction_vector_type = private_reduction_vector_type.get_pointer_to();
                    }
                    else
                    {
                        internal_error("Code unreachable", 0);
                    }

                    scope_entry_t* private_reduction_vector_sym = ::new_symbol(function_context, function_context.current_scope,
                            ("rdv_" + name).c_str());
                    private_reduction_vector_sym->kind = SK_VARIABLE;
                    private_reduction_vector_sym->type_information = private_reduction_vector_type.get_internal_type();
                    private_reduction_vector_sym->defined = private_reduction_vector_sym->entity_specs.is_user_declared = 1;

                    // Local variable (rdp stands for reduction private)
                    // This variable must be initialized properly
                    scope_entry_t* private_sym = ::new_symbol(function_context, function_context.current_scope,
                            ("rdp_" + name).c_str());
                    private_sym->kind = SK_VARIABLE;
                    private_sym->type_information = (*it)->get_private_type().get_internal_type();
                    private_sym->defined = private_sym->entity_specs.is_user_declared = 1;

                    if (sym.is_valid())
                    {
                        symbol_map->add_map(sym, private_sym);
                    }

                    break;
                }
            default:
                {
                    internal_error("Unexpected data sharing kind", 0);
                }
        }
    }

    // Update types of parameters (this is needed by VLAs)
    for (TL::ObjectList<TL::Symbol>::iterator it = parameter_symbols.begin();
            it != parameter_symbols.end();
            it++)
    {
        it->get_internal_symbol()->type_information =
            type_deep_copy(it->get_internal_symbol()->type_information,
                    function_context,
                    symbol_map->get_symbol_map());
    }
    // Update types of privates (this is needed by VLAs)
    for (TL::ObjectList<TL::Symbol>::iterator it = private_symbols.begin();
            it != private_symbols.end();
            it++)
    {
        it->get_internal_symbol()->type_information =
            type_deep_copy(it->get_internal_symbol()->type_information,
                    function_context,
                    symbol_map->get_symbol_map());
    }

    // Now everything is set to register the function
    scope_entry_t* new_function_sym = new_symbol(decl_context, decl_context.current_scope, function_name.c_str());
    new_function_sym->entity_specs.is_user_declared = 1;

    new_function_sym->kind = SK_FUNCTION;
    new_function_sym->file = "";
    new_function_sym->line = 0;

    // Make it static
    new_function_sym->entity_specs.is_static = 1;

    // Make it member if the enclosing function is member
    if (current_function.is_member())
    {
        new_function_sym->entity_specs.is_member = 1;
        new_function_sym->entity_specs.class_type = current_function.get_class_type().get_internal_type();

        new_function_sym->entity_specs.access = AS_PUBLIC;

        ::class_type_add_member(new_function_sym->entity_specs.class_type,
                new_function_sym);
    }

    function_context.function_scope->related_entry = new_function_sym;
    function_context.block_scope->related_entry = new_function_sym;

    new_function_sym->related_decl_context = function_context;

    parameter_info_t* p_types = new parameter_info_t[parameter_symbols.size()];

    parameter_info_t* it_ptypes = &(p_types[0]);
    for (ObjectList<TL::Symbol>::iterator it = parameter_symbols.begin();
            it != parameter_symbols.end();
            it++, it_ptypes++)
    {
        scope_entry_t* param = it->get_internal_symbol();

        symbol_set_as_parameter_of_function(param, new_function_sym, new_function_sym->entity_specs.num_related_symbols);

        P_LIST_ADD(new_function_sym->entity_specs.related_symbols,
                new_function_sym->entity_specs.num_related_symbols,
                param);

        it_ptypes->is_ellipsis = 0;
        it_ptypes->nonadjusted_type_info = NULL;

        // FIXME - We should do all the remaining lvalue adjustments
        type_t* param_type = get_unqualified_type(param->type_information);
        it_ptypes->type_info = param_type;
    }

    type_t *function_type = get_new_function_type(
            get_void_type(),
            p_types,
            parameter_symbols.size());

    new_function_sym->type_information = function_type;

    delete[] p_types;

    out_symbol_map = symbol_map;
    return new_function_sym;
}

void DeviceFPGA::build_empty_body_for_function(
        TL::Symbol function_symbol,
        Nodecl::NodeclBase &function_code,
        Nodecl::NodeclBase &empty_stmt
        )
{
    empty_stmt = Nodecl::EmptyStatement::make("", 0);
    Nodecl::List stmt_list = Nodecl::List::make(empty_stmt);

    if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
    {
        Nodecl::CompoundStatement compound_statement =
            Nodecl::CompoundStatement::make(stmt_list,
                    /* destructors */ Nodecl::NodeclBase::null(),
                    "", 0);
        stmt_list = Nodecl::List::make(compound_statement);
    }

    Nodecl::NodeclBase context = Nodecl::Context::make(
            stmt_list,
            function_symbol.get_related_scope(), "", 0);

    function_symbol.get_internal_symbol()->defined = 1;

    function_code = Nodecl::FunctionCode::make(context,
            // Initializers
            Nodecl::NodeclBase::null(),
            // Internal functions
            Nodecl::NodeclBase::null(),
            function_symbol,
            "", 0);
}
/*
 * We may need to set scalar arguments here, but not transfers
 */
Source DeviceFPGA::fpga_param_code(
        TL::ObjectList<OutlineDataItem*> &data_items,
        Nodecl::Utils::SymbolMap *symbol_map,//we may not need it
        Scope sc
        )
{

    //Nodecl::Utils::SimpleSymbolMap *ssmap = (Nodecl::Utils::SimpleSymbolMap*)symbol_map;
    //TL::ObjectList<OutlineDataItem*> data_items = outline_info.get_data_items();
    Source args_src;
    /*
     * At some point we should have something like a nanox fpga-api so we can call functions
     * like init/deinit hardware, get channels and (maybe) setup config.
     */

    /*
     * Get the fpga handle to write the data that we need.
     *
     * TODO: Make sure mmap + set arg does not break when we don't have scalar arguments
     */
    args_src
        << "int fd = open(\"/dev/mem\", NANOS_O_RDWR);"    //2=O_RDWR
//        << "unsigned int acc_addr = NANOS_AXI_BASE_ADDRESS;"
//        << "printf(\"address: %x\\n\", acc_addr);"
        << "unsigned int *acc_handle = "
        << "    (unsigned int *) mmap(0, NANOS_MMAP_SIZE,"     //0=NULL
        << "    NANOS_PROT_READ|NANOS_PROT_WRITE, NANOS_MAP_SHARED,"           //"        PROT_READ | PROT_WRITE, MAP_SHARED"
        << "    fd, NANOS_AXI_BASE_ADDRESS);"
    ;

    //set scalar arguments
    /* FIXME
     * We assume that the base address to set scalar parameters
     * (which appears to be true)
     * In fact all of this is defined in the 
     *   impl/pcores/foo_top_v1_00_a/include/xfoo_AXIlite.h
     * where foo is the name of the function and top_v1_00_a is the version name
     * This path may change so we are assuming base addres does not
     */

    /*
     * Parameter have an offset of 8 bytes with the preceding one (except for 64bit ones)
     * If any parameter is smaller than 32bit (4byte), padding is added in between
     * If a paramater is 64bit(aka long long int) another 32bit of padding are added 
     */

    int argIndex = 0x14/4;  //base address/(sizeof(int)=4)
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
//        print_dataItem_info(*it, sc);
        Symbol outline_symbol = (*it)->get_symbol();
//        OutlineDataItem::CopyDirectionality directionality = (*it)->get_directionality();
        const TL::ObjectList<OutlineDataItem::CopyItem> &copies = (*it)->get_copies();

        //if copies are empty, we need to set the scalar value
        if (copies.empty())
        {
            const Type & type = (*it)->get_field_type();

            args_src
                << "acc_handle[" << argIndex << "] = "
                << outline_symbol.get_name() << ";"
            ;

            //+1 for field +1 for padding
            //we are adding an index to int[] => addresses are 4x
            argIndex+=2;
            if (type.get_size() >= 8)
            {
                argIndex ++;
            }
        }

    }

    /*
     * There should be a control bus mapped at 0x40440000
     * This is true if function has non-scalar parameters
     * To start the device we must set the first bt to 1
     */
    args_src << "acc_handle[0] = 1;"
        << "munmap(acc_handle, NANOS_MMAP_SIZE);"
        << "close(fd);"
        ;


    return args_src;
}

void DeviceFPGA::add_hls_pragmas(
        Nodecl::NodeclBase &task,
        TL::ObjectList<OutlineDataItem*> &data_items
        )
{
    /*
     * Insert hls pragmas in order to denerate input/output connections
     * Every parameter needs a directive:
     * scalar: create plain wire connections:
     *      #pragma HLS INTERFACE ap_none port=VAR
     *      #pragma AP resource core=AXI_SLAVE variable=VAR metadata="-bus_bundle AXIlite"
     *
     * Array; create fifo port to be handled by axi stream
     *      #pragma HLS stream variable=VAR <-- NOT NEEDED
     *      #pragma HLS resource core=AXI4Stream variable=VAR
     *      #pragma HLS interface ap_fifo port=VAR
     *
     * For every task there is a control bus defined to kick the accelerator off:
     *
     * #pragma AP resource core=AXI_SLAVE variable=return metadata="-bus_bundle AXIlite" \
     *      port_map={{ap_start START} {ap_done DONE} {ap_idle IDLE} {ap_return RETURN}}
     *
     * All of this stuff must be inside the function body i.e.
     *
     * void foo(...)
     * {
     *     pragma stuff
     *     function body
     * }
     *
     */

    //see what kind of ast it really is

    std::cerr << ast_node_type_name(task.get_kind()) 
        << " in_list: " << task.is_in_list()
        << " locus: " << task.get_locus()
        << std::endl;

    //Dig into the tree and find where the function statements are
    ObjectList<Nodecl::NodeclBase> tchildren = task.children();
    Nodecl::NodeclBase& context = tchildren.front();
    ObjectList<Nodecl::NodeclBase> cchildren = context.children();
    Nodecl::List list(cchildren.front().get_internal_nodecl());
    Nodecl::List stlist(list.begin()->children().front().get_internal_nodecl());

    Nodecl::UnknownPragma ctrl_bus = Nodecl::UnknownPragma::make(
        "AP resource core=AXI_SLAVE variable=return metadata=\"-bus_bundle AXIlite\" port_map={{ap_start START} {ap_done DONE} {ap_idle IDLE} {ap_return RETURN}}");
    stlist.prepend(ctrl_bus);


    //since we are using prepend, everything is going to appar in reverse order
    //but this may not be a real issue
//    TL::ObjectList<OutlineDataItem*> data_items = outline_info.get_data_items();
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        std::string field_name = (*it)->get_field_name();
        Nodecl::UnknownPragma pragma_node;

        if ((*it)->get_copies().empty())
        {
            //set scalar argumenit pragmas
            pragma_node = Nodecl::UnknownPragma::make("HLS INTERFACE ap_none port=" + field_name);
            stlist.prepend(pragma_node);

            pragma_node = Nodecl::UnknownPragma::make("AP resource core=AXI_SLAVE variable="
                    + field_name
                    + " metadata=\"-bus_bundle AXIlite\"");
            stlist.prepend(pragma_node);
        }
        else
        {
            //set array/stream pragmas
            pragma_node = Nodecl::UnknownPragma::make(
                    "HLS resource core=AXI4Stream variable=" + field_name);
            stlist.prepend(pragma_node);
            pragma_node = Nodecl::UnknownPragma::make(
                    "HLS interface ap_fifo port=" + field_name);
            stlist.prepend(pragma_node);
            
        }
    }
}

static void get_inout_decl(ObjectList<OutlineDataItem*>& data_items, std::string &in_type, std::string &out_type)
{
    in_type = "";
    out_type = "";
    for (ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        const ObjectList<OutlineDataItem::CopyItem> &copies = (*it)->get_copies();
        if (!copies.empty())
        {
            Scope scope = (*it)->get_symbol().get_scope();
            if (copies.front().directionality == OutlineDataItem::COPY_IN
                    && in_type == "")
            {
                in_type = (*it)->get_field_type().get_simple_declaration(scope, "");
            }
            else if  (copies.front().directionality == OutlineDataItem::COPY_OUT
                    && out_type == "")
            {
                out_type = (*it)->get_field_type().get_simple_declaration(scope, "");
            } else if (copies.front().directionality == OutlineDataItem::COPY_INOUT)
            {
                //If we find an inout, set both input and output types and return
                out_type = (*it)->get_field_type().get_simple_declaration(scope, "");
                in_type = out_type;
                return;
            }
        }
    }
}

static int get_copy_elements(Nodecl::NodeclBase expr)
{
    DataReference datareference(expr);
    if (!datareference.is_valid())
    {
        internal_error("invalid data reference (%s)", datareference.get_locus().c_str());
    }
    Type type = datareference.get_data_type();
    if (!type.is_array() || !type.array_is_region())
    {
        internal_error("Data copies must be an array region expression (%d)", datareference.get_locus().c_str());
    }
    const Nodecl::NodeclBase &cp_size = type.array_get_region_size();
    if (!cp_size.is_constant())
    {
        internal_error("Copy expressions must be known at compile time when working in 'block mode' (%s)",
                datareference.get_locus().c_str());
    }
/*
    std::cerr 
        << "is array: " << type.is_array() << std::endl
        << "has size: " << type.array_has_size() << std::endl
        << "size:     " << type.array_get_size().prettyprint() << std::endl
        << "region:   " << type.array_is_region() << std::endl
        << "reg size: " << type.array_get_region_size().prettyprint() << std::endl
    ;
*/
    return const_value_cast_to_4(cp_size.get_constant());

}


/*
 * Create wrapper function for HLS to unpack streamed arguments
 * 
 */
Nodecl::NodeclBase DeviceFPGA::gen_hls_wrapper(const Symbol &func_symbol, ObjectList<OutlineDataItem*>& data_items)
{
    //Check that we are calling a function task (this checking may be performed earlyer in the code)
    if (!func_symbol.is_function())
    {
        running_error("Only function-tasks are supperted at this moment");
    }
    Scope fun_scope = func_symbol.get_scope();
//    const ObjectList<Symbol> &param_list = func_symbol.get_function_parameters();
    /*
     * FIXME We suppose that all the input or the output arrays
     * are of the same type
     * Otherwise we must convert (~cast, raw type conversion) for each type
     */

    /*
     * The wrapper function must have:
     *      An input and an output parameters
     *      with respective pragmas needed for streaming
     *      For each scalar parameter, another scalar patameter
     *      IN THE SAME ORDER AS THE ORIGINAL FUNCTION as long as we are generating
     *      scalar parameter passing based on original function task parameters
     */
    //Source wrapper_params;
    std::string in_dec, out_dec;
    get_inout_decl(data_items, in_dec, out_dec);
    Source pragmas_src;
    //call to task_has_scalars is not the optimal, but it is much more simple and readable
    //than checking inside another loop
    if (task_has_scalars(data_items))
    {
        pragmas_src
            << "#pragma HLS resource core=AXI_SLAVE variable=return metadata=\"-bus_bundle AXIlite\" "
            << "port_map={{ap_start START} {ap_done DONE} {ap_idle IDLE} {ap_return RETURN}}\n";
        ;
    }
    Source args;
    if (in_dec != "")
    {
        args << in_dec << hls_in;
        //add stream parameter pragma
        pragmas_src
            << "#pragma HLS resource core=AXI4Stream variable=" << hls_in << "\n"
            << "#pragma HLS interface ap_fifo port=" << hls_in << "\n"
        ;
    }
    if (out_dec != "")
    {
        args.append_with_separator(out_dec + hls_out, ",");
        pragmas_src
            << "#pragma HLS resource core=AXI4Stream variable=" << hls_out << "\n"
            << "#pragma HLS interface ap_fifo port=" << hls_out << "\n"
        ;
    }

    /*
     * Generate wrapper code
     * We are going to keep original parameter name for the original function
     *
     * input/outlut parameters are received concatenated one after another.
     * The wrapper must create local variables for each input/output and unpack
     * streamed input/output data into that local variables.
     *
     * Scalar parameters are going to be copied as long as no unpacking is needed
     */
    Source copies_src;
    Source in_copies, out_copies;
    Source fun_params;
    Source local_decls;
    int in_offset  = 0;
    int out_offset = 0;

    for (ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {


        fun_params.append_with_separator((*it)->get_field_name(), ",");
        const std::string &field_name = (*it)->get_field_name();
        const Scope &scope = (*it)->get_symbol().get_scope();
        const ObjectList<OutlineDataItem::CopyItem> &copies = (*it)->get_copies();
        if (!copies.empty())
        {
            Nodecl::NodeclBase expr = copies.front().expression;
            if (copies.size() > 1)
            {
                internal_error("Only one copy per object (in/out/inout) is allowed (%s)",
                        expr.get_locus().c_str());
            }

            /*
             * emit copy code
             * - Create local variable (known size in compile time)
             * - Create create copy loop + update param offset
             */

            //get copy size (must be known at compile time)
            int n_elements = get_copy_elements(expr);

            const Type &field_type = (*it)->get_field_type();
            Type elem_type;
            if (field_type.is_pointer())
            {
                elem_type = field_type.points_to();
            }
            else if (field_type.is_array())
            {
                elem_type = field_type.array_element();
            }
            else
            {
                internal_error("invalid type for input/output, only pointer and array is allowed (%d)",
                        expr.get_locus().c_str());
            }

            std::string par_simple_decl = elem_type.get_simple_declaration(scope, field_name);
            local_decls
                << par_simple_decl << "[" << n_elements << "];\n";

            if (copies.front().directionality == OutlineDataItem::COPY_IN
                    or copies.front().directionality == OutlineDataItem::COPY_INOUT)
            {
                in_copies
                    << "for (" << HLS_I << "=0;" << HLS_I << "<" << n_elements << "; " << HLS_I << "++)"
                    << "{"
                    << "  " << field_name << "[" << HLS_I << "] = " << hls_in << "[" << HLS_I << "+" << in_offset << "];"
                    << "}"
                ;
                in_offset += n_elements;
            }
            if (copies.front().directionality == OutlineDataItem::COPY_OUT
                    or copies.front().directionality == OutlineDataItem::COPY_INOUT)
            {
                out_copies
                    << "for (" << HLS_I << "=0;" << HLS_I << "<" << n_elements << "; " << HLS_I << "++)"
                    //<< "for (i=0; i<" << n_elements << "; i++)"
                    << "{"
                    << "  "  << hls_out << "[" << HLS_I << "+" << out_offset << "] = " << field_name << "[" << HLS_I << "];"
                    << "}"
                ;
                out_offset += n_elements;
            }
        }
        else
        {
            //generate scalar parameter code
            Source par_src;

            par_src
                << (*it)->get_field_type().get_simple_declaration(scope, field_name)
            ;
            args.append_with_separator(par_src, ",");
            pragmas_src
                << "#pragma HLS INTERFACE ap_none port=" <<  field_name << "\n"
                << "#pragma AP resource core=AXI_SLAVE variable=" << field_name << " metadata=\"-bus_bundle AXIlite\"\n"
            ;
        }
    }
    Nodecl::NodeclBase fun_code =  func_symbol.get_function_code();
    Source wrapper_src;
    wrapper_src
        << "void core_hw_accelerator(" << args<< "){"
    ;
    local_decls << "unsigned int " << HLS_I << ";";

    wrapper_src
        << pragmas_src
        << local_decls
        << in_copies
        << func_symbol.get_name() << "(" << fun_params << ");"
        << out_copies
        << "}"
    ;
    //parse source
    ReferenceScope refscope(func_symbol.get_scope());
    Nodecl::NodeclBase wrapper_node = wrapper_src.parse_global(refscope);

    return wrapper_node;

}

EXPORT_PHASE(TL::Nanox::DeviceFPGA);

