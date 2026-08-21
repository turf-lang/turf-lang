#ifndef CALLGRAPH_H
#define CALLGRAPH_H

#include <string>
#include <map>
#include <set>

class CFG;

class CallGraphPass {
public:
  // Build a call graph from stored CFGs and print a summary
  void runGlobal();
};

// Add a single call edge (caller -> callee)
void AddCallEdge(const std::string &caller, const std::string &callee);

// Clear collected call edges
void ClearCallGraph();

#endif // CALLGRAPH_H
