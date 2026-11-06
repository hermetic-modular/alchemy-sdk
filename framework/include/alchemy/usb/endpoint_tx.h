#pragma once
#include <array>
#include <atomic>
#include <cstring>
#include "alchemy/midi/queue.h"

namespace alchemy::usb {
// Submit is foreground-only. Pump/Complete/Reset are serialized by the USB IRQ guard.
template<size_t Capacity = 4, size_t MaxTransfer = 256> class EndpointTx {
    struct Transfer { std::array<uint8_t, MaxTransfer> bytes{}; uint16_t size = 0; };
  public:
    bool Submit(const uint8_t* bytes,size_t size,bool urgent = false) {
        if (!bytes || !size || size > MaxTransfer) return false;
        Transfer transfer;
        std::memcpy(transfer.bytes.data(),bytes,size); transfer.size = uint16_t(size);
        return urgent ? urgent_.Push(transfer) : ordinary_.Push(transfer);
    }
    template<class Start> bool Pump(Start&& start,bool batch = false) {
        if (busy_) return false;
        if (!have_urgent_) have_urgent_ = urgent_.Pop(pending_urgent_);
        if (!have_urgent_ && !have_ordinary_) {
            have_ordinary_ = ordinary_.Pop(pending_ordinary_);
            if (have_ordinary_ && batch) {
                Transfer next;
                while (ordinary_.Peek(next) && size_t(pending_ordinary_.size) + next.size <= MaxTransfer) {
                    ordinary_.Pop(next);
                    std::memcpy(pending_ordinary_.bytes.data()+pending_ordinary_.size,next.bytes.data(),next.size);
                    pending_ordinary_.size += next.size;
                }
            }
        }
        if (!have_urgent_ && !have_ordinary_) return false;
        transfer_ = have_urgent_ ? pending_urgent_ : pending_ordinary_;
        busy_ = true;
        if (!start(transfer_.bytes.data(),transfer_.size)) { busy_ = false; return false; }
        if (have_urgent_) have_urgent_ = false; else have_ordinary_ = false;
        return true;
    }
    void Complete() {
        if (busy_) { completed_.fetch_add(1,std::memory_order_relaxed); busy_ = false; }
    }
    // The driver retains the active buffer until completion or endpoint reset.
    void RetirePending() {
        cancelled_.fetch_add(uint32_t(ordinary_.Size()+urgent_.Size()+have_ordinary_+have_urgent_),std::memory_order_relaxed);
        ordinary_.Clear(); urgent_.Clear(); have_ordinary_ = have_urgent_ = false;
    }
    void Reset() {
        RetirePending();
        if (busy_) cancelled_.fetch_add(1,std::memory_order_relaxed);
        busy_ = false;
    }
    size_t Space() const { return (Capacity-ordinary_.Size())*MaxTransfer; }
    uint32_t Completed() const { return completed_.load(std::memory_order_relaxed); }
    uint32_t Cancelled() const { return cancelled_.load(std::memory_order_relaxed); }
  private:
    midi::Queue<Transfer,Capacity> ordinary_;
    midi::Queue<Transfer,8> urgent_;
    Transfer transfer_{},pending_ordinary_{},pending_urgent_{};
    std::atomic<uint32_t> completed_{0},cancelled_{0};
    bool busy_ = false,have_ordinary_ = false,have_urgent_ = false;
};
}
