#include "framework.h"

int main ()
{
	object obj1, obj2, obj3;
	GC_init();
	obj1 = NEW();
	obj2 = NEW();
	GC_register_reference(obj1, obj2, NULL);
	GC_autorelease(obj2);
	GC_collect(0);
	ASSERTLIVE(obj1);
	ASSERTLIVE(obj2);
	GC_unregister_reference(obj1, obj2, NULL);
	GC_collect(0);
	ASSERTLIVE(obj1);
	ASSERTDEAD(obj2);
	obj3 = NEW();
	GC_register_reference(obj1, obj3, NULL);
	GC_autorelease(obj3);
	GC_collect(0);
	ASSERTLIVE(obj1);
	ASSERTLIVE(obj3);
	GC_autorelease(obj1);
	GC_collect(0);
	ASSERTDEAD(obj1);
	ASSERTDEAD(obj3);
	GC_terminate(false);
	return 0;
}
