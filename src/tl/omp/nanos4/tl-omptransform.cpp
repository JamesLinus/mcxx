/*--------------------------------------------------------------------
  (C) Copyright 2006-2009 Barcelona Supercomputing Center 
                          Centro Nacional de Supercomputacion
  
  This file is part of Mercurium C/C++ source-to-source compiler.
  
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


#include "tl-omptransform.hpp"
#include "tl-pretransform.hpp"

namespace TL
{
    namespace Nanos4
    {
        OpenMPTransform::OpenMPTransform()
            : num_parallels(0),
            parallel_nesting(0),
            transaction_nesting(0),
            stm_log_file_opened(false),
            enable_mintaka_instr(false), 
            enable_nth_create(true), 
            disable_restrict_pointers(false),
            use_memcpy_always(true),
            stm_global_lock_enabled(false),
            run_pretransform(true),
            allow_inlining_of_outlines(true),
            atomic_as_critical(false)
        {
            // Set phase info
            set_phase_name("Nanos 4 OpenMP implementation");
            set_phase_description("Implementation of OpenMP for Nanos 4 runtime");

            // Register parameters for this phase
            register_parameter("nanos_new_interface", 
                    "If set to '1' will use the new interface for all parallel constructs",
                    nanos_new_interface_str,
                    "0").connect(functor(&OpenMPTransform::set_parallel_interface, *this));
            register_parameter("instrument",
                    "Enables mintaka instrumentation if set to '1'",
                    enable_mintaka_instr_str,
                    "0").connect(functor(&OpenMPTransform::set_instrumentation, *this));
            register_parameter("disable_restrict",
                    "Disables restricted pointers in outlines if set to '1'", 
                    disable_restrict_str,
                    "0").connect(functor(&OpenMPTransform::set_disable_restrict_pointers, *this));

            register_parameter("allow_inlining_of_outlines",
                    "Hints the backend compiler that the parallel outline can be inlined (if directly called)",
                    allow_inlining_of_outlines_str,
                    "1").connect(functor(&OpenMPTransform::set_allow_inlining_of_outlines, *this));

            C_LANGUAGE()
            {
                use_memcpy_always = true;
            }
            CXX_LANGUAGE()
            {
                register_parameter("use_memcpy_always",
                        "In C++, always use 'memcpy' when copying arrays if set to '1'. In C, 'memcpy' is always used.",
                        use_memcpy_always_str,
                        "0").connect(functor(&OpenMPTransform::set_use_memcpy_always, *this));
            }
            C_LANGUAGE()
            {
                run_pretransform = true;
            }
            CXX_LANGUAGE()
            {
                register_parameter("run_pretransform",
                        "In C++ some preliminar transforms are not fully supported. "
                        "This flag allows disabling them in case of trouble."
                        "In C these transformations are always performed.",
                        run_pretransform_str,
                        "1").connect(functor(&OpenMPTransform::set_run_pretransform, *this));
            }
            register_parameter("atomic_as_critical",
                    "When possible the compiler will use atomic builtins to implement 'atomic' construct."
                    "If your compiler does not support these atomic builtins you can use this flag and"
                    "atomics will be implemented as a critical region",
                    atomic_as_critical_str,
                    "0").connect(functor(&OpenMPTransform::set_atomic_as_critical, *this));

            // STM options
            register_parameter("STM_global_lock",
                    "Enables global lock interface for '#pragma omp transaction' regions. Disables STM memory tracking.",
                    stm_global_lock_enabled_str,
                    "0").connect(functor(&OpenMPTransform::set_stm_global_lock, *this));

            // No signals for these as their values are passed to the object initialization function
            register_parameter("STM_replace_functions_file", 
                    "Filter file of STM-replaced function calls",
                    stm_replace_functions_file,
                    "./stm_replace_functions_file");
            register_parameter("STM_replace_functions_mode",
                    "Filter mode when STM-replacing function calls. It can be 'normal' or 'inverted'",
                    stm_replace_functions_mode,
                    "normal");
            register_parameter("STM_wrap_functions_file",
                    "Filter file of STM-wrapped functions",
                    stm_wrap_functions_file,
                    "./stm_wrap_functions_file");
            register_parameter("STM_wrap_functions_mode",
                    "Filter mode when STM-wrapping functions. It can be either 'normal' or 'inverted'",
                    stm_wrap_functions_mode,
                    "normal");

            // Register callbacks for constructs and directives
#define OMP_CONSTRUCT(_name, _construct_name) \
              on_directive_pre[_name].connect(functor(&OpenMPTransform::_construct_name##_preorder, *this)); \
              on_directive_post[_name].connect(functor(&OpenMPTransform::_construct_name##_postorder, *this));
#define OMP_DIRECTIVE(_name, _construct_name) OMP_CONSTRUCT(_name, _construct_name)
#include "tl-omp-constructs.def"
#undef OMP_CONSTRUCT
#undef OMP_DIRECTIVE

            // #pragma omp taskgroup
            register_directive("taskgroup");
            on_directive_post["taskgroup"].connect(functor(&OpenMPTransform::taskgroup_postorder, *this));

            // #pragma omp taskyield
            register_directive("taskyield");
            on_directive_post["taskyield"].connect(functor(&OpenMPTransform::taskyield_postorder, *this));
            // End of OMP 3.0 tasks

            // --- Transactional world --
            // #pragma omp transaction
            register_construct("transaction");
            on_directive_pre["transaction"].connect(functor(&OpenMPTransform::stm_transaction_preorder, *this));
            on_directive_post["transaction"].connect(functor(&OpenMPTransform::stm_transaction_postorder, *this));

            // #pragma omp retry
            register_directive("retry");
            on_directive_post["retry"].connect(functor(&OpenMPTransform::stm_retry_postorder, *this));

            // #pragma omp preserve
            register_construct("preserve");
            on_directive_post["preserve"].connect(functor(&OpenMPTransform::stm_preserve_postorder, *this));

            // #pragma omp adf
            register_construct("adf");
            on_directive_pre["adf"].connect(functor(&OpenMPTransform::adf_task_preorder, *this));
            on_directive_post["adf"].connect(functor(&OpenMPTransform::adf_task_postorder, *this));
            // --- End of transactional world --
        }

        void OpenMPTransform::set_disable_restrict_pointers(const std::string& str)
        {
            disable_restrict_pointers = false;
            parse_boolean_option(/* Parameter name */ "disable_restrict", 
                    /* Given value */ str, 
                    /* Computed bool */ disable_restrict_pointers, 
                    /* Error message */  "Restrict pointers will be enabled");
        }

        void OpenMPTransform::set_parallel_interface(const std::string& str)
        {
            enable_nth_create = false;
            parse_boolean_option(/* Parameter name */ "nanos_new_interface", 
                    /* Given value */ str, 
                    /* Computed bool */ enable_nth_create, 
                    /* Error message */  "Old interface will be used for parallel spawns");
        }

        void OpenMPTransform::set_instrumentation(const std::string& str)
        {
            enable_mintaka_instr = false;
            parse_boolean_option(/* Parameter name */ "instrument", 
                    /* Given value */ str, 
                    /* Computed bool */ enable_mintaka_instr, 
                    /* Error message */  "Instrumentation disabled");
        }

        void OpenMPTransform::set_stm_global_lock(const std::string& str)
        {
            stm_global_lock_enabled = false;
            parse_boolean_option(/* Parameter name */ "STM_global_lock", 
                    /* Given value */ str, 
                    /* Computed bool */ stm_global_lock_enabled, 
                    /* Error message */  "STM global lock disabled");
        }

        void OpenMPTransform::set_use_memcpy_always(const std::string& str)
        {
            use_memcpy_always = false;
            parse_boolean_option(/* Parameter name */ "use_memcpy_always",
                    /* Given value */ str,
                    /* Compiler bool */ use_memcpy_always,
                    /* Error message */ "Will use plain assignment when copying arrays");
        }

        void OpenMPTransform::set_run_pretransform(const std::string& str)
        {
            run_pretransform = true;
            parse_boolean_option(/* Parameter name */ "run_pretransform",
                    /* Given value */ str,
                    /* Compiler bool */ run_pretransform,
                    /* Error message */ "Will run pretransformations");
        }

        void OpenMPTransform::set_atomic_as_critical(const std::string& str)
        {
            atomic_as_critical = false;
            parse_boolean_option(/* Parameter name */ "atomic_as_critical",
                    /* Given value */ str,
                    /* Compiler bool */ atomic_as_critical,
                    /* Error message */ "Atomics will be implemented using atomic builtins");
        }

        void OpenMPTransform::set_allow_inlining_of_outlines(const std::string& str)
        {
            allow_inlining_of_outlines = false;
            parse_boolean_option(/* Parameter name */ "allow_inlining_of_outlines",
                    /* Given value */ str,
                    /* Compiler bool */ allow_inlining_of_outlines,
                    /* Error message */ "Outlines will be inlined");
        }

        bool OpenMPTransform::instrumentation_requested()
        {
            return enable_mintaka_instr;
        }

        OpenMPTransform::~OpenMPTransform()
        {
            // This is needed since "init" is a virtual method
        }

        void OpenMPTransform::init(DTO& dto)
        {
            // This function is called in OpenMPPhase::run
            ObjectList<std::string> keys = dto.get_keys();

            if (keys.contains(SERIALIZED_FUNCTIONS_INFO))
            {
                RefPtr<Object> obj = dto[SERIALIZED_FUNCTIONS_INFO];
                serialized_functions_info = RefPtr<SerializedFunctionsInfo>::cast_dynamic(obj);
            }
        }
        
        void OpenMPTransform::run(DTO& dto)
        {
#if 0
            if (run_pretransform)
            {
                OpenMP_PreTransform pre_transform;
                pre_transform.run(dto);
                // Purge local threadprivates, promoting them as global ones
                pre_transform.purge_local_threadprivates();
            }
#endif

            // Call the OpenMPPhase::run
            OpenMPPhase::run(dto);
        }

        void OpenMPTransform::pre_run(DTO& dto)
        {
            OpenMPPhase::pre_run(dto);
            // ;
        }
    }
}

EXPORT_PHASE(TL::Nanos4::OpenMPTransform);