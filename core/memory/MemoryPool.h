#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <cstddef>
#include <mutex>
#include <vector>
#include <iostream>

class MemoryPool {
public:
    // 无参单例访问 — 调用 init() 完成两阶段初始化
    static MemoryPool& getInstance() {
        static MemoryPool instance;
        return instance;
    }

    // 两阶段初始化：main() 中只调用一次
    void init(size_t blockSize, int initialBlocks = 4) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (initialized_) {
            std::cerr << "[MemoryPool] Warning: double init ignored." << std::endl;
            return;
        }
        block_size_ = blockSize;
        expandPool(initialBlocks);
        initialized_ = true;
    }

    // 分配一块内存 (O(1))
    void* allocate() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!initialized_) return nullptr;  // fail-fast
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
    MemoryPool() = default;  // 默认构造，等待 init()

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    void expandPool(int numBlocks) {
        for (int i = 0; i < numBlocks; ++i) {
            void* newBlock = ::operator new(block_size_);
            chunks_.push_back(newBlock);
            // 插入空闲链表头部
            *reinterpret_cast<void**>(newBlock) = free_head_;
            free_head_ = newBlock;
        }
    }

    size_t block_size_ = 0;
    void* free_head_ = nullptr;
    std::vector<void*> chunks_;
    std::mutex mtx_;
    bool initialized_ = false;
};

#endif
