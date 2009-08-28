#include "framework.h"

int main ()
{
	object obj;
	GC_init();
	obj = NEW();
	ASSERTLIVE(obj);
	GC_collect(0);
	ASSERTLIVE(obj);
	GC_autorelease(obj);
	GC_collect(0);
	ASSERTDEAD(obj);
	GC_terminate(0);
	return 0;
}
