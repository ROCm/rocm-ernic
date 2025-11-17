/*
 * QEMU Object Model (QOM) stub
 * Not used in standalone - just for compilation
 */

#ifndef QOM_OBJECT_H
#define QOM_OBJECT_H

/* Object type - minimal stub */
typedef struct Object Object;

struct Object {
    int dummy;
};

/* Object iteration callback */
typedef int (*ObjectIterator)(Object *obj, void *opaque);

/* Stub function - not actually called in standalone */
static inline int object_child_foreach(Object *obj, ObjectIterator it,
                                       void *opaque)
{
    (void)obj;
    (void)it;
    (void)opaque;
    return 0;
}

#endif /* QOM_OBJECT_H */
