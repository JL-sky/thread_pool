#ifndef __THREADPOOL__
#define __THREADPOOL__

#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <future>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>

class ThreadPool {
public:
    ThreadPool(int size = std::thread::hardware_concurrency()) 
        : pool_size_(size), 
          idle_threads_(0),
          is_stop_(false) {
        for(int i = 0; i < pool_size_; ++i) {
            threads_.emplace_back([this]() {
                worker();
            });
        }
    }

    ~ThreadPool() {
        ShutDown();
    }

    void ShutDown() {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            is_stop_ = true;
        }
        not_empty_.notify_all();
        for(auto& thread : threads_) {
            if(thread.joinable())
                thread.join();
        }
    }

    template<typename F, typename... Args>
    auto Submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using ret_type = decltype(f(args...));
        auto task_ptr = std::make_shared<std::packaged_task<ret_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<ret_type> func_future = task_ptr->get_future();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.emplace([task_ptr]() {
                (*task_ptr)();
            });
        }
        not_empty_.notify_one();
        return func_future;
    }

    // 扩容方法
    void Expand(int new_size) {
        if (new_size <= pool_size_.load()) return;
        
        std::lock_guard<std::mutex> lock(mtx_);
        int add_num = new_size - pool_size_.load();
        for (int i = 0; i < add_num; ++i) {
            threads_.emplace_back([this]() {
                worker();
            });
        }
        pool_size_.store(new_size);
    }

    // 缩容方法
    void Shrink(int new_size) {
        if (new_size >= pool_size_.load() || new_size <= 0) return;
        
        {
            std::lock_guard<std::mutex> lock(mtx_);
            pool_size_.store(new_size);
        }
        // 唤醒所有线程，让多余的线程自然退出
        not_empty_.notify_all();
    }

    // 获取当前线程池状态
    struct PoolStatus {
        int total_threads;
        int idle_threads;
        int queue_size;
    };
    
    PoolStatus GetStatus() {
        std::lock_guard<std::mutex> lock(mtx_);
        return {
            static_cast<int>(threads_.size()),
            idle_threads_.load(),
            static_cast<int>(tasks_.size())
        };
    }

private:
    std::atomic<int> pool_size_;      // 目标线程数
    std::atomic<int> idle_threads_;  // 空闲线程数
    std::atomic<bool> is_stop_;      // 停止标志
    
    std::vector<std::thread> threads_;
    using Task = std::function<void()>;
    std::queue<Task> tasks_;

    std::mutex mtx_;
    std::condition_variable not_empty_;

    void worker() {
        while(true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                
                // 更新空闲线程计数
                idle_threads_++;
                
                // 等待条件：有任务或需要停止或需要缩容
                not_empty_.wait(lock, [this]() {
                    return is_stop_ || !tasks_.empty() || 
                           threads_.size() > pool_size_.load();
                });
                
                // 更新空闲线程计数
                idle_threads_--;
                
                // 退出条件：停止且无任务，或需要缩容且当前线程是多余的
                if ((is_stop_ && tasks_.empty()) || 
                    (threads_.size() > pool_size_.load())) {
                    // 如果是缩容导致的退出，从线程列表中移除当前线程
                    if (threads_.size() > pool_size_.load()) {
                        auto thread_id = std::this_thread::get_id();
                        for (auto it = threads_.begin(); it != threads_.end(); ++it) {
                            if (it->get_id() == thread_id) {
                                it->detach();
                                threads_.erase(it);
                                break;
                            }
                        }
                    }
                    return;
                }
                
                // 获取任务
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            
            // 执行任务
            task();
        }
    }
};

#endif