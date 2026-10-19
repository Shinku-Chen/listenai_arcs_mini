/*
 * Copyright (C) 2018 Ben Smith
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#ifndef BINJGB_MEMORY_H_
#define BINJGB_MEMORY_H_

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACE_MEMORY 0

#if TRACE_MEMORY

#define xmalloc(size) xmalloc_(__FILE__, __LINE__, size)
#define xrealloc(size) xrealloc_(__FILE__, __LINE__, p, size)
#define xfree(p) xfree_(__FILE__, __LINE__, p)
#define xcalloc(count, size) xcalloc_(__FILE__, __LINE__, count, size)
#define xstrdup(s) xstrdup_(__FILE__, __LINE__, s)

/* Use these instead to make it easier to track memory usage. */
void* xmalloc_(const char* file, int line, size_t);
void* xrealloc_(const char* file, int line, void*, size_t);
void xfree_(const char* file, int line, void*);
void* xcalloc_(const char* file, int line, size_t, size_t);
char* xstrdup_(const char* file, int line, const char*);

#else

/*
 * ARCS-MINI port: binjgb 的所有堆分配都改走 arcade_gbc.c 里的 PSRAM 包装。
 *
 * 原因：本平台 malloc 走内部 SRAM（几十 KB 就满），而模拟器的一份
 * Emulator 结构体近 300KB、ROM 更是 2MB 起，必须放 PSRAM；同时 PSRAM
 * 分配失败在 sysheap.c 里是 CRASH_ON_ALLOC_FAIL → __builtin_trap()，
 * 整机会死，所以包装层要先查余量、失败返回 NULL 让上层优雅报错。
 */
void* arcade_bin_alloc(size_t size);
void* arcade_bin_calloc(size_t count, size_t size);
void* arcade_bin_realloc(void* p, size_t size);
void arcade_bin_free(void* p);

#define xmalloc arcade_bin_alloc
#define xrealloc arcade_bin_realloc
#define xfree arcade_bin_free
#define xcalloc arcade_bin_calloc
#define xstrdup strdup

#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*  BINJGB_MEMORY_H_ */
