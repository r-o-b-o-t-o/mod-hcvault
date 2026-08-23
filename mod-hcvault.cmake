# nlohmann/json is vendored under libs/ so the module
# can be built against a core that does not ship it
target_include_directories(modules PUBLIC "mod-hcvault/libs")

# Request bodies are gzipped before they go out; see HcVaultHttpClient.cpp. zlib is already in the
# worldserver's link line by way of g3dlib, but naming it here is what puts <zlib.h> on this target's
# include path and stops the module depending on somebody else's transitive dependency.
target_link_libraries(modules PRIVATE zlib)

# The beast HTTP and TLS stream templates instantiate into more COMDAT sections than MSVC's default
# object format allows (fatal error C1128). AzerothCore only adds /bigobj to CMAKE_CXX_FLAGS_DEBUG,
# so every other configuration has to opt in here.
if(MSVC)
  file(GLOB_RECURSE MOD_HCVAULT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/mod-hcvault/src/*.cpp")
  set_source_files_properties(${MOD_HCVAULT_SOURCES} PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()
