#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

class RateLimiter {
public:
    /**
     * @param bytes_per_second 每秒允许通过的字节数
     */
    explicit RateLimiter(double bytes_per_second)
        : rate_(bytes_per_second),
          tokens_(bytes_per_second),  // 初始令牌桶满
          max_tokens_(bytes_per_second),
          last_refill_time_(std::chrono::steady_clock::now()) {}

    /**
     * 消费指定数量的令牌，如果不足则阻塞等待
     * @param bytes 需要消费的字节数
     */
    void consume(std::size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        refill();

        if (bytes > max_tokens_) {
            // 如果请求超过桶容量，按最大速率分段消费
            while (bytes > 0) {
                std::size_t chunk = std::min(bytes, static_cast<std::size_t>(max_tokens_));
                wait_for_tokens(chunk, lock);
                tokens_ -= chunk;
                bytes -= chunk;
            }
        } else {
            wait_for_tokens(bytes, lock);
            tokens_ -= bytes;
        }
    }

    /**
     * 获取当前速率配置
     */
    double rate() const { return rate_; }

    /**
     * 动态修改速率限制
     * @param bytes_per_second 新的每秒字节数
     */
    void set_rate(double bytes_per_second) {
        std::lock_guard<std::mutex> lock(mutex_);
        rate_ = bytes_per_second;
        max_tokens_ = bytes_per_second;
        tokens_ = std::min(tokens_, max_tokens_);
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_refill_time_).count();
        tokens_ = std::min(max_tokens_, tokens_ + elapsed * rate_);
        last_refill_time_ = now;
    }

    void wait_for_tokens(std::size_t bytes, std::unique_lock<std::mutex>& lock) {
        while (tokens_ < bytes) {
            double deficit = bytes - tokens_;
            double wait_seconds = deficit / rate_;
            auto wait_duration = std::chrono::duration<double>(wait_seconds);

            // 释放锁并休眠
            lock.unlock();
            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(wait_duration).count()
                ))
            );
            lock.lock();

            refill();
        }
    }

    double rate_;              // 每秒字节数
    double tokens_;            // 当前令牌数
    double max_tokens_;        // 桶容量
    std::chrono::steady_clock::time_point last_refill_time_;
    std::mutex mutex_;
};
