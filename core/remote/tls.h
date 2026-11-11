#pragma once
#include "protocol.h"
#include <memory>

namespace cortex::remote {
// TLS 1.2 ECDHE + AES-GCM, required peer certificates, operator-provided CA.
// No plaintext mode, default secret, certificate-verification bypass or retries.
class TlsContext {
public:
    TlsContext(const TlsFiles& files, bool server);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    std::string RandomId();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
    friend class Channel;
};
class Channel {
public:
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    static std::unique_ptr<Channel> Connect(TlsContext& context, const Endpoint& endpoint,
                                            const std::string& peerName, int timeoutMs);
    void Handshake();
    void Send(FrameKind kind, uint32_t id, const json& body);
    json Receive(FrameKind expectedKind, uint32_t expectedId);
    std::string PeerFingerprint() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
    Channel(TlsContext& context, uintptr_t socket, int timeoutMs, const std::string& peerName);
    friend class Listener;
};
class Listener {
public:
    explicit Listener(const Endpoint& endpoint);
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    // Bounded 200ms accept polling; nullptr means no client yet.
    std::unique_ptr<Channel> Accept(TlsContext& context, int timeoutMs);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace cortex::remote
