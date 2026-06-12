/*******************************************************************************
 *
 * \file
 * \brief main() for the ikos-analyzer executable
 *
 * The whole pipeline lives in analyzer_main() (ikos_analyzer.cpp) so that
 * the C API (capi.cpp) can run it in-process; this translation unit only
 * adds the process-level LLVM setup that must happen exactly once.
 *
 ******************************************************************************/

#include <llvm/Support/InitLLVM.h>

#include <ikos/analyzer/analyzer_main.hpp>

int main(int argc, char** argv) {
  llvm::InitLLVM x(argc, argv);
  return ikos::analyzer::analyzer_main(argc, argv);
}
