#pragma once

// Notice: TLS object is created only once for each combination of type and
// thread. Only use this when you prefer multiple callers share the same
// instance.
template <class T, class... Args>
inline T &TLS(Args &&...args)
{
    thread_local T _tls_item(std::forward<Args>(args)...);
    return _tls_item;
}

template <class T, class... Args>
inline T &Singleton(Args &&...args)
{
    static T _tls_item(std::forward<Args>(args)...);
    return _tls_item;
}
