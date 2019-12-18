
#include <mutex>
#include <condition_variable>
#include <vector>
#include "pink/include/thread_pool.h"


enum cache_notification_mode
{
  NOTIFY_ONE,
  NOTIFY_ALL
};

/**
  Generic "pointer" cache of a fixed size
  with fast put/get operations.

  Compared to STL containers, is faster/does not
  do allocations. However, put() operation will wait
  if there is no free items.
*/
template<typename T, typename std::enable_if<std::is_base_of<pink::TaskArg, T>::value>::type* = nullptr>
class Cache : public pink::TaskArgOwner
{
  std::vector<T> m_base;
  std::vector<T*> m_cache;

  bool is_full()
  {
    return m_cache.size() == m_base.size();
  }

public:
  Cache(size_t count):
  m_base(count), m_cache(count)
  {
    for(size_t i = 0 ; i < count; i++) {
      m_base[i].owner = this;
      m_cache[i]=&m_base[i];
    }
  }

  T* get()
  {
    if(m_cache.empty())
        return nullptr;
    T* ret = m_cache.back();
    m_cache.pop_back();
    return ret;
  }


  void put(T *ele)
  {
    m_cache.push_back(ele);
  }

  bool contains(T* ele)
  {
    return ele >= &m_base[0] && ele <= &m_base[m_base.size() -1];
  }

  size_t size()
  {
    return m_cache.size();
  }

  void gc(pink::TaskArg* ele) override {
    assert(ele->owner == this);
    assert(contains(ele));
    put(static_cast<T*>(ele));
  }
};
