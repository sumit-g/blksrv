// Allocation free doubly linked list
#ifndef _LIST_H_
#define _LIST_H_

#include <stdint.h>
#include <stddef.h>

#include <assert.h>

class ListLink {
 public:
  ListLink() { Reset(); }
  void Reset() { next = prev = this; }
  ListLink *next;
  ListLink *prev;
};

template <class T>
class List {
 public:
  List() { size_ = 0; off_ = -1; }
  List(int offset) { size_ = 0; off_ = offset; }
  ~List() { assert(off_ >= 0); }
  void set_offset(uint32_t off) { off_ = off; }

  void PushBack(T *obj) {
    ListLink *link = (ListLink *)(((uint8_t *)obj) + off_);
    assert(link->next == link || link->next == nullptr);
    assert(link->prev == link || link->prev == nullptr);
    link->prev = head_.prev;
    link->next = &head_;
    head_.prev->next = link;
    head_.prev = link;
    size_++;
  }
  void PushBack(T &obj) { PushBack(&obj); }

  void PushFront(T *obj) {
    ListLink *link = (ListLink *)(((uint8_t *)obj) + off_);
    assert(link->next == link || link->next == nullptr);
    assert(link->prev == link || link->prev == nullptr);
    link->next = head_.next;
    link->prev = &head_;
    head_.next->prev = link;
    head_.next = link;
    size_++;
  }
  void PushFront(T &obj) { PushFront(&obj); }

  T *PopFront() {
    if (size_ == 0)
      return nullptr;
    ListLink *link = head_.next;
    link->next->prev = &head_;
    head_.next = link->next;
    size_--;
    link->next = link->prev = link;
    return (T *)(((uint8_t *)link) - off_);
  }

  T *PopBack() {
    if (size_ == 0)
      return nullptr;
    ListLink *link = head_.prev;
    link->prev->next = &head_;
    head_.prev = link->prev;
    size_--;
    link->next = link->prev = link;
    return (T *)(((uint8_t *)link) - off_);
  }

  // Returns true if the link was in the list and was removed.
  bool Remove(T *obj) {
    ListLink *link = (ListLink *)(((uint8_t *)obj) + off_);
    if (link->next == link || link->next == nullptr) return false;
    if (link->prev == link || link->prev == nullptr) return false;
    // At this point we are going to assume that the element is in
    // the list.
    link->next->prev = link->prev;
    link->prev->next = link->next;
    link->next = link->prev = link;
    size_--;
    return true;
  }
  bool Remove(T &obj) { return Remove(&obj); }

  inline ListLink *Head() { return &head_; }

  inline uint32_t size() { return size_; }

  T *At(ListLink *link) {
    if (link == &head_ || link == nullptr) return nullptr;
    return (T *)(((uint8_t *)link) - off_);
  }

  T *First() { return At(head_.next); }
  T *Next(T *cur) {
    if (!cur)
      return nullptr;
    return At(((ListLink *)(((uint8_t *)cur)+off_))->next);
  }

  uint32_t off() { return off_; }

 private:
  ListLink head_;
  uint32_t size_;
  int off_;
};

#endif  // _LIST_H_
