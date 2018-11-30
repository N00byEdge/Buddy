#include <array>
#include <cstdint>
#include <ostream>
#include <bitset>

#include <sys/mman.h>
#include <unistd.h>

namespace Buddy {
  /* Linked freeList data structure begin */
  struct FreeList;
  struct Iterator {
    FreeList const *it;
    constexpr Iterator &operator++();
    constexpr FreeList const *operator*() const { return it; }
    constexpr bool operator!=(Iterator const &other) const { return it != other.it; }
    constexpr bool operator==(Iterator const &other) const { return it == other.it; }
  };

  std::ostream &operator<<(std::ostream &os, Iterator const &it) {
    os << it.it << ' ';
    return os;
  }

  struct FreeList {
    FreeList *next;
    void emplace(void *v) {
      auto newHead = reinterpret_cast<FreeList *>(v);
      newHead->next = next;
      next = newHead;
    }
    constexpr Iterator begin() const { return {next}; }
    constexpr Iterator end() const { return {nullptr}; }
    constexpr bool empty() const { return begin() == end(); }
    void *extract() { return std::exchange(this->next, this->next->next); }
  };

  std::ostream &operator<<(std::ostream &os, FreeList const &fl) {
    for(auto it = fl.begin(); it != fl.end(); ++ it) os << it;
    return os;
  }

  constexpr Iterator &Iterator::operator++() { it = it->next; return *this; }
  /* Linked freeList data structure end */

  constexpr std::uintptr_t maxLevels = 8;
  constexpr std::uintptr_t minSize = 32;

  template<typename T>
  constexpr int popcount(T v) {
    int sum = 0;
    for(; v; v >>= 1) sum += v & 1;
    return sum;
  }

  static_assert(popcount(minSize) == 1, "minSize is power of 2");

  inline std::array<FreeList, maxLevels> fList;

  inline std::array<std::uintptr_t, maxLevels> constexpr levelSize {[](){
    std::array<std::uintptr_t, maxLevels> levelSize{};
    for(int i = 0, sz = minSize; i < maxLevels; ++ i, sz <<= 1)
      levelSize[i] = sz;
    return levelSize;
  }()};

  constexpr std::uintptr_t maxSize = levelSize[maxLevels - 1];

  inline std::array constexpr reverseLookup{[](){
    auto constexpr arrsz = (minSize << (maxLevels - 1))/2 + 1;
    std::array<int, arrsz> rev{};
    for(int currSz = minSize, i = 0, l = 0; i < arrsz; ++ i) {
      if(currSz < i) currSz <<= 1, ++l;
      rev[i] = l;
    }
    return rev;
  }()};

  // Small struct to wrap the memory and size returned. (can be different from requested)
  // Memory is deallocated on destruction. (like unique_ptr)
  struct Allocation {
    void free();
    ~Allocation() { free(); }
    Allocation &operator=(Allocation &&other) {
      free();
      memory = std::exchange(other.memory, nullptr);
      size = std::exchange(other.size, 0);
      return *this;
    }
    void *memory = nullptr;
    std::uintptr_t size = 0;
    operator bool() const { return memory; }
  };

  struct Allocator {
    static Allocation allocate(std::uintptr_t sz) {
      auto level = sz > reverseLookup.size() ? maxLevels - 1 : reverseLookup[sz];

      char *mem = nullptr;
      int currLevel;

      // Find block in freelist
      for(int l = level; l < maxLevels; ++ l) if(!fList[l].empty()) {
        mem = reinterpret_cast<char *>(fList[l].extract());
        currLevel = l;
        break;
      }

      // No block found, create new block
      if(!mem)
        mem = reinterpret_cast<char *>(sbrk(maxSize)), currLevel = maxLevels - 1;

      if(mem == reinterpret_cast<char *>(-1))
        throw std::bad_alloc{}; // sbrk() hates us and we've run out of heap!

      // Split block to desired size
      while(currLevel --> level) fList[currLevel].emplace(mem + levelSize[currLevel]);
      return {mem, levelSize[level]};
    }

    static void deallocate(Allocation &alloc) {
      if(!alloc.memory) return;
      // Calculate level of freed memory
      auto lv = alloc.size > reverseLookup.size() ? maxLevels - 1 : reverseLookup[alloc.size];

      // Bubble up and merge buddies
      while(alloc.size != maxSize) {
        // Calculate buddy using bithack
        auto buddy = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(alloc.memory) ^ levelSize[lv] );

        // Scavenge freeList for our long lost buddy
        for(auto prev = &fList[lv]; prev->next != nullptr; prev = prev->next) {
          if(prev->next == buddy) {
            // Calculate combined memory block using another bithack
            alloc.memory = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(alloc.memory) & ~levelSize[lv++]);
            // Remove buddy from freelist
            prev->extract();
            // Now we're freeing the combined block instead
            alloc.size <<= 1;
            goto loopend;
          }
        }
        break; // Buddy not found
        loopend:; // Buddy found, continue bubbling
      }

      // Place the freed block in the freeList
      fList[lv].emplace(alloc.memory);

      // Clean up the allocation struct
      alloc.memory = nullptr;
      alloc.size = 0;
    };
  };

  void Allocation::free() {
    Allocator::deallocate(*this);
  }
}
