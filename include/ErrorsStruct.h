#ifndef ERRORS_STRUCT_H
#define ERRORS_STRUCT_H

#include "Errors.h"

/**
 * @brief A struct instance method call (`x.method(...)`) names a method that
 *        doesn't exist on that struct at all -- not "wrong argument count",
 *        genuinely no method by that name.
 *
 * Before this class existed, TypeChecker::visitMethodCall()'s struct-method
 * branch had a deliberate "be permissive" fallback for this exact case
 * (Node.ResolvedCallTarget simply stayed null, with a comment saying so).
 * That silence propagated all the way to Codegen: MethodCallExprAST::
 * codegen()'s struct-method path does `if (!CalleeF) return nullptr;` with
 * NO diagnostic of its own either. The net effect, confirmed via
 * `l.nonexistentMethod(5)` on a `List<int>`: the compiler reports success,
 * and the method call simply VANISHES from the compiled program -- no
 * error, no warning, no runtime crash, just silently miscompiled code that
 * looks like it worked. This is the struct-method counterpart of
 * UnknownFunctionError (free functions) / the Resolver's module-method
 * UnknownFunctionError usage (`module.method(...)`) -- both of those paths
 * already raised a real error for the equivalent mistake; only the struct
 * path was silently permissive.
 */
class StructMethodNotFoundError : public TurfError {
public:
  StructMethodNotFoundError(SourceLocation Loc, const std::string &StructName,
                            const std::string &MethodName,
                            const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + StructName +
                           Colors::BRIGHT_RED + "' has no method named '" +
                           Colors::CYAN + MethodName + Colors::BRIGHT_RED +
                           "'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    Hint_ = suggestionsOrFallback(Suggestions,
      "Double-check the spelling, or make sure the method is declared as "
      "`fn " + StructName + "." + MethodName + "(...): ReturnType` (instance "
      "method, called as `x." + MethodName + "(...)`) or `fn " + StructName +
      "." + MethodName + "(...) static: ReturnType` (static method, called "
      "as `" + StructName + "::" + MethodName + "(...)`).");
    trySetDeterministicRename(MethodName, Suggestions);
  }
};
/**
 * @brief A struct field access (`x.field`) names a field that doesn't exist
 *        on that struct -- the field-access counterpart of
 *        StructMethodNotFoundError above, and the concrete target of issue
 *        #241 Track D's "Did You Mean?" extension to struct fields
 *        (`user.pasword` -> `user.password`).
 *
 * Before this class existed, TypeChecker::visitFieldAccess() silently left
 * the node's type null for an unknown field (a deliberate permissive
 * fallback, same shape StructMethodNotFoundError's own doc comment
 * describes for methods) and let compilation continue -- the only place
 * that ever caught the mistake was CodegenStruct.cpp's raw LogErrorAt() at
 * actual codegen time, with no error code, no category, and critically no
 * suggestions at all, even though the exact same SuggestionIndex trie every
 * other "unknown name" diagnostic in this compiler already uses was sitting
 * right there unused. Raising this earlier, at TypeCheck time, also means a
 * bad field access is now caught before Codegen ever runs, same as every
 * other TypeChecker-owned class of mistake -- CodegenStruct.cpp's own check
 * stays in place as a defensive fallback for the CodegenExpr-only-nodes
 * case (see calling code), not because this path is expected to be reached
 * anymore.
 */
class UnknownStructFieldError : public TurfError {
public:
  UnknownStructFieldError(SourceLocation Loc, const std::string &StructName,
                          const std::string &FieldName,
                          const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "Struct '" + Colors::CYAN +
                           StructName + Colors::BRIGHT_RED +
                           "' has no field named '" + Colors::CYAN +
                           FieldName + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    Hint_ = suggestionsOrFallback(
        Suggestions, "Double-check the spelling, or make sure '" +
                         FieldName + "' is declared as a field on '" +
                         StructName + "'.");
    trySetDeterministicRename(FieldName, Suggestions);
  }
};
// `StructName::new(...)` was called but `StructName` has no `.new`
// constructor declared at all -- unlike a static method, `new` has no
// generic "not found" fallback because it isn't looked up by an arbitrary
// user-chosen name; either it exists or construction-by-arguments simply
// isn't available for this struct.
class NoConstructorDefinedError : public TurfError {
public:
  NoConstructorDefinedError(SourceLocation Loc, const std::string &StructName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + StructName +
                           Colors::BRIGHT_RED +
                           "' has no constructor -- '" + Colors::CYAN +
                           StructName + "::new(...)" + Colors::BRIGHT_RED +
                           "' can't be called." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "Define one with `fn " + StructName +
            ".new(...): void { self.field = ...; }`, or use `" + StructName +
            "::default()` if you just want every field zero-initialized.";
  }
};
// `fn StructName.default(...)` declared by hand -- `default` is always
// compiler-synthesized (recursively default-initializes every field) and
// can never have user code attached to it.
class ReservedConstructorNameError : public TurfError {
public:
  ReservedConstructorNameError(SourceLocation Loc, const std::string &StructName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + "default" +
                           Colors::BRIGHT_RED +
                           "' is reserved and can't be declared by hand." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'" + StructName + "::default()' is always compiler-generated "
            "(every field recursively zero-initialized) -- remove this "
            "declaration. Define '" + StructName +
            ".new(...)' instead if you need custom construction logic.";
  }
};
// `static` written on a function that isn't struct-bound at all (no
// 'StructName.' prefix) -- meaningless, since a free function never had a
// `self` to omit in the first place.
class StaticOutsideStructError : public TurfError {
public:
  StaticOutsideStructError(SourceLocation Loc, const std::string &FuncName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + "static" +
                           Colors::BRIGHT_RED +
                           "' can only be used on a struct-bound method." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'" + FuncName + "' isn't declared as 'StructName." + FuncName +
            "(...)', so there's no 'self' to omit. Remove 'static', or "
            "declare it as a method: 'fn StructName." + FuncName +
            "(...) static: ReturnType { ... }'.";
  }
};
// `fn StructName.new(...) static: ...` -- 'new' always constructs via
// `self` (StructName::new(...) allocates a fresh instance and passes it as
// self), so 'static' -- which means no self at all -- contradicts its
// entire purpose.
class ConstructorCannotBeStaticError : public TurfError {
public:
  ConstructorCannotBeStaticError(SourceLocation Loc, const std::string &StructName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + "new" +
                           Colors::BRIGHT_RED + "' can't be '" + Colors::CYAN +
                           "static" + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'" + StructName + ".new' always initializes 'self' -- remove "
            "'static' from its declaration.";
    withWordDeletionFixIt(
        "static", "'" + StructName + ".new' always initializes 'self', and "
                  "'static' means there is no 'self' -- removing 'static' is "
                  "the fix.");
  }
};
// `StructName::default(x, ...)` -- default() is always zero-argument; it
// has no user code to pass arguments to at all.
class DefaultConstructorArityError : public TurfError {
public:
  DefaultConstructorArityError(SourceLocation Loc, const std::string &StructName,
                               int Got)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + StructName +
                           "::default()" + Colors::BRIGHT_RED +
                           "' takes no arguments, but got " +
                           std::to_string(Got) + "." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::FUNCTION_SIGNATURE;
    Hint_ = "'" + StructName + "::default()' always zero-initializes every "
            "field with no input -- use '" + StructName +
            "::new(...)' if you need to pass values in.";
  }
};
// `instance.new(...)` / `instance.default(...)` -- these are reserved for
// construction (StructName::new(...)/StructName::default()) and can never
// be reached through an existing instance with '.', even though 'new' is
// otherwise an ordinary '.'-declared instance method internally.
class ConstructorCalledOnInstanceError : public TurfError {
public:
  ConstructorCalledOnInstanceError(SourceLocation Loc, const std::string &MethodName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + MethodName +
                           Colors::BRIGHT_RED +
                           "' can't be called on an instance with '.'." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'" + MethodName + "' only works as 'StructName::" + MethodName +
            "(...)', never through an existing instance.";
  }
};
// `new()` doesn't assign every field of its struct on all paths -- a real,
// control-flow-sensitive definite-assignment check (ConstructorFieldAssignmentPass,
// see Analysis.h/Analysis.cpp), not just "never assigned anywhere". Raised
// once per constructor, naming every field missed on at least one path.
class IncompleteConstructorError : public TurfError {
public:
  IncompleteConstructorError(SourceLocation Loc, const std::string &StructName,
                             const std::vector<std::string> &MissingFields)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + StructName +
                           ".new" + Colors::BRIGHT_RED +
                           "' doesn't assign every field on all paths." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    std::string Fields;
    for (size_t i = 0; i < MissingFields.size(); ++i) {
      if (i) Fields += ", ";
      Fields += MissingFields[i];
    }
    Message += "\n  " + Colors::BRIGHT_GREEN +
               "Not definitely assigned on every path: " + Colors::RESET +
               Colors::CYAN + Fields + Colors::RESET;
    Hint_ = "Every field of '" + StructName + "' must be set via "
            "'self.<field> = ...' on every path through 'new' -- including "
            "inside every if/else branch and before any early return.";
  }
};
// Raised when '.' -- reserved for instance-member access -- is used on a
// base that names a MODULE (`io.printline(...)`) or a STRUCT TYPE used
// bare, i.e. a static-style call with no instance (`StructName.method(...)`)
// -- instead of the required '::' (`io::printline(...)` /
// `StructName::method(...)`).
//
// Before this check existed, both cases had their own separate, fully
// functional legacy resolution paths (VariableExprAST::ResolvedModuleName
// for the module case, ::IsStructName for the struct case) that silently
// let '.' work as an undocumented ALIAS for '::' -- directly contradicting
// this language's stated, otherwise-consistent design ("'.' is instance-
// member-only, '::' is the general namespace-resolution operator", see
// docs/architecture/modules_and_scope_resolution.md section 1.1).
// Confirmed live: `io.printline(...)` and a `::`-declared static method
// called as `StructName.method(...)` both compiled and ran successfully
// before this fix, identically to their correct `::` counterparts.
class DotOperatorOnNonInstanceError : public TurfError {
public:
  DotOperatorOnNonInstanceError(SourceLocation Loc, const std::string &Name,
                                const std::string &Kind,
                                const std::string &MethodName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'.' can't be used here -- '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED +
                           "' is a " + Kind + ", not an instance." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'.' is for instance-member access only. Use '" + Name + "::" +
            MethodName + "(...)' instead.";
    // The hint spells the exact spelling out, so --smart writes it for us.
    withDeterministicFix(Name + "." + MethodName, Name + "::" + MethodName,
                         "'.' is for instance-member access only, and '" + Name +
                             "' is a " + Kind + ", not an instance -- a call "
                             "on it is written with '::'.");
  }
};
// The reverse of DotOperatorOnNonInstanceError: the base IS a real
// instance, but the named method is a TRUE static method (declared with
// 'static', no 'self' parameter at all). A '.' call always implies passing
// self, so the method's declared parameter list has one fewer slot than
// the call site assumes -- e.g. `mu.square(5)` where `square` takes only
// `(x: int)`. Before this check existed, that mismatch fell through to the
// generic WrongArityError, which described it self-contradictorily: "was
// called with 1 argument, but no overload accepts that many" printed right
// next to a listed candidate that visibly DOES take exactly 1 argument --
// the implicit self silently added by '.' was never mentioned anywhere.
class StaticMethodCalledOnInstanceError : public TurfError {
public:
  StaticMethodCalledOnInstanceError(SourceLocation Loc,
                                    const std::string &StructName,
                                    const std::string &MethodName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + MethodName +
                           Colors::BRIGHT_RED +
                           "' is a static method -- it can't be called "
                           "through an instance with '.'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'" + StructName + "." + MethodName + "' takes no implicit "
            "'self' (it's declared 'static'). Call it as '" + StructName +
            "::" + MethodName + "(...)' instead.";
  }
};

/**
 * @brief A struct contains itself by value, either directly (`struct A { me:
 *        A }`) or through a chain of other structs/arrays/generic
 *        instantiations (`struct A { b: B } struct B { a: A }`) -- an
 *        infinitely-sized type. TYPE-001
 *        (docs/planning/ARCHITECTURE_OVERHAUL.md §8, INV-LAYOUT-1).
 *
 * Before this check existed, BOTH of the examples above compiled with zero
 * diagnostics. The declaration alone never crashed anything (no layout is
 * computed just from registering a name), but the moment anything tried to
 * INSTANTIATE the type (`mut x: A;`) the compiler itself segfaulted --
 * confirmed live: exit 139, no output at all, reproducible as early as
 * `turf --resolve` on `struct A { me: A }` alone. Raising this at the end
 * of Resolver Pass 1 -- after every struct name is registered, before
 * anything downstream (layout, default-value construction, Codegen) ever
 * looks at a field -- turns that native crash into an ordinary diagnostic,
 * and is the one thing every later layout/default/GC-descriptor computation
 * in this compiler is allowed to assume never needs to happen (INV-LAYOUT-1).
 *
 * A *pointer* field (`next: Node*`, `prev: &Node`) is deliberately never an
 * edge in this check -- see checkStructLayoutAcyclicity()'s doc comment for
 * the full non-edge list. Recursive data structures written with a pointer
 * field remain completely unaffected; only by-value self-containment (which
 * has no finite size, and never worked beyond declaration anyway) is
 * rejected.
 */
class StructLayoutCycleError : public TurfError {
public:
  /**
   * @param Loc        Where the diagnostic points -- the declaration of the
   *                    struct at which the cycle was detected (CyclePath[0]).
   * @param StructName  Same struct's name, repeated for the headline message
   *                    (CyclePath[0], kept as a separate parameter rather
   *                    than re-indexing CyclePath so the constructor reads
   *                    correctly even if CyclePath's shape ever changes).
   * @param CyclePath   The full cycle, in traversal order, WITH the closing
   *                    repeat of CyclePath[0] at the end -- e.g. {"A", "B",
   *                    "A"} for a mutual A/B cycle, or {"A", "A"} for a
   *                    direct self-cycle. Rendered verbatim as "A -> B -> A".
   */
  StructLayoutCycleError(SourceLocation Loc, const std::string &StructName,
                         const std::vector<std::string> &CyclePath)
      : TurfError(Loc, Colors::BRIGHT_RED + "struct '" + Colors::CYAN +
                           StructName + Colors::BRIGHT_RED +
                           "' cannot contain itself by value." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    std::string Path;
    for (size_t i = 0; i < CyclePath.size(); ++i) {
      if (i) Path += Colors::BRIGHT_GREEN + " -> " + Colors::CYAN;
      Path += CyclePath[i];
    }
    Message += "\n  " + Colors::BRIGHT_GREEN + "cycle: " + Colors::CYAN +
               Path + Colors::RESET;
    Hint_ = "Make one of these fields a reference or pointer ('&" +
            StructName + "', '&?" + StructName + "', '" + StructName +
            "*') instead of holding the struct by value -- a struct that "
            "contains itself by value has no finite size.";
  }
};

#endif
