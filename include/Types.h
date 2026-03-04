#ifndef TYPES_H
#define TYPES_H

enum TurfType { TURF_VOID, TURF_DOUBLE, TURF_INT, TURF_BOOL, TURF_STRING };

// Human-readable name for a TurfType (used in diagnostics)
inline const char *turfTypeName(TurfType T) {
  switch (T) {
  case TURF_INT:    return "int";
  case TURF_DOUBLE: return "double";
  case TURF_BOOL:   return "bool";
  case TURF_STRING: return "string";
  case TURF_VOID:   return "void";
  }
  return "unknown";
}

#endif
