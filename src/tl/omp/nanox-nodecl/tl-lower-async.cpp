#include "tl-lowering-visitor.hpp"
#include "tl-source.hpp"
#include "tl-counters.hpp"
#include "tl-nodecl-alg.hpp"

using TL::Source;

namespace TL { namespace Nanox {

void LoweringVisitor::visit(const Nodecl::Parallel::Async& construct)
{
    Nodecl::NodeclBase environment = construct.get_environment();
    Nodecl::NodeclBase statements = construct.get_statements();

    OutlineInfo outline_info(environment);

    Symbol function_symbol = Nodecl::Utils::get_enclosing_function(construct);

    Source spawn_code;

    Source struct_arg_type_name,
           struct_runtime_size,
           struct_size,
           alignment,
           creation,
           priority,
           tiedness,
           fill_real_time_info,
           copy_decl,
           if_expr_cond_start,
           if_expr_cond_end,
           num_copies,
           copy_data,
           set_translation_fun,
           num_dependences,
           dependency_struct,
           dependency_array,
           immediate_decl,
           copy_imm_data,
           translation_fun_arg_name;

    Nodecl::NodeclBase fill_outline_arguments_tree,
        fill_dependences_outline_tree;
    Source fill_outline_arguments,
           fill_dependences_outline;

    Nodecl::NodeclBase fill_immediate_arguments_tree,
        fill_dependences_immediate_tree;
    Source fill_immediate_arguments,
           fill_dependences_immediate;

    Nodecl::NodeclBase copy_outline_setup_tree;
    Source copy_outline_setup;

    Nodecl::NodeclBase copy_immediate_setup_tree;
    Source copy_immediate_setup;

    // Name of the outline
    std::string outline_name;
    {
        Counter& task_counter = CounterManager::get_counter("nanos++-outline");
        std::stringstream ss;
        ss << "ol_" << function_symbol.get_name() << "_" << (int)task_counter;
        outline_name = ss.str();

        task_counter++;
    }
    
    // Devices stuff
    Source device_descriptor, 
           device_description, 
           device_description_line, 
           num_devices,
           ancillary_device_description;
    device_descriptor << outline_name << "_devices";
    device_description
        << ancillary_device_description
        << "nanos_device_t " << device_descriptor << "[1];"
        << device_description_line
        ;
    
    // Declare argument structure
    std::string structure_name = declare_argument_structure(outline_info, construct);
    struct_arg_type_name << structure_name;

    // FIXME - No devices yet, let's mimick the structure of one SMP
    {
        num_devices << "1";
        ancillary_device_description
            << comment("SMP device descriptor")
            << "nanos_smp_args_t " << outline_name << "_smp_args;" 
            << outline_name << "_smp_args.outline = (void(*)(void*))&" << outline_name << ";"
            ;

        device_description_line
            << device_descriptor << "[0].factory = &nanos_smp_factory;"
            // FIXME - Figure a way to get the true size
            << device_descriptor << "[0].dd_size = nanos_smp_dd_size;"
            << device_descriptor << "[0].arg = &" << outline_name << "_smp_args;";
    }

    // Outline
    emit_outline(outline_info, statements, outline_name, structure_name);

    // Fill argument structure
    bool immediate_is_alloca = false;
    if (!immediate_is_alloca)
    {
        immediate_decl
            << struct_arg_type_name << " imm_args;"
            ;
    }
    else
    {
        internal_error("Not yet implemented", 0);
    }

    Source err_name;
    err_name << "err";

    struct_size << "sizeof(imm_args)";
    alignment << "__alignof__(" << struct_arg_type_name << "), ";
    num_copies << "0";
    copy_data << "(nanos_copy_data_t**)0";
    copy_imm_data << "(nanos_copy_data_t*)0";
    translation_fun_arg_name << ", (void (*)(void*, void*))0";
    num_dependences << "0";
    dependency_struct << "nanos_dependence_t";
    dependency_array << "0";

    // Spawn code
    spawn_code
        << "{"
        // Devices related to this task
        <<     device_description
        // We use an extra struct because of Fortran
        <<     "struct { " << struct_arg_type_name << "* args; } ol_args;"
        <<     "ol_args.args = (" << struct_arg_type_name << "*) 0;"
        <<     immediate_decl
        <<     struct_runtime_size
        <<     "nanos_wd_t wd = (nanos_wd_t)0;"
        <<     "nanos_wd_props_t props;"
        <<     "props.mandatory_creation = 0;"
        <<     "props.tied = 0;"
        <<     "props.tie_to = (nanos_thread_t)0;"
        <<     "props.priority = 0;"
        <<     creation
        <<     priority
        <<     tiedness
        <<     fill_real_time_info
        <<     copy_decl
        <<     "nanos_err_t " << err_name <<";"
        <<     if_expr_cond_start
        <<     err_name << " = nanos_create_wd(&wd, " << num_devices << "," << device_descriptor << ","
        <<                 struct_size << ","
        <<                 alignment
        <<                 "(void**)&ol_args, nanos_current_wd(),"
        <<                 "&props, " << num_copies << ", " << copy_data << ");"
        <<     "if (" << err_name << " != NANOS_OK) nanos_handle_error (" << err_name << ");"
        <<     if_expr_cond_end
        <<     "if (wd != (nanos_wd_t)0)"
        <<     "{"
        <<        statement_placeholder(fill_outline_arguments_tree)
        <<        statement_placeholder(fill_dependences_outline_tree)
        <<        statement_placeholder(copy_outline_setup_tree)
        <<        copy_outline_setup
        <<        set_translation_fun
        <<        err_name << " = nanos_submit(wd, " << num_dependences << ", (" << dependency_struct << "*)" 
        <<         dependency_array << ", (nanos_team_t)0);"
        <<        "if (" << err_name << " != NANOS_OK) nanos_handle_error (" << err_name << ");"
        <<     "}"
        <<     "else"
        <<     "{"
        <<          statement_placeholder(fill_immediate_arguments_tree)
        <<          statement_placeholder(fill_dependences_immediate_tree)
        <<          statement_placeholder(copy_immediate_setup_tree)
        <<          err_name << " = nanos_create_wd_and_run(" 
        <<                  num_devices << ", " << device_descriptor << ", "
        <<                  struct_size << ", " 
        <<                  alignment
        <<                  (immediate_is_alloca ? "imm_args" : "&imm_args") << ","
        <<                  num_dependences << ", (" << dependency_struct << "*)" << dependency_array << ", &props,"
        <<                  num_copies << "," << copy_imm_data 
        <<                  translation_fun_arg_name << ");"
        <<          "if (" << err_name << " != NANOS_OK) nanos_handle_error (" << err_name << ");"
        <<     "}"
        << "}"
        ;

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

    // Now fill the arguments information (this part is language dependent)
    TL::ObjectList<OutlineDataItem> data_items = outline_info.get_data_items();
    if (IS_C_LANGUAGE
            || IS_CXX_LANGUAGE)
    {
        for (TL::ObjectList<OutlineDataItem>::iterator it = data_items.begin();
                it != data_items.end();
                it++)
        {

            if (it->get_sharing() == OutlineDataItem::SHARING_CAPTURE)
            {
                fill_outline_arguments << 
                    "ol_args.args->" << it->get_field_name() << " = " << it->get_symbol().get_name() << ";"
                    ;
                fill_immediate_arguments << 
                    "imm_args." << it->get_field_name() << " = " << it->get_symbol().get_name() << ";"
                    ;
            }
            else if (it->get_sharing() == OutlineDataItem::SHARING_SHARED)
            {
                fill_outline_arguments << 
                    "ol_args.args->" << it->get_field_name() << " = &" << it->get_symbol().get_name() << ";"
                    ;
                fill_immediate_arguments << 
                    "imm_args." << it->get_field_name() << " = &" << it->get_symbol().get_name() << ";"
                    ;
            }
        }
    }
    else if (IS_FORTRAN_LANGUAGE)
    {

        for (TL::ObjectList<OutlineDataItem>::iterator it = data_items.begin();
                it != data_items.end();
                it++)
        {

            if (it->get_sharing() == OutlineDataItem::SHARING_CAPTURE)
            {
                fill_outline_arguments << 
                    "ol_args % args % " << it->get_field_name() << " = " << it->get_symbol().get_name() << "\n"
                    ;
                fill_immediate_arguments << 
                    "imm_args % " << it->get_field_name() << " = " << it->get_symbol().get_name() << "\n"
                    ;
            }
            else if (it->get_sharing() == OutlineDataItem::SHARING_SHARED)
            {
                fill_outline_arguments << 
                    "ol_args % args %" << it->get_field_name() << " => " << it->get_symbol().get_name() << "\n"
                    ;
                fill_immediate_arguments << 
                    "imm_args % " << it->get_field_name() << " => " << it->get_symbol().get_name() << "\n"
                    ;
                // Make it TARGET as required by Fortran
                it->get_symbol().get_internal_symbol()->entity_specs.is_target = 1;
            }
        }
    }

    Nodecl::NodeclBase new_tree = fill_outline_arguments.parse_statement(fill_outline_arguments_tree);
    fill_outline_arguments_tree.integrate(new_tree);

    new_tree = fill_immediate_arguments.parse_statement(fill_immediate_arguments_tree);
    fill_immediate_arguments_tree.integrate(new_tree);

    construct.integrate(spawn_code_tree);
}

} }