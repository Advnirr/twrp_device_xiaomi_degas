/*
 * se_omapi_recovery — minimal native OMAPI service for the degas recovery.
 *
 * Why: the stock Weaver HAL (android.hardware.weaver-service.nxp ->
 * ese_weaver.nxp.so -> libjc_keymint_transport.nxp.so) talks to the eSE only
 * through OMAPI, android.se.omapi.ISecureElementService/default. In Android
 * that service is the Java SecureElement app (com.android.se), which does not
 * exist in recovery. Without Weaver the synthetic-password (CE) decryption
 * cannot complete.
 *
 * What: implements the frozen android.se.omapi V1 interfaces (Service, Reader,
 * Session, Channel) for a single reader "eSE1" and forwards everything to the
 * stock eSE HAL android.hardware.secure_element.ISecureElement/eSE1
 * (vendor.xiaomi.hardware.secure_element-service, MiTEE backed).
 * Channel/APDU handling follows AOSP SecureElementService (Channel.java,
 * Terminal.java): channel number in CLA, 6Cxx re-send, 61xx GET RESPONSE.
 * No access control (ARA) — recovery only, single trusted client (Weaver).
 *
 * Copyright (C) 2026. Licensed under the Apache License, Version 2.0.
 */

#include <aidl/android/hardware/secure_element/BnSecureElementCallback.h>
#include <aidl/android/hardware/secure_element/ISecureElement.h>
#include <aidl/android/se/omapi/BnSecureElementChannel.h>
#include <aidl/android/se/omapi/BnSecureElementReader.h>
#include <aidl/android/se/omapi/BnSecureElementService.h>
#include <aidl/android/se/omapi/BnSecureElementSession.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using aidl::android::hardware::secure_element::BnSecureElementCallback;
using aidl::android::hardware::secure_element::ISecureElement;
using aidl::android::hardware::secure_element::LogicalChannelResponse;
using aidl::android::se::omapi::BnSecureElementChannel;
using aidl::android::se::omapi::BnSecureElementReader;
using aidl::android::se::omapi::BnSecureElementService;
using aidl::android::se::omapi::BnSecureElementSession;
using aidl::android::se::omapi::ISecureElementChannel;
using aidl::android::se::omapi::ISecureElementListener;
using aidl::android::se::omapi::ISecureElementReader;
using aidl::android::se::omapi::ISecureElementSession;
using ndk::ScopedAStatus;

namespace {

constexpr const char* kReaderName = "eSE1";
constexpr const char* kHalInstance = "android.hardware.secure_element.ISecureElement/eSE1";
constexpr const char* kOmapiInstance = "android.se.omapi.ISecureElementService/default";

// OMAPI service-specific errors (AOSP SecureElementService).
constexpr int32_t kIoError = 1;
constexpr int32_t kNoSuchElement = 2;

std::string hex(const std::vector<uint8_t>& v, size_t max = 16) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < v.size() && i < max; i++) {
        s += d[v[i] >> 4];
        s += d[v[i] & 0xf];
    }
    if (v.size() > max) s += "..";
    return s;
}

// Map a HAL failure to an OMAPI error.
ScopedAStatus toOmapiError(const ScopedAStatus& st, const char* what) {
    LOG(ERROR) << what << " failed: " << st.getDescription();
    if (st.getExceptionCode() == EX_SERVICE_SPECIFIC &&
        st.getServiceSpecificError() == ISecureElement::NO_SUCH_ELEMENT_ERROR) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(kNoSuchElement, what);
    }
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(kIoError, what);
}

// ── Terminal: the eSE HAL connection ────────────────────────────────────────
class Terminal {
  public:
    class Callback : public BnSecureElementCallback {
      public:
        explicit Callback(Terminal* t) : t_(t) {}
        ScopedAStatus onStateChange(bool connected, const std::string& reason) override {
            LOG(INFO) << "eSE HAL state: connected=" << connected << " (" << reason << ")";
            t_->setConnected(connected);
            return ScopedAStatus::ok();
        }

      private:
        Terminal* t_;
    };

    std::shared_ptr<ISecureElement> hal() {
        std::unique_lock<std::mutex> lk(m_);
        if (!hal_) {
            LOG(INFO) << "waiting for " << kHalInstance;
            ndk::SpAIBinder b(AServiceManager_waitForService(kHalInstance));
            hal_ = ISecureElement::fromBinder(b);
            if (!hal_) {
                LOG(ERROR) << "cannot get " << kHalInstance;
                return nullptr;
            }
            cb_ = ndk::SharedRefBase::make<Callback>(this);
            lk.unlock();
            auto st = hal_->init(cb_);
            lk.lock();
            if (!st.isOk()) {
                LOG(ERROR) << "eSE HAL init failed: " << st.getDescription();
                hal_ = nullptr;
                return nullptr;
            }
            cv_.wait_for(lk, std::chrono::seconds(10), [this] { return connected_; });
            LOG(INFO) << "eSE HAL ready, connected=" << connected_;
        }
        return hal_;
    }

    void setConnected(bool c) {
        std::lock_guard<std::mutex> lk(m_);
        connected_ = c;
        cv_.notify_all();
    }

    // Terminal.transmit(): 6Cxx -> resend with Le, 61xx -> GET RESPONSE chain.
    ScopedAStatus transmit(std::vector<uint8_t> cmd, std::vector<uint8_t>* out) {
        auto h = hal();
        if (!h) return ScopedAStatus::fromServiceSpecificError(kIoError);
        std::lock_guard<std::mutex> io(io_);
        std::vector<uint8_t> rsp;
        auto st = h->transmit(cmd, &rsp);
        if (!st.isOk()) return toOmapiError(st, "transmit");
        if (rsp.size() < 2) return ScopedAStatus::fromServiceSpecificError(kIoError);

        uint8_t sw1 = rsp[rsp.size() - 2];
        if (sw1 == 0x6C && cmd.size() > 4) {
            cmd.back() = rsp.back();
            st = h->transmit(cmd, &rsp);
            if (!st.isOk()) return toOmapiError(st, "transmit(6C)");
        } else if (sw1 == 0x61) {
            std::vector<uint8_t> data(rsp.begin(), rsp.end() - 2);
            std::vector<uint8_t> getResponse = {cmd[0], 0xC0, 0x00, 0x00, 0x00};
            for (int i = 0; i < 64; i++) {
                getResponse[4] = rsp.back();
                st = h->transmit(getResponse, &rsp);
                if (!st.isOk()) return toOmapiError(st, "GET RESPONSE");
                if (rsp.size() >= 2 && rsp[rsp.size() - 2] == 0x61) {
                    data.insert(data.end(), rsp.begin(), rsp.end() - 2);
                    continue;
                }
                data.insert(data.end(), rsp.begin(), rsp.end());
                break;
            }
            rsp = std::move(data);
        }
        *out = std::move(rsp);
        return ScopedAStatus::ok();
    }

  private:
    std::mutex m_, io_;
    std::condition_variable cv_;
    bool connected_ = false;
    std::shared_ptr<ISecureElement> hal_;
    std::shared_ptr<Callback> cb_;
};

Terminal gTerminal;

// AOSP Channel.setChannelToClassByte()
uint8_t channelToCla(uint8_t cla, int channel) {
    if (channel < 4) return (cla & 0xBC) | channel;
    if (channel < 20) {
        bool sm = (cla & 0x0C) != 0;
        cla = (cla & 0xB0) | 0x40 | (channel - 4);
        if (sm) cla |= 0x20;
        return cla;
    }
    return cla;
}

// ── Channel ────────────────────────────────────────────────────────────────
class Channel : public BnSecureElementChannel {
  public:
    Channel(int number, std::vector<uint8_t> selectResponse, bool basic)
        : number_(number), select_(std::move(selectResponse)), basic_(basic) {}
    ~Channel() override { doClose(); }

    ScopedAStatus close() override {
        doClose();
        return ScopedAStatus::ok();
    }
    ScopedAStatus isClosed(bool* r) override {
        *r = closed_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus isBasicChannel(bool* r) override {
        *r = basic_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getSelectResponse(std::vector<uint8_t>* r) override {
        *r = select_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus transmit(const std::vector<uint8_t>& command, std::vector<uint8_t>* r) override {
        if (closed_) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        if (command.size() < 4) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::vector<uint8_t> cmd = command;
        cmd[0] = channelToCla(cmd[0], number_);
        return gTerminal.transmit(std::move(cmd), r);
    }
    ScopedAStatus selectNext(bool* r) override {
        *r = false;  // not needed by Weaver
        return ScopedAStatus::ok();
    }

  private:
    void doClose() {
        if (closed_) return;
        closed_ = true;
        if (auto h = gTerminal.hal()) {
            auto st = h->closeChannel(static_cast<int8_t>(number_));
            if (!st.isOk()) LOG(WARNING) << "closeChannel(" << number_ << "): " << st.getDescription();
        }
        LOG(INFO) << "channel " << number_ << " closed";
    }

    int number_;
    std::vector<uint8_t> select_;
    bool basic_;
    bool closed_ = false;
};

// ── Session ────────────────────────────────────────────────────────────────
class Session : public BnSecureElementSession {
  public:
    ScopedAStatus getAtr(std::vector<uint8_t>* r) override {
        auto h = gTerminal.hal();
        if (!h) return ScopedAStatus::fromServiceSpecificError(kIoError);
        auto st = h->getAtr(r);
        return st.isOk() ? ScopedAStatus::ok() : toOmapiError(st, "getAtr");
    }
    ScopedAStatus close() override {
        closeChannels();
        closed_ = true;
        return ScopedAStatus::ok();
    }
    ScopedAStatus closeChannels() override {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& w : channels_)
            if (auto c = w.lock()) c->close();
        channels_.clear();
        return ScopedAStatus::ok();
    }
    ScopedAStatus isClosed(bool* r) override {
        *r = closed_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus openBasicChannel(const std::vector<uint8_t>& aid, int8_t p2,
                                   const std::shared_ptr<ISecureElementListener>&,
                                   std::shared_ptr<ISecureElementChannel>* r) override {
        auto h = gTerminal.hal();
        if (!h) return ScopedAStatus::fromServiceSpecificError(kIoError);
        std::vector<uint8_t> sel;
        auto st = h->openBasicChannel(aid, p2, &sel);
        if (!st.isOk()) return toOmapiError(st, "openBasicChannel");
        LOG(INFO) << "basic channel, aid=" << hex(aid) << " select=" << hex(sel);
        auto c = ndk::SharedRefBase::make<Channel>(0, sel, true);
        track(c);
        *r = c;
        return ScopedAStatus::ok();
    }
    ScopedAStatus openLogicalChannel(const std::vector<uint8_t>& aid, int8_t p2,
                                     const std::shared_ptr<ISecureElementListener>&,
                                     std::shared_ptr<ISecureElementChannel>* r) override {
        auto h = gTerminal.hal();
        if (!h) return ScopedAStatus::fromServiceSpecificError(kIoError);
        LogicalChannelResponse resp;
        auto st = h->openLogicalChannel(aid, p2, &resp);
        if (!st.isOk()) {
            // OMAPI returns null when no channel is available
            if (st.getExceptionCode() == EX_SERVICE_SPECIFIC &&
                st.getServiceSpecificError() == ISecureElement::CHANNEL_NOT_AVAILABLE) {
                LOG(WARNING) << "openLogicalChannel: no channel available";
                *r = nullptr;
                return ScopedAStatus::ok();
            }
            return toOmapiError(st, "openLogicalChannel");
        }
        LOG(INFO) << "logical channel " << int(resp.channelNumber) << ", aid=" << hex(aid)
                  << " select=" << hex(resp.selectResponse);
        auto c = ndk::SharedRefBase::make<Channel>(resp.channelNumber, resp.selectResponse, false);
        track(c);
        *r = c;
        return ScopedAStatus::ok();
    }

  private:
    void track(const std::shared_ptr<Channel>& c) {
        std::lock_guard<std::mutex> lk(m_);
        channels_.push_back(c);
    }
    std::mutex m_;
    std::vector<std::weak_ptr<Channel>> channels_;
    bool closed_ = false;
};

// ── Reader ─────────────────────────────────────────────────────────────────
class Reader : public BnSecureElementReader {
  public:
    ScopedAStatus isSecureElementPresent(bool* r) override {
        auto h = gTerminal.hal();
        if (!h) {
            *r = false;
            return ScopedAStatus::ok();
        }
        auto st = h->isCardPresent(r);
        if (!st.isOk()) *r = false;
        return ScopedAStatus::ok();
    }
    ScopedAStatus openSession(std::shared_ptr<ISecureElementSession>* r) override {
        auto s = ndk::SharedRefBase::make<Session>();
        {
            std::lock_guard<std::mutex> lk(m_);
            sessions_.push_back(s);
        }
        *r = s;
        return ScopedAStatus::ok();
    }
    ScopedAStatus closeSessions() override {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& w : sessions_)
            if (auto s = w.lock()) s->close();
        sessions_.clear();
        return ScopedAStatus::ok();
    }
    ScopedAStatus reset(bool* r) override {
        auto h = gTerminal.hal();
        *r = h && h->reset().isOk();
        return ScopedAStatus::ok();
    }

  private:
    std::mutex m_;
    std::vector<std::weak_ptr<Session>> sessions_;
};

// ── Service ────────────────────────────────────────────────────────────────
class Service : public BnSecureElementService {
  public:
    ScopedAStatus getReaders(std::vector<std::string>* r) override {
        *r = {kReaderName};
        return ScopedAStatus::ok();
    }
    ScopedAStatus getReader(const std::string& name,
                            std::shared_ptr<ISecureElementReader>* r) override {
        if (name != kReaderName) {
            LOG(WARNING) << "unknown reader " << name;
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (!reader_) reader_ = ndk::SharedRefBase::make<Reader>();
        *r = reader_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus isNfcEventAllowed(const std::string&, const std::vector<uint8_t>&,
                                    const std::vector<std::string>& pkgs, int32_t,
                                    std::vector<bool>* r) override {
        r->assign(pkgs.size(), false);
        return ScopedAStatus::ok();
    }

  private:
    std::shared_ptr<Reader> reader_;
};

}  // namespace

int main(int, char** argv) {
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    auto service = ndk::SharedRefBase::make<Service>();
    binder_status_t st = AServiceManager_addService(service->asBinder().get(), kOmapiInstance);
    if (st != STATUS_OK) {
        LOG(FATAL) << "cannot register " << kOmapiInstance << ": " << st;
    }
    LOG(INFO) << "registered " << kOmapiInstance << " (reader " << kReaderName << " -> "
              << kHalInstance << ")";
    ABinderProcess_joinThreadPool();
    return 1;
}
