/*
 * splc_nmbs.c - builds the vendored nanomodbus.c with SimplePLC's renames and
 * pinned configuration (see splc_nmbs.h). A wrapper translation unit instead
 * of compiler flags, so it is portable and libs/nanomodbus/ stays untouched.
 * nanomodbus.c's own #include "nanomodbus.h" resolves to the vendored header
 * next to it (quoted include: the including file's directory comes first),
 * which the include guard has already seen through splc_nmbs.h.
 */
#include "splc_nmbs.h"
#include "../nanomodbus/nanomodbus.c"