/*******************************************************************************
 *
 * \file
 * \brief C API for running the IKOS analyzer in-process (see capi.h)
 *
 ******************************************************************************/

#include <string>
#include <vector>

#include <ikos/analyzer/capi.h>
#include <ikos/analyzer/analyzer_main.hpp>

extern "C" int ikos_analyzer_run(int argc, const char* const* argv) {
  // analyzer_main() takes the usual mutable argv; copy so the caller can
  // pass borrowed, immutable strings (e.g. from Rust CStrings).
  std::vector< std::string > args(argv, argv + argc);
  std::vector< char* > ptrs;
  ptrs.reserve(args.size() + 1);
  for (auto& arg : args) {
    ptrs.push_back(arg.data());
  }
  ptrs.push_back(nullptr);
  return ikos::analyzer::analyzer_main(static_cast< int >(args.size()),
                                       ptrs.data());
}
