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
 * Register an object with the GC subsystem, assumed live.
 *
 * This object will have a reference from the root object until specifically directed otherwise.
 *
 * @param object The object to register.
 * @param finaliser The function to call when finished, or NULL.
 */
void GC_register_object ( void* object, void (*finaliser)(void*) );
/**
 * Release the automatic reference kept on an object.
 *
 * This should be done on an object after it has been registered with the stack frame.
 */
#define GC_autorelease(object) GC_unregister_reference(GC_root(), object)
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
 * Gets the address of the GC root object
 */
void* GC_root ();
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

#ifdef __cplusplus
}
#endif
