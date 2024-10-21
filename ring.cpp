#include<iostream>
#include<algorithm>
#include<vector>
#include <cassert>

namespace pjssaber {
/*
    Implementation of a vector based ring from scratch.
    Hoping to build a thread-safe, lock-free ring gradually.
 */
template<typename T>
class base_ring {

public:

    explicit base_ring(int sz) {
        assert(sz >= 1);
        size = sz + 1;
        ary.resize(size);
        head = 0;
        rear = 0;
    }

    ~base_ring() {
    }

    bool empty() {  // return whether ring is empty or not 
        return head == rear;
    }   

    bool full() {  // return whether ring is full or not 
        return head == (rear+1) % size;
    }   

    void pop() {
        if (this->empty()) {
            std::cout<< "Empty ring"<< std::endl;
            return;
        }
        /*
            deconstructive ary[head]
         */
        head = (head + 1) % size;
    }

    void push(T& val) {
        if (this->full()) {
            this->pop();
        }
        assert(rear >= size);
        ary[rear] = val;
        rear = (rear + 1) % size;
    }

private:
    std::vector <T> ary;
    int size;
    int head, rear;
};

}