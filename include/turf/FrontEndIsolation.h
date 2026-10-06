/**
 * @file turf/FrontEndIsolation.h
 * @brief Internal: run a piece of front-end work (lexing a buffer, say) with
 *        every global the pipeline touches saved and put back around it.
 * @details NOT part of the tool API in turf/Analysis.h and not covered by its
 *          stability promise. It exists for the compiler's own code that has
 *          to drive the lexer on text it did not come from a compile with --
 *          the formatter -- so that the one place that knows what state the
 *          front end keeps (GlobalStateScope, src/AnalysisFacade.cpp) is also
 *          the only place that saves and restores it (INV-API-1,
 *          scripts/check_boundary.sh check 7). A second hand-written
 *          save/restore is the thing SMP-001 removed: it silently goes stale
 *          whenever the pipeline gains a global.
 *
 *          The work starts from a fresh-process state (empty diagnostics, a
 *          fresh symbol table, no recovery guard armed) and is serialized with
 *          analyze() by the same mutex. It may call itself or analyze() on the
 *          same thread.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#ifndef TURF_FRONT_END_ISOLATION_H
#define TURF_FRONT_END_ISOLATION_H

#include <functional>

namespace turf::detail {

/// Runs @p Work with all front-end global state isolated, then restores it
/// exactly as it was, whether @p Work returns or a `setjmp` it armed itself
/// was jumped to. Anything @p Work leaves in the globals is discarded.
void runIsolated(const std::function<void()> &Work);

} // namespace turf::detail

#endif
