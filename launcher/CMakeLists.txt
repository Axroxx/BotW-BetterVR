set(BETTERVR_LAUNCHER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher_common.h
    ${CMAKE_CURRENT_SOURCE_DIR}/openxr_checker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/update_checker.cpp
)

set(BETTERVR_LAUNCHER_EMBED_DIR "${CMAKE_BINARY_DIR}/generated/launcher")
set(BETTERVR_LAUNCHER_EMBED_HEADER "${BETTERVR_LAUNCHER_EMBED_DIR}/embedded_assets.h")
set(BETTERVR_LAUNCHER_EMBED_SOURCE "${BETTERVR_LAUNCHER_EMBED_DIR}/embedded_assets.cpp")
set(BETTERVR_LAUNCHER_EMBED_RC "${BETTERVR_LAUNCHER_EMBED_DIR}/embedded_assets.rc")
set(BETTERVR_LAUNCHER_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/generate_embedded_assets.cmake")

file(GLOB_RECURSE GRAPHIC_PACK_FILES "${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR/*")

add_executable(BetterVR_Launcher ${BETTERVR_LAUNCHER_SOURCES})
target_precompile_headers(BetterVR_Launcher PRIVATE ${CMAKE_SOURCE_DIR}/include/pch.h)
target_include_directories(BetterVR_Launcher BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(BetterVR_Launcher BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(BetterVR_Launcher AFTER PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(BetterVR_Launcher AFTER PRIVATE ${CMAKE_SOURCE_DIR}/dependencies)
target_include_directories(BetterVR_Launcher SYSTEM PRIVATE ${VULKAN_HEADERS_INCLUDE_DIRS})
target_link_libraries(BetterVR_Launcher PRIVATE OpenXR::headers OpenXR::openxr_loader)
target_compile_definitions(BetterVR_Launcher PRIVATE BETTERVR_LAUNCHER_VERSION="${PROJECT_VERSION}")
target_link_options(BetterVR_Launcher PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)

add_custom_command(
    OUTPUT ${BETTERVR_LAUNCHER_EMBED_HEADER} ${BETTERVR_LAUNCHER_EMBED_SOURCE} ${BETTERVR_LAUNCHER_EMBED_RC}
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT_HEADER=${BETTERVR_LAUNCHER_EMBED_HEADER}
        -DOUTPUT_SOURCE=${BETTERVR_LAUNCHER_EMBED_SOURCE}
        -DOUTPUT_RC=${BETTERVR_LAUNCHER_EMBED_RC}
        -DINPUT_DLL=$<TARGET_FILE:BetterVR_Layer>
        -DINPUT_JSON=$<TARGET_FILE_DIR:BetterVR_Layer>/BetterVR_Layer.json
        -DGRAPHIC_PACK_DIR=${CMAKE_SOURCE_DIR}/resources/BreathOfTheWild_BetterVR
        -DVERSION=${PROJECT_VERSION}
        -P ${BETTERVR_LAUNCHER_EMBED_SCRIPT}
    DEPENDS
        BetterVR_Layer
        ${CMAKE_SOURCE_DIR}/resources/BetterVR_Layer.json
        ${BETTERVR_LAUNCHER_EMBED_SCRIPT}
        ${GRAPHIC_PACK_FILES}
    VERBATIM
)

target_sources(BetterVR_Launcher PRIVATE ${BETTERVR_LAUNCHER_EMBED_SOURCE} ${BETTERVR_LAUNCHER_EMBED_HEADER} ${BETTERVR_LAUNCHER_EMBED_RC})
target_include_directories(BetterVR_Launcher PRIVATE ${BETTERVR_LAUNCHER_EMBED_DIR})

install(TARGETS BetterVR_Launcher DESTINATION "${CMAKE_INSTALL_PREFIX}")
install(FILES $<TARGET_PDB_FILE:BetterVR_Launcher> DESTINATION "${CMAKE_INSTALL_PREFIX}" OPTIONAL)
