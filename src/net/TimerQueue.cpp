//
// TimerQueue.cpp
//
// Copyright (c) 2017 Jiawei Feng
//

#include "TimerQueue.h"
#include "EventLoop.h"
#include "Timer.h"
#include "TimerId.h"

#include <sys/timerfd.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <algorithm>

namespace Dalin {
namespace Detail {

int createTimerfd()
{
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        fprintf(stderr, "Failed in timerfd_create\n");
        abort();
    }

    return timerfd;
}

struct timespec howMuchTimeFromNow(Timestamp when)
{
    int64_t microseconds = when.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();
    if (microseconds < 100) {
        microseconds = 100;
    }

    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(microseconds / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>(microseconds % Timestamp::kMicroSecondsPerSecond * 1000);

    return ts;
}

void readTimerfd(int timerfd, Timestamp now)
{
    uint64_t howmany;
    ssize_t n = ::read(timerfd, &howmany, sizeof(howmany));
    if (n != sizeof(howmany)) {
        fprintf(stderr, "TimerQueue::handleRead() reads %ld bytes instead of 8\n", n);
    }
}

void resetTimerfd(int timerfd, Timestamp expiration)
{
    // wake up loop by timerfd_settime()
    struct itimerspec newValue, oldValue;

    bzero(&newValue, sizeof(newValue));
    bzero(&oldValue, sizeof(newValue));

    newValue.it_value = howMuchTimeFromNow(expiration);
    int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
    if (ret) {
        fprintf(stderr, "Failed in timerfd_settime\n");
    }
}

}
}

using namespace Dalin;
using namespace Dalin::Net;
using namespace Dalin::Detail;

TimerQueue::TimerQueue(EventLoop *loop)
 : loop_(loop),
   timerfd_(createTimerfd()),
   timerfdChannel_(loop, timerfd_),
   timers_()
{
    timerfdChannel_.setReadCallback(std::bind(&TimerQueue::handleRead, this));
    timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue()
{
    ::close(timerfd_);
    // don't remove channel, since we're in EventLoop::dtor().
    std::for_each(timers_.begin(), timers_.end(), [](const Entry &value){ delete value.second; });
}

TimerId TimerQueue::addTimer(const TimerCallback &cb, Dalin::Timestamp when, double interval)
{
    Timer *timer = new Timer(cb, when, interval);

    loop_->runInLoop([&, timer](){ this->addTimerInLoop(timer); });

    return TimerId(timer);
}

void TimerQueue::addTimerInLoop(Timer *timer)
{
    loop_->assertInLoopThread();

    bool earliestChanged = insert(timer);
    if (earliestChanged) {
        resetTimerfd(timerfd_, timer->expiration());
    }
}

void TimerQueue::handleRead()
{
    loop_->assertInLoopThread();

    Timestamp now(Timestamp::now());
    readTimerfd(timerfd_, now);

    std::vector<Entry> expired = getExpired(now);

    for (auto it = expired.begin(); it != expired.end(); ++it) {
        it->second->run();
    }

    reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Dalin::Timestamp now)
{
    std::vector<Entry> expired;

    Entry sentry = std::make_pair(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
    TimerList::iterator it = timers_.lower_bound(sentry);
    assert(it == timers_.end() || now < it->first);

    std::copy(timers_.begin(), it, std::back_inserter(expired));
    timers_.erase(timers_.begin(), it);

    return expired;
}

void TimerQueue::reset(std::vector<Entry> &expired, Dalin::Timestamp now)
{
    Timestamp nextExpire;

    for (auto it = expired.begin(); it != expired.end(); ++it) {
        if (it->second->repeat()) {
            it->second->restart(now);
            insert(it->second);
        }
        else {
            delete it->second;
        }
    }

    if (!timers_.empty()) {
        nextExpire = timers_.begin()->second->expiration();
    }

    if (nextExpire.valid()) {
        resetTimerfd(timerfd_, nextExpire);
    }
}

bool TimerQueue::insert(Timer *timer)
{
    bool earliestChanged = false;
    Timestamp when = timer->expiration();

    auto it = timers_.begin();
    if (it == timers_.end() || when < it->first) {
        earliestChanged = true;
    }

    auto result = timers_.insert(std::make_pair(when, timer));
    assert(result.second);

    return earliestChanged;
}
