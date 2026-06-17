#include "CallGraph.h"
#include "CFG.h"
#include "AST.h"
#include <iostream>
#include <map>
#include <set>
#include <mutex>

// Global CFGs declared in CFG.h
extern std::vector<std::unique_ptr<CFG>> GlobalCFGs;

// internal global storage for call edges, protected by mutex
static std::map<std::string, std::set<std::string>> g_callEdges;
static std::mutex g_cg_mutex;

void CallGraphPass::runGlobal() {
  std::lock_guard<std::mutex> lock(g_cg_mutex);
  std::cerr << "[CallGraph] Functions: " << g_callEdges.size() << "\n";
  for (const auto &p : g_callEdges) {
    std::cerr << "[CallGraph] " << p.first << " -> ";
    bool first = true;
    for (const auto &c : p.second) {
      if (!first) std::cerr << ", ";
      std::cerr << c;
      first = false;
    }
    std::cerr << "\n";
  }
}

void AddCallEdge(const std::string &caller, const std::string &callee) {
  std::lock_guard<std::mutex> lock(g_cg_mutex);
  g_callEdges[caller].insert(callee);
}

void ClearCallGraph() {
  std::lock_guard<std::mutex> lock(g_cg_mutex);
  g_callEdges.clear();
}
