#include<dummy.h>
namespace pjssaber {

class handle_table {

public:

    handle_table(): length_(0), elem_(0), list_(nullptr) { Resize(); }
    ~handle_table() { delete[] list_; list_ = nullptr; }

    LRUHandle* lookup(const Slice& key, uint32_t hash) {
        return *FindPointer(key, hash);
    }

    LRUHandle* Insert(LRUHandle* h) {
        LRUHandle** it = FindPointer(h->key(), h->hash);
        LRUHandle* old = *it;
        h->next_hash = old == nullptr? nullptr: old->next_hash;
        *it = h;
        if (old == nullptr) {
            elem_++;
            if (elem_ >= length_) {
                Resize();
            }
        }
        return old;
    }

    LRUHandle* Remove(const Slice& key, uint32_t hash) {
        LRUHandle** it = FindPointer(key, hash);
        LRUHandle* old = *it;
        if (old != nullptr) {
            elem_--;
            (*it)  = (*it) ->next_hash;          // 注意理解这里指针的使用，雀氏很难搞透澈
        }
        return old;
    }

private:
    uint32_t length_;  // the length of array
    uint32_t elem_; // number of elements, use to realocate ary size
    LRUHandle** list_; // this is the actually ary

    LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
        LRUHandle** ptr = &list_[hash & (length_ - 1)];
        while (*ptr != nullptr && (hash != (*ptr)->hash || key != (*ptr)->key())) {
            ptr = &(*ptr) -> next_hash;
        }
        return ptr;
    }

    void Resize() {   // resize all the members of the original list. So all we don't need to destroy 
        uint32_t new_length = 4;
        while (new_length <= length_) {
            new_length = 2 * new_length;
        }

        LRUHandle** new_list = new LRUHandle*[new_length];
        memset(new_list, 0, sizeof(LRUHandle*) * new_length);
        assert(new_list != nullptr);  
        uint32_t count = 0;
        // move old list_ to new_list
        for (uint32_t i = 0; i < length_; i++) {  // iter all the pos
            auto it = list_[i];
            while (it != nullptr) {   // iter for all the hash_confict members
                uint32_t new_pos = (it->hash) & (new_length - 1);
                // 将节点放入new_list第一个位置，避免在new_list上遍历
                auto next = it -> next_hash;
                it ->next_hash = new_list[new_pos];  // 头插法
                new_list[new_pos] = it;
                it = next;
                count++;
            }
        }
        assert(count == elem_);
        delete[] list_;
        list_ = new_list;
        length_ = new_length; 
    }
};

}