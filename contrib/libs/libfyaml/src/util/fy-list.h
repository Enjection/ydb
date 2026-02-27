/*
 * fy-list.h - Circular doubly-linked list implementation
 *
 * Copyright (c) 2025 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FY_LIST_H
#define FY_LIST_H

#include "fy-utils.h"

struct fy_list_head {
	struct fy_list_head *next;
	struct fy_list_head *prev;
};

static inline void list_init(struct fy_list_head *list)
{
	list->next = list->prev = list;
}

static inline void list_add(struct fy_list_head *new_item, struct fy_list_head *head)
{
	struct fy_list_head *prev = head;
	struct fy_list_head *next = head->next;

	next->prev = new_item;
	new_item->next = next;
	new_item->prev = prev;
	prev->next = new_item;
}

static inline void list_add_tail(struct fy_list_head *new_item, struct fy_list_head *head)
{
	struct fy_list_head *prev = head->prev;
	struct fy_list_head *next = head;

	next->prev = new_item;
	new_item->next = next;
	new_item->prev = prev;
	prev->next = new_item;
}

static inline void list_del(struct fy_list_head *entry)
{
	struct fy_list_head *prev = entry->prev;
	struct fy_list_head *next = entry->next;

	next->prev = prev;
	prev->next = next;
}

static inline int list_empty(const struct fy_list_head *head)
{
	return head->next == head;
}

static inline int list_is_singular(const struct fy_list_head *head)
{
	return !list_empty(head) && head->next == head->prev;
}

static inline void list_splice(const struct fy_list_head *list,
			       struct fy_list_head *head)
{
	struct fy_list_head *first = list->next;
	struct fy_list_head *last = list->prev;
	struct fy_list_head *prev = head;
	struct fy_list_head *next = head->next;

	if (list_empty(list))
		return;

	first->prev = prev;
	prev->next = first;

	last->next = next;
	next->prev = last;
}

#define list_entry(ptr, type, member) \
	fy_container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)

#endif
