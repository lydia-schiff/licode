/*
 * DtlsConnection.cpp
 */
#include <iostream>
#include <cassert>

#include "DtlsTransport.h"
#include "NiceConnection.h"
#include "SrtpChannel.h"

#include "dtls/DtlsFactory.h"

#include "rtp/RtpHeaders.h"

using namespace erizo;
using namespace std;
using namespace dtls;

DEFINE_LOGGER(DtlsTransport, "DtlsTransport");
DEFINE_LOGGER(Resender, "Resender");

Resender::Resender(boost::shared_ptr<NiceConnection> nice, unsigned int comp, const unsigned char* data, unsigned int len) : 
  nice_(nice), comp_(comp), data_(data),len_(len), timer(service) {
  sent_ = 0;
}

Resender::~Resender() {
  ELOG_DEBUG("Resender destructor");
  timer.cancel();
  if (thread_.get()!=NULL) {
    ELOG_DEBUG("Resender destructor, joining thread");
    thread_->join();
    ELOG_DEBUG("Resender thread terminated on destructor");
  }
}

void Resender::cancel() {
  timer.cancel();
  sent_ = 1;
}

void Resender::start() {
  sent_ = 0;
  timer.cancel();
  if (thread_.get()!=NULL) {
    ELOG_ERROR("Starting Resender, joining thread to terminate");
    thread_->join();
    ELOG_ERROR("Thread terminated on start");
  }
  timer.expires_from_now(boost::posix_time::seconds(3));
  timer.async_wait(boost::bind(&Resender::resend, this, boost::asio::placeholders::error));
  thread_.reset(new boost::thread(boost::bind(&Resender::run, this)));
}

void Resender::run() {
  service.run();
}

int Resender::getStatus() {
  return sent_;
}

void Resender::resend(const boost::system::error_code& ec) {  
  if (ec == boost::asio::error::operation_aborted) {
    ELOG_DEBUG("%s - Cancelled", nice_->transportName->c_str());
    return;
  }
  
  if (nice_ != NULL) {
    ELOG_WARN("%s - Resending DTLS message to %d", nice_->transportName->c_str(), comp_);
    int val = nice_->sendData(comp_, data_, len_);
    if (val < 0) {
       sent_ = -1;
    } else {
       sent_ = 2;
    }
  }
}

DtlsTransport::DtlsTransport(MediaType med, const std::string &transport_name, bool bundle, bool rtcp_mux, TransportListener *transportListener, const std::string &stunServer, int stunPort, int minPort, int maxPort):Transport(med, transport_name, bundle, rtcp_mux, transportListener, stunServer, stunPort, minPort, maxPort) {
  ELOG_DEBUG( "Initializing DtlsTransport" );
  updateTransportState(TRANSPORT_INITIAL);

  readyRtp = false;
  readyRtcp = false;
  running_ = false;

  dtlsRtp.reset(new DtlsSocketContext());

  // TODO the ownership of classes here is....really awkward. Basically, the DtlsFactory created here ends up being owned the the created client
  // which is in charge of nuking it.  All of the session state is tracked in the DtlsSocketContext.
  //
  // A much more sane architecture would be simply having the client _be_ the context.
  (new DtlsFactory())->createClient(dtlsRtp);
  dtlsRtp->setDtlsReceiver(this);

  int comps = 1;
  if (!rtcp_mux) {
    comps = 2;
    dtlsRtcp.reset(new DtlsSocketContext());
    (new DtlsFactory())->createClient(dtlsRtcp);
    dtlsRtcp->setDtlsReceiver(this);
  }
  bundle_ = bundle;
  nice_.reset(new NiceConnection(med, transport_name, this, comps, stunServer, stunPort, minPort, maxPort));
  nice_->start();
  running_ =true;
  getNice_Thread_ = boost::thread(&DtlsTransport::getNiceDataLoop, this);

}

DtlsTransport::~DtlsTransport() {
  ELOG_DEBUG("DtlsTransport destructor");
  running_ = false;
  nice_->close();
  ELOG_DEBUG("Join thread getNice");
  getNice_Thread_.join();
  ELOG_DEBUG("writeMutex");
  boost::mutex::scoped_lock lockw(writeMutex_);
  ELOG_DEBUG("sessionMutex");
  boost::mutex::scoped_lock locks(sessionMutex_);
  ELOG_DEBUG("DTLSTransport destructor END");
}

void DtlsTransport::onNiceData(unsigned int component_id, char* data, int len, NiceConnection* nice) {
  int length = len;
  SrtpChannel *srtp = srtp_.get();
  if (DtlsTransport::isDtlsPacket(data, len)) {
    ELOG_DEBUG("%s - Received DTLS message from %u", transport_name.c_str(), component_id);
    if (component_id == 1) {
      if (rtpResender.get()!=NULL) {
        rtpResender->cancel();
      }
      dtlsRtp->read(reinterpret_cast<unsigned char*>(data), len);
    } else {
      if (rtcpResender.get()!=NULL) {
        rtcpResender->cancel();
      }
      dtlsRtcp->read(reinterpret_cast<unsigned char*>(data), len);
    }
    return;
  } else if (this->getTransportState() == TRANSPORT_READY) {
    memcpy(unprotectBuf_, data, len);

    if (dtlsRtcp != NULL && component_id == 2) {
      srtp = srtcp_.get();
    }
    if (srtp != NULL){
      RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (unprotectBuf_);
      if (chead->isRtcp()){
        if(srtp->unprotectRtcp(unprotectBuf_, &length)<0){
          return;
        }
      } else {
        if(srtp->unprotectRtp(unprotectBuf_, &length)<0){
          return;
        }
      }
    } else {
      return;
    }

    if (length <= 0){
      return;
    }

    getTransportListener()->onTransportData(unprotectBuf_, length, this);
  }
}

void DtlsTransport::onCandidate(const CandidateInfo &candidate, NiceConnection *conn) {
  std::string generation = " generation 0";
  std::string hostType_str;
  std::ostringstream sdp;
  switch (candidate.hostType) {
    case HOST:
      hostType_str = "host";
      break;
    case SRFLX:
      hostType_str = "srflx";
      break;
    case PRFLX:
      hostType_str = "prflx";
      break;
    case RELAY:
      hostType_str = "relay";
      break;
    default:
      hostType_str = "host";
      break;
  }
  sdp << "a=candidate:" << candidate.foundation << " " << candidate.componentId
      << " " << candidate.netProtocol << " " << candidate.priority << " "
      << candidate.hostAddress << " " << candidate.hostPort << " typ "
      << hostType_str;
  
  if (candidate.hostType == SRFLX || candidate.hostType == RELAY) {
    //raddr 192.168.0.12 rport 50483
    sdp << " raddr " << candidate.rAddress << " rport " << candidate.rPort;
  }
  
  sdp << generation;
  
  getTransportListener()->onCandidate(sdp.str(), this);
}

void DtlsTransport::write(char* data, int len) {
  boost::mutex::scoped_lock lock(writeMutex_);
  if (nice_==NULL)
    return;
  int length = len;
  SrtpChannel *srtp = srtp_.get();

  if (this->getTransportState() == TRANSPORT_READY) {
    memcpy(protectBuf_, data, len);
    int comp = 1;
    RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (protectBuf_);
    if (chead->isRtcp()) {
      if (!rtcp_mux_) {
        comp = 2;
      }
      if (dtlsRtcp != NULL) {
        srtp = srtcp_.get();
      }
      if (srtp && nice_->checkIceState() == NICE_READY) {
        if(srtp->protectRtcp(protectBuf_, &length)<0) {
          return;
        }
      }
    }
    else{
      comp = 1;

      if (srtp && nice_->checkIceState() == NICE_READY) {
        if(srtp->protectRtp(protectBuf_, &length)<0) {
          return;
        }
      }
    }
    if (length <= 10) {
      return;
    }
    if (nice_->checkIceState() == NICE_READY) {
      this->writeOnNice(comp, protectBuf_, length);
    }
  }
}

void DtlsTransport::writeDtls(DtlsSocketContext *ctx, const unsigned char* data, unsigned int len) {
  int comp = 1;
  if (ctx == dtlsRtcp.get()) {
    comp = 2;
    rtcpResender.reset(new Resender(nice_, comp, data, len));
    rtcpResender->start();
  } else {
    rtpResender.reset(new Resender(nice_, comp, data, len));
    rtpResender->start();
  }

  ELOG_DEBUG("%s - Sending DTLS message to %d", transport_name.c_str(), comp);

  nice_->sendData(comp, data, len);
}

void DtlsTransport::onHandshakeCompleted(DtlsSocketContext *ctx, std::string clientKey,std::string serverKey, std::string srtp_profile) {
  boost::mutex::scoped_lock lock(sessionMutex_);
  if (ctx == dtlsRtp.get()) {
    ELOG_DEBUG("%s - Setting RTP srtp params", transport_name.c_str());
    srtp_.reset(new SrtpChannel());
    if (srtp_->setRtpParams((char*) clientKey.c_str(), (char*) serverKey.c_str())) {
      readyRtp = true;
    } else {
      updateTransportState(TRANSPORT_FAILED);
    }
    if (dtlsRtcp == NULL) {
      readyRtcp = true;
    }
  }
  if (ctx == dtlsRtcp.get()) {
    ELOG_DEBUG("%s - Setting RTCP srtp params", transport_name.c_str());
    srtcp_.reset(new SrtpChannel());
    if (srtcp_->setRtpParams((char*) clientKey.c_str(), (char*) serverKey.c_str())) {
      readyRtcp = true;
    } else {
      updateTransportState(TRANSPORT_FAILED);
    }
  }
  ELOG_DEBUG("%s - Ready? %d %d", transport_name.c_str(), readyRtp, readyRtcp);
  if (readyRtp && readyRtcp) {
    ELOG_DEBUG("%s - Ready!!!", transport_name.c_str());
    updateTransportState(TRANSPORT_READY);
  }

}

std::string DtlsTransport::getMyFingerprint() {
  return dtlsRtp->getFingerprint();
}

void DtlsTransport::updateIceState(IceState state, NiceConnection *conn) {
  ELOG_DEBUG( "%s - New NICE state %d %d %d", transport_name.c_str(), state, mediaType, bundle_);
  if (state == NICE_INITIAL && this->getTransportState() != TRANSPORT_STARTED) {
    updateTransportState(TRANSPORT_STARTED);
  }
  if(state == NICE_FAILED){
    ELOG_DEBUG("Nice Failed, no more reading packets");
    running_ = false;
    updateTransportState(TRANSPORT_FAILED);
  }
  if (state == NICE_READY) {
    ELOG_DEBUG("%s - Nice ready", transport_name.c_str());
    if (!dtlsRtp->started || rtpResender->getStatus() < 0) {
      ELOG_DEBUG("%s - DTLSRTP Start", transport_name.c_str());
      dtlsRtp->start();
    }
    if (dtlsRtcp != NULL && (!dtlsRtcp->started || rtcpResender->getStatus() < 0)) {
      ELOG_DEBUG("%s - DTLSRTCP Start", transport_name.c_str());
      dtlsRtcp->start();
    }
  }
}

void DtlsTransport::processLocalSdp(SdpInfo *localSdp_) {
  ELOG_DEBUG( "Processing Local SDP in DTLS Transport" );
  localSdp_->isFingerprint = true;
  localSdp_->fingerprint = getMyFingerprint();
  std::string username;
  std::string password;
  nice_->getLocalCredentials(&username, &password);
  localSdp_->setCredentials(username, password);
  ELOG_DEBUG( "Processed Local SDP in DTLS Transport with credentials %s, %s", username.c_str(), password.c_str());
}

void DtlsTransport::getNiceDataLoop(){
  while(running_ == true){
    p_ = nice_->getPacket();
    if (p_->length > 0) {
        this->onNiceData(p_->comp, p_->data, p_->length, NULL);
    }
    if (p_->length == -1){    
      running_=false;
      return;
    }
  }
}
bool DtlsTransport::isDtlsPacket(const char* buf, int len) {
  int data = DtlsFactory::demuxPacket(reinterpret_cast<const unsigned char*>(buf),len);
  switch(data)
  {
    case DtlsFactory::dtls:
      return true;
      break;
    default:
      return false;
      break;
  }
}