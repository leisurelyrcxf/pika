//
// Created by xiaofan on 2020/11/26.
//

#ifndef PIKA_PIKA_UTILS_H
#define PIKA_PIKA_UTILS_H

class Cleaner {
public:
    Cleaner(std::function<void()> f) : cleaner(std::move(f)) {}
    ~Cleaner() { cleaner(); }

private:
    std::function<void()> cleaner;
};

#endif //PIKA_PIKA_UTILS_H
