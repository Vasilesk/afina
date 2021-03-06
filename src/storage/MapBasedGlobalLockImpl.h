#ifndef AFINA_STORAGE_MAP_BASED_GLOBAL_LOCK_IMPL_H
#define AFINA_STORAGE_MAP_BASED_GLOBAL_LOCK_IMPL_H

#include <map>
#include <mutex>
#include <string>

#include <tuple>
#include <memory>
#include <mutex>

#include <afina/Storage.h>

namespace Afina {
namespace Backend {

const int content_max_size = 1000;

struct list_elem {
    std::string key;
    std::string value;
    std::shared_ptr<list_elem> prev;
    std::shared_ptr<list_elem> next;

    list_elem(std::string k, std::string v, std::shared_ptr<list_elem> p, std::shared_ptr<list_elem> n) : key(k),
                                                                                                          value(v),
                                                                                                          prev(p),
                                                                                                          next(n)
                                                                                                          {}
    size_t get_size();
};

/**
 * # Map based implementation with global lock
 *
 *
 */
class MapBasedGlobalLockImpl : public Afina::Storage {
public:
    MapBasedGlobalLockImpl(size_t max_size = content_max_size) : _max_size(max_size),
                                                                  _current_size(0)
                                                                  {}
    ~MapBasedGlobalLockImpl() {}

    // Implements Afina::Storage interface
    // returns true if data was not presented before
    bool Put(const std::string &key, const std::string &value) override;

    // Implements Afina::Storage interface
    // returns true if data was set
    bool PutIfAbsent(const std::string &key, const std::string &value) override;

    // Implements Afina::Storage interface
    // returns true if data was set
    bool Set(const std::string &key, const std::string &value) override;

    // Implements Afina::Storage interface
    // returns true if anything was deleted
    bool Delete(const std::string &key) override;

    // Implements Afina::Storage interface
    // returns true if any data was got
    bool Get(const std::string &key, std::string &value) const override;

private:
    size_t _max_size;
    size_t _current_size;
    const std::string err_size = "data size is too big";
    mutable std::mutex _mtx;
    std::map<std::string, std::shared_ptr<list_elem>> _backend;

    std::shared_ptr<list_elem> _content_fst = nullptr;
    std::shared_ptr<list_elem> _content_lst = nullptr;

    // _drop_lst drops LRU element
    bool _drop_lst();

    // _fetch fetches element from LRU list by its pointer
    std::pair<bool, std::shared_ptr<list_elem>> _fetch(const std::string &key);

    // _place_fst places stored data to the last place of LRU
    void _place_fst(std::shared_ptr<list_elem> elem);

    // _insert_fst_new stores data and places it to the last place of LRU list
    // returns true if data was stored
    bool _insert_fst_new(const std::string &key, const std::string &value);
};

void pop_from_list(std::shared_ptr<list_elem>);

} // namespace Backend
} // namespace Afina

#endif // AFINA_STORAGE_MAP_BASED_GLOBAL_LOCK_IMPL_H
