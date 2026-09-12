# Dependencies to run the tests
add_dependencies(build-analyzer-tests ikos-analyzer ikos-pp)

# InterflopBench checkout used by the FP validation test. Not vendored; see
# test/fp/interflopbench/README.md for the one-line sparse clone.
set(INTERFLOPBENCH_DIR "$ENV{HOME}/.cache/interflopbench/examples"
    CACHE PATH "Path to the InterflopBench examples directory")

function(add_analysis_test test_name test_directory)
  add_test(NAME "analysis-${test_name}"
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${test_directory}"
           COMMAND ${PYTHON_EXECUTABLE} runtest
             --clang "${CLANG_EXECUTABLE}"
             --ikos-pp "${FRONTEND_LLVM_IKOS_PP_EXECUTABLE}"
             --ikos-analyzer "$<TARGET_FILE:ikos-analyzer>")
endfunction()

add_analysis_test(buffer-overflow boa)
add_analysis_test(division-by-zero dbz)
add_analysis_test(memory mem)
add_analysis_test(null-pointer-dereference null)
add_analysis_test(assert-prover prover)
add_analysis_test(uninitialized-variable uva)
add_analysis_test(unaligned-pointer upa)
add_analysis_test(signed-int-overflow sio)
add_analysis_test(unsigned-int-overflow uio)
add_analysis_test(shift-count shc)
add_analysis_test(pointer-overflow poa)
add_analysis_test(pointer-compare pcmp)
add_analysis_test(function-call fca)
add_analysis_test(double-free dfa)
add_analysis_test(soundness sound)
add_analysis_test(float fp)

# InterflopBench validation of the FP work (test/fp/interflopbench).
#
# Runs straight out of the build tree via the clang / ikos-pp / ikos-analyzer
# trio, so it needs no `cmake --install` step. SKIP_RETURN_CODE 77 means an
# absent benchmark checkout skips the test instead of failing it: the suite is
# offline-friendly, and a missing third-party repo is an environment gap rather
# than a regression. The test itself exits non-zero on an analyzer crash, a
# trap, or any unsound floating-point verdict.
add_test(NAME "interflopbench"
         COMMAND ${PYTHON_EXECUTABLE}
           "${CMAKE_SOURCE_DIR}/test/fp/interflopbench/run_interflopbench.py"
           --bench-dir "${INTERFLOPBENCH_DIR}"
           --clang "${CLANG_EXECUTABLE}"
           --ikos-pp "${FRONTEND_LLVM_IKOS_PP_EXECUTABLE}"
           --ikos-analyzer "$<TARGET_FILE:ikos-analyzer>"
           --jobs 4)
set_tests_properties(interflopbench PROPERTIES SKIP_RETURN_CODE 77)
