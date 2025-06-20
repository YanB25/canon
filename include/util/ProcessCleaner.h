#pragma once
#include <iostream>
#include <list>
#include <mutex>
#include <utility>

class ProcessCleaner
{
public:
    using mem_t = std::pair<void *, size_t>;
    static ProcessCleaner &ins()
    {
        static ProcessCleaner instance;
        return instance;
    }

    void reg_alloc(void *mem, size_t size);
    void do_cleanup();

private:
    std::mutex mu_;
    std::list<mem_t> mem_;
};

void register_signal_handlers();