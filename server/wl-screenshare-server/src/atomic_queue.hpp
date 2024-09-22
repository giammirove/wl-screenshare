#include <mutex>
#include <queue>

template <typename T> class atomic_queue {
public:
  void push(const T &value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queque.push(value);
  }

  T pop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    T f = m_queque.front();
    m_queque.pop();
    return f;
  }

  bool empty() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queque.empty();
  }

private:
  std::queue<T> m_queque;
  mutable std::mutex m_mutex;
};
