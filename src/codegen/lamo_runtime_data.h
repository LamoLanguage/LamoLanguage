#ifndef LAMO_RUNTIME_DATA_H
#define LAMO_RUNTIME_DATA_H

/*
 * lamo_runtime_data.h - Declares the embedded runtime source string.
 *
 * The actual bytes live in lamo_runtime_data.c (auto-generated from
 * lamo_runtime.h by scripts/embed_runtime.py). The codegen module emits
 * this string verbatim at the top of every generated .c program.
 */

extern const char lamo_runtime_source[];

#endif /* LAMO_RUNTIME_DATA_H */
