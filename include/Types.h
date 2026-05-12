#ifndef TYPES_H
#define TYPES_H

enum TurfType {
  TURF_VOID,
  TURF_DOUBLE,
  TURF_INT,
  TURF_BOOL,
  TURF_STRING,
  TURF_INT_ARRAY,
  TURF_DOUBLE_ARRAY,
  TURF_BOOL_ARRAY,
  TURF_STRING_ARRAY,
  // Dynamically-sized generic list<T>
  TURF_INT_LIST,
  TURF_DOUBLE_LIST,
  TURF_BOOL_LIST,
  TURF_STRING_LIST
};

inline bool isArrayType(TurfType T) {
  return T == TURF_INT_ARRAY || T == TURF_DOUBLE_ARRAY ||
         T == TURF_BOOL_ARRAY || T == TURF_STRING_ARRAY;
}

inline TurfType getArrayElementType(TurfType T) {
  switch (T) {
  case TURF_INT_ARRAY:    return TURF_INT;
  case TURF_DOUBLE_ARRAY: return TURF_DOUBLE;
  case TURF_BOOL_ARRAY:   return TURF_BOOL;
  case TURF_STRING_ARRAY: return TURF_STRING;
  default:                return T;
  }
}

inline TurfType getArrayType(TurfType ElemT) {
  switch (ElemT) {
  case TURF_INT:    return TURF_INT_ARRAY;
  case TURF_DOUBLE: return TURF_DOUBLE_ARRAY;
  case TURF_BOOL:   return TURF_BOOL_ARRAY;
  case TURF_STRING: return TURF_STRING_ARRAY;
  default:          return TURF_VOID;
  }
}

// ── List helpers ──────────────────────────────────────────────────────────────

inline bool isListType(TurfType T) {
  return T == TURF_INT_LIST || T == TURF_DOUBLE_LIST ||
         T == TURF_BOOL_LIST || T == TURF_STRING_LIST;
}

inline TurfType getListElementType(TurfType T) {
  switch (T) {
  case TURF_INT_LIST:    return TURF_INT;
  case TURF_DOUBLE_LIST: return TURF_DOUBLE;
  case TURF_BOOL_LIST:   return TURF_BOOL;
  case TURF_STRING_LIST: return TURF_STRING;
  default:               return T;
  }
}

inline TurfType getListType(TurfType ElemT) {
  switch (ElemT) {
  case TURF_INT:    return TURF_INT_LIST;
  case TURF_DOUBLE: return TURF_DOUBLE_LIST;
  case TURF_BOOL:   return TURF_BOOL_LIST;
  case TURF_STRING: return TURF_STRING_LIST;
  default:          return TURF_VOID;
  }
}

// Human-readable name for a TurfType (used in diagnostics)
inline const char *turfTypeName(TurfType T) {
  switch (T) {
  case TURF_INT:          return "int";
  case TURF_DOUBLE:       return "double";
  case TURF_BOOL:         return "bool";
  case TURF_STRING:       return "string";
  case TURF_VOID:         return "void";
  case TURF_INT_ARRAY:    return "int[]";
  case TURF_DOUBLE_ARRAY: return "double[]";
  case TURF_BOOL_ARRAY:   return "bool[]";
  case TURF_STRING_ARRAY: return "string[]";
  case TURF_INT_LIST:     return "list<int>";
  case TURF_DOUBLE_LIST:  return "list<double>";
  case TURF_BOOL_LIST:    return "list<bool>";
  case TURF_STRING_LIST:  return "list<string>";
  }
  return "unknown";
}

#endif
