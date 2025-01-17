/*
 * Copyright (c) 2016, 3 Point X.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __GENERIC_HEAP_H__
#define __GENERIC_HEAP_H__

/** \file generic_heap.h
 *  \ingroup curve_fit
 */

struct Heap;
struct HeapNode;
typedef struct Heap Heap;
typedef struct HeapNode HeapNode;

typedef void (*HeapFreeFP)(void *ptr);

Heap        *HEAP_new(unsigned int tot_reserve);
bool         HEAP_is_empty(const Heap *heap);
void         HEAP_free(Heap *heap, HeapFreeFP ptrfreefp);
void        *HEAP_node_ptr(HeapNode *node);
void         HEAP_remove(Heap *heap, HeapNode *node);
HeapNode    *HEAP_insert(Heap *heap, double value, void *ptr);
void         HEAP_insert_or_update(Heap *heap, HeapNode **node_p, double value, void *ptr);
void        *HEAP_popmin(Heap *heap);
void         HEAP_clear(Heap *heap, HeapFreeFP ptrfreefp);
unsigned int HEAP_size(const Heap *heap);
HeapNode    *HEAP_top(Heap *heap);
double       HEAP_top_value(const Heap *heap);
void         HEAP_node_value_update(Heap *heap, HeapNode *node, double value);
void         HEAP_node_value_update_ptr(Heap *heap, HeapNode *node, double value, void *ptr);
double       HEAP_node_value(const HeapNode *node);

#endif  /* __GENERIC_HEAP_IMPL_H__ */
