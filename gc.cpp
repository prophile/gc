#include "gc.h"
#include <set>
#include <map>
#include <vector>
#include <queue>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define GC_DEBUG

#ifdef GC_DEBUG
#define DEBUG(x) x
#define ASSERT(x, msg) { if (!(x)) { printf("[GC] Fatal: %s\n\t%s : %d\n", msg, __FILE__, __LINE__); abort(); } }
#else
#define DEBUG(x)
#define ASSERT(x, msg)
#endif

#define SINGLE_THREADED

namespace
{

class GCObject;

class GCWeakReference;
class GCStrongReference;

inline void Yield ()
{
#ifndef WIN32
	usleep(1);
#else
	Sleep(0);
#endif
}

#define WAITCONDITION(x) { bool __wci_exit = false; \
	for (int __wci = 0; __wci < 100; __wci++)\
	{\
		if (x)\
		{\
			__wci_exit = true;\
			break; \
		}\
	}\
	if (!__wci_exit)\
	{\
		while (!(x))\
			Yield();\
	}\
}

#ifdef SINGLE_THREADED
class GCLock
{
public:
	GCLock () {}
	~GCLock () {}
	void ReadLock () {}
	void ReadUnlock () {}
	void WriteLock () {}
	void WriteUnlock () {}
};
#else
class GCLock
{
private:
	volatile uint32_t status;
public:
	GCLock () : status(0) {}
	~GCLock () {}
	
	void ReadLock ()
	{
		bool haveLock = false;
		while (!haveLock)
		{
			// waiting on a write
			WAITCONDITION((status & 3) == 0);
			uint32_t oldReadCount = status;
			uint32_t newReadCount = status >> 2;
			newReadCount += 1;
			newReadCount <<= 2;
			haveLock = __sync_bool_compare_and_swap(&status, oldReadCount, newReadCount);
			__sync_synchronize();
		}
	}
	
	void ReadUnlock ()
	{
		bool haveUnlock = false;
		while (!haveUnlock)
		{
			uint32_t oldReadCount = status;
			uint32_t newReadCount = status >> 2;
			newReadCount -= 1;
			newReadCount <<= 2;
			haveUnlock = __sync_bool_compare_and_swap(&status, oldReadCount, newReadCount);
			__sync_synchronize();
		}
	}
	
	void WriteLock ()
	{
		bool haveLock = false;
		while (!haveLock)
		{
			__sync_or_and_fetch(&status, 2);
			uint32_t mask = ~3;
			WAITCONDITION(status & mask == 0)
			{
			}
			haveLock = __sync_bool_compare_and_swap(&status, 2, 1);
		}
	}
	
	void WriteUnlock ()
	{
		__sync_and_and_fetch(&status, ~(uint32_t)1);
	}
};
#endif

class GCReference
{
protected:
	GCObject* owner;
	GCObject* target;
public:
	GCReference ( GCObject* anOwner, GCObject* aTarget )
	: owner(anOwner),
	  target(aTarget)
	{
		ASSERT(anOwner, "reference constructed with null owner");
		ASSERT(aTarget, "reference constructed with null target");
	}
	
	virtual ~GCReference ()
	{
	}
	
	GCObject* Owner () { return owner; }
	GCObject* Target () { return target; }
	
	virtual void OwnerDied () = 0;	
	virtual void OwnerDisowned () = 0;
	virtual void TargetDied () = 0;
	virtual bool IsWeak () = 0;
	
	GCWeakReference* WeakReference ();
	GCStrongReference* StrongReference ();
};

class GCWeakReference : public GCReference
{
private:
	void** pointerLocation;
public:
	GCWeakReference ( GCObject* anOwner, GCObject* aTarget, void** aPointerLocation );
	virtual ~GCWeakReference ();
	
	virtual void OwnerDied ();	
	virtual void OwnerDisowned ();
	virtual void TargetDied ();
	
	virtual bool IsWeak () { return true; }
};

class GCStrongReference : public GCReference
{
public:
	GCStrongReference ( GCObject* anOwner, GCObject* aTarget );
	virtual ~GCStrongReference ();
	
	virtual void OwnerDied ();	
	virtual void OwnerDisowned ();
	virtual void TargetDied ();
	
	virtual bool IsWeak () { return false; }
};

GCWeakReference* GCReference::WeakReference ()
{
	if (IsWeak())
		return static_cast<GCWeakReference*>(this);
	else
		return NULL;
}

GCStrongReference* GCReference::StrongReference ()
{
	if (!IsWeak())
		return static_cast<GCStrongReference*>(this);
	else
		return NULL;
}

GCObject* rootObject;
void* rootObjectPointer; // certainly not a valid pointer

class GCObject
{
private:
	void* address;
	void (*finaliser)(void*);
	bool condemned;
public:
	std::set<GCReference*> pointingReferences;
	std::set<GCReference*> ownedReferences;
public:
	GCObject ( void* anAddress, void (*aFinaliser)(void*) )
	: address(anAddress),
	  finaliser(aFinaliser),
	  condemned(false)
	{
		ASSERT(anAddress, "object constructed with null address");
		DEBUG(printf("[GC] +OBJ %p\n", anAddress));
	}
	
	~GCObject ()
	{
		if (finaliser)
			finaliser(address);
		for (std::set<GCReference*>::iterator iter = ownedReferences.begin(); iter != ownedReferences.end(); ++iter)
		{
			ASSERT(*iter, "null reference found in owned reference list");
			(*iter)->OwnerDied();
		}
		for (std::set<GCReference*>::iterator iter = pointingReferences.begin(); iter != pointingReferences.end(); ++iter)
		{
			ASSERT(*iter, "null reference found in pointing reference list");
			(*iter)->TargetDied();
		}
	}
	
	void Condemn ()
	{
		if (condemned)
			return;
		condemned = true;
		delete this;
	}
	
	void* Address ()
	{
		return address;
	}
	
	bool IsReferenced ()
	{
		if (this == rootObject) return true;
		return !pointingReferences.empty();
	}
	
	void* GetPointer () { return address; }
};

std::map<void*, GCObject*> masterObjectSet;
std::map<void*, GCObject*> nurseryObjectSet;

GCLock globalLock;

void DoCollection ( std::map<void*, GCObject*>& field, std::map<void*, GCObject*>& superField )
{
	std::set<GCObject*> referencedObjects;
	std::queue<GCObject*> worklist;
	// root object is the first one to talk to
	worklist.push(rootObject);
	referencedObjects.insert(rootObject);
	// work through list of all referenced objects
	while (!worklist.empty())
	{
		GCObject* target = worklist.front();
		ASSERT(target, "null target in DoCollection");
		worklist.pop();
		// look through all refs owned by this object
		for (std::set<GCReference*>::iterator iter = target->ownedReferences.begin(); iter != target->ownedReferences.end(); ++iter)
		{
			// this discards weak references
			GCStrongReference* sr = (*iter)->StrongReference();
			if (!sr)
				continue;
			// get the target of this
			GCObject* liveObject = sr->Target();
			ASSERT(liveObject, "found null target");
			if (liveObject == target)
				continue; // object is self-referential, early exit
			// if we don't already have it listed, insert it
			if (referencedObjects.find(liveObject) == referencedObjects.end())
			{
				worklist.push(liveObject);
				referencedObjects.insert(liveObject);
			}
		}
	}
	// work through all objects
	for (std::map<void*, GCObject*>::iterator iter = field.begin(); iter != field.end(); ++iter)
	{
		GCObject* target = iter->second;
		ASSERT(target, "found null target in entire object list");
		// is it trivially referenced
		bool isReferenced = referencedObjects.find(target) != referencedObjects.end();
		// if unreferenced, try the superfield?
		if (!isReferenced && !superField.empty())
		{
			for (std::set<GCReference*>::iterator iter = target->pointingReferences.begin(); iter != target->pointingReferences.end(); ++iter)
			{
				GCReference* ref = *iter;
				ASSERT(ref, "found null reference in pointing reference list");
				if (ref->IsWeak())
					continue; // uninterested in weak references
				if (superField.find(ref->Owner()->Address()) != superField.end())
				{
					// we found a reference from the superfield, assume it's referenced
					isReferenced = true;
					break;
				}
			}
		}
		if (!isReferenced)
		{
			// unreferenced
			ASSERT(target != rootObject, "root object ended up unreferenced?");
			DEBUG(printf("[GC] -OBJ %p (not connected to root in object graph)\n", target->Address()));
			target->Condemn();
		}
		else
		{
			superField.insert(*iter);
		}
	}
	field.clear();
}

GCWeakReference::GCWeakReference ( GCObject* anOwner, GCObject* aTarget, void** aPointerLocation )
: GCReference(anOwner, aTarget),
  pointerLocation(aPointerLocation)
{
	ASSERT(aPointerLocation, "null pointer location found for weak reference init");
	DEBUG(printf("[GC] +WR %p => %p (%p)\n", anOwner->Address(), aTarget->Address(), aPointerLocation));
}

GCWeakReference::~GCWeakReference ()
{
	DEBUG(printf("[GC] -WR %p => %p (%p)\n", owner->Address(), target->Address(), pointerLocation));
}

GCStrongReference::GCStrongReference ( GCObject* anOwner, GCObject* aTarget )
: GCReference(anOwner, aTarget)
{
	DEBUG(printf("[GC] +SR %p => %p\n", anOwner->Address(), aTarget->Address()));
}

GCStrongReference::~GCStrongReference ()
{
	DEBUG(printf("[GC] -SR %p => %p\n", owner->Address(), target->Address()));
}

void CollectPartial ()
{
	DoCollection(nurseryObjectSet, masterObjectSet);
}

void CollectFull ()
{
	std::map<void*, GCObject*> aMap;
	DoCollection(masterObjectSet, aMap);
	masterObjectSet.swap(aMap);
}

GCObject* GetObject ( void* ptr )
{
	std::map<void*, GCObject*>::iterator iter = nurseryObjectSet.find(ptr);
	if (iter != nurseryObjectSet.end())
	{
		return iter->second;
	}
	iter = masterObjectSet.find(ptr);
	if (iter != masterObjectSet.end())
	{
		return iter->second;
	}
	ASSERT(0, "GetObject returned 0");
	return NULL;
}

void Unreference ( GCObject* src, GCObject* dst, bool isWeak )
{
	globalLock.WriteLock();
	for (std::set<GCReference*>::iterator iter = src->ownedReferences.begin(); iter != src->pointingReferences.end(); iter++)
	{
		GCReference* ref = *iter;
		ASSERT(ref, "found null reference in owned references list");
		if (ref->Target() != dst) // wrong target
			continue;
		if (ref->IsWeak() != isWeak) // looking for a different type
			continue;
		ref->OwnerDisowned();
		return;
	}
	globalLock.WriteUnlock();
}

void GCWeakReference::OwnerDied ()
{
	std::set<GCReference*>::iterator iter;
	iter = target->pointingReferences.find(this);
	ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
	target->pointingReferences.erase(iter);
	if (!target->IsReferenced())
	{
		DEBUG(printf("[GC] -OBJ %p (completely unreferenced)\n", target->Address()));
		target->Condemn();
	}
	delete this;
}

void GCWeakReference::OwnerDisowned ()
{
	std::set<GCReference*>::iterator iter;
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	iter = target->pointingReferences.find(this);
	ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
	target->pointingReferences.erase(iter);;
	delete this;
}

void GCWeakReference::TargetDied ()
{
	std::set<GCReference*>::iterator iter;
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	*pointerLocation = NULL;
	delete this;
}

void GCStrongReference::OwnerDied ()
{
	std::set<GCReference*>::iterator iter;
	iter = target->pointingReferences.find(this);
	ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
	target->pointingReferences.erase(iter);
	if (!target->IsReferenced())
	{
		DEBUG(printf("[GC] -OBJ %p (completely unreferenced)\n", target->Address()));
		target->Condemn();
	}
	delete this;
}

void GCStrongReference::OwnerDisowned ()
{
	std::set<GCReference*>::iterator iter;
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	iter = target->pointingReferences.find(this);
	ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
	target->pointingReferences.erase(iter);
	delete this;
}

void GCStrongReference::TargetDied ()
{
	// ...the hell?
	std::set<GCReference*>::iterator iter;
	ASSERT(0, "target died with strong reference attached");
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	delete this;
}

}

void GC_init ()
{
	DEBUG(printf("[GC] doing GC init\n"));
	DEBUG(printf("[GC] interesting stats:\n"));
	DEBUG(printf("[GC] \tsizeof(void*) = %d\n", sizeof(void*)));
	DEBUG(printf("[GC] \tsizeof(int) = %d\n", sizeof(int)));
	DEBUG(printf("[GC] \tsizeof(GCObject) = %d\n", sizeof(GCObject)));
	DEBUG(printf("[GC] \tsizeof(GCReference) = %d\n", sizeof(GCReference)));
	DEBUG(printf("[GC] \tsizeof(GCStrongReference) = %d\n", sizeof(GCStrongReference)));
	DEBUG(printf("[GC] \tsizeof(GCWeakReference) = %d\n", sizeof(GCWeakReference)));
	if (sizeof(unsigned long) == 4)
		rootObjectPointer = (void*)0xCAFEBABEUL;
	else
		rootObjectPointer = (void*)0xDEADBEEFFEEDFACEULL;
	rootObject = new GCObject(rootObjectPointer, 0);
	globalLock.WriteLock();
	masterObjectSet[rootObjectPointer] = rootObject;
	globalLock.WriteUnlock();
}

void GC_collect ( bool partial )
{
	globalLock.WriteLock();
	DEBUG(printf("[GC] doing generational collection\n"));
	CollectPartial();
	if (!partial)
	{
		DEBUG(printf("[GC] ... and going on to full collection\n"));
		CollectFull();
	}
	DEBUG(printf("[GC] collection finished\n"));
	globalLock.WriteUnlock();
}

void GC_autorelease ( void* object )
{
	ASSERT(object, "tried to autorelease bad object");
	GC_unregister_reference(rootObjectPointer, object);
}

void GC_register_object ( void* object, void (*finaliser)(void*) )
{
	ASSERT(object, "tried to register bad object");
	GCObject* obj = new GCObject(object, finaliser);
	ASSERT(obj, "could not allocate new GCObject");
	GCStrongReference* reference = new GCStrongReference(rootObject, obj);
	ASSERT(reference, "could not allocate new GCStrongReference");
	obj->pointingReferences.insert(reference);
	globalLock.WriteLock();
	nurseryObjectSet.insert(std::make_pair(object, obj));
	rootObject->ownedReferences.insert(reference);
	globalLock.WriteUnlock();
}

void GC_register_reference ( void* object, void* target )
{
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	ASSERT(src, "could not get source object");
	GCObject* dst = GetObject(target);
	ASSERT(dst, "could not get destination object");
	globalLock.ReadUnlock();
	GCStrongReference* reference = new GCStrongReference(src, dst);
	ASSERT(reference, "could not allocate strong reference");
	globalLock.WriteLock();
	src->ownedReferences.insert(reference);
	dst->pointingReferences.insert(reference);
	globalLock.WriteUnlock();
}

void GC_unregister_reference ( void* object, void* target )
{
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	ASSERT(src, "could not get source object");
	GCObject* dst = GetObject(target);
	ASSERT(dst, "could not get destination object");
	globalLock.ReadUnlock();
	Unreference(src, dst, false);
}

void* GC_root ()
{
	return rootObjectPointer;
}

void GC_register_weak_reference ( void* object, void* target, void** pointer )
{
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	ASSERT(src, "could not get source object");
	GCObject* dst = GetObject(target);
	ASSERT(dst, "could not get destination object");
	globalLock.ReadUnlock();
	GCWeakReference* reference = new GCWeakReference(src, dst, pointer);
	ASSERT(reference, "could not allocate weak reference");
	globalLock.WriteLock();
	src->ownedReferences.insert(reference);
	dst->pointingReferences.insert(reference);
	globalLock.WriteUnlock();
}

void GC_unregister_weak_reference ( void* object, void* target, void** pointer )
{
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	ASSERT(src, "could not get src");
	GCObject* dst = GetObject(target);
	ASSERT(dst, "could not get dst");
	globalLock.ReadUnlock();
	Unreference(src, dst, true);
}
