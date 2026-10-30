// TLS for our own WebSocket on Windows (wss://): SChannel, the system's own – and the server's certificate
// checked with the system's store and rules (the chain, the time, the name), as WinHTTP does. Our socket, so Nagle's algorithm is off on it (ws_plain.cpp): direct mode's
// WebSocket (to browser viewers) sends a frame's last bytes at once, not after the server's
// acknowledgement. TLS 1.2: 1.3 brings messages after the handshake that SChannel hands back as
// "renegotiate" (the server has TLS 1.2 too).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define SECURITY_WIN32
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "websocket.h"

namespace ss {

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

bool rawSend(SOCKET s, const uint8_t* p, size_t n) {
  while (n) {
    int r = ::send(s, reinterpret_cast<const char*>(p), int(std::min<size_t>(n, 1 << 20)), 0);
    if (r <= 0) return false;
    p += r;
    n -= size_t(r);
  }
  return true;
}

class Schannel : public TlsChannel {
 public:
  ~Schannel() override {
    if (haveCtx_) DeleteSecurityContext(&ctx_);
    if (haveCred_) FreeCredentialsHandle(&cred_);
  }

  bool handshake(uintptr_t sock, const std::string& host) override {
    SOCKET s = SOCKET(sock);
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &cred_, nullptr) != SEC_E_OK) return false;
    haveCred_ = true;
    name_ = wide(host);
    // the first message (with the name: SNI, and what the certificate is checked against)
    SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &out};
    DWORD attrs = 0;
    SECURITY_STATUS st = InitializeSecurityContextW(&cred_, nullptr, &name_[0], kFlags, 0, 0, nullptr, 0, &ctx_, &outDesc, &attrs, nullptr);
    if (st != SEC_I_CONTINUE_NEEDED) return false;
    haveCtx_ = true;
    if (!sendToken(s, out)) return false;
    for (;;) {
      if (in_.empty() || st == SEC_E_INCOMPLETE_MESSAGE) {
        if (!readMore(s)) return false;
      }
      SecBuffer inb[2] = {{ULONG(in_.size()), SECBUFFER_TOKEN, in_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc inDesc{SECBUFFER_VERSION, 2, inb};
      SecBuffer ob{0, SECBUFFER_TOKEN, nullptr};
      SecBufferDesc obDesc{SECBUFFER_VERSION, 1, &ob};
      st = InitializeSecurityContextW(&cred_, &ctx_, &name_[0], kFlags, 0, 0, &inDesc, 0, nullptr, &obDesc, &attrs, nullptr);
      if (st == SEC_E_INCOMPLETE_MESSAGE) { // (what came is kept: more is read to it)
        if (ob.pvBuffer) FreeContextBuffer(ob.pvBuffer);
        continue;
      }
      if (ob.pvBuffer && !sendToken(s, ob)) return false;
      if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) return false; // a certificate not trusted, another name, …
      // what came after the handshake's messages (already the server's first data, maybe)
      if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) in_.erase(in_.begin(), in_.end() - inb[1].cbBuffer);
      else in_.clear();
      if (st == SEC_E_OK) break;
    }
    return trusted() && QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) == SEC_E_OK;
  }

  bool send(uintptr_t sock, const uint8_t* p, size_t n) override {
    std::vector<uint8_t> buf;
    while (n) {
      size_t chunk = std::min<size_t>(n, sizes_.cbMaximumMessage);
      buf.resize(sizes_.cbHeader + chunk + sizes_.cbTrailer);
      std::memcpy(buf.data() + sizes_.cbHeader, p, chunk);
      SecBuffer b[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()},
                        {ULONG(chunk), SECBUFFER_DATA, buf.data() + sizes_.cbHeader},
                        {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + sizes_.cbHeader + chunk},
                        {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc d{SECBUFFER_VERSION, 4, b};
      {
        std::lock_guard<std::mutex> l(mu_); // (the reader decrypts with the same context)
        if (EncryptMessage(&ctx_, 0, &d, 0) != SEC_E_OK) return false;
      }
      if (!rawSend(SOCKET(sock), buf.data(), size_t(b[0].cbBuffer) + b[1].cbBuffer + b[2].cbBuffer)) return false;
      p += chunk;
      n -= chunk;
    }
    return true;
  }

  int recv(uintptr_t sock, uint8_t* p, size_t n) override {
    while (plainAt_ >= plain_.size()) {
      plain_.clear();
      plainAt_ = 0;
      if (!in_.empty()) {
        SecBuffer b[4] = {{ULONG(in_.size()), SECBUFFER_DATA, in_.data()}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS st;
        {
          std::lock_guard<std::mutex> l(mu_);
          st = DecryptMessage(&ctx_, &d, 0, nullptr);
        }
        if (st == SEC_E_OK || st == SEC_I_CONTEXT_EXPIRED || st == SEC_I_RENEGOTIATE) {
          std::vector<uint8_t> extra;
          for (const SecBuffer& x : b) {
            const uint8_t* at = static_cast<const uint8_t*>(x.pvBuffer);
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) plain_.insert(plain_.end(), at, at + x.cbBuffer);
            if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) extra.assign(at, at + x.cbBuffer);
          }
          in_ = std::move(extra);
          if (st == SEC_I_CONTEXT_EXPIRED) return 0; // the server closed (close_notify)
          if (st == SEC_I_RENEGOTIATE) return -1;   // (a TLS 1.2 server does not ask)
          continue;
        }
        if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;
      }
      if (!readMore(SOCKET(sock))) return 0;
    }
    size_t k = std::min(n, plain_.size() - plainAt_);
    std::memcpy(p, plain_.data() + plainAt_, k);
    plainAt_ += k;
    return int(k);
  }

 private:
  static constexpr DWORD kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

  /** The server's certificate: a chain to a root this computer trusts, valid now, for this name – checked
   *  here too, whatever SChannel checked itself (not every SChannel does). */
  bool trusted() {
    PCCERT_CONTEXT cert = nullptr;
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert) return false;
    bool ok = false;
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof para;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &para, 0, nullptr, &chain) && chain) {
      SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
      ssl.cbStruct = sizeof ssl;
      ssl.dwAuthType = AUTHTYPE_SERVER;
      ssl.pwszServerName = &name_[0];
      CERT_CHAIN_POLICY_PARA policy{};
      policy.cbSize = sizeof policy;
      policy.pvExtraPolicyPara = &ssl;
      CERT_CHAIN_POLICY_STATUS status{};
      status.cbSize = sizeof status;
      ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) && status.dwError == 0;
      CertFreeCertificateChain(chain);
    }
    CertFreeCertificateContext(cert);
    return ok;
  }

  /** A handshake message SChannel made: to the server, then freed. */
  bool sendToken(SOCKET s, SecBuffer& b) {
    bool ok = !b.cbBuffer || rawSend(s, static_cast<const uint8_t*>(b.pvBuffer), b.cbBuffer);
    FreeContextBuffer(b.pvBuffer);
    b.pvBuffer = nullptr;
    return ok;
  }

  bool readMore(SOCKET s) {
    uint8_t buf[16384];
    int r = ::recv(s, reinterpret_cast<char*>(buf), int(sizeof buf), 0);
    if (r <= 0) return false;
    in_.insert(in_.end(), buf, buf + r);
    return true;
  }

  CredHandle cred_{};
  CtxtHandle ctx_{};
  bool haveCred_ = false, haveCtx_ = false;
  std::wstring name_;
  SecPkgContext_StreamSizes sizes_{};
  std::mutex mu_;
  std::vector<uint8_t> in_;    // what came from the socket, not yet decrypted (the reader's)
  std::vector<uint8_t> plain_; // decrypted, not yet read
  size_t plainAt_ = 0;
};

}  // namespace

TlsFactory schannelTls() {
  return [] { return std::unique_ptr<TlsChannel>(new Schannel()); };
}

}  // namespace ss
