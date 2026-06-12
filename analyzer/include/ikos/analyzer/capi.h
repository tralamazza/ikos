/*******************************************************************************
 *
 * \file
 * \brief C API for running the IKOS analyzer in-process
 *
 ******************************************************************************/

#ifndef IKOS_ANALYZER_CAPI_H
#define IKOS_ANALYZER_CAPI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run the analyzer with ikos-analyzer command-line arguments; argv[0] is a
 * program-name placeholder. Returns the ikos-analyzer exit code (0 on
 * success; the result database is written to the path given with -o).
 *
 * Repeated calls in one process are supported, but NOT concurrently: LLVM
 * command-line options are process-wide globals, so callers must serialize
 * invocations. Errors are reported on stderr, like the executable. */
int ikos_analyzer_run(int argc, const char* const* argv);

#ifdef __cplusplus
}
#endif

#endif /* IKOS_ANALYZER_CAPI_H */
