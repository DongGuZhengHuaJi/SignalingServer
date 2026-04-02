#ifndef MY_THREAD_POOL_H
#define MY_THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using Task  = std::function<void()>;

class ThreadPool {
public:
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    static ThreadPool& getInstance(){
        static ThreadPool instance;
        return instance;
    }
    
    ~ThreadPool(){
        stop();
    }
    template<typename F, typename ...Args>
    auto commit(F&& f, Args&& ...args) -> std::future<decltype(f(args...))>{
        using rtn_type = decltype(f(args...));
        
        if(stop_.load()){
            return std::future<rtn_type>();
        }
        
        auto task = std::make_shared<std::packaged_task<rtn_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<rtn_type> future = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.emplace([task](){ (*task)(); });
        }
        cond_.notify_one();
        return future;
    }
    
    int idleThreadCnt(){
        return idle_thread_num_.load();
    }

private:
    std::atomic<bool> stop_;
    std::atomic<int> idle_thread_num_;
    int thread_num_;
    std::vector<std::thread> pool_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cond_;

private:
    ThreadPool(int thread_num = std::thread::hardware_concurrency())
    :stop_(false), idle_thread_num_(thread_num), thread_num_(thread_num){
        start();
    }

    void start(){
        for(int i=0;i<thread_num_;i++){
            pool_.emplace_back([this](){
                while(true){
                    Task task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cond_.wait(lock, [this](){
                            return stop_.load() || !tasks_.empty();
                        });

                        if(stop_.load() && tasks_.empty()){
                            return;
                        }
                        
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    idle_thread_num_--;
                    task();
                    idle_thread_num_++;
                }
            });
        }
    }

    void stop(){
        stop_.store(true);
        cond_.notify_all();
        for(std::thread &thread : pool_){
            if(thread.joinable()){
                thread.join();
            }
        }
    }

};

#endif // MY_THREAD_POOL_H
