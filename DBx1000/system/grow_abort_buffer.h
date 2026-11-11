#pragma once
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <mm_malloc.h>
#include <stdexcept>
// Retain every pending query and its retry/latency timestamps when growing.
template<class Entry>
void grow_abort_buffer(Entry *&entries, size_t &capacity, size_t &empty) {
    const size_t max_entries = std::numeric_limits<size_t>::max() / sizeof(Entry);
    if (capacity > max_entries / 2) throw std::bad_alloc();
    size_t next = capacity ? capacity * 2 : 1;
    Entry *fresh = static_cast<Entry *>(_mm_malloc(sizeof(Entry) * next, 64));
    if (!fresh) throw std::bad_alloc();
    if (capacity) std::memcpy(fresh, entries, sizeof(Entry) * capacity);
    for (size_t i = capacity; i < next; ++i) fresh[i].query = NULL;
    _mm_free(entries);
    entries = fresh;
    empty += next - capacity;
    capacity = next;
}
