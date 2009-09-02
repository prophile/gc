#include "framework.h"

int main ()
{
	object obj, handle;
	GC_init();
	obj = NEW();
	handle = obj;
	GC_register_weak_reference(GC_ROOT, obj, &handle);
	GC_collect(0);
	ASSERTLIVE(obj);
	ASSERTWRL(handle);
	RELEASE(obj);
	GC_collect(0);
	ASSERTDEAD(obj);
	ASSERTWRZ(handle);
	GC_terminate(0);
	return 0;
}
