#include "gc.h"
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

// testing framework

static void* __finalisersCalled[2048];
static int __finaliserIndex = 0;

static void __finaliser ( void* ptr )
{
	__finalisersCalled[__finaliserIndex++] = ptr;
}

#define ASSERT(x, message) if (!(x)) { printf("Assertion failure: %s\n\t%s : %d\n", message, __FILE__, __LINE__); abort(); }

typedef void* object;
static object NEW () { object obj = GC_new_object(10, GC_ROOT, __finaliser); return obj; }
static void RELEASE ( object obj ) { GC_unregister_reference(GC_ROOT, obj); }

#define ASSERTLIVE(obj) ASSERT(GC_object_live(obj), "object murdered")
#define ASSERTDEAD(obj) ASSERT(!GC_object_live(obj), "object survived unexpectedly")
#define ASSERTWRZ(wr) ASSERT(wr == NULL, "weak reference pointing to zombie")
#define ASSERTWRL(wr) ASSERT(wr != NULL, "weak reference unexpectedly nullified")
#define ASSERTFINAL(obj) { bool __finaliserF = 0; int __finaliserI; for (__finaliserI = 0; __finaliserI < __finaliserIndex; __finaliserI++) { if (__finalisersCalled[__finaliserI] == obj) { __finaliserF = 1; break; } } ASSERT(__finaliserF, "object finaliser not called"); }
#define ASSERTNOFINAL(obj) { bool __finaliserF = 0; int __finaliserI; for (__finaliserI = 0; __finaliserI < __finaliserIndex; __finaliserI++) { if (__finalisersCalled[__finaliserI] == obj) { __finaliserF = 1; break; } } ASSERT(!__finaliserF, "object finaliser unexpectedly called"); }
