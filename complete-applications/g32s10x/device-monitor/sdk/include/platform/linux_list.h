#ifndef __LINUX_LIST_H
#define __LINUX_LIST_H

#if 0
#if defined(WIN32)
#define INLINE __inline
#else
#define INLINE inline
#endif
#endif
/* This file is from Linux Kernel (include/linux/list.h) 
 * and modified by simply removing hardware prefetching of list items. 
 * Here by copyright, credits attributed to where ever they belong.
 * Get from http://isis.poly.edu/kulesh/stuff/src/klist/
 */

/*
 * Simple doubly linked list implementation.
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */

struct tglist_head {
	struct tglist_head *next, *prev;
};

#define TGLIST_HEAD_INIT(name) { &(name), &(name) }

#define TGLIST_HEAD(name) \
	struct tglist_head name = TGLIST_HEAD_INIT(name)

#define INIT_TGLIST_HEAD(ptr) do { \
	(ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

/*
 * Insert a newp entry between two known consecutive entries. 
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static INLINE void __tglist_add(struct tglist_head *newp,
			      struct tglist_head *prev,
			      struct tglist_head *next)
{
	newp->next = next;
	newp->prev = prev;
	next->prev = newp;
	prev->next = newp;
}

/**
 * tglist_add - add a newp entry
 * @newp: newp entry to be added
 * @head: list head to add it after
 *
 * Insert a newp entry after the specified head.
 * This is good for implementing stacks.
 */
static INLINE void tglist_add(struct tglist_head *newp, struct tglist_head *head)
{
	__tglist_add(newp, head, head->next);
}

/**
 * tglist_add_tail - add a newp entry
 * @newp: newp entry to be added
 * @head: list head to add it before
 *
 * Insert a newp entry before the specified head.
 * This is useful for implementing queues.
 */
static INLINE void tglist_add_tail(struct tglist_head *newp, struct tglist_head *head)
{
	__tglist_add(newp, head->prev, head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static INLINE void __tglist_del(struct tglist_head *prev, struct tglist_head *next)
{
	next->prev = prev;
	prev->next = next;
}

/**
 * tglist_del - deletes entry from list.
 * @entry: the element to delete from the list.
 * Note: tglist_empty on entry does not return true after this, the entry is in an undefined state.
 */
static INLINE void tglist_del(struct tglist_head *entry)
{
	__tglist_del(entry->prev, entry->next);
	entry->next = (struct tglist_head*) 0;
	entry->prev = (struct tglist_head *) 0;
}

/**
 * tglist_del_init - deletes entry from list and reinitialize it.
 * @entry: the element to delete from the list.
 */
static INLINE void tglist_del_init(struct tglist_head *entry)
{
	__tglist_del(entry->prev, entry->next);
	INIT_TGLIST_HEAD(entry); 
}

/**
 * tglist_move - delete from one list and add as another's head
 * @list: the entry to move
 * @head: the head that will precede our entry
 */
static INLINE void tglist_move(struct tglist_head *list, struct tglist_head *head)
{
        __tglist_del(list->prev, list->next);
        tglist_add(list, head);
}

/**
 * tglist_move_tail - delete from one list and add as another's tail
 * @list: the entry to move
 * @head: the head that will follow our entry
 */
static INLINE void tglist_move_tail(struct tglist_head *list,
				  struct tglist_head *head)
{
        __tglist_del(list->prev, list->next);
        tglist_add_tail(list, head);
}

/**
 * tglist_empty - tests whether a list is empty
 * @head: the list to test.
 */
static INLINE int tglist_empty(struct tglist_head *head)
{
	return head->next == head;
}

static INLINE void __tglist_splice(struct tglist_head *list,
				 struct tglist_head *head)
{
	struct tglist_head *first = list->next;
	struct tglist_head *last = list->prev;
	struct tglist_head *at = head->next;

	first->prev = head;
	head->next = first;

	last->next = at;
	at->prev = last;
}

/**
 * tglist_splice - join two lists
 * @list: the newp list to add.
 * @head: the place to add it in the first list.
 */
static INLINE void tglist_splice(struct tglist_head *list, struct tglist_head *head)
{
	if (!tglist_empty(list))
		__tglist_splice(list, head);
}

/**
 * tglist_splice_init - join two lists and reinitialise the emptied list.
 * @list: the newp list to add.
 * @head: the place to add it in the first list.
 *
 * The list at @list is reinitialised
 */
static INLINE void tglist_splice_init(struct tglist_head *list,
				    struct tglist_head *head)
{
	if (!tglist_empty(list)) {
		__tglist_splice(list, head);
		INIT_TGLIST_HEAD(list);
	}
}

/**
 * tglist_entry - get the struct for this entry
 * @ptr:	the &struct tglist_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the tglist_struct within the struct.
 */
#define tglist_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

/**
 * tglist_for_each	-	iterate over a list
 * @pos:	the &struct tglist_head to use as a loop counter.
 * @head:	the head for your list.
 */
#define tglist_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); \
        	pos = pos->next)
/**
 * tglist_for_each_prev	-	iterate over a list backwards
 * @pos:	the &struct tglist_head to use as a loop counter.
 * @head:	the head for your list.
 */
#define tglist_for_each_prev(pos, head) \
	for (pos = (head)->prev; pos != (head); \
        	pos = pos->prev)
        	
/**
 * tglist_for_each_safe	-	iterate over a list safe against removal of list entry
 * @pos:	the &struct tglist_head to use as a loop counter.
 * @n:		another &struct tglist_head to use as temporary storage
 * @head:	the head for your list.
 */
#define tglist_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next)

/**
 * tglist_for_each_entry	-	iterate over list of given type
 * @pos:	the type * to use as a loop counter.
 * @head:	the head for your list.
 * @member:	the name of the tglist_struct within the struct.
 */
#define tglist_for_each_entry(pos, head, member)				\
	for (pos = tglist_entry((head)->next, typeof(*pos), member);	\
	     &pos->member != (head); 					\
	     pos = tglist_entry(pos->member.next, typeof(*pos), member))

/**
 * tglist_for_each_entry_safe - iterate over list of given type safe against removal of list entry
 * @pos:	the type * to use as a loop counter.
 * @n:		another type * to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the tglist_struct within the struct.
 */
#define tglist_for_each_entry_safe(pos, n, head, member)			\
	for (pos = tglist_entry((head)->next, typeof(*pos), member),	\
		n = tglist_entry(pos->member.next, typeof(*pos), member);	\
	     &pos->member != (head); 					\
	     pos = n, n = tglist_entry(n->member.next, typeof(*n), member))


#endif
