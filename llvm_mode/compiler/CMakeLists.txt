
add_executable(KOClang ko_clang.c)
set_target_properties(KOClang PROPERTIES OUTPUT_NAME "ko-clang")

get_filename_component(Z3_LIB_DIR
                       "${Z3_CXX_INCLUDE_DIRS}/../lib"
                       ABSOLUTE)
target_compile_definitions(KOClang PRIVATE Z3_LIB_DIR="${Z3_LIB_DIR}")

add_custom_command(TARGET KOClang POST_BUILD
    COMMAND ln -sf "ko-clang" "ko-clang++")
install (TARGETS KOClang DESTINATION ${KIRENENKO_BIN_DIR})
install (FILES ${CMAKE_CURRENT_BINARY_DIR}/ko-clang++ DESTINATION ${KIRENENKO_BIN_DIR})
