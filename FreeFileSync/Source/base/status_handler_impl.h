// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef STATUS_HANDLER_IMPL_H_07682758976
#define STATUS_HANDLER_IMPL_H_07682758976

#include <zen/basic_math.h>
#include <zen/file_error.h>
#include <zen/thread.h>
#include "process_callback.h"
#include "speed_test.h"


namespace fff
{
class AsyncCallback //actor pattern
{
public:
    AsyncCallback() {}

    //non-blocking: context of worker thread (and main thread, see reportStats())
    void updateDataProcessed(int itemsDelta, int64_t bytesDelta) //noexcept!
    {
        itemsDeltaProcessed_ += itemsDelta;
        bytesDeltaProcessed_ += bytesDelta;
    }
    void updateDataTotal(int itemsDelta, int64_t bytesDelta) //noexcept!
    {
        itemsDeltaTotal_ += itemsDelta;
        bytesDeltaTotal_ += bytesDelta;
    }

    //context of worker thread
    void updateStatus(std::wstring&& msg) //throw ThreadStopRequest
    {
        assert(!zen::runningOnMainThread());
        {
            std::lock_guard dummy(lockCurrentStatus_);
            if (ThreadStatus* ts = getThreadStatus()) //call while holding "lockCurrentStatus_" lock!!
                ts->statusMsg = std::move(msg);
            else assert(false);
        }
        zen::interruptionPoint(); //throw ThreadStopRequest
    }

    //blocking call: context of worker thread
    //=> indirect support for "pause": logInfo() is called under singleThread lock,
    //   so all other worker threads will wait when coming out of parallel I/O (trying to lock singleThread)
    void logInfo(const std::wstring& msg) //throw ThreadStopRequest
    {
        assert(!zen::runningOnMainThread());
        std::unique_lock dummy(lockRequest_);
        zen::interruptibleWait(conditionReadyForNewRequest_, dummy, [this] { return !logInfoRequest_; }); //throw ThreadStopRequest

        logInfoRequest_ = /*std::move(taskPrefix) + */ msg;

        dummy.unlock(); //optimization for condition_variable::notify_all()
        conditionNewRequest.notify_all();
    }

    void reportInfo(std::wstring&& msg) //throw ThreadStopRequest
    {
        logInfo(msg);                 //throw ThreadStopRequest
        updateStatus(std::move(msg)); //
    }

    //blocking call: context of worker thread
    PhaseCallback::Response reportError(const PhaseCallback::ErrorInfo& errorInfo) //throw ThreadStopRequest
    {
        assert(!zen::runningOnMainThread());
        std::unique_lock dummy(lockRequest_);
        zen::interruptibleWait(conditionReadyForNewRequest_, dummy, [this] { return !errorRequest_ && !errorResponse_; }); //throw ThreadStopRequest

        errorRequest_ = errorInfo;
        conditionNewRequest.notify_all();

        zen::interruptibleWait(conditionHaveResponse_, dummy, [this] { return static_cast<bool>(errorResponse_); }); //throw ThreadStopRequest

        PhaseCallback::Response rv = *errorResponse_;

        errorRequest_  = std::nullopt;
        errorResponse_ = std::nullopt;

        dummy.unlock(); //optimization for condition_variable::notify_all()
        conditionReadyForNewRequest_.notify_all(); //=> spurious wake-up for AsyncCallback::logInfo()
        return rv;
    }

    //context of main thread
    void waitUntilDone(std::chrono::milliseconds cbInterval, PhaseCallback& cb) //throw X
    {
        assert(zen::runningOnMainThread());
        for (;;)
        {
            const std::chrono::steady_clock::time_point callbackTime = std::chrono::steady_clock::now() + cbInterval;

            for (std::unique_lock dummy(lockRequest_);;) //process all errors without delay
            {
                const bool rv = conditionNewRequest.wait_until(dummy, callbackTime, [this] { return (errorRequest_ && !errorResponse_) || logInfoRequest_ || finishNowRequest_; });
                if (!rv) //time-out + condition not met
                    break;

                if (errorRequest_ && !errorResponse_)
                {
                    assert(!finishNowRequest_);
                    errorResponse_ = cb.reportError(*errorRequest_); //throw X
                    conditionHaveResponse_.notify_all(); //instead of notify_one(); work around bug: https://svn.boost.org/trac/boost/ticket/7796
                }
                if (logInfoRequest_)
                {
                    cb.logInfo(*logInfoRequest_); //throw X
                    logInfoRequest_ = {};
                    conditionReadyForNewRequest_.notify_all(); //=> spurious wake-up for AsyncCallback::reportError()
                }
                if (finishNowRequest_)
                {
                    dummy.unlock(); //call member functions outside of mutex scope:
                    reportStats(cb); //one last call for accurate stat-reporting!
                    return;
                }
            }

            //call back outside of mutex scope:
            cb.updateStatus(getCurrentStatus()); //throw X
            reportStats(cb);
        }
    }

    void notifyTaskBegin(size_t prio) //noexcept
    {
        assert(!zen::runningOnMainThread());
        const std::thread::id threadId = std::this_thread::get_id();
        std::lock_guard dummy(lockCurrentStatus_);
        assert(!getThreadStatus());

        //const size_t taskIdx = [&]() -> size_t
        //{
        //    auto it = std::find(usedIndexNums_.begin(), usedIndexNums_.end(), false);
        //    if (it != usedIndexNums_.end())
        //    {
        //        *it = true;
        //        return it - usedIndexNums_.begin();
        //    }

        //    usedIndexNums_.push_back(true);
        //    return usedIndexNums_.size() - 1;
        //}();

        if (statusByPriority_.size() < prio + 1)
            statusByPriority_.resize(prio + 1);

        statusByPriority_[prio].push_back({threadId, /*taskIdx,*/ std::wstring()});
    }

    void notifyTaskEnd() //noexcept
    {
        assert(!zen::runningOnMainThread());
        const std::thread::id threadId = std::this_thread::get_id();
        std::lock_guard dummy(lockCurrentStatus_);

        for (std::vector<ThreadStatus>& sbp : statusByPriority_)
            for (ThreadStatus& ts : sbp)
                if (ts.threadId == threadId)
                {
                    //usedIndexNums_[ts.taskIdx] = false;
                    std::swap(ts, sbp.back());
                    sbp.pop_back();
                    return;
                }
        assert(false);
    }

    void notifyAllDone() //noexcept
    {
        std::lock_guard dummy(lockRequest_);
        assert(!finishNowRequest_);
        finishNowRequest_ = true;
        conditionNewRequest.notify_all(); //perf: should unlock mutex before notify!? (insignificant)
    }

private:
    AsyncCallback           (const AsyncCallback&) = delete;
    AsyncCallback& operator=(const AsyncCallback&) = delete;

    struct ThreadStatus
    {
        std::thread::id threadId;
        //size_t   taskIdx = 0; //nice human-readable task id for GUI
        std::wstring statusMsg;
    };

    ThreadStatus* getThreadStatus() //call while holding "lockCurrentStatus_" lock!!
    {
        assert(!zen::runningOnMainThread());
        const std::thread::id threadId = std::this_thread::get_id();

        for (std::vector<ThreadStatus>& sbp : statusByPriority_)
            for (ThreadStatus& ts : sbp) //thread count is (hopefully) small enough so that linear search won't hurt perf
                if (ts.threadId == threadId)
                    return &ts;
        return nullptr;
    }

#if 0 //maybe not that relevant after all!?
    std::wstring getTaskPrefix() //call *outside* of "lockCurrentStatus_" lock!!
    {
        const size_t taskIdx = [&]
        {
            std::lock_guard dummy(lockCurrentStatus_);
            const ThreadStatus* ts = getThreadStatus(); //call while holding "lockCurrentStatus_" lock!!
            return ts ? ts->taskIdx : static_cast<size_t>(-2);
        }();
        return totalThreadCount_ > 1 ? L'[' + zen::formatNumber(taskIdx + 1) + L"] " : L"";
    }
#endif

    //context of main thread
    void reportStats(PhaseCallback& cb)
    {
        assert(zen::runningOnMainThread());

        const std::pair<int, int64_t> deltaProcessed(itemsDeltaProcessed_, bytesDeltaProcessed_);
        if (deltaProcessed.first != 0 || deltaProcessed.second != 0)
        {
            updateDataProcessed   (-deltaProcessed.first, -deltaProcessed.second); //careful with these atomics: don't just set to 0
            cb.updateDataProcessed( deltaProcessed.first,  deltaProcessed.second); //noexcept!
        }
        const std::pair<int, int64_t> deltaTotal(itemsDeltaTotal_, bytesDeltaTotal_);
        if (deltaTotal.first != 0 || deltaTotal.second != 0)
        {
            updateDataTotal   (-deltaTotal.first, -deltaTotal.second);
            cb.updateDataTotal( deltaTotal.first,  deltaTotal.second); //noexcept!
        }
    }

    //context of main thread, call repreatedly
    std::wstring getCurrentStatus()
    {
        assert(zen::runningOnMainThread());

        size_t parallelOpsTotal = 0;
        std::wstring statusMsg;
        {
            std::lock_guard dummy(lockCurrentStatus_);

            for (const auto& sbp : statusByPriority_)
                parallelOpsTotal += sbp.empty() ? 0 : 1;
            statusMsg = [&]
            {
                for (const std::vector<ThreadStatus>& sbp : statusByPriority_)
                    for (const ThreadStatus& ts : sbp)
                        if (!ts.statusMsg.empty())
                            return ts.statusMsg;
                return std::wstring();
            }();
        }
        if (parallelOpsTotal >= 2)
            return L'[' + _P("1 thread", "%x threads", parallelOpsTotal) + L"] " + statusMsg;
        else
            return statusMsg;
    }

    //---- main <-> worker communication channel ----
    std::mutex lockRequest_;
    std::condition_variable conditionReadyForNewRequest_;
    std::condition_variable conditionNewRequest;
    std::condition_variable conditionHaveResponse_;
    std::optional<PhaseCallback::ErrorInfo> errorRequest_;
    std::optional<PhaseCallback::Response > errorResponse_;
    std::optional<std::wstring>             logInfoRequest_;
    bool finishNowRequest_ = false;

    //---- status updates ----
    std::mutex lockCurrentStatus_; //different lock for status updates so that we're not blocked by other threads reporting errors
    std::vector<std::vector<ThreadStatus>> statusByPriority_;
    //give status messages priority according to their folder pair (e.g. first folder pair has prio 0) => visualize (somewhat) natural processing order

    //std::vector<char/*bool*/> usedIndexNums_; //keep info for human-readable task index numbers

    //---- status updates II (lock-free) ----
    std::atomic<int>     itemsDeltaProcessed_{0}; //
    std::atomic<int64_t> bytesDeltaProcessed_{0}; //std:atomic is uninitialized by default!
    std::atomic<int>     itemsDeltaTotal_    {0}; //
    std::atomic<int64_t> bytesDeltaTotal_    {0}; //
};


//manage statistics reporting for a single item of work
template <class Callback>
class ItemStatReporter
{
public:
    ItemStatReporter(int itemsExpected, int64_t bytesExpected, Callback& cb) :
        itemsExpected_(itemsExpected),
        bytesExpected_(bytesExpected),
        cb_(cb) {}

    ~ItemStatReporter()
    {
        const bool scopeFail = std::uncaught_exceptions() > exeptionCount_;
        if (scopeFail)
            cb_.updateDataTotal(itemsReported_, bytesReported_); //=> unexpected increase of total workload
        else
            //update statistics to consider the real amount of data, e.g. more than the "file size" for ADS streams,
            //less for sparse and compressed files,  or file changed in the meantime!
            cb_.updateDataTotal(itemsReported_ - itemsExpected_, bytesReported_ - bytesExpected_); //noexcept!
    }

    void updateStatus(std::wstring&& msg) { cb_.updateStatus(std::move(msg)); } //throw X

    void reportDelta(int itemsDelta, int64_t bytesDelta) //noexcept!
    {
        cb_.updateDataProcessed(itemsDelta, bytesDelta); //noexcept!
        itemsReported_ += itemsDelta;
        bytesReported_ += bytesDelta;

        //special rule: avoid temporary statistics mess up, even though they are corrected anyway below:
        if (itemsReported_ > itemsExpected_)
        {
            cb_.updateDataTotal(itemsReported_ - itemsExpected_, 0); //noexcept!
            itemsReported_ = itemsExpected_;
        }
        if (bytesReported_ > bytesExpected_)
        {
            cb_.updateDataTotal(0, bytesReported_ - bytesExpected_); //=> everything above "bytesExpected" adds to both "processed" and "total" data
            bytesReported_ = bytesExpected_;
        }
    }

    int64_t getBytesReported() const { return bytesReported_; }
    int64_t getBytesExpected() const { return bytesExpected_; }

private:
    int itemsReported_ = 0;
    int64_t bytesReported_ = 0;
    const int itemsExpected_;
    const int64_t bytesExpected_;
    Callback& cb_;
    const int exeptionCount_ = std::uncaught_exceptions();
};

using AsyncItemStatReporter = ItemStatReporter<AsyncCallback>;

//=====================================================================================================================

constexpr std::chrono::seconds STATUS_PERCENT_DELAY(2);
constexpr std::chrono::seconds STATUS_PERCENT_MIN_DURATION(3);
const int                      STATUS_PERCENT_MIN_CHANGES_PER_SEC = 2;
constexpr std::chrono::seconds STATUS_PERCENT_SPEED_WINDOW(10);

template <class Callback>
struct PercentStatReporter
{
    PercentStatReporter(std::wstring&& statusMsg, int64_t bytesExpected, Callback& cb) : //throw X
        msgPrefix_(statusMsg + L"... "),
        statReporter_(1 /*itemsExpected*/, bytesExpected, cb)
    {
        statReporter_.updateStatus(std::move(statusMsg)); //throw X
    }

    void updateStatus(int itemsDelta, int64_t bytesDelta) //throw X
    {
        statReporter_.reportDelta(itemsDelta, bytesDelta);

        const auto now = std::chrono::steady_clock::now();
        if (now >= lastUpdate_ + UI_UPDATE_INTERVAL / 2) //every ~50 ms
        {
            lastUpdate_ = now;

            const int64_t bytesCopied = statReporter_.getBytesReported();
            const int64_t bytesTotal  = statReporter_.getBytesExpected();

            if (!showPercent_ && bytesCopied > 0)
            {
                if (startTime_ == std::chrono::steady_clock::time_point())
                {
                    startTime_ = now; //get higher-quality perf stats when starting timing here rather than constructor!?
                    speedTest_.addSample(std::chrono::seconds(0), 0 /*itemsCurrent*/, bytesCopied);
                }
                else if (const std::chrono::nanoseconds elapsed = now - startTime_;
                         elapsed >= STATUS_PERCENT_DELAY)
                {
                    speedTest_.addSample(elapsed, 0 /*itemsCurrent*/, bytesCopied);

                    if (const std::optional<double> remSecs = speedTest_.getRemainingSec(0, bytesTotal - bytesCopied))
                        if (*remSecs > std::chrono::duration<double>(STATUS_PERCENT_MIN_DURATION).count())
                        {
                            showPercent_ = true;
                            speedTest_.clear(); //discard (probably messy) numbers
                        }
                }
            }
            if (showPercent_)
            {
                speedTest_.addSample(now - startTime_, 0 /*itemsCurrent*/, bytesCopied);
                const std::optional<double> bps = speedTest_.getBytesPerSec();

                statReporter_.updateStatus(msgPrefix_ + formatPercent(std::min(static_cast<double>(bytesCopied) / bytesTotal, 1.0), //> 100% possible! see process_callback.h notes
                                                                      bps ? *bps : 0, bytesTotal)); //throw X
            }
        }
    }

    void updateStatus(std::wstring&& msg) { statReporter_.updateStatus(std::move(msg)); } //throw X

private:
    static std::wstring formatPercent(double fraction, double bytesPerSec, int64_t bytesTotal)
    {
        const double totalSecs = numeric::isNull(bytesPerSec) ? 0 : bytesTotal / bytesPerSec;
        const double expectedSteps = totalSecs * STATUS_PERCENT_MIN_CHANGES_PER_SEC;

        const wchar_t* format = [&] //TODO? protect against format flickering!?
        {
            if (expectedSteps <=   100) return L"%.0f";
            if (expectedSteps <=  1000) return L"%.1f";
            if (expectedSteps <= 10000) return L"%.2f";
            /**/                        return L"%.3f";
        }();
        return zen::printNumber<std::wstring>(format, fraction * 100) + L'%'; //need to localize percent!?
    }

    bool showPercent_ = false;
    const std::wstring msgPrefix_;
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point lastUpdate_;
    SpeedTest speedTest_{STATUS_PERCENT_SPEED_WINDOW};
    ItemStatReporter<Callback> statReporter_;
};

using AsyncPercentStatReporter = PercentStatReporter<AsyncCallback>;

//=====================================================================================================================

template <class Function, class Callback> inline //return ignored error message if available
std::wstring tryReportingError(Function cmd /*throw FileError*/, Callback& cb /*throw X*/)
{
    for (size_t retryNumber = 0;; ++retryNumber)
        try
        {
            cmd(); //throw FileError
            return std::wstring();
        }
        catch (zen::FileError& e)
        {
            assert(!e.toString().empty());
            switch (cb.reportError({e.toString(), std::chrono::steady_clock::now(), retryNumber})) //throw X
            {
                case PhaseCallback::ignore:
                    return e.toString();
                case PhaseCallback::retry:
                    break; //continue with loop
            }
        }
}

//=====================================================================================================================
struct ParallelContext
{
    const AbstractPath& itemPath;
    AsyncCallback& acb;
};
using ParallelWorkItem = std::function<void(ParallelContext& ctx)> /*throw ThreadStopRequest*/;


namespace
{
void massParallelExecute(const std::vector<std::pair<AbstractPath, ParallelWorkItem>>& workload,
                         const Zstring& threadGroupName,
                         PhaseCallback& callback /*throw X*/) //throw X
{
    using namespace zen;

    std::map<AfsDevice, std::vector<const std::pair<AbstractPath, ParallelWorkItem>*>> perDeviceWorkload;
    for (const auto& item : workload)
        perDeviceWorkload[item.first.afsDevice].push_back(&item);

    if (perDeviceWorkload.empty())
        return; //[!] otherwise AsyncCallback::notifyAllDone() is never called!

    AsyncCallback acb;                                               //manage life time: enclose ThreadGroup's!!!
    std::atomic<size_t> activeDeviceCount(perDeviceWorkload.size()); //

    //---------------------------------------------------------------------------------------------------------
    std::map<AfsDevice, ThreadGroup<std::function<void()>>> deviceThreadGroups; //worker threads live here...
    //---------------------------------------------------------------------------------------------------------

    for (const auto& [afsDevice, wl] : perDeviceWorkload)
    {
        const size_t statusPrio = deviceThreadGroups.size();

        auto& threadGroup = deviceThreadGroups.emplace(afsDevice, ThreadGroup<std::function<void()>>(
                                                           1,
                                                           threadGroupName + Zstr(' ') + utfTo<Zstring>(AFS::getDisplayPath(AbstractPath(afsDevice, AfsPath()))))).first->second;

        for (const std::pair<AbstractPath, ParallelWorkItem>* item : wl)
            threadGroup.run([&acb, statusPrio, &itemPath = item->first, &task = item->second]
        {
            acb.notifyTaskBegin(statusPrio);
            ZEN_ON_SCOPE_EXIT(acb.notifyTaskEnd());

            ParallelContext pctx{itemPath, acb};
            task(pctx); //throw ThreadStopRequest
        });

        threadGroup.notifyWhenDone([&acb, &activeDeviceCount] /*noexcept! runs on worker thread!*/
        {
            if (--activeDeviceCount == 0)
                acb.notifyAllDone(); //noexcept
        });
    }

    acb.waitUntilDone(UI_UPDATE_INTERVAL / 2 /*every ~50 ms*/, callback); //throw X
}
}

//=====================================================================================================================

template <class Function> inline
auto parallelScope(Function&& fun, std::mutex& singleThread) //throw X
{
    singleThread.unlock();
    ZEN_ON_SCOPE_EXIT(singleThread.lock());

    return fun(); //throw X
}
}

#endif //STATUS_HANDLER_IMPL_H_07682758976
