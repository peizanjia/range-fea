cmake_minimum_required(VERSION 3.21)
project(RangeFea)
set(RANGE_PRIVATE_PYTHON_DIRECTORY "" CACHE PATH "Verified private Python runtime assembled by scripts/packaging/build_private_python.py")
file(TO_CMAKE_PATH "${RANGE_PRIVATE_PYTHON_DIRECTORY}" RANGE_PRIVATE_PYTHON_DIRECTORY_CMAKE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(Config.cmake)

set(DEBUG_EXT "")
set(PROJECT_HUMAN_REL_NAME "${PROJECT_HUMAN_NAME}")
set(PROJECT_SHORT_REL_NAME "${PROJECT_SHORT_NAME}")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEBUG_EXT "_debug")
    add_compile_definitions(DEBUG)
    set(PROJECT_HUMAN_REL_NAME "${PROJECT_HUMAN_REL_NAME} (${CMAKE_BUILD_TYPE})")
    set(PROJECT_SHORT_REL_NAME "${PROJECT_SHORT_REL_NAME} (${CMAKE_BUILD_TYPE})")
endif()

set(BUILD_SHARED_LIBS OFF)

# Open MP configuration -------------------------------------------------------
if(APPLE AND (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    execute_process(COMMAND brew --prefix libomp
                    OUTPUT_VARIABLE OMP_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NOT OMP_PREFIX)
        message(FATAL_ERROR "brew --prefix libomp failed. Is libomp installed?")
    endif()

    message(STATUS "libomp prefix: ${OMP_PREFIX}")

    # macOS + Clang + possibly Homebrew or MacPorts
    find_path(OpenMP_INCLUDES
        NAMES omp.h
        PATHS "${OMP_PREFIX}/include"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
        REQUIRED
        DOC "Path to OpenMP headers"
    )
    find_library(OpenMP_LIBRARY
        NAMES omp
        PATHS "${OMP_PREFIX}/lib"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
        REQUIRED
        DOC "Path to OpenMP library"
    )

    if(OpenMP_INCLUDES AND OpenMP_LIBRARY)
        message(STATUS "Found OpenMP headers at: ${OpenMP_INCLUDES}")
        message(STATUS "Found OpenMP library at: ${OpenMP_LIBRARY}")

        set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp -I${OpenMP_INCLUDES}")
        set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I${OpenMP_INCLUDES}")
        set(OpenMP_C_LIB_NAMES "omp")
        set(OpenMP_CXX_LIB_NAMES "omp")
        set(OpenMP_omp_LIBRARY ${OpenMP_LIBRARY})
    else()
        message(WARNING "OpenMP not found. Please install libomp (e.g., brew install libomp).")
        if(NOT OpenMP_INCLUDES)
            message(WARNING "OpenMP header not found.")
        endif()
        if(NOT OpenMP_LIBRARY)
            message(WARNING "OpenMP library not found.")
        endif()
    endif()
endif()
# Open MP configuration - End  ------------------------------------------------

find_package(OpenGL REQUIRED)
find_package(OpenMP REQUIRED)
if(MSVC)
    # Reusing a CMake tree configured for /openmp:llvm can leave the LLVM
    # runtime cached even after the project stops requesting it. Reject any
    # remaining LLVM flags or libraries so packaging cannot pair libomp with
    # the MSVC vcomp redistributable. Configure a fresh build directory.
    string(TOLOWER "${OpenMP_CXX_FLAGS}" _range_openmp_cxx_flags_lower)
    string(TOLOWER
        "${OpenMP_CXX_LIB_NAMES};${OpenMP_LIB_NAMES};${OpenMP_CXX_LIBRARIES};${OpenMP_libomp_LIBRARY};${OpenMP_omp_LIBRARY};${OpenMP_RUNTIME_MSVC}"
        _range_openmp_link_settings_lower)
    if(_range_openmp_cxx_flags_lower MATCHES "openmp:llvm"
       OR _range_openmp_link_settings_lower MATCHES "libomp|openmp:llvm")
        message(FATAL_ERROR
            "The MSVC CMake cache still selects LLVM OpenMP. "
            "Configure a fresh build directory; do not reuse a cache created with /openmp:llvm.")
    endif()
endif()
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Concurrent HttpServer Network Widgets OpenGL OpenGLWidgets Multimedia PrintSupport LinguistTools)
qt_standard_project_setup()

# Unit tests (opt-in). Configure with -DRANGE_BUILD_TESTS=ON and run `ctest`.
option(RANGE_BUILD_TESTS "Build unit tests" OFF)
option(RANGE_REQUIRE_NUMERIC_TESTS
    "Fail configuration unless the Python, NumPy, and SciPy numeric tests are available" OFF)
option(RANGE_ENABLE_AGENT "Build the optional AI Agent integration" ON)
if(RANGE_REQUIRE_NUMERIC_TESTS AND NOT RANGE_BUILD_TESTS)
    message(FATAL_ERROR "RANGE_REQUIRE_NUMERIC_TESTS requires RANGE_BUILD_TESTS=ON")
endif()
if(RANGE_BUILD_TESTS)
    # Import Qt6::Test in the top-level directory as well as in individual
    # test directories so the Windows runtime deployment target can copy it.
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test)
    enable_testing()
endif()

add_library(common_defines INTERFACE)
target_compile_definitions(common_defines
    INTERFACE
        RANGE_ENABLE_AGENT=$<BOOL:${RANGE_ENABLE_AGENT}>
        VERSION="${PROJECT_VERSION}"
        YEAR=${PROJECT_YEAR}
        PACKAGENAME="${PROJECT_PACKAGE_NAME}"
        NAME="${PROJECT_HUMAN_REL_NAME}"
        SHORTNAME="${PROJECT_SHORT_NAME}"
        FAMILYNAME="${PROJECT_FAMILY_NAME}"
        TITLE="${PROJECT_TITLE}"
        DESCRIPTION="${PROJECT_DESCRIPTION}"
        AUTHOR="${PROJECT_AUTHOR}"
        EMAIL="${PROJECT_EMAIL}"
        WWW="${PROJECT_WWW}"
        WWWDOMAIN="${PROJECT_WWW_DOMAIN}"
)

function(set_debug_suffix_if_needed target_name)
    if(CMAKE_CONFIGURATION_TYPES)  # Multi-config (e.g. Visual Studio, Xcode)
        set_target_properties(${target_name} PROPERTIES
            OUTPUT_NAME_DEBUG "${target_name}${DEBUG_EXT}"
            OUTPUT_NAME_RELEASE "${target_name}"
        )
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")  # Single-config (e.g. Make, Ninja)
        set_target_properties(${target_name} PROPERTIES
            OUTPUT_NAME "${target_name}${DEBUG_EXT}"
        )
    else()
        set_target_properties(${target_name} PROPERTIES
            OUTPUT_NAME "${target_name}"
        )
    endif()
endfunction()

set(all_targets range-base-lib range-cloud-lib range-gui-lib TetGen range-model-lib range-solver-lib range-application-lib fea-solver fea)
if(RANGE_ENABLE_AGENT)
    list(INSERT all_targets 2 range-ai-lib)
endif()

foreach(tgt IN LISTS all_targets)
    add_subdirectory(${tgt})
    set_debug_suffix_if_needed(${tgt})
    if(EXISTS "${CMAKE_SOURCE_DIR}/${tgt}/doc/RELEASE_NOTES.md")
        add_custom_target(copy-release-notes-${tgt} ALL
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_SOURCE_DIR}/${tgt}/doc/RELEASE_NOTES.md"
                    "$<TARGET_FILE_DIR:${tgt}>/RELEASE_NOTES.md"
            DEPENDS ${tgt}
            COMMENT "Copying RELEASE_NOTES.md for ${tgt}"
            VERBATIM
        )
        if(NOT APPLE)
            install(FILES "$<TARGET_FILE_DIR:${tgt}>/RELEASE_NOTES.md"
                    DESTINATION doc
                    RENAME "RELEASE_NOTES_${tgt}.md"
                    COMPONENT Application)
        endif()
        list(APPEND _release_notes_targets ${tgt})
    endif()
endforeach()

add_subdirectory(range-workflow)

if(WIN32 AND RANGE_GMSH_RUNTIME)
    add_custom_target(deploy-gmsh-runtime ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RANGE_GMSH_RUNTIME}"
                "$<TARGET_FILE_DIR:fea>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RANGE_GMSH_RUNTIME}"
                "$<TARGET_FILE_DIR:fea-solver>"
        DEPENDS fea fea-solver
        COMMENT "Deploying optional Gmsh runtime"
        VERBATIM)
endif()

# Generate RELEASE_NOTES.json (all platforms)
# Chapters are listed in reverse all_targets order, so the top level
# applications come first and the foundational libraries last.
if(_release_notes_targets)
    set(_rn_json_targets ${_release_notes_targets})
    list(REVERSE _rn_json_targets)
    set(_rn_json_chapters "")
    foreach(_rn_tgt IN LISTS _rn_json_targets)
        if(_rn_json_chapters)
            string(APPEND _rn_json_chapters ",\n")
        endif()
        string(APPEND _rn_json_chapters
            "        {\n            \"title\": \"${_rn_tgt}\",\n            \"file\": \"RELEASE_NOTES_${_rn_tgt}.md\"\n        }")
    endforeach()
    set(_rn_json_path "${CMAKE_BINARY_DIR}/RELEASE_NOTES.json")
    file(WRITE "${_rn_json_path}"
        "{\n    \"chapters\": [\n${_rn_json_chapters}\n    ]\n}\n")
endif()

# Define OpenSSL ROOT directory -----------------------------------------------

set(OPENSSL_ROOT "" CACHE PATH "OpenSSL prefix")

if(OPENSSL_ROOT STREQUAL "" AND DEFINED ENV{OPENSSL_ROOT})
  set(OPENSSL_ROOT "$ENV{OPENSSL_ROOT}" CACHE PATH "OpenSSL prefix" FORCE)
endif()

# Resolve symlinks so install_name_tool -change matches the actual embedded paths
if(OPENSSL_ROOT AND APPLE)
  file(REAL_PATH "${OPENSSL_ROOT}" OPENSSL_ROOT_REAL)
else()
  set(OPENSSL_ROOT_REAL "${OPENSSL_ROOT}")
endif()

# Deploy Qt -------------------------------------------------------------------

get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)

set(_qt_deploy_dir "$<TARGET_FILE_DIR:${PROJECT_TARGET_NAME}>/qtlib")

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
    set(DEPLOY_COMMAND
        ${WINDEPLOYQT_EXECUTABLE}
        $<TARGET_FILE:${PROJECT_TARGET_NAME}>
        --dir ${_qt_deploy_dir}
        --force-openssl
    )

    add_custom_target(deploy-qt ALL
        COMMAND ${DEPLOY_COMMAND}
        DEPENDS fea
        COMMENT "Deploying Windows Qt libraries"
        VERBATIM
    )

    # Visual Studio places every test executable beside fea.exe, while
    # windeployqt stages the runtime in the qtlib subdirectory used by the
    # installer.  Keep the staging directory clean for packaging and mirror
    # its contents beside the tests so a plain `ctest -C <config>` works on a
    # developer machine without modifying PATH.
    if(RANGE_BUILD_TESTS)
        add_custom_target(deploy-test-runtime ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${_qt_deploy_dir}
                    $<TARGET_FILE_DIR:${PROJECT_TARGET_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:Qt6::Test>
                    $<TARGET_FILE_DIR:${PROJECT_TARGET_NAME}>
            DEPENDS deploy-qt
            COMMENT "Deploying Qt runtime for Windows tests"
            VERBATIM
        )
    endif()
elseif(APPLE)
    # Put fea-solver into FEA.app/Contents/MacOS on macOS ---------------------
    # This copies the already-built fea-solver next to the main executable inside the bundle
    add_custom_target(embed-fea-solver ALL
        COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:fea-solver>
                $<TARGET_FILE_DIR:fea>
        DEPENDS fea fea-solver
        COMMENT "Embedding fea-solver into FEA.app/Contents/MacOS"
        VERBATIM
    )

    # Create custom .DS_Store on MacOS ----------------------------------------
    set(APP_DIR_OLD "$<TARGET_BUNDLE_DIR:${PROJECT_TARGET_NAME}>")
    set(APP_DIR_TMP "${CMAKE_BINARY_DIR}/${PROJECT_TARGET_NAME}/${PROJECT_SHORT_REL_NAME}_tmp.app")
    set(APP_DIR_NEW "${CMAKE_BINARY_DIR}/${PROJECT_TARGET_NAME}/${PROJECT_SHORT_REL_NAME}.app")
    set(DSSTORE_FILE "${CMAKE_BINARY_DIR}/${PROJECT_TARGET_NAME}.dsstore")

    add_custom_target(app-rename ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${APP_DIR_TMP}
        COMMAND ${CMAKE_COMMAND} -E rename ${APP_DIR_OLD} ${APP_DIR_TMP}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${APP_DIR_NEW}
        COMMAND ${CMAKE_COMMAND} -E rename ${APP_DIR_TMP} ${APP_DIR_NEW}
        DEPENDS embed-fea-solver
        COMMENT "Renaming ${APP_DIR_OLD} bundle to ${APP_DIR_NEW}"
        VERBATIM
    )

    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${_qt_bin_dir}")
    set(DEPLOY_COMMAND
        ${MACDEPLOYQT_EXECUTABLE}
        ${APP_DIR_NEW}
        -executable=${APP_DIR_NEW}/Contents/MacOS/fea-solver
    )

    add_custom_target(deploy-qt ALL
        COMMAND ${DEPLOY_COMMAND}
        DEPENDS app-rename
        COMMENT "Deploying MacOS Qt libraries"
        VERBATIM
    )

    add_custom_target(deploy-data ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/help ${APP_DIR_NEW}/Contents/Resources/help
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/data ${APP_DIR_NEW}/Contents/Resources/data
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/etc ${APP_DIR_NEW}/Contents/Resources/etc
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/materials ${APP_DIR_NEW}/Contents/Resources/materials
        COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_DIR_NEW}/Contents/Resources/doc
        COMMAND ${CMAKE_COMMAND} -E copy           ${CMAKE_BINARY_DIR}/LICENSE.txt ${APP_DIR_NEW}/Contents/Resources/doc/LICENSE.txt
        DEPENDS deploy-qt
        COMMENT "Deploying application data"
        VERBATIM
    )

    add_custom_target(gen-dsstore ALL
        COMMAND "${CMAKE_SOURCE_DIR}/range-build-tools/create_dsstore.sh"
                --app-bundle=${APP_DIR_NEW}
                --background=${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/pixmaps/range-dmg_background.png
                --icon=${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/pixmaps/${PROJECT_PACKAGE_NAME}.icns
                --ds-store=${DSSTORE_FILE}
        DEPENDS deploy-data
        COMMENT "Generating custom .DS_Store for Drag-and-Drop DMG"
        VERBATIM
    )

    # Copy submodule release notes and RELEASE_NOTES.json into app bundle
    if(_release_notes_targets)
        set(_rn_cmds)
        foreach(_rn_tgt IN LISTS _release_notes_targets)
            list(APPEND _rn_cmds
                COMMAND ${CMAKE_COMMAND} -E copy
                        "${CMAKE_SOURCE_DIR}/${_rn_tgt}/doc/RELEASE_NOTES.md"
                        "${APP_DIR_NEW}/Contents/Resources/doc/RELEASE_NOTES_${_rn_tgt}.md"
            )
        endforeach()
        list(APPEND _rn_cmds
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${_rn_json_path}"
                    "${APP_DIR_NEW}/Contents/Resources/doc/RELEASE_NOTES.json"
        )
        add_custom_target(deploy-submodule-release-notes ALL
            ${_rn_cmds}
            DEPENDS deploy-data
            COMMENT "Copying submodule release notes into bundle"
            VERBATIM
        )
        add_dependencies(gen-dsstore deploy-submodule-release-notes)
    endif()

    # Put openssl tool into Contents/MacOS on macOS ---------------------------
    if (OPENSSL_ROOT)
        # This copies the already-built openssl tool next to the main executable inside the bundle
        add_custom_target(embed-openssl-tool ALL
            COMMAND ${CMAKE_COMMAND} -E copy ${OPENSSL_ROOT}/bin/openssl ${APP_DIR_NEW}/Contents/MacOS/
            COMMAND ${CMAKE_COMMAND} -E copy ${OPENSSL_ROOT}/lib/libcrypto.3.dylib ${APP_DIR_NEW}/Contents/Frameworks/
            COMMAND ${CMAKE_COMMAND} -E copy ${OPENSSL_ROOT}/lib/libssl.3.dylib ${APP_DIR_NEW}/Contents/Frameworks/
            # Fix dylib identities
            COMMAND install_name_tool -id @rpath/libcrypto.3.dylib ${APP_DIR_NEW}/Contents/Frameworks/libcrypto.3.dylib
            COMMAND install_name_tool -id @rpath/libssl.3.dylib ${APP_DIR_NEW}/Contents/Frameworks/libssl.3.dylib
            # Fix libssl -> libcrypto reference (use REAL path to match what's embedded in the binary)
            COMMAND install_name_tool -change ${OPENSSL_ROOT_REAL}/lib/libcrypto.3.dylib @rpath/libcrypto.3.dylib ${APP_DIR_NEW}/Contents/Frameworks/libssl.3.dylib
            # Fix openssl binary references and add rpath
            COMMAND install_name_tool -change ${OPENSSL_ROOT_REAL}/lib/libssl.3.dylib @rpath/libssl.3.dylib ${APP_DIR_NEW}/Contents/MacOS/openssl
            COMMAND install_name_tool -change ${OPENSSL_ROOT_REAL}/lib/libcrypto.3.dylib @rpath/libcrypto.3.dylib ${APP_DIR_NEW}/Contents/MacOS/openssl
            COMMAND install_name_tool -add_rpath @executable_path/../Frameworks ${APP_DIR_NEW}/Contents/MacOS/openssl
            DEPENDS deploy-qt
            COMMENT "Embedding openssl-tool"
            VERBATIM
        )

        add_dependencies(gen-dsstore embed-openssl-tool)
    endif()

    # Explicit sign with Hardened Runtime -------------------------------------
    if(DEFINED CODESIGN_ID)
        add_custom_target(codesign ALL
            COMMAND /bin/echo "Signing with: ${CODESIGN_ID}"
            COMMAND /bin/chmod -R u+w "${APP_DIR_NEW}"
            COMMAND /usr/bin/codesign -vvv --deep --strict --force --options runtime --timestamp --sign "${CODESIGN_ID}" "${APP_DIR_NEW}"
            DEPENDS gen-dsstore
            COMMENT "Codesign"
            VERBATIM
        )
    endif()
else()
    get_filename_component(_qt_dir "${_qt_bin_dir}" DIRECTORY)
    set(DEPLOY_COMMAND
        ${CMAKE_SOURCE_DIR}/range-build-tools/linux_deploy_qt.sh
        --executable=$<TARGET_FILE:${PROJECT_TARGET_NAME}>
        --install-to=${_qt_deploy_dir}
        --only-qt
        --qt-path=${_qt_dir}
    )

    add_custom_target(deploy-qt ALL
        COMMAND ${DEPLOY_COMMAND}
        DEPENDS fea
        COMMENT "Deploying Linux Qt libraries"
        VERBATIM
    )
endif()

# Install ---------------------------------------------------------------------

if(WIN32)
    install(TARGETS fea DESTINATION bin COMPONENT Application)
    install(TARGETS fea-solver DESTINATION bin COMPONENT Application)
    install(FILES "${CMAKE_SOURCE_DIR}/TetGen/LICENSE"
            DESTINATION doc/licenses/TetGen COMPONENT Application)

    get_filename_component(_range_qt_license_default
        "${_qt_bin_dir}/../../../Licenses" ABSOLUTE)
    set(RANGE_QT_LICENSE_DIRECTORY "${_range_qt_license_default}" CACHE PATH
        "Qt supplier-local notice directory; not a complete deployed-module license mapping")
    foreach(_range_qt_notice COPYING.txt Copyright.txt LICENSE LICENSE.FDL LICENSE.GPL3-EXCEPT)
        if(EXISTS "${RANGE_QT_LICENSE_DIRECTORY}/${_range_qt_notice}")
            install(FILES "${RANGE_QT_LICENSE_DIRECTORY}/${_range_qt_notice}"
                    DESTINATION doc/licenses/Qt-sdk COMPONENT Application)
        else()
            message(WARNING "Qt supplier notice missing: ${RANGE_QT_LICENSE_DIRECTORY}/${_range_qt_notice}; final package coverage remains unverified.")
        endif()
    endforeach()
    # Install the Qt 6.11.1 attribution pages for deployed Qt modules and the
    # qtpdf module/plugin copied by Qt deploy; preserve the Qt Docs subdirectories.
    get_filename_component(_range_qt_docs_default
        "${_qt_bin_dir}/../../../Docs/Qt-${Qt6_VERSION}" ABSOLUTE)
    set(RANGE_QT_DOCS_DIRECTORY "${_range_qt_docs_default}" CACHE PATH
        "Qt documentation directory matching the deployed Qt version")
    set(_range_qt_attribution_relative_pages
        qtcore/qtcore-attribution-android-gradle-wrapper.html
        qtcore/qtcore-attribution-blake2.html
        qtcore/qtcore-attribution-doubleconversion.html
        qtcore/qtcore-attribution-easing.html
        qtcore/qtcore-attribution-extra-cmake-modules.html
        qtcore/qtcore-attribution-forkfd.html
        qtcore/qtcore-attribution-kwin.html
        qtcore/qtcore-attribution-md4.html
        qtcore/qtcore-attribution-md5.html
        qtcore/qtcore-attribution-pcre2-sljit.html
        qtcore/qtcore-attribution-pcre2.html
        qtcore/qtcore-attribution-qeventdispatcher-cf.html
        qtcore/qtcore-attribution-rfc6234.html
        qtcore/qtcore-attribution-sha1.html
        qtcore/qtcore-attribution-sha3-endian.html
        qtcore/qtcore-attribution-sha3-keccak.html
        qtcore/qtcore-attribution-siphash.html
        qtcore/qtcore-attribution-tika-mimetypes.html
        qtcore/qtcore-attribution-tinycbor.html
        qtcore/qtcore-attribution-tlexpected.html
        qtcore/qtcore-attribution-unicode-character-database.html
        qtcore/qtcore-attribution-unicode-cldr.html
        qtcore/qtcore-attribution-zlib.html
        qtdoc/qt-attribution-llvm.html
        qtdoc/qt-attribution-llvmpipe.html
        qtgui/qtgui-attribution-aglfn.html
        qtgui/qtgui-attribution-android-native-style.html
        qtgui/qtgui-attribution-cocoa-platform-plugin.html
        qtgui/qtgui-attribution-d3d12memoryallocator.html
        qtgui/qtgui-attribution-dejayvu.html
        qtgui/qtgui-attribution-emoji-segmenter.html
        qtgui/qtgui-attribution-freetype-bdf.html
        qtgui/qtgui-attribution-freetype-pcf.html
        qtgui/qtgui-attribution-freetype-zlib.html
        qtgui/qtgui-attribution-freetype.html
        qtgui/qtgui-attribution-grayraster.html
        qtgui/qtgui-attribution-harfbuzz-ng.html
        qtgui/qtgui-attribution-icc-srgb-color-profile.html
        qtgui/qtgui-attribution-libjpeg.html
        qtgui/qtgui-attribution-libpng.html
        qtgui/qtgui-attribution-md4c.html
        qtgui/qtgui-attribution-opengl-es2-headers.html
        qtgui/qtgui-attribution-opengl-headers.html
        qtgui/qtgui-attribution-pixman.html
        qtgui/qtgui-attribution-rhi-miniengine-d3d12-mipmap.html
        qtgui/qtgui-attribution-smooth-scaling-algorithm.html
        qtgui/qtgui-attribution-vulkan-xml-spec.html
        qtgui/qtgui-attribution-vulkanmemoryallocator.html
        qtgui/qtgui-attribution-webgradients.html
        qtgui/qtgui-attribution-wintab.html
        qtgui/qtgui-attribution-xcb-xinput.html
        qtgui/qtgui-attribution-xserverhelper.html
        qtmultimedia/qtmultimedia-attribution-dr-libs.html
        qtmultimedia/qtmultimedia-attribution-ffmpeg-boost.html
        qtmultimedia/qtmultimedia-attribution-ffmpeg-libjpeg.html
        qtmultimedia/qtmultimedia-attribution-ffmpeg-zlib.html
        qtmultimedia/qtmultimedia-attribution-ffmpeg.html
        qtmultimedia/qtmultimedia-attribution-signalsmith-stretch.html
        qtmultimedia/qtmultimedia-attribution-tlfs.html
        qtnetwork/qtnetwork-attribution-libpsl.html
        qtnetwork/qtnetwork-attribution-psl-data.html
        qtpdf/qtpdf-3rdparty-abseil.html
        qtpdf/qtpdf-3rdparty-fast-float.html
        qtpdf/qtpdf-3rdparty-freetype.html
        qtpdf/qtpdf-3rdparty-icu.html
        qtpdf/qtpdf-3rdparty-libjpeg-turbo.html
        qtpdf/qtpdf-3rdparty-libpng.html
        qtpdf/qtpdf-3rdparty-pdfium.html
        qtpdf/qtpdf-3rdparty-the-chromium-project.html
        qtpdf/qtpdf-3rdparty-zlib.html
        qtsvg/qtsvg-attribution-xsvg.html
    )
    set(_range_qt_attribution_installed_count 0)
    foreach(_range_qt_attribution_relative_path IN LISTS _range_qt_attribution_relative_pages)
        set(_range_qt_attribution_source
            "${RANGE_QT_DOCS_DIRECTORY}/${_range_qt_attribution_relative_path}")
        get_filename_component(_range_qt_attribution_module
            "${_range_qt_attribution_relative_path}" DIRECTORY)
        if(EXISTS "${_range_qt_attribution_source}")
            install(FILES "${_range_qt_attribution_source}"
                    DESTINATION "doc/licenses/Qt-sdk/attribution/${_range_qt_attribution_module}"
                    COMPONENT Application)
            math(EXPR _range_qt_attribution_installed_count
                "${_range_qt_attribution_installed_count} + 1")
        endif()
    endforeach()
    if(NOT _range_qt_attribution_installed_count EQUAL 71)
        message(WARNING
            "Only ${_range_qt_attribution_installed_count} of 71 identified Qt attribution pages are available under ${RANGE_QT_DOCS_DIRECTORY}; notice coverage remains incomplete.")
    endif()
    # Preserve the SDK's original SBOM and embedded extracted-license texts.
    # This is supplier reference data, not a claim that all entries are deployed.
    get_filename_component(_range_qt_kit_dir "${_qt_bin_dir}/.." ABSOLUTE)
    if(IS_DIRECTORY "${_range_qt_kit_dir}/sbom")
        install(DIRECTORY "${_range_qt_kit_dir}/sbom/"
                DESTINATION doc/licenses/Qt-sdk/sbom COMPONENT Application)
    else()
        message(WARNING "Qt SDK SBOM directory missing: ${_range_qt_kit_dir}/sbom; final package coverage remains unverified.")
    endif()
    # This SDK has no separate extracted-license directory; its SPDX JSON
    # hasExtractedLicensingInfos retain those texts inside the copied SBOM.
    if(RANGE_GMSH_RUNTIME)
        install(FILES "${RANGE_GMSH_RUNTIME}" DESTINATION bin COMPONENT Application)
        if(EXISTS "${RANGE_GMSH_ROOT}/share/doc/gmsh/LICENSE.txt")
            install(FILES "${RANGE_GMSH_ROOT}/share/doc/gmsh/LICENSE.txt"
                    DESTINATION doc RENAME LICENSE_GMSH.txt COMPONENT Application)
        endif()
    endif()
    install(DIRECTORY ${PROJECT_TARGET_NAME}/help/ DESTINATION help COMPONENT Application)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/data/ DESTINATION data COMPONENT Application)
    # Install the generated aggregate license below; do not first copy the
    # source document to the same destination and duplicate its manifest path.
    install(DIRECTORY ${PROJECT_TARGET_NAME}/doc/ DESTINATION doc COMPONENT Application
            PATTERN "LICENSE.txt" EXCLUDE)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/etc/ DESTINATION etc COMPONENT Application)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/materials/ DESTINATION materials COMPONENT Application)
    install(DIRECTORY ${_qt_deploy_dir}/ DESTINATION bin COMPONENT Application)
    install(FILES ${CMAKE_BINARY_DIR}/LICENSE.txt DESTINATION doc COMPONENT Application)
    if(_rn_json_path)
        install(FILES ${_rn_json_path} DESTINATION doc COMPONENT Application)
    endif()
    if(OPENSSL_ROOT)
        set(_openssl_exe "${OPENSSL_ROOT}/bin/openssl.exe")
        set(_openssl_crypto "${OPENSSL_ROOT}/bin/libcrypto-3-x64.dll")
        set(_openssl_ssl "${OPENSSL_ROOT}/bin/libssl-3-x64.dll")
        if(EXISTS "${_openssl_exe}" AND EXISTS "${_openssl_crypto}" AND EXISTS "${_openssl_ssl}")
            install(FILES "${_openssl_exe}" DESTINATION bin COMPONENT Application)
            install(FILES "${_openssl_crypto}" DESTINATION bin COMPONENT Application)
            install(FILES "${_openssl_ssl}" DESTINATION bin COMPONENT Application)
            if(EXISTS "${OPENSSL_ROOT}/LICENSE.txt")
                install(FILES "${OPENSSL_ROOT}/LICENSE.txt"
                        DESTINATION doc/licenses/OpenSSL RENAME LICENSE.txt COMPONENT Application)
            else()
                message(FATAL_ERROR "OPENSSL_ROOT is set, but LICENSE.txt is missing: ${OPENSSL_ROOT}/LICENSE.txt")
            endif()
        else()
            message(FATAL_ERROR "OPENSSL_ROOT is set to '${OPENSSL_ROOT}', but required OpenSSL runtime files were not found. Refusing to create an install without the selected OpenSSL runtime.")
        endif()
    endif()

    # MinGW runtime DLLs
    if(MINGW)
        get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(_dll IN ITEMS
                libgomp-1.dll
                libgcc_s_seh-1.dll
                libstdc++-6.dll
                libwinpthread-1.dll)
            if(EXISTS "${_mingw_bin_dir}/${_dll}")
                install(FILES "${_mingw_bin_dir}/${_dll}" DESTINATION bin COMPONENT Application)
            else()
                message(WARNING "MinGW runtime DLL not found, skipping: ${_mingw_bin_dir}/${_dll}")
            endif()
        endforeach()
    endif()
elseif(APPLE)
    install(DIRECTORY ${CMAKE_BINARY_DIR}/${PROJECT_TARGET_NAME}/${PROJECT_SHORT_REL_NAME}.app DESTINATION . USE_SOURCE_PERMISSIONS COMPONENT Application)
else()
    set(CMAKE_INSTALL_PREFIX "/opt/${PROJECT_FAMILY_NAME}/${PROJECT_SHORT_REL_NAME}" CACHE PATH "Install path prefix" FORCE)
    set(CPACK_SET_DESTDIR ON)
    set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

    install(TARGETS fea DESTINATION bin COMPONENT Application)
    install(TARGETS fea-solver DESTINATION bin COMPONENT Application)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/help/ DESTINATION help COMPONENT Application)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/data/ DESTINATION data COMPONENT Application)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/doc/ DESTINATION doc COMPONENT Application)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/etc/ DESTINATION etc COMPONENT Application)
    install(DIRECTORY ${PROJECT_TARGET_NAME}/materials/ DESTINATION materials COMPONENT Application)
    install(DIRECTORY ${_qt_deploy_dir}/ DESTINATION . COMPONENT Application)
    install(FILES ${CMAKE_BINARY_DIR}/LICENSE.txt DESTINATION doc COMPONENT Application)
    install(FILES ${PROJECT_TARGET_NAME}/pixmaps/${PROJECT_PACKAGE_NAME}.svg DESTINATION pixmaps COMPONENT Application)
    if(_rn_json_path)
        install(FILES ${_rn_json_path} DESTINATION doc COMPONENT Application)
    endif()

    # ---- DEB/RPM
    set(DESKTOP_ABS "${CMAKE_BINARY_DIR}/desktop/${PROJECT_PACKAGE_NAME}.desktop")
    set(EXEC_PATH "${CMAKE_INSTALL_PREFIX}/bin/${PROJECT_TARGET_NAME}${DEBUG_EXT}")

    configure_file(
        ${CMAKE_SOURCE_DIR}/range-build-tools/packaging/app.desktop.in
        ${DESKTOP_ABS}
        @ONLY
    )

    install(FILES ${DESKTOP_ABS} DESTINATION desktop COMPONENT Application)
endif()

# CPack -----------------------------------------------------------------------

set(CPACK_COMPONENTS_ALL
    Application
)

set(CPACK_PACKAGE_NAME "${PROJECT_PACKAGE_NAME}${DEBUG_EXT}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_RELEASE "1")
set(CPACK_PACKAGE_CONTACT "${PROJECT_EMAIL}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_WWW}")
set(_qt_kernel "${CMAKE_SYSTEM_NAME}")
string(TOLOWER "${_qt_kernel}" _qt_kernel)
if(_qt_kernel STREQUAL "windows")
  set(_qt_kernel "winnt")
endif()
set(CPACK_SYSTEM_NAME "${_qt_kernel}")
set(_qt_processor "${CMAKE_SYSTEM_PROCESSOR}")
string(TOLOWER "${_qt_processor}" _qt_processor)
if(_qt_processor STREQUAL "AMD64")
  set(_qt_processor "x86_64")
endif()
if(_qt_processor STREQUAL "aarch64")
  set(_qt_processor "arm64")
endif()
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}-${_qt_processor}")

if(APPLE)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.rtf")
else()
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")
endif()
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/../README.md")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "STGZ;DEB;RPM")
    #set(CPACK_GENERATOR "IFW;ZIP;TGZ;STGZ;DEB;RPM")
endif()

# DEB
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "openssl, ca-certificates, libbrotli1, libdbus-1-3, libegl-mesa0, libegl1, libexpat1, libffi8, libfontconfig1, libfreetype6, libglib2.0-0, libglx0, libglx-mesa0, libgl1, libgl1-mesa-dri, libopengl0, libpcre2-8-0, libuuid1, libx11-6, libx11-xcb1, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb-xinerama0, libxcb-xkb1, libxcb1, libxi6, libxkbcommon-x11-0, libxkbcommon0, libxrender1, zlib1g, libgomp1, libpulse0")
#set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.27)")

# RPM
set(CPACK_RPM_PACKAGE_LICENSE "GPLv3")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_REQUIRES "openssl, ca-certificates, brotli-libs, dbus-libs, mesa-libEGL, expat, libffi, fontconfig, freetype, glib2, libglvnd-glx, mesa-libGL, mesa-dri-drivers, libglvnd-opengl, pcre2, libuuid, libX11, libX11-xcb, xcb-util-cursor, xcb-util-wm, xcb-util-image, xcb-util-keysyms, xcb-util-renderutil, libxcb, libXi, libxkbcommon-x11, libxkbcommon, libXrender, zlib, libgomp, pulseaudio-libs")
#set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.27")

# Generate scripts from templates (so you can @ONLY-substitute project vars)
configure_file(${CMAKE_SOURCE_DIR}/range-build-tools/packaging/debian/postinst.in
               ${CMAKE_BINARY_DIR}/packaging/debian/postinst
               @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/range-build-tools/packaging/debian/prerm.in
               ${CMAKE_BINARY_DIR}/packaging/debian/prerm
               @ONLY)

# Debian control scripts MUST be executable
file(CHMOD ${CMAKE_BINARY_DIR}/packaging/debian/postinst PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)
file(CHMOD ${CMAKE_BINARY_DIR}/packaging/debian/prerm    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)

# Tell CPack-DEB to include them
# (generic for whole package)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_BINARY_DIR}/packaging/debian/postinst;${CMAKE_BINARY_DIR}/packaging/debian/prerm")

# If you package by component, use the component-scoped var instead:
# set(CPACK_DEBIAN_APPLICATION_PACKAGE_CONTROL_EXTRA
#     "${CMAKE_BINARY_DIR}/packaging/debian/postinst;${CMAKE_BINARY_DIR}/packaging/debian/prerm")


# RPM scripts (don’t need +x; CPack embeds their contents)
configure_file(${CMAKE_SOURCE_DIR}/range-build-tools/packaging/rpm/post.in
               ${CMAKE_BINARY_DIR}/packaging/rpm/post
               @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/range-build-tools/packaging/rpm/preun.in
               ${CMAKE_BINARY_DIR}/packaging/rpm/preun
               @ONLY)

# Tell CPack-RPM where they are
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_BINARY_DIR}/packaging/rpm/post")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_BINARY_DIR}/packaging/rpm/preun")

# If packaging by component "Application", use component-scoped variants:
# set(CPACK_RPM_APPLICATION_POST_INSTALL_SCRIPT_FILE  "${CMAKE_BINARY_DIR}/packaging/rpm/post")
# set(CPACK_RPM_APPLICATION_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_BINARY_DIR}/packaging/rpm/preun")

# DMG
if(APPLE)
    set(DMG_ICON "${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/pixmaps/${PROJECT_PACKAGE_NAME}.icns")

    set(CPACK_BUNDLE_ICON "${DMG_ICON}")
    set(CPACK_PACKAGE_ICON "${DMG_ICON}")
    set(CPACK_DMG_VOLUME_NAME "${PROJECT_HUMAN_REL_NAME} Installer")
    set(CPACK_DMG_VOLUME_ICON "${DMG_ICON}")
    set(CPACK_DMG_FORMAT "UDZO")
    set(CPACK_DMG_CREATE_APPLICATIONS_LINK "ON")
    set(CPACK_DMG_DS_STORE "${DSSTORE_FILE}")

    # Sign DMG after creation
    if(DEFINED CODESIGN_ID)
        configure_file(
            ${CMAKE_SOURCE_DIR}/range-build-tools/packaging/macos/sign_dmg.cmake.in
            ${CMAKE_BINARY_DIR}/packaging/macos/sign_dmg.cmake
            @ONLY
        )
        set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_BINARY_DIR}/packaging/macos/sign_dmg.cmake")
    endif()
endif()

# Convert files ---------------------------------------------------------------

file(READ "${CMAKE_SOURCE_DIR}/../LICENSE" LICENSE_TEXT)

configure_file(
    ${CMAKE_SOURCE_DIR}/range-build-tools/LICENSE.txt.in
    ${CMAKE_BINARY_DIR}/LICENSE.txt
    @ONLY
)

if(WIN32)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/staging" ISS_STAGING_DIR)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" ISS_OUTPUT_DIR)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/${PROJECT_TARGET_NAME}/pixmaps/${PROJECT_PACKAGE_NAME}.ico" ISS_ICON_FILE)
    configure_file(
        ${CMAKE_SOURCE_DIR}/packaging/windows/setup.iss.in
        ${CMAKE_BINARY_DIR}/setup.iss
        @ONLY
    )

    find_program(ISCC_EXECUTABLE
        NAMES iscc ISCC
        PATHS
            "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
            "$ENV{ProgramFiles}/Inno Setup 6"
            "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
        DOC "Inno Setup Compiler (iscc.exe)"
    )

    if(ISCC_EXECUTABLE)
        configure_file(
            ${CMAKE_SOURCE_DIR}/packaging/windows/run_iscc.cmake.in
            ${CMAKE_BINARY_DIR}/run_iscc.cmake
            @ONLY
        )
        set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_BINARY_DIR}/run_iscc.cmake")
        message(STATUS "Inno Setup found: ${ISCC_EXECUTABLE} — installer .exe will be created by the 'package' target")
    else()
        message(STATUS "Inno Setup (iscc.exe) not found; installer .exe will not be created. Install from https://jrsoftware.org/isinfo.php")
    endif()
endif()

if(APPLE)
    execute_process(
        COMMAND textutil -convert rtf -encoding UTF-8 ${CMAKE_BINARY_DIR}/LICENSE.txt -output ${CMAKE_BINARY_DIR}/LICENSE.rtf
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Failed to generate LICENSE.rtf")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Use the toolset's official redistributable tree for vcomp140.dll. The
    # module installs this Release runtime into bin as part of Application;
    # it must not source a DLL from System32 or debug_nonredist.
    set(CMAKE_INSTALL_OPENMP_LIBRARIES ON)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT Application)
endif()
include(InstallRequiredSystemLibraries)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_range_msvc_openmp_runtime_found FALSE)
    foreach(_range_system_runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        get_filename_component(_range_system_runtime_name "${_range_system_runtime}" NAME)
        if(_range_system_runtime_name MATCHES "^vcomp[0-9]+\\.dll$")
            set(_range_msvc_openmp_runtime_found TRUE)
            break()
        endif()
    endforeach()
    if(NOT _range_msvc_openmp_runtime_found)
        message(FATAL_ERROR
            "The MSVC OpenMP redistributable vcomp DLL was not found. "
            "Install the matching Visual C++ redistributable component before packaging a Release build.")
    endif()
endif()
include(CPack)
