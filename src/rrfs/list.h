/*
 * Copyright © 2011 Coraid, Inc.
 * All rights reserved.
 */

typedef struct Node Node;
typedef struct List List;

struct Node {
	Node	*next;
	Node	*prev;
};

struct List {
	Node	*head;
	Node	*tail;
	ulong	offset;
};

#define	listempty(l)	((l)->head == nil)

#define	listdata(l, n)	((void *)((uchar *)(n)-(l)->offset))
#define	listnode(l, p)	((void *)((uchar *)(p)+(l)->offset))

#define	listhead(l)	listdata(l, (l)->head)
#define	listtail(l)	listdata(l, (l)->head)

#define	listinit(l, s, m)					\
	do {							\
		memset((l), 0, sizeof *(l));			\
		(l)->offset = offsetof(s, m);			\
	} while(0)

#define	listadd(l, p)						\
	do {							\
		Node *_n;					\
		_n = listnode(l, p);				\
		memset(_n, 0, sizeof *_n);			\
		if (listempty(l))				\
			(l)->head = _n;				\
		else {						\
			_n->prev = (l)->tail;			\
			(l)->tail->next = _n;			\
		}						\
		(l)->tail = _n;					\
	} while(0)

#define	listinsert(l, p, q)					\
	do {							\
		Node *_n, *_m;					\
		_n = listnode(l, p);				\
		if (q == nil)					\
			listadd(l, p);				\
		else {						\
			_m = listnode(l, q);			\
			if (_m->prev == nil)			\
				(l)->head = _n;			\
			else					\
				_m->prev->next = _n;		\
			_n->prev = _m->prev;			\
			_m->prev = _n;				\
			_n->next = _m;				\
		}						\
	} while(0)

#define	listremove(l, p)					\
	do {							\
		Node *_n;					\
		_n = listnode(l, p);				\
		if (_n->prev == nil && _n->next == nil)		\
			(l)->head = (l)->tail = nil;		\
		else if (_n->prev == nil) {			\
			_n->next->prev = nil;			\
			(l)->head = _n->next;			\
		} else if (_n->next == nil) {			\
			_n->prev->next = nil;			\
			(l)->tail = _n->prev;			\
		} else {					\
			_n->prev->next = _n->next;		\
			_n->next->prev = _n->prev;		\
		}						\
	} while(0)

#define	listforeach(l, p)					\
	do {							\
		Node *_n, *_m;					\
		_m = (l)->head;					\
		while ((_n = _m) != nil) {			\
			_m = _n->next;				\
			(p) = listdata(l, _n);			\

#define	listdone						\
		}						\
	} while(0)
