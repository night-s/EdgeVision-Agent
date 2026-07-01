#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <cstddef>
#include <mutex>
#include <vector>

class MemoryPool {
public:
    // 单例模式，全局共享
    static MemoryPool& getInstance(size_t blockSize, int initialBlocks = 4) {
        static MemoryPool instance(blockSize, initialBlocks);
        return instance;
    }

    // 分配一块内存 (O(1))
    void* allocate() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (free_head_ == nullptr) {
            expandPool(4); // 自动扩容
        }
        void* ptr = free_head_;
        // 侵入式链表核心：内存块头部存放了下一个空闲块的地址
        free_head_ = *reinterpret_cast<void**>(free_head_); 
        return ptr;
    }

    // 归还一块内存 (O(1))
    void deallocate(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(mtx_);
        // 将归还的块插入链表头部
        *reinterpret_cast<void**>(ptr) = free_head_; 
        free_head_ = ptr;
    }

    ~MemoryPool() {
        for (void* chunk : chunks_) {
            ::operator delete(chunk);
        }
    }

private:
    MemoryPool(size_t blockSize, int initialBlocks) : block_size_(blockSize), free_head_(nullptr) {
        expandPool(initialBlocks);
    }

    void expandPool(int numBlocks) {
        for (int i = 0; i < numBlocks; ++i) {
            void* newBlock = ::operator new(block_size_);
            chunks_.push_back(newBlock);
            // 插入空闲链表头部
            *reinterpret_cast<void**>(newBlock) = free_head_;
            free_head_ = newBlock;
        }
    }

    size_t block_size_;
    void* free_head_;        // 空闲链表头部（侵入式）
    std::vector<void*> chunks_; // 用于析构时统一释放
    std::mutex mtx_;
};

#endif