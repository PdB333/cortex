#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif
#include "tls.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace cortex::remote {
namespace {
using Clock=std::chrono::steady_clock;
#ifdef _WIN32
using Socket=SOCKET;constexpr Socket kInvalid=INVALID_SOCKET;
void Close(Socket s){closesocket(s);}
bool Again(){int e=WSAGetLastError();return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS;}
struct Winsock{Winsock(){WSADATA d;Require(WSAStartup(MAKEWORD(2,2),&d)==0,"winsock_failed");}~Winsock(){WSACleanup();}};
void Startup(){static Winsock init;(void)init;}
#else
using Socket=int;constexpr Socket kInvalid=-1;
void Close(Socket s){close(s);}
bool Again(){return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINPROGRESS||errno==EINTR;}
void Startup(){}
#endif
struct OwnedSocket {
    Socket value=kInvalid;
    ~OwnedSocket(){if(value!=kInvalid)Close(value);}
    Socket Release(){Socket s=value;value=kInvalid;return s;}
};
void Nonblocking(Socket s){
#ifdef _WIN32
    u_long value=1;Require(ioctlsocket(s,FIONBIO,&value)==0,"socket_nonblocking_failed");
#else
    Require(s>=0&&s<FD_SETSIZE,"socket_descriptor_limit");int flags=fcntl(s,F_GETFL,0);Require(flags>=0&&fcntl(s,F_SETFL,flags|O_NONBLOCK)==0,"socket_nonblocking_failed");
#endif
}
sockaddr_in Address(const Endpoint& endpoint){sockaddr_in a{};
#ifdef __FreeBSD__
    a.sin_len=sizeof(a);
#endif
    a.sin_family=AF_INET;a.sin_port=htons(endpoint.port);Require(inet_pton(AF_INET,endpoint.host.c_str(),&a.sin_addr)==1,"invalid_endpoint");return a;
}
bool Poll(Socket s,bool writing,Clock::time_point deadline,bool allowTimeout=false){
    for(;;){
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(remaining<=0){if(allowTimeout)return false;throw std::runtime_error("timeout");}
        fd_set ready,failed;FD_ZERO(&ready);FD_ZERO(&failed);FD_SET(s,&ready);FD_SET(s,&failed);
        timeval tv{};tv.tv_sec=static_cast<long>(remaining/1000);tv.tv_usec=static_cast<long>((remaining%1000)*1000);
#ifdef _WIN32
        int nfds=0;
#else
        int nfds=s+1;
#endif
        int n=select(nfds,writing?nullptr:&ready,writing?&ready:nullptr,&failed,&tv);
#ifndef _WIN32
        if(n<0&&errno==EINTR)continue;
#endif
        if(n==0&&allowTimeout)return false;
        Require(n>0,n==0?"timeout":"socket_select_failed");Require(!FD_ISSET(s,&failed),"socket_failed");return true;
    }
}
void Ok(int result,const char* name){if(result!=0)throw std::runtime_error(std::string(name)+":"+std::to_string(result));}
std::string Pem(const std::string& path){auto data=ReadFile(path,65536);Require(!data.empty(),"empty_credential_file");data.push_back('\0');return data;}
} // namespace

struct TlsContext::Impl {
    mbedtls_ssl_config config;mbedtls_x509_crt ca,cert;mbedtls_pk_context key;
    mbedtls_ctr_drbg_context random;mbedtls_entropy_context entropy;
    Impl(){mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&ca);mbedtls_x509_crt_init(&cert);mbedtls_pk_init(&key);mbedtls_ctr_drbg_init(&random);mbedtls_entropy_init(&entropy);}
    ~Impl(){mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&cert);mbedtls_x509_crt_free(&ca);mbedtls_pk_free(&key);mbedtls_ctr_drbg_free(&random);mbedtls_entropy_free(&entropy);}
};
TlsContext::TlsContext(const TlsFiles& files,bool server):impl_(new Impl){
    auto& c=*impl_;Require(psa_crypto_init()==PSA_SUCCESS,"crypto_init_failed");
    const char label[]="Cortex Remote v1";Ok(mbedtls_ctr_drbg_seed(&c.random,mbedtls_entropy_func,&c.entropy,reinterpret_cast<const unsigned char*>(label),sizeof(label)-1),"entropy_failed");
    auto ca=Pem(files.ca),cert=Pem(files.certificate),key=Pem(files.privateKey);
    Ok(mbedtls_x509_crt_parse(&c.ca,reinterpret_cast<const unsigned char*>(ca.data()),ca.size()),"ca_parse_failed");
    Ok(mbedtls_x509_crt_parse(&c.cert,reinterpret_cast<const unsigned char*>(cert.data()),cert.size()),"certificate_parse_failed");
    int keyResult=mbedtls_pk_parse_key(&c.key,reinterpret_cast<const unsigned char*>(key.data()),key.size(),nullptr,0,mbedtls_ctr_drbg_random,&c.random);
    mbedtls_platform_zeroize(key.data(),key.size());Ok(keyResult,"private_key_parse_failed");
    Ok(mbedtls_ssl_config_defaults(&c.config,server?MBEDTLS_SSL_IS_SERVER:MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT),"tls_config_failed");
    // Restrict negotiation to authenticated ephemeral key exchange and AEAD.
    static const int suites[]={MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,0};
    mbedtls_ssl_conf_min_tls_version(&c.config,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&c.config,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&c.config,suites);
    mbedtls_ssl_conf_authmode(&c.config,MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c.config,&c.ca,nullptr);
    mbedtls_ssl_conf_rng(&c.config,mbedtls_ctr_drbg_random,&c.random);
    Ok(mbedtls_ssl_conf_own_cert(&c.config,&c.cert,&c.key),"tls_own_cert_failed");
#if defined(MBEDTLS_SSL_RENEGOTIATION)
    mbedtls_ssl_conf_renegotiation(&c.config,MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    mbedtls_ssl_conf_session_tickets(&c.config,MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
}
TlsContext::~TlsContext()=default;
std::string TlsContext::RandomId(){Bytes data(16);Ok(mbedtls_ctr_drbg_random(&impl_->random,data.data(),data.size()),"random_failed");return ByteHex(data);}

struct Channel::Impl {
    OwnedSocket socket;mbedtls_ssl_context ssl;Clock::time_point deadline;
    Impl(){mbedtls_ssl_init(&ssl);}
    ~Impl(){mbedtls_ssl_free(&ssl);}
    static int Send(void* context,const unsigned char* data,size_t length){
        auto& self=*static_cast<Impl*>(context);
#ifdef MSG_NOSIGNAL
        constexpr int flags=MSG_NOSIGNAL;
#else
        constexpr int flags=0;
#endif
        int n=static_cast<int>(send(self.socket.value,reinterpret_cast<const char*>(data),static_cast<int>(std::min<size_t>(length,16384)),flags));
        if(n<0&&Again())return MBEDTLS_ERR_SSL_WANT_WRITE;
        return n>0?n:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    static int Receive(void* context,unsigned char* data,size_t length){
        auto& self=*static_cast<Impl*>(context);
        int n=static_cast<int>(recv(self.socket.value,reinterpret_cast<char*>(data),static_cast<int>(std::min<size_t>(length,16384)),0));
        if(n<0&&Again())return MBEDTLS_ERR_SSL_WANT_READ;
        return n>=0?n:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    bool Retry(int code){
        Require(Clock::now()<deadline,"timeout");
        if(code==MBEDTLS_ERR_SSL_WANT_READ){Poll(socket.value,false,deadline);return true;}
        if(code==MBEDTLS_ERR_SSL_WANT_WRITE){Poll(socket.value,true,deadline);return true;}
        return false;
    }
    void Write(const uint8_t* data,size_t size){
        for(size_t at=0;at<size;){Require(Clock::now()<deadline,"timeout");int n=mbedtls_ssl_write(&ssl,data+at,size-at);if(n>0){at+=static_cast<size_t>(n);continue;}if(Retry(n))continue;throw std::runtime_error("tls_write_failed");}
    }
    void Read(uint8_t* data,size_t size){
        for(size_t at=0;at<size;){Require(Clock::now()<deadline,"timeout");int n=mbedtls_ssl_read(&ssl,data+at,size-at);if(n>0){at+=static_cast<size_t>(n);continue;}if(Retry(n))continue;throw std::runtime_error(n==0||n==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY?"truncated_response":"tls_read_failed");}
    }
};
Channel::Channel(TlsContext& context,uintptr_t socket,int timeoutMs,const std::string& peerName):impl_(new Impl){
    impl_->socket.value=static_cast<Socket>(socket);impl_->deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
    Nonblocking(impl_->socket.value);Ok(mbedtls_ssl_setup(&impl_->ssl,&context.impl_->config),"tls_setup_failed");
    if(!peerName.empty())Ok(mbedtls_ssl_set_hostname(&impl_->ssl,peerName.c_str()),"peer_name_invalid");
    mbedtls_ssl_set_bio(&impl_->ssl,impl_.get(),Impl::Send,Impl::Receive,nullptr);
}
Channel::~Channel()=default;
std::unique_ptr<Channel> Channel::Connect(TlsContext& context,const Endpoint& endpoint,const std::string& peerName,int timeoutMs){
    Startup();OwnedSocket sock;sock.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Require(sock.value!=kInvalid,"socket_create_failed");Nonblocking(sock.value);
    auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);auto address=Address(endpoint);
    if(connect(sock.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){
        Require(Again(),"connect_failed");Poll(sock.value,true,deadline);int error=0;
#ifdef _WIN32
        int length=sizeof(error);Require(getsockopt(sock.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)==0&&error==0,"connect_failed");
#else
        socklen_t length=sizeof(error);Require(getsockopt(sock.value,SOL_SOCKET,SO_ERROR,&error,&length)==0&&error==0,"connect_failed");
#endif
    }
    // Ownership is transferred only after storage for Channel is allocated.
    auto channel=std::unique_ptr<Channel>(new Channel(context,static_cast<uintptr_t>(sock.Release()),timeoutMs,peerName));
    channel->impl_->deadline=deadline;channel->Handshake();return channel;
}
void Channel::Handshake(){for(;;){Require(Clock::now()<impl_->deadline,"timeout");int result=mbedtls_ssl_handshake(&impl_->ssl);if(result==0)break;if(impl_->Retry(result))continue;throw std::runtime_error("tls_handshake_failed");}Require(mbedtls_ssl_get_verify_result(&impl_->ssl)==0&&mbedtls_ssl_get_peer_cert(&impl_->ssl)!=nullptr,"tls_peer_not_verified");}
void Channel::Send(FrameKind kind,uint32_t id,const json& body){auto text=DumpJson(body);auto h=EncodeHeader(kind,id,static_cast<uint32_t>(text.size()));impl_->Write(h.data(),h.size());impl_->Write(reinterpret_cast<const uint8_t*>(text.data()),text.size());}
json Channel::Receive(FrameKind kind,uint32_t id){std::array<uint8_t,16> bytes{};impl_->Read(bytes.data(),bytes.size());auto h=DecodeHeader(bytes);Require(h.kind==kind&&h.id==id,"unexpected_frame");std::string body(h.size,'\0');impl_->Read(reinterpret_cast<uint8_t*>(body.data()),body.size());return ParseJson(body);}
std::string Channel::PeerFingerprint()const{auto* crt=mbedtls_ssl_get_peer_cert(&impl_->ssl);Require(crt!=nullptr,"tls_peer_not_verified");Bytes hash(32);Ok(mbedtls_sha256(crt->raw.p,crt->raw.len,hash.data(),0),"fingerprint_failed");return ByteHex(hash);}
struct Listener::Impl{OwnedSocket socket;};
Listener::Listener(const Endpoint& endpoint):impl_(new Impl){
    Startup();impl_->socket.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Require(impl_->socket.value!=kInvalid,"listen_socket_failed");
#ifdef _WIN32
    BOOL exclusive=TRUE;Require(setsockopt(impl_->socket.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0,"exclusive_listen_failed");
#else
    int reuse=1;(void)setsockopt(impl_->socket.value,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
#endif
    Nonblocking(impl_->socket.value);auto address=Address(endpoint);
    Require(bind(impl_->socket.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"bind_failed");Require(listen(impl_->socket.value,16)==0,"listen_failed");
}
Listener::~Listener()=default;
std::unique_ptr<Channel> Listener::Accept(TlsContext& context,int timeoutMs){
    if(!Poll(impl_->socket.value,false,Clock::now()+std::chrono::milliseconds(200),true))return {};
    OwnedSocket socket;socket.value=accept(impl_->socket.value,nullptr,nullptr);
    if(socket.value==kInvalid&&Again())return {};Require(socket.value!=kInvalid,"accept_failed");
    return std::unique_ptr<Channel>(new Channel(context,static_cast<uintptr_t>(socket.Release()),timeoutMs,""));
}
} // namespace cortex::remote
