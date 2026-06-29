#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <iostream>
#include <vector>
#include <mutex>
#include <stack>

class MemoryPool {
public:
    // 单例模式，全局共享一个池
    static MemoryPool& getInstance(size_t blockSize, int initialBlocks = 4) {
        static MemoryPool instance(blockSize, initialBlocks);
        return instance;
    }

    // 分配一块内存
    void* allocate() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (free_list_.empty()) {
            // 如果池空了，动态扩容（面试可以提：预留动态扩容能力）
            expandPool(4);
        }
        void* ptr = free_list_.top();
        free_list_.pop();
        return ptr;
    }

    // 归还内存
    void deallocate(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(mtx_);
        free_list_.push(ptr);
    }

    ~MemoryPool() {
        for (void* ptr : chunks_) {
            ::operator delete(ptr);
        }
    }

private:
    MemoryPool(size_t blockSize, int initialBlocks) : block_size_(blockSize) {
        expandPool(initialBlocks);
    }

    void expandPool(int numBlocks) {
        for (int i = 0; i < numBlocks; ++i) {
            void* newBlock = ::operator new(block_size_);
            chunks_.push_back(newBlock);
            free_list_.push(newBlock);
        }
    }

    size_t block_size_;
    std::stack<void*> free_list_; // 空闲链表（用栈模拟 LIFO，提高缓存命中率）
    std::vector<void*> chunks_;   // 记录所有申请的大块内存，方便析构时释放
    std::mutex mtx_;
};

// 重载 new 运算符的辅助宏，方便外部使用
#define DECLARE_POOL_NEW_DELETE(CLASS_NAME) \
    static void* operator new(size_t size) { return MemoryPool::getInstance(sizeof(CLASS_NAME)).allocate(); } \
    static void operator delete(void* ptr) { MemoryPool::getInstance(sizeof(CLASS_NAME)).deallocate(ptr); }

#endif // MEMORY_POOL_H