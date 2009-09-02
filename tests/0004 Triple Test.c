#include "framework.h"

int main ()
{
	object obj1, obj2, obj3, o3h;
	GC_init();
	obj1 = NEW();
	obj2 = NEW();
	obj3 = NEW();
	ASSERTLIVE(obj1);
	ASSERTLIVE(obj2);
	ASSERTLIVE(obj3);
	GC_register_reference(obj1, obj2, NULL);
	GC_register_reference(obj2, obj1, NULL);
	o3h = obj3;
	GC_register_weak_reference(GC_ROOT, obj3, &o3h);
	GC_collect(0);
	ASSERTLIVE(obj1);
	ASSERTLIVE(obj2);
	ASSERTLIVE(obj3);
	ASSERTWRL(o3h);
	RELEASE(obj3);
	GC_collect(0);
	ASSERTLIVE(obj1);
	ASSERTLIVE(obj2);
	ASSERTDEAD(obj3);
	ASSERTWRZ(o3h);
	RELEASE(obj2);
	RELEASE(obj1);
	GC_collect(0);
	ASSERTDEAD(obj1);
	ASSERTDEAD(obj2);
	ASSERTDEAD(obj3);
	GC_terminate(0);
	return 0;
}
