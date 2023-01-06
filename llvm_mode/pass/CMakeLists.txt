
set (CMAKE_CXX_STANDARD 17)
# fix pass bug: https://github.com/sampsyo/llvm-pass-skeleton/issues/7#issuecomment-401834287
set (CMAKE_MODULE_LINKER_FLAGS "${CMAKE_CXX_LINK_FLAGS} -Wl,-znodelete")
if(APPLE)
    # User teor2345 reports that this is required to make things work on MacOS X.
    set (CMAKE_MODULE_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -Wl,-flat_namespace -Wl,-undefined,suppress")
endif(APPLE)

add_library(UnfoldBranchPass MODULE UnfoldBranchPass.cc)
target_link_libraries(UnfoldBranchPass LLVMPassConfig)
install (TARGETS UnfoldBranchPass DESTINATION ${KIRENENKO_PASS_DIR})

add_library(AngoraPass MODULE AngoraPass.cc)
target_link_libraries(AngoraPass LLVMPassConfig)
install (TARGETS AngoraPass DESTINATION ${KIRENENKO_PASS_DIR})

add_library(DFSanPass MODULE DFSanPass.cc)
target_link_libraries(DFSanPass LLVMPassConfig)
install (TARGETS DFSanPass DESTINATION ${KIRENENKO_PASS_DIR})

add_library(TaintPass MODULE TaintPass.cc)
target_link_libraries(TaintPass LLVMPassConfig)
install (TARGETS TaintPass DESTINATION ${KIRENENKO_PASS_DIR})
