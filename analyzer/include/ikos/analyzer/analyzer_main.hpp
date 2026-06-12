/*******************************************************************************
 *
 * \file
 * \brief Entry point of the analyzer pipeline, shared by the ikos-analyzer
 * executable and the C API (capi.cpp)
 *
 ******************************************************************************/

#pragma once

namespace ikos {
namespace analyzer {

/// \brief Run the full analyzer pipeline with ikos-analyzer's command line
///
/// This is main() minus llvm::InitLLVM: it parses \p argv with LLVM's
/// command-line machinery (resetting any occurrences left by a previous
/// call, so repeated in-process invocations work) and writes the result
/// database to the path given with -o.
///
/// Not thread-safe: LLVM command-line options are process-wide globals, so
/// callers must serialize invocations.
///
/// \returns the ikos-analyzer exit code (0 on success)
int analyzer_main(int argc, char** argv);

} // end namespace analyzer
} // end namespace ikos
