#ifndef _CACHE_ALLOCATOR_H_
#define _CACHE_ALLOCATOR_H_

#include "list.h"
#include <mutex>
#include <time.h>
#include <functional>

using namespace std;

// A generic and simple cache allocator class which uses lazy free to maintain
// a cache of objects.
// The alloc_func (passed to constructor) is suppose to do any one time init.
// The Alloc() call wont perform this init again. The code calling Alloc() should
// be doing any per-call initialization.
// Also The allocated object is suppose to have a ListLink member, whose
// offset is also passed to the constructor.
template <class T>
class CacheAllocator {
 public:
  CacheAllocator(function<T*(void*)> alloc_func, void *alloc_arg,
                 function<void(void*, T*)> free_func, void *free_arg,
                 unsigned link_offset);
  ~CacheAllocator();
  // The unique_lock pointer is supposed to be held at the time
  // of call. This is used to protect the internal lists. If cache is empty,
  // The lock will be dropped before calling the alloc_func. The idea is that most of
  // the time we can alloc from cache and hence we avoid taking/dropping another lock.
  T *Alloc(unique_lock<mutex> *l);

  // Free never drops the lock but we still pass it to be consistent.
  void Free(unique_lock<mutex> *l, T *e);

  // Polling function, called at regular intervals. Typically expected to be called
  // at 1 second boundary.
  void HouseKeeping(unique_lock<mutex> *l, time_t cur_time=time(nullptr));

 private:
  time_t last_time_ = 0;
  List<T> free_objs_;
  function<T*(void*)> alloc_func_;
  void *alloc_arg_;
  function<void(void*, T*)> free_func_;
  void *free_arg_;
  unsigned cur_allocations_ = 0;
  unsigned max_allocations_ = 0;
};

template <class T>
CacheAllocator<T>::CacheAllocator(
    function<T*(void*)> alloc_func, void *alloc_arg,
    function<void(void*, T*)> free_func, void *free_arg,
    unsigned link_offset) {
 alloc_func_ = alloc_func;
 alloc_arg_ = alloc_arg;
 free_func_ = free_func;
 free_arg_ = free_arg;
 free_objs_.set_offset(link_offset);
}

template <class T>
CacheAllocator<T>::~CacheAllocator() {
  T* ptr;
  while ((ptr = free_objs_.PopFront()) != nullptr)
    free_func_(free_arg_, ptr);
}

template <class T>
T* CacheAllocator<T>::Alloc(unique_lock<mutex> *l) {
  T *ret = free_objs_.PopFront();
  if (ret != nullptr) {
    cur_allocations_++;
    if (cur_allocations_ > max_allocations_)
      max_allocations_ = cur_allocations_;
    return ret;
  }
  l->unlock();
  ret = alloc_func_(alloc_arg_);
  l->lock();
  if (ret != nullptr) {
    cur_allocations_++;
    if (cur_allocations_ > max_allocations_)
      max_allocations_ = cur_allocations_;
  }
  return ret;
}

template <class T>
void CacheAllocator<T>::Free(unique_lock<mutex> *l, T *e) {
  free_objs_.PushFront(e);
  cur_allocations_--;
}

template <class T>
void CacheAllocator<T>::HouseKeeping(unique_lock<mutex> *l, time_t cur_time) {
  if (cur_time == last_time_)
    return;
  last_time_ = cur_time;
  unsigned backend_allocations = free_objs_.size() + cur_allocations_;
  unsigned excess = backend_allocations - max_allocations_;
  // reset max_allocations_
  max_allocations_ = cur_allocations_;
  if (excess <= 2) {
    return;
  }

  // Try to free half of excess.
  excess >>= 1;
  List<T> freed(free_objs_.off());
  for (unsigned i = 0; i < excess; i++) {
    T* ptr = free_objs_.PopFront();
    if (!ptr)
      break;
    freed.PushFront(ptr);
  }
  l->unlock();
  T* ptr;
  while ((ptr = freed.PopFront()) != nullptr)
    free_func_(free_arg_, ptr);
  l->lock();
}

#endif  // _CACHE_ALLOCATOR_H_
