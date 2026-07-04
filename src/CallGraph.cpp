#include "CallGraph.h"
#include "CFG.h"
#include "AST.h"
#include <iostream>
#include <map>
#include <set>
#include <mutex>

// internal global storage for call edges, protected by mutex
static std::map<std::string, std::set<std::string>> g_callEdges;
static std::mutex g_cg_mutex;

void CallGraphPass::runGlobal() {
  std::lock_guard<std::mutex> lock(g_cg_mutex);
  // Function is currently just a stub.
}

void AddCallEdge(const std::string &caller, const std::string &callee) {
  std::lock_guard<std::mutex> lock(g_cg_mutex);
  g_callEdges[caller].insert(callee);
}

void ClearCallGraph() {
  std::lock_guard<std::mutex> lock(g_cg_mutex);
  g_callEdges.clear();
}
