#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CoreHelpers {

class EventSubscription {
public:
    EventSubscription() = default;
    explicit EventSubscription(std::function<void()> unsubscribe)
        : unsubscribe_(std::move(unsubscribe)) {}

    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;

    EventSubscription(EventSubscription&& other) noexcept
        : unsubscribe_(std::move(other.unsubscribe_)) {
        other.unsubscribe_ = nullptr;
    }

    EventSubscription& operator=(EventSubscription&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        unsubscribe_ = std::move(other.unsubscribe_);
        other.unsubscribe_ = nullptr;
        return *this;
    }

    ~EventSubscription() {
        Reset();
    }

    void Reset() {
        if (unsubscribe_) {
            unsubscribe_();
            unsubscribe_ = nullptr;
        }
    }

private:
    std::function<void()> unsubscribe_;
};

template <typename TEvent>
class Event {
public:
    using Handler = std::function<void(const TEvent&)>;

    EventSubscription Subscribe(Handler handler) {
        static_assert(!std::is_reference<TEvent>::value, "TEvent must not be a reference type");
        if (!handler) {
            return EventSubscription();
        }

        const uint64_t id = nextId_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handlers_[id] = std::move(handler);
        }

        return EventSubscription([this, id]() { Unsubscribe(id); });
    }

    void Publish(const TEvent& event) const {
        std::vector<Handler> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.reserve(handlers_.size());
            for (const auto& kv : handlers_) {
                snapshot.push_back(kv.second);
            }
        }

        for (const auto& handler : snapshot) {
            if (handler) {
                handler(event);
            }
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_.clear();
    }

private:
    void Unsubscribe(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_.erase(id);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Handler> handlers_;
    std::atomic<uint64_t> nextId_{1};
};

class EventBus {
public:
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template <typename TEvent>
    Event<TEvent>& GetEvent() {
        const std::type_index key(typeid(TEvent));
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = events_.find(key);
        if (it == events_.end()) {
            auto inserted = events_.emplace(key, std::make_shared<TypedEventHolder<TEvent>>());
            it = inserted.first;
        }

        return static_cast<TypedEventHolder<TEvent>*>(it->second.get())->event;
    }

    template <typename TEvent>
    EventSubscription Subscribe(std::function<void(const TEvent&)> handler) {
        return GetEvent<TEvent>().Subscribe(std::move(handler));
    }

    template <typename TEvent>
    void Publish(const TEvent& event) {
        GetEvent<TEvent>().Publish(event);
    }

private:
    struct IEventHolder {
        virtual ~IEventHolder() = default;
    };

    template <typename TEvent>
    struct TypedEventHolder : IEventHolder {
        Event<TEvent> event;
    };

private:
    std::mutex mutex_;
    std::unordered_map<std::type_index, std::shared_ptr<IEventHolder>> events_;
};

}  // namespace CoreHelpers
