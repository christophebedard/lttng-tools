/*
 * SPDX-FileCopyrightText: 2022 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_URCU_H
#define LTTNG_URCU_H

#define _LGPL_SOURCE
#include <common/exception.hpp>
#include <common/hashtable/hashtable.hpp>
#include <common/macros.hpp>

#include <iterator>
#include <mutex>
#include <urcu.h>
#include <urcu/list.h>
#include <urcu/rculfhash.h>

namespace lttng {
namespace urcu {

namespace details {
/*
 * Wrapper around an urcu read lock which satisfies the 'Mutex' named
 * requirements of C++11. Satisfying those requirements facilitates the use of
 * standard concurrency support library facilities.
 *
 * read_lock is under the details namespace since it is unlikely to be used
 * directly by exception-safe code. See read_lock_guard.
 */
class read_lock {
public:
	read_lock() = default;
	~read_lock() = default;

	/* "Not copyable" and "not moveable" Mutex requirements. */
	read_lock(const read_lock&) = delete;
	read_lock(read_lock&&) = delete;
	read_lock& operator=(read_lock&&) = delete;
	read_lock& operator=(const read_lock&) = delete;

	void lock()
	{
		rcu_read_lock();
	}

	bool try_lock()
	{
		lock();
		return true;
	}

	void unlock()
	{
		rcu_read_unlock();
	}
};
} /* namespace details */

/*
 * Provides the basic concept of std::lock_guard for rcu reader locks.
 *
 * The RCU reader lock is held for the duration of lock_guard's lifetime.
 */
class read_lock_guard {
public:
	read_lock_guard() = default;
	~read_lock_guard() = default;

	read_lock_guard(const read_lock_guard&) = delete;
	read_lock_guard(read_lock_guard&&) = delete;
	read_lock_guard& operator=(read_lock_guard&&) = delete;
	read_lock_guard& operator=(const read_lock_guard&) = delete;

private:
	details::read_lock _lock;
	std::lock_guard<details::read_lock> _guard{ _lock };
};

using unique_read_lock = std::unique_lock<details::read_lock>;

namespace details {

/* Implementation for types that contain a straight cds_lfht_node. */
template <typename ContainedType, typename NodeType, NodeType ContainedType::*Member>
static typename std::enable_if<std::is_same<cds_lfht_node, NodeType>::value, ContainedType *>::type
get_element_from_node(cds_lfht_node& node)
{
	return lttng::utils::container_of(&node, Member);
}

/* Specialization for NodeType deriving from lttng_ht_node. */
template <typename ContainedType, typename NodeType, NodeType ContainedType::*Member>
static typename std::enable_if<std::is_base_of<lttng_ht_node, NodeType>::value, ContainedType *>::type
get_element_from_node(cds_lfht_node& node)
{
	return lttng_ht_node_container_of(&node, Member);
}
} /* namespace details */

/*
 * The lfht_iteration_adapter class template wraps the liburcu lfht API to provide iteration
 * capabilities. It allows users to iterate over a lock-free hash table with ranged-for semantics
 * while holding the RCU read lock. The reader lock is held for the lifetime of the iteration
 * adapter (i.e. not the lifetime of the iterators it provides).
 */
template <typename ContainedType, typename NodeType, NodeType ContainedType::*Member>
class lfht_iteration_adapter {
public:
	/* Nested iterator class defines the iterator for lfht_iteration_adapter. */
	class iterator : public std::iterator<std::input_iterator_tag, std::uint64_t> {
		/* Allow lfht_iteration_adapter to access private members of iterator. */
		friend lfht_iteration_adapter;

	public:
		iterator(const iterator& other) = default;
		iterator(iterator&& other) noexcept = default;
		~iterator() = default;
		iterator& operator=(const iterator&) = delete;
		iterator& operator=(iterator&&) noexcept = delete;

		/* Move to the next element in the hash table. */
		iterator& operator++()
		{
			cds_lfht_next(&_ht, &_it);
			return *this;
		}

		bool operator==(const iterator& other) const noexcept
		{
			/* Compare pointed nodes by address. */
			return other._it.node == _it.node;
		}

		bool operator!=(const iterator& other) const noexcept
		{
			return !(*this == other);
		}

		/* Dereference the iterator to access the contained element. */
		ContainedType *operator*() const
		{
			auto *node = _it.node;

			/* Throw an exception if dereferencing an end iterator. */
			if (!node) {
				LTTNG_THROW_OUT_OF_RANGE(
					"Dereferenced an iterator at the end of a liburcu hashtable");
			}

			/* Retrieve the element from the node. */
			return details::get_element_from_node<ContainedType, NodeType, Member>(
				*node);
		}

	protected:
		iterator(cds_lfht& ht, const cds_lfht_iter& it) : _ht(ht), _it(it)
		{
		}

		/* Reference to the hash table being iterated over. */
		cds_lfht& _ht;
		/* Native lfht iterator pointing to the current position. */
		cds_lfht_iter _it;
	};

	explicit lfht_iteration_adapter(cds_lfht& ht) : _ht(ht)
	{
	}

	/* Return an iterator to the beginning of the hash table. */
	iterator begin() const noexcept
	{
		cds_lfht_iter it;

		cds_lfht_first(&_ht, &it);
		return iterator(_ht, it);
	}

	/* Return an iterator to the end of the hash table. */
	iterator end() const noexcept
	{
		const cds_lfht_iter it = {};

		return iterator(_ht, it);
	}

protected:
	/* Reference to the hash table being iterated over. */
	cds_lfht& _ht;
	/* RCU read lock held during the iteration. */
	const lttng::urcu::read_lock_guard read_lock;
};

/*
 * The lfht_filtered_iteration_adapter class template wraps the liburcu lfht API to provide
 * iteration capabilities over a result set. It allows users to iterate over a lock-free hash
 * table's elements matching a given key with ranged-for semantics while holding the RCU read lock.
 * The reader lock is held for the lifetime of the iteration adapter (i.e. not the lifetime of the
 * iterators it provides).
 */
template <typename ContainedType, typename NodeType, NodeType ContainedType::*Member, typename KeyType>
class lfht_filtered_iteration_adapter
	: public lfht_iteration_adapter<ContainedType, NodeType, Member> {
public:
	/* Nested iterator class defines the iterator for lfht_filtered_iteration_adapter. */
	class iterator : public lfht_iteration_adapter<ContainedType, NodeType, Member>::iterator {
		/* Allow lfht_filtered_iteration_adapter to access private members of iterator. */
		friend lfht_filtered_iteration_adapter;

	public:
		iterator(const iterator& other) noexcept = default;
		iterator(iterator&& other) noexcept = default;
		~iterator() noexcept = default;
		iterator& operator=(const iterator&) = delete;
		iterator& operator=(iterator&&) noexcept = delete;

		/* Move to the next element in the result set. */
		iterator& operator++() noexcept
		{
			LTTNG_ASSERT(this->_it.node);
			/* NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast) */
			cds_lfht_next_duplicate(
				&this->_ht,
				_match_function,
				reinterpret_cast<void *>(const_cast<KeyType *>(_key)),
				&this->_it);
			/* NOLINTEND(cppcoreguidelines-pro-type-const-cast) */
			return *this;
		}

	private:
		iterator(cds_lfht& ht,
			 const cds_lfht_iter& it,
			 const KeyType *key,
			 cds_lfht_match_fct match_function) noexcept :
			lfht_iteration_adapter<ContainedType, NodeType, Member>::iterator(ht, it),
			_key(key),
			_match_function(match_function)
		{
		}

		/* Only used to create an end iterator. */
		iterator(cds_lfht& ht, const cds_lfht_iter& it) noexcept :
			lfht_iteration_adapter<ContainedType, NodeType, Member>::iterator(ht, it),
			_key(nullptr),
			_match_function(nullptr)
		{
		}

		const KeyType *_key;
		const cds_lfht_match_fct _match_function;
	};

	explicit lfht_filtered_iteration_adapter(cds_lfht& ht,
						 const KeyType *key,
						 unsigned long key_hash,
						 cds_lfht_match_fct match_function) noexcept :
		lfht_iteration_adapter<ContainedType, NodeType, Member>(ht),
		_key(key),
		_key_hash(key_hash),
		_match_function(match_function)
	{
	}

	/* Return an iterator to the first result. */
	iterator begin() const noexcept
	{
		cds_lfht_iter it;

		/* NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast) */
		cds_lfht_lookup(&this->_ht,
				_key_hash,
				_match_function,
				reinterpret_cast<void *>(const_cast<KeyType *>(_key)),
				&it);
		/* NOLINTEND(cppcoreguidelines-pro-type-const-cast) */
		return iterator(this->_ht, it, _key, _match_function);
	}

	iterator end() const noexcept
	{
		const cds_lfht_iter it = {};

		return iterator(this->_ht, it);
	}

private:
	const KeyType *_key;
	const unsigned long _key_hash;
	const cds_lfht_match_fct _match_function;
};

template <typename ContainedType, cds_list_head ContainedType::*Member>
class list_iteration_adapter {
public:
	/* Nested iterator class defines the iterator for list_iteration_adapter. */
	class iterator : public std::iterator<std::input_iterator_tag, std::uint64_t> {
		/* Allow list_iteration_adapter to access private members of iterator. */
		friend list_iteration_adapter;

	public:
		iterator(const iterator& other) = default;
		iterator(iterator&& other) noexcept = default;
		~iterator() = default;
		iterator& operator=(const iterator&) = delete;
		iterator& operator=(iterator&&) noexcept = delete;

		/* Move to the next element in the hash table. */
		iterator& operator++()
		{
			_node = _node_contents.next;
			_node_contents = *_node;
			return *this;
		}

		bool operator==(const iterator& other) const noexcept
		{
			return other._node == _node;
		}

		bool operator!=(const iterator& other) const noexcept
		{
			return !(*this == other);
		}

		/* Dereference the iterator to access the contained element. */
		ContainedType *operator*() const
		{
			/* Retrieve the element from the node. */
			return lttng::utils::container_of(_node, Member);
		}

	protected:
		explicit iterator(const cds_list_head& node) : _node(&node), _node_contents(node)
		{
		}

		/* Current node. */
		const cds_list_head *_node;
		/* Copy of node contents to allow safe deletion during the iteration. */
		cds_list_head _node_contents;
	};

	explicit list_iteration_adapter(const cds_list_head& list) : _list(list)
	{
	}

	/* Return an iterator to the beginning of the hash table. */
	iterator begin() const noexcept
	{
		return iterator(*_list.next);
	}

	/* Return an iterator to the end of the hash table. */
	iterator end() const noexcept
	{
		return iterator(_list);
	}

protected:
	/* Reference to the list being iterated over. */
	const cds_list_head& _list;
};

template <typename ContainedType, cds_list_head ContainedType::*Member>
class rcu_list_iteration_adapter : public list_iteration_adapter<ContainedType, Member> {
public:
	/* Nested iterator class defines the iterator for rcu_list_iteration_adapter. */
	class iterator : public list_iteration_adapter<ContainedType, Member>::iterator {
		/* Allow rcu_list_iteration_adapter to access private members of iterator. */
		friend rcu_list_iteration_adapter;

	public:
		iterator(const iterator& other) = default;
		iterator(iterator&& other) noexcept = default;
		~iterator() = default;
		iterator& operator=(const iterator&) = delete;
		iterator& operator=(iterator&&) noexcept = delete;

		/* Move to the next element in the hash table. */
		iterator& operator++()
		{
			this->_node = rcu_dereference(this->_node_contents.next);
			this->_node_contents = *this->_node;
			return *this;
		}

	protected:
		explicit iterator(const cds_list_head& node) :
			list_iteration_adapter<ContainedType, Member>::iterator(node)
		{
		}
	};

	explicit rcu_list_iteration_adapter(const cds_list_head& list) :
		list_iteration_adapter<ContainedType, Member>(list)
	{
	}

	/* Return an iterator to the beginning of the hash table. */
	iterator begin() const noexcept
	{
		return iterator(*rcu_dereference(this->_list.next));
	}

	/* Return an iterator to the end of the hash table. */
	iterator end() const noexcept
	{
		return iterator(this->_list);
	}

protected:
	/* RCU read lock held during the iteration. */
	const lttng::urcu::read_lock_guard read_lock;
};

class scoped_thread_registration {
public:
	scoped_thread_registration()
	{
		/* Register the current thread with liburcu. */
		rcu_register_thread();
	}

	/* Not copyable or movable. */
	scoped_thread_registration(const scoped_thread_registration&) = delete;
	scoped_thread_registration& operator=(const scoped_thread_registration&) = delete;
	scoped_thread_registration(scoped_thread_registration&&) = delete;
	scoped_thread_registration& operator=(scoped_thread_registration&&) = delete;

	/* Unregister the current thread from liburcu. */
	void unregister_thread()
	{
		rcu_unregister_thread();
	}

	~scoped_thread_registration()
	{
		unregister_thread();
	}
};

/*
 * scoped_rcu_read_lock is a RAII-style wrapper around the RCU read lock.
 * It ensures that the read lock is acquired upon construction and released
 * upon destruction, providing exception safety.
 *
 * In general, prefer using read_lock_guard instead of scoped_rcu_read_lock
 * as it provides a more standard interface and is more idiomatic in C++.
 * However, scoped_rcu_read_lock is provided to make the scoped lock
 * moveable in the few cases where that is required.
 */
class scoped_rcu_read_lock {
public:
	scoped_rcu_read_lock()
	{
		rcu_read_lock();
	}

	~scoped_rcu_read_lock()
	{
		if (_armed) {
			rcu_read_unlock();
		}
	}

	scoped_rcu_read_lock(scoped_rcu_read_lock&& other) noexcept
	{
		other._armed = false;
	}

	scoped_rcu_read_lock(scoped_rcu_read_lock& other) = delete;
	scoped_rcu_read_lock& operator=(const scoped_rcu_read_lock&) = delete;
	scoped_rcu_read_lock& operator=(scoped_rcu_read_lock&&) noexcept = delete;

private:
	bool _armed = true;
};

/*
 * is_list_empty checks if a cds_list_head is empty.
 * It is equivalent to cds_list_empty, but that function is not const-correct
 * before urcu 0.15.
 */
static inline bool is_list_empty(const cds_list_head *head)
{
	return head == head->next;
}

} /* namespace urcu */
} /* namespace lttng */

#endif /* LTTNG_URCU_H */
