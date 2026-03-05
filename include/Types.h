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
  TURF_STRING_ARRAY
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
  }
  return "unknown";
}

#endif
