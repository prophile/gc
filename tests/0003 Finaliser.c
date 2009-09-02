#include "framework.h"

int main ()
{
	object obj;
	GC_init();
	obj = NEW();
	RELEASE(obj);
	GC_collect(0);
	ASSERTDEAD(obj);
	ASSERTFINAL(obj);
	obj = NEW();
	GC_collect(0);
	ASSERTLIVE(obj);
	ASSERTNOFINAL(obj);
	GC_terminate(0);
	ASSERTNOFINAL(obj);
	GC_init();
	obj = NEW();
	GC_terminate(1);
	ASSERTFINAL(obj);
	return 0;
}
