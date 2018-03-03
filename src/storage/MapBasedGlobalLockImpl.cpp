#include "MapBasedGlobalLockImpl.h"

#include <mutex>

#include <tuple>
#include <memory>

namespace Afina {
namespace Backend {

size_t list_elem::get_size() {
    return key.size() + value.size();
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Put(const std::string &key, const std::string &value) {
    auto record = _fetch(key);
    _insert_fst_new(key, value);

    return !record.first;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::PutIfAbsent(const std::string &key, const std::string &value) {
    auto record = _fetch(key);
    if (record.first) {
        _place_fst(record.second);
    } else {
        _insert_fst_new(key, value);
    }

    return !record.first;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Set(const std::string &key, const std::string &value) {
    auto record = _fetch(key);
    if (record.first) {
       _insert_fst_new(key, value);
    }

    return record.first;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Delete(const std::string &key) {
    auto record = _fetch(key);
    if(record.first) {
      _backend.erase(key);
    }

    return record.first;
}

// See MapBasedGlobalLockImpl.h
bool MapBasedGlobalLockImpl::Get(const std::string &key, std::string &value) const {
    auto record = const_cast<MapBasedGlobalLockImpl*>(this)->_fetch(key);
    if(record.first) {
        value = record.second->value;
    }

    return record.first;
}

std::pair<bool, std::shared_ptr<list_elem>> MapBasedGlobalLockImpl::_fetch(std::string key) {
    if (_backend.find(key) != _backend.end()) {
        auto elem = _backend[key];
        pop_from_list(elem);
        return std::pair<bool, std::shared_ptr<list_elem>>(true, elem);
    }

    return std::pair<bool, std::shared_ptr<list_elem>>(false, nullptr);
}

bool MapBasedGlobalLockImpl::_drop_lst() {
    bool exists = _content_lst != nullptr;
    if(exists) {
        auto content = _backend.find(_content_lst->key);
        if (content != _backend.end()) {
            _backend.erase(content);
        }
        _current_size -= 1;
        pop_from_list(_content_lst);
        _content_lst = _content_lst->prev;
        if(_content_lst == nullptr) {
            _content_fst = _content_lst;
        }
    }

    return exists;
}

void MapBasedGlobalLockImpl::_place_fst(std::shared_ptr<list_elem> elem) {
    elem->prev = nullptr;
    elem->next = _content_fst;
    if(_content_fst != nullptr) {
        _content_fst->prev = elem;
    }
    _content_fst = elem;
    if(_content_lst == nullptr) {
        _content_lst = elem;
    }
}

bool MapBasedGlobalLockImpl::_insert_fst_new(std::string key, std::string value) {
    std::shared_ptr<list_elem>elem = std::make_shared<list_elem>(key, value, nullptr, nullptr);
    _backend[key] = elem;
    _place_fst(elem);

    _current_size += 1;
    if(_current_size > _max_size) {
        bool to_drop = _drop_lst();

        while (_current_size > _max_size && to_drop) {
          to_drop = _drop_lst();
        }
    }

    return true;
}

void pop_from_list(std::shared_ptr<list_elem> elem) {
    if(elem->prev != nullptr) {
        elem->prev->next = elem->next;
    }
    if(elem->next != nullptr) {
        elem->next->prev = elem->prev;
    }
}

} // namespace Backend
} // namespace Afina
