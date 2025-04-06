#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
 public:
    ThreadPool(int size = std::thread::hardware_concurrency())
        : pool_size_(size), isStop_(false) {
        for (int i = 0; i < pool_size_; ++i) {
            // threads_.push_back(std::thread(&ThreadPool::worker,this));
            threads_.emplace_back([this]() { worker(); });
        }
    }

    ~ThreadPool() { ShutDown(); }

    void ShutDown() {
        isStop_ = true;

        not_empty_cond_.notify_all();

        for (auto &thread : threads_) {
            if (thread.joinable())
                thread.join();
        }
    }

    template <typename F, typename... Args>
    auto Submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))> {
        using func_type = decltype(f(args...));
        auto task_ptr = std::make_shared<std::packaged_task<func_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<func_type> func_future = task_ptr->get_future();

        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (isStop_)
                throw std::runtime_error("threadpool has stop!!!");
            task_queue_.emplace([task_ptr]() { (*task_ptr)(); });
        }
        not_empty_cond_.notify_one();

        return func_future;
    }

 private:
    std::atomic_bool isStop_;
    int pool_size_;

    using Task = std::function<void()>;

    std::vector<std::thread> threads_;
    std::queue<Task> task_queue_;

    std::mutex mtx_;
    std::condition_variable not_empty_cond_;

    void worker() {
        while (1) {
            std::unique_lock<std::mutex> lock(mtx_);

            not_empty_cond_.wait(
                lock, [this]() { return isStop_ || !task_queue_.empty(); });

            if (isStop_ && task_queue_.empty())
                return;

            Task task = task_queue_.front();
            task_queue_.pop();

            lock.unlock();

            task();
        }
        return;
    }
};

#endif // THREADPOOL_H
