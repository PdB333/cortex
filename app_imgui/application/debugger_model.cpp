#include "debugger_model.h"

#include "veh_debug_provider.h"
#include "windows_debug_provider.h"

#include <algorithm>

namespace cortex::application {

DebuggerModel::DebuggerModel(target::SessionManager& sessions,
                             services::PayloadClient& payload,
                             SettingsStore& settings)
    : sessions_(sessions), payload_(payload), settings_(settings) {}

DebuggerModel::~DebuggerModel() {
    if (provider_) provider_->Detach();
}

void DebuggerModel::Reset() {
    if (provider_) provider_->Detach();
    provider_.reset();
    providerBackend_.clear();
    targetId_.clear();
    threads_.clear();
    snapshot_ = {};
    currentThreadId_ = 0;
    breakpoints_.clear();
    pausedThreads_.clear();
}

bool DebuggerModel::EnsureProvider(bool attach, std::string* error) {
    if (error) error->clear();
    const auto session = sessions_.Active();
    if (!session) {
        if (error) *error = "no_active_session";
        return false;
    }

    const std::string desiredBackend =
        settings_.Values().debuggerBackend == "veh" ? "veh" : "windows";
    const std::string desiredTarget = session->Target().id;

    if (!provider_ || providerBackend_ != desiredBackend || targetId_ != desiredTarget) {
        if (provider_) provider_->Detach();
        provider_.reset();
        if (desiredBackend == "veh")
            provider_ = std::make_unique<VehDebugProvider>(sessions_, payload_);
        else
            provider_ = std::make_unique<WindowsDebugProvider>(sessions_, &payload_);
        providerBackend_ = desiredBackend;
        targetId_ = desiredTarget;
        threads_.clear();
        snapshot_ = {};
        currentThreadId_ = 0;
        breakpoints_.clear();
        pausedThreads_.clear();
    }

    if (!attach || provider_->Ready()) return true;
    if (!provider_->Attach(error)) return false;
    return true;
}

bool DebuggerModel::EnsureAttached(std::string* error) {
    return EnsureProvider(true, error);
}

bool DebuggerModel::Ready() {
    std::string ignored;
    return EnsureProvider(false, &ignored) && provider_ && provider_->Ready();
}

const std::string& DebuggerModel::Backend() {
    std::string ignored;
    EnsureProvider(false, &ignored);
    return providerBackend_;
}

bool DebuggerModel::RefreshThreads(std::string* error) {
    if (!EnsureProvider(false, error)) return false;
    threads_ = provider_->Threads(error);
    if (error && !error->empty()) return false;

    if (threads_.empty()) {
        snapshot_ = {};
        currentThreadId_ = 0;
        return true;
    }
    if (std::find(threads_.begin(), threads_.end(), currentThreadId_) == threads_.end())
        return SelectThread(threads_.front(), error);
    return true;
}

void DebuggerModel::ApplySnapshot(target::ThreadRegisterSnapshot snapshot) {
    currentThreadId_ = snapshot.threadId;
    snapshot_ = std::move(snapshot);
}

bool DebuggerModel::SelectThread(uint64_t threadId, std::string* error) {
    if (!EnsureProvider(false, error)) return false;
    target::ThreadRegisterSnapshot snapshot;
    if (!provider_->GetRegisters(threadId, snapshot, error)) {
        currentThreadId_ = threadId;
        snapshot_ = {};
        return false;
    }
    ApplySnapshot(std::move(snapshot));
    return true;
}

bool DebuggerModel::RefreshRuntime(std::string* error) {
    if (!EnsureProvider(true, error)) return false;
    breakpoints_ = provider_->Breakpoints(error);
    if (error && !error->empty()) return false;
    pausedThreads_ = provider_->PausedThreads(error);
    if (error && !error->empty()) return false;
    if (!pausedThreads_.empty()) ApplySnapshot(pausedThreads_.front().registers);
    return true;
}

bool DebuggerModel::AddBreakpoint(const std::string& address,
                                  const std::string& kind,
                                  int size,
                                  bool pauseOnHit,
                                  bool processGlobal,
                                  uint64_t threadId,
                                  std::string* error) {
    if (!EnsureProvider(true, error)) return false;
    const int id = provider_->SetBreakpoint(address, kind, size, pauseOnHit,
                                            processGlobal, threadId, error);
    if (id < 0) return false;
    return RefreshRuntime(error);
}

bool DebuggerModel::RemoveBreakpoint(int id, std::string* error) {
    if (!EnsureProvider(true, error) || id < 0) return false;
    if (!provider_->RemoveBreakpoint(id, error)) return false;
    return RefreshRuntime(error);
}

bool DebuggerModel::Pause(std::string* error) {
    if (!EnsureProvider(true, error) || currentThreadId_ == 0) {
        if (error && error->empty()) *error = "no_thread_selected";
        return false;
    }
    target::ThreadRegisterSnapshot snapshot;
    if (!provider_->Pause(currentThreadId_, snapshot, error)) return false;
    ApplySnapshot(std::move(snapshot));
    return RefreshRuntime(error);
}

bool DebuggerModel::Resume(std::string* error) {
    if (!EnsureProvider(true, error) || currentThreadId_ == 0) {
        if (error && error->empty()) *error = "no_thread_selected";
        return false;
    }
    if (!provider_->Resume(currentThreadId_, error)) return false;
    RefreshThreads(nullptr);
    return RefreshRuntime(error);
}

bool DebuggerModel::Step(uint32_t timeoutMs, std::string* error) {
    if (!EnsureProvider(true, error) || currentThreadId_ == 0) {
        if (error && error->empty()) *error = "no_thread_selected";
        return false;
    }
    target::ThreadRegisterSnapshot snapshot;
    timeoutMs = std::clamp<uint32_t>(timeoutMs, 100, 120000);
    if (!provider_->Step(currentThreadId_, timeoutMs, snapshot, error)) return false;
    ApplySnapshot(std::move(snapshot));
    return RefreshRuntime(error);
}

bool DebuggerModel::StepOver(uint32_t timeoutMs, std::string* error) {
    if (!EnsureProvider(true, error) || currentThreadId_ == 0) {
        if (error && error->empty()) *error = "no_thread_selected";
        return false;
    }
    target::ThreadRegisterSnapshot snapshot;
    timeoutMs = std::clamp<uint32_t>(timeoutMs, 100, 120000);
    if (!provider_->StepOver(currentThreadId_, timeoutMs, snapshot, error)) return false;
    ApplySnapshot(std::move(snapshot));
    return RefreshRuntime(error);
}

} // namespace cortex::application
