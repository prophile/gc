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

//#define GC_DEBUG

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

bool shuttingDown = false;
bool disableFinalisers = false;
bool disableTrivialExecution = false;

static void DefaultWeakInvalidator ( void* source, void** pointer )
{
	*pointer = NULL;
}

void (*weakInvalidator)(void*, void**) = DefaultWeakInvalidator;

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
static bool AtomicCAS ( volatile uint32_t* ptr, uint32_t oldVal, uint32_t newVal )
{
	return __sync_bool_compare_and_swap(ptr, oldVal, newVal);
}

static void AtomicFence ()
{
	__sync_synchronize();
}

static bool AtomicBitwise ( volatile uint32_t* ptr, uint32_t set, uint32_t clear, bool tryOnly = false )
{
	uint32_t oldVal, newVal;
	do
	{
		oldVal = *ptr;
		newVal = oldVal | set;
		newVal = newVal & ~clear;
		if (tryOnly)
		{
			return AtomicCAS(ptr, oldVal, newVal);
		}
	} while (!AtomicCAS(ptr, oldVal, newVal));
	return true;
}

static bool AtomicSetBits ( volatile uint32_t* ptr, uint32_t bits, bool tryOnly = false )
{
	return AtomicBitwise(ptr, bits, 0, tryOnly);
}

static bool AtomicClearBits ( volatile uint32_t* ptr, uint32_t bits, bool tryOnly = false )
{
	return AtomicBitwise(ptr, 0, bits, tryOnly);
}

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
			haveLock = AtomicCAS(&status, oldReadCount, newReadCount);
			AtomicFence();
		}
		DEBUG(printf("[GC] +LK RD\n"));
	}
	
	void ReadUnlock ()
	{
		bool haveUnlock = false;
		while (!haveUnlock)
		{
			uint32_t oldReadCount = status;
			uint32_t newReadCount = status >> 2;
			ASSERT(newReadCount > 0, "unlocking already unlocked read lock");
			newReadCount -= 1;
			newReadCount <<= 2;
			haveUnlock = AtomicCAS(&status, oldReadCount, newReadCount);
			AtomicFence();
		}
		DEBUG(printf("[GC] -LK RD\n"));
	}
	
	void WriteLock ()
	{
		bool haveLock = false;
		const uint32_t MASK = ~3;
		while (!haveLock)
		{
			AtomicSetBits(&status, 2);
			WAITCONDITION((status & MASK) == 0);
			haveLock = AtomicBitwise(&status, 1, 2, true);
			AtomicFence();
		}
		DEBUG(printf("[GC] +LK WR\n"));
	}
	
	void WriteUnlock ()
	{
		ASSERT(status & 1, "unlocked already unlocked write lock");
		AtomicClearBits(&status, 1);
		AtomicFence();
		DEBUG(printf("[GC] -LK WR\n"));
	}
};
#endif

class GCReference
{
protected:
	GCObject* owner;
	GCObject* target;
	void** pointerLocation;
public:
	GCReference ( GCObject* anOwner, GCObject* aTarget, void** aPointerLocation )
	: owner(anOwner),
	  target(aTarget),
	  pointerLocation(aPointerLocation)
	{
		ASSERT(anOwner, "reference constructed with null owner");
		ASSERT(aTarget, "reference constructed with null target");
	}
	
	virtual ~GCReference ()
	{
	}
	
	void** PointerLocation () const { return pointerLocation; }
	GCObject* Owner () const { return owner; }
	GCObject* Target () const { return target; }
	
	virtual void OwnerDied () = 0;	
	virtual void OwnerDisowned () = 0;
	virtual void TargetDied () = 0;
	virtual bool IsWeak () const = 0;
	
	GCWeakReference* WeakReference ();
	GCStrongReference* StrongReference ();
};

class GCWeakReference : public GCReference
{
public:
	GCWeakReference ( GCObject* anOwner, GCObject* aTarget, void** aPointerLocation );
	virtual ~GCWeakReference ();
	
	virtual void OwnerDied ();	
	virtual void OwnerDisowned ();
	virtual void TargetDied ();
	
	virtual bool IsWeak () const { return true; }
};

class GCStrongReference : public GCReference
{
public:
	GCStrongReference ( GCObject* anOwner, GCObject* aTarget, void** aPointerLocation );
	virtual ~GCStrongReference ();
	
	virtual void OwnerDied ();	
	virtual void OwnerDisowned ();
	virtual void TargetDied ();
	
	virtual bool IsWeak () const { return false; }
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

class GCObject
{
private:
	void* address;
	void (*finaliser)(void*);
	bool condemned;
	size_t selfAssignedLength;
public:
	std::set<GCReference*> pointingReferences;
	std::set<GCReference*> ownedReferences;
public:
	GCObject ( void* anAddress, void (*aFinaliser)(void*), size_t selfAssignedLen )
	: address(anAddress),
	  finaliser(aFinaliser),
	  condemned(false),
	  selfAssignedLength(selfAssignedLen)
	{
		ASSERT(anAddress, "object constructed with null address");
		DEBUG(printf("[GC] +OBJ %p\n", anAddress));
	}
	
	~GCObject ()
	{
		if (finaliser && !disableFinalisers)
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
		if (selfAssignedLength > 0)
		{
			free(address);
		}
	}
	
	void Migrate ( void* newTarget );
	
	void Resize ( size_t len )
	{
		ASSERT(selfAssignedLength, "tried to resize non-GC-allocated object");
		void* newAddress = realloc(address, len);
		selfAssignedLength = len;
		if (newAddress != address)
		{
			Migrate(newAddress);
		}
	}
	
	unsigned long GetLength () { return selfAssignedLength; }
	
	void Condemn ( GCReference* lastReference );
	bool IsCondemned () { return condemned; }
	
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

class GCField
{
private:
	GCObject* LookupRecursive ( void* ptr )
	{
		std::map<void*, GCObject*>::iterator iter = field.find(ptr);
		if (iter != field.end())
			return iter->second;
		else
			if (parent)
				return parent->LookupRecursive(ptr);
			else
				return NULL;
	}
	
	static void DoCollection ( std::map<void*, GCObject*>& field, std::map<void*, GCObject*>& targetField, GCField* parent )
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
				{
					continue;
				}
				// get the target of this
				GCObject* liveObject = sr->Target();
				ASSERT(liveObject, "found null target");
				if (liveObject == target)
				{
					continue; // object is self-referential, early exit
				}
				// grab only targets in this field
				if (field.find(liveObject->Address()) == field.end())
				{
					continue;
				}
				// if we don't already have it listed, insert it
				if (referencedObjects.find(liveObject) == referencedObjects.end())
				{
					worklist.push(liveObject);
					referencedObjects.insert(liveObject);
				}
			}
		}
		// work through all objects
		disableTrivialExecution = true;
		for (std::map<void*, GCObject*>::iterator iter = field.begin(); iter != field.end(); ++iter)
		{
			GCObject* target = iter->second;
			ASSERT(target, "found null target in entire object list");
			// is it trivially referenced
			bool isReferenced = referencedObjects.find(target) != referencedObjects.end();
			// if unreferenced, try the parent?
			if (!isReferenced && parent)
			{
				for (std::set<GCReference*>::iterator iter = target->pointingReferences.begin(); iter != target->pointingReferences.end(); ++iter)
				{
					GCReference* ref = *iter;
					ASSERT(ref, "found null reference in pointing reference list");
					if (ref->IsWeak())
						continue; // uninterested in weak references
					// is the ref owner somewhere up in the heirarchy?
					if (parent->LookupRecursive(ref->Owner()->Address()))
					{
						isReferenced = true;
						break;
					}
				}
			}
			if (!isReferenced)
			{
				// unreferenced
				ASSERT(target != rootObject, "root object ended up unreferenced?");
				worklist.push(target);
			}
			else
			{
				targetField.insert(*iter);
			}
		}
		disableTrivialExecution = false;
		while (!worklist.empty())
		{
			worklist.front()->Condemn(NULL);
			worklist.pop();
		}
		field.clear();
	}
	std::map<void*, GCObject*> field;
	GCField* parent;
public:
	GCField ( GCField* aParent ) : parent(aParent) {}
	~GCField ()
	{
		for (std::map<void*, GCObject*>::iterator iter = field.begin(); iter != field.end(); iter++)
		{
			delete iter->second;
		}
		if (parent) delete parent;
	}
	void Collect ( int depth )
	{
		if (parent)
		{
			DoCollection(field, parent->field, parent);
			ASSERT(field.empty(), "secondary field not empty after collection");
		}
		else
		{
			std::map<void*, GCObject*> replacementField;
			DoCollection(field, replacementField, NULL);
			field.swap(replacementField);
		}
		depth--;
		if (depth > 0 && parent)
		{
			parent->Collect(depth);
			ASSERT(field.empty(), "field not empty after parent collection");
		}
	}
	void InsertShallow ( GCObject* object )
	{
		field[object->Address()] = object;
	}
	void InsertDeep ( GCObject* object )
	{
		if (parent)
			parent->InsertDeep(object);
		else
			InsertShallow(object);
	}
	GCObject* Lookup ( void* ptr )
	{
		return LookupRecursive(ptr);
	}
	void Remove ( GCObject* obj )
	{
		ASSERT(!disableTrivialExecution, "Remove() called with TE disabled");
		std::map<void*, GCObject*>::iterator iter = field.find(obj->Address());
		if (iter != field.end())
			field.erase(iter);
		else if (parent)
			parent->Remove(obj);
	}
	void Move ( void* oldAddress, void* newAddress )
	{
		std::map<void*, GCObject*>::iterator iter = field.find(oldAddress);
		if (iter == field.end())
		{
			if (parent)
			{
				parent->Move(oldAddress, newAddress);
			}
		}
		else
		{
			GCObject* object = iter->second;
			field.erase(iter);
			field.insert(std::make_pair(newAddress, object));
		}
	}
};

GCField* field;

void GCObject::Condemn ( GCReference* lastReference )
{
	if (condemned || (lastReference && disableTrivialExecution))
		return;
	condemned = true;
	if (lastReference)
	{
		std::set<GCReference*>::iterator iter = pointingReferences.find(lastReference);
		ASSERT(iter != pointingReferences.end(), "lastReference not pointing");
		pointingReferences.erase(iter);
	}
	else
	{
		std::vector<GCReference*> worklist;
		std::set<GCReference*>::iterator iter;
		for (iter = pointingReferences.begin(); iter != pointingReferences.end(); ++iter)
		{
			if (!(*iter)->IsWeak())
			{
				worklist.push_back(*iter);
			}
		}
		for (std::vector<GCReference*>::iterator wlIter = worklist.begin(); wlIter != worklist.end(); ++wlIter)
		{
			iter = pointingReferences.find(*wlIter);
			ASSERT(iter != pointingReferences.end(), "worklist item not in reference list");
			pointingReferences.erase(iter);
		} 
	}
	field->Remove(this);
	delete this;
}

#define FIELDCOUNT 3
#define FIELDPARTIALDEPTH 1

GCLock globalLock;

GCWeakReference::GCWeakReference ( GCObject* anOwner, GCObject* aTarget, void** aPointerLocation )
: GCReference(anOwner, aTarget, aPointerLocation)
{
	DEBUG(printf("[GC] +WR %p => %p (%p)\n", anOwner->Address(), aTarget->Address(), aPointerLocation));
}

GCWeakReference::~GCWeakReference ()
{
	DEBUG(printf("[GC] -WR %p => %p (%p)\n", owner->Address(), target->Address(), pointerLocation));
}

GCStrongReference::GCStrongReference ( GCObject* anOwner, GCObject* aTarget, void** aPointerLocation )
: GCReference(anOwner, aTarget, aPointerLocation)
{
	DEBUG(printf("[GC] +SR %p => %p (%p)\n", anOwner->Address(), aTarget->Address(), aPointerLocation));
}

GCStrongReference::~GCStrongReference ()
{
	DEBUG(printf("[GC] -SR %p => %p\n", owner->Address(), target->Address()));
}

void CollectPartial ()
{
	field->Collect(FIELDPARTIALDEPTH);
}

void CollectFull ()
{
	field->Collect(FIELDCOUNT);
}

GCObject* GetObject ( void* ptr )
{
	if (!ptr)
		return NULL;
	GCObject* object = field->Lookup(ptr);
	//ASSERT(object, "GetObject returned 0");
	return object;
}

void Unreference ( GCObject* src, GCObject* dst, bool isWeak )
{
	ASSERT(src, "Unreference with src=null");
	ASSERT(dst, "Unreference with dst=null");
	globalLock.WriteLock();
	for (std::set<GCReference*>::iterator iter = src->ownedReferences.begin(); iter != src->ownedReferences.end(); iter++)
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
	if (!target->IsCondemned())
	{
		iter = target->pointingReferences.find(this);
		ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
		target->pointingReferences.erase(iter);
		if (!target->IsReferenced() && !disableTrivialExecution)
		{
			DEBUG(printf("[GC] -OBJ %p (completely unreferenced)\n", target->Address()));
			target->Condemn(NULL);
		}
	}
	delete this;
}

void GCWeakReference::OwnerDisowned ()
{
	std::set<GCReference*>::iterator iter;
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	if (!target->IsCondemned())
	{
		iter = target->pointingReferences.find(this);
		ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
		target->pointingReferences.erase(iter);
		if (!target->IsReferenced() && !disableTrivialExecution)
		{
			DEBUG(printf("[GC] -OBJ %p (completely unreferenced)\n", target->Address()));
			target->Condemn(NULL);
		}
	}
	delete this;
}

void GCWeakReference::TargetDied ()
{
	std::set<GCReference*>::iterator iter;
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	weakInvalidator(owner->Address(), pointerLocation);
	//*pointerLocation = NULL;
	delete this;
}

void GCStrongReference::OwnerDied ()
{
	std::set<GCReference*>::iterator iter;
	if (!target->IsCondemned())
	{
		iter = target->pointingReferences.find(this);
		ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
		target->pointingReferences.erase(iter);
		if (!target->IsReferenced() && !disableTrivialExecution)
		{
			DEBUG(printf("[GC] -OBJ %p (completely unreferenced)\n", target->Address()));
			target->Condemn(NULL);
		}
	}
	delete this;
}

void GCStrongReference::OwnerDisowned ()
{
	std::set<GCReference*>::iterator iter;
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	if (!target->IsCondemned())
	{
		iter = target->pointingReferences.find(this);
		ASSERT(iter != target->pointingReferences.end(), "reference isn't in pointing list");
		target->pointingReferences.erase(iter);
		if (!target->IsReferenced() && !disableTrivialExecution)
		{
			DEBUG(printf("[GC] -OBJ %p (completely unreferenced)\n", target->Address()));
			target->Condemn(NULL);
		}
	}
	delete this;
}

void GCStrongReference::TargetDied ()
{
	// ...the hell?
	std::set<GCReference*>::iterator iter;
	if (!shuttingDown) // crazy shiz does happen whilst shutting down
	{
		ASSERT(0, "target died with strong reference attached");
	}
	iter = owner->ownedReferences.find(this);
	ASSERT(iter != owner->ownedReferences.end(), "reference isn't in owned list");
	owner->ownedReferences.erase(iter);
	delete this;
}

void GCObject::Migrate ( void* newTarget )
{
	// update field
	void* oldAddress = address;
	address = newTarget;
	// update all references
	for (std::set<GCReference*>::iterator iter = pointingReferences.begin(); iter != pointingReferences.end(); iter++)
	{
		GCReference* ref = *iter;
		if (ref->PointerLocation())
			*(ref->PointerLocation()) = newTarget;
	}
	// move in main map
	field->Move(oldAddress, newTarget);
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
	DEBUG(printf("[GC] \tsizeof(GCField) = %d\n", sizeof(GCField)));
	rootObject = new GCObject(GC_ROOT, 0, 0);
	globalLock.WriteLock();
	field = NULL;
	for (int i = 0; i < FIELDCOUNT; i++)
		field = new GCField(field);
	field->InsertDeep(rootObject);
	globalLock.WriteUnlock();
}

void GC_terminate ( bool callFinalisers )
{
	disableFinalisers = !callFinalisers;
	shuttingDown = true;
	delete field;
	shuttingDown = false;
	disableFinalisers = false;
}

void GC_collect ( bool partial )
{
	globalLock.WriteLock();
	DEBUG(printf("[GC] doing %s collection\n", partial ? "generational" : "full"));
	(partial ? CollectPartial : CollectFull)();
	DEBUG(printf("[GC] collection finished\n"));
	globalLock.WriteUnlock();
}

void* GC_new_object ( unsigned long len, void* owner, void (*finaliser)(void*) )
{
	if (len < sizeof(void*))
		len = sizeof(void*);
	void* pointer = calloc(1, len);
	GCObject* obj = new GCObject(pointer, finaliser, len);
	ASSERT(obj, "could not allocate new GCObject");
	globalLock.ReadLock();
	GCObject* owningObject = GetObject(owner);
	GCStrongReference* reference = new GCStrongReference(owningObject, obj, NULL);
	ASSERT(reference, "could not allocate new GCStrongReference");
	globalLock.ReadUnlock();
	globalLock.WriteLock();
	obj->pointingReferences.insert(reference);
	owningObject->ownedReferences.insert(reference);
	field->InsertShallow(obj);
	globalLock.WriteUnlock();
	return pointer;
}

void GC_register_object ( void* object, void* owner, void (*finaliser)(void*) )
{
	ASSERT(object, "tried to register bad object");
	GCObject* obj = new GCObject(object, finaliser, 0);
	ASSERT(obj, "could not allocate new GCObject");
	globalLock.ReadLock();
	GCObject* owningObject = GetObject(owner);
	GCStrongReference* reference = new GCStrongReference(owningObject, obj, NULL);
	ASSERT(reference, "could not allocate new GCStrongReference");
	globalLock.ReadUnlock();
	globalLock.WriteLock();
	obj->pointingReferences.insert(reference);
	owningObject->ownedReferences.insert(reference);
	field->InsertShallow(obj);
	globalLock.WriteUnlock();
}

void GC_register_reference ( void* object, void* target, void** pointerLocation )
{
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	ASSERT(src, "could not get source object");
	GCObject* dst = GetObject(target);
	ASSERT(dst, "could not get destination object");
	globalLock.ReadUnlock();
	GCStrongReference* reference = new GCStrongReference(src, dst, pointerLocation);
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

void GC_register_weak_reference ( void* object, void* target, void** pointer )
{
	ASSERT(pointer, "tried to create weak reference with null location");
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

bool GC_object_live ( void* object )
{
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	globalLock.ReadUnlock();
	return src != NULL;
}

void GC_object_migrate ( void* oldLocation, void* newLocation )
{
	globalLock.WriteLock();
	GCObject* src = GetObject(oldLocation);
	ASSERT(src, "could not get old object for GC migration");
	ASSERT(newLocation, "tried to move object to bad location");
	src->Migrate(newLocation);
	globalLock.WriteUnlock();
}

unsigned long GC_object_size ( void* object )
{
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	ASSERT(src, "could not get object to look up length");
	unsigned long len = src->GetLength();
	globalLock.ReadUnlock();
	return len;
}

void GC_object_resize ( void* object, unsigned long newLength )
{
	ASSERT(newLength, "tried to resize object to null length");
	globalLock.ReadLock();
	GCObject* src = GetObject(object);
	globalLock.ReadUnlock();
	ASSERT(src, "could not get object to resize");
	globalLock.WriteLock();
	src->Resize(newLength);
	globalLock.WriteUnlock();
}

void GC_weak_invalidator ( void (*invalidator)(void*, void**) )
{
	if (invalidator == NULL)
		invalidator = DefaultWeakInvalidator;
	weakInvalidator = invalidator;
}
