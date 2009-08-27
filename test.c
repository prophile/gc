#include "gc.h"
#include <stdlib.h>
#include <assert.h>

int main ()
{
	void* object1;
	void* object2;
	void* object3;
	GC_init();
	object1 = malloc(10);
	GC_register_object(object1, NULL);
	object2 = malloc(10);
	GC_register_object(object2, NULL);
	object3 = malloc(10);
	GC_register_object(object3, NULL);
	GC_register_reference(object1, object2);
	GC_register_reference(object2, object1);
	GC_register_weak_reference(GC_root(), object3, &object3);
	GC_autorelease(object3);
	GC_autorelease(object2);
	GC_collect(1);
	assert(object3 == NULL);
	GC_autorelease(object1);
	GC_collect(1);
	GC_collect(0);
	return 0;
}
