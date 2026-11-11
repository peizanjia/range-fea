# Source docs live by goal/reference; installed names remain the existing allowlist.
# Rebase only links among these installed files, never expand Agent knowledge access.
if(NOT DEFINED RANGE_DOCUMENTATION_SOURCE_ROOT)
    get_filename_component(RANGE_DOCUMENTATION_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT DEFINED RANGE_INSTALL_DOCUMENTATION_ROOT)
    set(RANGE_INSTALL_DOCUMENTATION_ROOT "${CMAKE_BINARY_DIR}/documentation")
endif()
set(_range_documentation_pairs
    "docs/rounds/2026-10-01-application-agent-v1/design/WORKFLOW_CONTRACT.md|WORKFLOW_CONTRACT.md"
    "docs/reference/CONTROL_WORKFLOW.md|CONTROL_WORKFLOW.md"
    "docs/reference/WORKFLOW_SDK.md|WORKFLOW_SDK.md"
    "docs/reference/NATIVE_DOCUMENT_SDK.md|NATIVE_DOCUMENT_SDK.md"
    "docs/reference/NATIVE_RESULTS.md|NATIVE_RESULTS.md"
    "docs/reference/RECOVERY_OPERATIONS.md|RECOVERY_OPERATIONS.md"
    "docs/rounds/2026-10-08-workflow-delivery/design/RECOVERY_CONTRACT.md|RECOVERY_CONTRACT.md"
    "docs/reference/CONVERSATIONS.md|CONVERSATIONS.md"
    "docs/reference/TASK_PLANS.md|TASK_PLANS.md"
    "docs/rounds/2026-10-01-application-agent-v1/design/GUI_INTERACTION_DESIGN.md|GUI_INTERACTION_DESIGN.md"
    "docs/rounds/2026-10-01-application-agent-v1/acceptance/GUI_UX_ACCEPTANCE.md|GUI_UX_ACCEPTANCE.md"
    "docs/reference/EXTENSION_SDK.md|EXTENSION_SDK.md"
    "docs/rounds/2026-10-01-application-agent-v1/acceptance/STAGE12_ACCEPTANCE.md|STAGE12_ACCEPTANCE.md"
    "docs/reference/AGENT_OPERATIONS.md|AGENT_OPERATIONS.md"
    "docs/PIEZO_CONTROL_DELIVERY.md|PIEZO_CONTROL_DELIVERY.md"
    "docs/rounds/2026-10-12-piezo-delivery/design/PIEZO_DEPENDENCY_PROVENANCE.md|PIEZO_DEPENDENCY_PROVENANCE.md"
    "docs/piezo_control_plan.json|piezo_control_plan.json"
    "docs/reference/APPLICATION_SERVICE.md|APPLICATION_SERVICE.md"
    "docs/reference/PROJECT_CATALOG.md|PROJECT_CATALOG.md"
    "docs/reference/NATIVE_MODEL_OPERATIONS.md|NATIVE_MODEL_OPERATIONS.md"
    "docs/reference/CONTROLLER_ADAPTERS.md|CONTROLLER_ADAPTERS.md"
    "docs/reference/TYPED_FE_CONTROL.md|TYPED_FE_CONTROL.md"
    "docs/rounds/2026-09-25-coupling-reliability/acceptance/STAGE9_CONTROL_ANALYSIS.md|STAGE9_CONTROL_ANALYSIS.md"
    "docs/rounds/2026-09-25-coupling-reliability/design/TECHNICAL_ROUTE_STAGE9.md|TECHNICAL_ROUTE_STAGE9.md"
    "docs/rounds/2026-10-12-piezo-delivery/evidence/piezo_dependency_provenance.json|evidence/piezo_dependency_provenance.json"
    "docs/rounds/2026-10-12-piezo-delivery/evidence/piezo_delivery_performance_contract.json|evidence/piezo_delivery_performance_contract.json"
    "docs/rounds/2026-10-12-piezo-delivery/evidence/piezo_representative_cases_spec.json|evidence/piezo_representative_cases_spec.json"
 )
foreach(_range_pair IN LISTS _range_documentation_pairs)
    string(REPLACE "|" ";" _range_parts "${_range_pair}")
    list(GET _range_parts 0 _range_source)
    list(GET _range_parts 1 _range_destination)
    set(_range_input "${RANGE_DOCUMENTATION_SOURCE_ROOT}/${_range_source}")
    if(NOT EXISTS "${_range_input}")
        message(FATAL_ERROR "Missing maintained installation document: ${_range_source}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_range_input}")
    file(READ "${_range_input}" _range_content)
    get_filename_component(_range_source_dir "${_range_input}" DIRECTORY)
    get_filename_component(_range_destination_dir "${RANGE_INSTALL_DOCUMENTATION_ROOT}/${_range_destination}" DIRECTORY)
    if(_range_source MATCHES "\\.md$")
        foreach(_range_other IN LISTS _range_documentation_pairs)
            string(REPLACE "|" ";" _range_other_parts "${_range_other}")
            list(GET _range_other_parts 0 _range_other_source)
            list(GET _range_other_parts 1 _range_other_destination)
            file(RELATIVE_PATH _range_source_link "${_range_source_dir}" "${RANGE_DOCUMENTATION_SOURCE_ROOT}/${_range_other_source}")
            file(RELATIVE_PATH _range_installed_link "${_range_destination_dir}" "${RANGE_INSTALL_DOCUMENTATION_ROOT}/${_range_other_destination}")
            string(REPLACE "(${_range_source_link}" "(${_range_installed_link}" _range_content "${_range_content}")
        endforeach()
    endif()
    file(MAKE_DIRECTORY "${_range_destination_dir}")
    set(_range_output "${RANGE_INSTALL_DOCUMENTATION_ROOT}/${_range_destination}")
    # Avoid touching timestamps when content has not changed.
    set(_range_write TRUE)
    if(EXISTS "${_range_output}")
        file(READ "${_range_output}" _range_existing)
        if(_range_existing STREQUAL _range_content)
            set(_range_write FALSE)
        endif()
    endif()
    if(_range_write)
        file(WRITE "${_range_output}" "${_range_content}")
    endif()
endforeach()
