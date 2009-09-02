#ifdef __cplusplus
extern "C"
{
#else
#include <stdbool.h>
#endif

/**
 * Initialise the GC subsystem
 */
void GC_init ();
/**
 * SHUT DOWN EVERYTHING
 */
void GC_terminate ( bool callFinalisers );
/**
 * Perform a GC collection
 *
 * @param partial Whether to make this is a small partial collection or a full collection.
 */
void GC_collect ( bool partial );
/**
 * Create a new object using the GC subsystem, assumed live.
 *
 * This object will have a reference from the root object until specifically directed otherwise.
 *
 * @param len The length of the object.
 * @param finaliser The function to call when finished, or NULL.
 */
void* GC_new_object ( unsigned long len, void* owner, void (*finaliser)(void*) );
/**
 * Register an object with the GC subsystem, assumed live.
 *
 * This object will have a reference from the root object until specifically directed otherwise.
 *
 * @param object The object to register.
 * @param finaliser The function to call when finished, or NULL.
 */
void GC_register_object ( void* object, void* owner, void (*finaliser)(void*) );
/**
 * Register a reference to an object.
 *
 * @param object The object containing the reference.
 * @param target The target of the reference.
 */
void GC_register_reference ( void* object, void* target, void** pointer );
/**
 * Unregister a reference to an object.
 *
 * @param object The object containing the reference.
 * @param target The target of the reference.
 */
void GC_unregister_reference ( void* object, void* target );
/**
 * The address of the GC root object
 */
#define GC_ROOT ((void*)7)
/**
 * Register a weak reference to an object
 *
 * @param object The object containing the reference
 * @param target The target of the reference
 * @param pointer The address of the actual reference
 */
void GC_register_weak_reference ( void* object, void* target, void** pointer );
/**
 * Unregister a weak reference to an object
 *
 * @param object The object containing the reference
 * @param target The target of the reference
 * @param pointer The address of the actual reference
 */
void GC_unregister_weak_reference ( void* object, void* target );
/**
 * Checks if a given object is live.
 */
bool GC_object_live ( void* object );
/**
 * Migrates an object from one memory location to another
 */
void GC_object_migrate ( void* oldLocation, void* newLocation );
/**
 * Returns the size of a GC-allocated object.
 */
unsigned long GC_object_size ( void* object );
/**
 * Resizes a GC-allocated object.
 */
void GC_object_resize ( void* object, unsigned long newLength );
/**
 * Sets the weak reference invalidator.
 *
 * The invalidator takes two params:
 *  param 1 is the object which owns the reference
 *  param 2 is the pointer to the actual reference
 * pass NULL to reset to the default, which writes NULL to the pointer.
 */
void GC_weak_invalidator ( void (*invalidator)(void*, void**) );

#ifdef __cplusplus
}
#endif
