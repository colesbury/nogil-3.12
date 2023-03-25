#ifndef Py_PYLINKEDLIST_H
#define Py_PYLINKEDLIST_H
/* Header excluded from the stable API */
#ifndef Py_LIMITED_API

// See pycore_llist.h for the implementation of the linked list API.

struct llist_node {
    struct llist_node *next;
    struct llist_node *prev;
};

#endif /* !defined(Py_LIMITED_API) */
#endif /* !Py_PYLINKEDLIST_H */
